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
#include <memory>
#include <string>

#include "../../utils/directx.hpp"
#include "detour_guard.hpp"
#include "fg_frame.hpp"
#include "submission_tracker.hpp"

namespace renodx::addons::dlss5::xefg_present_probe {
using Microsoft::WRL::ComPtr;
inline constexpr const char* kEvidence = "metadata_not_pixels_or_completion";
inline constexpr size_t kMaxBuffers = 8;
inline constexpr uint32_t kMaxSamplesPerWindow = 64;
enum class Kind : uint8_t { INIT_BEGIN, INIT_RETURN, CREATE_RETURN, PRESENT_BEFORE, PRESENT_RETURN, RESIZE_BEFORE, RESIZE_RETURN, INVALIDATED };
using DebugFn = int32_t (*)(void*, int32_t, uint32_t, void*);
struct QueueOwner {
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Device> device;
};
struct InitKey {
  uint32_t slot = UINT32_MAX;
  uintptr_t module = 0, handle = 0, factory = 0, window = 0, application_swapchain = 0;
  std::string module_path;
  DebugFn debug = nullptr;
};
struct QueueIdentity {
  uintptr_t queue = 0, device = 0;
  std::shared_ptr<QueueOwner> owner;
};
struct Candidate {
  uintptr_t swapchain = 0, queue = 0, device = 0, method_module = 0;
  uint32_t buffer_count = 0;
  bool hooks_ready = false;
  uint32_t width = 0, height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  std::shared_ptr<QueueOwner> owner;
};
struct PendingInit {
  InitKey key;
  uint64_t serial = 0;
  uintptr_t context = 0;
  uint64_t context_epoch = 0, context_generation = 0;
  Candidate candidate;
  std::array<uintptr_t, kMaxBuffers> queues{};
  std::array<std::shared_ptr<QueueOwner>, kMaxBuffers> queue_owners{};
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
struct Target {
  uint32_t slot = UINT32_MAX;
  uintptr_t module = 0, handle = 0;
  uint64_t init_serial = 0;
  uintptr_t context = 0;
  uint64_t context_generation = 0, buffer_generation = 0;
};
struct DebugCall {
  uint64_t call_id = 0, enter_sequence = 0, return_sequence = 0;
  Target target;
  int32_t feature = 0;
  uint32_t value = 0;
  uintptr_t argument = 0;
  uint64_t own_job_id = 0;
  uintptr_t caller_address = 0, caller_module = 0;
  std::string caller_path;
  uint64_t native_present_serial = 0;
  uint64_t entered_ms = 0, returned_ms = 0;
  uint32_t entered_thread = 0, returned_thread = 0;
  bool returned = false;
  int32_t result = E_PENDING;
};
struct DebugSnapshot {
  uint64_t sequence = 0, entered = 0, returned = 0, in_flight = 0;
  uint64_t overwritten = 0, overwritten_unreturned = 0, stale_returns = 0;
  std::array<DebugCall, 24> calls{};
};
struct TagControl {
  uint64_t context_generation = 0, buffer_generation = 0;
  uint64_t entered = 0, returned = 0, external_calls = 0, external_false_calls = 0;
  uint64_t conflicts = 0, own_true_call = 0, own_window_conflicts = 0, last_entry = 0, last_return = 0;
  uint32_t in_flight = 0;
  bool value_available = false, enabled = false;
};
struct Binding {
  InitKey key;
  uint64_t init_serial = 0;
  uintptr_t context = 0;
  uint64_t context_epoch = 0, context_generation = 0;
  Candidate created;
  std::array<uintptr_t, kMaxBuffers> queues{};
  std::array<std::shared_ptr<QueueOwner>, kMaxBuffers> queue_owners{};
  uint32_t buffer_count = 0;
  uint64_t buffer_generation = 0, present_calls = 0, successful_present_calls = 0, test_calls = 0;
  uint64_t last_present_serial = 0;
  uint32_t last_present_index = UINT32_MAX;
  int32_t last_present_result = 0;
  bool live = false, bound = false, resizing = false;
  const char* reason = "unobserved";
  TagControl tag_control;
};
struct Snapshot {
  bool stopped = false;
  uint64_t sequence = 0, next_init = 0, next_present = 0;
  uint64_t rejected_creates = 0, stale_returns = 0, overflow = 0;
  PendingInit pending;
  std::array<Binding, 4> bindings{};
  std::array<Event, 24> events{};
  DebugSnapshot debug_calls;
};
struct Calibration {
  Target target;
  bool baseline_known = false, tag_enabled = false, active = false, restore_pending = false, cancelled = false;
  uint32_t control_thread = 0, requested = 0, attempted = 0, accepted = 0, submitted = 0;
  uint64_t generation = 0, deadline_ms = 0, api_sequence = 0;
  int32_t baseline_result = E_PENDING, tag_result = E_PENDING, restore_result = E_PENDING;
  const char* reason = "idle";
};
enum class ControlAction : uint8_t { BASELINE, ARM, POLL, STOP };
enum class ControlPhase : uint8_t { IDLE, QUEUED, WAITING_LIFECYCLE, SDK_ENTERED, RETURNED, CANCELLED, EXPIRED, REJECTED };
struct ControlJob {
  uint64_t job_id = 0;
  std::string request_id;
  ControlAction action = ControlAction::BASELINE;
  ControlPhase phase = ControlPhase::IDLE;
  Target target;
  bool tag_enabled = false;
  uint32_t samples = 0, duration_ms = 0;
  bool finished = false, accepted = false, cancel_requested = false;
  bool sdk_entered = false, sdk_returned = false, api_result_available = false;
  int32_t result = E_PENDING;
  uint32_t queued_thread = 0, worker_thread = 0, lifecycle_thread = 0, sdk_entered_thread = 0, sdk_returned_thread = 0;
  uint64_t queued_ms = 0, deadline_ms = 0, waiting_ms = 0, lifecycle_ms = 0, sdk_entered_ms = 0, sdk_returned_ms = 0, finished_ms = 0;
  const char* reason = "idle";
};
struct CopyContext {
  Target target;
  uint64_t window_generation = 0, native_serial = 0, tag_api_sequence = 0;
  uintptr_t swapchain = 0, queue = 0, buffer = 0;
  uint32_t index = UINT32_MAX, width = 0, height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  bool tag_enabled = false;
  uint64_t recording_list = 0, recording_generation = 0;
};
struct CopyPacket {
  std::shared_mutex mutex;
  std::shared_ptr<QueueOwner> owner;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  bool submitted = false, signaled = false, retired = false, completed = false, removed = false, retiring = false, retirement_failed = false;
  HRESULT signal_result = E_PENDING;
  ~CopyPacket() {
    if (!submitted && list != nullptr) {
      submission::OnCommandListReset(list.Get());
    }
  }
  void Poll() {
    {
      std::unique_lock lock(mutex);
      if (retired || retiring || retirement_failed || !submitted || !signaled) return;
      retiring = true;
    }
    const auto value = fence->GetCompletedValue();
    bool release_recording = false, healthy = false, failed = false;
    if (value == UINT64_MAX) {
      submission::OnCommandListReset(list.Get());
      release_recording = true;
    } else if (value >= 1) {
      healthy = true;
      if (SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr))) {
        submission::OnCommandListReset(list.Get());
        release_recording = SUCCEEDED(list->Close());
      }
      failed = !release_recording;
    }
    std::unique_lock lock(mutex);
    retiring = false;
    removed = value == UINT64_MAX;
    completed = healthy;
    retired = release_recording;
    retirement_failed = failed;
  }
  bool IsRetired() {
    std::shared_lock lock(mutex);
    return retired;
  }
  bool IsCompleted() {
    std::shared_lock lock(mutex);
    return completed && retired && signaled && !removed;
  }
};
using PrepareCopy = uint64_t (*)(const CopyContext&, const std::shared_ptr<CopyPacket>&, ID3D12Resource*);
using ReturnCopy = void (*)(uint64_t, HRESULT);
using InstallCopyHooks = bool (*)(ID3D12CommandQueue*, ID3D12GraphicsCommandList*);
using CancelCopy = void (*)(const char*);
struct CaptureCallbacks {
  PrepareCopy prepare = nullptr;
  ReturnCopy returned = nullptr;
  InstallCopyHooks install = nullptr;
  CancelCopy cancel = nullptr;
  bool (*ready)() = nullptr;
  void (*release)() = nullptr;
};
static Calibration calibration;
static CaptureCallbacks capture_callbacks;
static ControlJob control_job;
static uint64_t control_sequence = 0;
static thread_local uint64_t executing_control_job = 0;
struct OwnDebugRequest {
  Target target;
  uint64_t job_id = 0;
  uint32_t value = 0;
  bool consumed = false;
};
static thread_local OwnDebugRequest* own_debug_request = nullptr;
struct ProbeState {
  std::shared_mutex mutex;
  Snapshot snapshot;
};
static ProbeState probe_state;
// SDK Init/Destroy and the explicit control worker serialize here. Native
// DXGI callbacks only take the short ledger lock; Stop never waits here.
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
  PendingInit retired;
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
  retired = std::move(snapshot.pending);
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
      pending.queue_owners.fill(candidate.owner);
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
  Binding retired;
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
    retired = std::move(*selected);
    *selected = {.key = pending.key, .init_serial = serial, .context = pending.context,
        .context_epoch = pending.context_epoch, .context_generation = pending.context_generation,
        .created = pending.candidate, .queues = pending.queues, .queue_owners = pending.queue_owners, .buffer_count = pending.candidate.buffer_count,
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
      if (calibration.target.context == context) {
        calibration.active = false;
        calibration.restore_pending = calibration.baseline_known;
        calibration.reason = "context_generation_changed";
      }
    }
  }
}
inline void Invalidate(uint32_t slot, uintptr_t handle, const char* reason) {
  std::array<std::shared_ptr<QueueOwner>, 2 * kMaxBuffers + 2> retired;
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  if (snapshot.pending.key.slot == slot && snapshot.pending.key.handle == handle) {
    retired[0] = std::move(snapshot.pending.candidate.owner);
    for (size_t index = 0; index < kMaxBuffers; ++index) {
      retired[1 + index] = std::move(snapshot.pending.queue_owners[index]);
    }
  }
  if (snapshot.pending.active && snapshot.pending.key.slot == slot && snapshot.pending.key.handle == handle) {
    snapshot.pending.ambiguous = true;
  }
  for (auto& binding : snapshot.bindings) {
    if (binding.live && binding.key.slot == slot && binding.key.handle == handle) {
      binding.live = binding.bound = false;
      binding.reason = reason;
      retired[kMaxBuffers + 1] = std::move(binding.created.owner);
      for (size_t index = 0; index < kMaxBuffers; ++index) {
        retired[kMaxBuffers + 2 + index] = std::move(binding.queue_owners[index]);
      }
      if (calibration.target.init_serial == binding.init_serial) {
        calibration.active = false;
        calibration.reason = reason;
      }
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
  const bool target_resizing = std::any_of(probe_state.snapshot.bindings.begin(), probe_state.snapshot.bindings.end(),
      [&](const Binding& binding) { return binding.init_serial == calibration.target.init_serial && binding.created.swapchain == swapchain; });
  if (calibration.active && target_resizing) {
    calibration.active = false;
    calibration.restore_pending = calibration.baseline_known;
    calibration.reason = "native_resizing";
  }
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
                         const std::array<QueueIdentity, kMaxBuffers>& queues, bool queues_supplied,
                         uint32_t width = 0, uint32_t height = 0, DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN) {
  if (ticket.init_serial == 0) return;
  std::array<std::shared_ptr<QueueOwner>, kMaxBuffers> retired_owners;
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  const auto apply_queues = [&](std::array<uintptr_t, kMaxBuffers>* destination,
                                std::array<std::shared_ptr<QueueOwner>, kMaxBuffers>* owners,
                                uint32_t previous_count, uintptr_t device) {
    if (buffer_count == 0 || buffer_count > kMaxBuffers || previous_count == 0 || previous_count > kMaxBuffers) return false;
    if (queues_supplied) {
      if (std::any_of(queues.begin(), queues.begin() + buffer_count,
          [&](const QueueIdentity& queue) { return queue.queue == 0 || queue.device != device; })) return false;
      for (uint32_t index = 0; index < buffer_count; ++index) {
        (*destination)[index] = queues[index].queue;
      }
      retired_owners = std::move(*owners);
      for (uint32_t index = 0; index < buffer_count; ++index) {
        (*owners)[index] = queues[index].owner;
      }
    } else if (previous_count != buffer_count) {
      if (!std::all_of(destination->begin(), destination->begin() + previous_count,
          [&](uintptr_t queue) { return queue != 0 && queue == (*destination)[0]; })) return false;
      destination->fill((*destination)[0]);
      retired_owners = std::move(*owners);
      owners->fill(retired_owners[0]);
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
      if (apply_queues(&pending.queues, &pending.queue_owners, pending.candidate.buffer_count, pending.candidate.device)) {
        pending.candidate.buffer_count = buffer_count;
        pending.candidate.width = width;
        pending.candidate.height = height;
        pending.candidate.format = format;
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
    } else if (!apply_queues(&binding.queues, &binding.queue_owners, binding.buffer_count, binding.created.device)) {
      binding.reason = "resize_queue_contract_unavailable";
    } else {
      binding.buffer_count = buffer_count;
      binding.created.width = width;
      binding.created.height = height;
      binding.created.format = format;
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
  Snapshot retired;
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  retired = snapshot;
  snapshot.stopped = true;
  snapshot.pending.active = false;
  snapshot.pending.candidate.owner.reset();
  snapshot.pending.queue_owners = {};
  for (auto& binding : snapshot.bindings) {
    binding.live = binding.bound = false;
    binding.reason = "stopped";
    binding.created.owner.reset();
    binding.queue_owners = {};
  }
}

inline bool Matches(const Binding& binding, const Target& target, bool generations = true) {
  return binding.live && binding.init_serial == target.init_serial && binding.key.slot == target.slot
      && binding.key.module == target.module && binding.key.handle == target.handle && binding.context == target.context
      && (!generations || (binding.context_generation == target.context_generation && binding.buffer_generation == target.buffer_generation));
}
inline Calibration CopyCalibration() {
  std::shared_lock lock(probe_state.mutex);
  return calibration;
}
inline ControlJob CopyControl() {
  std::shared_lock lock(probe_state.mutex);
  return control_job;
}
inline bool ControlBusy(const ControlJob& job) {
  return job.job_id != 0 && !job.finished;
}
inline uint64_t QueueControl(const std::string& request_id, ControlAction action, const Target& target,
                             bool tag_enabled = false, uint32_t samples = 0, uint32_t duration_ms = 0) {
  std::unique_lock lock(probe_state.mutex, std::try_to_lock);
  if (!lock.owns_lock() || probe_state.snapshot.stopped) return 0;
  if (control_job.request_id == request_id && control_job.job_id != 0) return control_job.job_id;
  if (ControlBusy(control_job)) {
    if (action == ControlAction::STOP && control_job.target.init_serial == target.init_serial) {
      control_job.cancel_requested = true;
      calibration.active = false;
      calibration.cancelled = true;
      return control_job.job_id;
    }
    return 0;
  }
  control_job = {.job_id = ++control_sequence, .request_id = request_id, .action = action,
      .phase = ControlPhase::QUEUED, .target = target, .tag_enabled = tag_enabled, .samples = samples,
      .duration_ms = duration_ms, .queued_thread = GetCurrentThreadId(), .queued_ms = GetTickCount64(),
      .deadline_ms = GetTickCount64() + 2500, .reason = "queued"};
  return control_job.job_id;
}
inline void RejectControlStart(const char* reason = "worker_unavailable") {
  std::unique_lock lock(probe_state.mutex);
  if (control_job.phase == ControlPhase::QUEUED) {
    control_job.phase = ControlPhase::REJECTED;
    control_job.finished = true;
    control_job.finished_ms = GetTickCount64();
    control_job.reason = reason;
  }
}
inline void ConfigureCapture(CaptureCallbacks callbacks) {
  std::unique_lock lock(probe_state.mutex);
  capture_callbacks = callbacks;
}
inline DebugCall BeginDebug(uint32_t slot, uintptr_t module, uintptr_t handle, int32_t feature,
                            uint32_t value, void* argument, uintptr_t caller_address = 0,
                            uintptr_t caller_module = 0, const std::string& caller_path = {}) {
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  if (snapshot.stopped) return {};
  for (auto& binding : snapshot.bindings) {
    if (!binding.live || !binding.bound || binding.resizing || binding.key.slot != slot
        || binding.key.module != module || binding.key.handle != handle) continue;
    auto& debug = snapshot.debug_calls;
    DebugCall call{.call_id = ++debug.entered, .enter_sequence = ++debug.sequence,
        .target = {.slot = slot, .module = module, .handle = handle, .init_serial = binding.init_serial,
            .context = binding.context, .context_generation = binding.context_generation, .buffer_generation = binding.buffer_generation},
        .feature = feature, .value = value, .argument = reinterpret_cast<uintptr_t>(argument),
        .caller_address = caller_address, .caller_module = caller_module, .caller_path = caller_path,
        .native_present_serial = binding.last_present_serial,
        .entered_ms = GetTickCount64(), .entered_thread = GetCurrentThreadId()};
    if (own_debug_request != nullptr && !own_debug_request->consumed && feature == 1
        && value == own_debug_request->value && Matches(binding, own_debug_request->target)) {
      call.own_job_id = own_debug_request->job_id;
      own_debug_request->consumed = true;
    }
    auto& retained = debug.calls[(call.call_id - 1) % debug.calls.size()];
    if (retained.call_id != 0) {
      ++debug.overwritten;
      debug.overwritten_unreturned += !retained.returned;
    }
    retained = call;
    ++debug.in_flight;
    auto& tag = binding.tag_control;
    if (tag.context_generation != binding.context_generation || tag.buffer_generation != binding.buffer_generation) {
      tag = {.context_generation = binding.context_generation, .buffer_generation = binding.buffer_generation};
    }
    ++tag.entered;
    if (call.own_job_id == 0) {
      ++tag.external_calls;
    }
    if (feature == 1) {
      if (call.own_job_id != 0 && value != 0) {
        tag.own_true_call = call.call_id;
        tag.own_window_conflicts = 0;
      }
      if (tag.in_flight != 0) {
        ++tag.conflicts;
        ++tag.own_window_conflicts;
      }
      if (tag.own_true_call != 0 && call.call_id > tag.own_true_call && value == 0) {
        tag.conflicts += call.own_job_id == 0;
        ++tag.own_window_conflicts;
      }
      if (call.own_job_id == 0 && value == 0) {
        ++tag.external_false_calls;
      }
      ++tag.in_flight;
      tag.last_entry = call.call_id;
      tag.value_available = false;
    }
    return call;
  }
  return {};
}
inline void FinishDebug(const DebugCall& ticket, int32_t result) {
  if (ticket.call_id == 0) return;
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  auto& debug = snapshot.debug_calls;
  const auto sequence = ++debug.sequence;
  ++debug.returned;
  --debug.in_flight;
  auto& retained = debug.calls[(ticket.call_id - 1) % debug.calls.size()];
  if (retained.call_id == ticket.call_id) {
    retained.return_sequence = sequence;
    retained.returned_ms = GetTickCount64();
    retained.returned_thread = GetCurrentThreadId();
    retained.returned = true;
    retained.result = result;
  }
  for (auto& binding : snapshot.bindings) {
    if (!Matches(binding, ticket.target) || !binding.bound || binding.resizing) continue;
    auto& tag = binding.tag_control;
    ++tag.returned;
    if (ticket.feature == 1) {
      --tag.in_flight;
      tag.last_return = ticket.call_id;
      tag.value_available = tag.in_flight == 0 && tag.last_entry == ticket.call_id && result >= 0;
      if (tag.value_available) {
        tag.enabled = ticket.value != 0;
      }
    }
    return;
  }
  ++debug.stale_returns;
}
inline bool ApplyTag(const Target& target, bool enabled, int32_t* result, bool generations = true) {
  if (executing_control_job == 0) {
    *result = E_ACCESSDENIED;
    return false;
  }
  const std::lock_guard lifecycle(lifecycle_mutex);
  Binding selected;
  {
    std::shared_lock lock(probe_state.mutex);
    for (const auto& binding : probe_state.snapshot.bindings) {
      if (Matches(binding, target, generations) && (!generations || (binding.bound && !binding.resizing))) {
        selected = binding;
      }
    }
  }
  if (selected.init_serial == 0 || selected.key.debug == nullptr || selected.key.module_path.empty()) {
    *result = E_NOINTERFACE;
    return false;
  }
  {
    std::unique_lock lock(probe_state.mutex);
    if (control_job.job_id != executing_control_job || control_job.cancel_requested
        || GetTickCount64() >= control_job.deadline_ms) {
      *result = E_ABORT;
      return false;
    }
    control_job.phase = ControlPhase::SDK_ENTERED;
    control_job.sdk_entered = true;
    control_job.sdk_entered_thread = GetCurrentThreadId();
    control_job.sdk_entered_ms = GetTickCount64();
    control_job.reason = "sdk_entered";
  }
  // Feature 1 is TAG_INTERPOLATED_FRAMES. No SHOW call or assumed initial value.
  OwnDebugRequest request{.target = {.slot = selected.key.slot, .module = selected.key.module,
      .handle = selected.key.handle, .init_serial = selected.init_serial, .context = selected.context,
      .context_generation = selected.context_generation, .buffer_generation = selected.buffer_generation},
      .job_id = executing_control_job, .value = enabled ? 1u : 0u};
  struct OwnCallScope {
    OwnDebugRequest* previous = own_debug_request;
    explicit OwnCallScope(OwnDebugRequest* request) { own_debug_request = request; }
    ~OwnCallScope() { own_debug_request = previous; }
  } own_call(&request);
  *result = selected.key.debug(reinterpret_cast<void*>(selected.key.handle), 1, enabled ? 1u : 0u, nullptr);
  std::unique_lock lock(probe_state.mutex);
  ++calibration.api_sequence;
  control_job.sdk_returned = true;
  control_job.api_result_available = true;
  control_job.result = *result;
  control_job.sdk_returned_thread = GetCurrentThreadId();
  control_job.sdk_returned_ms = GetTickCount64();
  return *result >= 0;
}
inline bool RestoreBaseline() {
  if (executing_control_job == 0) return false;
  const std::lock_guard lifecycle(lifecycle_mutex);
  const auto before = CopyCalibration();
  if (!before.baseline_known) return false;
  int32_t result = E_PENDING;
  const bool restored = ApplyTag(before.target, false, &result, false);
  std::unique_lock lock(probe_state.mutex);
  calibration.restore_result = result;
  calibration.restore_pending = !restored;
  calibration.active = false;
  if (restored) {
    calibration.tag_enabled = false;
  }
  calibration.reason = (restored ? "known_test_baseline_restored" : "test_baseline_restore_failed");
  return restored;
}
inline void RestoreForHandle(uint32_t slot, uintptr_t handle) {
  std::unique_lock lock(probe_state.mutex);
  if (ControlBusy(control_job) && control_job.target.slot == slot && control_job.target.handle == handle) {
    control_job.cancel_requested = true;
  }
  if (calibration.target.slot == slot && calibration.target.handle == handle) {
    calibration.active = false;
    calibration.cancelled = true;
    if (calibration.baseline_known && (calibration.tag_enabled || calibration.restore_pending)) {
      calibration.reason = "context_ending_without_synchronous_restore";
    }
  }
}
inline void ContextDestroyed(uint32_t slot, uintptr_t handle, int32_t result) {
  std::unique_lock lock(probe_state.mutex);
  if (result >= 0 && calibration.target.slot == slot && calibration.target.handle == handle) {
    calibration.baseline_known = calibration.active = calibration.tag_enabled = calibration.restore_pending = false;
    calibration.cancelled = true;
    calibration.reason = "context_destroyed";
  }
}
inline bool EstablishBaseline(const Target& target) {
  if (executing_control_job == 0) return false;
  const std::lock_guard lifecycle(lifecycle_mutex);
  const auto before = CopyCalibration();
  if (before.active || before.restore_pending || before.tag_enabled) return false;
  int32_t result = E_PENDING;
  const bool accepted = ApplyTag(target, false, &result);
  std::unique_lock lock(probe_state.mutex);
  const auto sequence = calibration.api_sequence;
  const auto generation = calibration.generation;
  calibration = {.target = target, .baseline_known = accepted, .control_thread = GetCurrentThreadId(),
      .generation = generation, .api_sequence = sequence, .baseline_result = result,
      .reason = (accepted ? "known_false_test_baseline_established" : "test_baseline_failed")};
  return accepted;
}
inline bool Arm(const Target& target, bool tag_enabled, uint32_t samples, uint32_t duration_ms) {
  if (executing_control_job == 0) return false;
  const std::lock_guard lifecycle(lifecycle_mutex);
  const auto before = CopyCalibration();
  if (!before.baseline_known || before.active || before.restore_pending || samples == 0 || samples > kMaxSamplesPerWindow
      || duration_ms < 100 || duration_ms > 3000 || before.target.init_serial != target.init_serial
      || before.target.slot != target.slot || before.target.module != target.module || before.target.handle != target.handle
      || before.target.context != target.context) return false;
  int32_t result = E_PENDING;
  const bool accepted = ApplyTag(target, tag_enabled, &result);
  std::unique_lock lock(probe_state.mutex);
  calibration.target = target;
  calibration.tag_result = result;
  calibration.tag_enabled = accepted && tag_enabled;
  calibration.active = accepted;
  calibration.cancelled = !accepted;
  calibration.restore_pending = !accepted;
  calibration.requested = samples;
  calibration.attempted = 0;
  calibration.accepted = 0;
  calibration.submitted = 0;
  ++calibration.generation;
  calibration.deadline_ms = GetTickCount64() + duration_ms;
  calibration.reason = (accepted ? "armed" : "tag_control_failed");
  return accepted;
}
inline bool PollControl(bool stop = false) {
  if (executing_control_job == 0) return false;
  CancelCopy cancel = nullptr;
  bool restore = false;
  {
    std::unique_lock lock(probe_state.mutex);
    if (stop || (calibration.active && GetTickCount64() >= calibration.deadline_ms)) {
      calibration.active = false;
      calibration.cancelled = stop;
      calibration.restore_pending = calibration.baseline_known;
      calibration.reason = (stop ? "explicit_stop" : "time_limit");
    }
    if (stop) {
      cancel = capture_callbacks.cancel;
    }
    restore = calibration.restore_pending;
  }
  if (cancel != nullptr) {
    cancel("explicit_stop");
  }
  return !restore || RestoreBaseline();
}
inline void RunControlJob(void (*finish_capture)()) noexcept {
  ControlJob selected;
  {
    std::unique_lock lock(probe_state.mutex);
    if (control_job.phase != ControlPhase::QUEUED) return;
    control_job.worker_thread = GetCurrentThreadId();
    control_job.waiting_ms = GetTickCount64();
    control_job.phase = ControlPhase::WAITING_LIFECYCLE;
    control_job.reason = "waiting_lifecycle";
    selected = control_job;
  }
  bool accepted = false;
  ControlPhase terminal = ControlPhase::RETURNED;
  const char* reason = "control_operation_rejected";
  try {
    {
      const std::lock_guard lifecycle(lifecycle_mutex);
      bool current = false;
      {
        std::unique_lock lock(probe_state.mutex);
        control_job.lifecycle_thread = GetCurrentThreadId();
        control_job.lifecycle_ms = GetTickCount64();
        if (control_job.cancel_requested || probe_state.snapshot.stopped) {
          terminal = ControlPhase::CANCELLED;
          reason = "cancelled_before_sdk";
        } else if (GetTickCount64() >= control_job.deadline_ms) {
          terminal = ControlPhase::EXPIRED;
          reason = "expired_before_sdk";
        } else if (selected.queued_thread == GetCurrentThreadId()) {
          terminal = ControlPhase::REJECTED;
          reason = "control_worker_thread_required";
        } else {
          const bool exact_generation = selected.action == ControlAction::BASELINE || selected.action == ControlAction::ARM;
          for (const auto& binding : probe_state.snapshot.bindings) {
            if (Matches(binding, selected.target, exact_generation)) {
              current = true;
            }
          }
          if (!current) {
            terminal = ControlPhase::REJECTED;
            reason = "control_target_changed";
          }
        }
      }
      if (current) {
        struct WorkerTurn {
          explicit WorkerTurn(uint64_t job_id) { executing_control_job = job_id; }
          ~WorkerTurn() { executing_control_job = 0; }
        } turn(selected.job_id);
        switch (selected.action) {
          case ControlAction::BASELINE: accepted = EstablishBaseline(selected.target); break;
          case ControlAction::ARM: accepted = Arm(selected.target, selected.tag_enabled, selected.samples, selected.duration_ms); break;
          case ControlAction::POLL: accepted = PollControl(); break;
          case ControlAction::STOP: accepted = PollControl(true); break;
        }
        reason = (accepted ? "control_operation_returned" : "control_operation_rejected");
      }
    }
  } catch (...) {
    reason = "control_worker_exception";
  }
  {
    std::unique_lock lock(probe_state.mutex);
    if (control_job.cancel_requested && control_job.sdk_entered) {
      calibration.active = false;
      calibration.cancelled = true;
      calibration.restore_pending = calibration.baseline_known && calibration.tag_enabled;
      reason = "cancel_requested_after_sdk_entry";
    }
  }
  if (finish_capture != nullptr) {
    Observe([&] { finish_capture(); });
  }
  std::unique_lock lock(probe_state.mutex);
  if (!control_job.sdk_entered && control_job.cancel_requested) {
    terminal = ControlPhase::CANCELLED;
    reason = "cancelled_before_sdk";
  } else if (!control_job.sdk_entered && GetTickCount64() >= control_job.deadline_ms) {
    terminal = ControlPhase::EXPIRED;
    reason = "expired_before_sdk";
  }
  control_job.phase = terminal;
  control_job.accepted = accepted;
  control_job.finished = true;
  control_job.finished_ms = GetTickCount64();
  control_job.reason = reason;
}
inline bool FrameCurrent(const CopyContext& context) {
  std::shared_lock lock(probe_state.mutex);
  if (calibration.cancelled || calibration.generation != context.window_generation) return false;
  for (const auto& binding : probe_state.snapshot.bindings) {
    if (Matches(binding, context.target) && binding.bound && binding.created.swapchain == context.swapchain
        && context.index < binding.buffer_count && binding.queues[context.index] == context.queue) return true;
  }
  return false;
}
struct CopyReturn {
  uint64_t serial = 0;
  ReturnCopy returned = nullptr;
};
inline CopyReturn CaptureBefore(const PresentTicket& ticket, IDXGISwapChain* swapchain) {
  if (ticket.serial == 0 || ticket.queue == 0) return {};
  std::shared_ptr<QueueOwner> owner;
  CopyContext context;
  CaptureCallbacks callbacks;
  {
    std::unique_lock lock(probe_state.mutex);
    if (!calibration.active || GetTickCount64() >= calibration.deadline_ms) {
      if (calibration.active) {
        calibration.active = false;
        calibration.restore_pending = true;
        calibration.reason = "time_limit";
      }
      return {};
    }
    for (const auto& binding : probe_state.snapshot.bindings) {
      if (Matches(binding, calibration.target) && binding.bound && !binding.resizing
          && binding.init_serial == ticket.init_serial && binding.buffer_generation == ticket.buffer_generation
          && ticket.index < binding.buffer_count && binding.queues[ticket.index] == ticket.queue) {
        owner = binding.queue_owners[ticket.index];
      }
    }
    if (owner == nullptr) {
      calibration.reason = "native_queue_owner_unavailable";
      return {};
    }
    context = {.target = calibration.target, .window_generation = calibration.generation,
        .native_serial = ticket.serial, .tag_api_sequence = calibration.api_sequence, .swapchain = ticket.swapchain,
        .queue = ticket.queue, .index = ticket.index, .tag_enabled = calibration.tag_enabled};
    callbacks = capture_callbacks;
  }
  if (callbacks.prepare == nullptr || callbacks.returned == nullptr || callbacks.install == nullptr
      || callbacks.ready == nullptr || callbacks.release == nullptr || !callbacks.ready()) return {};
  struct Attempt {
    CaptureCallbacks callbacks;
    uint64_t generation;
    ~Attempt() {
      callbacks.release();
      std::unique_lock lock(probe_state.mutex);
      if (calibration.generation == generation && calibration.attempted >= calibration.requested) {
        calibration.active = false;
        calibration.restore_pending = true;
        calibration.reason = "sample_attempt_limit";
      }
    }
  } attempt{callbacks, context.window_generation};
  {
    std::unique_lock lock(probe_state.mutex);
    if (!calibration.active || calibration.generation != context.window_generation || calibration.attempted >= calibration.requested) return {};
    ++calibration.attempted;
  }
  ComPtr<ID3D12Resource> buffer;
  if (FAILED(swapchain->GetBuffer(ticket.index, IID_PPV_ARGS(&buffer)))) return {};
  const auto desc = buffer->GetDesc();
  context.buffer = reinterpret_cast<uintptr_t>(buffer.Get());
  context.width = static_cast<uint32_t>(desc.Width);
  context.height = desc.Height;
  context.format = desc.Format;
  auto packet = std::make_shared<CopyPacket>();
  packet->owner = owner;
  if (FAILED(owner->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&packet->allocator)))
      || FAILED(owner->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, packet->allocator.Get(), nullptr, IID_PPV_ARGS(&packet->list)))
      || FAILED(owner->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&packet->fence)))
      || !callbacks.install(owner->queue.Get(), packet->list.Get())) return {};
  const auto recording = submission::RecordingIdentity(packet->list.Get());
  context.recording_list = recording.first;
  context.recording_generation = recording.second;
  const auto serial = callbacks.prepare(context, packet, buffer.Get());
  if (serial == 0) return {};
  struct FailedRecording {
    std::shared_ptr<CopyPacket> packet;
    ReturnCopy returned;
    uint64_t serial;
    bool released = false;
    ~FailedRecording() {
      if (released) return;
      if (!packet->submitted) {
        std::unique_lock lock(packet->mutex);
        submission::OnCommandListReset(packet->list.Get());
        packet->retired = true;
      }
      returned(serial, E_FAIL);
    }
  } failed_recording{packet, callbacks.returned, serial};
  submission::TrackUse(packet->list.Get(), packet->allocator.Get());
  submission::TrackUse(packet->list.Get(), packet->list.Get());
  const bool closed = SUCCEEDED(packet->list->Close());
  HRESULT signal_result = E_FAIL;
  if (closed) {
    ID3D12CommandList* lists[]{packet->list.Get()};
    owner->queue->ExecuteCommandLists(1, lists);
    signal_result = owner->queue->Signal(packet->fence.Get(), 1);
  } else {
    submission::OnCommandListReset(packet->list.Get());
  }
  {
    std::unique_lock lock(packet->mutex);
    packet->submitted = closed;
    packet->signal_result = signal_result;
    packet->signaled = closed && SUCCEEDED(signal_result);
    packet->retired = !closed;
  }
  {
    std::unique_lock lock(probe_state.mutex);
    if (calibration.generation == context.window_generation) {
      ++calibration.accepted;
      calibration.submitted += packet->submitted && packet->signaled;
    }
  }
  failed_recording.released = true;
  return {.serial = serial, .returned = callbacks.returned};
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
  auto owner = std::make_shared<QueueOwner>();
  owner->queue = native;
  owner->device = device;
  return {.queue = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(native)),
      .device = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(device.Get())), .owner = std::move(owner)};
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
  inline static thread_local uintptr_t capture_object = 0;
  struct CaptureScope {
    uintptr_t previous = capture_object;
    bool owner = false;
    CopyReturn capture;
    CaptureScope(const PresentTicket& ticket, IDXGISwapChain* swapchain) noexcept {
      owner = ticket.serial != 0 && capture_object != ticket.swapchain;
      if (owner) {
        capture_object = ticket.swapchain;
        Observe([&] { capture = CaptureBefore(ticket, swapchain); });
      }
    }
    ~CaptureScope() {
      if (owner) {
        capture_object = previous;
      }
    }
  };
  static HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* swapchain, UINT interval, UINT flags) {
    PresentTicket ticket;
    Observe([&] {
      ComPtr<IDXGISwapChain3> swapchain3;
      const auto index = (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) ? swapchain3->GetCurrentBackBufferIndex() : UINT32_MAX);
      ticket = BeginPresent(reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(swapchain)), index, 1, flags);
    });
    const CaptureScope capture(ticket, swapchain);
    const auto result = reinterpret_cast<PresentFn>(outputs[Index].real[0])(swapchain, interval, flags);
    if (capture.capture.returned != nullptr) {
      Observe([&] { capture.capture.returned(capture.capture.serial, result); });
    }
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
    const CaptureScope capture(ticket, swapchain);
    const auto result = reinterpret_cast<Present1Fn>(outputs[Index].real[1])(swapchain, interval, flags, parameters);
    if (capture.capture.returned != nullptr) {
      Observe([&] { capture.capture.returned(capture.capture.serial, result); });
    }
    Observe([&] { FinishPresent(ticket, result); });
    return result;
  }
  static HRESULT STDMETHODCALLTYPE Resize(IDXGISwapChain* swapchain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    const ResizeScope scope(swapchain);
    const auto result = reinterpret_cast<ResizeFn>(outputs[Index].real[2])(swapchain, count, width, height, format, flags);
    Observe([&] {
      DXGI_SWAP_CHAIN_DESC desc{};
      const auto actual_count = (SUCCEEDED(result) && SUCCEEDED(swapchain->GetDesc(&desc)) ? desc.BufferCount : 0);
      FinishResize(scope.ticket, result, actual_count, {}, false, desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format);
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
      FinishResize(scope.ticket, result, actual_count, requested_queues, queues != nullptr, desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format);
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
        candidate.width = desc.BufferDesc.Width;
        candidate.height = desc.BufferDesc.Height;
        candidate.format = desc.BufferDesc.Format;
        candidate.owner = queue.owner;
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
  InitScope(uint32_t slot, HMODULE module, void* handle, IDXGIFactory2* factory, HWND window, IDXGISwapChain* application_swapchain) noexcept {
    previous = init_thread_binding;
    owner = previous.serial == 0 || previous.slot != slot || previous.handle != reinterpret_cast<uintptr_t>(handle);
    if (!owner) {
      serial = previous.serial;
      return;
    }
    Observe([&] {
      RestoreForHandle(slot, reinterpret_cast<uintptr_t>(handle));
      std::wstring path(32768, L'\0');
      const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
      std::string utf8;
      if (length != 0 && length < path.size()) {
        const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        if (bytes > 0) {
          utf8.resize(bytes);
          WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(length), utf8.data(), bytes, nullptr, nullptr);
        }
      }
      serial = BeginInit({.slot = slot, .module = reinterpret_cast<uintptr_t>(module), .handle = reinterpret_cast<uintptr_t>(handle),
          .factory = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(factory)), .window = reinterpret_cast<uintptr_t>(window),
          .application_swapchain = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(application_swapchain)),
          .module_path = std::move(utf8), .debug = reinterpret_cast<DebugFn>(GetProcAddress(module, "xefgSwapChainEnableDebugFeature"))});
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
  CancelCopy cancel = nullptr;
  {
    std::unique_lock lock(probe_state.mutex);
    if (ControlBusy(control_job)) {
      control_job.cancel_requested = true;
    }
    calibration.active = false;
    calibration.cancelled = true;
    calibration.reason = "stopped_without_synchronous_restore";
    cancel = capture_callbacks.cancel;
  }
  if (cancel != nullptr) {
    cancel("probe_stopped");
  }
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
