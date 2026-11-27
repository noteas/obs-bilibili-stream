#include "srt/srt_output.hpp"

#include <obs-avc.h>
#include <obs-hevc.h>
#include <obs-module.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "srt/flv_mux.hpp"
#include "srt/av1_parse.hpp"
#include "srt/hevc_parse.hpp"
#include "srt/rtmp_client.hpp"
#include "srt/srt_target.hpp"
#include "srt/srt_transport.hpp"

namespace Srt {

namespace {

constexpr size_t kMaxQueuedPackets = 1000;

// track0 视频编码（obs_encoder_get_codec 口径），决定 legacy / Enhanced 封装路线。
enum class VideoCodecKind { H264, Hevc, Av1 };

struct SrtOutput {
	obs_output_t *output = nullptr;
	VideoCodecKind video_codec = VideoCodecKind::H264;

	std::mutex mutex;
	std::condition_variable cv;
	std::deque<encoder_packet *> queue;

	std::thread worker;

	std::atomic<bool> stopping{false};
	std::atomic<bool> failed{false};
	std::atomic<bool> capture_started{false};
	std::atomic<uint64_t> total_bytes{0};
	std::atomic<int> dropped_frames{0};
	std::atomic<int> connect_ms{0};

	// 设置项：UI 线程在 output_update 里写，worker 线程只在开播/重连的开头读一次快照，
	// 运行中的改动（带宽除外）到下次开播才生效。bind_ip 是 std::string，必须上锁读写。
	std::mutex settings_mutex;
	int max_bandwidth = 0;
	int recv_timeout_sec = 0;
	std::string bind_ip;
	std::atomic<int> pending_max_bandwidth{0};

	// 时间戳（仅发送线程访问）
	int64_t dts_offset_us = 0;
	bool have_dts_offset = false;
	int64_t last_video_ms = -1;
	int64_t last_audio_ms = -1;
};

int64_t PacketDtsUs(const encoder_packet *packet)
{
	const __int128 v = static_cast<__int128>(packet->dts) * packet->timebase_num * 1000000 /
			   (packet->timebase_den ? packet->timebase_den : 1);
	return static_cast<int64_t>(v);
}

int64_t PacketPtsUs(const encoder_packet *packet)
{
	const __int128 v = static_cast<__int128>(packet->pts) * packet->timebase_num * 1000000 /
			   (packet->timebase_den ? packet->timebase_den : 1);
	return static_cast<int64_t>(v);
}

void SetLastError(obs_output_t *output, const std::string &message)
{
	obs_output_set_last_error(output, message.c_str());
	blog(LOG_WARNING, "[bvc-srt] %s", message.c_str());
}

// 上游 send_video_metadata：仅 PQ/HLG（HDR）且非 H.264 时发 colorInfo metadata tag。
// OBSCOL_* 取值 = libobs obs.h（ISO/IEC 23091-2 注册值；系统 30.x 头没有这些常量，自带）。
constexpr int kColPriSmpte170M = 6;
constexpr int kColPriBt709 = 1;
constexpr int kColPriBt2020 = 9;
constexpr int kColTrcIec61966_2_1 = 13;
constexpr int kColTrcSmpte2084 = 16;
constexpr int kColTrcAribStdB67 = 18;
constexpr int kColSpcBt2020Ncl = 9;

bool SendColorInfoIfHdr(SrtOutput *ctx, obs_encoder_t *venc, RtmpClient &client, std::string &error)
{
	if (ctx->video_codec == VideoCodecKind::H264)
		return true;
	const EnhancedVideoCodec ex =
		ctx->video_codec == VideoCodecKind::Hevc ? EnhancedVideoCodec::Hevc : EnhancedVideoCodec::Av1;

	video_t *video = obs_encoder_video(venc);
	if (!video) {
		error = "无法读取视频输出信息";
		return false;
	}
	const struct video_output_info *info = video_output_get_info(video);
	if (!info || !(info->colorspace == VIDEO_CS_2100_PQ || info->colorspace == VIDEO_CS_2100_HLG))
		return true;

	int bits_per_raw_sample;
	switch (info->format) {
	case VIDEO_FORMAT_I010:
	case VIDEO_FORMAT_P010:
	case VIDEO_FORMAT_I210:
		bits_per_raw_sample = 10;
		break;
	case VIDEO_FORMAT_I412:
	case VIDEO_FORMAT_YA2L:
		bits_per_raw_sample = 12;
		break;
	default:
		bits_per_raw_sample = 8;
		break;
	}

	int pri = 0, trc = 0, spc = 0;
	switch (info->colorspace) {
	case VIDEO_CS_601:
		pri = kColPriSmpte170M;
		trc = kColPriSmpte170M;
		spc = kColPriSmpte170M;
		break;
	case VIDEO_CS_DEFAULT:
	case VIDEO_CS_709:
		pri = kColPriBt709;
		trc = kColPriBt709;
		spc = kColPriBt709;
		break;
	case VIDEO_CS_SRGB:
		pri = kColPriBt709;
		trc = kColTrcIec61966_2_1;
		spc = kColPriBt709;
		break;
	case VIDEO_CS_2100_PQ:
		pri = kColPriBt2020;
		trc = kColTrcSmpte2084;
		spc = kColSpcBt2020Ncl;
		break;
	case VIDEO_CS_2100_HLG:
		pri = kColPriBt2020;
		trc = kColTrcAribStdB67;
		spc = kColSpcBt2020Ncl;
		break;
	}

	int max_luminance = 0;
	if (trc == kColTrcAribStdB67)
		max_luminance = 1000;
	else if (trc == kColTrcSmpte2084)
		max_luminance = (int)obs_get_video_hdr_nominal_peak_level();

	FlvTag tag;
	if (!MakeColorInfoTag(ex, bits_per_raw_sample, pri, trc, spc, 0, max_luminance, tag)) {
		error = "构造 colorInfo metadata tag 失败";
		return false;
	}
	return client.SendTag(tag, error);
}

bool SendHeaders(SrtOutput *ctx, RtmpClient &client, std::string &error)
{
	obs_encoder_t *aenc = obs_output_get_audio_encoder(ctx->output, 0);
	if (aenc) {
		uint8_t *extra = nullptr;
		size_t size = 0;
		if (obs_encoder_get_extra_data(aenc, &extra, &size) && extra && size > 0) {
			FlvTag tag;
			if (MakeAudioHeaderTag(extra, size, tag) && !client.SendTag(tag, error))
				return false;
		}
	}

	obs_encoder_t *venc = obs_output_get_video_encoder(ctx->output);
	if (!venc) {
		error = "缺少视频编码器";
		return false;
	}

	// 上游 send_headers 顺序：先 colorInfo（仅 HDR），后序列头。
	if (!SendColorInfoIfHdr(ctx, venc, client, error))
		return false;

	uint8_t *extra = nullptr;
	size_t size = 0;
	if (!obs_encoder_get_extra_data(venc, &extra, &size) || !extra || size == 0) {
		error = "视频编码器没有参数集（extra data）";
		return false;
	}

	switch (ctx->video_codec) {
	case VideoCodecKind::H264: {
		uint8_t *avcc = nullptr;
		const size_t avcc_size = obs_parse_avc_header(&avcc, extra, size);
		if (avcc_size == 0 || !avcc) {
			error = "构造 AVCDecoderConfigurationRecord 失败";
			bfree(avcc);
			return false;
		}
		FlvTag video_header;
		const bool ok = MakeVideoHeaderTag(avcc, avcc_size, video_header) && client.SendTag(video_header, error);
		bfree(avcc);
		if (!ok)
			return false;
		break;
	}
	case VideoCodecKind::Hevc: {
		std::vector<uint8_t> hvcc;
		if (!BuildHevcHvcC(extra, size, hvcc)) {
			error = "构造 HEVCDecoderConfigurationRecord(hvcC) 失败";
			return false;
		}
		FlvTag video_header;
		if (!MakeVideoHeaderTagEx(hvcc.data(), hvcc.size(), EnhancedVideoCodec::Hevc, video_header) ||
		    !client.SendTag(video_header, error))
			return false;
		break;
	}
	case VideoCodecKind::Av1: {
		std::vector<uint8_t> av1c;
		if (!BuildAv1CodecConfRecord(extra, size, av1c)) {
			error = "构造 AV1CodecConfigurationRecord(av1C) 失败";
			return false;
		}
		FlvTag video_header;
		if (!MakeVideoHeaderTagEx(av1c.data(), av1c.size(), EnhancedVideoCodec::Av1, video_header) ||
		    !client.SendTag(video_header, error))
			return false;
		break;
	}
	}

	// 不发 onMetaData：官方只在 HDR 分支发 metadata，普通 H.264/AAC 只有序列头 + 媒体 tag（E-25 §6）。
	return true;
}

bool SendQueuedPacket(SrtOutput *ctx, RtmpClient &client, encoder_packet *packet, std::string &error)
{
	const int64_t dts_us = PacketDtsUs(packet);
	if (!ctx->have_dts_offset) {
		ctx->dts_offset_us = dts_us;
		ctx->have_dts_offset = true;
	}

	int64_t ms = (dts_us - ctx->dts_offset_us) / 1000;

	FlvTag tag;
	if (packet->type == OBS_ENCODER_VIDEO) {
		const int32_t ct_ms = static_cast<int32_t>((PacketPtsUs(packet) - dts_us) / 1000);
		if (ms <= ctx->last_video_ms)
			ms = ctx->last_video_ms + 1;
		ctx->last_video_ms = ms;

		if (ctx->video_codec == VideoCodecKind::Hevc) {
			encoder_packet parsed = {};
			obs_parse_hevc_packet(&parsed, packet);
			const bool ok = MakeVideoFrameTagEx(parsed.data, parsed.size, parsed.keyframe, ms, ct_ms,
							    packet->dts == packet->pts, EnhancedVideoCodec::Hevc,
							    tag) &&
					client.SendTag(tag, error);
			obs_encoder_packet_release(&parsed);
			return ok;
		}
		if (ctx->video_codec == VideoCodecKind::Av1) {
			// 上游 obs_parse_av1_packet：剔除 TD/冗余帧头 OBU 并按帧头重算 keyframe/priority。
			std::vector<uint8_t> payload;
			bool keyframe = packet->keyframe;
			int priority = packet->priority;
			ConvertAv1TemporalUnit(packet->data, packet->size, &keyframe, &priority, payload);
			const bool ok = MakeVideoFrameTagEx(payload.data(), payload.size(), keyframe, ms, ct_ms, false,
							    EnhancedVideoCodec::Av1, tag) &&
					client.SendTag(tag, error);
			return ok;
		}
		encoder_packet parsed = {};
		obs_parse_avc_packet(&parsed, packet);
		const bool ok = MakeVideoFrameTag(parsed.data, parsed.size, parsed.keyframe, ms, ct_ms, tag) &&
				client.SendTag(tag, error);
		obs_encoder_packet_release(&parsed);
		return ok;
	}

	if (packet->type == OBS_ENCODER_AUDIO) {
		if (ms <= ctx->last_audio_ms)
			ms = ctx->last_audio_ms + 1;
		ctx->last_audio_ms = ms;
		return MakeAudioFrameTag(packet->data, packet->size, ms, tag) && client.SendTag(tag, error);
	}
	return true;
}

void MarkFailed(SrtOutput *ctx, const std::string &message)
{
	// 原因必须写进 OBS 的 last error，否则 OBS 的「停止原因」只会显示通用的 disconnected。
	SetLastError(ctx->output, message);
	if (ctx->failed.exchange(true))
		return;
	ctx->stopping = true;
	ctx->cv.notify_all();
}

void ReaderMain(SrtOutput *ctx, RtmpClient *client)
{
	std::string error;
	while (!ctx->stopping) {
		const int rc = client->PollIncoming(error);
		if (rc < 0) {
			MarkFailed(ctx, error.empty() ? "RTMP 连接中断" : error);
			return;
		}
	}
}

void WorkerMain(SrtOutput *ctx)
{
	const auto started = std::chrono::steady_clock::now();

	Target target;
	{
		obs_service_t *service = obs_output_get_service(ctx->output);
		if (!service) {
			MarkFailed(ctx, "未绑定推流服务");
			obs_output_signal_stop(ctx->output, OBS_OUTPUT_BAD_PATH);
			return;
		}
		const char *url = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
		const char *key = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
		std::string parse_error;
		if (!ParseTarget(url ? url : "", key ? key : "", target, parse_error)) {
			MarkFailed(ctx, parse_error);
			obs_output_signal_stop(ctx->output, OBS_OUTPUT_BAD_PATH);
			return;
		}
	}

	// 设置快照（ParseTarget 会重置 target，所以必须在它之后）：UI 线程随时可能 update()，
	// 见 SrtOutput::settings_mutex。
	{
		std::lock_guard<std::mutex> lock(ctx->settings_mutex);
		ctx->pending_max_bandwidth = 0;
		target.max_bandwidth = ctx->max_bandwidth;
		target.recv_timeout_sec = ctx->recv_timeout_sec;
		target.bind_ip = ctx->bind_ip;
	}

	SrtTransport transport(target);
	std::string error;
	if (!transport.Connect(error)) {
		MarkFailed(ctx, error);
		obs_output_signal_stop(ctx->output, OBS_OUTPUT_CONNECT_FAILED);
		return;
	}

	RtmpClient client(transport, target);
	if (!client.HandshakeAndPublish(error)) {
		MarkFailed(ctx, error);
		obs_output_signal_stop(ctx->output, OBS_OUTPUT_CONNECT_FAILED);
		transport.Close();
		return;
	}

	ctx->connect_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
						   std::chrono::steady_clock::now() - started)
						   .count());

	if (!SendHeaders(ctx, client, error)) {
		MarkFailed(ctx, error);
		obs_output_signal_stop(ctx->output, OBS_OUTPUT_CONNECT_FAILED);
		client.SendTeardown();
		transport.Close();
		return;
	}

	if (!obs_output_begin_data_capture(ctx->output, 0)) {
		MarkFailed(ctx, "启动编码器数据捕获失败");
		obs_output_signal_stop(ctx->output, OBS_OUTPUT_ERROR);
		client.SendTeardown();
		transport.Close();
		return;
	}
	ctx->capture_started = true;

	// 收线程归本函数所有、在这里 join；output_stop/destroy 只 join worker，
	// 避免出现两个线程对同一个 std::thread 对象 join/detach 的竞争。
	std::thread reader(ReaderMain, ctx, &client);

	while (!ctx->stopping) {
		const int new_maxbw = ctx->pending_max_bandwidth.exchange(0);
		if (new_maxbw > 0) {
			std::string bw_error;
			if (!transport.UpdateMaxBandwidth(new_maxbw, bw_error)) {
				MarkFailed(ctx, bw_error);
				break;
			}
		}

		encoder_packet *packet = nullptr;
		{
			std::unique_lock<std::mutex> lock(ctx->mutex);
			ctx->cv.wait_for(lock, std::chrono::milliseconds(5),
					 [ctx] { return ctx->stopping.load() || !ctx->queue.empty(); });
			if (!ctx->queue.empty()) {
				packet = ctx->queue.front();
				ctx->queue.pop_front();
			}
		}

		if (!packet)
			continue;

		if (!SendQueuedPacket(ctx, client, packet, error)) {
			obs_encoder_packet_release(packet);
			delete packet;
			MarkFailed(ctx, error.empty() ? "发送失败" : error);
			break;
		}
		obs_encoder_packet_release(packet);
		delete packet;
		ctx->total_bytes = transport.bytes_sent();
	}

	ctx->stopping = true;
	ctx->cv.notify_all();
	if (reader.joinable())
		reader.join();
	// 上游 send_footers：停播时 track0 非 H.264 要补发 SEQ_END；断线（failed）时 socket 已废，跳过。
	if (ctx->capture_started && !ctx->failed && ctx->video_codec != VideoCodecKind::H264) {
		const EnhancedVideoCodec ex = ctx->video_codec == VideoCodecKind::Hevc ? EnhancedVideoCodec::Hevc
											: EnhancedVideoCodec::Av1;
		FlvTag footer;
		std::string ignored;
		// 上游 footer 包 dts=0 → ts = 0 - start_dts_offset（int32 环绕由 builder 处理）。
		if (MakeVideoSeqEndTag(ctx->have_dts_offset ? -(ctx->dts_offset_us / 1000) : 0, ex, footer))
			client.SendTag(footer, ignored);
	}

	// librtmp 的顺序：RTMP_Close() 里先 FCUnpublish/deleteStream，再关 socket（E-25 §5）。
	client.SendTeardown();
	transport.Close();

	if (ctx->failed) {
		obs_output_signal_stop(ctx->output, OBS_OUTPUT_DISCONNECTED);
	} else if (ctx->capture_started) {
		obs_output_end_data_capture(ctx->output);
	}
}

void DrainQueue(SrtOutput *ctx)
{
	std::lock_guard<std::mutex> lock(ctx->mutex);
	for (encoder_packet *packet : ctx->queue) {
		obs_encoder_packet_release(packet);
		delete packet;
	}
	ctx->queue.clear();
}

void *output_create(obs_data_t *settings, obs_output_t *output)
{
	auto *ctx = new SrtOutput();
	ctx->output = output;
	if (settings) {
		ctx->max_bandwidth = static_cast<int>(obs_data_get_int(settings, "srt_max_bandwidth"));
		ctx->recv_timeout_sec = static_cast<int>(obs_data_get_int(settings, "srt_recv_timeout_sec"));
		const char *bind = obs_data_get_string(settings, "bind_ip");
		ctx->bind_ip = bind ? bind : "";
	}
	return ctx;
}

void output_destroy(void *data)
{
	auto *ctx = static_cast<SrtOutput *>(data);
	if (!ctx)
		return;

	ctx->stopping = true;
	ctx->cv.notify_all();
	if (ctx->worker.joinable())
		ctx->worker.join();

	DrainQueue(ctx);
	delete ctx;
}

bool output_start(void *data)
{
	auto *ctx = static_cast<SrtOutput *>(data);
	if (!ctx)
		return false;

	if (ctx->worker.joinable()) {
		ctx->stopping = true;
		ctx->cv.notify_all();
		ctx->worker.join();
	}

	obs_service_t *service = obs_output_get_service(ctx->output);
	if (!service) {
		SetLastError(ctx->output, "未绑定推流服务（请先在控制台应用私有 SRT 线路）");
		return false;
	}

	const char *url = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	const char *key = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
	Target probe;
	std::string error;
	if (!ParseTarget(url ? url : "", key ? key : "", probe, error)) {
		SetLastError(ctx->output, error);
		return false;
	}

	obs_encoder_t *venc = obs_output_get_video_encoder(ctx->output);
	if (!venc) {
		SetLastError(ctx->output, "缺少视频编码器");
		return false;
	}
	const char *vcodec = obs_encoder_get_codec(venc);
	if (!vcodec || strcmp(vcodec, "h264") == 0)
		ctx->video_codec = VideoCodecKind::H264;
	else if (strcmp(vcodec, "hevc") == 0)
		ctx->video_codec = VideoCodecKind::Hevc;
	else if (strcmp(vcodec, "av1") == 0)
		ctx->video_codec = VideoCodecKind::Av1;
	else {
		SetLastError(ctx->output, std::string("不支持的视频编码器「") + (vcodec ? vcodec : "?") +
						   "」，可选 H.264 / HEVC / AV1");
		return false;
	}

	if (ctx->video_codec == VideoCodecKind::Av1) {
		// 上游 rtmp-stream：AV1 + PQ/HLG 直接 OBS_OUTPUT_HDR_DISABLED 停播。
		video_t *video = obs_encoder_video(venc);
		const struct video_output_info *info = video ? video_output_get_info(video) : nullptr;
		if (info && (info->colorspace == VIDEO_CS_2100_PQ || info->colorspace == VIDEO_CS_2100_HLG)) {
			SetLastError(ctx->output, "AV1 不支持 HDR 推流（上游 OBS 同款限制），请改用 HEVC/H.264");
			return false;
		}
	}

	obs_encoder_t *aenc = obs_output_get_audio_encoder(ctx->output, 0);
	if (aenc) {
		const char *acodec = obs_encoder_get_codec(aenc);
		if (!acodec || strcmp(acodec, "aac") != 0) {
			SetLastError(ctx->output, "私有 SRT 输出仅支持 AAC 音频编码器（上游 track0 恒 legacy AAC）");
			return false;
		}
	}

	if (!obs_output_can_begin_data_capture(ctx->output, 0))
		return false;
	if (!obs_output_initialize_encoders(ctx->output, 0))
		return false;

	DrainQueue(ctx);
	ctx->stopping = false;
	ctx->failed = false;
	ctx->capture_started = false;
	ctx->total_bytes = 0;
	ctx->dropped_frames = 0;
	ctx->connect_ms = 0;
	ctx->dts_offset_us = 0;
	ctx->have_dts_offset = false;
	ctx->last_video_ms = -1;
	ctx->last_audio_ms = -1;

	try {
		ctx->worker = std::thread(WorkerMain, ctx);
	} catch (const std::exception &e) {
		SetLastError(ctx->output, std::string("创建推流线程失败: ") + e.what());
		return false;
	}
	return true;
}

void output_stop(void *data, uint64_t /*ts*/)
{
	auto *ctx = static_cast<SrtOutput *>(data);
	if (!ctx)
		return;

	ctx->stopping = true;
	ctx->cv.notify_all();
	// worker 自己负责 join 它的收线程并做 RTMP 收尾，这里只等 worker。
	if (ctx->worker.joinable())
		ctx->worker.join();

	if (ctx->capture_started) {
		obs_output_end_data_capture(ctx->output);
		ctx->capture_started = false;
	}
	DrainQueue(ctx);
}

void output_encoded_packet(void *data, struct encoder_packet *packet)
{
	auto *ctx = static_cast<SrtOutput *>(data);
	if (!ctx || !packet || ctx->stopping)
		return;

	auto *copy = new encoder_packet();
	obs_encoder_packet_ref(copy, packet);
	{
		std::lock_guard<std::mutex> lock(ctx->mutex);
		ctx->queue.push_back(copy);
		while (ctx->queue.size() > kMaxQueuedPackets) {
			// 拥塞时优先丢非关键帧视频，保住音频与关键帧（OBS 同款策略）。
			auto victim = std::find_if(ctx->queue.begin(), ctx->queue.end(), [](const encoder_packet *p) {
				return p->type == OBS_ENCODER_VIDEO && !p->keyframe;
			});
			if (victim == ctx->queue.end())
				victim = ctx->queue.begin();
			obs_encoder_packet_release(*victim);
			delete *victim;
			ctx->queue.erase(victim);
			ctx->dropped_frames.fetch_add(1);
		}
	}
	ctx->cv.notify_one();
}

void output_update(void *data, obs_data_t *settings)
{
	auto *ctx = static_cast<SrtOutput *>(data);
	if (!ctx || !settings)
		return;

	const int maxbw = static_cast<int>(obs_data_get_int(settings, "srt_max_bandwidth"));
	const int recv_timeout = static_cast<int>(obs_data_get_int(settings, "srt_recv_timeout_sec"));
	const char *bind = obs_data_get_string(settings, "bind_ip");

	std::lock_guard<std::mutex> lock(ctx->settings_mutex);
	ctx->max_bandwidth = maxbw;
	ctx->recv_timeout_sec = recv_timeout;
	ctx->bind_ip = bind ? bind : "";
	// 带宽能在线生效（SRTO_MAXBW 支持运行中改）；接收超时与绑定地址是连接期参数，
	// 下次开播才生效。
	ctx->pending_max_bandwidth = maxbw;
}

void output_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "srt_max_bandwidth", 0); // 0 = 官方默认 2 MiB/s
	obs_data_set_default_int(settings, "srt_recv_timeout_sec", 0);
	obs_data_set_default_string(settings, "bind_ip", "default");
}

obs_properties_t *output_get_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, "srt_max_bandwidth", "SRT 最大带宽（B/s，0=官方默认 2 MiB/s）", 0,
			       1000000000, 1000);
	obs_properties_add_int(props, "srt_recv_timeout_sec", "SRT 接收超时（秒，0=默认 3；取值 3-60，下次开播生效）",
			       0, 60, 1);
	obs_properties_add_text(props, "bind_ip", "绑定本地 IP（default=不绑定，下次开播生效）", OBS_TEXT_DEFAULT);
	return props;
}

uint64_t output_total_bytes(void *data)
{
	auto *ctx = static_cast<SrtOutput *>(data);
	return ctx ? ctx->total_bytes.load() : 0;
}

int output_dropped_frames(void *data)
{
	auto *ctx = static_cast<SrtOutput *>(data);
	return ctx ? ctx->dropped_frames.load() : 0;
}

int output_connect_time(void *data)
{
	auto *ctx = static_cast<SrtOutput *>(data);
	return ctx ? ctx->connect_ms.load() : 0;
}

const char *output_name(void *)
{
	return "B站私有 SRT（RTMP over SRT）";
}

obs_output_info g_output_info = {};

} // namespace

void InitOutputInfo()
{
	g_output_info.id = "bvc_srt_output";
	g_output_info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE;
	g_output_info.get_name = output_name;
	g_output_info.create = output_create;
	g_output_info.destroy = output_destroy;
	g_output_info.start = output_start;
	g_output_info.stop = output_stop;
	g_output_info.encoded_packet = output_encoded_packet;
	g_output_info.update = output_update;
	g_output_info.get_defaults = output_get_defaults;
	g_output_info.get_properties = output_get_properties;
	g_output_info.get_total_bytes = output_total_bytes;
	g_output_info.get_dropped_frames = output_dropped_frames;
	g_output_info.get_connect_time_ms = output_connect_time;
	g_output_info.encoded_video_codecs = "h264;hevc;av1";
	g_output_info.encoded_audio_codecs = "aac";
	g_output_info.protocols = "bvc_srt";
}

obs_output_info *OutputInfo()
{
	return &g_output_info;
}

} // namespace Srt
