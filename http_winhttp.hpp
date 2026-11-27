/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "delayimp.lib")

namespace renodx::addons::dlss5::http {

struct Url {
  std::wstring host;
  std::wstring path;
  INTERNET_PORT port = 0;
  bool secure = false;
  bool loopback = false;
};

// Validate the authority WinHTTP will actually use before applying a caller's host policy.
inline bool ParseUrl(std::wstring_view url, Url* parsed) {
  if (parsed == nullptr || url.empty() || url.size() > 8192) return false;
  for (wchar_t c : url) {
    if (c <= L' ' || c == L'\\' || c == L'"' || c == L'<' || c == L'>' || c == L'#') return false;
  }
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = 1;
  parts.dwUserNameLength = parts.dwPasswordLength = 1;
  if (!WinHttpCrackUrl(url.data(), static_cast<DWORD>(url.size()), 0, &parts)
      || parts.dwHostNameLength == 0 || parts.nPort == 0
      || parts.dwUserNameLength != 0 || parts.dwPasswordLength != 0) return false;
  if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) return false;
  const size_t authority_at = url.find(L"://");
  if (authority_at == std::wstring_view::npos) return false;
  const auto authority = url.substr(authority_at + 3, url.find_first_of(L"/?", authority_at + 3) - (authority_at + 3));
  if (authority.find(L'@') != std::wstring_view::npos) return false;
  std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  for (wchar_t& c : host) {
    if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
  }
  if (host.size() > 253) return false;
  const bool ipv6 = host.front() == L'[' && host.back() == L']';
  for (size_t i = 0; i < host.size(); ++i) {
    const wchar_t c = host[i];
    if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'-') continue;
    if (ipv6 && (c == L':' || (i == 0 && c == L'[') || (i + 1 == host.size() && c == L']'))) continue;
    return false;
  }
  const bool loopback = host == L"localhost" || host == L"127.0.0.1" || host == L"[::1]";
  const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (path.empty()) path = L"/";
  if (parts.dwExtraInfoLength != 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  *parsed = {.host = std::move(host), .path = std::move(path), .port = parts.nPort,
             .secure = secure, .loopback = loopback};
  return true;
}

struct CloseHandle {
  void operator()(void* handle) const noexcept {
    const DWORD error = GetLastError();
    WinHttpCloseHandle(handle);
    SetLastError(error);
  }
};
using Handle = std::unique_ptr<void, CloseHandle>;

inline Handle OpenSession(const wchar_t* user_agent, bool loopback, DWORD* error) {
  auto access = (loopback ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY);
  Handle session(WinHttpOpen(user_agent, access, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session && !loopback) {
    session.reset(WinHttpOpen(user_agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  }
  if (!session) {
    *error = GetLastError();
    return {};
  }
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
  if (!WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
    protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    if (!WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
      *error = GetLastError();
      return {};
    }
  }
  return session;
}

inline bool DisableAmbientFeatures(HINTERNET request) {
  DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
  return WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
}

inline bool QueryStatus(HINTERNET request, DWORD* status) {
  DWORD bytes = sizeof(*status);
  return WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, status, &bytes, WINHTTP_NO_HEADER_INDEX);
}

}  // namespace renodx::addons::dlss5::http
