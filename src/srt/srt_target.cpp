#include "srt/srt_target.hpp"

#include <cctype>
#include <cstdlib>

namespace Srt {

namespace {

constexpr uint16_t kDefaultSrtPort = 1935; // E-24：官方 rtmpsrt 默认端口（UDP 1935）

std::string Trim(const std::string &s)
{
	size_t b = 0;
	size_t e = s.size();
	while (b < e && (std::isspace(static_cast<unsigned char>(s[b])) || s[b] == '\r' || s[b] == '\n'))
		++b;
	while (e > b && (std::isspace(static_cast<unsigned char>(s[e - 1])) || s[e - 1] == '\r' || s[e - 1] == '\n'))
		--e;
	return s.substr(b, e - b);
}

bool StartsWithNoCase(const std::string &s, const std::string &prefix)
{
	if (s.size() < prefix.size())
		return false;
	for (size_t i = 0; i < prefix.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(s[i])) !=
		    std::tolower(static_cast<unsigned char>(prefix[i])))
			return false;
	}
	return true;
}

bool AllDigits(const std::string &s)
{
	if (s.empty())
		return false;
	for (char c : s) {
		if (!std::isdigit(static_cast<unsigned char>(c)))
			return false;
	}
	return true;
}

} // namespace

bool ParseTarget(const std::string &server_url, const std::string &stream_key, Target &out, std::string &error)
{
	out = Target{};

	const std::string url = Trim(server_url);
	const std::string key = Trim(stream_key);
	if (url.empty()) {
		error = "推流地址为空";
		return false;
	}

	const std::string scheme = "rtmpsrt://";
	if (!StartsWithNoCase(url, scheme)) {
		error = "仅支持 rtmpsrt:// 线路（B站私有 RTMP over SRT）；普通 rtmp:// 请用 OBS 原生推流";
		return false;
	}

	const size_t host_begin = scheme.size();
	const size_t slash = url.find('/', host_begin);
	if (slash == std::string::npos) {
		error = "地址缺少应用路径（应形如 rtmpsrt://host/live-bvc/）";
		return false;
	}

	std::string hostport = url.substr(host_begin, slash - host_begin);
	if (hostport.empty()) {
		error = "地址缺少主机名";
		return false;
	}

	// host[:port]，兼容 [IPv6]:port。
	std::string host;
	std::string port_text;
	if (hostport.front() == '[') {
		const size_t close = hostport.find(']');
		if (close == std::string::npos) {
			error = "IPv6 地址缺少 ']'";
			return false;
		}
		host = hostport.substr(1, close - 1);
		if (close + 1 < hostport.size()) {
			if (hostport[close + 1] != ':') {
				error = "IPv6 地址端口格式错误";
				return false;
			}
			port_text = hostport.substr(close + 2);
		}
	} else {
		const size_t colon = hostport.rfind(':');
		if (colon != std::string::npos) {
			host = hostport.substr(0, colon);
			port_text = hostport.substr(colon + 1);
		} else {
			host = hostport;
		}
	}

	if (host.empty()) {
		error = "地址缺少主机名";
		return false;
	}

	uint16_t port = kDefaultSrtPort;
	if (!port_text.empty()) {
		if (!AllDigits(port_text)) {
			error = "端口不是数字: " + port_text;
			return false;
		}
		const long p = std::strtol(port_text.c_str(), nullptr, 10);
		if (p < 1 || p > 65535) {
			error = "端口超出范围: " + port_text;
			return false;
		}
		port = static_cast<uint16_t>(p);
	}

	// 路径：app = 第一段（去掉尾部 '/'）；查询串可作为推流码的回退来源。
	std::string path = url.substr(slash + 1);
	std::string query;
	const size_t qmark = path.find('?');
	if (qmark != std::string::npos) {
		query = path.substr(qmark); // 含 '?'
		path = path.substr(0, qmark);
	}

	const size_t app_end = path.find('/');
	std::string app = path.substr(0, app_end);
	while (!app.empty() && app.back() == '/')
		app.pop_back();
	if (app.empty()) {
		error = "地址缺少应用名（应形如 rtmpsrt://host/live-bvc/）";
		return false;
	}

	std::string stream = key;
	if (stream.empty()) {
		if (query.empty()) {
			error = "缺少推流码（线路条目里的 code，形如 ?streamname=…&key=…）";
			return false;
		}
		stream = query;
	} else if (!query.empty()) {
		error = "地址已带查询串，同时又填了推流码；请只保留一种写法";
		return false;
	}

	out.server_url = url;
	out.host = host;
	out.port = port;
	out.app = app;
	out.stream = stream;
	// tcUrl = 原 URL 截到 app 末尾（不含尾部 '/'，不含查询串），与 librtmp 截断口径一致。
	out.tc_url = url.substr(0, slash + 1 + app.size());
	return true;
}

} // namespace Srt
