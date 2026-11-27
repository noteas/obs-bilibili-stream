#include "http_client.hpp"
#include <curl/curl.h>
#include <obs.h>
#include <chrono>
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
// 故意泄漏到堆、永不释放：这些状态被 detach 的 worker 线程读写。OBS 关闭路径
// 刻意 detach 不 join（见 cleanup()），进程静态析构时可能仍有任务在跑——放在
// 静态存储期的 mutex/map/function 会在 worker 脚下析构，属未定义行为。堆块
// 永不析构，代价只有几 KB。
struct CookieJarState {
	std::mutex mutex;
	std::map<std::string, std::string> jar;
	std::function<void(const std::string &)> change_callback;
};
static CookieJarState &g_jar = *new CookieJarState;
std::mutex &jar_mutex = g_jar.mutex;
std::map<std::string, std::string> &cookie_jar = g_jar.jar;
std::function<void(const std::string &)> &cookie_change_callback = g_jar.change_callback;

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
		auto it = cookie_jar.find(key);
		if (value.empty()) {
			// 删除型 Set-Cookie(name=; Expires=过去):服务端注销会话,本地必须跟着清掉。
			// 旧版把它当「保持原值」 ⇒ 退出登录后仍带着失效的 SESSDATA。
			if (it == cookie_jar.end())
				continue;
			cookie_jar.erase(it);
			if (is_persistent_key(key))
				persistent_changed = true;
			continue;
		}
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
		// 「name=」(空值)要保留:它是服务端的删除指令,由 jar 侧解释。
		if (cookie.empty() || cookie.find('=') == std::string::npos) {
			return realsize;
		}
		if (!cookies->empty())
			*cookies += "; ";
		*cookies += cookie;
	}
	return realsize;
}

// Cookie 只发给 B站自有域名。jar 是域无关的(单会话、全键一副),而插件会跟随
// 服务端下发的任意跳转(login ticket、sso[] 列表、get_following 的每一跳);不设防时
// SESSDATA/bili_jct 会被原样带到任何目标主机。名单按「主机名后缀」匹配。
static const char *const kCookieHostSuffixes[] = {".bilibili.com",   ".biligame.com", ".bilibili.tv",
						  ".biliapi.net",    ".biliapi.com",  ".bilivideo.com",
						  ".acgvideo.com",   ".b23.tv",       ".bilicdn1.com",
						  ".hdslb.com"};

static std::string lower_ascii(std::string s)
{
	for (char &c : s)
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	return s;
}

// 从 URL 取主机名(小写);取不到返回空。
static std::string url_host(const std::string &url)
{
	size_t scheme = url.find("://");
	size_t begin = scheme == std::string::npos ? 0 : scheme + 3;
	size_t end = url.find_first_of("/?#", begin);
	std::string authority = url.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
	const size_t at = authority.rfind('@');
	if (at != std::string::npos)
		authority = authority.substr(at + 1);
	if (!authority.empty() && authority.front() == '[') { // [IPv6]:port
		const size_t close = authority.find(']');
		return lower_ascii(authority.substr(1, close == std::string::npos ? std::string::npos : close - 1));
	}
	const size_t colon = authority.rfind(':');
	return lower_ascii(colon == std::string::npos ? authority : authority.substr(0, colon));
}

static bool cookie_allowed_for_host(const std::string &host)
{
	if (host.empty())
		return false;
	for (const char *suffix : kCookieHostSuffixes) {
		const std::string tail = suffix + 1; // 去掉前导点,同时覆盖裸域与子域
		if (host == tail || (host.size() > tail.size() && host.compare(host.size() - tail.size(), tail.size(), tail) == 0))
			return true;
	}
	return false;
}

// Official native header set (E-08 §1): UA / Cookie / client_traceid /
// Content-Type / Accept-Language — no Accept, no Origin/Referer, no Sec-Ch-Ua
// family, no charset on Content-Type. libcurl's automatic "Accept: */*" is
// suppressed with the empty-value header trick.
static struct curl_slist *build_fingerprint_headers(bool is_post, const std::string &url)
{
	struct curl_slist *list = nullptr;
	list = curl_slist_append(list, (std::string("User-Agent: ") + LIVEHIME_UA).c_str());
	list = curl_slist_append(list, "Accept-Language: zh-CN,zh;q=0.9");
	list = curl_slist_append(list, "Accept:");
	if (is_post)
		list = curl_slist_append(list, "Content-Type: application/x-www-form-urlencoded");
	if (!HttpClient::traceid().empty())
		list = curl_slist_append(list, ("client_traceid: " + HttpClient::traceid()).c_str());
	if (cookie_allowed_for_host(url_host(url))) {
		const std::string cookies = HttpClient::cookie_header();
		if (!cookies.empty())
			list = curl_slist_append(list, ("Cookie: " + cookies).c_str());
	}
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

namespace {
// 同 cookie jar 的理由：worker 线程可能在进程析构阶段仍在跑任务，它用的
// queue/mutex/condition_variable 放在故意泄漏的堆块里，永不析构。std::thread
// 同样永不析构——joinable 线程的析构会直接 std::terminate，因此 detach（见
// cleanup()）就是它的唯一出口，不需要额外的退出守卫。
struct WorkerState {
	std::queue<AsyncJob> queue;
	std::mutex mutex;
	std::condition_variable cv;
	bool stop = false;
	std::thread thread;
	// 退出信号：workerLoop 返回前置位并广播，cleanup() 据此在有限时间内 join。
	std::mutex exit_mutex;
	std::condition_variable exit_cv;
	bool exited = false;
};
static WorkerState &g_worker = *new WorkerState;
std::queue<AsyncJob> &async_queue = g_worker.queue;
std::mutex &queue_mutex = g_worker.mutex;
std::condition_variable &queue_cv = g_worker.cv;
bool &stop_worker = g_worker.stop;
std::thread &worker_thread = g_worker.thread;

// 卸载时等待 worker 退出的上限。正常路径（请求被中止、轮询被唤醒）在毫秒级返回；
// 只有传输层卡死才会等满，此时宁可 detach 并记错误日志，也不无限期阻塞 OBS 退出。
constexpr auto kUnloadJoinTimeout = std::chrono::seconds(5);
} // namespace

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

	{
		std::lock_guard<std::mutex> lock(g_worker.exit_mutex);
		g_worker.exited = true;
	}
	g_worker.exit_cv.notify_all();
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
	queue_cv.notify_all(); // 唤醒 sleep_unless_stopping() 里的轮询循环
	// **必须 join，不能 detach**：obs_free_modules() 卸载模块时会 dlclose 本 .so，
	// 而 worker 线程的循环体、libcurl 回调、异常处理全是本模块的代码——线程还在跑
	// 就 unmap 代码页 = 退出 OBS 时的崩溃/未定义行为。停止请求经三条路径即时生效：
	// 队列里未开始的job被丢弃、curl 进度回调中止在途请求、轮询 sleep 被唤醒。
	// 等待有上限：传输层卡死时（理论上只剩阻塞式 DNS）退化为 detach + 错误日志，
	// 不无限期阻塞 OBS 退出。curl_global_cleanup() 同样跳过（进程退出时可选）。
	if (worker_thread.joinable()) {
		bool exited = false;
		{
			std::unique_lock<std::mutex> lock(g_worker.exit_mutex);
			exited = g_worker.exit_cv.wait_for(lock, kUnloadJoinTimeout, [] { return g_worker.exited; });
		}
		if (exited) {
			worker_thread.join();
		} else {
			worker_thread.detach();
			obs_log(LOG_ERROR, "HTTP worker 未在 %lld 秒内退出，已 detach（模块卸载后可能仍有代码在运行）",
				static_cast<long long>(kUnloadJoinTimeout.count()));
		}
	}
	// Drop the persistence hook so a late worker-side merge cannot call into
	// a destroyed console.
	{
		std::lock_guard<std::mutex> lock(jar_mutex);
		cookie_change_callback = nullptr;
	}
}

// 进度回调只做一件事：模块开始卸载就中止传输。
static int abortWhenStopping(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	return workerStopping() ? 1 : 0;
}

// Shared transport for get()/post()/get_following(). Never follows redirects.
static HttpResponse perform(const std::string &url, const std::string *body, long timeout_ms)
{
	HttpResponse response;
	response.status = 0;
	CURL *curl = curl_easy_init();
	if (!curl) {
		response.data = Utils::Text("Http.CurlInitFail");
		return response;
	}

	std::string response_data;
	std::string response_cookies;
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	if (body) {
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
	}
	// 显式关闭:跳转由 get_following() 逐跳处理(见头文件注释)。
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_cookies);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	// 卸载时中止在途请求（返回非 0 ⇒ CURLE_ABORTED_BY_CALLBACK）：libcurl 约每秒
	// 调一次进度回调，包含连接/解析阶段，cleanup() 因此能在秒级内 join worker。
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abortWhenStopping);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);

	struct curl_slist *header_list = build_fingerprint_headers(body != nullptr, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		response.timeout = (res == CURLE_OPERATION_TIMEDOUT);
		// 传输层失败必须进日志:调用方只会把「网络错误」四个字放进状态行,没有 curl 的
		// 具体原因用户和开发者都无从排查。只记主机名与原因,不记 URL 查询串(含票据)。
		obs_log(res == CURLE_ABORTED_BY_CALLBACK ? LOG_INFO : LOG_WARNING, "HTTP 传输失败(%s): %s",
			url_host(url).c_str(), curl_easy_strerror(res));
		response.data = Utils::Text("Http.NetworkError", curl_easy_strerror(res));
		response.status = 0;
		curl_slist_free_all(header_list);
		curl_easy_cleanup(curl);
		return response;
	}

	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
	// FOLLOWLOCATION 关闭时 libcurl 仍会给出「本该跳去哪」的绝对 URL。
	if (response.status >= 300 && response.status < 400 && response.status != 304) {
		char *redirect = nullptr;
		if (curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirect) == CURLE_OK && redirect)
			response.redirect_url = redirect;
	}
	response.data = std::move(response_data);
	response.cookies = std::move(response_cookies);
	curl_slist_free_all(header_list);
	curl_easy_cleanup(curl);
	absorb_response_cookies(response.cookies);
	return response;
}

HttpResponse HttpClient::get(const std::string &url, long timeout_ms)
{
	return perform(url, nullptr, timeout_ms);
}

HttpResponse HttpClient::get_following(const std::string &url, long max_hops, long timeout_ms)
{
	HttpResponse response = perform(url, nullptr, timeout_ms);
	for (long hops = 0; hops < max_hops && !response.redirect_url.empty(); ++hops)
		response = perform(response.redirect_url, nullptr, timeout_ms);
	return response;
}

HttpResponse HttpClient::post(const std::string &url, const std::string &data, long timeout_ms)
{
	return perform(url, &data, timeout_ms);
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

bool HttpClient::sleep_unless_stopping(long ms)
{
	std::unique_lock<std::mutex> lock(queue_mutex);
	// enqueue() 的 notify_one 会把 wait_for 提前唤醒，谓词保证只对 stop_worker 提前返回。
	return !queue_cv.wait_for(lock, std::chrono::milliseconds(ms), [] { return stop_worker; });
}
} // namespace Http
