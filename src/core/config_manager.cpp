#include "core/config_manager.hpp"
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
	m_config.rtmp_addr = obs_data_get_string(settings, "rtmp_addr");
	m_config.rtmp_code = obs_data_get_string(settings, "rtmp_code");
	m_config.part_id = static_cast<int>(obs_data_get_int(settings, "part_id"));
	m_config.area_id = static_cast<int>(obs_data_get_int(settings, "area_id"));
	m_config.use_legacy_endpoints = obs_data_get_bool(settings, "use_legacy_endpoints");
	m_config.recommend_protocol = obs_data_get_string(settings, "recommend_protocol");
	m_config.recommend_ip = obs_data_get_string(settings, "recommend_ip");
	m_config.bitrate_kbps = static_cast<int>(obs_data_get_int(settings, "bitrate_kbps"));
	m_config.rtmp_backup_addr = obs_data_get_string(settings, "rtmp_backup_addr");
	m_config.rtmp_backup_code = obs_data_get_string(settings, "rtmp_backup_code");
	m_config.backup_stream_type_probe = obs_data_get_string(settings, "backup_stream_type_probe");
	m_config.stream_biz_probe = obs_data_get_string(settings, "stream_biz_probe");
	m_config.client_traceid_probe = obs_data_get_string(settings, "client_traceid_probe");
	m_config.access_key = obs_data_get_string(settings, "access_key");
	m_config.refresh_token = obs_data_get_string(settings, "refresh_token");
	// 线路表(ADR-0005):JSON 数组持久化,便于停播后仍可抄地址。
	{
		m_config.stream_targets.clear();
		std::string err;
		json11::Json arr = json11::Json::parse(obs_data_get_string(settings, "stream_targets"), err);
		for (const auto &item : arr.array_items()) {
			Bili::Config::StreamTarget t;
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

	if (m_config.room_id.empty())
		m_config.room_id = "12345";
	if (m_config.csrf_token.empty())
		m_config.csrf_token = "your_csrf_token";
	if (m_config.title.empty())
		m_config.title = "我的直播";
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
	obs_data_set_string(settings, "rtmp_addr", m_config.rtmp_addr.c_str());
	obs_data_set_string(settings, "rtmp_code", m_config.rtmp_code.c_str());
	obs_data_set_int(settings, "part_id", m_config.part_id);
	obs_data_set_int(settings, "area_id", m_config.area_id);
	obs_data_set_bool(settings, "use_legacy_endpoints", m_config.use_legacy_endpoints);
	obs_data_set_string(settings, "recommend_protocol", m_config.recommend_protocol.c_str());
	obs_data_set_string(settings, "recommend_ip", m_config.recommend_ip.c_str());
	obs_data_set_int(settings, "bitrate_kbps", m_config.bitrate_kbps);
	obs_data_set_string(settings, "rtmp_backup_addr", m_config.rtmp_backup_addr.c_str());
	obs_data_set_string(settings, "rtmp_backup_code", m_config.rtmp_backup_code.c_str());
	obs_data_set_string(settings, "backup_stream_type_probe",
			    m_config.backup_stream_type_probe.c_str());
	obs_data_set_string(settings, "stream_biz_probe", m_config.stream_biz_probe.c_str());
	obs_data_set_string(settings, "client_traceid_probe", m_config.client_traceid_probe.c_str());
	obs_data_set_string(settings, "access_key", m_config.access_key.c_str());
	obs_data_set_string(settings, "refresh_token", m_config.refresh_token.c_str());
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
	std::error_code ec;
	try {
		auto configPath = std::filesystem::u8path(configFile);
		std::filesystem::create_directories(configPath.parent_path(), ec);
	} catch (const std::exception &e) {
		blog(LOG_WARNING, "[obs-bilibili-stream] 创建配置目录失败: %s", e.what());
	}
	if (!obs_data_save_json(settings, configFile)) {
		blog(LOG_WARNING, "[obs-bilibili-stream] 配置保存失败: %s", configFile);
	}
	obs_data_release(settings);
	bfree(configFile);
}
} // namespace Core
