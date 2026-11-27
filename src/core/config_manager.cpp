#include "config_manager.hpp"
#include <obs-module.h>
#include <filesystem>

#include "json11/json11.hpp"

namespace Core {
ConfigManager::ConfigManager() {}

char *ConfigManager::getConfigPath()
{
	char *path = obs_module_config_path("config.json");
	if (path)
		return path;
	return obs_module_file("config.json");
}

void ConfigManager::load()
{
	// 默认构造 = 全部字段为「未知/空」:没有任何占位值。旧版本的占位默认
	// (room_id="12345"/csrf_token="your_csrf_token"/title="我的直播")会让未登录的
	// 控制台显示一个不存在的房间、把「我的直播」当现标题提交——已彻底移除。
	m_config = Bili::Config();

	char *configFile = getConfigPath();
	if (!configFile)
		return;

	obs_data_t *settings = obs_data_create_from_json_file(configFile);
	if (!settings) {
		bfree(configFile);
		return;
	}

	m_config.room_id = obs_data_get_string(settings, "room_id");
	m_config.csrf_token = obs_data_get_string(settings, "csrf_token");
	m_config.mid = obs_data_get_string(settings, "mid");
	m_config.cookies = obs_data_get_string(settings, "cookies");
	m_config.title = obs_data_get_string(settings, "title");
	m_config.part_id = static_cast<int>(obs_data_get_int(settings, "part_id"));
	m_config.area_id = static_cast<int>(obs_data_get_int(settings, "area_id"));
	m_config.dock_introduced = obs_data_get_bool(settings, "dock_introduced");
	// 线路表(ADR-0005):JSON 数组持久化,OBS 重启后进行中的场次仍可抄地址/推流码。
	{
		m_config.stream_targets.clear();
		std::string err;
		json11::Json arr = json11::Json::parse(obs_data_get_string(settings, "stream_targets"), err);
		for (const auto &item : arr.array_items()) {
			Bili::StreamTarget t;
			t.protocol = item["protocol"].string_value();
			t.provider = item["provider"].string_value();
			t.addr = item["addr"].string_value();
			t.key = item["key"].string_value();
			t.backup = item["backup"].bool_value();
			if (!t.addr.empty())
				m_config.stream_targets.push_back(t);
		}
	}
	obs_data_release(settings);
	bfree(configFile);
}

void ConfigManager::save()
{
	char *configFile = getConfigPath();
	if (!configFile)
		return;

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "room_id", m_config.room_id.c_str());
	obs_data_set_string(settings, "csrf_token", m_config.csrf_token.c_str());
	obs_data_set_string(settings, "cookies", m_config.cookies.c_str());
	obs_data_set_string(settings, "mid", m_config.mid.c_str());
	obs_data_set_string(settings, "title", m_config.title.c_str());
	obs_data_set_int(settings, "part_id", m_config.part_id);
	obs_data_set_int(settings, "area_id", m_config.area_id);
	obs_data_set_bool(settings, "dock_introduced", m_config.dock_introduced);
	{
		std::vector<json11::Json> items;
		for (const auto &t : m_config.stream_targets) {
			items.push_back(json11::Json::object{{"protocol", t.protocol},
							     {"provider", t.provider},
							     {"addr", t.addr},
							     {"key", t.key},
							     {"backup", t.backup}});
		}
		const std::string json = json11::Json(items).dump();
		obs_data_set_string(settings, "stream_targets", json.c_str());
	}
	const std::filesystem::path configPath = std::filesystem::u8path(configFile);
	bfree(configFile);
	std::error_code ec;
	try {
		std::filesystem::create_directories(configPath.parent_path(), ec);
	} catch (const std::exception &e) {
		blog(LOG_WARNING, "[obs-bilibili-stream] 创建配置目录失败: %s", e.what());
	}
	// 原子写(临时文件 + 替换,与 OBS 自身保存 service.json 同款):config.json 里有完整
	// 登录会话,崩溃/断电截断文件就等于被登出。旧版 obs_data_save_json 先截断再写。
	if (!obs_data_save_json_safe(settings, configPath.u8string().c_str(), "tmp", nullptr)) {
		blog(LOG_WARNING, "[obs-bilibili-stream] 配置保存失败: %s", configPath.u8string().c_str());
		obs_data_release(settings);
		return;
	}
	obs_data_release(settings);
#ifndef _WIN32
	// config.json 里有完整的登录会话(SESSDATA/bili_jct)与推流码:多用户机器上不能让
	// 其他账号读到。obs_data_save_json 用默认 umask 创建文件,这里收紧到属主读写。
	std::filesystem::permissions(configPath, std::filesystem::perms::owner_read |
						 std::filesystem::perms::owner_write,
				     std::filesystem::perm_options::replace, ec);
#endif
}
} // namespace Core
