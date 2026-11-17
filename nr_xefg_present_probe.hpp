/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <shared_mutex>
#include <span>

#include "../../utils/directx.hpp"
#include "detour_guard.hpp"
#include "fg_frame.hpp"

namespace renodx::addons::dlss5::xefg_present_probe {
using Microsoft::WRL::ComPtr;
inline constexpr const char* kEvidence = "metadata_not_pixels_or_completion";
inline constexpr size_t kMaxBuffers = 8;
enum class Kind : uint8_t { INIT_BEGIN, INIT_RETURN, CREATE_RETURN, PRESENT_BEFORE, PRESENT_RETURN, RESIZE_BEFORE, RESIZE_RETURN, INVALIDATED };
struct InitKey {
  uint32_t slot = UINT32_MAX;
  uintptr_t module = 0, handle = 0, factory = 0, window = 0, application_swapchain = 0;
};
struct QueueIdentity {
  uintptr_t queue = 0, device = 0;
};
struct Candidate {
  uintptr_t swapchain = 0, queue = 0, device = 0, method_module = 0;
  uint32_t buffer_count = 0;
  bool hooks_ready = false;
};
struct PendingInit {
  InitKey key;
  uint64_t serial = 0;
  uintptr_t context = 0;
  uint64_t context_epoch = 0, context_generation = 0;
  Candidate candidate;
  std::array<uintptr_t, kMaxBuffers> queues{};
  uint32_t candidates = 0, creates_inflight = 0, owner_thread = 0;
  bool active = false, ambiguous = false, factory_hooks_ready = false, resizing = false;
};
struct Event {
  Kind kind = Kind::INIT_BEGIN;
  uint64_t sequence = 0, init_serial = 0, buffer_generation = 0, present_serial = 0;
  uintptr_t swapchain = 0, queue = 0;
  uint32_t thread_id = 0, method = 0, index = UINT32_MAX, flags = 0;
  int32_t result = 0;
  const char* reason = "unobserved";
};
struct Binding {
  InitKey key;
  uint64_t init_serial = 0;
  uintptr_t context = 0;
  uint64_t context_epoch = 0, context_generation = 0;
  Candidate created;
  std::array<uintptr_t, kMaxBuffers> queues{};
  uint32_t buffer_count = 0;
  uint64_t buffer_generation = 0, present_calls = 0, successful_present_calls = 0, test_calls = 0;
  uint64_t last_present_serial = 0;
  uint32_t last_present_index = UINT32_MAX;
  int32_t last_present_result = 0;
  bool live = false, bound = false, resizing = false;
  const char* reason = "unobserved";
};
struct Snapshot {
  bool stopped = false;
  uint64_t sequence = 0, next_init = 0, next_present = 0;
  uint64_t rejected_creates = 0, stale_returns = 0, overflow = 0;
  PendingInit pending;
  std::array<Binding, 4> bindings{};
  std::array<Event, 24> events{};
};
struct ProbeState {
  std::shared_mutex mutex;
  Snapshot snapshot;
};
static ProbeState probe_state;
// SDK owners serialize Init/Destroy/Stop here. Worker callbacks only take the
// short ledger lock, including when Init is waiting for that worker.
static std::recursive_mutex lifecycle_mutex;
static std::shared_mutex install_mutex;

template <typename Callback>
inline void Observe(Callback&& callback) noexcept {
  try {
    callback();
  } catch (...) {
    // Metadata failure never prevents the original SDK/DXGI call.
  }
}
// Caller holds probe_state.mutex. No COM references are owned by this ledger.
inline void RecordLocked(Event event) {
  event.sequence = ++probe_state.snapshot.sequence;
  event.thread_id = GetCurrentThreadId();
  probe_state.snapshot.events[(event.sequence - 1) % probe_state.snapshot.events.size()] = event;
}
inline uint64_t BeginInit(const InitKey& key) {
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  if (snapshot.stopped) return 0;
  if (snapshot.pending.active) {
    snapshot.pending.ambiguous = true;
    return 0;
  }
  bool shared_factory = false;
  for (auto& binding : snapshot.bindings) {
    if (!binding.live) continue;
    if (binding.key.slot == key.slot && binding.key.handle == key.handle) {
      binding.live = binding.bound = false;
      binding.reason = "context_reinitializing";
    } else if (key.factory != 0 && binding.key.factory == key.factory) {
      shared_factory = true;
    }
  }
  snapshot.pending = {.key = key, .serial = ++snapshot.next_init, .owner_thread = GetCurrentThreadId(),
      .active = true, .ambiguous = shared_factory};
  RecordLocked({.kind = Kind::INIT_BEGIN, .init_serial = snapshot.pending.serial,
      .reason = (key.factory == 0 ? "existing_swapchain_creation_queue_unobserved" : "initializing")});
  return snapshot.pending.serial;
}
inline void ContextInitialized(uint64_t serial, uintptr_t context, uint64_t epoch, uint64_t generation) {
  std::unique_lock lock(probe_state.mutex);
  auto& pending = probe_state.snapshot.pending;
  if (!pending.active || pending.serial != serial || probe_state.snapshot.stopped) return;
  pending.context = context;
  pending.context_epoch = epoch;
  pending.context_generation = generation;
}
inline uint64_t BeginCreate(uintptr_t factory, uintptr_t window) {
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  auto& pending = snapshot.pending;
  if (snapshot.stopped || !pending.active) return 0;
  if (pending.key.factory == 0 || factory != pending.key.factory || window != pending.key.window) {
    ++snapshot.rejected_creates;
    return 0;
  }
  ++pending.creates_inflight;
  return pending.serial;
}
inline void FinishCreate(uint64_t serial, int32_t result, const Candidate& candidate, uint32_t method) {
  if (serial == 0) return;
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  auto& pending = snapshot.pending;
  if (snapshot.stopped || !pending.active || pending.serial != serial) {
    ++snapshot.stale_returns;
    return;
  }
  if (pending.creates_inflight != 0) {
    --pending.creates_inflight;
  }
  const bool valid = SUCCEEDED(result) && candidate.swapchain != 0 && candidate.queue != 0
      && candidate.device != 0 && candidate.buffer_count != 0 && candidate.buffer_count <= kMaxBuffers
      && candidate.hooks_ready;
  if (valid) {
    if (pending.candidates == 0) {
      pending.candidate = candidate;
      pending.queues.fill(candidate.queue);
      pending.candidates = 1;
    } else if (pending.candidate.swapchain != candidate.swapchain || pending.candidate.queue != candidate.queue) {
      pending.ambiguous = true;
      ++pending.candidates;
    }
  } else if (SUCCEEDED(result)) {
    pending.ambiguous = true;
  }
  RecordLocked({.kind = Kind::CREATE_RETURN, .init_serial = serial, .swapchain = candidate.swapchain,
      .queue = candidate.queue, .method = method, .result = result,
      .reason = (valid ? "native_create_candidate" : "create_failed_or_native_contract_unavailable")});
}
inline void FinishInit(uint64_t serial, int32_t result) {
  if (serial == 0) return;
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  if (!snapshot.pending.active || snapshot.pending.serial != serial) return;
  const auto pending = snapshot.pending;
  snapshot.pending.active = false;
  Binding* selected = nullptr;
  for (auto& binding : snapshot.bindings) {
    if (!binding.live) {
      selected = &binding;
      break;
    }
  }
  const bool identity_valid = pending.key.slot < 2 && pending.key.module != 0 && pending.key.handle != 0
      && pending.key.factory != 0 && pending.key.window != 0;
  const bool bound = result == 0 && !snapshot.stopped && identity_valid
      && pending.factory_hooks_ready && !pending.ambiguous && !pending.resizing && pending.creates_inflight == 0
      && pending.candidates == 1 && pending.context != 0 && pending.context_generation != 0;
  const char* reason = "init_failed";
  if (snapshot.stopped) {
    reason = "stopped";
  } else if (result == 0) {
    if (pending.key.factory == 0) {
      reason = "existing_swapchain_creation_queue_unobserved";
    } else if (!identity_valid) {
      reason = "invalid_init_identity";
    } else if (!pending.factory_hooks_ready) {
      reason = "factory_hooks_unavailable";
    } else if (pending.ambiguous || pending.resizing || pending.creates_inflight != 0 || pending.candidates > 1) {
      reason = "init_creation_ambiguous";
    } else if (pending.candidates == 0) {
      reason = "native_create_unobserved";
    } else if (pending.context == 0 || pending.context_generation == 0) {
      reason = "context_registration_unobserved";
    } else {
      reason = "native_create_bound";
    }
  }
  if (selected == nullptr) {
    ++snapshot.overflow;
    reason = "context_capacity_reached";
  } else {
    *selected = {.key = pending.key, .init_serial = serial, .context = pending.context,
        .context_epoch = pending.context_epoch, .context_generation = pending.context_generation,
        .created = pending.candidate, .queues = pending.queues, .buffer_count = pending.candidate.buffer_count,
        .buffer_generation = 1, .live = result == 0 && !snapshot.stopped, .bound = bound, .reason = reason};
    if (result == 0 && pending.candidate.swapchain != 0) {
      for (auto& other : snapshot.bindings) {
        if (&other != selected && other.live && other.created.swapchain == selected->created.swapchain) {
          other.bound = selected->bound = false;
          other.reason = selected->reason = "native_swapchain_multiple_contexts";
        }
      }
    }
  }
  RecordLocked({.kind = Kind::INIT_RETURN, .init_serial = serial, .swapchain = pending.candidate.swapchain,
      .queue = pending.candidate.queue, .result = result, .reason = reason});
}
inline void UpdateContext(uintptr_t context, uint64_t epoch, uint64_t generation) {
  std::unique_lock lock(probe_state.mutex);
  for (auto& binding : probe_state.snapshot.bindings) {
    if (binding.live && binding.context == context) {
      binding.context_epoch = epoch;
      binding.context_generation = generation;
      ++binding.buffer_generation;
    }
  }
}
inline void Invalidate(uint32_t slot, uintptr_t handle, const char* reason) {
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  if (snapshot.pending.active && snapshot.pending.key.slot == slot && snapshot.pending.key.handle == handle) {
    snapshot.pending.ambiguous = true;
  }
  for (auto& binding : snapshot.bindings) {
    if (binding.live && binding.key.slot == slot && binding.key.handle == handle) {
      binding.live = binding.bound = false;
      binding.reason = reason;
      RecordLocked({.kind = Kind::INVALIDATED, .init_serial = binding.init_serial, .swapchain = binding.created.swapchain, .reason = reason});
    }
  }
}
struct PresentTicket {
  uint64_t init_serial = 0, buffer_generation = 0, serial = 0;
  uintptr_t swapchain = 0, queue = 0;
  uint32_t index = UINT32_MAX, method = 0, flags = 0;
  bool record_event = false;
};
inline PresentTicket BeginPresent(uintptr_t swapchain, uint32_t index, uint32_t method, uint32_t flags) {
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  if (snapshot.stopped) return {};
  for (auto& binding : snapshot.bindings) {
    if (!binding.live || !binding.bound || binding.resizing || binding.created.swapchain != swapchain) continue;
    if ((flags & DXGI_PRESENT_TEST) != 0) {
      ++binding.test_calls;
      return {};
    }
    ++binding.present_calls;
    PresentTicket ticket{.init_serial = binding.init_serial, .buffer_generation = binding.buffer_generation,
        .serial = ++snapshot.next_present, .swapchain = swapchain,
        .queue = (index < binding.buffer_count ? binding.queues[index] : 0), .index = index, .method = method,
        .flags = flags, .record_event = binding.present_calls <= 8};
    if (ticket.record_event) {
      RecordLocked({.kind = Kind::PRESENT_BEFORE, .init_serial = ticket.init_serial,
          .buffer_generation = ticket.buffer_generation, .present_serial = ticket.serial, .swapchain = swapchain,
          .queue = ticket.queue, .method = method, .index = index, .flags = flags,
          .reason = (ticket.queue == 0 ? "present_buffer_index_unavailable" : "native_api_call_before")});
    }
    return ticket;
  }
  return {};
}
inline void FinishPresent(const PresentTicket& ticket, int32_t result) {
  if (ticket.serial == 0) return;
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  for (auto& binding : snapshot.bindings) {
    if (!snapshot.stopped && binding.live && binding.bound && binding.init_serial == ticket.init_serial
        && binding.buffer_generation == ticket.buffer_generation && binding.created.swapchain == ticket.swapchain) {
      binding.last_present_serial = ticket.serial;
      binding.last_present_index = ticket.index;
      binding.last_present_result = result;
      binding.successful_present_calls += SUCCEEDED(result);
      if (ticket.record_event) {
        RecordLocked({.kind = Kind::PRESENT_RETURN, .init_serial = ticket.init_serial,
            .buffer_generation = ticket.buffer_generation, .present_serial = ticket.serial, .swapchain = ticket.swapchain,
            .queue = ticket.queue, .method = ticket.method, .index = ticket.index, .flags = ticket.flags,
            .result = result, .reason = "native_api_call_returned"});
      }
      return;
    }
  }
  ++snapshot.stale_returns;
}
struct ResizeTicket {
  uint64_t init_serial = 0, buffer_generation = 0;
  uintptr_t swapchain = 0;
  bool was_bound = false, pending = false;
};
inline ResizeTicket BeginResize(uintptr_t swapchain) {
  std::unique_lock lock(probe_state.mutex);
  if (probe_state.snapshot.stopped) return {};
  auto& pending = probe_state.snapshot.pending;
  if (pending.active && pending.candidates == 1 && pending.candidate.swapchain == swapchain) {
    if (pending.resizing) {
      pending.ambiguous = true;
      return {};
    }
    pending.resizing = true;
    RecordLocked({.kind = Kind::RESIZE_BEFORE, .init_serial = pending.serial,
        .swapchain = swapchain, .reason = "native_resizing_during_init"});
    return {.init_serial = pending.serial, .swapchain = swapchain, .pending = true};
  }
  for (auto& binding : probe_state.snapshot.bindings) {
    if (!binding.live || binding.created.swapchain != swapchain) continue;
    const ResizeTicket ticket{.init_serial = binding.init_serial, .buffer_generation = ++binding.buffer_generation,
        .swapchain = swapchain, .was_bound = binding.bound};
    if (binding.resizing) {
      binding.bound = false;
      binding.reason = "overlapping_native_resize";
      return {};
    }
    binding.resizing = true;
    binding.bound = false;
    binding.reason = "native_resizing";
    RecordLocked({.kind = Kind::RESIZE_BEFORE, .init_serial = ticket.init_serial,
        .buffer_generation = ticket.buffer_generation, .swapchain = swapchain, .reason = "native_resizing"});
    return ticket;
  }
  return {};
}
inline void FinishResize(const ResizeTicket& ticket, int32_t result, uint32_t buffer_count,
                         const std::array<QueueIdentity, kMaxBuffers>& queues, bool queues_supplied) {
  if (ticket.init_serial == 0) return;
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  const auto apply_queues = [&](std::array<uintptr_t, kMaxBuffers>* destination, uint32_t previous_count, uintptr_t device) {
    if (buffer_count == 0 || buffer_count > kMaxBuffers || previous_count == 0 || previous_count > kMaxBuffers) return false;
    if (queues_supplied) {
      if (std::any_of(queues.begin(), queues.begin() + buffer_count,
          [&](const QueueIdentity& queue) { return queue.queue == 0 || queue.device != device; })) return false;
      for (uint32_t index = 0; index < buffer_count; ++index) {
        (*destination)[index] = queues[index].queue;
      }
    } else if (previous_count != buffer_count) {
      if (!std::all_of(destination->begin(), destination->begin() + previous_count,
          [&](uintptr_t queue) { return queue != 0 && queue == (*destination)[0]; })) return false;
      destination->fill((*destination)[0]);
    }
    return true;
  };
  if (ticket.pending) {
    auto& pending = snapshot.pending;
    if (snapshot.stopped || !pending.active || pending.serial != ticket.init_serial
        || pending.candidate.swapchain != ticket.swapchain) {
      ++snapshot.stale_returns;
      return;
    }
    pending.resizing = false;
    const char* reason = "resize_failed_queue_request_not_committed";
    if (SUCCEEDED(result)) {
      if (apply_queues(&pending.queues, pending.candidate.buffer_count, pending.candidate.device)) {
        pending.candidate.buffer_count = buffer_count;
        reason = "native_resize_during_init_observed";
      } else {
        pending.ambiguous = true;
        reason = "resize_queue_contract_unavailable";
      }
    }
    RecordLocked({.kind = Kind::RESIZE_RETURN, .init_serial = ticket.init_serial, .swapchain = ticket.swapchain,
        .queue = pending.queues[0], .result = result, .reason = reason});
    return;
  }
  for (auto& binding : snapshot.bindings) {
    if (snapshot.stopped || !binding.live || binding.init_serial != ticket.init_serial
        || binding.buffer_generation != ticket.buffer_generation) continue;
    binding.resizing = false;
    if (FAILED(result)) {
      binding.bound = ticket.was_bound;
      binding.reason = "resize_failed_queue_request_not_committed";
    } else if (!apply_queues(&binding.queues, binding.buffer_count, binding.created.device)) {
      binding.reason = "resize_queue_contract_unavailable";
    } else {
      binding.buffer_count = buffer_count;
      binding.bound = ticket.was_bound;
      binding.reason = (binding.bound ? "native_resize_bound" : "resize_cannot_create_context_binding");
    }
    RecordLocked({.kind = Kind::RESIZE_RETURN, .init_serial = ticket.init_serial,
        .buffer_generation = ticket.buffer_generation, .swapchain = ticket.swapchain,
        .queue = binding.queues[0], .result = result, .reason = binding.reason});
    return;
  }
  ++snapshot.stale_returns;
}
inline Snapshot CopySnapshot() {
  std::shared_lock lock(probe_state.mutex);
  return probe_state.snapshot;
}
inline void StopObservation() {
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  snapshot.stopped = true;
  snapshot.pending.active = false;
  for (auto& binding : snapshot.bindings) {
    binding.live = binding.bound = false;
    binding.reason = "stopped";
  }
}

struct MethodSlot {
  std::array<void*, 4> real{};
  std::array<detour_guard::Record, 4> records{};
  bool installed = false, complete = false;
};
static std::array<MethodSlot, 4> outputs, factories;
inline bool AttachPinned(MethodSlot* slot, const std::array<void*, 4>& targets,
                         const std::array<void*, 4>& detours, size_t count) {
  if (!fg::PinRuntimeCallback()) return false;
  for (size_t index = 0; index < count; ++index) {
    HMODULE pinned = nullptr;
    if (targets[index] == nullptr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(targets[index]), &pinned)) return false;
  }
  slot->real = targets;
  std::array<detour_guard::Hook, 4> hooks{};
  for (size_t index = 0; index < count; ++index) {
    hooks[index] = {.name = "XeFG native present metadata probe", .real = &slot->real[index],
        .detour = detours[index], .record = &slot->records[index]};
  }
  size_t failed = 0;
  const auto attached = detour_guard::AttachAll(std::span(hooks).first(count), &failed);
  slot->installed = attached != 0;
  slot->complete = attached == count;
  return slot->complete;
}
inline QueueIdentity ReadQueue(IUnknown* object) {
  ComPtr<ID3D12CommandQueue> queue;
  if (object == nullptr || FAILED(object->QueryInterface(IID_PPV_ARGS(&queue)))) return {};
  ID3D12CommandQueue* native = queue.Get();
  renodx::utils::directx::NativeFromReShadeProxy(&native);
  ComPtr<ID3D12Device> device;
  if (native->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT || FAILED(native->GetDevice(IID_PPV_ARGS(&device)))) return {};
  return {.queue = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(native)),
      .device = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(device.Get()))};
}
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Resize1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
struct ResizeScope {
  inline static thread_local ResizeScope* current = nullptr;
  ResizeScope* previous = current;
  uintptr_t identity = 0;
  ResizeTicket ticket;
  explicit ResizeScope(IDXGISwapChain* swapchain) noexcept {
    Observe([&] {
      identity = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(swapchain));
      if (previous == nullptr || previous->identity != identity) {
        ticket = BeginResize(identity);
      }
    });
    current = this;
  }
  ~ResizeScope() { current = previous; }
};
template <size_t Index>
struct OutputHooks {
  static HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* swapchain, UINT interval, UINT flags) {
    PresentTicket ticket;
    Observe([&] {
      ComPtr<IDXGISwapChain3> swapchain3;
      const auto index = (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) ? swapchain3->GetCurrentBackBufferIndex() : UINT32_MAX);
      ticket = BeginPresent(reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(swapchain)), index, 1, flags);
    });
    const auto result = reinterpret_cast<PresentFn>(outputs[Index].real[0])(swapchain, interval, flags);
    Observe([&] { FinishPresent(ticket, result); });
    return result;
  }
  static HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1* swapchain, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) {
    PresentTicket ticket;
    Observe([&] {
      ComPtr<IDXGISwapChain3> swapchain3;
      const auto index = (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) ? swapchain3->GetCurrentBackBufferIndex() : UINT32_MAX);
      ticket = BeginPresent(reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(swapchain)), index, 2, flags);
    });
    const auto result = reinterpret_cast<Present1Fn>(outputs[Index].real[1])(swapchain, interval, flags, parameters);
    Observe([&] { FinishPresent(ticket, result); });
    return result;
  }
  static HRESULT STDMETHODCALLTYPE Resize(IDXGISwapChain* swapchain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    const ResizeScope scope(swapchain);
    const auto result = reinterpret_cast<ResizeFn>(outputs[Index].real[2])(swapchain, count, width, height, format, flags);
    Observe([&] {
      DXGI_SWAP_CHAIN_DESC desc{};
      const auto actual_count = (SUCCEEDED(result) && SUCCEEDED(swapchain->GetDesc(&desc)) ? desc.BufferCount : 0);
      FinishResize(scope.ticket, result, actual_count, {}, false);
    });
    return result;
  }
  static HRESULT STDMETHODCALLTYPE Resize1(IDXGISwapChain3* swapchain, UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                          UINT flags, const UINT* masks, IUnknown* const* queues) {
    const ResizeScope scope(swapchain);
    std::array<QueueIdentity, kMaxBuffers> requested_queues{};
    Observe([&] {
      if (scope.ticket.init_serial == 0 || queues == nullptr) return;
      DXGI_SWAP_CHAIN_DESC desc{};
      const auto requested_count = (count != 0 ? count : (SUCCEEDED(swapchain->GetDesc(&desc)) ? desc.BufferCount : 0));
      if (requested_count <= kMaxBuffers) {
        for (uint32_t index = 0; index < requested_count; ++index) {
          requested_queues[index] = ReadQueue(queues[index]);
        }
      }
    });
    const auto result = reinterpret_cast<Resize1Fn>(outputs[Index].real[3])(swapchain, count, width, height, format, flags, masks, queues);
    Observe([&] {
      DXGI_SWAP_CHAIN_DESC desc{};
      const auto actual_count = (SUCCEEDED(result) && SUCCEEDED(swapchain->GetDesc(&desc)) ? desc.BufferCount : 0);
      FinishResize(scope.ticket, result, actual_count, requested_queues, queues != nullptr);
    });
    return result;
  }
};
template <size_t Index>
inline std::array<void*, 4> OutputDetours() {
  return {reinterpret_cast<void*>(&OutputHooks<Index>::Present), reinterpret_cast<void*>(&OutputHooks<Index>::Present1),
      reinterpret_cast<void*>(&OutputHooks<Index>::Resize), reinterpret_cast<void*>(&OutputHooks<Index>::Resize1)};
}
inline bool InstallOutput(IDXGISwapChain3* swapchain, uintptr_t* method_module) {
  auto** table = *reinterpret_cast<void***>(swapchain);
  HMODULE owner = nullptr;
  wchar_t module_path[MAX_PATH]{}, system_path[MAX_PATH]{};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(table[8]), &owner)) return false;
  *method_module = reinterpret_cast<uintptr_t>(owner);
  const auto path_length = GetModuleFileNameW(owner, module_path, MAX_PATH);
  const auto system_length = GetSystemDirectoryW(system_path, MAX_PATH);
  if (path_length == 0 || path_length >= MAX_PATH || system_length == 0 || system_length + 10 > MAX_PATH) return false;
  std::copy_n(L"\\dxgi.dll", 10, system_path + system_length);
  // Unwrapping is intentionally limited to ReShade. Other providers' proxies
  // cannot become a native output merely by exposing IDXGISwapChain3.
  if (_wcsicmp(module_path, system_path) != 0) return false;
  const std::array<void*, 4> targets{table[8], table[22], table[13], table[39]};
  std::unique_lock lock(install_mutex);
  for (size_t index = 0; index < outputs.size(); ++index) {
    auto& slot = outputs[index];
    if (slot.installed) {
      if (slot.records[0].target == targets[0]) return slot.complete;
      continue;
    }
    const auto detours = (index == 0 ? OutputDetours<0>() : index == 1 ? OutputDetours<1>()
        : index == 2 ? OutputDetours<2>() : OutputDetours<3>());
    return AttachPinned(&slot, targets, detours, 4);
  }
  return false;
}
using CreateFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
inline void ObserveCreated(uint64_t serial, HRESULT result, IUnknown* device, IDXGISwapChain* returned, uint32_t method) {
  if (serial == 0) return;
  Candidate candidate;
  if (SUCCEEDED(result) && returned != nullptr) {
    const auto queue = ReadQueue(device);
    ComPtr<IDXGISwapChain3> swapchain;
    renodx::utils::directx::NativeFromReShadeProxy(&returned);
    if (SUCCEEDED(returned->QueryInterface(IID_PPV_ARGS(&swapchain)))) {
      ComPtr<ID3D12Device> swapchain_device;
      DXGI_SWAP_CHAIN_DESC desc{};
      candidate.swapchain = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(swapchain.Get()));
      if (queue.queue != 0 && SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&swapchain_device)))
          && queue.device == reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(swapchain_device.Get()))
          && SUCCEEDED(swapchain->GetDesc(&desc))) {
        candidate.queue = queue.queue;
        candidate.device = queue.device;
        candidate.buffer_count = desc.BufferCount;
        candidate.hooks_ready = InstallOutput(swapchain.Get(), &candidate.method_module);
      }
    }
  }
  FinishCreate(serial, result, candidate, method);
}
template <size_t Index>
struct FactoryHooks {
  static HRESULT STDMETHODCALLTYPE Create(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** output) {
    uint64_t serial = 0;
    Observe([&] { serial = BeginCreate(reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(factory)),
        reinterpret_cast<uintptr_t>(desc != nullptr ? desc->OutputWindow : nullptr)); });
    const auto result = reinterpret_cast<CreateFn>(factories[Index].real[0])(factory, device, desc, output);
    Observe([&] { ObserveCreated(serial, result, device, (output != nullptr && SUCCEEDED(result) ? *output : nullptr), 1); });
    return result;
  }
  static HRESULT STDMETHODCALLTYPE CreateHwnd(IDXGIFactory2* factory, IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* restrict_output, IDXGISwapChain1** output) {
    uint64_t serial = 0;
    Observe([&] { serial = BeginCreate(reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(factory)), reinterpret_cast<uintptr_t>(window)); });
    const auto result = reinterpret_cast<CreateHwndFn>(factories[Index].real[1])(factory, device, window, desc, fullscreen, restrict_output, output);
    Observe([&] { ObserveCreated(serial, result, device, (output != nullptr && SUCCEEDED(result) ? *output : nullptr), 2); });
    return result;
  }
};
template <size_t Index>
inline std::array<void*, 4> FactoryDetours() {
  return {reinterpret_cast<void*>(&FactoryHooks<Index>::Create), reinterpret_cast<void*>(&FactoryHooks<Index>::CreateHwnd), nullptr, nullptr};
}
struct InitThreadBinding {
  uint32_t slot = UINT32_MAX;
  uintptr_t handle = 0;
  uint64_t serial = 0;
};
static thread_local InitThreadBinding init_thread_binding;
struct InitScope {
  InitThreadBinding previous;
  uint64_t serial = 0;
  bool owner = false, finished = false;
  InitScope(uint32_t slot, uintptr_t module, void* handle, IDXGIFactory2* factory, HWND window, IDXGISwapChain* application_swapchain) noexcept {
    previous = init_thread_binding;
    owner = previous.serial == 0 || previous.slot != slot || previous.handle != reinterpret_cast<uintptr_t>(handle);
    if (!owner) {
      serial = previous.serial;
      return;
    }
    Observe([&] {
      serial = BeginInit({.slot = slot, .module = module, .handle = reinterpret_cast<uintptr_t>(handle),
          .factory = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(factory)), .window = reinterpret_cast<uintptr_t>(window),
          .application_swapchain = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(application_swapchain))});
      init_thread_binding = {.slot = slot, .handle = reinterpret_cast<uintptr_t>(handle), .serial = serial};
      bool ready = false;
      if (serial != 0 && factory != nullptr) {
        renodx::utils::directx::NativeFromReShadeProxy(&factory);
        auto** table = *reinterpret_cast<void***>(factory);
        const std::array<void*, 4> targets{table[10], table[15], nullptr, nullptr};
        std::unique_lock lock(install_mutex);
        for (size_t index = 0; index < factories.size(); ++index) {
          auto& entry = factories[index];
          if (entry.installed) {
            if (entry.records[0].target == targets[0]) {
              ready = entry.complete;
              break;
            }
            continue;
          }
          const auto detours = (index == 0 ? FactoryDetours<0>() : index == 1 ? FactoryDetours<1>()
              : index == 2 ? FactoryDetours<2>() : FactoryDetours<3>());
          ready = AttachPinned(&entry, targets, detours, 2);
          break;
        }
      }
      std::unique_lock lock(probe_state.mutex);
      if (probe_state.snapshot.pending.active && probe_state.snapshot.pending.serial == serial) {
        probe_state.snapshot.pending.factory_hooks_ready = ready;
      }
    });
  }
  void Finish(int32_t result) noexcept {
    if (owner && !finished) {
      Observe([&] { FinishInit(serial, result); });
    }
    finished = true;
  }
  ~InitScope() {
    if (owner) {
      if (!finished) {
        Observe([&] { FinishInit(serial, E_FAIL); });
      }
      init_thread_binding = previous;
    }
  }
  InitScope(const InitScope&) = delete;
  InitScope& operator=(const InitScope&) = delete;
};
inline void Stop() {
  const std::lock_guard lifecycle(lifecycle_mutex);
  StopObservation();
  std::unique_lock lock(install_mutex);
  for (size_t index = 0; index < outputs.size(); ++index) {
    auto& output = outputs[index];
    auto& factory = factories[index];
    const auto output_detours = (index == 0 ? OutputDetours<0>() : index == 1 ? OutputDetours<1>()
        : index == 2 ? OutputDetours<2>() : OutputDetours<3>());
    const auto factory_detours = (index == 0 ? FactoryDetours<0>() : index == 1 ? FactoryDetours<1>()
        : index == 2 ? FactoryDetours<2>() : FactoryDetours<3>());
    for (size_t method = 0; method < 4; ++method) {
      if (output.installed && output.real[method] != nullptr) {
        detour_guard::Neutralize(&output.real[method], output_detours[method], output.records[method]);
      }
      if (factory.installed && factory.real[method] != nullptr) {
        detour_guard::Neutralize(&factory.real[method], factory_detours[method], factory.records[method]);
      }
    }
  }
}
}  // namespace renodx::addons::dlss5::xefg_present_probe
#endif
