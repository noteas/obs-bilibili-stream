// 手工集成验证（见本目录 README）：在纯 libobs 环境里加载 rtmp-services + 本插件，验证
// services.json 合并、协议注册与 output 选择。
#include <obs.h>

#include "srt/srt_catalog.hpp"

#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c, m)                                                                                                    \
	do {                                                                                                           \
		if (!(c)) {                                                                                            \
			std::printf("FAIL: %s\n", m);                                                                  \
			++g_fail;                                                                                      \
		} else {                                                                                               \
			std::printf("ok: %s\n", m);                                                                    \
		}                                                                                                      \
	} while (0)

static bool enum_output_cb(void *data, const char *id)
{
	auto *out = static_cast<std::string *>(data);
	if (*out == id)
		return false; // stop
	*out = id;
	return true;
}

int main()
{
	const char *module_config = "/tmp/opencode/obs-test-config/plugin_config";
	std::system(
		"rm -rf /tmp/opencode/obs-test-config /tmp/opencode/obs-test-bin && mkdir -p /tmp/opencode/obs-test-bin /tmp/opencode/obs-test-config/plugin_config");
	std::system("ln -s /usr/lib/x86_64-linux-gnu/obs-plugins/rtmp-services.so /tmp/opencode/obs-test-bin/");
	std::system("mkdir -p /tmp/opencode/obs-test-data");

	if (!obs_startup("en-US", module_config, nullptr)) {
		std::printf("obs_startup failed\n");
		return 1;
	}
	obs_module_t *outmod = nullptr;
	if (obs_open_module(&outmod, "/usr/lib/x86_64-linux-gnu/obs-plugins/obs-outputs.so",
			    "/usr/share/obs/obs-plugins/obs-outputs") == MODULE_SUCCESS)
		obs_init_module(outmod);

	obs_module_t *mod = nullptr;
	if (obs_open_module(&mod, "/usr/lib/x86_64-linux-gnu/obs-plugins/rtmp-services.so",
			    "/usr/share/obs/obs-plugins/rtmp-services") != MODULE_SUCCESS ||
	    !obs_init_module(mod)) {
		std::printf("rtmp-services module load failed\n");
		return 1;
	}
	obs_module_t *ours = nullptr;
	if (obs_open_module(&ours, "/home/zxd/tools/obs-bilibili-stream/build_x86_64_debug/bilibili-stream-for-obs.so",
			    "/tmp/opencode/obs-test-data") != MODULE_SUCCESS ||
	    !obs_init_module(ours)) {
		std::printf("our module load failed\n");
		return 1;
	}
	obs_post_load_modules();

	Srt::UpdateServiceCatalog({{"主线路1 · srt", "rtmpsrt://live-push.bilivideo.com/live-bvc/"},
				   {"主线路2 · srt", "rtmpsrt://txy.live-push.bilivideo.com/live-bvc/"}});

	CHECK(obs_get_module("rtmp-services") != nullptr, "rtmp-services 模块已加载");
	CHECK(obs_is_output_protocol_registered("bvc_srt"), "协议 bvc_srt 已注册");
	CHECK(obs_is_output_protocol_registered("RTMP"), "原生 RTMP 协议已注册（obs-outputs）");

	std::string output_id;
	obs_enum_output_types_with_protocol("bvc_srt", &output_id, enum_output_cb);
	CHECK(output_id == "bvc_srt_output", "协议 bvc_srt 命中 bvc_srt_output");

	// 服务列表：应包含合并进去的条目
	obs_properties_t *props = obs_get_service_properties("rtmp_common");
	CHECK(props != nullptr, "rtmp_common properties");
	bool found = false;
	if (props) {
		obs_property_t *services = obs_properties_get(props, "service");
		const size_t count = obs_property_list_item_count(services);
		for (size_t i = 0; i < count; ++i) {
			const char *name = obs_property_list_item_string(services, i);
			if (name && std::string(name).find("Bilibili Live - SRT") != std::string::npos)
				found = true;
		}
		std::printf("service list entries(default)=%zu\n", count);
		obs_data_t *s = obs_data_create();
		obs_data_set_bool(s, "show_all", true);
		obs_property_modified(obs_properties_get(props, "show_all"), s);
		obs_data_release(s);
		std::printf("service list entries(show_all)=%zu\n", obs_property_list_item_count(services));
		obs_properties_destroy(props);
	}
	CHECK(found, "服务下拉包含「Bilibili Live - SRT」");

	obs_output_t *out = obs_output_create("bvc_srt_output", "test_output", nullptr, nullptr);
	CHECK(out != nullptr, "创建 bvc_srt_output");
	if (out) {
		const char *name = obs_output_get_name(out);
		std::printf("output name=%s\n", name ? name : "(null)");
		obs_output_release(out);
	}

	// 选中该服务后，protocol 应为 bvc_srt
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "service", "Bilibili Live - SRT | 哔哩哔哩直播 - SRT");
	obs_data_set_string(settings, "protocol", "");
	obs_data_set_string(settings, "server", "rtmpsrt://live-push.bilivideo.com/live-bvc/");
	obs_data_set_string(settings, "key", "?streamname=x&key=y");
	obs_service_t *service = obs_service_create("rtmp_common", "test_service", settings, nullptr);
	obs_data_release(settings);
	CHECK(service != nullptr, "创建 rtmp_common 服务");
	if (service) {
		const char *protocol = obs_service_get_protocol(service);
		std::printf("protocol=%s\n", protocol ? protocol : "(null)");
		CHECK(protocol && std::string(protocol) == "bvc_srt", "服务协议=bvc_srt");
		const char *url = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
		const char *key = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
		CHECK(url && std::string(url).find("rtmpsrt://") == 0, "服务 URL 透传 rtmpsrt://");
		CHECK(key && std::string(key).find("?streamname=") == 0, "服务 key 透传推流码");
		obs_service_release(service);
	}

	std::system(
		"grep -c 'Bilibili Live - SRT' /tmp/opencode/obs-test-config/plugin_config/rtmp-services/services.json || true");
	obs_shutdown();
	std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fail);
	return g_fail ? 1 : 0;
}
