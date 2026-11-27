#pragma once
#include "bilibili_api.hpp"

namespace Core {
// 插件配置(config.json,位于 OBS 的 plugin_config/<module>/ 下)。只存「跨启动需要
// 留住」的东西:登录 Cookie 白名单镜像、房间号/标题/分区镜像、上次获取的推流线路。
// 登录态/进行中场次/忙碌标志一律不持久化——以服务端 room/GetInfo 为准。
class ConfigManager {
public:
	ConfigManager();
	void load();
	void save();
	Bili::Config &config() { return m_config; }
	const Bili::Config &config() const { return m_config; }

private:
	Bili::Config m_config;
	char *getConfigPath();
};
} // namespace Core
