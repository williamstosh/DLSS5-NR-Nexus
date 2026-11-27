/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

#include "brand.h"
#include "http_winhttp.hpp"
#include "preset_dialog.hpp"
#include "version.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "uuid.lib")

namespace renodx::addons::dlss5::update {

struct Settings {
  bool auto_check = true;
  std::filesystem::path download_directory;
  int64_t next_auto_check_utc = 0;
};

struct Asset {
  std::string name;
  uint64_t size = 0;
  std::string url;
  uint64_t id = 0;
};

struct Release {
  std::string version;
  std::string tag;
  std::string published_at;
  std::string body;
  std::string html_url;
  std::optional<Asset> asset;
};

enum class CheckState { IDLE, CHECKING, UPDATE_AVAILABLE, UP_TO_DATE, AHEAD, FAILED };
enum class DownloadState { IDLE, DOWNLOADING, COMPLETED, FAILED };
enum class Error {
  NONE,
  THREAD_START,
  CANCELLED,
  INVALID_URL,
  NETWORK,
  HTTP_STATUS,
  RATE_LIMITED,
  NO_RELEASE,
  INVALID_RELEASE,
  RESPONSE_TOO_LARGE,
  REDIRECT,
  DIRECTORY_UNAVAILABLE,
  FILE_CONFLICT,
  FILE_IO,
  SIZE_MISMATCH,
  DIALOG,
  SHELL,
  INTERNAL,
};

struct Failure {
  Error code = Error::NONE;
  uint32_t os_code = 0;
  uint32_t http_status = 0;
};

struct Snapshot {
  Settings settings;
  std::filesystem::path addon_directory;
  std::shared_ptr<const Release> release;
  CheckState check_state = CheckState::IDLE;
  DownloadState download_state = DownloadState::IDLE;
  bool busy = false;
  bool stale = false;
  uint64_t downloaded_bytes = 0;
  uint64_t download_size = 0;
  std::filesystem::path completed_path;
  int64_t retry_after_utc = 0;
  Failure check_error;
  Failure download_error;
  Failure action_error;
};

inline int64_t UtcNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

namespace internal {

constexpr char LATEST_URL[] = "https://api.github.com/repos/williamstosh/DLSS5-NR-Nexus/releases/latest";
constexpr int64_t HOUR = 3600;
constexpr int64_t DAY = 24 * HOUR;
constexpr size_t MAX_RELEASE_BYTES = 1024 * 1024;
using TransferClock = std::chrono::steady_clock;

enum class StopReason { NONE, DETACH };
static thread_local const std::atomic_bool* active_picker_cancel = nullptr;

inline bool IsStopping(const std::atomic<StopReason>& stop) {
  return stop.load(std::memory_order_relaxed) != StopReason::NONE;
}

struct TransferTimeouts {
  TransferClock::duration check = std::chrono::seconds(60);
  TransferClock::duration download = std::chrono::minutes(15);
};

template <typename Integer, typename Character>
bool Decimal(std::basic_string_view<Character> text, Integer* number) {
  if (text.empty()) return false;
  Integer value = 0;
  for (const auto c : text) {
    if (c < '0' || c > '9') return false;
    const auto digit = static_cast<Integer>(c - '0');
    if (value > ((std::numeric_limits<Integer>::max)() - digit) / 10) return false;
    value = static_cast<Integer>(value * 10 + digit);
  }
  *number = value;
  return true;
}

struct Version {
  std::array<uint16_t, 3> core{};
  bool prerelease = false;
};

inline std::optional<Version> ParseVersion(std::string_view text) {
  Version version;
  const auto separator = text.find_first_of("-+");
  auto core = text.substr(0, separator);
  for (size_t i = 0; i < version.core.size(); ++i) {
    const auto dot = core.find('.');
    const auto part = core.substr(0, dot);
    if ((dot == core.npos) != (i + 1 == version.core.size())
        || (part.size() > 1 && part[0] == '0') || !Decimal(part, &version.core[i])) return {};
    if (dot != core.npos) {
      core.remove_prefix(dot + 1);
    }
  }
  if (separator == text.npos) return version;

  auto suffix = text.substr(separator);
  version.prerelease = suffix.front() == '-';
  if (version.prerelease) {
    suffix.remove_prefix(1);
  }
  const auto valid_identifiers = [](std::string_view ids, bool prerelease) {
    for (;;) {
      const auto dot = ids.find('.');
      const auto id = ids.substr(0, dot);
      if (id.empty()) return false;
      bool numeric = true;
      for (const char c : id) {
        if (c >= '0' && c <= '9') continue;
        numeric = false;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-')) return false;
      }
      if (prerelease && numeric && id.size() > 1 && id[0] == '0') return false;
      if (dot == ids.npos) return true;
      ids.remove_prefix(dot + 1);
    }
  };
  const auto build = suffix.find('+');
  if (version.prerelease && !valid_identifiers(suffix.substr(0, build), true)) return {};
  if (build != suffix.npos && !valid_identifiers(suffix.substr(build + 1), false)) return {};
  return version;
}

inline CheckState CompareVersions(const Version& local, const Version& latest) {
  if (local.core > latest.core) return CheckState::AHEAD;
  if (local.core < latest.core || local.prerelease) return CheckState::UPDATE_AVAILABLE;
  return CheckState::UP_TO_DATE;
}

enum class UrlUse { API, ASSET, REDIRECT, RELEASE_PAGE };

// A nonzero port belongs only to an explicitly constructed test service.
struct Origin {
  INTERNET_PORT loopback_port = 0;

  bool Allows(const http::Url& url, UrlUse use) const {
    if (loopback_port != 0) {
      return !url.secure && url.host == L"127.0.0.1" && url.port == loopback_port;
    }
    if (!url.secure || url.port != INTERNET_DEFAULT_HTTPS_PORT) return false;
    switch (use) {
      case UrlUse::API:
        return url.host == L"api.github.com";
      case UrlUse::ASSET:
        return url.host == L"github.com";
      case UrlUse::REDIRECT:
        return url.host == L"release-assets.githubusercontent.com" || url.host == L"objects.githubusercontent.com";
      case UrlUse::RELEASE_PAGE:
        return url.host == L"github.com"
            && url.path.starts_with(L"/williamstosh/DLSS5-NR-Nexus/releases/tag/");
    }
    return false;
  }
};

inline bool TrustedUrl(std::string_view url, const Origin& origin, UrlUse use, http::Url* parts) {
  if (url.size() > 8192 || !std::ranges::all_of(url, [](char c) { return c > ' ' && c <= '~'; })) return false;
  return http::ParseUrl(std::wstring(url.begin(), url.end()), parts) && origin.Allows(*parts, use);
}

inline std::optional<Release> ParseRelease(std::string_view input, const Origin& origin) {
  using Json = nlohmann::json;
  const auto json = Json::parse(input, nullptr, false);
  if (!json.is_object()) return {};
  const auto read_string = [](const Json& object, const char* key) -> const std::string* {
    const auto entry = object.find(key);
    if (entry == object.end()) return nullptr;
    return entry->get_ptr<const std::string*>();
  };
  const auto flag_is_false = [&](const char* key) {
    const auto entry = json.find(key);
    return entry != json.end() && entry->is_boolean() && !entry->get<bool>();
  };
  const auto* tag = read_string(json, "tag_name");
  const auto* published_at = read_string(json, "published_at");
  const auto* html_url = read_string(json, "html_url");
  const auto assets = json.find("assets");
  if (!flag_is_false("draft") || !flag_is_false("prerelease") || tag == nullptr
      || published_at == nullptr || published_at->empty() || html_url == nullptr
      || assets == json.end() || !assets->is_array()) return {};
  std::string_view version = *tag;
  if (version.starts_with('v')) {
    version.remove_prefix(1);
  }
  const auto parsed = ParseVersion(version);
  http::Url page;
  if (!parsed || parsed->prerelease || !TrustedUrl(*html_url, origin, UrlUse::RELEASE_PAGE, &page)) return {};
  const auto body = json.find("body");
  if (body != json.end() && !body->is_null() && !body->is_string()) return {};
  Release release = {.version = std::string(version),
                     .tag = *tag,
                     .published_at = *published_at,
                     .body = (body != json.end() && body->is_string() ? body->get<std::string>() : std::string{}),
                     .html_url = *html_url};
  const auto prefix = std::string("NR-Nexus-v") + release.version + "-";
  constexpr std::string_view suffix = "-windows-x64.zip";
  bool matched_name = false;
  for (const auto& entry : *assets) {
    if (!entry.is_object()) continue;
    const auto* name = read_string(entry, "name");
    const auto* state = read_string(entry, "state");
    if (name == nullptr || state == nullptr || *state != "uploaded"
        || !name->starts_with(prefix) || !name->ends_with(suffix)
        || name->size() < prefix.size() + suffix.size() + 8) continue;
    const auto sha = std::string_view(*name).substr(prefix.size(), name->size() - prefix.size() - suffix.size());
    if (!std::ranges::all_of(sha, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) continue;
    if (matched_name) {
      release.asset.reset();
      break;
    }
    matched_name = true;
    const auto* url = read_string(entry, "browser_download_url");
    const auto size = entry.find("size");
    const auto id = entry.find("id");
    http::Url download;
    if (url == nullptr || size == entry.end() || id == entry.end()
        || !size->is_number_unsigned() || !id->is_number_unsigned()
        || !TrustedUrl(*url, origin, UrlUse::ASSET, &download)) continue;
    if (size->get<uint64_t>() != 0 && id->get<uint64_t>() != 0) {
      release.asset = Asset{.name = *name, .size = size->get<uint64_t>(), .url = *url, .id = id->get<uint64_t>()};
    }
  }
  return release;
}

struct Response {
  http::Handle session;
  http::Handle connection;
  http::Handle request;
  DWORD status = 0;
  Failure error;

  std::wstring Header(DWORD query, const wchar_t* name = WINHTTP_HEADER_NAME_BY_INDEX) const {
    DWORD bytes = 0;
    if (WinHttpQueryHeaders(request.get(), query, name, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX)
        || GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes > 8193 * sizeof(wchar_t)) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request.get(), query, name, value.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) return {};
    value.resize(bytes / sizeof(wchar_t));
    return value;
  }

  int64_t RetryAfter(int64_t now_utc) const {
    if (status != 403 && status != 429) return 0;
    const auto retry = Header(WINHTTP_QUERY_RETRY_AFTER);
    const bool exhausted = Header(WINHTTP_QUERY_CUSTOM, L"X-RateLimit-Remaining") == L"0";
    if (status == 403 && retry.empty() && !exhausted) return 0;
    int64_t seconds = 0;
    if (Decimal(std::wstring_view(retry), &seconds) && seconds > 0
        && now_utc <= (std::numeric_limits<int64_t>::max)() - seconds) return now_utc + seconds;
    SYSTEMTIME utc{};
    FILETIME timestamp{};
    if (!retry.empty() && WinHttpTimeToSystemTime(retry.c_str(), &utc) && SystemTimeToFileTime(&utc, &timestamp)) {
      ULARGE_INTEGER ticks{};
      ticks.LowPart = timestamp.dwLowDateTime;
      ticks.HighPart = timestamp.dwHighDateTime;
      const auto epoch = static_cast<int64_t>(ticks.QuadPart / 10'000'000) - 11'644'473'600;
      if (epoch > now_utc) return epoch;
    }
    const auto reset = Header(WINHTTP_QUERY_CUSTOM, L"X-RateLimit-Reset");
    if (Decimal(std::wstring_view(reset), &seconds) && seconds > now_utc) return seconds;
    return now_utc + 60;
  }
};

inline Response OpenResponse(const Origin& origin, std::string_view url, bool download,
                             const std::wstring& etag, const std::atomic<StopReason>& stop,
                             TransferClock::time_point deadline) {
  Response response;
  const auto fail = [&](Error code, DWORD os_code = 0) {
    response.error = {.code = code, .os_code = os_code, .http_status = response.status};
    return std::move(response);
  };
  http::Url parts;
  if (!TrustedUrl(url, origin, (download ? UrlUse::ASSET : UrlUse::API), &parts)) return fail(Error::INVALID_URL);
  if (IsStopping(stop)) return fail(Error::CANCELLED);
  DWORD os_code = 0;
  response.session = http::OpenSession(L"" NR_NEXUS_NAME L"/" NR_NEXUS_VERSION, parts.loopback, &os_code);
  if (!response.session) return fail(Error::NETWORK, os_code);
  if (!WinHttpSetTimeouts(response.session.get(), 5'000, 10'000, 15'000, 15'000)) {
    return fail(Error::NETWORK, GetLastError());
  }
  std::wstring headers;
  if (!download) {
    headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!etag.empty()) {
      headers += L"If-None-Match: " + etag + L"\r\n";
    }
  }
  for (int hops = 0;; ++hops) {
    response.status = 0;
    if (IsStopping(stop)) return fail(Error::CANCELLED);
    if (TransferClock::now() >= deadline) return fail(Error::NETWORK, ERROR_WINHTTP_TIMEOUT);
    response.connection.reset(WinHttpConnect(response.session.get(), parts.host.c_str(), parts.port, 0));
    if (!response.connection) return fail(Error::NETWORK, GetLastError());
    response.request.reset(WinHttpOpenRequest(response.connection.get(), L"GET", parts.path.c_str(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               parts.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!response.request || !http::DisableAmbientFeatures(response.request.get())
        || !WinHttpSendRequest(response.request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(response.request.get(), nullptr)
        || !http::QueryStatus(response.request.get(), &response.status)) return fail(Error::NETWORK, GetLastError());
    if (TransferClock::now() >= deadline) return fail(Error::NETWORK, ERROR_WINHTTP_TIMEOUT);
    if (response.status < 300 || response.status >= 400 || response.status == 304) return response;
    if (!download || hops == 5
        || (response.status != 301 && response.status != 302 && response.status != 303
            && response.status != 307 && response.status != 308)) return fail(Error::REDIRECT);

    auto location = response.Header(WINHTTP_QUERY_LOCATION);
    if (location.empty()) return fail(Error::REDIRECT);
    if (location.find(L"://") == location.npos || location.starts_with(L"//")) {
      std::wstring target = (parts.secure ? L"https://" : L"http://");
      if (location.starts_with(L"//")) {
        target += location.substr(2);
      } else {
        target += parts.host;
        if (!parts.secure || parts.port != INTERNET_DEFAULT_HTTPS_PORT) {
          target += L":" + std::to_wstring(parts.port);
        }
        if (!location.starts_with(L'/')) {
          const auto path = parts.path.substr(0, parts.path.find(L'?'));
          target += path.substr(0, location.starts_with(L'?') ? path.size() : path.rfind(L'/') + 1);
        }
        target += location;
      }
      location = std::move(target);
    }
    if (!http::ParseUrl(location, &parts) || !origin.Allows(parts, UrlUse::REDIRECT)) return fail(Error::REDIRECT);
    response.request.reset();
  }
}

template <typename Sink>
Failure ReadBody(const Response& response, uint64_t limit, Error limit_error, const std::atomic<StopReason>& stop,
                 TransferClock::time_point deadline, Sink sink) {
  std::array<char, 64 * 1024> chunk;
  uint64_t received = 0;
  for (;;) {
    if (IsStopping(stop)) return {.code = Error::CANCELLED};
    if (TransferClock::now() >= deadline) {
      return {.code = Error::NETWORK, .os_code = ERROR_WINHTTP_TIMEOUT, .http_status = response.status};
    }
    DWORD bytes = 0;
    if (!WinHttpReadData(response.request.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &bytes)) {
      return {.code = Error::NETWORK, .os_code = GetLastError(), .http_status = response.status};
    }
    if (IsStopping(stop)) return {.code = Error::CANCELLED};
    if (TransferClock::now() >= deadline) {
      return {.code = Error::NETWORK, .os_code = ERROR_WINHTTP_TIMEOUT, .http_status = response.status};
    }
    if (bytes == 0) return {};
    if (bytes > limit - received) return {.code = limit_error, .http_status = response.status};
    if (const auto result = sink(chunk.data(), bytes); result.code != Error::NONE) return result;
    received += bytes;
  }
}

inline bool ValidDirectoryPath(const std::filesystem::path& path) {
  if (path.empty() || !path.is_absolute()
      || std::ranges::any_of(path.native(), [](wchar_t c) { return c < L' '; })
      || path.native().size() > 32'767) return false;
  return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.native().data(),
                             static_cast<int>(path.native().size()), nullptr, 0, nullptr, nullptr) > 0;
}

struct FileTransaction {
  HANDLE file = INVALID_HANDLE_VALUE;

  FileTransaction() = default;
  FileTransaction(const FileTransaction&) = delete;
  FileTransaction& operator=(const FileTransaction&) = delete;

  ~FileTransaction() { Discard(); }

  DWORD Discard() {
    if (file == INVALID_HANDLE_VALUE) return ERROR_SUCCESS;
    FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
    const auto marked = SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
    const DWORD error = (marked ? ERROR_SUCCESS : GetLastError());
    const bool closed = CloseHandle(std::exchange(file, INVALID_HANDLE_VALUE)) != FALSE;
    if (error != ERROR_SUCCESS) return error;
    return (closed ? ERROR_SUCCESS : GetLastError());
  }
};

struct TaskResult {
  Failure error;
  std::shared_ptr<const Release> release;
  CheckState check_state = CheckState::FAILED;
  std::wstring etag;
  std::filesystem::path path;
  int64_t retry_after_utc = 0;
};

enum class TaskKind { CHECK, DOWNLOAD, CHOOSE_DIRECTORY, OPEN_DIRECTORY, OPEN_RELEASE };

class Service {
 public:
  explicit Service(std::string endpoint = LATEST_URL, Origin origin = {}, TransferTimeouts timeouts = {})
      : endpoint_(std::move(endpoint)), origin_(origin), timeouts_(timeouts) {}

  ~Service() {
    if (state_.worker != nullptr) {
      CloseHandle(state_.worker);
    }
  }

  void Configure(const Settings& settings, const std::filesystem::path& addon_directory) {
    const auto directory = ValidDirectoryPath(addon_directory) ? addon_directory : std::filesystem::path{};
    auto loaded = settings;
    if (!ValidDirectoryPath(loaded.download_directory)) {
      loaded.download_directory.clear();
    }
    const std::unique_lock lock(state_.mutex);
    state_.snapshot.addon_directory = directory;
    if (!state_.settings_dirty) {
      if (state_.snapshot.check_state == CheckState::CHECKING) {
        loaded.next_auto_check_utc = state_.snapshot.settings.next_auto_check_utc;
      }
      state_.snapshot.settings = std::move(loaded);
    }
  }

  Snapshot GetSnapshot() const {
    const std::shared_lock lock(state_.mutex);
    auto snapshot = state_.snapshot;
    // The task publishes its result before it releases its DLL reference.
    snapshot.busy = state_.worker != nullptr && WaitForSingleObject(state_.worker, 0) != WAIT_OBJECT_0;
    return snapshot;
  }

  std::optional<Settings> TakeSettingsUpdate() {
    const std::unique_lock lock(state_.mutex);
    ReapWorker();
    if (!state_.settings_dirty) return {};
    const auto settings = state_.snapshot.settings;
    state_.settings_dirty = false;
    return settings;
  }

  void SetAutoCheck(bool enabled) {
    const std::unique_lock lock(state_.mutex);
    if (state_.snapshot.settings.auto_check != enabled) {
      state_.snapshot.settings.auto_check = enabled;
      state_.settings_dirty = true;
    }
  }

  bool SetDownloadDirectory(const std::filesystem::path& path, const std::atomic_bool* cancelled = nullptr) {
    if (!path.empty() && !ValidDirectoryPath(path)) return false;
    const std::unique_lock lock(state_.mutex);
    if (cancelled != nullptr && cancelled->load()) return false;
    if (state_.snapshot.settings.download_directory != path) {
      state_.snapshot.settings.download_directory = path;
      state_.settings_dirty = true;
    }
    return true;
  }

  void Shutdown() noexcept { stop_.store(StopReason::DETACH, std::memory_order_relaxed); }

  void OnHostUnload() {
    HANDLE worker = nullptr;
    {
      const std::unique_lock lock(state_.mutex);
      ReapWorker();
      if (state_.worker == nullptr || state_.worker_kind != TaskKind::CHOOSE_DIRECTORY) return;
      state_.picker_cancel->store(true);
      // Retain this thread's identity while posting the close request. Reaping
      // the original handle or a later task must not redirect it to another picker.
      if (!DuplicateHandle(GetCurrentProcess(), state_.worker, GetCurrentProcess(), &worker,
                           0, FALSE, DUPLICATE_SAME_ACCESS)) return;
    }
    EnumThreadWindows(GetThreadId(worker), +[](HWND window, LPARAM param) -> BOOL {
      const auto thread = reinterpret_cast<HANDLE>(param);
      if (WaitForSingleObject(thread, 0) != WAIT_TIMEOUT) return FALSE;
      if (GetWindowThreadProcessId(window, nullptr) == GetThreadId(thread)) {
        PostMessageW(window, WM_CLOSE, 0, 0);
      }
      return TRUE;
    }, reinterpret_cast<LPARAM>(worker));
    CloseHandle(worker);
  }

  bool Start(TaskKind kind, int64_t now_utc = UtcNow(), bool automatic = false, bool completed = false) {
    const std::unique_lock lock(state_.mutex);
    auto& snapshot = state_.snapshot;
    ReapWorker();
    if (IsStopping(stop_)) return false;
    auto& next_auto_check = snapshot.settings.next_auto_check_utc;
    if (automatic) {
      if (state_.startup_seen) return false;
      state_.startup_seen = true;
      if (next_auto_check < 0 || (next_auto_check > now_utc && next_auto_check - now_utc > DAY)) {
        next_auto_check = 0;
        state_.settings_dirty = true;
      }
      if (!snapshot.settings.auto_check || next_auto_check > now_utc) return false;
    }
    if (state_.worker != nullptr || (kind == TaskKind::CHECK && snapshot.retry_after_utc > now_utc)) return false;
    const auto& directory = (snapshot.settings.download_directory.empty()
                                 ? snapshot.addon_directory
                                 : snapshot.settings.download_directory);
    auto task = std::make_unique<Task>(Task{.service = this,
                                           .kind = kind,
                                           .now_utc = now_utc,
                                           .release = snapshot.release});
    switch (kind) {
      case TaskKind::CHECK:
        task->etag = state_.etag;
        break;
      case TaskKind::DOWNLOAD:
        if (!snapshot.release || !snapshot.release->asset) return false;
        task->directory = directory;
        task->retry_after_utc = snapshot.retry_after_utc;
        break;
      case TaskKind::CHOOSE_DIRECTORY:
        task->directory = directory;
        task->fallback_directory = snapshot.addon_directory;
        task->picker_cancel = std::make_shared<std::atomic_bool>(false);
        break;
      case TaskKind::OPEN_DIRECTORY:
        if (completed && snapshot.completed_path.empty()) return false;
        task->directory = (completed ? snapshot.completed_path.parent_path() : directory);
        break;
      case TaskKind::OPEN_RELEASE:
        if (!snapshot.release) return false;
        break;
    }

    DWORD os_code = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&Worker), &task->module)) {
      os_code = GetLastError();
    } else {
      if (const HANDLE thread = create_thread(nullptr, 0, &Worker, task.get(), 0, nullptr)) {
        state_.picker_cancel = task->picker_cancel;
        task.release();
        state_.worker = thread;
        state_.worker_kind = kind;
      } else {
        os_code = GetLastError();
        FreeLibrary(task->module);
      }
    }
    const bool started = task == nullptr;
    const Failure error = (started ? Failure{} : Failure{.code = Error::THREAD_START, .os_code = os_code});
    switch (kind) {
      case TaskKind::CHECK:
        next_auto_check = now_utc + HOUR;
        state_.settings_dirty = true;
        snapshot.check_state = (started ? CheckState::CHECKING : CheckState::FAILED);
        snapshot.check_error = error;
        if (!started) {
          snapshot.stale = snapshot.release != nullptr;
        }
        break;
      case TaskKind::DOWNLOAD:
        snapshot.download_state = (started ? DownloadState::DOWNLOADING : DownloadState::FAILED);
        snapshot.download_error = error;
        snapshot.downloaded_bytes = 0;
        snapshot.download_size = snapshot.release->asset->size;
        snapshot.completed_path.clear();
        break;
      default:
        snapshot.action_error = error;
        break;
    }
    return started;
  }

  // Same worker entry and network path are exercised by the independent CPU suite.
  decltype(&CreateThread) create_thread = CreateThread;

 private:
  struct State {
    mutable std::shared_mutex mutex;
    Snapshot snapshot;
    std::wstring etag;
    HANDLE worker = nullptr;
    TaskKind worker_kind = TaskKind::CHECK;
    std::shared_ptr<std::atomic_bool> picker_cancel;
    bool settings_dirty = false;
    bool startup_seen = false;
  };

  struct Task {
    Service* service;
    TaskKind kind;
    int64_t now_utc;
    std::shared_ptr<const Release> release;
    std::wstring etag;
    std::filesystem::path directory;
    std::filesystem::path fallback_directory;
    std::shared_ptr<std::atomic_bool> picker_cancel;
    int64_t retry_after_utc = 0;
    HMODULE module = nullptr;
    TransferClock::time_point started_at = TransferClock::now();

    int64_t CurrentUtc() const {
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(TransferClock::now() - started_at);
      return now_utc + elapsed.count();
    }
  };

  State state_;
  const std::string endpoint_;
  const Origin origin_;
  const TransferTimeouts timeouts_;
  std::atomic<StopReason> stop_{StopReason::NONE};

  // The caller owns the state lock. Completion includes release of the worker's DLL reference.
  void ReapWorker() {
    if (state_.worker != nullptr && WaitForSingleObject(state_.worker, 0) == WAIT_OBJECT_0) {
      CloseHandle(std::exchange(state_.worker, nullptr));
      state_.picker_cancel.reset();
    }
  }

  static DWORD WINAPI Worker(void* parameter) {
    HMODULE module = nullptr;
    {
      // Wait for Start to finish publishing the task before taking its ownership.
      auto* work = static_cast<Task*>(parameter);
      auto& service = *work->service;
      {
        const std::shared_lock lock(service.state_.mutex);
      }
      const std::unique_ptr<Task> task(work);
      module = task->module;
      TaskResult result;
      try {
        if (IsStopping(service.stop_)) {
          result.error.code = Error::CANCELLED;
        } else {
          result = [&]() -> TaskResult {
            namespace dialogs = presets::dialogs;
            switch (task->kind) {
              case TaskKind::CHECK: {
                const auto deadline = TransferClock::now() + service.timeouts_.check;
                const auto response = OpenResponse(service.origin_, service.endpoint_, false, task->etag, service.stop_, deadline);
                if (response.error.code != Error::NONE) return {.error = response.error};
                if (const auto retry = response.RetryAfter(task->CurrentUtc()); retry != 0) {
                  return {.error = {.code = Error::RATE_LIMITED, .http_status = response.status}, .retry_after_utc = retry};
                }
                TaskResult result;
                switch (response.status) {
                  case 200: {
                    std::string body;
                    result.error = ReadBody(response, MAX_RELEASE_BYTES, Error::RESPONSE_TOO_LARGE, service.stop_, deadline,
                                            [&](const char* data, DWORD bytes) -> Failure {
                                              body.append(data, bytes);
                                              return {};
                                            });
                    if (result.error.code != Error::NONE) return result;
                    auto release = ParseRelease(body, service.origin_);
                    if (release) {
                      result.release = std::make_shared<const Release>(std::move(*release));
                      result.etag = response.Header(WINHTTP_QUERY_ETAG);
                      auto tag = std::wstring_view(result.etag);
                      if (tag.starts_with(L"W/")) {
                        tag.remove_prefix(2);
                      }
                      DWORD next_header = 1;
                      DWORD unused_bytes = 0;
                      // Multiple ETag fields and lists cannot identify one cached representation.
                      if (tag.size() < 2 || tag.front() != L'"' || tag.back() != L'"'
                          || !std::ranges::all_of(tag.substr(1, tag.size() - 2), [](wchar_t c) {
                               return c >= L'!' && c <= L'~' && c != L'"';
                             })
                          || WinHttpQueryHeaders(response.request.get(), WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX,
                                                 nullptr, &unused_bytes, &next_header)
                          || GetLastError() != ERROR_WINHTTP_HEADER_NOT_FOUND) {
                        result.etag.clear();
                      }
                    }
                    break;
                  }
                  case 304:
                    if (!task->etag.empty()) {
                      result.release = task->release;
                      result.etag = task->etag;
                    }
                    break;
                  case 404:
                    return {.error = {.code = Error::NO_RELEASE, .http_status = response.status}};
                  default:
                    return {.error = {.code = Error::HTTP_STATUS, .http_status = response.status}};
                }
                const auto local = ParseVersion(NR_NEXUS_VERSION);
                if (!result.release || !local) return {.error = {.code = Error::INVALID_RELEASE, .http_status = response.status}};
                result.check_state = CompareVersions(*local, *ParseVersion(result.release->version));
                return result;
              }
              case TaskKind::DOWNLOAD: {
                const auto& asset = *task->release->asset;
                const auto fail_file = [](Error error, DWORD os_code) -> TaskResult {
                  return {.error = {.code = error, .os_code = os_code}};
                };
                if (task->directory.empty()) return fail_file(Error::DIRECTORY_UNAVAILABLE, ERROR_PATH_NOT_FOUND);
                const DWORD attributes = GetFileAttributesW(task->directory.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES) return fail_file(Error::DIRECTORY_UNAVAILABLE, GetLastError());
                if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return fail_file(Error::DIRECTORY_UNAVAILABLE, ERROR_DIRECTORY);
                const auto final_path = task->directory / std::wstring(asset.name.begin(), asset.name.end());
                enum class Existing { ABSENT, MATCH, CONFLICT, UNREADABLE };
                const auto existing = [&](DWORD* os_code) {
                  WIN32_FILE_ATTRIBUTE_DATA data{};
                  if (!GetFileAttributesExW(final_path.c_str(), GetFileExInfoStandard, &data)) {
                    *os_code = GetLastError();
                    if (*os_code == ERROR_FILE_NOT_FOUND) return Existing::ABSENT;
                    return Existing::UNREADABLE;
                  }
                  if ((data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
                    return Existing::CONFLICT;
                  }
                  ULARGE_INTEGER size{};
                  size.LowPart = data.nFileSizeLow;
                  size.HighPart = data.nFileSizeHigh;
                  return size.QuadPart == asset.size ? Existing::MATCH : Existing::CONFLICT;
                };
                const auto complete = [&]() -> TaskResult {
                  const std::unique_lock lock(service.state_.mutex);
                  service.state_.snapshot.downloaded_bytes = asset.size;
                  return {.path = final_path};
                };
                DWORD os_code = 0;
                switch (existing(&os_code)) {
                  case Existing::MATCH: return complete();
                  case Existing::CONFLICT: return fail_file(Error::FILE_CONFLICT, ERROR_FILE_EXISTS);
                  case Existing::UNREADABLE: return fail_file(Error::FILE_IO, os_code);
                  case Existing::ABSENT: break;
                }

                if (task->retry_after_utc > task->now_utc) return {.error = {.code = Error::RATE_LIMITED}};
                GUID id{};
                wchar_t suffix[40];
                const HRESULT id_status = CoCreateGuid(&id);
                if (FAILED(id_status)) return fail_file(Error::FILE_IO, static_cast<DWORD>(id_status));
                if (StringFromGUID2(id, suffix, static_cast<int>(std::size(suffix))) == 0) {
                  return fail_file(Error::FILE_IO, ERROR_INSUFFICIENT_BUFFER);
                }
                const auto part_path = final_path.native() + L"." + suffix + L".nr-nexus.part";
                FileTransaction temporary;
                // Hold one exclusive handle through rename and obtain cleanup rights before writing.
                temporary.file = CreateFileW(part_path.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE,
                                             0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (temporary.file == INVALID_HANDLE_VALUE) return fail_file(Error::FILE_IO, GetLastError());

                const auto deadline = TransferClock::now() + service.timeouts_.download;
                const auto response = OpenResponse(service.origin_, asset.url, true, {}, service.stop_, deadline);
                if (response.error.code != Error::NONE) return {.error = response.error};
                if (const auto retry = response.RetryAfter(task->CurrentUtc()); retry != 0) {
                  return {.error = {.code = Error::RATE_LIMITED, .http_status = response.status}, .retry_after_utc = retry};
                }
                if (response.status != 200) return {.error = {.code = Error::HTTP_STATUS, .http_status = response.status}};
                uint64_t written = 0;
                const auto error = ReadBody(response, asset.size, Error::SIZE_MISMATCH, service.stop_, deadline,
                                            [&](const char* data, DWORD bytes) -> Failure {
                                              DWORD count = 0;
                                              if (!WriteFile(temporary.file, data, bytes, &count, nullptr)) {
                                                return {.code = Error::FILE_IO, .os_code = GetLastError()};
                                              }
                                              if (count != bytes) return {.code = Error::FILE_IO, .os_code = ERROR_WRITE_FAULT};
                                              written += count;
                                              const std::unique_lock lock(service.state_.mutex);
                                              service.state_.snapshot.downloaded_bytes = written;
                                              return {};
                                            });
                if (error.code != Error::NONE) return {.error = error};
                if (IsStopping(service.stop_)) return {.error = {.code = Error::CANCELLED}};
                LARGE_INTEGER file_size{};
                if (!GetFileSizeEx(temporary.file, &file_size)) return fail_file(Error::FILE_IO, GetLastError());
                if (static_cast<uint64_t>(file_size.QuadPart) != asset.size) {
                  return {.error = {.code = Error::SIZE_MISMATCH, .http_status = response.status}};
                }
                if (!FlushFileBuffers(temporary.file)) return fail_file(Error::FILE_IO, GetLastError());
                if (IsStopping(service.stop_)) return {.error = {.code = Error::CANCELLED}};

                const auto& destination = final_path.native();
                const auto name_bytes = destination.size() * sizeof(wchar_t);
                const auto rename_bytes = sizeof(FILE_RENAME_INFO) + name_bytes;
                const auto rename_buffer = std::make_unique<std::byte[]>(rename_bytes);
                auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(rename_buffer.get());
                rename->ReplaceIfExists = FALSE;
                rename->RootDirectory = nullptr;
                rename->FileNameLength = static_cast<DWORD>(name_bytes);
                std::memcpy(rename_buffer.get() + offsetof(FILE_RENAME_INFO, FileName),
                            destination.c_str(), name_bytes + sizeof(wchar_t));
                if (SetFileInformationByHandle(temporary.file, FileRenameInfo, rename, static_cast<DWORD>(rename_bytes))) {
                  if (!CloseHandle(std::exchange(temporary.file, INVALID_HANDLE_VALUE))) {
                    return fail_file(Error::FILE_IO, GetLastError());
                  }
                  return complete();
                }
                const DWORD move_error = GetLastError();
                switch (existing(&os_code)) {
                  case Existing::MATCH:
                    if (const DWORD cleanup_error = temporary.Discard(); cleanup_error != ERROR_SUCCESS) {
                      return fail_file(Error::FILE_IO, cleanup_error);
                    }
                    return complete();
                  case Existing::CONFLICT: return fail_file(Error::FILE_CONFLICT, move_error);
                  default: return fail_file(Error::FILE_IO, move_error);
                }
              }
              case TaskKind::CHOOSE_DIRECTORY: {
                if (task->picker_cancel->load()) return {};
                active_picker_cancel = task->picker_cancel.get();
                const auto release_hook = +[](HHOOK hook) {
                  UnhookWindowsHookEx(hook);
                  active_picker_cancel = nullptr;
                };
                // Cancellation belongs to this task, including the interval before
                // Show creates a window. The hook is limited to this pinned worker.
                std::unique_ptr<std::remove_pointer_t<HHOOK>, decltype(release_hook)> picker_hook(
                    SetWindowsHookExW(WH_CBT, +[](int code, WPARAM window, LPARAM data) -> LRESULT {
                      if (code >= 0 && active_picker_cancel != nullptr && active_picker_cancel->load()) {
                        bool close = code == HCBT_ACTIVATE;
                        if (code == HCBT_CREATEWND) {
                          const auto* created = reinterpret_cast<const CBT_CREATEWNDW*>(data);
                          close = (created->lpcs->style & (WS_CHILD | WS_VISIBLE)) == WS_VISIBLE;
                        }
                        if (close) {
                          PostMessageW(reinterpret_cast<HWND>(window), WM_CLOSE, 0, 0);
                        }
                      }
                      return CallNextHookEx(nullptr, code, window, data);
                    }, nullptr, GetCurrentThreadId()), release_hook);
                if (!picker_hook) {
                  active_picker_cancel = nullptr;
                  return {.error = {.code = Error::DIALOG, .os_code = GetLastError()}};
                }
                const auto available = [](const std::filesystem::path& path) {
                  std::error_code error;
                  return !path.empty() && std::filesystem::is_directory(path, error);
                };
                auto initial = task->directory;
                if (!available(initial)) {
                  initial = task->fallback_directory;
                  if (!available(initial)) {
                    initial.clear();
                  }
                }
                if (task->picker_cancel->load()) return {};
                auto chosen = dialogs::Choose(dialogs::Kind::FOLDER, initial);
                if (chosen.cancelled || task->picker_cancel->load()) return {};
                if (!chosen.error.empty() || chosen.path.empty()) return {.error = {.code = Error::DIALOG}};
                return {.path = std::move(chosen.path)};
              }
              case TaskKind::OPEN_DIRECTORY: {
                std::error_code error;
                if (!ValidDirectoryPath(task->directory) || !std::filesystem::is_directory(task->directory, error)) {
                  return {.error = {.code = Error::DIRECTORY_UNAVAILABLE, .os_code = static_cast<uint32_t>(error.value())}};
                }
                if (!dialogs::OpenFolder(task->directory, nullptr)) return {.error = {.code = Error::SHELL}};
                return {};
              }
              case TaskKind::OPEN_RELEASE: {
                http::Url parts;
                if (!TrustedUrl(task->release->html_url, service.origin_, UrlUse::RELEASE_PAGE, &parts)) {
                  return {.error = {.code = Error::INVALID_URL}};
                }
                const std::wstring url(task->release->html_url.begin(), task->release->html_url.end());
                dialogs::internal::Apartment apartment;
                if (FAILED(apartment.status)) {
                  return {.error = {.code = Error::SHELL, .os_code = static_cast<uint32_t>(apartment.status)}};
                }
                const auto shell_status = reinterpret_cast<INT_PTR>(
                    dialogs::internal::shell_execute(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
                if (shell_status <= 32) return {.error = {.code = Error::SHELL, .os_code = static_cast<uint32_t>(shell_status)}};
                return {};
              }
            }
            return {.error = {.code = Error::INTERNAL}};
          }();
        }
        if (task->kind == TaskKind::CHOOSE_DIRECTORY && !result.path.empty()
            && !IsStopping(service.stop_)
            && !service.SetDownloadDirectory(result.path, task->picker_cancel.get())) {
          if (!task->picker_cancel->load()) {
            result.error.code = Error::DIALOG;
          }
        }
      } catch (...) {
        result.error.code = Error::INTERNAL;
      }
      const std::unique_lock lock(service.state_.mutex);
      auto& snapshot = service.state_.snapshot;
      if (result.retry_after_utc > snapshot.retry_after_utc) {
        snapshot.retry_after_utc = result.retry_after_utc;
      }
      switch (task->kind) {
        case TaskKind::CHECK:
          snapshot.check_error = result.error;
          if (result.error.code == Error::NONE) {
            snapshot.release = std::move(result.release);
            service.state_.etag = std::move(result.etag);
            snapshot.settings.next_auto_check_utc = task->CurrentUtc() + DAY;
            service.state_.settings_dirty = true;
          }
          snapshot.check_state = result.check_state;
          snapshot.stale = result.error.code != Error::NONE && snapshot.release != nullptr;
          break;
        case TaskKind::DOWNLOAD:
          snapshot.download_error = result.error;
          snapshot.download_state = (result.error.code == Error::NONE ? DownloadState::COMPLETED : DownloadState::FAILED);
          snapshot.completed_path = std::move(result.path);
          break;
        default:
          snapshot.action_error = result.error;
          break;
      }
    }
    FreeLibraryAndExitThread(module, 0);
  }
};

inline Service service;

}  // namespace internal

inline void Configure(const Settings& settings, const std::filesystem::path& addon_directory) {
  internal::service.Configure(settings, addon_directory);
}
inline Snapshot GetSnapshot() { return internal::service.GetSnapshot(); }
inline std::optional<Settings> TakeSettingsUpdate() { return internal::service.TakeSettingsUpdate(); }
inline void SetAutoCheck(bool enabled) { internal::service.SetAutoCheck(enabled); }
inline bool SetDownloadDirectory(const std::filesystem::path& path) { return internal::service.SetDownloadDirectory(path); }
inline bool OnStartup(int64_t now_utc = UtcNow()) { return internal::service.Start(internal::TaskKind::CHECK, now_utc, true); }
inline bool StartCheck(int64_t now_utc = UtcNow()) { return internal::service.Start(internal::TaskKind::CHECK, now_utc); }
inline bool StartDownload() { return internal::service.Start(internal::TaskKind::DOWNLOAD); }
inline bool ChooseDirectory() { return internal::service.Start(internal::TaskKind::CHOOSE_DIRECTORY); }
inline bool OpenDirectory(bool completed) { return internal::service.Start(internal::TaskKind::OPEN_DIRECTORY, UtcNow(), false, completed); }
inline bool OpenRelease() { return internal::service.Start(internal::TaskKind::OPEN_RELEASE); }
inline void OnHostUnload() { internal::service.OnHostUnload(); }
inline void Shutdown() noexcept { internal::service.Shutdown(); }

}  // namespace renodx::addons::dlss5::update
