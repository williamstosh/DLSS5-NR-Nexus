/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
#include <array>
#include <cstdint>
#include <mutex>
#include <shared_mutex>

namespace renodx::addons::dlss5::fg_output_probe {

// ABI facts from Intel's inc/xess_fg/xefg_swapchain_debug.h. The SDK header
// and runtime are not redistributed by this validation-only interop declaration.
enum class XefgDebugFeature : int32_t { SHOW_ONLY_INTERPOLATION = 0, TAG_INTERPOLATED_FRAMES = 1 };
using EnableXefgDebug = int32_t (*)(void*, XefgDebugFeature, uint32_t, void*);
static_assert(sizeof(XefgDebugFeature) == 4);

struct NativeReceipt {
  uint64_t observed_sequence = 0;
  uint64_t after_success_generation = 0;
  bool observed = false;
  bool active = false;
  bool stopped = false;
  bool version_supported = false;
  bool flag_forwarded = false;
  const char* reason = "unobserved";
  uint32_t viewport = 0;
  uint64_t struct_version = 0;
  uint32_t mode = 0;
  uint32_t num_frames = 0;
  uint32_t original_flags = 0;
  uint32_t forwarded_flags = 0;
  int32_t result = 0;
};
struct XefgReceipt {
  uint64_t observed_sequence = 0;
  bool observed = false;
  bool registered = false;
  bool stopped = false;
  bool init_result_available = false;
  int32_t init_result = 0;
  const char* reason = "unobserved";
  uint32_t sdk_slot = 0;
  uintptr_t module_base = 0;
  uintptr_t sdk_context = 0;
  uintptr_t context_identity = 0;
  uint64_t init_epoch = 0;
  uint64_t init_generation = 0;
  uint64_t current_epoch = 0;
  uint64_t current_generation = 0;
  uintptr_t debug_export = 0;
  bool show_called = false;
  int32_t show_result = 0;
  bool tag_called = false;
  int32_t tag_result = 0;
  bool debug_applied = false;
  bool destroy_called = false;
  int32_t destroy_result = 0;
};
struct Snapshot {
  bool native_stopped = false;
  bool xefg_stopped = false;
  uint64_t sequence = 0;
  uint64_t overflow = 0;
  NativeReceipt native;
  std::array<XefgReceipt, 8> xefg{};
};
struct ProbeState {
  std::shared_mutex mutex;
  Snapshot snapshot;
};
static ProbeState probe_state;
// InitDesc may delegate to Init in the same SDK. This guard serializes our
// complete Init/debug-call/Destroy turns, including such same-thread nesting.
static std::recursive_mutex xefg_lifecycle_mutex;
struct InitBinding {
  uint32_t slot = 0;
  uintptr_t handle = 0;
  bool active = false;
};
static thread_local InitBinding init_binding;
struct XefgInitScope {
  InitBinding previous;
  bool owner = false;
  XefgInitScope(uint32_t slot, uintptr_t handle) {
    owner = !init_binding.active || init_binding.slot != slot || init_binding.handle != handle;
    if (owner) {
      previous = init_binding;
      init_binding = {.slot = slot, .handle = handle, .active = true};
    }
  }
  ~XefgInitScope() {
    if (owner) {
      init_binding = previous;
    }
  }
  XefgInitScope(const XefgInitScope&) = delete;
  XefgInitScope& operator=(const XefgInitScope&) = delete;
};

inline void RecordNative(NativeReceipt receipt, bool success) {
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  receipt.observed_sequence = ++snapshot.sequence;
  receipt.after_success_generation = snapshot.native.after_success_generation + static_cast<uint64_t>(success);
  receipt.observed = true;
  receipt.stopped = receipt.stopped || snapshot.native_stopped;
  receipt.active = success && receipt.version_supported && receipt.flag_forwarded && !receipt.stopped;
  if (receipt.stopped) {
    receipt.reason = "stopped";
  }
  snapshot.native = receipt;
}

inline void StopNative() {
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  snapshot.native_stopped = true;
  snapshot.native.active = false;
  snapshot.native.stopped = true;
  snapshot.native.reason = "stopped";
}

inline void BeginXefg(uint32_t slot, uintptr_t module, uintptr_t handle) {
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  XefgReceipt* selected = nullptr;
  for (auto& context : snapshot.xefg) {
    if (context.observed && context.sdk_slot == slot && context.sdk_context == handle) {
      selected = &context;
      break;
    }
  }
  if (selected == nullptr) {
    for (auto& context : snapshot.xefg) {
      if (!context.observed) {
        selected = &context;
        break;
      }
    }
  }
  if (selected == nullptr) {
    for (auto& context : snapshot.xefg) {
      if (!context.registered && (context.destroy_called || context.init_result_available)) {
        selected = &context;
        break;
      }
    }
  }
  if (selected == nullptr) {
    ++snapshot.overflow;
    return;
  }
  *selected = {.observed_sequence = ++snapshot.sequence, .observed = true,
      .stopped = snapshot.xefg_stopped, .reason = (snapshot.xefg_stopped ? "stopped" : "initializing"), .sdk_slot = slot,
      .module_base = module, .sdk_context = handle};
}

// Caller owns xefg_lifecycle_mutex through the successful real Init and both
// public SDK calls. Neither function pointers nor callable handles are retained.
inline void ApplyXefg(XefgReceipt receipt, EnableXefgDebug enable_debug) {
  bool may_call = false;
  {
    std::shared_lock lock(probe_state.mutex);
    for (const auto& context : probe_state.snapshot.xefg) {
      if (context.observed && context.sdk_slot == receipt.sdk_slot && context.sdk_context == receipt.sdk_context) {
        may_call = !probe_state.snapshot.xefg_stopped;
        break;
      }
    }
  }
  if (!may_call) return;
  receipt.observed = true;
  receipt.registered = true;
  receipt.init_result_available = true;
  receipt.init_result = 0;
  receipt.debug_export = reinterpret_cast<uintptr_t>(enable_debug);
  if (enable_debug != nullptr) {
    receipt.show_called = true;
    receipt.show_result = enable_debug(reinterpret_cast<void*>(receipt.sdk_context),
        XefgDebugFeature::SHOW_ONLY_INTERPOLATION, 1, nullptr);
    {
      std::shared_lock lock(probe_state.mutex);
      may_call = !probe_state.snapshot.xefg_stopped;
    }
    if (may_call) {
      receipt.tag_called = true;
      receipt.tag_result = enable_debug(reinterpret_cast<void*>(receipt.sdk_context),
          XefgDebugFeature::TAG_INTERPOLATED_FRAMES, 1, nullptr);
    }
  }
  receipt.debug_applied = receipt.show_called && receipt.show_result == 0
      && receipt.tag_called && receipt.tag_result == 0;
  receipt.reason = (enable_debug == nullptr ? "debug_export_unavailable"
      : (receipt.debug_applied ? "debug_features_applied" : "debug_feature_failed"));
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  for (auto& context : snapshot.xefg) {
    if (context.observed && context.sdk_slot == receipt.sdk_slot && context.sdk_context == receipt.sdk_context) {
      receipt.observed_sequence = ++snapshot.sequence;
      receipt.stopped = snapshot.xefg_stopped;
      if (receipt.stopped) {
        receipt.reason = "stopped";
        receipt.debug_applied = false;
      }
      context = receipt;
      return;
    }
  }
}

inline void FinishXefgInit(uint32_t slot, uintptr_t handle, int32_t result) {
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  for (auto& context : snapshot.xefg) {
    if (context.observed && context.sdk_slot == slot && context.sdk_context == handle) {
      context.init_result_available = true;
      context.init_result = result;
      if (snapshot.xefg_stopped) {
        context.debug_applied = false;
        context.reason = "stopped";
      } else if (result != 0 || !context.registered) {
        context.debug_applied = false;
        context.reason = (result != 0 ? "init_failed" : "context_not_registered");
      }
      return;
    }
  }
}

inline void UpdateXefg(uintptr_t context_identity, uint64_t epoch, uint64_t generation) {
  std::unique_lock lock(probe_state.mutex);
  for (auto& context : probe_state.snapshot.xefg) {
    if (context.registered && context.context_identity == context_identity) {
      context.current_epoch = epoch;
      context.current_generation = generation;
    }
  }
}

inline void DestroyXefg(uint32_t slot, uintptr_t handle, int32_t result) {
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  for (auto& context : snapshot.xefg) {
    if (context.observed && context.sdk_slot == slot && context.sdk_context == handle) {
      context.observed_sequence = ++snapshot.sequence;
      context.destroy_called = true;
      context.destroy_result = result;
      context.debug_applied = false;
      context.reason = (result == 0 ? "destroyed" : "destroy_failed");
      if (result == 0) {
        context.registered = false;
      }
    }
  }
}

inline void StopXefg() {
  std::unique_lock lock(probe_state.mutex);
  auto& snapshot = probe_state.snapshot;
  snapshot.xefg_stopped = true;
  for (auto& context : snapshot.xefg) {
    if (context.observed) {
      context.stopped = true;
      context.debug_applied = false;
      context.reason = "stopped";
    }
  }
}

inline Snapshot CopySnapshot() {
  std::shared_lock lock(probe_state.mutex);
  return probe_state.snapshot;
}

}  // namespace renodx::addons::dlss5::fg_output_probe
#endif
