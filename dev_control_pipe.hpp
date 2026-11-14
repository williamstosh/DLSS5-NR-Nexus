// SPDX-License-Identifier: MIT
#pragma once

#if defined(NR_NEXUS_DEV_CONTROL)

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace renodx::addons::dlss5::dev_control_pipe {

using Json = nlohmann::json;
using Execute = Json (*)(const Json&);

namespace internal {

constexpr std::uint32_t MAX_FRAME_BYTES = 64 * 1024;
constexpr ULONGLONG IDLE_LEASE_MS = 1000;
constexpr ULONGLONG WORKER_LEASE_MS = 5000;
constexpr ULONGLONG RESPONSE_RESERVE_MS = 250;

struct Handle {
  HANDLE value = nullptr;

  explicit Handle(HANDLE value = nullptr) : value(value) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() {
    if (value != nullptr && value != INVALID_HANDLE_VALUE) {
      CloseHandle(value);
    }
  }
};

enum class Phase { QUEUED,
                   EXECUTING,
                   COMPLETE,
                   CANCELLED };

struct Request {
  Json payload;
  Json response;
  std::string id;
  ULONGLONG deadline_ms = 0;
  std::uint64_t cancellation_epoch = 0;
  Phase phase = Phase::QUEUED;
  Handle completed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
};

struct State {
  std::shared_mutex mutex;
  Handle worker;
  std::shared_ptr<Request> pending;
  std::shared_ptr<Request> last;
  std::atomic<std::uint64_t> cancellation_epoch = 0;
  std::atomic<HANDLE> cancelled = nullptr;

  ~State() {
    if (const auto event = cancelled.load()) {
      CloseHandle(event);
    }
  }
};

static State state;

inline Json Error(const Json& id, const char* code, const char* message, bool execution_started = false) {
  return {{"id", id}, {"ok", false}, {"status", (execution_started ? "outcome_unknown" : "error")}, {"code", code}, {"reason", code}, {"message", message}};
}

struct WorkerContext {
  HMODULE module_reference = nullptr;
  std::uint64_t cancellation_epoch = 0;
};

struct ThreadModuleReference {
  HMODULE value = nullptr;

  ~ThreadModuleReference() {
    if (value != nullptr) {
      FreeLibraryAndExitThread(value, 0);
    }
  }
};

static DWORD WINAPI Listen(void* argument) {
  const auto* context = static_cast<WorkerContext*>(argument);
  // Destroy this last: FreeLibraryAndExitThread does not unwind the worker's C++ objects.
  ThreadModuleReference module_reference{.value = context->module_reference};
  const auto cancellation_epoch = context->cancellation_epoch;
  delete context;
  {
    std::shared_ptr<Request> request;
    try {
      const auto start_ms = GetTickCount64();
      const auto lease_deadline_ms = start_ms + WORKER_LEASE_MS;
      const auto request_deadline_ms = lease_deadline_ms - RESPONSE_RESERVE_MS;
      const auto cancelled = state.cancelled.load();
      Handle token;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return 0;
      DWORD token_bytes = 0;
      GetTokenInformation(token.value, TokenUser, nullptr, 0, &token_bytes);
      if (token_bytes == 0) return 0;
      std::vector<std::uint8_t> token_data(token_bytes);
      if (!GetTokenInformation(token.value, TokenUser, token_data.data(), token_bytes, &token_bytes)) return 0;
      const auto sid = reinterpret_cast<TOKEN_USER*>(token_data.data())->User.Sid;
      const auto acl_bytes = static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD))
                             + GetLengthSid(sid);
      std::vector<std::uint8_t> acl_data(acl_bytes);
      auto* acl = reinterpret_cast<ACL*>(acl_data.data());
      SECURITY_DESCRIPTOR security{};
      if (!InitializeAcl(acl, acl_bytes, ACL_REVISION)
          || !AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, sid)
          || !InitializeSecurityDescriptor(&security, SECURITY_DESCRIPTOR_REVISION)
          || !SetSecurityDescriptorOwner(&security, sid, FALSE)
          || !SetSecurityDescriptorDacl(&security, TRUE, acl, FALSE)) return 0;
      SECURITY_ATTRIBUTES attributes{.nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = &security, .bInheritHandle = FALSE};
      const auto name = L"\\\\.\\pipe\\nr-nexus-dev-" + std::to_wstring(GetCurrentProcessId());
      Handle pipe(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   1, MAX_FRAME_BYTES + 4, MAX_FRAME_BYTES + 4, 1000, &attributes));
      if (pipe.value == INVALID_HANDLE_VALUE) return 0;
      Handle io_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      if (io_event.value == nullptr) return 0;

      auto wait_io = [&](OVERLAPPED* operation, DWORD* transferred, ULONGLONG deadline_ms) {
        const auto now_ms = GetTickCount64();
        const auto remaining_ms = static_cast<DWORD>(deadline_ms > now_ms ? (deadline_ms - now_ms) : 0);
        const std::array<HANDLE, 2> events{cancelled, operation->hEvent};
        const auto result = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, remaining_ms);
        if (result == WAIT_OBJECT_0 + 1 && cancellation_epoch == state.cancellation_epoch.load()) {
          return GetOverlappedResult(pipe.value, operation, transferred, FALSE) != FALSE;
        }
        // The OVERLAPPED and buffer must remain alive until cancellation completes.
        CancelIoEx(pipe.value, operation);
        GetOverlappedResult(pipe.value, operation, transferred, TRUE);
        return false;
      };
      auto transfer = [&](void* bytes, DWORD count, bool read, ULONGLONG deadline_ms) {
        DWORD offset = 0;
        while (offset < count) {
          if (GetTickCount64() >= deadline_ms || cancellation_epoch != state.cancellation_epoch.load()) return false;
          ResetEvent(io_event.value);
          OVERLAPPED operation{.hEvent = io_event.value};
          DWORD transferred = 0;
          BOOL result = FALSE;
          if (read) {
            result = ReadFile(pipe.value, static_cast<std::uint8_t*>(bytes) + offset,
                              count - offset, &transferred, &operation);
          } else {
            result = WriteFile(pipe.value, static_cast<std::uint8_t*>(bytes) + offset,
                               count - offset, &transferred, &operation);
          }
          if (!result && (GetLastError() != ERROR_IO_PENDING || !wait_io(&operation, &transferred, deadline_ms))) return false;
          if (transferred == 0) return false;
          offset += transferred;
        }
        return true;
      };

      OVERLAPPED connect{.hEvent = io_event.value};
      if (!ConnectNamedPipe(pipe.value, &connect)) {
        const auto error = GetLastError();
        DWORD transferred = 0;
        if (error != ERROR_PIPE_CONNECTED
            && (error != ERROR_IO_PENDING || !wait_io(&connect, &transferred, start_ms + IDLE_LEASE_MS))) return 0;
      }
      std::array<std::uint8_t, 4> size_bytes{};
      if (!transfer(size_bytes.data(), static_cast<DWORD>(size_bytes.size()), true, request_deadline_ms)) return 0;
      const std::uint32_t size = static_cast<std::uint32_t>(size_bytes[0])
                                 | (static_cast<std::uint32_t>(size_bytes[1]) << 8)
                                 | (static_cast<std::uint32_t>(size_bytes[2]) << 16)
                                 | (static_cast<std::uint32_t>(size_bytes[3]) << 24);
      Json response;
      if (size == 0 || size > MAX_FRAME_BYTES) {
        response = Error(nullptr, "transport_invalid_frame", "Frame length must be between 1 and 65536 bytes.");
      } else {
        std::string bytes(size, '\0');
        if (!transfer(bytes.data(), size, true, request_deadline_ms)) return 0;
        auto payload = Json::parse(bytes, nullptr, false);
        if (!payload.is_object() || !payload.contains("id") || !payload["id"].is_string()
            || payload["id"].get_ref<const std::string&>().empty()) {
          response = Error(nullptr, "transport_invalid_request", "A JSON object with a nonempty string id is required.");
        } else {
          const auto id = payload["id"].get<std::string>();
          {
            std::unique_lock lock(state.mutex);
            if (state.last && state.last->id == id) {
              response = (state.last->payload == payload
                              ? state.last->response
                              : Error(id, "transport_id_conflict", "The id was already used for a different request."));
            } else if (state.pending) {
              if (state.pending->id == id && state.pending->payload == payload) {
                request = state.pending;
              } else {
                response = Error(id, "transport_busy", "Another request is still executing.");
              }
            } else if (cancellation_epoch != state.cancellation_epoch.load()) {
              response = Error(id, "transport_stopped", "The control listener was stopped.");
            } else {
              request = std::make_shared<Request>();
              if (request->completed.value == nullptr) return 0;
              request->payload = std::move(payload);
              request->id = id;
              request->deadline_ms = request_deadline_ms;
              request->cancellation_epoch = cancellation_epoch;
              state.pending = request;
            }
          }
          if (request) {
            const auto now_ms = GetTickCount64();
            const std::array<HANDLE, 2> events{cancelled, request->completed.value};
            WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE,
                                   static_cast<DWORD>(request_deadline_ms > now_ms ? (request_deadline_ms - now_ms) : 0));
            std::unique_lock lock(state.mutex);
            if (request->phase == Phase::COMPLETE || request->phase == Phase::CANCELLED) {
              response = request->response;
            } else {
              const bool stopped = cancellation_epoch != state.cancellation_epoch.load();
              const bool execution_started = request->phase == Phase::EXECUTING;
              response = Error(id, (stopped ? "transport_stopped" : "transport_timeout"),
                               (execution_started ? "Execution started; its receipt was not available before the deadline."
                                                  : "The request was cancelled before execution."),
                               execution_started);
              if (!execution_started) {
                request->response = response;
                request->phase = Phase::CANCELLED;
                state.last = request;
                if (state.pending == request) {
                  state.pending.reset();
                }
              }
            }
          }
        }
      }
      auto response_bytes = response.dump();
      if (response_bytes.size() > MAX_FRAME_BYTES) {
        response_bytes = Error(response.value("id", Json(nullptr)), "transport_response_too_large",
                               "The response exceeded 65536 bytes.")
                             .dump();
        if (response_bytes.size() > MAX_FRAME_BYTES) return 0;
      }
      const auto response_size = static_cast<std::uint32_t>(response_bytes.size());
      for (std::size_t index = 0; index < size_bytes.size(); ++index) {
        size_bytes[index] = static_cast<std::uint8_t>(response_size >> (index * 8));
      }
      if (transfer(size_bytes.data(), static_cast<DWORD>(size_bytes.size()), false, lease_deadline_ms)) {
        transfer(response_bytes.data(), response_size, false, lease_deadline_ms);
      }
    } catch (...) {
      // A failed worker may never leave a queued command for a later Present.
      if (request) {
        std::unique_lock lock(state.mutex);
        if (request->phase == Phase::QUEUED) {
          request->response = Error(request->id, "transport_aborted", "The control worker failed before execution.");
          request->phase = Phase::CANCELLED;
          state.last = request;
          if (state.pending == request) {
            state.pending.reset();
          }
        }
      }
    }
    if (request) {
      bool execution_started = false;
      {
        std::shared_lock lock(state.mutex);
        execution_started = request->phase == Phase::EXECUTING;
      }
      // The protocol deadline ends pipe I/O, but an entered callback still needs its DLL.
      // The host must also keep its normal reference until the Present callback returns.
      if (execution_started) {
        WaitForSingleObject(request->completed.value, INFINITE);
      }
    }
  }
  return 0;
}

}  // namespace internal

// Stop is safe from DLL detach: no join, mutex acquisition, allocation, or file I/O.
inline void Stop() {
  internal::state.cancellation_epoch.fetch_add(1);
  if (const auto event = internal::state.cancelled.load()) {
    SetEvent(event);
  }
}

// Call only at the selected Present boundary. Parsing, serialization, and pipe I/O stay on the worker.
inline void Tick(HMODULE module, Execute execute) {
  if (module == nullptr || execute == nullptr) return;
  std::shared_ptr<internal::Request> request;
  {
    std::unique_lock lock(internal::state.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (internal::state.pending && internal::state.pending->phase == internal::Phase::QUEUED) {
      request = internal::state.pending;
      const bool stopped = request->cancellation_epoch != internal::state.cancellation_epoch.load();
      if (stopped || GetTickCount64() >= request->deadline_ms) {
        request->response = internal::Error(request->id, (stopped ? "transport_stopped" : "transport_timeout"),
                                            "The request was cancelled before execution.");
        request->phase = internal::Phase::CANCELLED;
        internal::state.last = request;
        internal::state.pending.reset();
        SetEvent(request->completed.value);
        request.reset();
      } else {
        request->phase = internal::Phase::EXECUTING;
      }
    }
    if (internal::state.worker.value != nullptr
        && WaitForSingleObject(internal::state.worker.value, 0) == WAIT_OBJECT_0) {
      CloseHandle(internal::state.worker.value);
      internal::state.worker.value = nullptr;
    }
    if (internal::state.worker.value == nullptr) {
      if (internal::state.cancelled.load() == nullptr) {
        internal::state.cancelled.store(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (internal::state.cancelled.load() == nullptr) return;
      }
      const auto cancellation_epoch = internal::state.cancellation_epoch.load();
      ResetEvent(internal::state.cancelled.load());
      HMODULE module_reference = nullptr;
#if defined(NR_NEXUS_DEV_CONTROL_PIPE_TEST_EXE)
      const bool executable_test = module == GetModuleHandleW(nullptr);
#else
      constexpr bool executable_test = false;
#endif
      if (!executable_test
          && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(module),
                                 &module_reference)) return;
      auto context = std::make_unique<internal::WorkerContext>(internal::WorkerContext{
          .module_reference = module_reference, .cancellation_epoch = cancellation_epoch});
      internal::state.worker.value = CreateThread(nullptr, 0, internal::Listen, context.get(), 0, nullptr);
      if (internal::state.worker.value != nullptr) {
        context.release();
      } else if (module_reference != nullptr) {
        FreeLibrary(module_reference);
      }
    }
  }
  if (!request) return;
  Json response;
  try {
    response = execute(request->payload);
    if (!response.is_object()) {
      response = {{"ok", true}, {"result", std::move(response)}};
    }
    response["id"] = request->id;
  } catch (...) {
    response = internal::Error(request->id, "transport_execute_exception", "The command callback threw an exception.");
  }
  {
    std::unique_lock lock(internal::state.mutex);
    request->response = std::move(response);
    request->phase = internal::Phase::COMPLETE;
    internal::state.last = request;
    if (internal::state.pending == request) {
      internal::state.pending.reset();
    }
    SetEvent(request->completed.value);
  }
}

}  // namespace renodx::addons::dlss5::dev_control_pipe

#endif  // NR_NEXUS_DEV_CONTROL
