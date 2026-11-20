/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>

#include <sl_dlss_g.h>

#include "../../utils/directx.hpp"
#include "detour_guard.hpp"
#include "fg_frame.hpp"
#include "lastgasp.hpp"
#include "output_framerate.hpp"
#include "native_fg_frame.hpp"
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
static thread_local std::optional<output_framerate::OutputIdentity> options_output;
#if defined(NR_NEXUS_TEST_NATIVE_SDK_CLOCK)
static thread_local int64_t test_now_ns = 0;
#endif

inline int64_t NowNs() {
#if defined(NR_NEXUS_TEST_NATIVE_SDK_CLOCK)
  if (test_now_ns != 0) return test_now_ns;
#endif
  return output_framerate::NowNs();
}

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
// Installation facts are separate from observed calls. These bits never grant
// a frame; the existing bounded source log uses them to explain a missing scope.
enum FrameHook : uint32_t {
  OPTIONS_ATTACHED = 1u << 0,
  REFLEX_ATTACHED = 1u << 1,
  PRESENT_RESOLVED = 1u << 2,
  PRESENT1_RESOLVED = 1u << 3,
  PRESENT_ATTACHED = 1u << 4,
  PRESENT1_ATTACHED = 1u << 5,
};
static std::atomic_uint32_t frame_hooks{0};
#endif

struct ReflexMarkerHook {
  HMODULE module = nullptr;
  frame_source::SetMarker real = nullptr;
  detour_guard::Record record;
  bool installed = false;
};
static ReflexMarkerHook reflex_marker;

static sl::Result HookedReflexMarker(sl::PCLMarker marker, const sl::FrameToken& frame) {
  if (stopped.load(std::memory_order_acquire)) return reflex_marker.real(marker, frame);
  PinRuntimeCallback();
  return frame_source::ObserveMarker(reflex_marker.real, marker, frame, NowNs);
}

static const detour_guard::Hook REFLEX_MARKER_HOOK[] = {{
    .name = "slReflexSetMarker", .real = reinterpret_cast<void**>(&reflex_marker.real),
    .detour = reinterpret_cast<void*>(&HookedReflexMarker), .record = &reflex_marker.record,
}};

struct PresentDecisionScope {
  static inline thread_local const PresentDecisionScope* active = nullptr;
  static inline thread_local const PresentDecisionScope* publishing = nullptr;

  IDXGISwapChain* swapchain;
  UINT flags;
  PresentObserved callback = nullptr;
  output_framerate::OutputIdentity output_identity;
  uintptr_t native_swapchain = 0;
  uint64_t binding_generation = 0;
  uintptr_t sdk_back_buffer = 0;
  uintptr_t application_back_buffer = 0;
  bool application_tags_conflicting = false;
  std::optional<output_framerate::NativeApplicationFrame> application_frame;
  const PresentDecisionScope* previous = active;
  std::optional<frame_source::SdkScope> frame_scope;

  PresentDecisionScope(IDXGISwapChain* output, UINT present_flags) : swapchain(output), flags(present_flags) {
    const bool observing = !stopped.load(std::memory_order_acquire) && (flags & DXGI_PRESENT_TEST) == 0;
    if (observing) {
      native_swapchain = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(swapchain));
    }
    const bool outermost = present_depth++ == 0;
    if (outermost && observing) {
      output_identity = output_framerate::MatchNativeSwapchain(native_swapchain);
      binding_generation = output_framerate::BeginNativeOutputPresent(output_identity);
    }
    frame_scope.emplace(native_swapchain, observing && (!outermost || output_identity), NowNs());
    if (outermost && observing) {
      const bool has_tags = output_framerate::HasNativeApplicationTags(output_identity);
      const bool has_marker = output_identity && frame_source::CurrentFrame(native_swapchain, NowNs()).available;
      if (has_tags || has_marker) {
        Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain3;
        Microsoft::WRL::ComPtr<IUnknown> back_buffer;
        if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3)))
            && SUCCEEDED(swapchain3->GetBuffer(swapchain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back_buffer)))) {
          sdk_back_buffer = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(back_buffer.Get()));
          if (has_tags) {
            application_back_buffer = sdk_back_buffer;
            application_frame = output_framerate::TakeNativeApplicationFrame(output_identity, binding_generation,
                application_back_buffer, NowNs(), &application_tags_conflicting);
          }
        }
      }
      callback = present_observed.load(std::memory_order_acquire);
      if (callback != nullptr) {
        callback(swapchain, flags, PresentPhase::SDK_BEGIN);
      }
    }
    active = (!outermost && observing && previous != nullptr && previous->native_swapchain == native_swapchain
        ? previous : this);
  }
  void Publish(HRESULT result, bool skip) const {
    active = previous;
    if (output_identity && !stopped.load(std::memory_order_acquire)) {
      output_framerate::ObserveNativePresent(output_identity, result == S_OK, NowNs(),
          skip, binding_generation, application_frame);
    }
    if (callback != nullptr) {
      const auto* outer = std::exchange(publishing, this);
      callback(swapchain, flags, FAILED(result) ? PresentPhase::SDK_FAILED
          : (skip ? PresentPhase::SDK_HANDLED : PresentPhase::SDK_FORWARD));
      publishing = outer;
    }
  }
  ~PresentDecisionScope() {
    active = previous;
    --present_depth;
  }
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
  const auto entered_output = output_framerate::NativeViewportOutput(static_cast<uint32_t>(viewport));
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
  const auto result = ProbeSetOptions(state.real, viewport, options, stopped.load(std::memory_order_acquire));
#else
  const auto result = state.real(viewport, options);
#endif
  // Enabled modes clear an explicit Off request; actual NGX evaluation
  // remains the evidence required to acquire native processing ownership.
  if (result == sl::Result::eOk && !stopped.load(std::memory_order_acquire)
      && options.structVersion >= sl::kStructVersion1) {
    output_framerate::ObserveNativeOptions(static_cast<uint32_t>(viewport), options.structVersion,
        static_cast<uint32_t>(options.mode),
        options.numFramesToGenerate < UINT32_MAX ? options.numFramesToGenerate + 1 : 0, entered_output);
    if (const auto callback = options_changed.load(std::memory_order_acquire); callback != nullptr) {
      const auto previous_output = std::exchange(options_output, entered_output);
      callback(options.mode != sl::DLSSGMode::eOff);
      options_output = previous_output;
    }
  }
  return result;
}

static sl::Result HookedGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& output,
                                 const sl::DLSSGOptions* options) {
  if (!stopped.load(std::memory_order_acquire)) PinRuntimeCallback();
  const auto query = (!stopped.load(std::memory_order_acquire)
      ? output_framerate::BeginNativeState(static_cast<uint32_t>(viewport)) : output_framerate::NativeStateQuery{});
  const auto result = state.get_state(viewport, output, options);
  // Observe the application's existing call. An extra call would consume the
  // SDK's since-last-call counter and violate its non-thread-safe contract.
  if (!stopped.load(std::memory_order_acquire)) {
    const bool valid = result == sl::Result::eOk && output.structVersion >= sl::kStructVersion1;
    output_framerate::ObserveNativeState(query, valid && output.status == sl::DLSSGStatus::eOk,
        valid ? output.numFramesActuallyPresented : 0, NowNs());
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

static const detour_guard::Hook PRESENT_HOOKS[] = {
    {"slHookPresent", reinterpret_cast<void**>(&state.present),
        reinterpret_cast<void*>(&HookedPresent), &state.present_record},
    {"slHookPresent1", reinterpret_cast<void**>(&state.present1),
        reinterpret_cast<void*>(&HookedPresent1), &state.present1_record},
};
}  // namespace internal

inline std::optional<output_framerate::OutputIdentity> ChangedOutput() noexcept {
  return internal::options_output;
}

inline void Configure(void (*callback)(bool enabled) noexcept,
    void (*present)(IDXGISwapChain*, UINT, PresentPhase) noexcept = nullptr) {
  internal::options_changed.store(callback, std::memory_order_release);
  internal::present_observed.store(present, std::memory_order_release);
}

inline bool ObservesPresentDecisions() noexcept {
  return internal::decisions_ready.load(std::memory_order_acquire)
      && !internal::stopped.load(std::memory_order_acquire);
}

inline frame_source::Frame CurrentPresentFrame(const void* output, ID3D12Resource* back_buffer) {
  const auto* present = internal::PresentDecisionScope::active;
  if (present == nullptr || !present->output_identity || present->sdk_back_buffer == 0
      || internal::stopped.load(std::memory_order_acquire)) {
    return {.reason = "sdk_present_frame_unavailable"};
  }
  if (reinterpret_cast<uintptr_t>(output) != present->native_swapchain
      || reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(back_buffer))
          != present->sdk_back_buffer
      || output_framerate::MatchPresentOutput(present->output_identity.swapchain,
             present->sdk_back_buffer) != present->output_identity
      || output_framerate::NativeBindingGeneration(present->output_identity) != present->binding_generation) {
    return {.reason = "sdk_present_frame_changed"};
  }
  auto marker = frame_source::CurrentFrame(present->native_swapchain, internal::NowNs());
  if (!marker.available) return marker;
  if (present->application_tags_conflicting
      || (present->application_frame && present->application_frame->index != marker.frame)) {
    return {.reason = "sdk_present_start_conflicts_with_backbuffer_tag"};
  }
  // A game can reach the registered rotating backbuffer without a Streamline
  // viewport tag. The live output generation then identifies this lifetime.
  marker.generation = (present->binding_generation != 0 ? present->binding_generation : present->output_identity.generation);
  return marker;
}

inline bool InSdkPresent() noexcept { return internal::present_depth != 0; }

// The successful Streamline tag call is the viewport-to-output evidence.
// Look up the tagged native pointer as a recorded backbuffer before using it.
inline void ObserveBackBufferTags(uint32_t viewport, const sl::ResourceTag* tags, uint32_t count,
                                   std::optional<uint32_t> frame = std::nullopt,
                                   uint64_t resource_generation = 0) {
  if (tags == nullptr) return;
  output_framerate::OutputIdentity output;
  uintptr_t tagged_source = 0;
  uintptr_t tagged_back_buffer = 0;
  bool source_ambiguous = false;
  bool back_buffer_ambiguous = false;
  bool valid_until_present = true;
  bool scaling_output_tagged = false;
  bool back_buffer_has_no_resource = false;
  bool legacy_back_buffer_valid = true;
  const auto valid_persistent_resource = [](sl::ResourceLifecycle lifetime) {
    return lifetime == sl::ResourceLifecycle::eValidUntilPresent
        || lifetime == sl::ResourceLifecycle::eValidUntilEvaluate;
  };
  for (uint32_t index = 0; index < count; ++index) {
    const auto& tag = tags[index];
    if (tag.type == sl::kBufferTypeScalingOutputColor) {
      scaling_output_tagged = true;
      if (valid_persistent_resource(tag.lifecycle) && resource_generation != 0
          && tag.resource != nullptr && tag.resource->native != nullptr
          && tag.resource->type == sl::ResourceType::eTex2d) {
        const auto resource = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(
            reinterpret_cast<IUnknown*>(tag.resource->native)));
        source_ambiguous |= tagged_source != 0 && tagged_source != resource;
        tagged_source = resource;
      } else {
        source_ambiguous = true;
      }
    }
    if (tag.type != sl::kBufferTypeBackbuffer) continue;
    // Streamline accepts extent-only BB tags. Only exact resources can
    // establish a new owner or supply an application's frame token.
    if ((tag.resource != nullptr && tag.resource->native == nullptr)
        || (tag.resource == nullptr && tag.extent)) {
      back_buffer_has_no_resource = true;
      continue;
    }
    if (tag.resource == nullptr || tag.resource->type != sl::ResourceType::eTex2d) {
      output_framerate::ReleaseNativeViewport(viewport, false);
      return;
    }
    const auto back_buffer = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(
        reinterpret_cast<IUnknown*>(tag.resource->native)));
    const auto tagged = output_framerate::FindPresentOutput(0, back_buffer);
    if (!tagged || (output && output != tagged)) {
      output_framerate::ReleaseNativeViewport(viewport, false);
      return;
    }
    output = tagged;
    back_buffer_ambiguous |= tagged_back_buffer != 0 && tagged_back_buffer != back_buffer;
    tagged_back_buffer = back_buffer;
    valid_until_present &= tag.lifecycle == sl::ResourceLifecycle::eValidUntilPresent;
    legacy_back_buffer_valid &= valid_persistent_resource(tag.lifecycle);
  }
  if (back_buffer_has_no_resource && frame) output_framerate::WithdrawNativeApplicationTag(viewport, *frame);
  if (output) {
    output_framerate::BindNativeOutput(output, viewport, frame && !source_ambiguous ? tagged_source : 0, frame,
        {.back_buffer = (back_buffer_ambiguous ? 0 : tagged_back_buffer),
         .index = (valid_until_present && !back_buffer_has_no_resource ? frame : std::nullopt),
         .observed_ns = internal::NowNs()}, resource_generation);
  }
  if (!frame) {
    std::optional<output_framerate::OutputIdentity> legacy_back_buffer;
    if (output) legacy_back_buffer = (legacy_back_buffer_valid && !back_buffer_ambiguous
        ? output : output_framerate::OutputIdentity{});
    output_framerate::ObserveNativeLegacySourceTag(viewport, legacy_back_buffer, scaling_output_tagged,
        source_ambiguous ? 0 : tagged_source, resource_generation);
  }
}

#if defined(NR_NEXUS_DEV_CONTROL)
inline frame_source::Frame CurrentFrame(uintptr_t output_identity) {
  return frame_source::CurrentFrame(output_identity, internal::NowNs());
}

inline uint32_t FrameHookFacts() {
  return internal::frame_hooks.load(std::memory_order_acquire);
}
#endif

// Terminal logical disable; it does not drain an already executing callback.
// Both images remain pinned and the installed hook continues to pass through.
inline void Stop() noexcept {
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
  fg_output_probe::StopNative();
#endif
  internal::stopped.store(true, std::memory_order_release);
  output_framerate::ReleaseNative();
  frame_source::Stop();
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
  if (internal::reflex_marker.installed) {
    detour_guard::Neutralize(reinterpret_cast<void* const*>(&internal::reflex_marker.real),
        reinterpret_cast<void*>(&internal::HookedReflexMarker), internal::reflex_marker.record);
  }
}

// Call outside DllMain and without the host's runtime lock: discovery takes
// the loader lock, and AttachAll owns the existing Detours transaction lock.
inline void Poll() {
  if (internal::stopped.load(std::memory_order_acquire)
      || internal::options_changed.load(std::memory_order_acquire) == nullptr) return;
  std::unique_lock lock(internal::state.mutex, std::try_to_lock);
  if (!lock.owns_lock() || (internal::state.installed && internal::reflex_marker.installed)
      || internal::stopped.load(std::memory_order_acquire)) return;

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
          size_t failed = 0;
          internal::reflex_marker.installed = detour_guard::AttachAll(internal::REFLEX_MARKER_HOOK, &failed) == 1;
          if (internal::reflex_marker.installed) {
#if defined(NR_NEXUS_DEV_CONTROL)
            internal::frame_hooks.fetch_or(internal::REFLEX_ATTACHED, std::memory_order_release);
#endif
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
#if defined(NR_NEXUS_DEV_CONTROL)
  if (present != nullptr && reinterpret_cast<uintptr_t>(present) - reinterpret_cast<uintptr_t>(plugin) < image_size) {
    internal::frame_hooks.fetch_or(internal::PRESENT_RESOLVED, std::memory_order_release);
  }
  if (present1 != nullptr && reinterpret_cast<uintptr_t>(present1) - reinterpret_cast<uintptr_t>(plugin) < image_size) {
    internal::frame_hooks.fetch_or(internal::PRESENT1_RESOLVED, std::memory_order_release);
  }
#endif
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
#if defined(NR_NEXUS_DEV_CONTROL)
  if (internal::state.installed) {
    internal::frame_hooks.fetch_or(internal::OPTIONS_ATTACHED, std::memory_order_release);
  }
#endif
  if (internal::state.installed && internal::state.get_state != nullptr) {
    const detour_guard::Hook state_hooks[] = {{
        .name = "slDLSSGGetState", .real = reinterpret_cast<void**>(&internal::state.get_state),
        .detour = reinterpret_cast<void*>(&internal::HookedGetState), .record = &internal::state.get_state_record,
    }};
    detour_guard::AttachAll(state_hooks, &failed);
  }
  if (internal::state.installed && has_decisions) {
    internal::decisions_ready.store(detour_guard::AttachAll(internal::PRESENT_HOOKS, &failed) == 2,
                                    std::memory_order_release);
#if defined(NR_NEXUS_DEV_CONTROL)
    if (internal::state.present != nullptr) {
      internal::frame_hooks.fetch_or(internal::PRESENT_ATTACHED, std::memory_order_release);
    }
    if (internal::state.present1 != nullptr) {
      internal::frame_hooks.fetch_or(internal::PRESENT1_ATTACHED, std::memory_order_release);
    }
#endif
  }
}

}  // namespace renodx::addons::dlss5::fg::native_options
