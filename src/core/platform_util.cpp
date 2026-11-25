#include "core/platform_util.hpp"

#ifdef _WIN32
#include <windows.h>
#include <string>
#else
#include <spawn.h>
#include <unistd.h>
#include <fcntl.h>
extern char **environ;
#endif

namespace Core {

#ifdef _WIN32
static std::wstring toWide(const std::string &utf8)
{
	if (utf8.empty())
		return std::wstring();
	const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
	std::wstring wide(len, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), wide.data(), len);
	return wide;
}
#else
static bool spawnLauncher(const char *program, const char *arg)
{
	const char *argv[] = {program, arg, nullptr};
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	// The launcher prints noise to stdout/stderr; send it nowhere.
	posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY | O_CREAT | O_APPEND, 0644);
	posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY | O_CREAT | O_APPEND, 0644);
	pid_t pid = 0;
	// posix_spawnp (NOT posix_spawn): "xdg-open" is a bare PATH name and
	// posix_spawn does not search PATH — it fails ENOENT silently, which was
	// the "browser never opens" bug (verified: spawn rc=2, spawnp rc=0).
	const int rc = posix_spawnp(&pid, program, &actions, nullptr, const_cast<char *const *>(argv), environ);
	posix_spawn_file_actions_destroy(&actions);
	return rc == 0;
}
#endif

bool openInBrowser(const std::string &urlOrPath)
{
	if (urlOrPath.empty())
		return false;

#ifdef _WIN32
	// ShellExecuteW accepts both URLs and local paths; SEE_MASK_FLAG_NO_UI
	// would suppress its own error dialogs, which we want anyway: the return
	// value is the only signal the caller gets.
	HINSTANCE result = ShellExecuteW(nullptr, L"open", toWide(urlOrPath).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	return reinterpret_cast<INT_PTR>(result) > 32;
#elif defined(__APPLE__)
	return spawnLauncher("/usr/bin/open", urlOrPath.c_str());
#else
	return spawnLauncher("xdg-open", urlOrPath.c_str());
#endif
}

} // namespace Core
