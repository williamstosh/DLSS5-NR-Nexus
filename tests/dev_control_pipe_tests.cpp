// SPDX-License-Identifier: MIT
#include <aclapi.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(NR_NEXUS_DEV_CONTROL_PIPE_TEST_DLL)
#define NR_NEXUS_DEV_CONTROL_PIPE_TEST_EXE 1
#endif
#include "dev_control_pipe.hpp"

namespace pipe = renodx::addons::dlss5::dev_control_pipe;
using Json = pipe::Json;

#if defined(NR_NEXUS_DEV_CONTROL_PIPE_TEST_DLL)

namespace {
HMODULE fixture_module = nullptr;
HANDLE fixture_started = nullptr;
HANDLE fixture_release = nullptr;
Json Execute(const Json& request) {
  if (request.value("op", "") == "slow") {
    SetEvent(fixture_started);
    WaitForSingleObject(fixture_release, 10000);
  }
  return {{"ok", true}};
}
}  // namespace

extern "C" __declspec(dllexport) void FixtureTick() { pipe::Tick(fixture_module, Execute); }
extern "C" __declspec(dllexport) void FixtureStop() { pipe::Stop(); }
extern "C" __declspec(dllexport) void FixtureConfigureBlocking(HANDLE started, HANDLE release) {
  fixture_started = started;
  fixture_release = release;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    fixture_module = module;
    DisableThreadLibraryCalls(module);
  } else if (reason == DLL_PROCESS_DETACH) {
    pipe::Stop();
  }
  return TRUE;
}

#else

namespace {

std::uint32_t checks = 0;
std::atomic<std::uint32_t> calls = 0;
std::atomic<bool> executing = false;
std::atomic<bool> release_execution = false;
Json client_settings{{"NeuralUplift", false}, {"NRHookPoint", 0}, {"NRPasses", 1}, {"NRAdaptiveResolution", false}};
Json client_receipt;
std::uint64_t client_generation = 1;

std::string ProcessCreated() {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
    throw std::runtime_error("fixture process identity unavailable");
  }
  return std::to_string((std::uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime);
}

Json ClientExecute(const Json& request) {
  Json changes = Json::object();
  if (request.value("op", "") == "set") {
    const auto key = request.at("key").get<std::string>();
    client_settings[key] = request.at("value");
    changes[key] = request.at("value");
    ++client_generation;
  }
  const Json identity{{"configuration_generation", client_generation}, {"config_epoch", client_generation}, {"settings_hash", std::to_string(client_generation)}, {"resolution_hash", "cpu-fixture"}};
  Json response{{"id", request.at("id")}, {"ok", true}, {"status", (request.value("op", "") == "set" ? "applied" : "observed")}, {"reason", "ok"}, {"identity", identity}, {"changes", std::move(changes)}, {"state", {{"pid", GetCurrentProcessId()}, {"process_created", ProcessCreated()}, {"available", true}, {"reason", "observed"}, {"settings", client_settings}, {"identity", identity}}}};
  if (request.value("op", "") == "set") {
    client_receipt = response;
  } else {
    response["receipt"] = (client_receipt.is_object() && request.contains("query_id")
                                   && request["query_id"] == client_receipt["id"]
                               ? client_receipt
                               : Json(nullptr));
  }
  return response;
}

void Check(bool passed, const char* message) {
  ++checks;
  std::printf("%s: %s (win32=%lu)\n", (passed ? "PASS" : "FAIL"), message, GetLastError());
  if (!passed) throw std::runtime_error(message);
}

Json Execute(const Json& request) {
  ++calls;
  if (request.value("op", "") == "throw") throw std::runtime_error("synthetic callback failure");
  if (request.value("op", "") == "slow") {
    executing.store(true);
    const auto deadline_ms = GetTickCount64() + 7000;
    while (!release_execution.load() && GetTickCount64() < deadline_ms) {
      Sleep(1);
    }
  }
  if (request.value("op", "") == "large") {
    return {{"ok", true}, {"value", std::string(pipe::internal::MAX_FRAME_BYTES, 'x')}};
  }
  return {{"ok", true}, {"id", "callback-id-must-be-replaced"}, {"value", request.value("value", Json(nullptr))}};
}

std::string Frame(const std::string& payload, std::uint32_t declared_size = 0) {
  const auto size = (declared_size == 0 ? static_cast<std::uint32_t>(payload.size()) : declared_size);
  std::string frame(4, '\0');
  for (std::size_t index = 0; index < 4; ++index) {
    frame[index] = static_cast<char>(size >> (index * 8));
  }
  frame += payload;
  return frame;
}

HANDLE Connect() {
  const auto name = L"\\\\.\\pipe\\nr-nexus-dev-" + std::to_wstring(GetCurrentProcessId());
  const auto deadline_ms = GetTickCount64() + 2000;
  while (GetTickCount64() < deadline_ms) {
    const auto handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL, 0, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle != INVALID_HANDLE_VALUE) return handle;
    Sleep(1);
  }
  throw std::runtime_error("client could not connect to listener");
}

Json Exchange(const std::string& frame, bool fragmented = false) {
  pipe::internal::Handle client(Connect());
  pipe::internal::Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  const auto deadline_ms = GetTickCount64() + 6500;
  auto transfer = [&](void* buffer, DWORD size, bool read) {
    DWORD offset = 0;
    while (offset < size) {
      ResetEvent(event.value);
      OVERLAPPED operation{.hEvent = event.value};
      DWORD transferred = 0;
      BOOL result = FALSE;
      if (read) {
        result = ReadFile(client.value, static_cast<std::uint8_t*>(buffer) + offset,
                          size - offset, &transferred, &operation);
      } else {
        result = WriteFile(client.value, static_cast<std::uint8_t*>(buffer) + offset,
                           size - offset, &transferred, &operation);
      }
      if (!result) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        const auto now_ms = GetTickCount64();
        if (WaitForSingleObject(event.value, static_cast<DWORD>(deadline_ms > now_ms ? (deadline_ms - now_ms) : 0))
            != WAIT_OBJECT_0) {
          CancelIoEx(client.value, &operation);
          GetOverlappedResult(client.value, &operation, &transferred, TRUE);
          return false;
        }
        if (!GetOverlappedResult(client.value, &operation, &transferred, FALSE)) return false;
      }
      if (transferred == 0) return false;
      offset += transferred;
    }
    return true;
  };
  if (fragmented) {
    for (std::size_t offset = 0; offset < frame.size();) {
      const auto size = static_cast<DWORD>((offset < 4 ? 1 : (std::min<std::size_t>)(3, frame.size() - offset)));
      if (!transfer(const_cast<char*>(frame.data()) + offset, size, false)) return nullptr;
      offset += size;
      Sleep(1);
    }
  } else if (!transfer(const_cast<char*>(frame.data()), static_cast<DWORD>(frame.size()), false)) {
    return nullptr;
  }
  std::array<std::uint8_t, 4> header{};
  if (!transfer(header.data(), static_cast<DWORD>(header.size()), true)) return nullptr;
  const auto size = static_cast<std::uint32_t>(header[0]) | (static_cast<std::uint32_t>(header[1]) << 8)
                    | (static_cast<std::uint32_t>(header[2]) << 16) | (static_cast<std::uint32_t>(header[3]) << 24);
  if (size == 0 || size > pipe::internal::MAX_FRAME_BYTES) throw std::runtime_error("invalid server frame size");
  std::string response(size, '\0');
  if (!transfer(response.data(), size, true)) return nullptr;
  return Json::parse(response);
}

Json Pump(std::future<Json>* response) {
  const auto deadline_ms = GetTickCount64() + 7000;
  while (response->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    if (GetTickCount64() >= deadline_ms) throw std::runtime_error("Present pump timed out");
    pipe::Tick(GetModuleHandleW(nullptr), Execute);
    Sleep(1);
  }
  return response->get();
}

void WaitPending() {
  const auto deadline_ms = GetTickCount64() + 2000;
  while (GetTickCount64() < deadline_ms) {
    {
      std::shared_lock lock(pipe::internal::state.mutex);
      if (pipe::internal::state.pending && pipe::internal::state.pending->phase == pipe::internal::Phase::QUEUED) return;
    }
    Sleep(1);
  }
  throw std::runtime_error("request never became queued");
}

void StopAndDrain() {
  pipe::Stop();
  HANDLE worker = nullptr;
  {
    std::shared_lock lock(pipe::internal::state.mutex);
    worker = pipe::internal::state.worker.value;
  }
  if (worker != nullptr && WaitForSingleObject(worker, 2000) != WAIT_OBJECT_0) {
    throw std::runtime_error("stopped worker did not finish");
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  try {
    const auto module = GetModuleHandleW(nullptr);
    if (argc >= 2 && std::string(argv[1]) == "--client-fixture") {
      const auto duration_ms = (argc >= 3 ? std::stoul(argv[2]) : 5000UL);
      if (duration_ms < 1000 || duration_ms > 60000) throw std::runtime_error("fixture duration must be 1000..60000 ms");
      std::printf("%s\n", Json{{"pid", GetCurrentProcessId()}, {"process_created", ProcessCreated()}, {"duration_ms", duration_ms}, {"mode", "client-fixture"}}
                              .dump()
                              .c_str());
      const auto deadline_ms = GetTickCount64() + duration_ms;
      while (GetTickCount64() < deadline_ms) {
        pipe::Tick(module, ClientExecute);
        Sleep(1);
      }
      StopAndDrain();
      return 0;
    }
    pipe::Tick(module, Execute);
    {
      pipe::internal::Handle client(Connect());
      PSECURITY_DESCRIPTOR security = nullptr;
      PACL acl = nullptr;
      PSID owner = nullptr;
      Check(GetSecurityInfo(client.value, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                            &owner, nullptr, &acl, nullptr, &security)
                == ERROR_SUCCESS,
            "real pipe exposes its owner and DACL");
      void* ace = nullptr;
      Check(acl != nullptr && acl->AceCount == 1 && GetAce(acl, 0, &ace), "pipe grants exactly one owner ACE");
      const auto* allow = static_cast<ACCESS_ALLOWED_ACE*>(ace);
      Check(allow->Header.AceType == ACCESS_ALLOWED_ACE_TYPE
                && EqualSid(owner, const_cast<DWORD*>(&allow->SidStart)),
            "the only allowed SID is the pipe owner");
      LocalFree(security);
    }
    StopAndDrain();

    const Json normal{{"id", "关联-1"}, {"op", "set"}, {"value", 123}};
    auto response = std::async(std::launch::async, Exchange, Frame(normal.dump()), true);
    auto result = Pump(&response);
    Check(result.value("ok", false) && result["id"] == normal["id"] && result["value"] == 123,
          "partial header/body framing preserves UTF-8 id and callback result");
    Check(calls.load() == 1, "one request executes once on the Present pump");

    response = std::async(std::launch::async, Exchange, Frame(normal.dump()), false);
    Check(Pump(&response) == result && calls.load() == 1, "same id and payload reuse the cached receipt");
    response = std::async(std::launch::async, Exchange,
                          Frame(Json{{"id", normal["id"]}, {"op", "set"}, {"value", 124}}.dump()), false);
    result = Pump(&response);
    Check(result.value("code", "") == "transport_id_conflict" && !result.value("ok", true) && calls.load() == 1,
          "same id with different payload is rejected without execution");

    for (const auto& frame : {Frame("not-json"), Frame(Json{{"op", "status"}}.dump()),
                              Frame("", pipe::internal::MAX_FRAME_BYTES + 1), std::string(4, '\0')}) {
      response = std::async(std::launch::async, Exchange, frame, false);
      result = Pump(&response);
      Check(!result.value("ok", true) && result.value("status", "") == "error" && calls.load() == 1,
            "malformed JSON, missing id, oversized and zero frames never execute");
    }

    Json bounded{{"id", "bounded"}, {"op", "set"}, {"padding", ""}};
    bounded["padding"] = std::string(pipe::internal::MAX_FRAME_BYTES - bounded.dump().size(), 'x');
    response = std::async(std::launch::async, Exchange, Frame(bounded.dump()), false);
    Check(bounded.dump().size() == pipe::internal::MAX_FRAME_BYTES
              && Pump(&response).value("ok", false) && calls.load() == 2,
          "an exact 65536-byte request succeeds");
    response = std::async(std::launch::async, Exchange, Frame(Json{{"id", "throw"}, {"op", "throw"}}.dump()), false);
    result = Pump(&response);
    Check(result.value("code", "") == "transport_execute_exception" && calls.load() == 3,
          "callback exceptions become correlated transport receipts");
    response = std::async(std::launch::async, Exchange, Frame(Json{{"id", "large"}, {"op", "large"}}.dump()), false);
    result = Pump(&response);
    Check(result.value("code", "") == "transport_response_too_large" && result["id"] == "large" && calls.load() == 4,
          "oversized callback responses become bounded errors");

    const Json no_tick{{"id", "no-tick"}, {"op", "set"}, {"value", 999}};
    StopAndDrain();
    pipe::Tick(module, Execute);
    response = std::async(std::launch::async, Exchange, Frame(no_tick.dump()), false);
    WaitPending();
    Check(response.wait_for(std::chrono::seconds(6)) == std::future_status::ready,
          "no-Present request returns within the worker lease");
    result = response.get();
    Check(result.value("code", "") == "transport_timeout" && result.value("status", "") == "error"
              && result["id"] == "no-tick" && calls.load() == 4,
          "queued timeout reports cancellation before execution");
    pipe::Tick(module, Execute);
    Check(calls.load() == 4, "a later Present cannot execute the expired request");
    response = std::async(std::launch::async, Exchange, Frame(no_tick.dump()), false);
    Check(Pump(&response) == result && calls.load() == 4, "retry of cancelled request reuses its timeout receipt");

    StopAndDrain();
    pipe::Tick(module, Execute);
    response = std::async(std::launch::async, Exchange,
                          Frame(Json{{"id", "stop-queued"}, {"op", "set"}}.dump()), false);
    WaitPending();
    StopAndDrain();
    Check(response.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
          "Stop cancels the live pipe and lets the worker finish");
    response.get();
    pipe::Tick(module, Execute);
    Check(calls.load() == 4, "Stop prevents an already queued command from executing after restart");

    response = std::async(std::launch::async, Exchange,
                          Frame(Json{{"id", "started-timeout"}, {"op", "slow"}, {"value", 777}}.dump()), false);
    WaitPending();
    std::thread present([&]() { pipe::Tick(module, Execute); });
    const auto execution_deadline_ms = GetTickCount64() + 1000;
    while (!executing.load() && GetTickCount64() < execution_deadline_ms) {
      Sleep(1);
    }
    const bool started = executing.load();
    const bool ready = response.wait_for(std::chrono::seconds(6)) == std::future_status::ready;
    release_execution.store(true);
    present.join();
    Check(started && ready, "a running callback can outlive the bounded worker lease");
    result = response.get();
    Check(result.value("status", "") == "outcome_unknown" && !result.value("ok", true)
              && result.value("code", "") == "transport_timeout" && calls.load() == 5,
          "started timeout reports unknown outcome instead of claiming cancellation");
    response = std::async(std::launch::async, Exchange,
                          Frame(Json{{"id", "started-timeout"}, {"op", "slow"}, {"value", 777}}.dump()), false);
    result = Pump(&response);
    Check(result.value("ok", false) && result["value"] == 777 && calls.load() == 5,
          "retry retrieves the eventual actual receipt without repeating execution");

    StopAndDrain();
    pipe::Tick(module, Execute);
    HANDLE idle_worker = nullptr;
    {
      std::shared_lock lock(pipe::internal::state.mutex);
      idle_worker = pipe::internal::state.worker.value;
    }
    Check(idle_worker != nullptr && WaitForSingleObject(idle_worker, 1800) == WAIT_OBJECT_0,
          "an idle listener exits after its one-second lease");
    StopAndDrain();

    for (const bool stop : {false, true}) {
      const auto fixture = LoadLibraryW(L".\\dev_control_pipe_fixture.dll");
      Check(fixture != nullptr, "load real transport fixture DLL");
      const auto tick = reinterpret_cast<void (*)()>(GetProcAddress(fixture, "FixtureTick"));
      const auto stop_fixture = reinterpret_cast<void (*)()>(GetProcAddress(fixture, "FixtureStop"));
      Check(tick != nullptr && stop_fixture != nullptr, "resolve DLL lifecycle fixture exports");
      tick();
      Check(GetModuleHandleW(L"dev_control_pipe_fixture.dll") == fixture, "worker owns its containing DLL");
      if (stop) {
        stop_fixture();
      }
      Check(FreeLibrary(fixture) != FALSE, "release caller DLL reference without joining the listener");
      if (!stop) {
        Check(GetModuleHandleW(L"dev_control_pipe_fixture.dll") == fixture,
              "the idle worker reference keeps the DLL alive until its lease ends");
      }
      const auto deadline_ms = GetTickCount64() + 1800;
      while (GetModuleHandleW(L"dev_control_pipe_fixture.dll") != nullptr && GetTickCount64() < deadline_ms) {
        Sleep(1);
      }
      Check(GetModuleHandleW(L"dev_control_pipe_fixture.dll") == nullptr,
            "worker releases its DLL with FreeLibraryAndExitThread at idle/Stop");
    }

    {
      const auto fixture = LoadLibraryW(L".\\dev_control_pipe_fixture.dll");
      Check(fixture != nullptr, "load DLL for callback lifetime test");
      const auto tick = reinterpret_cast<void (*)()>(GetProcAddress(fixture, "FixtureTick"));
      const auto configure = reinterpret_cast<void (*)(HANDLE, HANDLE)>(GetProcAddress(fixture, "FixtureConfigureBlocking"));
      Check(tick != nullptr && configure != nullptr, "resolve blocking callback fixture exports");
      pipe::internal::Handle dll_started(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      pipe::internal::Handle release(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      configure(dll_started.value, release.value);
      tick();
      response = std::async(std::launch::async, Exchange, Frame(Json{{"id", "dll-slow"}, {"op", "slow"}}.dump()), false);
      std::thread dll_present([&]() {
        const auto deadline_ms = GetTickCount64() + 2000;
        while (WaitForSingleObject(dll_started.value, 0) != WAIT_OBJECT_0 && GetTickCount64() < deadline_ms) {
          tick();
          Sleep(1);
        }
      });
      const bool callback_started = WaitForSingleObject(dll_started.value, 2500) == WAIT_OBJECT_0;
      if (!callback_started) {
        SetEvent(release.value);
        dll_present.join();
        FreeLibrary(fixture);
        Check(false, "blocking DLL callback entered");
      }
      const bool receipt_ready = response.wait_for(std::chrono::seconds(6)) == std::future_status::ready;
      const bool caller_released = FreeLibrary(fixture) != FALSE;
      const bool held_during_callback = GetModuleHandleW(L"dev_control_pipe_fixture.dll") == fixture;
      // Restore the host's reference before allowing the callback to return through DLL code.
      HMODULE completion_reference = nullptr;
      const bool caller_restored = GetModuleHandleExW(0, L"dev_control_pipe_fixture.dll", &completion_reference) != FALSE;
      SetEvent(release.value);
      dll_present.join();
      if (completion_reference != nullptr) {
        FreeLibrary(completion_reference);
      }
      result = response.get();
      Check(receipt_ready && result.value("status", "") == "outcome_unknown",
            "slow DLL callback returns unknown outcome at the protocol deadline");
      Check(caller_released && held_during_callback && caller_restored,
            "the worker keeps its DLL reference after timeout until the entered callback completes");
      const auto deadline_ms = GetTickCount64() + 1800;
      while (GetModuleHandleW(L"dev_control_pipe_fixture.dll") != nullptr && GetTickCount64() < deadline_ms) {
        Sleep(1);
      }
      Check(GetModuleHandleW(L"dev_control_pipe_fixture.dll") == nullptr,
            "the slow worker releases its DLL after callback completion");
    }
    std::printf("PASS: %u CPU/Win32 pipe checks; no GPU, game, UI, or deployment.\n", checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    StopAndDrain();
    return 1;
  }
}

#endif
