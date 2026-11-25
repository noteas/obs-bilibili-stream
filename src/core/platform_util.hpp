#pragma once
#include <string>

namespace Core {
// Opens an http(s) URL or a local file in the user's default browser, detached
// from the plugin (QR pages, live room pages, captcha pages). Returns false only
// when the launcher could not be started, so the caller can fall back to
// showing the URL as text.
bool openInBrowser(const std::string &urlOrPath);
} // namespace Core
