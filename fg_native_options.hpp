/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <windows.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>

#include <sl_dlss_g.h>

#include "detour_guard.hpp"
#include "fg_frame.hpp"
#include "lastgasp.hpp"
#include "output_framerate.hpp"
#if defined(NR_NEXUS_DEV_CONTROL)
#include "../../utils/directx.hpp"
#include "native_fg_frame.hpp"
#endif
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
#include <cstring>

#include "nr_fg_output_probe.hpp"
#endif

namespace renodx::addons::dlss5::fg::native_options {

// These phases describe the SDK's routing decision, not COM completion.
enum class PresentPhase { SDK_BEGIN, SDK_FORWARD, SDK_HANDLED, SDK_FAILED };

namespace internal {
using OptionsChanged = void (*)(bool enabled) noexcept;
using PresentObserved = void (*)(IDXGISwapChain*, UINT, PresentPhase) noexcept;
using Present = HRESULT (*)(IDXGISwapChain*, UINT, UINT, bool&);
using Present1 = HRESULT (*)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*, bool&);
static std::atomic<OptionsChanged> options_changed{nullptr};
static std::atomic<PresentObserved> present_observed{nullptr};
static std::atomic_bool stopped{false};
static std::atomic_bool decisions_ready{false};
static thread_local uint32_t present_depth = 0;

struct State {
  std::shared_mutex mutex;
  PFun_slDLSSGSetOptions* real = nullptr;
  PFun_slDLSSGGetState* get_state = nullptr;
  detour_guard::Record record;
  detour_guard::Record get_state_record;
  Present present = nullptr;
  Present1 present1 = nullptr;
  detour_guard::Record present_record;
  detour_guard::Record present1_record;
  bool installed = false;
};
static State state;

#if defined(NR_NEXUS_DEV_CONTROL)
struct ReflexMarkerHook {
  HMODULE module = nullptr;
  frame_source::SetMarker real = nullptr;
  detour_guard::Record record;
  bool installed = false;
};
static ReflexMarkerHook reflex_marker;

static sl::Result HookedReflexMarker(sl::PCLMarker marker, const sl::FrameToken& frame) {
  if (!stopped.load(std::memory_order_acquire)) PinRuntimeCallback();
  return frame_source::ObserveMarker(reflex_marker.real, marker, frame);
}
#endif

struct PresentDecisionScope {
  IDXGISwapChain* swapchain;
  UINT flags;
  PresentObserved callback = nullptr;
#if defined(NR_NEXUS_DEV_CONTROL)
  frame_source::SdkScope frame_scope;
#endif

  PresentDecisionScope(IDXGISwapChain* output, UINT present_flags) : swapchain(output), flags(present_flags)
#if defined(NR_NEXUS_DEV_CONTROL)
      , frame_scope((!stopped.load(std::memory_order_acquire) && (present_flags & DXGI_PRESENT_TEST) == 0
                        ? reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(output)) : 0),
            !stopped.load(std::memory_order_acquire) && (present_flags & DXGI_PRESENT_TEST) == 0)
#endif
  {
    if (present_depth == 0 && (flags & DXGI_PRESENT_TEST) != 0) {
      output_framerate::Invalidate(output_framerate::Reason::kTEST_PRESENT);
    }
    if (present_depth++ == 0 && !stopped.load(std::memory_order_acquire) && (flags & DXGI_PRESENT_TEST) == 0) {
      callback = present_observed.load(std::memory_order_acquire);
      if (callback != nullptr) {
        callback(swapchain, flags, PresentPhase::SDK_BEGIN);
      }
    }
  }
  void Publish(HRESULT result, bool skip) const {
    if (callback != nullptr) {
      output_framerate::ObserveNativePresent(reinterpret_cast<uintptr_t>(swapchain),
          result == S_OK, output_framerate::NowNs());
      callback(swapchain, flags, FAILED(result) ? PresentPhase::SDK_FAILED
          : (skip ? PresentPhase::SDK_HANDLED : PresentPhase::SDK_FORWARD));
    }
  }
  ~PresentDecisionScope() { --present_depth; }
};

#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
inline size_t ProbeOptionsBytes(uint64_t version) {
  // sl_dlss_g.h labels the last member added by each version. Copy only
  // through that member, never sizeof(the current type) from an older object.
  using Options = sl::DLSSGOptions;
  switch (version) {
    case 1: return offsetof(Options, onErrorCallback) + sizeof(Options::onErrorCallback);
    case 2: return offsetof(Options, bReserved15) + sizeof(Options::bReserved15);
    case 3: return offsetof(Options, queueParallelismMode) + sizeof(Options::queueParallelismMode);
    case 4: return offsetof(Options, enableUserInterfaceRecomposition) + sizeof(Options::enableUserInterfaceRecomposition);
    case 5: return offsetof(Options, dynamicTargetFrameRate) + sizeof(Options::dynamicTargetFrameRate);
    default: return 0;
  }
}

inline sl::Result ProbeSetOptions(PFun_slDLSSGSetOptions* real, const sl::ViewportHandle& viewport,
                                  const sl::DLSSGOptions& options, bool stopped) {
  const auto bytes = ProbeOptionsBytes(options.structVersion);
  fg_output_probe::NativeReceipt receipt{.stopped = stopped, .version_supported = bytes != 0,
      .viewport = static_cast<uint32_t>(viewport), .struct_version = options.structVersion};
  sl::DLSSGOptions copy;
  const auto* forwarded = &options;
  if (bytes != 0) {
    receipt.mode = static_cast<uint32_t>(options.mode);
    receipt.num_frames = options.numFramesToGenerate;
    receipt.original_flags = static_cast<uint32_t>(options.flags);
    receipt.forwarded_flags = receipt.original_flags;
    if (!stopped && options.mode == sl::DLSSGMode::eOn) {
      std::memcpy(&copy, &options, bytes);
      copy.flags |= sl::DLSSGFlags::eShowOnlyInterpolatedFrame;
      forwarded = &copy;
      receipt.flag_forwarded = true;
      receipt.forwarded_flags = static_cast<uint32_t>(copy.flags);
    }
  }
  const auto result = real(viewport, *forwarded);
  receipt.result = static_cast<int32_t>(result);
  receipt.reason = (stopped ? "stopped" : bytes == 0 ? "struct_version_not_supported"
      : !receipt.flag_forwarded ? "mode_not_on" : result != sl::Result::eOk ? "sdk_failed" : "show_only_forwarded");
  fg_output_probe::RecordNative(receipt, result == sl::Result::eOk);
  return result;
}
#endif

static sl::Result HookedSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options) {
  if (!stopped.load(std::memory_order_acquire)) PinRuntimeCallback();
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
  const auto result = ProbeSetOptions(state.real, viewport, options, stopped.load(std::memory_order_acquire));
#else
  const auto result = state.real(viewport, options);
#endif
  // Enabled modes clear an explicit Off request; actual NGX evaluation
  // remains the evidence required to acquire native processing ownership.
  if (result == sl::Result::eOk && !stopped.load(std::memory_order_acquire)) {
    output_framerate::ObserveNativeOptions(static_cast<uint32_t>(viewport), options.structVersion,
        static_cast<uint32_t>(options.mode), options.numFramesToGenerate + 1);
    if (const auto callback = options_changed.load(std::memory_order_acquire); callback != nullptr) {
      callback(options.mode != sl::DLSSGMode::eOff);
    }
  }
  return result;
}

static sl::Result HookedGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& output,
                                 const sl::DLSSGOptions* options) {
  if (!stopped.load(std::memory_order_acquire)) PinRuntimeCallback();
  const auto result = state.get_state(viewport, output, options);
  // Observe the application's existing call. An extra call would consume the
  // SDK's since-last-call counter and violate its non-thread-safe contract.
  if (!stopped.load(std::memory_order_acquire)) {
    output_framerate::ObserveNativeState(static_cast<uint32_t>(viewport),
        result == sl::Result::eOk && output.status == sl::DLSSGStatus::eOk,
        (result == sl::Result::eOk ? output.numFramesActuallyPresented : 0), output_framerate::NowNs());
  }
  return result;
}

static HRESULT HookedPresent(IDXGISwapChain* swapchain, UINT interval, UINT flags, bool& skip) {
  if (!stopped.load(std::memory_order_acquire)) PinRuntimeCallback();
  const PresentDecisionScope scope(swapchain, flags);
  const HRESULT result = state.present(swapchain, interval, flags, skip);
  scope.Publish(result, skip);
  return result;
}

static HRESULT HookedPresent1(IDXGISwapChain* swapchain, UINT interval, UINT flags,
                              const DXGI_PRESENT_PARAMETERS* parameters, bool& skip) {
  if (!stopped.load(std::memory_order_acquire)) PinRuntimeCallback();
  const PresentDecisionScope scope(swapchain, flags);
  const HRESULT result = state.present1(swapchain, interval, flags, parameters, skip);
  scope.Publish(result, skip);
  return result;
}
}  // namespace internal

inline void Configure(void (*callback)(bool enabled) noexcept,
    void (*present)(IDXGISwapChain*, UINT, PresentPhase) noexcept = nullptr) {
  internal::options_changed.store(callback, std::memory_order_release);
  internal::present_observed.store(present, std::memory_order_release);
}

inline bool ObservesPresentDecisions() noexcept {
  return internal::decisions_ready.load(std::memory_order_acquire)
      && !internal::stopped.load(std::memory_order_acquire);
}

#if defined(NR_NEXUS_DEV_CONTROL)
inline frame_source::Frame CurrentFrame(uintptr_t output_identity) {
  return frame_source::CurrentFrame(output_identity);
}
#endif

// Terminal logical disable; it does not drain an already executing callback.
// Both images remain pinned and the installed hook continues to pass through.
inline void Stop() noexcept {
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
  fg_output_probe::StopNative();
#endif
  output_framerate::ReleaseNative();
  internal::stopped.store(true, std::memory_order_release);
#if defined(NR_NEXUS_DEV_CONTROL)
  frame_source::Stop();
#endif
  internal::options_changed.store(nullptr, std::memory_order_release);
  internal::present_observed.store(nullptr, std::memory_order_release);
  internal::decisions_ready.store(false, std::memory_order_release);
  if (internal::state.installed) {
    detour_guard::Neutralize(reinterpret_cast<void* const*>(&internal::state.real),
        reinterpret_cast<void*>(&internal::HookedSetOptions), internal::state.record);
    if (internal::state.get_state != nullptr) {
      detour_guard::Neutralize(reinterpret_cast<void* const*>(&internal::state.get_state),
          reinterpret_cast<void*>(&internal::HookedGetState), internal::state.get_state_record);
    }
    if (internal::state.present != nullptr) {
      detour_guard::Neutralize(reinterpret_cast<void* const*>(&internal::state.present),
          reinterpret_cast<void*>(&internal::HookedPresent), internal::state.present_record);
    }
    if (internal::state.present1 != nullptr) {
      detour_guard::Neutralize(reinterpret_cast<void* const*>(&internal::state.present1),
          reinterpret_cast<void*>(&internal::HookedPresent1), internal::state.present1_record);
    }
  }
#if defined(NR_NEXUS_DEV_CONTROL)
  if (internal::reflex_marker.installed) {
    detour_guard::Neutralize(reinterpret_cast<void* const*>(&internal::reflex_marker.real),
        reinterpret_cast<void*>(&internal::HookedReflexMarker), internal::reflex_marker.record);
  }
#endif
}

// Call outside DllMain and without the host's runtime lock: discovery takes
// the loader lock, and AttachAll owns the existing Detours transaction lock.
inline void Poll() {
  if (internal::stopped.load(std::memory_order_acquire)
      || internal::options_changed.load(std::memory_order_acquire) == nullptr) return;
  std::unique_lock lock(internal::state.mutex, std::try_to_lock);
#if defined(NR_NEXUS_DEV_CONTROL)
  if (!lock.owns_lock() || (internal::state.installed && internal::reflex_marker.installed)
      || internal::stopped.load(std::memory_order_acquire)) return;
#else
  if (!lock.owns_lock() || internal::state.installed
      || internal::stopped.load(std::memory_order_acquire)) return;
#endif

  HMODULE plugin = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"sl.dlss_g.dll", &plugin)) return;
  const auto image_size = detour_guard::ImageIdentity(plugin) & 0xFFFFFFFFu;
  if (image_size == 0) return;
  using Gateway = void* (*)(const char*);
  const auto gateway = reinterpret_cast<Gateway>(GetProcAddress(plugin, "slGetPluginFunction"));
  if (gateway == nullptr
      || reinterpret_cast<uintptr_t>(gateway) - reinterpret_cast<uintptr_t>(plugin) >= image_size) return;

  // Keep SEH in a scope without C++ objects requiring unwinding (MSVC C2712).
  const auto resolve = [](Gateway entry, const char* name) -> void* {
    ++lastgasp::expected_fault_depth;
    __try {
      __try {
        return entry(name);
      } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                      ? EXCEPTION_EXECUTE_HANDLER
                      : EXCEPTION_CONTINUE_SEARCH) {
        return nullptr;
      }
    } __finally {
      --lastgasp::expected_fault_depth;
    }
  };
#if defined(NR_NEXUS_DEV_CONTROL)
  if (!internal::reflex_marker.installed) {
    HMODULE reflex = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"sl.reflex.dll", &reflex)) {
      const auto reflex_size = detour_guard::ImageIdentity(reflex) & 0xFFFFFFFFu;
      const auto reflex_gateway = reinterpret_cast<Gateway>(GetProcAddress(reflex, "slGetPluginFunction"));
      if (reflex_size != 0 && reflex_gateway != nullptr
          && reinterpret_cast<uintptr_t>(reflex_gateway) - reinterpret_cast<uintptr_t>(reflex) < reflex_size) {
        void* const marker = resolve(reflex_gateway, "slReflexSetMarker");
        if (marker != nullptr && reinterpret_cast<uintptr_t>(marker) - reinterpret_cast<uintptr_t>(reflex) < reflex_size
            && !internal::stopped.load(std::memory_order_acquire)) {
          internal::reflex_marker.module = reflex;
          internal::reflex_marker.real = reinterpret_cast<frame_source::SetMarker>(marker);
          const detour_guard::Hook hooks[] = {{
              .name = "slReflexSetMarker", .real = reinterpret_cast<void**>(&internal::reflex_marker.real),
              .detour = reinterpret_cast<void*>(&internal::HookedReflexMarker), .record = &internal::reflex_marker.record}};
          size_t failed = 0;
          internal::reflex_marker.installed = detour_guard::AttachAll(hooks, &failed) == 1;
          if (internal::reflex_marker.installed) {
            frame_source::Activate();
            if (internal::stopped.load(std::memory_order_acquire)) {
              detour_guard::Neutralize(reinterpret_cast<void* const*>(&internal::reflex_marker.real),
                  reinterpret_cast<void*>(&internal::HookedReflexMarker), internal::reflex_marker.record);
            }
          }
        }
      }
    }
  }
  if (internal::state.installed) return;
#endif
  void* const target = resolve(gateway, "slDLSSGSetOptions");
  if (target == nullptr
      || reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(plugin) >= image_size) return;

  if (internal::stopped.load(std::memory_order_acquire)) return;

  internal::state.real = reinterpret_cast<PFun_slDLSSGSetOptions*>(target);
  void* const get_state = resolve(gateway, "slDLSSGGetState");
  if (get_state != nullptr
      && reinterpret_cast<uintptr_t>(get_state) - reinterpret_cast<uintptr_t>(plugin) < image_size) {
    internal::state.get_state = reinterpret_cast<PFun_slDLSSGGetState*>(get_state);
  }
  void* const present = resolve(gateway, "slHookPresent");
  void* const present1 = resolve(gateway, "slHookPresent1");
  const bool has_decisions = present != nullptr && present1 != nullptr
      && reinterpret_cast<uintptr_t>(present) - reinterpret_cast<uintptr_t>(plugin) < image_size
      && reinterpret_cast<uintptr_t>(present1) - reinterpret_cast<uintptr_t>(plugin) < image_size;
  if (has_decisions) {
    internal::state.present = reinterpret_cast<internal::Present>(present);
    internal::state.present1 = reinterpret_cast<internal::Present1>(present1);
  }
  const detour_guard::Hook hooks[] = {{
      .name = "slDLSSGSetOptions",
      .real = reinterpret_cast<void**>(&internal::state.real),
      .detour = reinterpret_cast<void*>(&internal::HookedSetOptions),
      .record = &internal::state.record,
  }};
  size_t failed = 0;
  internal::state.installed = detour_guard::AttachAll(hooks, &failed) == 1;
  if (internal::state.installed && internal::state.get_state != nullptr) {
    const detour_guard::Hook state_hooks[] = {{
        .name = "slDLSSGGetState", .real = reinterpret_cast<void**>(&internal::state.get_state),
        .detour = reinterpret_cast<void*>(&internal::HookedGetState), .record = &internal::state.get_state_record,
    }};
    detour_guard::AttachAll(state_hooks, &failed);
  }
  if (internal::state.installed && has_decisions) {
    const detour_guard::Hook present_hooks[] = {
        {"slHookPresent", reinterpret_cast<void**>(&internal::state.present),
            reinterpret_cast<void*>(&internal::HookedPresent), &internal::state.present_record},
        {"slHookPresent1", reinterpret_cast<void**>(&internal::state.present1),
            reinterpret_cast<void*>(&internal::HookedPresent1), &internal::state.present1_record},
    };
    internal::decisions_ready.store(detour_guard::AttachAll(present_hooks, &failed) == 2,
                                    std::memory_order_release);
  }
}

}  // namespace renodx::addons::dlss5::fg::native_options
