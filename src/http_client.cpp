#include "http_client.hpp"
#include <curl/curl.h>
#include <obs.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <exception>
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include "plugin_utils.hpp"
#include "util/base.h"

#ifdef _WIN32
#define strncasecmp _strnicmp
#endif
#include <queue>

namespace Http {

// ---------------------------------------------------------------------------
// fingerprint constants (contract §1.1; pinned to the reverse-engineering
// baseline LiveHime 8.16.0.11349, UA template E-02 §5 — deliberately *not*
// tied to the plugin's own version, and deliberately os/Windows on every
// platform: the fingerprint is a clone, not a host description)
// ---------------------------------------------------------------------------
static const char *const LIVEHIME_UA =
	"LiveHime/8.16.0.11349 os/Windows pc_app/livehime build/11349 osVer/10.0_22631 arch/x64";

// Keys the server rotates that must survive a restart (login session + the
// buvid3/buvid4 device identity). Everything else the server sets stays
// in-memory only.
static const char *const PERSIST_COOKIE_KEYS[] = {"SESSDATA", "bili_jct", "DedeUserID", "buvid3", "buvid4"};

// client_traceid 恒不发:E-21 证实官方仅在会话 traceid 非空时才发该头,插件
// 无服务端来源即应缺省不发(此前自造随机 UUID 属虚构数据,见 E-21)。3.0.2
// 起不再保留任何注入通道。

// ---------------------------------------------------------------------------
// cookie jar + identity state (mutex-guarded: mutated on the worker thread by
// response merges, read from the UI thread by the persistence plumbing)
// ---------------------------------------------------------------------------
namespace {
std::mutex jar_mutex;
std::map<std::string, std::string> cookie_jar;
std::function<void(const std::string &)> cookie_change_callback;

bool is_persistent_key(const std::string &key)
{
	for (const char *candidate : PERSIST_COOKIE_KEYS) {
		if (key == candidate)
			return true;
	}
	return false;
}

// Caller holds jar_mutex.
std::string serialize_jar_locked(bool persistent_only)
{
	std::string out;
	for (const auto &entry : cookie_jar) {
		if (persistent_only && !is_persistent_key(entry.first))
			continue;
		if (!out.empty())
			out += "; ";
		out += entry.first + "=" + entry.second;
	}
	return out;
}

// Merges "k=v[; k=v]*" pairs into the jar; returns true when a persistent key
// changed. Empty pairs/garbage are skipped, never fatal.
bool merge_pairs_locked(const std::string &pairs)
{
	bool persistent_changed = false;
	size_t pos = 0;
	while (pos < pairs.size()) {
		size_t end = pairs.find(';', pos);
		if (end == std::string::npos)
			end = pairs.size();
		std::string pair = pairs.substr(pos, end - pos);
		pos = end + 1;
		size_t start = pair.find_first_not_of(" \t");
		if (start == std::string::npos)
			continue;
		size_t stop = pair.find_last_not_of(" \t");
		pair = pair.substr(start, stop - start + 1);
		size_t eq = pair.find('=');
		if (eq == std::string::npos || eq == 0)
			continue;
		const std::string key = pair.substr(0, eq);
		const std::string value = pair.substr(eq + 1);
		if (value.empty())
			continue; // deletion-style Set-Cookie: keep what we have
		auto it = cookie_jar.find(key);
		if (it != cookie_jar.end() && it->second == value)
			continue;
		cookie_jar[key] = value;
		if (is_persistent_key(key))
			persistent_changed = true;
	}
	return persistent_changed;
}

// Applies a jar mutation and, when persistent keys changed, notifies the
// callback outside the lock (handlers post to the UI thread, never re-enter).
void apply_and_notify(bool persistent_changed)
{
	if (!persistent_changed)
		return;
	std::function<void(const std::string &)> callback;
	std::string snapshot;
	{
		std::lock_guard<std::mutex> lock(jar_mutex);
		callback = cookie_change_callback;
		snapshot = serialize_jar_locked(true);
	}
	if (callback)
		callback(snapshot);
}
} // namespace

void HttpClient::load_session_cookies(const std::string &cookie_header)
{
	bool changed;
	{
		std::lock_guard<std::mutex> lock(jar_mutex);
		changed = merge_pairs_locked(cookie_header);
	}
	// Seeding from config.json does not count as a server rotation: no notify.
	(void)changed;
}

std::string HttpClient::cookie_header()
{
	std::lock_guard<std::mutex> lock(jar_mutex);
	return serialize_jar_locked(false);
}

std::string HttpClient::persistent_cookie_header()
{
	std::lock_guard<std::mutex> lock(jar_mutex);
	return serialize_jar_locked(true);
}

std::string HttpClient::cookie_value(const std::string &key)
{
	std::lock_guard<std::mutex> lock(jar_mutex);
	auto it = cookie_jar.find(key);
	return it == cookie_jar.end() ? std::string() : it->second;
}

bool HttpClient::has_cookie(const std::string &key)
{
	std::lock_guard<std::mutex> lock(jar_mutex);
	return cookie_jar.count(key) != 0;
}

void HttpClient::inject_cookie(const std::string &key, const std::string &value)
{
	bool changed;
	{
		std::lock_guard<std::mutex> lock(jar_mutex);
		auto it = cookie_jar.find(key);
		changed = !(it != cookie_jar.end() && it->second == value);
		if (changed)
			cookie_jar[key] = value;
		changed = changed && is_persistent_key(key);
	}
	apply_and_notify(changed);
}

void HttpClient::set_cookie_change_callback(std::function<void(const std::string &)> cb)
{
	std::lock_guard<std::mutex> lock(jar_mutex);
	cookie_change_callback = std::move(cb);
}

const std::string &HttpClient::traceid()
{
	static const std::string empty;
	return empty;
}

const char *HttpClient::user_agent()
{
	return LIVEHIME_UA;
}

static size_t writeCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	auto *response = static_cast<std::string *>(userp);
	response->append(static_cast<char *>(contents), realsize);
	return realsize;
}

static size_t headerCallback(char *buffer, size_t size, size_t nitems, void *userp)
{
	size_t realsize = size * nitems;
	auto *cookies = static_cast<std::string *>(userp);
	const char *set_cookie = "Set-Cookie: ";
	// libcurl does not guarantee a NUL-terminated buffer and may deliver short
	// header lines (such as the terminating CRLF), so never read past realsize.
	if (realsize > strlen(set_cookie) && strncasecmp(buffer, set_cookie, strlen(set_cookie)) == 0) {
		std::string cookie(buffer + strlen(set_cookie), realsize - strlen(set_cookie));
		size_t end = cookie.find(';');
		if (end != std::string::npos)
			cookie = cookie.substr(0, end);
		if (cookie.empty() || cookie.find('=') == std::string::npos) {
			return realsize;
		}
		if (!cookies->empty())
			*cookies += "; ";
		*cookies += cookie;
	}
	return realsize;
}

// Official native header set (E-08 §1): UA / Cookie / client_traceid /
// Content-Type / Accept-Language — no Accept, no Origin/Referer, no Sec-Ch-Ua
// family, no charset on Content-Type. libcurl's automatic "Accept: */*" is
// suppressed with the empty-value header trick.
static struct curl_slist *build_fingerprint_headers(bool is_post)
{
	struct curl_slist *list = nullptr;
	list = curl_slist_append(list, (std::string("User-Agent: ") + LIVEHIME_UA).c_str());
	list = curl_slist_append(list, "Accept-Language: zh-CN,zh;q=0.9");
	list = curl_slist_append(list, "Accept:");
	if (is_post)
		list = curl_slist_append(list, "Content-Type: application/x-www-form-urlencoded");
	if (!HttpClient::traceid().empty())
		list = curl_slist_append(list, ("client_traceid: " + HttpClient::traceid()).c_str());
	const std::string cookies = HttpClient::cookie_header();
	if (!cookies.empty())
		list = curl_slist_append(list, ("Cookie: " + cookies).c_str());
	return list;
}

// Merges this response's Set-Cookie capture into the session jar and notifies
// persistent-key rotations (server-side SESSDATA refresh must not be lost).
static void absorb_response_cookies(const std::string &captured)
{
	bool persistent_changed;
	{
		std::lock_guard<std::mutex> lock(jar_mutex);
		persistent_changed = merge_pairs_locked(captured);
	}
	apply_and_notify(persistent_changed);
}

// All background work runs as opaque jobs on the single worker thread, which
// also serializes the libcurl calls made by HttpClient::get()/post().
using AsyncJob = std::function<void()>;

static std::queue<AsyncJob> async_queue;
static std::mutex queue_mutex;
static std::condition_variable queue_cv;
static bool stop_worker = false;
static std::thread worker_thread;

static bool workerStopping()
{
	std::lock_guard<std::mutex> lock(queue_mutex);
	return stop_worker;
}

static void enqueue(AsyncJob job)
{
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		if (stop_worker)
			return; // module is unloading: refuse new work
		async_queue.push(std::move(job));
	}
	queue_cv.notify_one();
}

// A completed job is handed back to the OBS UI thread through libobs's task
// queue (OBS_TASK_UI). Once the module starts unloading, continuations are
// dropped: the plugin state they capture may already be gone.
static std::atomic<bool> ui_delivery_allowed{true};

struct UiTask {
	std::function<void()> fn;
};

static void runUiTask(void *param)
{
	std::unique_ptr<UiTask> task(static_cast<UiTask *>(param));
	if (!ui_delivery_allowed.load())
		return;
	if (task->fn)
		task->fn();
}

static void deliverToUiThread(std::function<void()> done)
{
	if (!done)
		return;
	if (!ui_delivery_allowed.load())
		return;
	obs_queue_task(OBS_TASK_UI, runUiTask, new UiTask{std::move(done)}, false);
}

static void workerLoop()
{
	while (true) {
		std::unique_lock<std::mutex> lock(queue_mutex);
		queue_cv.wait(lock, [] { return !async_queue.empty() || stop_worker; });
		if (stop_worker) {
			// Shutting down: drop queued jobs and exit. Draining them here
			// could block module unload for the full timeout of every pending
			// request.
			while (!async_queue.empty())
				async_queue.pop();
			break;
		}
		AsyncJob job = async_queue.front();
		async_queue.pop();
		lock.unlock();

		try {
			job();
		} catch (const std::exception &error) {
			obs_log(LOG_ERROR, "异步任务执行失败: %s", error.what());
		} catch (...) {
			obs_log(LOG_ERROR, "异步任务执行失败: 未知异常");
		}
	}
}

void HttpClient::init()
{
	if (worker_thread.joinable())
		return;
	curl_global_init(CURL_GLOBAL_ALL);
	stop_worker = false;
	worker_thread = std::thread(workerLoop);
}

void HttpClient::cleanup()
{
	ui_delivery_allowed.store(false);
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		stop_worker = true;
	}
	queue_cv.notify_all();
	// Never block OBS shutdown: the worker exits by itself once the request
	// in flight (if any) returns. curl_global_cleanup() is skipped for the
	// same reason; it is optional at process exit.
	if (worker_thread.joinable())
		worker_thread.detach();
	// Drop the persistence hook so a late worker-side merge cannot call into
	// a destroyed console.
	{
		std::lock_guard<std::mutex> lock(jar_mutex);
		cookie_change_callback = nullptr;
	}
}

// Shared transport for get()/post()/get_following().
static HttpResponse perform(const std::string &url, const std::string *body, bool follow_redirects, long timeout_ms)
{
	HttpResponse response;
	response.status = 0;
	CURL *curl = curl_easy_init();
	if (!curl) {
		response.data = "CURL 初始化失败";
		return response;
	}

	std::string response_data;
	std::string response_cookies;
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	if (body) {
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
	}
	if (follow_redirects) {
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	}
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_cookies);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	struct curl_slist *header_list = build_fingerprint_headers(body != nullptr);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		response.timeout = (res == CURLE_OPERATION_TIMEDOUT);
		response.data = std::string("网络错误: ") + curl_easy_strerror(res);
		response.status = 0;
		curl_slist_free_all(header_list);
		curl_easy_cleanup(curl);
		return response;
	}

	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
	response.data = std::move(response_data);
	response.cookies = std::move(response_cookies);
	curl_slist_free_all(header_list);
	curl_easy_cleanup(curl);
	absorb_response_cookies(response.cookies);
	return response;
}

HttpResponse HttpClient::get(const std::string &url, long timeout_ms)
{
	return perform(url, nullptr, false, timeout_ms);
}

HttpResponse HttpClient::get_following(const std::string &url, long max_hops, long timeout_ms)
{
	(void)max_hops; // capped by CURLOPT_MAXREDIRS in perform()
	return perform(url, nullptr, true, timeout_ms);
}

HttpResponse HttpClient::post(const std::string &url, const std::string &data, long timeout_ms)
{
	return perform(url, &data, false, timeout_ms);
}

void HttpClient::runAsync(std::function<void()> work, std::function<void()> done)
{
	enqueue([work = std::move(work), done = std::move(done)]() {
		try {
			work();
		} catch (const std::exception &error) {
			// Swallow the failure, but still deliver `done`: the callers rely
			// on it to clear their busy state. The continuation reports the
			// error through the shared result struct it captured.
			obs_log(LOG_ERROR, "异步任务执行失败: %s", error.what());
		} catch (...) {
			obs_log(LOG_ERROR, "异步任务执行失败: 未知异常");
		}
		if (workerStopping())
			return; // unload in progress: drop UI continuations
		deliverToUiThread(std::move(done));
	});
}

bool HttpClient::isStopping()
{
	return workerStopping();
}

void HttpClient::postToUi(std::function<void()> fn)
{
	deliverToUiThread(std::move(fn));
}
} // namespace Http
