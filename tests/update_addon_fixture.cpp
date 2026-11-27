/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <new>
#include <string>
#include <utility>

#include "../addon.cpp"

namespace {

namespace nr = renodx::addons::dlss5;
namespace update = nr::update;

HANDLE download_thread = nullptr;
bool configured = false;

HANDLE WINAPI CaptureDownloadThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stack_size,
    LPTHREAD_START_ROUTINE entry, LPVOID parameter, DWORD flags, LPDWORD thread_id) {
  const auto thread = CreateThread(attributes, stack_size, entry, parameter, flags, thread_id);
  if (thread != nullptr) {
    const auto process = GetCurrentProcess();
    DuplicateHandle(process, thread, process, &download_thread, SYNCHRONIZE, FALSE, 0);
  }
  return thread;
}

}  // namespace

extern "C" __declspec(dllexport) bool FixtureConfigure(uint16_t port, const char* route, const wchar_t* directory) {
  if (configured || port == 0 || update::GetSnapshot().busy) return false;
  std::string endpoint = "http://127.0.0.1:" + std::to_string(port) + "/check/" + route;
  auto& service = update::internal::service;
  // The first host runtime callback starts this service. Its endpoint and origin
  // are immutable for the rest of the mapped image's lifetime.
  service.~Service();
  new (&service) update::internal::Service(std::move(endpoint), {.loopback_port = port});
  update::Configure(update::Settings{}, std::filesystem::path(directory));
  configured = true;
  return true;
}

extern "C" __declspec(dllexport) HANDLE FixtureStartDownload() {
  auto& service = update::internal::service;
  service.create_thread = CaptureDownloadThread;
  const bool started = update::StartDownload();
  service.create_thread = CreateThread;
  return (started ? std::exchange(download_thread, nullptr) : nullptr);
}

extern "C" __declspec(dllexport) void FixtureRequestHud() {
  nr::internal::hud_enabled.store(true);
  nr::internal::NotifyNrOperation(true);
}

extern "C" __declspec(dllexport) const char* FixtureReadTransfer(uint64_t* received, uint64_t* expected) {
  const auto snapshot = update::GetSnapshot();
  *received = snapshot.downloaded_bytes;
  *expected = snapshot.download_size;
  switch (snapshot.download_state) {
    case update::DownloadState::DOWNLOADING: return (snapshot.busy ? "downloading" : "failed");
    case update::DownloadState::COMPLETED:
      if (snapshot.busy) return "finishing";
      return (snapshot.completed_path.empty() ? "failed" : "completed");
    case update::DownloadState::FAILED: return "failed";
    case update::DownloadState::IDLE:
      if (snapshot.busy) return "checking";
      if (snapshot.check_state == update::CheckState::UPDATE_AVAILABLE) return "available";
      if (snapshot.check_state == update::CheckState::FAILED) return "failed";
      return "idle";
  }
  return "failed";
}

extern "C" __declspec(dllexport) size_t FixtureReadCallbacks(uint64_t* presents) {
  *presents = nr::internal::presents_observed.load();
  return nr::internal::live_effect_runtimes.size();
}
