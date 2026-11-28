// 测试专用 LD_PRELOAD 垫片:让 libcurl(OpenSSL 后端)在它自带的系统 CA 之外再信任
// 假服务端的测试 CA(环境变量 BILI_TEST_CA=<pem 路径>)。
//
// 为什么要它:libcurl 的**库 API**不读 SSL_CERT_FILE(只有 curl 命令行工具读),CA 路径
// 是编译期固定 / CURLOPT_CAFILE;而被测插件不许为测试改动一行。系统 libcurl(8.5 +
// OpenSSL 3.0)把编译期 CA 包经 X509_STORE_load_file() 装进 SSL_CTX 的默认 store,所以
// 垫片拦截这一个入口:先按原参数调真函数,再把测试 CA 追加进同一个 store。
// 只给 tests/harness/run_headless.sh 用(CMake 目标 bili_test_ca_shim)。
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>

// 工程全局 -fvisibility=hidden:被拦截符号必须显式导出,否则 LD_PRELOAD 拦不到。
#define SHIM_EXPORT __attribute__((visibility("default")))

#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

typedef int (*load_file_fn)(X509_STORE *, const char *);

SHIM_EXPORT int X509_STORE_load_file(X509_STORE *store, const char *file)
{
	load_file_fn real = (load_file_fn)dlsym(RTLD_NEXT, "X509_STORE_load_file");
	const int rc = real ? real(store, file) : 0;
	const char *ca = getenv("BILI_TEST_CA");
	if (real && ca && *ca && ca != file)
		(void)real(store, ca);
	return rc;
}
