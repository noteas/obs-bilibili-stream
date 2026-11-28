#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <functional>

namespace Http {
struct HttpResponse {
	long status;
	std::string data;
	// Raw "k=v; k=v" capture of every Set-Cookie seen on this response
	// (informational; the session jar is updated automatically).
	std::string cookies;
	// 3xx 响应时 libcurl 算出的绝对跳转目标(get()/post() 从不自动跟随)。
	std::string redirect_url;
	bool timeout = false;
};

// Single egress for HTTP and for the request fingerprint: every request leaves
// through get()/post() carrying the fixed LiveHime UA, the session
// client_traceid and the cookie jar below — callers cannot add headers.
// Cookie/UA semantics follow docs/api/livehime-api.md §1.1 (E-08 header set).
class HttpClient {
public:
	static void init();
	// Module unload: refuses new jobs, aborts the request in flight, wakes
	// sleeping jobs and **joins** the worker thread, so no plugin code keeps
	// running once the module is unmapped (dlclose). The wait is bounded (a
	// wedged transport falls back to detach + error log instead of hanging
	// OBS shutdown forever).
	static void cleanup();

	static HttpResponse get(const std::string &url, long timeout_ms = 10000);
	static HttpResponse post(const std::string &url, const std::string &data, long timeout_ms = 10000);
	// GET that follows redirects (default get()/post() never follow: callers
	// consume 302 Set-Cookies directly, e.g. the crossDomain ticket hop). SSO
	// propagation needs each domain's full 302 chain so every hop's cookies
	// reach the jar. Redirects are followed **one request at a time**: every
	// hop re-evaluates the cookie allowlist for its own host and absorbs the
	// previous hop's Set-Cookie (CURLOPT_FOLLOWLOCATION would replay the first
	// hop's Cookie header on every later host).
	static HttpResponse get_following(const std::string &url, long max_hops = 5, long timeout_ms = 10000);

	// Runs `work` on the HTTP worker thread, so a blocking network call can never
	// stall the OBS UI thread, and delivers `done` on the OBS UI thread through
	// the libobs UI task queue. `done` is dropped once the module is unloading,
	// so it is safe to touch plugin state from it.
	// `work` must not touch UI objects: capture what it needs by value instead of
	// reading plugin state.
	static void runAsync(std::function<void()> work, std::function<void()> done);

	// True once cleanup() has been called (module unload); jobs on the worker
	// thread poll this to bail out early.
	static bool isStopping();

	// Worker-thread sleep used by polling loops (login QR poll). Returns false
	// as soon as the module starts unloading — never sleep_for() there: the
	// thread must be able to exit promptly for cleanup() to join it.
	static bool sleep_unless_stopping(long ms);

	// Posts `fn` onto the OBS UI thread immediately (no HTTP work involved).
	// Same unload-drop guarantee as runAsync's `done`.
	static void postToUi(std::function<void()> fn);

	// --- session identity (cookie jar + traceid + UA) ---------------------
	// The jar is domain-agnostic (single Bilibili session, as the plugin has
	// always done); every key is sent on every request, but only a whitelist
	// (SESSDATA/bili_jct/DedeUserID/buvid3/buvid4) is considered persistent.

	// Seeds the jar from a persisted "k=v; k=v" cookie string (config.json).
	static void load_session_cookies(const std::string &cookie_header);
	// Full jar serialized for sending; "" when empty.
	static std::string cookie_header();
	// Jar serialized down to the persistent whitelist keys (config.json form).
	static std::string persistent_cookie_header();
	static std::string cookie_value(const std::string &key);
	static bool has_cookie(const std::string &key);
	// Adds/overwrites one key server-side style: triggers the change callback
	// when a persistent key changes (used by the finger/spi buvid bootstrap).
	static void inject_cookie(const std::string &key, const std::string &value);

	// Fires (on the worker thread that observed the change) whenever a
	// Set-Cookie rotation or inject_cookie changes a persistent key; the payload
	// is the new persistent_cookie_header(). Handlers must not re-enter the jar.
	static void set_cookie_change_callback(std::function<void(const std::string &)> cb);

	// Session traceid sent as the client_traceid header. Always empty (E-21:
	// official omits the header when no session traceid; the plugin has no
	// server-side source and no longer fabricates one).
	static const std::string &traceid();
	// The fixed LiveHime UA string (contract §1.1 / E-02 §5).
	static const char *user_agent();
};
} // namespace Http
