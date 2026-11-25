#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <functional>

namespace Http {
struct HttpResponse {
	long status;
	std::string data;
	std::string cookies;
	bool timeout = false;
};

class HttpClient {
public:
	static void init();
	static void cleanup();
	static HttpResponse get(const std::string &url, const std::vector<std::string> &headers = {},
				long timeout_ms = 10000);
	static HttpResponse post(const std::string &url, const std::string &data,
				 const std::vector<std::string> &headers = {}, long timeout_ms = 10000);
	// NOTE: the getAsync/postAsync callbacks still run on the HTTP worker thread;
	// use runAsync() below when the result has to reach the OBS UI thread.
	static void getAsync(const std::string &url, const std::vector<std::string> &headers,
			     std::function<void(HttpResponse)> callback, long timeout_ms = 10000);
	static void postAsync(const std::string &url, const std::string &data, const std::vector<std::string> &headers,
			      std::function<void(HttpResponse)> callback, long timeout_ms = 10000);

	// Runs `work` on the HTTP worker thread, so a blocking network call can never
	// stall the OBS UI thread, and delivers `done` on the OBS UI thread through
	// the libobs UI task queue. `done` is dropped once the module is unloading,
	// so it is safe to touch plugin state from it.
	// `work` must not touch UI objects: capture what it needs by value instead of
	// reading plugin state.
	static void runAsync(std::function<void()> work, std::function<void()> done);

	// True once cleanup() has been called (module unload); long-running jobs on
	// the worker thread poll this to bail out early.
	static bool isStopping();

	// Posts `fn` onto the OBS UI thread immediately (no HTTP work involved).
	// Same unload-drop guarantee as runAsync's `done`.
	static void postToUi(std::function<void()> fn);
};
} // namespace Http
