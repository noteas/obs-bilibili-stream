// 手工验证（见本目录 README）：rtmp-services 在运行期是否会重读用户级 services.json
// （= 打开控制台写入条目后，重新打开「设置→直播」能否看到条目）
// 另外枚举已注册的 service 类型，评估「插件自带 service 类型」路线。
#include <obs.h>
#include <obs-module.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "srt/srt_catalog.hpp"

static bool has_list_item(obs_property_t *prop, const char *needle)
{
	if (!prop)
		return false;
	const size_t n = obs_property_list_item_count(prop);
	for (size_t i = 0; i < n; i++) {
		const char *name = obs_property_list_item_name(prop, i);
		if (name && strstr(name, needle))
			return true;
	}
	return false;
}

int main()
{
	std::system("rm -rf /tmp/opencode/obs-reload-config /tmp/opencode/obs-reload-bin && "
		    "mkdir -p /tmp/opencode/obs-reload-config/plugin_config /tmp/opencode/obs-reload-bin");
	std::system("ln -s /usr/lib/x86_64-linux-gnu/obs-plugins/rtmp-services.so /tmp/opencode/obs-reload-bin/");
	std::system("mkdir -p /tmp/opencode/obs-reload-data");

	setvbuf(stdout, nullptr, _IONBF, 0);

	if (!obs_startup("en-US", "/tmp/opencode/obs-reload-config/plugin_config", nullptr)) {
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
			    "/tmp/opencode/obs-reload-data") != MODULE_SUCCESS ||
	    !obs_init_module(ours)) {
		std::printf("our module load failed\n");
		return 1;
	}
	obs_post_load_modules();

	// 1) 未写入条目前，服务下拉里没有我们那条
	obs_service_t *s1 = obs_service_create("rtmp_common", "s1", nullptr, nullptr);
	obs_properties_t *props = obs_service_properties(s1);
	obs_data_t *settings = obs_data_create();
	obs_property_t *services1 = obs_properties_get(props, "service");
	const bool before = has_list_item(services1, "Bilibili Live - SRT");
	std::printf("before write: entry in list = %s (count=%zu)\n", before ? "YES" : "no",
		    obs_property_list_item_count(services1));

	// 2) 插件在「打开控制台/开播成功」时写 services.json
	Srt::UpdateServiceCatalog({{"主线路1 · srt", "rtmpsrt://live-push.bilivideo.com/live-bvc/"},
				   {"主线路2 · srt", "rtmpsrt://txy.live-push.bilivideo.com/live-bvc/"}});
	std::printf("catalog write done\n");

	// 3) 重新构建 properties（等价于关掉设置窗口再打开）
	obs_properties_destroy(props);
	obs_service_release(s1);
	obs_data_release(settings);
	obs_service_t *s2 = obs_service_create("rtmp_common", "s2", nullptr, nullptr);
	props = obs_service_properties(s2);
	services1 = obs_properties_get(props, "service");
	const bool after = has_list_item(services1, "Bilibili Live - SRT");
	std::printf("after write (same OBS run, props rebuilt): entry in list = %s (count=%zu)\n", after ? "YES" : "no",
		    obs_property_list_item_count(services1));
	obs_properties_destroy(props);
	obs_service_release(s2);

	std::printf("--- registered service types ---\n");
	std::fflush(stdout);
	const char *sid = nullptr;
	for (size_t i = 0; obs_enum_service_types(i, &sid); i++)
		std::printf("  type[%zu]=%s name=%s\n", i, sid,
			    obs_service_get_display_name(sid) ? obs_service_get_display_name(sid) : "(null)");

	// 判定：写入前没有、写入后立刻有（= OBS 每次重建属性面板都重读 services.json）
	const bool pass = !before && after;
	std::printf(pass ? "ALL PASS\n" : "FAILURE(S)\n");

	// 正常收尾：obs_shutdown 会走各模块的 obs_module_unload（含 rtmp-services
	// 回收它的联网更新线程）。不调用它就退出 = 更新线程在进程拆台时仍可能
	// 跑在已析构的 libcrypto 上，ASan 报 UAF——那是 harness 假象，不是插件缺陷。
	obs_shutdown();
	std::printf("obs_shutdown done\n");
	return pass ? 0 : 1;
}
