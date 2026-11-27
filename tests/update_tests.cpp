/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <bit>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <tuple>

#include "update.hpp"

namespace update = renodx::addons::dlss5::update;
namespace backend = update::internal;
namespace dialogs = renodx::addons::dlss5::presets::dialogs;
namespace http = renodx::addons::dlss5::http;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

#ifdef NR_UPDATE_TEST_DLL

namespace {
std::unique_ptr<backend::Service> pinned_service;
}

extern "C" __declspec(dllexport) bool StartPinnedUpdate(INTERNET_PORT port) {
  pinned_service = std::make_unique<backend::Service>(
      "http://127.0.0.1:" + std::to_string(port) + "/check/pinned", backend::Origin{.loopback_port = port});
  return pinned_service->Start(backend::TaskKind::CHECK, update::UtcNow(), true);
}

#else

namespace {

using update::CheckState;
using update::DownloadState;
using update::Error;
using backend::TaskKind;

constexpr wchar_t ARCHIVE[] = L"NR-Nexus-v65535.0.0-abcdef123456-windows-x64.zip";

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void Parity(const char* result) {
  std::cout << "NR_RELEASE_PARITY online-update " << result << std::endl;
}

template <typename Predicate>
void WaitFor(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  do {
    if (predicate()) return;
    std::this_thread::sleep_for(5ms);
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error("timed out waiting for worker");
}

struct Fixture {
  backend::Service service;

  Fixture(INTERNET_PORT port, std::string_view route, const fs::path& directory,
          backend::TransferTimeouts timeouts = {})
      : service("http://127.0.0.1:" + std::to_string(port) + std::string(route),
                {.loopback_port = port}, timeouts) {
    service.Configure({}, directory);
  }

  ~Fixture() {
    service.Shutdown();
    try {
      WaitFor([&] { return !service.GetSnapshot().busy; });
    } catch (...) {
      std::abort();
    }
  }

  update::Snapshot Done() {
    update::Snapshot snapshot;
    WaitFor([&] {
      snapshot = service.GetSnapshot();
      return !snapshot.busy;
    });
    return snapshot;
  }

  update::Snapshot Check(int64_t now = update::UtcNow()) {
    Require(service.Start(TaskKind::CHECK, now), "check dispatch");
    return Done();
  }

  void CheckAsset() {
    const auto snapshot = Check();
    Require(snapshot.check_state == CheckState::UPDATE_AVAILABLE && snapshot.release
                && snapshot.release->asset && snapshot.release->asset->id == 42,
            "loopback did not deliver the synthetic release");
  }

  update::Snapshot Download() {
    Require(service.Start(TaskKind::DOWNLOAD), "download dispatch");
    return Done();
  }
};

void Unit() {
  for (const auto* invalid : {"1.2", "01.2.3", "1.02.3", "1.2.3.4", "65536.2.3", "1.2.3-01",
                              "1.2.3-", "1.2.3+", "1.2.3-a..b", "1.2.3+a+b", "1.2.3+汉", "1.2.3\n", "v1.2.3"}) {
    Require(!backend::ParseVersion(invalid), invalid);
  }
  const auto version = [](std::string_view text) {
    const auto parsed = backend::ParseVersion(text);
    Require(parsed.has_value(), "valid SemVer rejected");
    return *parsed;
  };
  Require(backend::CompareVersions(version("1.9.0"), version("1.10.0")) == CheckState::UPDATE_AVAILABLE,
          "numeric version ordering");
  Require(backend::CompareVersions(version("1.10.0+001"), version("1.10.0")) == CheckState::UP_TO_DATE,
          "build metadata should not change precedence");
  Require(backend::CompareVersions(version("1.10.0-dev.1"), version("1.10.0")) == CheckState::UPDATE_AVAILABLE,
          "same-baseline formalization");
  Require(backend::CompareVersions(version("1.11.0-dev.1"), version("1.10.0")) == CheckState::AHEAD,
          "ahead prerelease");
  Require(version("65535.65535.65535-a01+build.003").core[2] == 65535, "PE numeric version maximum");

  using Use = backend::UrlUse;
  http::Url parts;
  Require(backend::TrustedUrl(backend::LATEST_URL, {}, Use::API, &parts), "production API");
  Require(backend::TrustedUrl("https://GitHub.com/a", {}, Use::ASSET, &parts), "production asset");
  for (const auto* cdn : {"https://release-assets.githubusercontent.com/a?sig=synthetic",
                          "https://objects.githubusercontent.com/a"}) {
    Require(backend::TrustedUrl(cdn, {}, Use::REDIRECT, &parts), "production CDN");
    Require(!backend::TrustedUrl(cdn, {}, Use::ASSET, &parts), "CDN only after redirect");
  }
  for (const auto* denied : {"http://github.com/a", "https://github.com:444/a", "https://github.com.evil/a",
                             "https://github.com%2e/a", "https://github.com./a", "https://@github.com/a",
                             "https://github.com@evil/a", "https://github.com\\@evil/a", "https://127.0.0.1/a"}) {
    Require(!backend::TrustedUrl(denied, {}, Use::ASSET, &parts), denied);
  }
  Require(!backend::TrustedUrl("https://github.com/a", {}, Use::REDIRECT, &parts), "redirect host policy");
  const backend::Origin fixture = {.loopback_port = 8999};
  Require(backend::TrustedUrl("http://127.0.0.1:8999/a", fixture, Use::ASSET, &parts), "fixture origin");
  Require(!backend::TrustedUrl("http://127.0.0.1:9000/a", fixture, Use::REDIRECT, &parts), "fixture port");
  Require(!backend::TrustedUrl("http://localhost:8999/a", fixture, Use::REDIRECT, &parts), "fixture exact host");
  Require(!backend::TrustedUrl("https://github.com/a", fixture, Use::ASSET, &parts), "fixture cannot use public network");

  using Json = nlohmann::json;
  const Json asset = {{"name", "NR-Nexus-v1.10.0+local-abcdef123456-windows-x64.zip"},
                      {"id", 42}, {"size", 10}, {"state", "uploaded"},
                      {"browser_download_url", "https://github.com/fake-asset"}};
  const Json original = {{"tag_name", "v1.10.0+local"}, {"draft", false}, {"prerelease", false}, {"body", nullptr},
                         {"published_at", "2026-11-26T10:42:50Z"},
                         {"html_url", "https://github.com/williamstosh/DLSS5-NR-Nexus/releases/tag/v1.10.0+local"},
                         {"zipball_url", "https://github.com/source.zip"}, {"assets", Json::array({asset})}};
  auto parsed = backend::ParseRelease(original.dump(), {});
  Require(parsed && parsed->asset && parsed->version == "1.10.0+local" && parsed->body.empty(), "ZIP asset shape");
  for (const auto* tag : {"vv1.2.3", "v1.2.3-dev", "v1.2.03"}) {
    auto broken = original;
    broken["tag_name"] = tag;
    Require(!backend::ParseRelease(broken.dump(), {}), "invalid tag");
  }
  for (const auto* flag : {"draft", "prerelease"}) {
    auto broken = original;
    broken[flag] = true;
    Require(!backend::ParseRelease(broken.dump(), {}), "unpublished release");
  }
  for (const auto& assets : {Json::array(), Json::array({asset, asset}), Json::array({{{"name", "source.zip"}}})}) {
    auto broken = original;
    broken["assets"] = assets;
    parsed = backend::ParseRelease(broken.dump(), {});
    Require(parsed && !parsed->asset, "missing/ambiguous/source ZIP should retain release information");
  }
  for (const auto* name : {"../NR-Nexus-v1.10.0+local-abcdef123456-windows-x64.zip",
                           "NR-Nexus-v1.10.0+local-abcdef12:stream-windows-x64.zip",
                           "NR-Nexus-v1.10.0+local-abcdef123456-windows-x64.zip.sha256",
                           "NR-Nexus-v1.10.0+local-abcdef1-windows-x64.zip"}) {
    auto broken = original;
    broken["assets"][0]["name"] = name;
    parsed = backend::ParseRelease(broken.dump(), {});
    Require(parsed && !parsed->asset, "invalid filename accepted");
  }
  for (const auto* field : {"id", "size"}) {
    auto broken = original;
    broken["assets"][0][field] = 0;
    parsed = backend::ParseRelease(broken.dump(), {});
    Require(parsed && !parsed->asset, "zero asset id/size accepted");
  }
  Parity("semver_release_asset_url=passed");

  const auto cwd = fs::current_path();
  backend::Service service;
  service.Configure({}, cwd);
  for (const auto& path : {fs::path(L"relative"), cwd / L"a\r\nb", cwd / L"a\tb",
                            cwd / std::wstring(L"a\0b", 3), cwd / std::wstring(1, L'\xD800')}) {
    Require(!service.SetDownloadDirectory(path), "malformed runtime directory accepted");
  }
  const auto preferred = cwd / L"中文, 空格";
  Require(service.SetDownloadDirectory(preferred), "Unicode directory rejected");
  service.Configure({}, cwd);
  const auto dirty = service.TakeSettingsUpdate();
  Require(dirty && dirty->download_directory == preferred && !service.TakeSettingsUpdate(), "directory receipt/reload");
  Require(service.SetDownloadDirectory(LR"(\\server\share\中文, 空格)"), "UNC directory rejected");
  Require(service.SetDownloadDirectory({}), "default directory rejected");
  service.SetAutoCheck(false);
  Require(!service.Start(TaskKind::CHECK, 100, true), "auto check disabled");
  service.SetAutoCheck(true);
  Require(!service.Start(TaskKind::CHECK, 101, true), "must only opt into automatic check once");
  service.create_thread = +[](LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD) -> HANDLE {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
  };
  Require(!service.Start(TaskKind::CHECK, 102), "fake thread started");
  const auto failed = service.GetSnapshot();
  Require(failed.check_state == CheckState::FAILED && failed.check_error.code == Error::THREAD_START
              && failed.check_error.os_code == ERROR_NOT_ENOUGH_MEMORY
              && failed.settings.next_auto_check_utc == 102 + backend::HOUR && !failed.busy,
          "thread startup failure and backoff");
  Parity("directory_startup_thread_failure=passed");
}

void Loopback(INTERNET_PORT port) {
  const auto base = fs::current_path() / L"download-fixtures";
  const auto dir = [&](const wchar_t* name) {
    auto path = base / name;
    fs::create_directories(path);
    return path;
  };
  const auto no_parts = [](const fs::path& directory) {
    for (const auto& entry : fs::directory_iterator(directory)) {
      Require(!entry.path().native().ends_with(L".nr-nexus.part"), "leftover part");
    }
  };
  const auto downloaded = [&](const update::Snapshot& snapshot, const fs::path& directory) {
    Require(snapshot.download_state == DownloadState::COMPLETED && snapshot.completed_path == directory / ARCHIVE,
            "download completion path");
    Require(snapshot.downloaded_bytes == snapshot.download_size && snapshot.download_size > 192 * 1024
                && fs::file_size(snapshot.completed_path) == snapshot.download_size,
            "download completion byte count");
    std::ifstream zip(snapshot.completed_path, std::ios::binary);
    char header[4]{};
    zip.read(header, sizeof(header));
    Require(std::string_view(header, sizeof(header)) == "PK\x03\x04", "fixture ZIP was not written");
    no_parts(directory);
  };

  const auto actual = dir(L"更新, 原目录");
  Fixture fixture(port, "/check/reuse", actual);
  const auto now = update::UtcNow();
  Require(fixture.service.Start(TaskKind::CHECK, now, true), "startup dispatch");
  Require(!fixture.service.Start(TaskKind::CHECK, now, true), "duplicate startup");
  const auto first = fixture.Done();
  Require(first.release && first.release->body.starts_with("## 标题") && first.release->asset
              && first.settings.next_auto_check_utc == now + backend::DAY,
          "successful check, release body and 24 hour backoff");
  Require(fixture.service.TakeSettingsUpdate().has_value(), "persist successful check");
  auto reloaded = first.settings;
  reloaded.auto_check = false;
  fixture.service.Configure(reloaded, actual);
  Require(fixture.Check().release == first.release, "304 paired with same immutable release");
  Fixture weak(port, "/check/etag-weak", actual);
  const auto weak_release = weak.Check().release;
  Require(weak_release && weak.Check().release == weak_release, "weak ETag did not support a conditional 304");
  for (const auto* route : {"/check/etag-wildcard", "/check/etag-list", "/check/etag-duplicate"}) {
    Fixture invalid_validator(port, route, actual);
    const auto original = invalid_validator.Check().release;
    const auto changed = invalid_validator.Check().release;
    Require(original && changed && changed->body != original->body, "non-single ETag hid a changed release");
    if (std::string_view(route).ends_with("wildcard")) {
      const auto latest = invalid_validator.Check().release;
      Require(latest && latest->body != changed->body, "wildcard retained after overwriting a valid validator");
    }
  }
  downloaded(fixture.Download(), actual);
  downloaded(fixture.Download(), actual);
  const auto default_path = actual.parent_path() / L"alternate";
  Require(fixture.service.SetDownloadDirectory(default_path), "change directory without moving prior downloads");
  Require(fixture.service.GetSnapshot().completed_path.parent_path() == actual, "completed path remains bound to download");
  Parity("startup_manual_get_etag_redirect_reuse=passed");

  const auto unrelated = dir(L"unrelated");
  Fixture throttled(port, "/check/should-not-reach", unrelated);
  throttled.service.Configure({.next_auto_check_utc = now + backend::DAY}, unrelated);
  Require(!throttled.service.Start(TaskKind::CHECK, now, true), "startup cooldown");
  Require(throttled.service.GetSnapshot().check_state == CheckState::IDLE && !throttled.service.GetSnapshot().release,
          "throttled startup cannot claim up-to-date");
  Fixture clock_adjusted(port, "/check/stale", unrelated);
  clock_adjusted.service.Configure({.next_auto_check_utc = now + backend::DAY + 1}, unrelated);
  Require(clock_adjusted.service.Start(TaskKind::CHECK, now, true), "implausible cooldown after clock adjustment");
  const auto cached = clock_adjusted.Done().release;
  Require(cached != nullptr, "cache not populated");
  const auto stale = clock_adjusted.Check(now);
  Require(stale.check_state == CheckState::FAILED && stale.check_error.http_status == 503 && stale.stale
              && stale.release == cached && stale.settings.next_auto_check_utc == now + backend::HOUR,
          "failed recheck must preserve stale release and set attempt backoff");
  for (const auto& [route, code, status] :
       {std::tuple{"/check/missing", Error::NO_RELEASE, 404u}, {"/check/invalid", Error::INVALID_RELEASE, 200u},
        {"/check/huge", Error::RESPONSE_TOO_LARGE, 200u}, {"/check/unpaired", Error::INVALID_RELEASE, 304u},
        {"/check/unrelated403", Error::HTTP_STATUS, 403u}, {"/check/api-redirect", Error::REDIRECT, 302u},
        {"/check/retry429", Error::RATE_LIMITED, 429u}, {"/check/reset403", Error::RATE_LIMITED, 403u}}) {
    Fixture check(port, route, unrelated);
    const auto response = check.Check();
    Require(response.check_state == CheckState::FAILED && response.check_error.code == code
                && response.check_error.http_status == status && !response.release && !response.stale, route);
    if (code == Error::RATE_LIMITED) {
      Require(response.retry_after_utc >= now + 89 && !check.service.Start(TaskKind::CHECK), "manual rate-limit gate");
    }
  }
  Parity("stale_boot_throttle_http_metadata_limits=passed");

  for (const auto& [route, code] : {std::pair{"/check/outside-host", Error::REDIRECT},
                                   {"/check/outside-port", Error::REDIRECT}, {"/check/redirect-loop", Error::REDIRECT},
                                   {"/check/short", Error::SIZE_MISMATCH}, {"/check/long", Error::SIZE_MISMATCH}}) {
    Fixture failure(port, route, unrelated);
    failure.CheckAsset();
    const auto response = failure.Download();
    Require(response.download_state == DownloadState::FAILED && response.download_error.code == code
                && response.completed_path.empty() && !fs::exists(unrelated / ARCHIVE), route);
    no_parts(unrelated);
  }
  Fixture retry(port, "/check/recover", dir(L"recover"));
  retry.CheckAsset();
  const auto disconnected = retry.Download();
  Require(disconnected.download_state == DownloadState::FAILED && disconnected.completed_path.empty()
              && (disconnected.download_error.code == Error::NETWORK || disconnected.download_error.code == Error::SIZE_MISMATCH),
          "incomplete body published");
  no_parts(base / L"recover");
  downloaded(retry.Download(), base / L"recover");
  Parity("redirect_policy_sizes_disconnect_retry=passed");

  Fixture missing(port, "/check/reuse", base / L"missing");
  missing.CheckAsset();
  Require(missing.Download().download_error.code == Error::DIRECTORY_UNAVAILABLE, "missing output directory");
  Fixture denied(port, "/check/denied-directory", base / L"denied");
  denied.CheckAsset();
  const auto unwritable = denied.Download();
  Require(unwritable.download_error.code == Error::FILE_IO && unwritable.download_error.os_code == ERROR_ACCESS_DENIED,
          "write protection evidence");
  no_parts(base / L"denied");
  const auto denied_delete = base / L"denied-delete";
  std::ofstream(denied_delete / L"write-probe", std::ios::binary) << "ok";
  Require(fs::file_size(denied_delete / L"write-probe") == 2, "DELETE denial fixture must still allow writes");
  Require(!DeleteFileW((denied_delete / L"write-probe").c_str()) && GetLastError() == ERROR_ACCESS_DENIED,
          "DELETE denial fixture must deny removal by pathname");
  Fixture no_delete(port, "/check/denied-delete", denied_delete);
  no_delete.CheckAsset();
  const auto cannot_clean = no_delete.Download();
  Require(cannot_clean.download_error.code == Error::SIZE_MISMATCH
              || (cannot_clean.download_error.code == Error::FILE_IO && cannot_clean.download_error.os_code == ERROR_ACCESS_DENIED),
          "interrupted transfer in DELETE-restricted directory");
  no_parts(denied_delete);
  const auto existing = dir(L"existing");
  std::ofstream(existing / ARCHIVE, std::ios::binary) << "old";
  Fixture wrong_size(port, "/check/existing-conflict", existing);
  wrong_size.CheckAsset();
  Require(wrong_size.Download().download_error.code == Error::FILE_CONFLICT && fs::file_size(existing / ARCHIVE) == 3,
          "existing conflict overwritten");
  no_parts(existing);
  Parity("directory_unavailable_denied_existing_conflict=passed");

  const auto race_path = dir(L"race");
  Fixture left(port, "/check/race", race_path), right(port, "/check/race", race_path);
  left.CheckAsset();
  right.CheckAsset();
  Require(left.service.Start(TaskKind::DOWNLOAD) && right.service.Start(TaskKind::DOWNLOAD), "concurrent workers");
  const auto left_result = left.Done();
  const auto right_result = right.Done();
  downloaded(left_result, race_path);
  downloaded(right_result, race_path);
  Require(std::distance(fs::directory_iterator(race_path), fs::directory_iterator{}) == 1, "race leaked extra files");

  const auto conflict = dir(L"race-conflict");
  Fixture racer(port, "/check/conflict-race", conflict);
  racer.CheckAsset();
  Require(racer.service.Start(TaskKind::DOWNLOAD), "race conflict dispatch");
  WaitFor([&] { return racer.service.GetSnapshot().downloaded_bytes > 0; });
  std::ofstream(conflict / ARCHIVE, std::ios::binary) << "racer";
  Require(racer.Done().download_error.code == Error::FILE_CONFLICT && fs::file_size(conflict / ARCHIVE) == 5,
          "concurrent different-length file overwritten");
  no_parts(conflict);
  Parity("same_and_different_size_rename_race=passed");

  const auto slow_path = dir(L"slow");
  Fixture slow(port, "/check/slow", slow_path);
  slow.CheckAsset();
  Require(slow.service.Start(TaskKind::DOWNLOAD), "slow download dispatch");
  WaitFor([&] { return slow.service.GetSnapshot().downloaded_bytes > 0; });
  for (const auto& entry : fs::directory_iterator(slow_path)) {
    if (!entry.path().native().ends_with(L".nr-nexus.part")) continue;
    Require(!MoveFileExW(entry.path().c_str(), (slow_path / L"foreign").c_str(), 0)
                && (GetLastError() == ERROR_SHARING_VIOLATION || GetLastError() == ERROR_ACCESS_DENIED),
            "could swap a temporary file held by the worker");
  }
  Require(slow.service.GetSnapshot().busy && !slow.service.Start(TaskKind::CHECK)
              && !slow.service.Start(TaskKind::CHOOSE_DIRECTORY),
          "worker only accepts one in-flight task");
  const auto other = dir(L"other");
  Require(slow.service.SetDownloadDirectory(other), "update directory during transfer");
  slow.service.Configure({}, slow_path);
  downloaded(slow.Done(), slow_path);
  Require(slow.service.GetSnapshot().settings.download_directory == other, "reload discarded dirty path");

  const auto scratch = dir(L"cancel-timeout");
  Fixture stopped(port, "/check/slow", scratch);
  stopped.CheckAsset();
  Require(stopped.service.Start(TaskKind::DOWNLOAD), "cancel download dispatch");
  WaitFor([&] { return stopped.service.GetSnapshot().downloaded_bytes > 0; });
  stopped.service.Shutdown();
  Require(stopped.Done().download_error.code == Error::CANCELLED && !fs::exists(scratch / ARCHIVE),
          "shutdown did not cancel download");
  Require(!stopped.service.Start(TaskKind::CHECK, now, true) && !stopped.service.Start(TaskKind::CHECK),
          "terminal detach stop was reopened by a runtime callback");
  no_parts(scratch);
  Fixture bounded(port, "/check/slow", scratch, {.download = 400ms});
  bounded.CheckAsset();
  const auto limited = bounded.Download();
  Require(limited.download_error.code == Error::NETWORK && limited.download_error.os_code == ERROR_WINHTTP_TIMEOUT
              && limited.downloaded_bytes > 0 && !fs::exists(scratch / ARCHIVE), "overall transfer deadline");
  no_parts(scratch);
  Parity("progress_frozen_directory_cancel_deadline=passed");

  struct DialogHooks {
    decltype(dialogs::internal::show_dialog) show = dialogs::internal::show_dialog;
    decltype(dialogs::internal::shell_execute) shell = dialogs::internal::shell_execute;
    ~DialogHooks() {
      dialogs::internal::show_dialog = show;
      dialogs::internal::shell_execute = shell;
    }
  } hooks;
  static fs::path shell_path;
  static std::wstring shell_verb;
  static std::thread::id action_thread;
  dialogs::internal::show_dialog = +[](IFileDialog* dialog, HWND) -> HRESULT {
    Microsoft::WRL::ComPtr<IShellItem> folder;
    dialogs::internal::ShellPath folder_path;
    if (FAILED(dialog->GetFolder(folder.GetAddressOf()))
        || FAILED(folder->GetDisplayName(SIGDN_FILESYSPATH, &folder_path.value))) return E_FAIL;
    shell_path = folder_path.value;
    action_thread = std::this_thread::get_id();
    return HRESULT_FROM_WIN32(ERROR_CANCELLED);
  };
  dialogs::internal::shell_execute = +[](HWND, LPCWSTR verb, LPCWSTR target, LPCWSTR, LPCWSTR, INT) -> HINSTANCE {
    shell_path = target;
    shell_verb = verb;
    action_thread = std::this_thread::get_id();
    return reinterpret_cast<HINSTANCE>(33);
  };
  const auto caller = std::this_thread::get_id();
  Require(fixture.service.TakeSettingsUpdate().has_value(), "directory update receipt before cancellation");
  Require(fixture.service.Start(TaskKind::CHOOSE_DIRECTORY), "directory chooser");
  Require(fixture.Done().action_error.code == Error::NONE && fs::equivalent(shell_path, actual)
              && action_thread != caller && !fixture.service.TakeSettingsUpdate(),
          "folder chooser falls back and cancel leaves directory preference");
  Require(fixture.service.Start(TaskKind::OPEN_DIRECTORY, now, false, true), "open completed directory");
  Require(fixture.Done().action_error.code == Error::NONE && shell_path == actual && shell_verb == L"explore"
              && action_thread != caller, "Shell uses the completed download directory on its worker");
  Require(fixture.service.Start(TaskKind::OPEN_RELEASE), "open release page");
  Require(fixture.Done().action_error.code == Error::NONE && shell_path.native().ends_with(L"/release")
              && shell_verb == L"open" && action_thread != caller, "Shell uses the validated release URL");
  static std::atomic_bool picker_ready = false;
  static std::atomic_bool picker_closed = false;
  dialogs::internal::show_dialog = +[](IFileDialog*, HWND) -> HRESULT {
    const HWND popup = CreateWindowExW(0, L"STATIC", L"Update modal fixture", WS_OVERLAPPED,
                                      0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);
    if (popup == nullptr) return E_FAIL;
    picker_ready = true;
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      if (message.hwnd == popup && message.message == WM_CLOSE) {
        picker_closed = true;
        break;
      }
      DispatchMessageW(&message);
    }
    DestroyWindow(popup);
    return HRESULT_FROM_WIN32(ERROR_CANCELLED);
  };
  Require(fixture.service.Start(TaskKind::CHOOSE_DIRECTORY), "open modal fixture");
  WaitFor([] { return picker_ready.load(); });
  fixture.service.OnHostUnload();
  WaitFor([&] { return !fixture.service.GetSnapshot().busy; });
  Require(picker_closed && !fixture.service.TakeSettingsUpdate(),
          "host unload must request closing only the active picker");
  static std::atomic_bool picker_show_entered = false;
  static std::atomic_bool picker_show_release = false;
  picker_ready = false;
  picker_closed = false;
  dialogs::internal::show_dialog = +[](IFileDialog*, HWND) -> HRESULT {
    picker_show_entered = true;
    WaitFor([] { return picker_show_release.load(); });
    const HWND popup = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Late update picker",
                                      WS_OVERLAPPED | WS_VISIBLE, -32000, -32000, 1, 1,
                                      nullptr, nullptr, nullptr, nullptr);
    if (popup == nullptr) return E_FAIL;
    picker_ready = true;
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      if (message.hwnd == popup && message.message == WM_CLOSE) {
        picker_closed = true;
        break;
      }
      DispatchMessageW(&message);
    }
    DestroyWindow(popup);
    return HRESULT_FROM_WIN32(ERROR_CANCELLED);
  };
  Require(fixture.service.Start(TaskKind::CHOOSE_DIRECTORY), "start picker before its window exists");
  WaitFor([] { return picker_show_entered.load(); });
  fixture.service.OnHostUnload();
  Require(fixture.service.GetSnapshot().busy && !picker_ready, "unload waited for the gated picker");
  picker_show_release = true;
  WaitFor([&] { return !fixture.service.GetSnapshot().busy; });
  Require(picker_ready && picker_closed && !fixture.service.TakeSettingsUpdate()
              && fixture.service.GetSnapshot().action_error.code == Error::NONE,
          "picker cancellation was lost before window creation");
  picker_show_entered = false;
  dialogs::internal::show_dialog = +[](IFileDialog*, HWND) -> HRESULT {
    picker_show_entered = true;
    return HRESULT_FROM_WIN32(ERROR_CANCELLED);
  };
  Require(fixture.service.Start(TaskKind::CHOOSE_DIRECTORY), "new picker after canceled generation");
  Require(fixture.Done().action_error.code == Error::NONE && picker_show_entered,
          "old picker cancellation affected a new task");
  Parity("chooser_fallback_cancel_background_shell=passed");

  const auto module = LoadLibraryW(L"update_worker_fixture.dll");
  Require(module != nullptr, "could not load pinning fixture DLL");
  using StartPinned = bool (*)(INTERNET_PORT);
  const auto entry = GetProcAddress(module, "StartPinnedUpdate");
  const auto start = std::bit_cast<StartPinned>(entry);
  Require(start != nullptr && start(port), "could not start pinning fixture DLL");
  Require(FreeLibrary(module) != FALSE, "could not release external DLL reference");
  Require(GetModuleHandleW(L"update_worker_fixture.dll") != nullptr, "running worker did not pin DLL");
  WaitFor([] { return GetModuleHandleW(L"update_worker_fixture.dll") == nullptr; });
  Parity("module_pin_and_release=passed");
}

void Public(const fs::path& directory) {
  Require(fs::is_directory(directory), "public smoke directory must already exist");
  update::Configure({.auto_check = false}, directory);
  Require(update::SetDownloadDirectory(directory), "public smoke directory must be absolute");
  const auto wait = [] {
    WaitFor([] { return !update::GetSnapshot().busy; });
    return update::GetSnapshot();
  };
  Require(update::StartCheck(), "public check dispatch");
  const auto first = wait();
  if (!first.release) {
    std::cerr << "check error=" << static_cast<int>(first.check_error.code) << " OS=" << first.check_error.os_code
              << " HTTP=" << first.check_error.http_status << '\n';
  }
  Require(first.release && first.release->asset, "public latest release or unique ZIP missing");
  std::cout << "public metadata status=200 tag=" << first.release->tag << " asset_id=" << first.release->asset->id
            << '\n';
  Require(update::StartCheck(), "public conditional check dispatch");
  Require(wait().release == first.release && !update::GetSnapshot().stale, "public ETag 304 not observed");
  std::cout << "public conditional status=304\n";
  Require(update::StartDownload(), "public download dispatch");
  const auto download = wait();
  if (download.download_state != DownloadState::COMPLETED) {
    std::cerr << "download error=" << static_cast<int>(download.download_error.code) << " OS=" << download.download_error.os_code
              << " HTTP=" << download.download_error.http_status << '\n';
  }
  Require(download.download_state == DownloadState::COMPLETED, "public download did not complete");
  Require(update::StartDownload() && wait().download_state == DownloadState::COMPLETED, "public cached ZIP did not complete");
  std::cout << "public completed and reused name=" << first.release->asset->name << " bytes=" << download.downloaded_bytes << '\n';
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
#ifdef NR_NEXUS_DEV_CONTROL
    std::cout << "NR_UPDATE_TEST_BUILD DevControl=On\n";
#else
    std::cout << "NR_UPDATE_TEST_BUILD DevControl=Off\n";
#endif
    if (argc == 3 && std::wstring_view(argv[1]) == L"--public") {
      Public(argv[2]);
    } else if (argc == 3 && std::wstring_view(argv[1]) == L"--loopback") {
      unsigned short port = 0;
      Require(backend::Decimal(std::wstring_view(argv[2]), &port) && port != 0, "invalid fixture port");
      Loopback(port);
    } else {
      Require(argc == 2 && std::wstring_view(argv[1]) == L"--unit", "expected --unit, --loopback or explicit --public");
      Unit();
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "update test failure: " << error.what() << '\n';
    return 1;
  }
}

#endif
