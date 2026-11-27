#include "srt/srt_output.hpp"

#include <obs-avc.h>
#include <obs-module.h>

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
#include "srt/rtmp_client.hpp"
#include "srt/srt_target.hpp"
#include "srt/srt_transport.hpp"

namespace Srt {

namespace {

constexpr size_t kMaxQueuedPackets = 1000;

struct SrtOutput {
	obs_output_t *output = nullptr;

	std::mutex mutex;
	std::condition_variable cv;
	std::deque<encoder_packet *> queue;

	std::thread worker;
	std::thread reader;

	std::atomic<bool> stopping{false};
	std::atomic<bool> failed{false};
	std::atomic<bool> capture_started{false};
	std::atomic<uint64_t> total_bytes{0};
	std::atomic<int> dropped_frames{0};
	std::atomic<int> connect_ms{0};
	std::atomic<int> pending_max_bandwidth{0};

	int max_bandwidth = 0;
	int recv_timeout_sec = 0;
	std::string bind_ip;

	std::string fail_reason;

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

	uint8_t *extra = nullptr;
	size_t size = 0;
	if (!obs_encoder_get_extra_data(venc, &extra, &size) || !extra || size == 0) {
		error = "视频编码器没有 SPS/PPS（extra data）";
		return false;
	}

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

	FlvTag metadata;
	if (MakeMetadataTag(ctx->output, metadata) && !client.SendTag(metadata, error))
		return false;
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
		encoder_packet parsed = {};
		obs_parse_avc_packet(&parsed, packet);
		const int32_t ct_ms = static_cast<int32_t>((PacketPtsUs(packet) - dts_us) / 1000);
		if (ms <= ctx->last_video_ms)
			ms = ctx->last_video_ms + 1;
		ctx->last_video_ms = ms;
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
	if (ctx->failed.exchange(true))
		return;
	ctx->fail_reason = message;
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

	target.max_bandwidth = ctx->max_bandwidth;
	target.recv_timeout_sec = ctx->recv_timeout_sec;
	target.bind_ip = ctx->bind_ip;

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
		transport.Close();
		return;
	}

	if (!obs_output_begin_data_capture(ctx->output, 0)) {
		MarkFailed(ctx, "启动编码器数据捕获失败");
		transport.Close();
		return;
	}
	ctx->capture_started = true;

	ctx->reader = std::thread(ReaderMain, ctx, &client);

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
	if (ctx->reader.joinable())
		ctx->reader.join();
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
	if (ctx->reader.joinable())
		ctx->reader.join();

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
	if (!vcodec || strcmp(vcodec, "h264") != 0) {
		SetLastError(ctx->output, "私有 SRT 输出 v1 仅支持 H.264 视频编码器");
		return false;
	}

	obs_encoder_t *aenc = obs_output_get_audio_encoder(ctx->output, 0);
	if (aenc) {
		const char *acodec = obs_encoder_get_codec(aenc);
		if (!acodec || strcmp(acodec, "aac") != 0) {
			SetLastError(ctx->output, "私有 SRT 输出 v1 仅支持 AAC 音频编码器");
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
	if (ctx->worker.joinable())
		ctx->worker.join();
	if (ctx->reader.joinable())
		ctx->reader.join();

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
			encoder_packet *old = ctx->queue.front();
			ctx->queue.pop_front();
			obs_encoder_packet_release(old);
			delete old;
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

	ctx->max_bandwidth = static_cast<int>(obs_data_get_int(settings, "srt_max_bandwidth"));
	ctx->recv_timeout_sec = static_cast<int>(obs_data_get_int(settings, "srt_recv_timeout_sec"));
	const char *bind = obs_data_get_string(settings, "bind_ip");
	ctx->bind_ip = bind ? bind : "";
	ctx->pending_max_bandwidth = ctx->max_bandwidth;
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
	obs_properties_add_int(props, "srt_max_bandwidth", "SRT 最大带宽（B/s，0=自动 2 MiB/s）", 0,
			       1000000000, 1000);
	obs_properties_add_int(props, "srt_recv_timeout_sec", "SRT 接收超时（秒，0=默认）", 0, 60, 1);
	obs_properties_add_text(props, "bind_ip", "绑定本地 IP（default=不绑定）", OBS_TEXT_DEFAULT);
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
	g_output_info.encoded_video_codecs = "h264";
	g_output_info.encoded_audio_codecs = "aac";
	g_output_info.protocols = "bvc_srt";
}

obs_output_info *OutputInfo()
{
	return &g_output_info;
}

} // namespace Srt
