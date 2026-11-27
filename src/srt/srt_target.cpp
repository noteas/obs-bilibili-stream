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

bool IsSrtLine(const std::string &url)
{
	return StartsWithNoCase(Trim(url), kSrtScheme);
}

bool ParseTarget(const std::string &server_url, const std::string &stream_key, Target &out, std::string &error)
{
	out = Target{};

	const std::string url = Trim(server_url);
	const std::string key = Trim(stream_key);
	if (url.empty()) {
		error = "推流地址为空";
		return false;
	}

	if (!StartsWithNoCase(url, kSrtScheme)) {
		error = "仅支持 rtmpsrt:// 线路（B站私有 RTMP over SRT）；普通 rtmp:// 请用 OBS 原生推流";
		return false;
	}

	const size_t host_begin = std::char_traits<char>::length(kSrtScheme);
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

	// librtmp(parseurl.c) 不解析 '?'：app = 第一个 '/' 之后的全部（**含查询串**），
	// 只在结尾是 '/' 时裁掉一个；tcUrl = URL 起始至 app 末尾（同样含查询串，端口不补写）。
	// 口径出处 = E-25 §2（B站 fork 用的就是 OBS vendored librtmp 这段逻辑）。
	std::string rest = url.substr(slash + 1);
	std::string app = rest;
	if (!app.empty() && app.back() == '/')
		app.pop_back();
	if (app.empty()) {
		error = "地址缺少应用名（应形如 rtmpsrt://host/live-bvc/）";
		return false;
	}

	// 查询串（含 '?'）：推流码为空时的回退来源，以及「地址已带参数」的判定。
	std::string query;
	const size_t qmark = rest.find('?');
	if (qmark != std::string::npos)
		query = rest.substr(qmark);

	// publish 名 = 推流码原文（E-25 §3：librtmp 只做 slist/%XX/扩展名三类清洗，B站 key 都不触发）。
	// 地址里带查询串 + 另填报流码是官方的正常形态（E-24 §6：鉴权参数在 URL 里，key 另有其值），
	// 所以两者可以同时存在，不报错。
	std::string stream = key;
	if (stream.empty()) {
		if (query.empty()) {
			error = "缺少推流码（线路条目里的 code，形如 ?streamname=…&key=…）";
			return false;
		}
		// 推流码留空但地址自带查询串时，退化为用查询串当 publish 名（官方在此形态下会发空名，
		// 未见服务端接受过；这里取「至少能试一次」的口径，已在 E-25 §3 标注为实现扩展）。
		stream = query;
	}

	out.server_url = url;
	out.host = host;
	out.port = port;
	out.app = app;
	// tcUrl = 原 URL 起始至 app 末尾（E-25 §2）：等价于「裁掉结尾那个 '/' 的完整 URL」，
	// 端口按 URL 原样（没写就不补 :1935）。app 从 slash+1 起算，故长度 = slash + 1 + app.size()。
	out.tc_url = url.substr(0, slash + 1 + app.size());
	out.stream = stream;
	return true;
}

} // namespace Srt
