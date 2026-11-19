/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#if defined(NR_NEXUS_DEV_CONTROL)
#include <intrin.h>
#endif

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../../utils/directx.hpp"

#include "detour_guard.hpp"
#include "fg_frame.hpp"
#include "output_framerate.hpp"
#include "submission_tracker.hpp"
#if defined(NR_NEXUS_DEV_CONTROL)
#include "nr_edit_fg_observation.hpp"
#endif
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
#include "nr_fg_output_probe.hpp"
#endif
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
#include "nr_xefg_present_probe.hpp"
#endif

namespace renodx::addons::dlss5::fg::xefg {

struct BridgeSourceIdentity {
  uintptr_t device11 = 0;
  uintptr_t handle = 0;
  uint64_t generation = 0;
  uintptr_t device12 = 0;
  bool operator==(const BridgeSourceIdentity&) const = default;
};
#if defined(NR_NEXUS_DEV_CONTROL)
enum class BridgeEventKind : uint8_t { ENABLE_BEGIN, ENABLE_END, LINK_REVOKED, LINK_OBSERVED };
enum class BridgePresentMethod : uint8_t { UNOBSERVED, PRESENT, PRESENT1 };
struct BridgeSourceEntry {
  BridgePresentMethod method = BridgePresentMethod::UNOBSERVED;
  uintptr_t return_address = 0;
  uint32_t thread_id = 0;
  uint32_t proxy_entry_depth = 0;
  uint32_t interval = 0;
  uint32_t flags = 0;
};
struct BridgeEvent {
  BridgeEventKind kind = BridgeEventKind::ENABLE_BEGIN;
  uint64_t sequence = 0;
  int64_t observed_ns = 0;
  // Next-statement sample after the real SDK call, not an exact function-body boundary.
  // ENABLE_END's observed_ns is the later receipt commit.
  int64_t sdk_return_ns = 0;
  uint32_t thread_id = 0;
  uintptr_t context_identity = 0;
  uint64_t declaration = 0;
  uint64_t epoch = 0;
  uint64_t link_generation = 0;
  // ENABLE_BEGIN keeps the initial attempt snapshot, captured before its source observer.
  // ENABLE_END keeps the link state after receipt commit.
  uintptr_t source_swapchain = 0;
  uint64_t source_swapchain_generation = 0;
  // SDK events keep the source sampled before the call; link events keep the affected link's source.
  BridgeSourceIdentity source;
  const char* reason = "unobserved";
  uint32_t enabled_request = 0;
  int32_t sdk_status = 0;
  bool off_observed = false;
  BridgeSourceEntry source_entry;
};
struct BridgeEventSnapshot {
  std::array<BridgeEvent, 32> events{};
  uint32_t count = 0;
  uint64_t last_sequence = 0;
  uint64_t overwritten = 0;
};
struct EnabledContextMetadata {
  bool available = false;
  const char* reason = "unobserved";
  uintptr_t context_identity = 0;
  uint64_t epoch = 0;
  uint64_t generation = 0;
};
#endif
enum class BridgeSnapshotState : uint8_t { UNOBSERVED, RUNTIME_BUSY, SHUTTING_DOWN, LOCKED };
struct BridgeSourceObservation {
  BridgeSourceIdentity identity;
  uintptr_t queried_device11 = 0;
  BridgeSnapshotState state = BridgeSnapshotState::UNOBSERVED;
  uint32_t candidates = 0;
  uint32_t matches = 0;
  uintptr_t queried_device12 = 0;
  bool operator==(const BridgeSourceObservation&) const = default;
};
using BridgeSourceObserver = BridgeSourceObservation (*)(uintptr_t device11) noexcept;
inline const char* BridgeSourceReason(const BridgeSourceObservation& observation) noexcept {
  switch (observation.state) {
    case BridgeSnapshotState::UNOBSERVED: return "unobserved";
    case BridgeSnapshotState::RUNTIME_BUSY: return "runtime_busy";
    case BridgeSnapshotState::SHUTTING_DOWN: return "shutting_down";
    case BridgeSnapshotState::LOCKED: break;
  }
  if (observation.candidates == 0) return "no_live_source";
  const bool device_matches = (observation.queried_device12 != 0
      ? observation.queried_device11 == 0 && observation.identity.device11 == 0
          && observation.identity.device12 == observation.queried_device12
      : observation.identity.device12 == 0 && observation.identity.device11 == observation.queried_device11);
  if (observation.matches == 0 || !device_matches) return "device_mismatch";
  if (observation.matches != 1 || observation.identity.handle == 0 || observation.identity.generation == 0) return "ambiguous";
  return "matched";
}
struct BridgeDeviceObservation {
  bool object_present = false;
  bool swapchain_unwrapped = false;
  HRESULT swapchain_result = E_PENDING;
  HRESULT device11_result = E_PENDING;
  HRESULT device12_result = E_PENDING;
  // Prefer GetDevice11's returned object; use GetDevice12 when it is absent.
  uintptr_t returned_device_identity = 0;
  HRESULT device11_interface_result = E_PENDING;
  HRESULT device12_interface_result = E_PENDING;
  uintptr_t device11_interface_identity = 0;
  uintptr_t device12_interface_identity = 0;
  uint32_t returned_device_request = 0;
  bool operator==(const BridgeDeviceObservation&) const = default;
};
struct BridgeOffEvidence {
  bool available = false;
  const char* reason = "unobserved";
  uintptr_t context = 0;
  uintptr_t proxy = 0;
  uintptr_t application_device11 = 0;
  uintptr_t proxy_device11 = 0;
  uint64_t declaration = 0;
  uint64_t epoch = 0;
  BridgeSourceIdentity source;
  BridgeSourceObservation before;
  BridgeSourceObservation after;
  const char* init_kind = "unobserved";
  BridgeDeviceObservation application;
  BridgeDeviceObservation proxy_device;
  const char* source_link_reason = "source_swapchain_unobserved";
  uintptr_t source_swapchain = 0;
  uint64_t source_swapchain_generation = 0;
  uint64_t source_link_generation = 0;
  bool uses_source_link = false;
  uintptr_t context_identity = 0;
  uint32_t source_present_thread_id = 0;
  uint32_t source_present_proxy_entry_depth = 0;
  bool MatchesOutput(const output_framerate::Snapshot& output) const {
    return available && context_identity != 0 && output.available && output.fg_off
        && output.source == output_framerate::Source::XEFG_STATUS && output.identity == context_identity;
  }
  bool operator==(const BridgeOffEvidence&) const = default;
};

// Public x64 XeSS-FG ABI, fixed Intel 8fe81bdb/de0fb9c1 headers. No SDK
// object layout is inspected. BACKBUFFER tags describe a region only.
namespace abi {
struct Extent { uint32_t x; uint32_t y; };
struct Resource {
  int32_t type;
  int32_t validity;
  Extent base;
  Extent size;
  ID3D12Resource* resource;
  D3D12_RESOURCE_STATES state;
};
struct Constants {
  float view[16];
  float projection[16];
  float jitter_x;
  float jitter_y;
  float motion_scale_x;
  float motion_scale_y;
  uint32_t reset;
  float frame_time_ms;
};
struct Init {
  IDXGISwapChain* application_swapchain;
  uint32_t flags;
  uint32_t max_interpolated;
  uint32_t creation_mask;
  uint32_t visible_mask;
  ID3D12Heap* buffer_heap;
  uint64_t buffer_offset;
  ID3D12Heap* texture_heap;
  uint64_t texture_offset;
  ID3D12PipelineLibrary* library;
  int32_t ui_mode;
};
struct PresentStatus {
  uint32_t frames_presented;
  int32_t frame_gen_result;
  uint32_t enabled;
};
static_assert(sizeof(Resource) == 40 && offsetof(Resource, resource) == 24);
static_assert(sizeof(Constants) == 152 && offsetof(Constants, reset) == 144);
static_assert(sizeof(Init) == 72 && offsetof(Init, ui_mode) == 64);
}  // namespace abi

namespace internal {
using Microsoft::WRL::ComPtr;
inline constexpr int32_t kUiModeAuto = 0;
inline std::atomic<Service> service{nullptr};
inline std::atomic<PrepareCapture> prepare_capture{nullptr};
inline std::atomic<Retire> retire{nullptr};
inline std::atomic<bool (*)() noexcept> requested{nullptr};
inline std::atomic_bool block_optional_hudless{false};
// Startup-only diagnostic switch. When false, XeFG SDK exports and its proxy
// Present hooks are left untouched so a host can isolate addon interception
// without detaching a live context.
inline std::atomic_bool sdk_detours_enabled{true};
inline std::atomic_bool stopped{false};
inline std::atomic_bool owns_stream{false};
inline std::atomic<BridgeSourceObserver> bridge_source_observer{nullptr};
inline std::atomic<BridgeSourceObserver> d3d12_source_observer{nullptr};
inline std::atomic<Result> result{Result::NOT_SERVED};
inline std::atomic<Result> display_result{Result::NOT_SERVED};
inline std::atomic_uint64_t submitted_frames{0};
inline void PublishResult(Result value) noexcept {
  display_result.store(value, std::memory_order_release);
  result.store(value, std::memory_order_release);
}
inline thread_local uint32_t proxy_depth = 0;
// SDK Present1 may call Present even when FG does not own the stream.
inline thread_local uint32_t proxy_entry_depth = 0;

struct Surface {
  ComPtr<ID3D12Resource> texture;
  ComPtr<ID3D12Fence> consumer_fence;
  uint64_t consumer_value = 0;
  bool reserved = true;
  bool unproven = false;
};
struct TaggedResource {
  abi::Resource data{};
  ComPtr<ID3D12Resource> resource;
  std::shared_ptr<Surface> copy;
};
struct Frame {
  std::array<TaggedResource, 4> resources;
  std::vector<std::shared_ptr<Surface>> captures;
  abi::Constants constants{};
  abi::Resource region{};
  bool has_constants = false;
  bool has_region = false;
  bool rejected = false;
  bool presenting = false;
};
// Resource identities rotate with the swapchain/capture pool. Log their values
// in samples, but only nullness changes consume the contract-change budget.
struct TraceResource {
  bool present = false;
  int32_t type = -1;
  int32_t validity = -1;
  abi::Extent base{};
  abi::Extent size{};
  uintptr_t resource = 0;
  uint32_t state = 0;
  bool operator==(const TraceResource& other) const {
    return present == other.present && type == other.type && validity == other.validity
        && base.x == other.base.x && base.y == other.base.y && size.x == other.size.x && size.y == other.size.y
        && (resource != 0) == (other.resource != 0) && state == other.state;
  }
};
struct TraceResourceCall {
  TraceResource original;
  TraceResource forwarded;
  bool replaced = false;
  bool sdk_called = false;
  int32_t status = 0;
  bool nr_requested = false;
  bool ui_composition = false;
  bool operator==(const TraceResourceCall&) const = default;
};
template <typename Sample>
struct ContractTrace {
  Sample last{};
  uint32_t last_id = 0;
  uint32_t frame_samples = 0;
  uint32_t change_samples = 0;
  bool seen = false;
  bool Observe(uint32_t id, const Sample& sample) {
    bool emit = false;
    if (frame_samples < 3 && (!seen || id != last_id)) {
      ++frame_samples;
      emit = true;
    } else if (seen && !(sample == last) && change_samples < 8) {
      ++change_samples;
      emit = true;
    }
    last = sample;
    last_id = id;
    seen = true;
    return emit;
  }
};
struct Context {
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Device> device;
  ComPtr<IUnknown> device_identity;
  ComPtr<ID3D12Fence> fence;
  uintptr_t proxy_identity = 0;
  abi::Init init{};
  DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  std::map<uint32_t, std::shared_ptr<Frame>> frames;
  std::vector<std::shared_ptr<Surface>> surfaces;
  uint64_t epoch = 1;
#if defined(NR_NEXUS_DEV_CONTROL)
  uint64_t fg_environment_generation = 0;
#endif
  uint64_t application_frame = 0;
  uint64_t last_submission_frame = 0;
  uint64_t next_fence = 0;
  uint32_t present_id = 0;
  uint32_t last_id = 0;
  bool fresh_id = false;
  bool last_id_known = false;
  bool enabled = false;
  ComPtr<IUnknown> bridge_application_device11;
  ComPtr<IUnknown> bridge_proxy_device11;
  const char* bridge_init_kind = "unobserved";
  BridgeDeviceObservation bridge_application_observation;
  BridgeDeviceObservation bridge_proxy_observation;
  bool bridge_context_ready = false;
  bool bridge_proxy_ready = false;
  bool bridge_off_observed = false;
  uint32_t bridge_enable_inflight = 0;
  uint64_t bridge_declaration = 0;
  uint64_t bridge_off_epoch = 0;
  BridgeSourceIdentity bridge_off_source;
  BridgeSourceObservation bridge_before_observation;
  BridgeSourceObservation bridge_after_observation;
  const char* bridge_off_reason = "disable_unobserved";
  uintptr_t bridge_source_swapchain = 0;
  uint64_t bridge_source_generation = 0;
  BridgeSourceIdentity bridge_link_source;
  uint64_t bridge_link_generation = 0;
  const char* bridge_link_reason = "source_swapchain_unobserved";
  bool bridge_off_uses_source_link = false;
  uint64_t bridge_off_link_generation = 0;
  uint32_t requested_interpolated = 0;
  bool requested_interpolated_known = false;
  uint64_t framerate_configuration = 0;
  bool omit_optional_hudless = false;
  bool ui_composition = false;
  // Lifetime budgets: at most 3 tagged frame samples + 8 contract changes per
  // resource kind (slot 5 covers null/unknown tags), and 1 + 8 UI-state calls.
  std::array<ContractTrace<TraceResourceCall>, 6> resource_trace;
  ContractTrace<std::array<int64_t, 3>> ui_trace;
  int32_t (*get_status)(void*, abi::PresentStatus*) = nullptr;
  std::chrono::steady_clock::time_point interval_start = std::chrono::steady_clock::now();
  uint64_t interval_presents = 0;
  uint64_t interval_tagged_presents = 0;
  uint64_t interval_untagged_presents = 0;
  uint64_t interval_displayed = 0;
  uint64_t interval_submitted = 0;
  uint64_t interval_status_samples = 0;
  // Protected by state.mutex; bind role counts to a completed Present rather
  // than inferring executed work from its available input resources.
  uint64_t service_stats_serial = 0;
  uint32_t service_hudless_processes = 0;
  uint32_t service_backbuffer_processes = 0;
  bool service_composed = false;
  std::atomic_uint64_t interval_nr_frames{0};
  std::atomic_uint64_t interval_nr_processes{0};
  std::atomic_uint64_t interval_nr_evaluates{0};
  std::atomic_uint64_t interval_nr_hudless_processes{0};
  std::atomic_uint64_t interval_nr_backbuffer_processes{0};
  std::atomic_uint64_t interval_nr_two_color_frames{0};
  std::atomic_uint64_t interval_nr_composed_frames{0};
  std::atomic_uint64_t interval_hudless_blocked{0};
  std::atomic_uint64_t interval_hudless_sdk_calls{0};
  std::atomic_uint64_t interval_hudless_sdk_success{0};
  std::atomic_uint64_t interval_input_hudless{0};
  std::atomic_uint64_t interval_input_ui{0};
};
struct BridgeSourceSwapchain {
  uintptr_t owner = 0;
  uintptr_t identity = 0;
  ComPtr<IUnknown> device11;
  uint64_t generation = 0;
  bool hook_ready = false;
  std::weak_ptr<Context> context;
  uintptr_t proxy = 0;
  const char* reason = "source_present_unobserved";
  ComPtr<IUnknown> device12;
  uint32_t source_present_thread_id = 0;
  uint32_t source_present_proxy_entry_depth = 0;
};
struct State {
  std::shared_mutex mutex;
  std::map<void*, std::shared_ptr<Context>> contexts;
  std::vector<std::shared_ptr<Context>> retired_contexts;
  uint64_t bridge_declaration = 0;
  std::map<uintptr_t, std::shared_ptr<BridgeSourceSwapchain>> bridge_swapchains;
  uint64_t bridge_source_generation = 0;
  uint64_t bridge_link_generation = 0;
#if defined(NR_NEXUS_DEV_CONTROL)
  std::array<BridgeEvent, 32> bridge_events{};
  uint64_t bridge_event_sequence = 0;
  uint64_t fg_environment_generation = 0;
#endif
};
inline State state;
inline std::shared_mutex install_mutex;
inline std::atomic<DWORD> module_bytes{0};

#if defined(NR_NEXUS_DEV_CONTROL)
// state.mutex is held by callers. No callbacks, allocation, or file I/O occur here.
inline void RecordBridgeEventLocked(BridgeEvent event) {
  event.sequence = ++state.bridge_event_sequence;
  event.observed_ns = output_framerate::NowNs();
  event.thread_id = GetCurrentThreadId();
  state.bridge_events[(event.sequence - 1) % state.bridge_events.size()] = event;
}
#endif

// state.mutex is held by callers. A host link never substitutes for a Disable
// receipt, and its failure must not invalidate the independent direct path.
inline void RevokeBridgeSourceLinkLocked(Context* context, const char* reason
#if defined(NR_NEXUS_DEV_CONTROL)
    , const BridgeSourceEntry& source_entry = {}
#endif
) {
#if defined(NR_NEXUS_DEV_CONTROL)
  const bool had_link = context->bridge_source_swapchain != 0;
  // Revocation keeps the old identity, but its generation and Off state describe the committed revocation.
  BridgeEvent event{
      .kind = BridgeEventKind::LINK_REVOKED,
      .context_identity = reinterpret_cast<uintptr_t>(context),
      .declaration = context->bridge_declaration,
      .epoch = context->epoch,
      .source_swapchain = context->bridge_source_swapchain,
      .source_swapchain_generation = context->bridge_source_generation,
      .source = context->bridge_link_source,
      .reason = reason,
      .source_entry = source_entry};
#endif
  if (context->bridge_source_swapchain != 0) {
    context->bridge_link_generation = ++state.bridge_link_generation;
  }
  context->bridge_source_swapchain = 0;
  context->bridge_source_generation = 0;
  context->bridge_link_source = {};
  context->bridge_link_reason = reason;
  if (context->bridge_off_uses_source_link) {
    context->bridge_off_observed = false;
    context->bridge_off_reason = reason;
  }
#if defined(NR_NEXUS_DEV_CONTROL)
  if (had_link) {
    event.link_generation = context->bridge_link_generation;
    event.off_observed = context->bridge_off_observed;
    RecordBridgeEventLocked(event);
  }
#endif
}

inline bool BridgeSourceLinkCurrentLocked(const Context& context) {
  const auto source = state.bridge_swapchains.find(context.bridge_source_swapchain);
  if (source == state.bridge_swapchains.end() || !source->second->hook_ready
      || source->second->generation != context.bridge_source_generation
      || source->second->context.lock().get() != &context || source->second->proxy != context.proxy_identity
      || context.bridge_link_source.handle == 0 || context.bridge_link_source.generation == 0) return false;
  if (context.bridge_link_source.device12 == 0) return source->second->device12 == nullptr;
  if (context.bridge_link_source.device11 != 0 || source->second->device11 != nullptr
      || source->second->device12 == nullptr || source->second->device12.Get() != context.device_identity.Get()
      || reinterpret_cast<uintptr_t>(source->second->device12.Get()) != context.bridge_link_source.device12) return false;
  return std::count_if(state.bridge_swapchains.begin(), state.bridge_swapchains.end(), [&](const auto& entry) {
    const auto& [owner, registered] = entry;
    return registered->device12 == source->second->device12;
  }) == 1;
}

inline void ClearBridgeSourceObservationsLocked(Context* context, const char* reason) {
  RevokeBridgeSourceLinkLocked(context, reason);
  for (const auto& [owner, source] : state.bridge_swapchains) {
    if (source->context.lock().get() == context) {
      source->context.reset();
      source->proxy = 0;
      source->reason = reason;
    }
  }
}

inline bool Requested() noexcept {
  const auto policy = requested.load(std::memory_order_acquire);
  return !stopped.load(std::memory_order_acquire) && policy != nullptr && policy();
}

inline void LogDiagnostic(const char* message) noexcept {
#if defined(RESHADE_API_LIBRARY)
  ReShadeLogMessage(nullptr, static_cast<int>(reshade::log::level::info), message);
#else
  const auto module = reshade::internal::get_reshade_module_handle();
  if (module == nullptr) return;
  const auto function = reinterpret_cast<void (*)(void*, int, const char*)>(
      GetProcAddress(module, "ReShadeLogMessage"));
  if (function == nullptr) return;
  function(reshade::internal::get_current_module_handle(), static_cast<int>(reshade::log::level::info), message);
#endif
}

inline void TraceResourceContract(void* handle, ID3D12CommandList* list, uint32_t id,
                                  const abi::Resource* original, const abi::Resource* forwarded,
                                  bool sdk_called, int32_t status) {
  if (stopped.load(std::memory_order_acquire) || !sdk_detours_enabled.load(std::memory_order_acquire)) return;
  const auto snapshot = [](const abi::Resource* data) {
    if (data == nullptr) return TraceResource{};
    return TraceResource{.present = true, .type = data->type, .validity = data->validity,
        .base = data->base, .size = data->size, .resource = reinterpret_cast<uintptr_t>(data->resource),
        .state = static_cast<uint32_t>(data->state)};
  };
  TraceResourceCall sample{.original = snapshot(original), .forwarded = snapshot(forwarded),
      .replaced = sdk_called && original != forwarded, .sdk_called = sdk_called, .status = status,
      .nr_requested = Requested()};
  uint32_t frame_samples = 0;
  uint32_t change_samples = 0;
  int32_t ui_mode = 0;
  {
    std::unique_lock lock(state.mutex);
    const auto it = state.contexts.find(handle);
    if (it == state.contexts.end()) return;
    sample.ui_composition = it->second->ui_composition;
    ui_mode = it->second->init.ui_mode;
    const size_t slot = (original != nullptr && original->type >= 0 && original->type <= 4
        ? static_cast<size_t>(original->type) : 5);
    auto& trace = it->second->resource_trace[slot];
    if (!trace.Observe(id, sample)) return;
    frame_samples = trace.frame_samples;
    change_samples = trace.change_samples;
  }
  constexpr const char* names[]{"HUDLess", "Depth", "Motion", "UI", "BackbufferRegion", "NullOrUnknown"};
  const auto& raw = sample.original;
  const auto& actual = sample.forwarded;
  const auto* name = names[(raw.present && raw.type >= 0 && raw.type <= 4 ? raw.type : 5)];
  char sdk_status[24] = "not_called";
  if (sdk_called) std::snprintf(sdk_status, sizeof(sdk_status), "%d", status);
  char message[1024];
  std::snprintf(message, sizeof(message),
      "DLSS5 XeFG resource_contract context=%llx present_id=%u tag=%s list=%llx nr_requested=%u "
      "ui_mode=%d ui_composition=%u frame_sample=%u/3 change_sample=%u/8 "
      "original[present=%u type=%d validity=%d base=%u,%u size=%u,%u resource=%llx state=%u] "
      "forwarded[present=%u type=%d validity=%d base=%u,%u size=%u,%u resource=%llx state=%u] "
      "replaced=%u sdk_called=%u sdk_status=%s adapter_return=%d",
      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(handle)), id, name,
      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(list)), static_cast<unsigned>(sample.nr_requested),
      ui_mode, static_cast<unsigned>(sample.ui_composition), frame_samples, change_samples,
      static_cast<unsigned>(raw.present), raw.type, raw.validity, raw.base.x, raw.base.y, raw.size.x, raw.size.y,
      static_cast<unsigned long long>(raw.resource), raw.state,
      static_cast<unsigned>(actual.present), actual.type, actual.validity, actual.base.x, actual.base.y, actual.size.x, actual.size.y,
      static_cast<unsigned long long>(actual.resource), actual.state,
      static_cast<unsigned>(sample.replaced), static_cast<unsigned>(sdk_called), sdk_status, status);
  LogDiagnostic(message);
}

inline void RecordServiceStats(uintptr_t context_value, uint32_t process_calls,
                               uint32_t evaluates, uint32_t hudless_processes,
                               uint32_t backbuffer_processes, bool two_color,
                               bool composed) noexcept {
  auto* context = reinterpret_cast<Context*>(context_value);
  if (context == nullptr) return;
  std::unique_lock lock(state.mutex);
  ++context->service_stats_serial;
  context->service_hudless_processes = hudless_processes;
  context->service_backbuffer_processes = backbuffer_processes;
  context->service_composed = composed;
  context->interval_nr_frames.fetch_add(1, std::memory_order_relaxed);
  context->interval_nr_processes.fetch_add(process_calls, std::memory_order_relaxed);
  context->interval_nr_evaluates.fetch_add(evaluates, std::memory_order_relaxed);
  context->interval_nr_hudless_processes.fetch_add(hudless_processes, std::memory_order_relaxed);
  context->interval_nr_backbuffer_processes.fetch_add(backbuffer_processes, std::memory_order_relaxed);
  context->interval_nr_two_color_frames.fetch_add(two_color, std::memory_order_relaxed);
  context->interval_nr_composed_frames.fetch_add(composed, std::memory_order_relaxed);
}

inline void RefreshOwnership() {
  bool active = false;
  {
    std::shared_lock lock(state.mutex);
    for (const auto& [handle, context] : state.contexts) {
      active |= context->enabled && context->queue != nullptr && context->proxy_identity != 0;
    }
  }
  owns_stream.store(active, std::memory_order_release);
}

inline uintptr_t Identity(IUnknown* object) {
  ComPtr<IUnknown> identity;
  if (object == nullptr || FAILED(object->QueryInterface(IID_PPV_ARGS(&identity)))) return 0;
  return reinterpret_cast<uintptr_t>(identity.Get());
}

// Called only while the SDK callback still owns a valid swapchain argument.
inline ComPtr<IUnknown> BridgeDeviceIdentity(IUnknown* object, BridgeDeviceObservation* observation = nullptr) {
  if (observation != nullptr) {
    *observation = {.object_present = object != nullptr};
  }
  ComPtr<IDXGISwapChain> swapchain;
  ComPtr<IUnknown> device;
  if (object == nullptr) return {};
  const auto swapchain_result = object->QueryInterface(IID_PPV_ARGS(&swapchain));
  if (observation != nullptr) {
    observation->swapchain_result = swapchain_result;
  }
  if (FAILED(swapchain_result)) return {};
  auto* native_swapchain = swapchain.Get();
  if (renodx::utils::directx::NativeFromReShadeProxy(&native_swapchain)) {
    // Own the borrowed base before releasing the outer interface, then query
    // the known base's device rather than the wrapper's reported interface.
    swapchain = native_swapchain;
    if (observation != nullptr) {
      observation->swapchain_unwrapped = true;
    }
  }
  const auto device11_result = swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(device.GetAddressOf()));
  if (observation == nullptr) {
    if (FAILED(device11_result) || device == nullptr) return {};
    ComPtr<ID3D11Device> device11_interface;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device11_interface))) || device11_interface == nullptr) return {};
    return ComPtr<IUnknown>(renodx::utils::directx::NativeIdentity(device11_interface.Get()));
  }
  observation->device11_result = device11_result;
  ComPtr<IUnknown> device12;
  observation->device12_result = swapchain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(device12.GetAddressOf()));
  if (SUCCEEDED(device11_result) && device != nullptr) {
    observation->returned_device_request = 11;
  } else if (SUCCEEDED(observation->device12_result) && device12 != nullptr) {
    observation->returned_device_request = 12;
    device = std::move(device12);
  } else {
    return {};
  }
  observation->returned_device_identity = Identity(device.Get());
  ComPtr<ID3D11Device> device11_interface;
  observation->device11_interface_result = device->QueryInterface(IID_PPV_ARGS(&device11_interface));
  if (SUCCEEDED(observation->device11_interface_result)) {
    observation->device11_interface_identity = Identity(device11_interface.Get());
  }
  ComPtr<ID3D12Device> device12_interface;
  observation->device12_interface_result = device->QueryInterface(IID_PPV_ARGS(&device12_interface));
  if (SUCCEEDED(observation->device12_interface_result)) {
    observation->device12_interface_identity = Identity(device12_interface.Get());
  }
  if (FAILED(observation->device11_interface_result) || device11_interface == nullptr
      || observation->returned_device_identity == 0
      || observation->device11_interface_identity != observation->returned_device_identity) return {};
  return ComPtr<IUnknown>(renodx::utils::directx::NativeIdentity(device11_interface.Get()));
}

inline bool BridgeDeviceCaptureReady(const BridgeDeviceObservation& observation, IUnknown* device11, bool allow_absent) {
  if (!observation.object_present) return allow_absent;
  if (FAILED(observation.swapchain_result) || observation.returned_device_identity == 0
      || (observation.returned_device_request != 11 && observation.returned_device_request != 12)) return false;
  if (SUCCEEDED(observation.device11_interface_result)) {
    return device11 != nullptr && observation.device11_interface_identity == observation.returned_device_identity;
  }
  return observation.device11_interface_result == E_NOINTERFACE && SUCCEEDED(observation.device12_interface_result)
      && observation.device12_interface_identity == observation.returned_device_identity;
}

inline ComPtr<IUnknown> D3D12SourceDeviceIdentity(IDXGISwapChain* swapchain) {
  if (swapchain == nullptr) return {};
  renodx::utils::directx::NativeFromReShadeProxy(&swapchain);
  ComPtr<ID3D12Device> device;
  if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&device)))) return {};
  return ComPtr<IUnknown>(renodx::utils::directx::NativeIdentity(device.Get()));
}

inline uint64_t InvalidateBridgeOff(void* handle, const char* reason,
    bool invalidate_context = false, bool invalidate_proxy = false) {
  std::unique_lock lock(state.mutex);
  if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
    auto& context = *it->second;
    context.bridge_off_observed = false;
    context.bridge_before_observation = {};
    context.bridge_after_observation = {};
    context.bridge_off_reason = reason;
    context.bridge_declaration = ++state.bridge_declaration;
    if (invalidate_context || invalidate_proxy) {
      ClearBridgeSourceObservationsLocked(&context, reason);
    }
    if (invalidate_context) {
      context.bridge_context_ready = false;
    }
    if (invalidate_proxy) {
      context.bridge_proxy_ready = false;
    }
    return context.bridge_declaration;
  }
  return 0;
}

inline std::shared_ptr<Frame> GetFrame(Context* context, uint32_t id) {
  if (const auto it = context->frames.find(id); it != context->frames.end()) {
    return it->second->presenting ? nullptr : it->second;
  }
  // A caller may tag ahead. Keep the admission bounded without discarding a
  // frame whose SDK still holds one of our substituted resources.
  if (context->frames.size() >= 16) return nullptr;
  auto frame = std::make_shared<Frame>();
  context->frames.emplace(id, frame);
  return frame;
}

inline void ReleaseFrames(Context* context) {
  output_framerate::Invalidate();
  for (const auto& [id, frame] : context->frames) {
    for (const auto& resource : frame->resources) {
      if (resource.copy != nullptr) resource.copy->unproven = true;
    }
  }
  context->frames.clear();
  context->fresh_id = false;
  context->last_id_known = false;
  context->last_submission_frame = 0;
  ++context->epoch;
#if defined(NR_NEXUS_DEV_CONTROL)
  edit_fg_observation::StopEnvironment(2, reinterpret_cast<uintptr_t>(context));
  context->fg_environment_generation = ++state.fg_environment_generation;
#if defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
  xefg_present_probe::Observe([&] {
    xefg_present_probe::UpdateContext(reinterpret_cast<uintptr_t>(context), context->epoch,
        context->fg_environment_generation);
  });
#endif
#if defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
  fg_output_probe::UpdateXefg(reinterpret_cast<uintptr_t>(context), context->epoch,
      context->fg_environment_generation);
#endif
#endif
}

inline std::shared_ptr<Surface> AcquireSurface(const std::shared_ptr<Context>& context,
                                              const D3D12_RESOURCE_DESC& description) {
  std::shared_ptr<Surface> surface;
  {
    std::unique_lock lock(state.mutex);
    for (const auto& candidate : context->surfaces) {
      if (candidate->reserved || candidate->unproven
          || !submission::ResourceReleasable(candidate->texture.Get())) continue;
      if (candidate->consumer_fence != nullptr
          && candidate->consumer_fence->GetCompletedValue() < candidate->consumer_value) continue;
      const auto have = candidate->texture->GetDesc();
      if (have.Width == description.Width && have.Height == description.Height
          && have.Format == description.Format) {
        surface = candidate;
        surface->reserved = true;
        break;
      }
    }
    if (surface == nullptr && context->surfaces.size() >= 64) return nullptr;
  }
  if (surface != nullptr) return surface;
  D3D12_RESOURCE_DESC copy = description;
  copy.Alignment = 0;
  copy.MipLevels = 1;
  copy.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  copy.Flags &= ~(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  surface = std::make_shared<Surface>();
  if (FAILED(context->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copy,
          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&surface->texture)))) return nullptr;
  surface->texture->SetName(L"DLSS5 FG tagged input");
  {
    std::unique_lock lock(state.mutex);
    context->surfaces.push_back(surface);
  }
  return surface;
}

// Several ONLY_NOW resources commonly come from one command-list generation.
// One queue wait on the highest value of a fence covers all lower values; an
// already completed value needs no queue mutation at all.
inline void CoalesceWaits(std::vector<std::pair<FenceRef, uint64_t>>* waits) {
  std::vector<std::pair<FenceRef, uint64_t>> unique;
  unique.reserve(waits->size());
  for (auto& [fence, value] : *waits) {
    if (fence.empty()) continue;
    auto found = std::find_if(unique.begin(), unique.end(),
        [&](const auto& candidate) { return candidate.first.fence == fence.fence; });
    if (found == unique.end()) {
      unique.emplace_back(std::move(fence), value);
    } else {
      found->second = std::max(found->second, value);
    }
  }
  waits->swap(unique);
}

inline bool OrderCopies(const std::shared_ptr<Frame>& frame, ID3D12CommandQueue* queue) {
  if (frame == nullptr) return true;
  std::vector<std::pair<FenceRef, uint64_t>> waits;
  std::vector<std::pair<FenceRef, uint64_t>> resource_waits;
  bool ordered = true;
  for (const auto& resource : frame->resources) {
    if (resource.copy == nullptr) continue;
    const auto resource_state = submission::UseSubmissionTokens(resource.resource.Get(), &resource_waits);
    if (resource_state == submission::UseSubmission::kUnsubmitted) {
      ordered = false;
    }
    waits.insert(waits.end(),
        std::make_move_iterator(resource_waits.begin()),
        std::make_move_iterator(resource_waits.end()));
  }
  CoalesceWaits(&waits);
  for (const auto& [fence, value] : waits) {
    const uint64_t completed = fence->GetCompletedValue();
    if (completed != UINT64_MAX && completed >= value) continue;
    if (FAILED(queue->Wait(fence.fence, value))) {
      ordered = false;
    }
  }
  return ordered;
}

inline void Initialize(void* handle, ID3D12CommandQueue* queue, const abi::Init* parameters,
                        HMODULE sdk_module = nullptr, const char* init_kind = "Init"
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
                        , uint32_t probe_sdk_slot = UINT32_MAX
#endif
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
                        , uint64_t present_probe_serial = 0
#endif
                        ) {
  if (handle == nullptr || queue == nullptr || parameters == nullptr) return;
  auto context = std::make_shared<Context>();
  context->queue = queue;
  context->init = *parameters;
  context->bridge_init_kind = init_kind;
  context->bridge_application_device11 = BridgeDeviceIdentity(parameters->application_swapchain,
      &context->bridge_application_observation);
  output_framerate::ObserveXeFGMetadata(reinterpret_cast<uintptr_t>(context.get()), parameters->max_interpolated + 1);
  context->omit_optional_hudless = block_optional_hudless.load(std::memory_order_acquire)
      && parameters->ui_mode == kUiModeAuto;
  if (sdk_module != nullptr) {
    context->get_status = reinterpret_cast<int32_t (*)(void*, abi::PresentStatus*)>(
        GetProcAddress(sdk_module, "xefgSwapChainGetLastPresentStatus"));
  }
  if (FAILED(queue->GetDevice(IID_PPV_ARGS(&context->device)))
      || FAILED(context->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&context->fence)))) return;
  context->device_identity = renodx::utils::directx::NativeIdentity(context->device.Get());
  std::unique_lock lock(state.mutex);
  context->bridge_context_ready = BridgeDeviceCaptureReady(
      context->bridge_application_observation, context->bridge_application_device11.Get(), true);
  context->bridge_declaration = ++state.bridge_declaration;
#if defined(NR_NEXUS_DEV_CONTROL)
  context->fg_environment_generation = ++state.fg_environment_generation;
#endif
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
  const fg_output_probe::XefgReceipt probe_context{
      .sdk_slot = probe_sdk_slot, .module_base = reinterpret_cast<uintptr_t>(sdk_module),
      .sdk_context = reinterpret_cast<uintptr_t>(handle), .context_identity = reinterpret_cast<uintptr_t>(context.get()),
      .init_epoch = context->epoch, .init_generation = context->fg_environment_generation,
      .current_epoch = context->epoch, .current_generation = context->fg_environment_generation};
#endif
  if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
    state.retired_contexts.push_back(it->second);
  }
  state.contexts[handle] = std::move(context);
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
  xefg_present_probe::Observe([&] {
    const auto& registered = state.contexts.at(handle);
    xefg_present_probe::ContextInitialized(present_probe_serial, reinterpret_cast<uintptr_t>(registered.get()),
        registered->epoch, registered->fg_environment_generation);
  });
#endif
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
  lock.unlock();
  if (probe_sdk_slot < 2) {
    // The actual Init hook owns the opaque-handle lifecycle turn. This export
    // is resolved from that exact SDK slot, never from a library basename.
    fg_output_probe::ApplyXefg(probe_context, reinterpret_cast<fg_output_probe::EnableXefgDebug>(
        GetProcAddress(sdk_module, "xefgSwapChainEnableDebugFeature")));
  }
#endif
}

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Resize1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                              const UINT*, IUnknown* const*);
using ColorFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
struct ProxySlot {
  std::array<void*, 5> real{};
  std::array<detour_guard::Record, 5> records;
  bool installed = false;
  bool complete = false;
};
inline std::array<ProxySlot, 4> proxies;

struct BridgeSourcePresentMapping {
  std::shared_ptr<BridgeSourceSwapchain> registration;
  BridgeSourceObservation before;
  std::shared_ptr<Context> context;
  void* context_handle = nullptr;
  uintptr_t proxy = 0;
  uint64_t declaration = 0;
  uint64_t epoch = 0;
  const char* failure = nullptr;
  bool proxy_outer = false;
  bool unregistered_outer = false;
  uint32_t source_present_thread_id = 0;
  uint32_t source_present_proxy_entry_depth = 0;
#if defined(NR_NEXUS_DEV_CONTROL)
  BridgeSourceEntry source_entry;
#endif
};

// Both call directions commit only after their outer Present returns. All
// COM and source-observer calls finish before this state-only transaction.
inline void RecordSourcePresentMapping(const BridgeSourcePresentMapping& mapping, const BridgeSourceObservation& after) {
  const auto& registration = mapping.registration;
  if (registration == nullptr) return;
  const bool native12 = registration->device12 != nullptr;
  const auto expected = (native12 ? registration->device12.Get() : registration->device11.Get());
  const auto& before = mapping.before;
  const auto& context = mapping.context;
  const auto proxy = mapping.proxy;
  const char* reason = mapping.failure;
  if (reason == nullptr && (before.state == BridgeSnapshotState::RUNTIME_BUSY
      || after.state == BridgeSnapshotState::RUNTIME_BUSY)) {
    reason = "source_observer_busy";
  }
  if (reason == nullptr && (before.identity.handle == 0 || before.identity.generation == 0
      || (native12 ? before.identity.device11 != 0 || before.identity.device12 != reinterpret_cast<uintptr_t>(expected)
                   : before.identity.device12 != 0 || before.identity.device11 != reinterpret_cast<uintptr_t>(expected)))) {
    reason = "source_missing_before_present";
  }
  if (reason == nullptr && before.identity != after.identity) {
    reason = "source_changed_during_present";
  }
  if (reason == nullptr && native12 && registration->identity == proxy) {
    reason = "source_is_sdk_proxy";
  }
  std::unique_lock lock(state.mutex);
  const auto registered = state.bridge_swapchains.find(registration->owner);
  if (registered == state.bridge_swapchains.end() || registered->second != registration) return;
  if (mapping.source_present_thread_id != 0) {
    registration->source_present_thread_id = mapping.source_present_thread_id;
    registration->source_present_proxy_entry_depth = mapping.source_present_proxy_entry_depth;
  }
  if (reason == nullptr) {
    const auto active = state.contexts.find(mapping.context_handle);
    if (stopped.load() || !sdk_detours_enabled.load() || active == state.contexts.end() || active->second != context
        || !context->bridge_context_ready || !context->bridge_proxy_ready || context->proxy_identity != proxy) {
      reason = "context_changed_during_present";
    } else if (native12 && (context->device_identity.Get() != expected
        || context->bridge_application_device11 != nullptr || context->bridge_proxy_device11 != nullptr)) {
      reason = "source_device12_conflict";
    } else if (!native12 && context->bridge_application_device11 != nullptr
        && reinterpret_cast<uintptr_t>(context->bridge_application_device11.Get()) != before.identity.device11) {
      reason = "application_source_conflict";
    } else if (context->bridge_declaration != mapping.declaration || context->epoch != mapping.epoch) {
      // A concurrent Enable/Disable owns its new receipt. An old candidate
      // cannot refresh or revoke an independently proven, unchanged link.
      registration->reason = "present_declaration_changed";
      return;
    }
  }
  if (reason != nullptr) {
    registration->reason = reason;
    if (std::strcmp(reason, "source_observer_busy") == 0) return;
    if (const auto old = registration->context.lock(); old != nullptr
        && old->bridge_source_swapchain == registration->owner
        && old->bridge_source_generation == registration->generation) {
      RevokeBridgeSourceLinkLocked(old.get(), reason
#if defined(NR_NEXUS_DEV_CONTROL)
          , mapping.source_entry
#endif
      );
    }
    registration->context.reset();
    registration->proxy = 0;
    return;
  }
  registration->context = context;
  registration->proxy = proxy;
  const char* observed_reason = "source_mapping_observed";
  if (mapping.proxy_outer) {
    observed_reason = (mapping.unregistered_outer ? "proxy_to_source_mapping_observed_unregistered_outer"
                                                : "proxy_to_source_mapping_observed");
  }
  registration->reason = observed_reason;
  uint32_t sources = 0;
  for (const auto& [owner, candidate] : state.bridge_swapchains) {
    if (native12 ? candidate->device12 == registration->device12
                 : candidate->context.lock() == context && candidate->proxy == proxy) {
      ++sources;
    }
  }
  if (sources != 1) {
    registration->reason = "multiple_source_swapchains";
    RevokeBridgeSourceLinkLocked(context.get(), "multiple_source_swapchains"
#if defined(NR_NEXUS_DEV_CONTROL)
        , mapping.source_entry
#endif
    );
    return;
  }
  if (context->bridge_source_swapchain != registration->owner
      || context->bridge_source_generation != registration->generation || context->bridge_link_source != before.identity) {
    RevokeBridgeSourceLinkLocked(context.get(), "source_mapping_changed"
#if defined(NR_NEXUS_DEV_CONTROL)
        , mapping.source_entry
#endif
    );
    context->bridge_source_swapchain = registration->owner;
    context->bridge_source_generation = registration->generation;
    context->bridge_link_source = before.identity;
    context->bridge_link_generation = ++state.bridge_link_generation;
#if defined(NR_NEXUS_DEV_CONTROL)
    RecordBridgeEventLocked({
        .kind = BridgeEventKind::LINK_OBSERVED,
        .context_identity = reinterpret_cast<uintptr_t>(context.get()),
        .declaration = context->bridge_declaration,
        .epoch = context->epoch,
        .link_generation = context->bridge_link_generation,
        .source_swapchain = context->bridge_source_swapchain,
        .source_swapchain_generation = context->bridge_source_generation,
        .source = context->bridge_link_source,
        .reason = observed_reason,
        .off_observed = context->bridge_off_observed,
        .source_entry = mapping.source_entry});
#endif
  }
  context->bridge_link_reason = observed_reason;
}

struct D3D12ProxyPresentObservation {
  inline static thread_local D3D12ProxyPresentObservation* current = nullptr;
  D3D12ProxyPresentObservation* previous = current;
  BridgeSourcePresentMapping mapping;
  IUnknown* device = nullptr;  // The captured Context owns this device.
  uint32_t source_calls = 0;
  bool source_finished = false;
};

// A registration owns its device, never the swapchain. The actual Present
// argument is borrowed only on this thread's call stack, including on unwind.
struct BridgeSourcePresent : BridgeSourcePresentMapping {
  inline static thread_local BridgeSourcePresent* current = nullptr;
  BridgeSourcePresent* previous = current;
  BridgeSourcePresent* root = this;
  IDXGISwapChain* borrowed_swapchain = nullptr;
  D3D12ProxyPresentObservation* outer_proxy = nullptr;
  uint32_t proxy_calls = 0;
  bool test = false;
  bool finished = false;

  BridgeSourcePresent(IDXGISwapChain* swapchain, UINT flags
#if defined(NR_NEXUS_DEV_CONTROL)
      , const BridgeSourceEntry& entry = {}
#endif
  ) noexcept
      : borrowed_swapchain(swapchain), test((flags & DXGI_PRESENT_TEST) != 0) {
#if defined(NR_NEXUS_DEV_CONTROL)
    source_entry = entry;
#endif
    try {
      if (!stopped.load() && sdk_detours_enabled.load()) {
        const auto identity = Identity(swapchain);
        {
          std::shared_lock lock(state.mutex);
          for (const auto& [owner, candidate] : state.bridge_swapchains) {
            if (identity == 0 || candidate->identity != identity || !candidate->hook_ready) continue;
            if (registration != nullptr) {
              failure = "multiple_source_swapchains";
              break;
            }
            registration = candidate;
          }
        }
        if (registration != nullptr) {
          source_present_thread_id = GetCurrentThreadId();
          source_present_proxy_entry_depth = proxy_entry_depth;
        }
        if (previous != nullptr && !test && !previous->test) {
          if (registration != nullptr && previous->registration == registration) {
            root = previous->root;  // The same wrapper's Present1 can call Present.
          } else if (previous->registration != nullptr) {
            previous->root->failure = failure = "nested_source_swapchains";
          }
        }
        if (root == this && registration != nullptr && !test && failure == nullptr) {
          const bool native12 = registration->device12 != nullptr;
          if (native12 && D3D12ProxyPresentObservation::current != nullptr) {
            outer_proxy = D3D12ProxyPresentObservation::current;
            if (++outer_proxy->source_calls != 1) {
              outer_proxy->mapping.failure = "multiple_source_presents";
            } else {
              outer_proxy->mapping.registration = registration;
              outer_proxy->mapping.source_present_thread_id = source_present_thread_id;
              outer_proxy->mapping.source_present_proxy_entry_depth = source_present_proxy_entry_depth;
#if defined(NR_NEXUS_DEV_CONTROL)
              outer_proxy->mapping.source_entry = source_entry;
#endif
            }
          }
          const auto device = (native12 ? D3D12SourceDeviceIdentity(swapchain) : BridgeDeviceIdentity(swapchain));
          const auto expected = (native12 ? registration->device12.Get() : registration->device11.Get());
          if (device.Get() != expected) {
            failure = "source_device_changed";
          }
          const auto observer = (native12 ? d3d12_source_observer.load(std::memory_order_acquire)
                                         : bridge_source_observer.load(std::memory_order_acquire));
          if (observer != nullptr) {
            before = observer(reinterpret_cast<uintptr_t>(expected));
          }
        }
      }
    } catch (...) {
      failure = "source_present_observation_failed";
    }
    current = this;
  }

  void Finish(HRESULT status) noexcept {
    if (finished) return;
    finished = true;
    current = previous;
    if (root != this) {
      if (FAILED(status)) {
        root->failure = "source_present_failed";
      }
      return;
    }
    if (registration == nullptr || test) return;
    try {
      BridgeSourceObservation after;
      const bool native12 = registration->device12 != nullptr;
      const auto expected = (native12 ? registration->device12.Get() : registration->device11.Get());
      const auto observer = (native12 ? d3d12_source_observer.load(std::memory_order_acquire)
                                     : bridge_source_observer.load(std::memory_order_acquire));
      if (observer != nullptr) {
        after = observer(reinterpret_cast<uintptr_t>(expected));
      }
      const char* reason = failure;
      if (FAILED(status)) {
        reason = (failure != nullptr ? failure : "source_present_failed");
      }
      const auto current_device = (native12 ? D3D12SourceDeviceIdentity(borrowed_swapchain)
                                            : BridgeDeviceIdentity(borrowed_swapchain));
      if (reason == nullptr && (Identity(borrowed_swapchain) != registration->identity
          || current_device.Get() != expected)) {
        reason = "source_device_changed";
      }
      if (outer_proxy != nullptr) {
        if (reason == nullptr && (before.state == BridgeSnapshotState::RUNTIME_BUSY
            || after.state == BridgeSnapshotState::RUNTIME_BUSY)) {
          reason = "source_observer_busy";
        }
        if (reason == nullptr && (before.identity != outer_proxy->mapping.before.identity
            || after.identity != before.identity)) {
          reason = "source_changed_during_present";
        }
        if (reason != nullptr && outer_proxy->mapping.failure == nullptr) {
          outer_proxy->mapping.failure = reason;
        }
        outer_proxy->source_finished = true;
        return;
      }
      if (reason == nullptr && (context == nullptr || proxy_calls != 1)) {
        reason = "nested_proxy_unobserved";
      }
      failure = reason;
      RecordSourcePresentMapping(*this, after);
    } catch (...) {
      // QueryBridgeOff must fail closed even if the observation bookkeeping
      // itself cannot finish; the caller's original Present result is preserved.
      stopped.store(true, std::memory_order_release);
    }
  }

  ~BridgeSourcePresent() {
    if (!finished) {
      failure = "source_present_exception";
      Finish(E_FAIL);
    }
  }
};

struct BridgeProxyPresent {
  BridgeSourcePresent* source = nullptr;
  std::shared_ptr<Context> context;
  void* handle = nullptr;
  uintptr_t proxy = 0;
  uint64_t declaration = 0;
  uint64_t epoch = 0;
  IDXGISwapChain* borrowed_swapchain = nullptr;
  D3D12ProxyPresentObservation reverse;
  bool reverse_active = false;
  bool finished = false;

  BridgeProxyPresent(IDXGISwapChain* swapchain, UINT flags) noexcept : borrowed_swapchain(swapchain) {
    auto* current = BridgeSourcePresent::current;
    if ((flags & DXGI_PRESENT_TEST) != 0 || (current != nullptr && current->test)) return;
    bool unregistered_outer = false;
    // Function detours also enter for other objects sharing the method. Only
    // a registered source can select the forward association path.
    while (current != nullptr && current->root->registration == nullptr) {
      unregistered_outer = true;
      current = current->previous;
    }
    if (current == nullptr && d3d12_source_observer.load(std::memory_order_acquire) == nullptr) return;
    if (current != nullptr) {
      source = current->root;
    }
    try {
      proxy = Identity(swapchain);
      if (source != nullptr) {
        ++source->proxy_calls;
      }
      {
        std::shared_lock lock(state.mutex);
        for (const auto& [candidate_handle, candidate] : state.contexts) {
          if (proxy == 0 || candidate->proxy_identity != proxy) continue;
          if (context != nullptr || !candidate->bridge_context_ready || !candidate->bridge_proxy_ready) {
            if (source != nullptr) {
              source->failure = "proxy_context_ambiguous_or_unready";
            }
            return;
          }
          context = candidate;
          handle = candidate_handle;
          declaration = context->bridge_declaration;
          epoch = context->epoch;
        }
        if (source == nullptr && context != nullptr && context->bridge_application_device11 == nullptr
            && context->bridge_proxy_device11 == nullptr) {
          reverse.device = context->device_identity.Get();
          if (const auto old = state.bridge_swapchains.find(context->bridge_source_swapchain);
              old != state.bridge_swapchains.end()) {
            reverse.mapping.registration = old->second;
          }
        }
      }
      if (source != nullptr) {
        if (context == nullptr) {
          source->failure = "proxy_context_unobserved";
        }
      } else if (reverse.device != nullptr) {
        if (reverse.previous != nullptr) {
          reverse.previous->mapping.failure = "multiple_proxy_presents";
          return;
        }
        reverse.mapping.context = context;
        reverse.mapping.context_handle = handle;
        reverse.mapping.proxy = proxy;
        reverse.mapping.declaration = declaration;
        reverse.mapping.epoch = epoch;
        reverse.mapping.proxy_outer = true;
        reverse.mapping.unregistered_outer = unregistered_outer;
        const auto observer = d3d12_source_observer.load(std::memory_order_acquire);
        if (observer != nullptr) {
          reverse.mapping.before = observer(reinterpret_cast<uintptr_t>(reverse.device));
        }
        D3D12ProxyPresentObservation::current = &reverse;
        reverse_active = true;
      }
    } catch (...) {
      if (source != nullptr) {
        source->failure = "proxy_present_observation_failed";
      }
    }
  }

  void Finish(HRESULT status) noexcept {
    finished = true;
    if (reverse_active) {
      D3D12ProxyPresentObservation::current = reverse.previous;
      reverse_active = false;
      try {
        if (FAILED(status) && reverse.mapping.failure == nullptr) {
          reverse.mapping.failure = "proxy_present_failed";
        }
        if (reverse.mapping.failure == nullptr && (reverse.source_calls != 1 || !reverse.source_finished)) {
          reverse.mapping.failure = "nested_source_unobserved";
        }
        if (reverse.mapping.failure == nullptr && Identity(borrowed_swapchain) != proxy) {
          reverse.mapping.failure = "proxy_changed_during_present";
        }
        BridgeSourceObservation after;
        const auto observer = d3d12_source_observer.load(std::memory_order_acquire);
        if (observer != nullptr) {
          after = observer(reinterpret_cast<uintptr_t>(reverse.device));
        }
        RecordSourcePresentMapping(reverse.mapping, after);
      } catch (...) {
        stopped.store(true, std::memory_order_release);
      }
      return;
    }
    if (source == nullptr) return;
    if (FAILED(status)) {
      source->failure = "proxy_present_failed";
    } else if (context != nullptr) {
      if (source->context != nullptr || source->proxy_calls != 1) {
        source->failure = "multiple_proxy_presents";
      } else {
        source->context = context;
        source->context_handle = handle;
        source->proxy = proxy;
        source->declaration = declaration;
        source->epoch = epoch;
      }
    }
  }

  ~BridgeProxyPresent() {
    if (reverse_active) {
      reverse.mapping.failure = "proxy_present_exception";
      Finish(E_FAIL);
    }
    if (!finished && source != nullptr) {
      source->failure = "proxy_present_exception";
    }
  }
};

struct SourcePresentSlot {
  std::array<void*, 2> real{};
  std::array<detour_guard::Record, 2> records;
  bool installed = false;
  bool complete = false;
};
inline std::array<SourcePresentSlot, 4> source_present_slots;

template <size_t Index>
struct SourcePresentHooks {
  static HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* swapchain, UINT interval, UINT flags) {
    BridgeSourcePresent present(swapchain, flags
#if defined(NR_NEXUS_DEV_CONTROL)
        , {.method = BridgePresentMethod::PRESENT,
            .return_address = reinterpret_cast<uintptr_t>(_ReturnAddress()),
            .thread_id = GetCurrentThreadId(), .proxy_entry_depth = proxy_entry_depth, .interval = interval, .flags = flags}
#endif
    );
    const auto status = reinterpret_cast<PresentFn>(source_present_slots[Index].real[0])(swapchain, interval, flags);
    present.Finish(status);
    return status;
  }
  static HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1* swapchain, UINT interval, UINT flags,
                                            const DXGI_PRESENT_PARAMETERS* parameters) {
    BridgeSourcePresent present(swapchain, flags
#if defined(NR_NEXUS_DEV_CONTROL)
        , {.method = BridgePresentMethod::PRESENT1,
            .return_address = reinterpret_cast<uintptr_t>(_ReturnAddress()),
            .thread_id = GetCurrentThreadId(), .proxy_entry_depth = proxy_entry_depth, .interval = interval, .flags = flags}
#endif
    );
    const auto status = reinterpret_cast<Present1Fn>(source_present_slots[Index].real[1])(swapchain, interval, flags, parameters);
    present.Finish(status);
    return status;
  }
};

template <size_t Index>
inline std::array<void*, 2> SourcePresentDetours() {
  return {reinterpret_cast<void*>(&SourcePresentHooks<Index>::Present), reinterpret_cast<void*>(&SourcePresentHooks<Index>::Present1)};
}

inline bool InstallSourcePresent(IDXGISwapChain* swapchain) {
  auto** table = *reinterpret_cast<void***>(swapchain);
  std::array<void*, 2> targets{table[8], nullptr};
  ComPtr<IDXGISwapChain1> swapchain1;
  if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain1)))) {
    targets[1] = (*reinterpret_cast<void***>(swapchain1.Get()))[22];
  }
  std::unique_lock install(install_mutex);
  for (const auto& slot : proxies) {
    if (slot.installed && slot.records[0].target == targets[0]) return false;
  }
  for (size_t index = 0; index < source_present_slots.size(); ++index) {
    auto& slot = source_present_slots[index];
    if (slot.installed) {
      if (slot.records[0].target == targets[0]) return slot.complete;
      continue;
    }
    if (!PinRuntimeCallback()) return false;
    // These method implementations and our callback must remain mapped while
    // a code detour can be reached. This does not retain the swapchain object.
    for (const auto target : targets) {
      HMODULE pinned = nullptr;
      if (target != nullptr && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
          reinterpret_cast<LPCWSTR>(target), &pinned)) return false;
    }
    slot.real = targets;
    const auto detours = (index == 0 ? SourcePresentDetours<0>() : index == 1 ? SourcePresentDetours<1>()
        : index == 2 ? SourcePresentDetours<2>() : SourcePresentDetours<3>());
    std::array<detour_guard::Hook, 2> hooks;
    for (size_t i = 0; i < hooks.size(); ++i) hooks[i] = {
        .name = "Bridge source Present", .real = &slot.real[i], .detour = detours[i], .record = &slot.records[i]};
    size_t failed = 0;
    const auto attached = detour_guard::AttachAll(hooks, &failed);
    slot.installed = attached != 0;
    slot.complete = attached == (targets[1] != nullptr ? 2u : 1u);
    return slot.complete;
  }
  return false;
}

struct PresentFrame {
  std::shared_ptr<Context> context;
  std::shared_ptr<Frame> frame;
  std::shared_ptr<Frame> pending_frame;
  ComPtr<IDXGISwapChain3> swapchain;
  ComPtr<ID3D12Resource> backbuffer;
  FrameInput input;
  bool service_owned = false;
  bool service_eligible = false;
  bool continuing_submission = false;
  uint64_t service_stats_serial = 0;
  Result service_result = Result::NOT_SERVED;
  bool repeated = false;
#if defined(NR_NEXUS_DEV_CONTROL)
  edit_fg_observation::Ticket edit_fg_ticket;
  int64_t edit_fg_return_ns = 0;
#endif
};

#if defined(NR_NEXUS_DEV_CONTROL)
inline void ObserveEditPresent(PresentFrame* present, IDXGISwapChain* swapchain,
                               uint32_t method, UINT interval, UINT flags) {
  const auto generation = edit_fg_observation::CaptureGeneration();
  if (generation == 0 || present->context == nullptr) return;
  edit_fg_observation::XefgPresentObservation observation{
      .sdk_context = present->input.context, .context_identity = present->input.diagnostic_context,
      .epoch = present->input.epoch, .present_id = present->input.present_id,
      .has_present_id = present->input.has_present_id, .repeated = present->repeated,
      .proxy = edit_fg_observation::ObserveResource(swapchain),
      .method = method, .interval = interval, .flags = flags};
  {
    std::shared_lock lock(state.mutex);
    observation.generation = present->context->fg_environment_generation;
  }
  // BackbufferRegion carries coordinates, not a resource. Observe the actual
  // current proxy buffer separately, without recording any GPU commands.
  ComPtr<IDXGISwapChain3> swapchain3;
  ComPtr<ID3D12Resource> backbuffer;
  if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3)))
      && SUCCEEDED(swapchain3->GetBuffer(swapchain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)))) {
    observation.backbuffer = edit_fg_observation::ObserveResource(backbuffer.Get());
  }
  present->edit_fg_ticket = edit_fg_observation::Record(generation,
      {.kind = edit_fg_observation::Kind::XEFG_PRESENT_ENTRY, .xefg_present = observation});
}
#endif

inline PresentFrame BeginPresent(IDXGISwapChain* swapchain, UINT flags) {
  PresentFrame present;
  if ((flags & DXGI_PRESENT_TEST) != 0 || proxy_depth != 0) {
    if ((flags & DXGI_PRESENT_TEST) != 0) output_framerate::Invalidate(output_framerate::Reason::kTEST_PRESENT);
    return present;
  }
  const bool service_requested = Requested();
  const auto identity = Identity(swapchain);
  bool repeated = false;
  {
    std::unique_lock lock(state.mutex);
    for (const auto& [handle, context] : state.contexts) {
      if (context->proxy_identity != identity) continue;
      present.context = context;
      present.service_stats_serial = context->service_stats_serial;
      present.service_owned = service_requested && context->enabled;
      const bool has_present_id = context->fresh_id;
      repeated = has_present_id && context->last_id_known && context->last_id == context->present_id;
      present.repeated = repeated;
      if (has_present_id) {
        ++context->interval_tagged_presents;
        if (!repeated) {
          ++context->application_frame;
          context->last_id = context->present_id;
          context->last_id_known = true;
        }
      } else {
        ++context->interval_untagged_presents;
        // The outer swapchain Present still identifies a new application frame
        // when XeFG does not provide guides (for example, menus and cutscenes).
        ++context->application_frame;
      }
      present.input = {.queue = context->queue.Get(), .context = reinterpret_cast<uintptr_t>(handle),
          .epoch = context->epoch, .application_frame = context->application_frame,
          .present_id = context->present_id, .init_flags = context->init.flags,
          .ui_mode = static_cast<uint32_t>(context->init.ui_mode), .color_space = context->color_space,
          .has_present_id = has_present_id, .fg_enabled = context->enabled,
          .ui_composition = context->ui_composition,
          .diagnostic_context = reinterpret_cast<uintptr_t>(context.get())};
      present.service_eligible = present.service_owned && !repeated;
      present.continuing_submission = context->last_submission_frame != 0
          && context->last_submission_frame + 1 == present.input.application_frame;
      if (const auto it = context->frames.find(context->present_id); has_present_id && it != context->frames.end()) {
        if (!it->second->presenting) {
          it->second->presenting = true;
          present.pending_frame = it->second;
          present.frame = std::make_shared<Frame>(*it->second);
        }
      }
      context->fresh_id = false;
      break;
    }
  }
  if (present.context == nullptr) return present;
  // A previously substituted HUDLess remains an SDK input even if NR was
  // switched off or another input declined after its tag.
  const bool copies_ordered = OrderCopies(present.frame, present.context->queue.Get());
  if (!present.service_owned) return present;
  // A repeated tagged ID is not a new application frame. Untagged outer
  // Presents still use the current backbuffer with empty guide fields; the
  // Present service can apply its Optional-guide fallback without borrowing
  // resources or constants from an earlier XeFG frame.
  if (!present.service_eligible) return present;
  // A frame phase is not a service outage. Keep an adjacent proven service
  // active only while its next frame records, never after a declined return.
  display_result.store(present.continuing_submission ? Result::SUBMITTED : Result::PREPARED,
                       std::memory_order_release);
  result.store(Result::PREPARED, std::memory_order_release);
  const auto publish_result = [&](PresentFrame*) noexcept {
    PublishResult(present.service_result);
  };
  const std::unique_ptr<PresentFrame, decltype(publish_result)> completion(&present, publish_result);
  if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&present.swapchain)))
      || FAILED(present.swapchain->GetBuffer(present.swapchain->GetCurrentBackBufferIndex(),
                                            IID_PPV_ARGS(&present.backbuffer)))) return present;
  present.input.swapchain = present.swapchain.Get();
  present.input.backbuffer = present.backbuffer.Get();
  const auto description = present.backbuffer->GetDesc();
  present.input.color_width = static_cast<uint32_t>(description.Width);
  present.input.color_height = description.Height;
  if (present.frame != nullptr) {
    const auto& frame = *present.frame;
    if (frame.rejected) return present;
    if (frame.has_region) {
      present.input.color_x = frame.region.base.x;
      present.input.color_y = frame.region.base.y;
      present.input.color_width = frame.region.size.x;
      present.input.color_height = frame.region.size.y;
    }
    const auto& hudless = frame.resources[0];
    const auto& depth = frame.resources[1];
    const auto& motion = frame.resources[2];
    const auto& ui = frame.resources[3];
    // Only explicitly active NONE/BACKBUFFER_UITEXTURE modes document that
    // HUDLess is not used. With composition disabled, optional HUDLess tags
    // can still affect interpolation; preserve consistent NR inputs there.
    present.input.hudless = (present.input.ui_composition
        && (present.input.ui_mode == 1 || present.input.ui_mode == 2) ? nullptr : hudless.resource.Get());
    present.input.hudless_state = hudless.data.state;
    present.input.ui = ui.resource.Get();
    present.input.ui_state = ui.data.state;
    if (present.input.hudless != nullptr) {
      present.context->interval_input_hudless.fetch_add(1, std::memory_order_relaxed);
    }
    if (present.input.ui != nullptr) {
      present.context->interval_input_ui.fetch_add(1, std::memory_order_relaxed);
    }
    if (present.input.has_present_id && depth.resource && motion.resource && depth.data.size.x == motion.data.size.x
        && depth.data.size.y == motion.data.size.y && frame.has_constants) {
      present.input.depth = depth.resource.Get();
      present.input.depth_state = depth.data.state;
      present.input.motion = motion.resource.Get();
      present.input.motion_state = motion.data.state;
      present.input.depth_x = depth.data.base.x;
      present.input.depth_y = depth.data.base.y;
      present.input.motion_x = motion.data.base.x;
      present.input.motion_y = motion.data.base.y;
      present.input.guide_width = motion.data.size.x;
      present.input.guide_height = motion.data.size.y;
    }
    present.input.has_constants = frame.has_constants;
    present.input.motion_scale_x = frame.constants.motion_scale_x;
    present.input.motion_scale_y = frame.constants.motion_scale_y;
    present.input.jitter_x = frame.constants.jitter_x;
    present.input.jitter_y = frame.constants.jitter_y;
    present.input.frame_time_ms = frame.constants.frame_time_ms;
    present.input.reset = frame.constants.reset != 0;
  }
  const auto callback = service.load(std::memory_order_acquire);
  if (callback != nullptr && copies_ordered) {
    present.service_result = callback(present.input);
  }
  return present;
}

inline void EndPresent(PresentFrame* present, HRESULT present_result = E_FAIL) {
  if (present->context == nullptr) return;
  uint64_t value = 0;
  bool needs_consumer_fence = false;
  {
    std::unique_lock lock(state.mutex);
    needs_consumer_fence = present->frame != nullptr && !present->frame->captures.empty();
    if (needs_consumer_fence) value = ++present->context->next_fence;
  }
  // Intel permits tagged input reuse after matching proxy Present. This
  // marker orders our copies behind that public consumption boundary.
  const bool signaled = !needs_consumer_fence
      || SUCCEEDED(present->context->queue->Signal(present->context->fence.Get(), value));
  abi::PresentStatus status{};
#if defined(NR_NEXUS_DEV_CONTROL)
  const bool status_queried = present_result == S_OK && present->context->get_status != nullptr;
  const int32_t status_result = (status_queried
      ? present->context->get_status(reinterpret_cast<void*>(present->input.context), &status) : 0);
  const bool has_status = status_queried && status_result == 0;
  edit_fg_observation::RecordResult(present->edit_fg_ticket, present_result, present->edit_fg_return_ns,
      {.queried = status_queried, .api_result = status_result, .frames_presented = status.frames_presented,
          .frame_gen_result = status.frame_gen_result, .enabled = status.enabled});
#else
  const bool has_status = present_result == S_OK && present->context->get_status != nullptr
      && present->context->get_status(reinterpret_cast<void*>(present->input.context), &status) == 0;
#endif
  std::unique_lock lock(state.mutex);
  if (present->frame != nullptr) {
    for (const auto& copy : present->frame->captures) {
      copy->consumer_fence = present->context->fence;
      copy->consumer_value = value;
      copy->unproven = !signaled;
      copy->reserved = false;
    }
    if (const auto it = present->context->frames.find(present->input.present_id);
        it != present->context->frames.end() && it->second == present->pending_frame) {
      present->context->frames.erase(it);
    }
  }
  auto& context = *present->context;
  output_framerate::ObserveXeFG(reinterpret_cast<uintptr_t>(present->context.get()),
      (context.epoch << 32) ^ context.framerate_configuration,
      output_framerate::NowNs(), present_result == S_OK, has_status && status.frame_gen_result >= 0,
      present->repeated, status.frames_presented,
      (context.requested_interpolated_known ? context.requested_interpolated + 1 : 0),
      (has_status ? status.frames_presented : 0), context.init.max_interpolated + 1,
      has_status && status.enabled == 0);
  ++context.interval_presents;
  context.interval_submitted += present->service_result == Result::SUBMITTED;
  if (present->service_result == Result::SUBMITTED) {
    submitted_frames.fetch_add(1, std::memory_order_relaxed);
    if (context.epoch == present->input.epoch) context.last_submission_frame = present->input.application_frame;
  }
  if (has_status) {
    context.interval_displayed += status.frames_presented;
    ++context.interval_status_samples;
  }
  const auto now = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(now - context.interval_start).count();
  if (seconds >= 5.0) {
    const auto nr_frames = context.interval_nr_frames.exchange(0, std::memory_order_relaxed);
    const auto nr_processes = context.interval_nr_processes.exchange(0, std::memory_order_relaxed);
    const auto nr_evaluates = context.interval_nr_evaluates.exchange(0, std::memory_order_relaxed);
    const auto nr_hudless = context.interval_nr_hudless_processes.exchange(0, std::memory_order_relaxed);
    const auto nr_backbuffer = context.interval_nr_backbuffer_processes.exchange(0, std::memory_order_relaxed);
    const auto nr_two_color = context.interval_nr_two_color_frames.exchange(0, std::memory_order_relaxed);
    const auto nr_composed = context.interval_nr_composed_frames.exchange(0, std::memory_order_relaxed);
    const auto hudless_blocked = context.interval_hudless_blocked.exchange(0, std::memory_order_relaxed);
    const auto hudless_sdk_calls = context.interval_hudless_sdk_calls.exchange(0, std::memory_order_relaxed);
    const auto hudless_sdk_success = context.interval_hudless_sdk_success.exchange(0, std::memory_order_relaxed);
    const auto input_hudless = context.interval_input_hudless.exchange(0, std::memory_order_relaxed);
    const auto input_ui = context.interval_input_ui.exchange(0, std::memory_order_relaxed);
    const auto tagged_presents = context.interval_tagged_presents;
    const auto untagged_presents = context.interval_untagged_presents;
    // `last` describes this completed Present, while nr_service remains an
    // interval total. Declined or failed frames never inherit an earlier route.
    const char* last_nr_route = "not_processed";
    const char* last_nr_route_reason = "not_served";
    if (!present->service_owned) {
      last_nr_route_reason = (present->input.fg_enabled ? "nr_off" : "fg_off");
    } else if (present->service_result == Result::FAILED) {
      last_nr_route_reason = "failed";
    } else if (present->service_result == Result::SUBMITTED) {
      last_nr_route_reason = "service_stats_unavailable";
      // Multiple overlapping recordings cannot be attributed to this Present.
      if (context.service_stats_serial == present->service_stats_serial + 1) {
        if (context.service_hudless_processes != 0 && context.service_backbuffer_processes != 0) {
          last_nr_route = "dual_chain";
          last_nr_route_reason = (present->input.ui_composition ? "ui_composition"
              : (present->input.ui_mode != kUiModeAuto ? "ui_mode" : "manual_dual"));
        } else if (context.service_hudless_processes == 0 && context.service_backbuffer_processes != 0) {
          last_nr_route = "single_backbuffer";
          if (present->input.ui_composition && (present->input.ui_mode == 1 || present->input.ui_mode == 2)) {
            last_nr_route_reason = "ui_mode";
          } else if (context.omit_optional_hudless && !present->input.ui_composition) {
            last_nr_route_reason = "single_color_default";
          } else {
            last_nr_route_reason = "hudless_unavailable";
          }
        } else if (context.service_hudless_processes != 0 && context.service_composed) {
          last_nr_route = "single_hudless_composed";
          last_nr_route_reason = "ui_composition";
        }
      }
    }
    char message[1536];
    std::snprintf(message, sizeof(message),
        "DLSS5 FG interval context=%llx seconds=%.3f application_presents=%llu tagged_presents=%llu "
        "untagged_presents=%llu nr_submitted=%llu "
        "status_samples=%llu displayed_frames=%llu application_fps=%.2f displayed_fps=%.2f last_result=%u; "
        "ui_mode=%u ui_composition=%u nr_colors=%u sdk_fg_enabled=%u fresh_present_id=%u "
        "nr_service[frames=%llu process=%llu evals=%llu hudless=%llu backbuffer=%llu two_color=%llu composed=%llu]; "
        "hudless_contract[blocked=%llu sdk_calls=%llu sdk_success=%llu input=%llu ui=%llu]; "
        "last_nr_route=%s last_nr_route_reason=%s; "
        "public_SDK_status_not_visual_acceptance",
        static_cast<unsigned long long>(present->input.context), seconds,
        static_cast<unsigned long long>(context.interval_presents),
        static_cast<unsigned long long>(tagged_presents),
        static_cast<unsigned long long>(untagged_presents),
        static_cast<unsigned long long>(context.interval_submitted),
        static_cast<unsigned long long>(context.interval_status_samples),
        static_cast<unsigned long long>(context.interval_displayed), context.interval_presents / seconds,
        context.interval_displayed / seconds, static_cast<unsigned>(present->service_result), present->input.ui_mode,
        static_cast<unsigned>(present->input.ui_composition), present->input.hudless != nullptr ? 2u : 1u,
        has_status ? status.enabled : 0u, static_cast<unsigned>(present->input.has_present_id),
        static_cast<unsigned long long>(nr_frames), static_cast<unsigned long long>(nr_processes),
        static_cast<unsigned long long>(nr_evaluates), static_cast<unsigned long long>(nr_hudless),
        static_cast<unsigned long long>(nr_backbuffer), static_cast<unsigned long long>(nr_two_color),
        static_cast<unsigned long long>(nr_composed), static_cast<unsigned long long>(hudless_blocked),
        static_cast<unsigned long long>(hudless_sdk_calls), static_cast<unsigned long long>(hudless_sdk_success),
        static_cast<unsigned long long>(input_hudless), static_cast<unsigned long long>(input_ui),
        last_nr_route, last_nr_route_reason);
    context.interval_start = now;
    context.interval_presents = context.interval_tagged_presents = context.interval_untagged_presents = 0;
    context.interval_displayed = context.interval_submitted = context.interval_status_samples = 0;
    lock.unlock();
    reshade::log::message(reshade::log::level::info, message);
  }
}

inline void BeforeResize(IDXGISwapChain* swapchain) {
  const auto identity = Identity(swapchain);
  const auto callback = retire.load(std::memory_order_acquire);
  if (callback != nullptr) callback();
  std::unique_lock lock(state.mutex);
  for (const auto& [handle, context] : state.contexts) {
    if (context->proxy_identity == identity) {
      ClearBridgeSourceObservationsLocked(context.get(), "proxy_resizing");
      ReleaseFrames(context.get());
    }
  }
  PublishResult(Result::NOT_SERVED);
}

struct ProxyEntryScope {
  ProxyEntryScope() { ++proxy_entry_depth; }
  ~ProxyEntryScope() { --proxy_entry_depth; }
};

template <size_t Index>
struct ProxyHooks {
  static HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* swapchain, UINT interval, UINT flags) {
    if (proxy_entry_depth != 0) {
      if (auto* reverse = D3D12ProxyPresentObservation::current; reverse != nullptr
          && (BridgeSourcePresent::current != nullptr || Identity(swapchain) != reverse->mapping.proxy)) {
        reverse->mapping.failure = "multiple_proxy_presents";
      }
      return reinterpret_cast<PresentFn>(proxies[Index].real[0])(swapchain, interval, flags);
    }
    const ProxyEntryScope entry;
    BridgeProxyPresent bridge_present(swapchain, flags);
    PresentFrame present;
    try { present = BeginPresent(swapchain, flags); } catch (...) { PublishResult(Result::FAILED); }
#if defined(NR_NEXUS_DEV_CONTROL)
    try { ObserveEditPresent(&present, swapchain, 1, interval, flags); }
    catch (...) { edit_fg_observation::Stop("observation_failed"); }
#endif
    if (present.service_owned) ++proxy_depth;
    const HRESULT status = reinterpret_cast<PresentFn>(proxies[Index].real[0])(swapchain, interval, flags);
#if defined(NR_NEXUS_DEV_CONTROL)
    if (present.edit_fg_ticket.call_id != 0) {
      present.edit_fg_return_ns = edit_fg_observation::NowNs();
    }
#endif
    if (present.service_owned) --proxy_depth;
    try { EndPresent(&present, status); } catch (...) { PublishResult(Result::FAILED); }
    bridge_present.Finish(status);
    return status;
  }
  static HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1* swapchain, UINT interval, UINT flags,
                                            const DXGI_PRESENT_PARAMETERS* parameters) {
    if (proxy_entry_depth != 0) {
      if (auto* reverse = D3D12ProxyPresentObservation::current; reverse != nullptr
          && (BridgeSourcePresent::current != nullptr || Identity(swapchain) != reverse->mapping.proxy)) {
        reverse->mapping.failure = "multiple_proxy_presents";
      }
      return reinterpret_cast<Present1Fn>(proxies[Index].real[1])(swapchain, interval, flags, parameters);
    }
    const ProxyEntryScope entry;
    BridgeProxyPresent bridge_present(swapchain, flags);
    PresentFrame present;
    try { present = BeginPresent(swapchain, flags); } catch (...) { PublishResult(Result::FAILED); }
#if defined(NR_NEXUS_DEV_CONTROL)
    try { ObserveEditPresent(&present, swapchain, 2, interval, flags); }
    catch (...) { edit_fg_observation::Stop("observation_failed"); }
#endif
    if (present.service_owned) ++proxy_depth;
    const HRESULT status = reinterpret_cast<Present1Fn>(proxies[Index].real[1])(swapchain, interval, flags, parameters);
#if defined(NR_NEXUS_DEV_CONTROL)
    if (present.edit_fg_ticket.call_id != 0) {
      present.edit_fg_return_ns = edit_fg_observation::NowNs();
    }
#endif
    if (present.service_owned) --proxy_depth;
    try { EndPresent(&present, status); } catch (...) { PublishResult(Result::FAILED); }
    bridge_present.Finish(status);
    return status;
  }
  static HRESULT STDMETHODCALLTYPE Resize(IDXGISwapChain* swapchain, UINT count, UINT width, UINT height,
                                          DXGI_FORMAT format, UINT flags) {
    try { BeforeResize(swapchain); } catch (...) { PublishResult(Result::FAILED); }
    return reinterpret_cast<ResizeFn>(proxies[Index].real[2])(swapchain, count, width, height, format, flags);
  }
  static HRESULT STDMETHODCALLTYPE Resize1(IDXGISwapChain3* swapchain, UINT count, UINT width, UINT height,
                                           DXGI_FORMAT format, UINT flags, const UINT* masks, IUnknown* const* queues) {
    try { BeforeResize(swapchain); } catch (...) { PublishResult(Result::FAILED); }
    return reinterpret_cast<Resize1Fn>(proxies[Index].real[3])(swapchain, count, width, height, format, flags, masks, queues);
  }
  static HRESULT STDMETHODCALLTYPE Color(IDXGISwapChain3* swapchain, DXGI_COLOR_SPACE_TYPE color) {
    const auto status = reinterpret_cast<ColorFn>(proxies[Index].real[4])(swapchain, color);
    if (SUCCEEDED(status)) {
      const auto identity = Identity(swapchain);
      std::unique_lock lock(state.mutex);
      for (const auto& [handle, context] : state.contexts) {
        if (context->proxy_identity == identity) {
          if (context->color_space != color) ReleaseFrames(context.get());
          context->color_space = color;
        }
      }
    }
    return status;
  }
};

template <size_t Index>
inline std::array<void*, 5> ProxyDetours() {
  return {reinterpret_cast<void*>(&ProxyHooks<Index>::Present), reinterpret_cast<void*>(&ProxyHooks<Index>::Present1),
      reinterpret_cast<void*>(&ProxyHooks<Index>::Resize), reinterpret_cast<void*>(&ProxyHooks<Index>::Resize1),
      reinterpret_cast<void*>(&ProxyHooks<Index>::Color)};
}

inline bool InstallProxy(IUnknown* object, HMODULE sdk_module) {
  ComPtr<IDXGISwapChain3> proxy;
  if (object == nullptr || FAILED(object->QueryInterface(IID_PPV_ARGS(&proxy)))) return false;
  auto** table = *reinterpret_cast<void***>(proxy.Get());
  const std::array<void*, 5> targets{table[8], table[22], table[13], table[39], table[38]};
  std::unique_lock install(install_mutex);
  for (size_t index = 0; index < proxies.size(); ++index) {
    auto& slot = proxies[index];
    if (slot.installed) {
      if (slot.records[0].target == targets[0]) return slot.complete;
      continue;
    }
    // Install inside OptiScaler's later FGPresent detour. Attaching for the
    // first time over an existing detour would run before its final submits.
    // Public COM method code may have a linker thunk within the SDK image;
    // a jump outside that image cannot establish this boundary.
    for (size_t method : {size_t{0}, size_t{1}}) {
      auto* code = static_cast<uint8_t*>(targets[method]);
      for (size_t hop = 0; hop < 4; ++hop) {
        uint8_t bytes[6] = {};
        if (!detour_guard::ReadCode(code, bytes, sizeof(bytes))) return false;
        HMODULE owner = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(code), &owner) || owner != sdk_module) return false;
        if (bytes[0] == 0xe9) {
          int32_t displacement = 0;
          std::memcpy(&displacement, bytes + 1, sizeof(displacement));
          code += 5 + displacement;
        } else if (bytes[0] == 0xeb) {
          code += 2 + static_cast<int8_t>(bytes[1]);
        } else if (bytes[0] == 0xff && bytes[1] == 0x25) {
          return false;
        } else {
          break;
        }
        if (hop == 3) return false;
      }
    }
    slot.real = targets;
    const auto detours = (index == 0 ? ProxyDetours<0>() : index == 1 ? ProxyDetours<1>()
        : index == 2 ? ProxyDetours<2>() : ProxyDetours<3>());
    std::array<detour_guard::Hook, 5> hooks;
    for (size_t i = 0; i < hooks.size(); ++i) hooks[i] = {
        .name = "XeFG proxy", .real = &slot.real[i], .detour = detours[i], .record = &slot.records[i]};
    size_t failed = 0;
    const auto attached = detour_guard::AttachAll(hooks, &failed);
    slot.installed = attached != 0;
    slot.complete = attached == hooks.size();
    return slot.complete;
  }
  return false;
}

inline constexpr std::array EXPORTS{
    "xefgSwapChainD3D12InitFromSwapChain", "xefgSwapChainD3D12InitFromSwapChainDesc",
    "xefgSwapChainD3D12GetSwapChainPtr", "xefgSwapChainD3D12TagFrameResource", "xefgSwapChainTagFrameConstants",
    "xefgSwapChainSetPresentId", "xefgSwapChainSetEnabled", "xefgSwapChainSetUiCompositionState", "xefgSwapChainDestroy",
    "xefgSwapChainSetNumInterpolatedFrames"
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
    , "xefgSwapChainEnableDebugFeature"
#endif
};
struct SdkSlot {
  HMODULE module = nullptr;
  std::array<void*, EXPORTS.size()> real{};
  std::array<detour_guard::Record, EXPORTS.size()> records;
  bool installed = false;
  bool complete = false;
};
inline std::array<SdkSlot, 2> modules;

template <size_t Index>
struct SdkHooks {
  static int32_t Init(void* handle, ID3D12CommandQueue* queue, const abi::Init* parameters) {
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
    const std::lock_guard present_probe_lifecycle(xefg_present_probe::lifecycle_mutex);
    xefg_present_probe::InitScope present_probe_init(static_cast<uint32_t>(Index),
        modules[Index].module, handle, nullptr, nullptr,
        (parameters != nullptr ? parameters->application_swapchain : nullptr));
#endif
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
    const std::lock_guard probe_lifecycle(fg_output_probe::xefg_lifecycle_mutex);
    const fg_output_probe::XefgInitScope probe_init(static_cast<uint32_t>(Index), reinterpret_cast<uintptr_t>(handle));
    if (probe_init.owner && !stopped.load()) {
      fg_output_probe::BeginXefg(static_cast<uint32_t>(Index), reinterpret_cast<uintptr_t>(modules[Index].module),
          reinterpret_cast<uintptr_t>(handle));
    }
#endif
    if (!stopped.load()) PinRuntimeCallback();
    InvalidateBridgeOff(handle, "context_initializing", true);
    const auto status = reinterpret_cast<int32_t (*)(void*, ID3D12CommandQueue*, const abi::Init*)>(modules[Index].real[0])(
        handle, queue, parameters);
    if (status == 0 && modules[Index].complete && !stopped.load()) {
      try {
        Initialize(handle, queue, parameters, modules[Index].module, "Init"
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
                   , (probe_init.owner ? static_cast<uint32_t>(Index) : UINT32_MAX)
#endif
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
                   , present_probe_init.serial
#endif
                   );
      } catch (...) { PublishResult(Result::FAILED); }
    }
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
    if (probe_init.owner) {
      fg_output_probe::FinishXefgInit(static_cast<uint32_t>(Index), reinterpret_cast<uintptr_t>(handle), status);
    }
#endif
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
    present_probe_init.Finish(status);
#endif
    return status;
  }
  static int32_t InitDesc(void* handle, HWND window, const DXGI_SWAP_CHAIN_DESC1* description,
                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, ID3D12CommandQueue* queue,
                          IDXGIFactory2* factory, const abi::Init* parameters) {
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
    const std::lock_guard present_probe_lifecycle(xefg_present_probe::lifecycle_mutex);
    xefg_present_probe::InitScope present_probe_init(static_cast<uint32_t>(Index),
        modules[Index].module, handle, factory, window,
        (parameters != nullptr ? parameters->application_swapchain : nullptr));
#endif
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
    const std::lock_guard probe_lifecycle(fg_output_probe::xefg_lifecycle_mutex);
    const fg_output_probe::XefgInitScope probe_init(static_cast<uint32_t>(Index), reinterpret_cast<uintptr_t>(handle));
    if (probe_init.owner && !stopped.load()) {
      fg_output_probe::BeginXefg(static_cast<uint32_t>(Index), reinterpret_cast<uintptr_t>(modules[Index].module),
          reinterpret_cast<uintptr_t>(handle));
    }
#endif
    if (!stopped.load()) PinRuntimeCallback();
    InvalidateBridgeOff(handle, "context_initializing", true);
    const auto status = reinterpret_cast<int32_t (*)(void*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, ID3D12CommandQueue*, IDXGIFactory2*, const abi::Init*)>(modules[Index].real[1])(
        handle, window, description, fullscreen, queue, factory, parameters);
    if (status == 0 && modules[Index].complete && !stopped.load()) {
      try {
        Initialize(handle, queue, parameters, modules[Index].module, "InitDesc"
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
                   , (probe_init.owner ? static_cast<uint32_t>(Index) : UINT32_MAX)
#endif
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
                   , present_probe_init.serial
#endif
                   );
      } catch (...) { PublishResult(Result::FAILED); }
    }
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
    if (probe_init.owner) {
      fg_output_probe::FinishXefgInit(static_cast<uint32_t>(Index), reinterpret_cast<uintptr_t>(handle), status);
    }
#endif
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
    present_probe_init.Finish(status);
#endif
    return status;
  }
  static int32_t GetSwapchain(void* handle, REFIID iid, void** output) {
    if (!stopped.load()) PinRuntimeCallback();
    const auto declaration = InvalidateBridgeOff(handle, "proxy_observation_changed", false, true);
    const auto status = reinterpret_cast<int32_t (*)(void*, REFIID, void**)>(modules[Index].real[2])(handle, iid, output);
    if (status == 0 && output != nullptr && *output != nullptr && !stopped.load()) {
      try {
        if (!InstallProxy(static_cast<IUnknown*>(*output), modules[Index].module)) {
          reshade::log::message(reshade::log::level::warning,
              "DLSS5 FG: SDK proxy boundary unavailable; frame remains unserved");
          return status;
        }
        const auto identity = Identity(static_cast<IUnknown*>(*output));
        BridgeDeviceObservation device_observation;
        auto device11 = BridgeDeviceIdentity(static_cast<IUnknown*>(*output), &device_observation);
        const bool proxy_ready = BridgeDeviceCaptureReady(device_observation, device11.Get(), false);
        ComPtr<IUnknown> retired_device11;
        {
          std::unique_lock lock(state.mutex);
          if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
            it->second->proxy_identity = identity;
            if (it->second->bridge_declaration == declaration) {
              retired_device11 = std::move(it->second->bridge_proxy_device11);
              it->second->bridge_proxy_device11 = std::move(device11);
              it->second->bridge_proxy_observation = device_observation;
              it->second->bridge_proxy_ready = proxy_ready;
            } else {
              it->second->bridge_off_observed = false;
              it->second->bridge_proxy_ready = false;
              it->second->bridge_off_reason = "proxy_changed_during_observation";
              it->second->bridge_declaration = ++state.bridge_declaration;
            }
          }
        }
        RefreshOwnership();
      } catch (...) { PublishResult(Result::FAILED); }
    }
    return status;
  }
  static int32_t Resource(void* handle, ID3D12CommandList* list, uint32_t id, const abi::Resource* data) {
    if (!stopped.load()) PinRuntimeCallback();
    std::shared_ptr<Context> context;
    std::shared_ptr<Frame> frame;
    TaggedResource tagged;
    abi::Resource replacement{};
    const abi::Resource* forwarded = data;
    bool omitted = false;
    try {
      // Only AUTO without UI composition may omit the optional color. NR Off
      // always forwards it; a frame tagged before NR resumes must remain
      // unprocessed because the SDK may already own an ONLY_NOW color copy.
      if (Requested()
          && !stopped.load(std::memory_order_acquire)
          && data != nullptr && data->type == 0 && data->resource != nullptr
          && (data->validity == 0 || (data->validity == 1 && list != nullptr))) {
        std::shared_lock lock(state.mutex);
        if (const auto it = state.contexts.find(handle); it != state.contexts.end()
            && it->second->omit_optional_hudless && it->second->enabled && !it->second->ui_composition) {
          const auto description = data->resource->GetDesc();
          const auto tagged_frame = it->second->frames.find(id);
          const bool already_forwarded = tagged_frame != it->second->frames.end()
              && (tagged_frame->second->resources[0].resource != nullptr || tagged_frame->second->rejected);
          if (!already_forwarded && description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
              && description.DepthOrArraySize == 1 && description.SampleDesc.Count == 1
              && data->size.x != 0 && data->size.y != 0
              && uint64_t{data->base.x} + data->size.x <= description.Width
              && uint64_t{data->base.y} + data->size.y <= description.Height) {
            it->second->interval_hudless_blocked.fetch_add(1, std::memory_order_relaxed);
            omitted = true;
          }
        }
      }
      if (omitted) {
        TraceResourceContract(handle, list, id, data, nullptr, false, 0);
        return 0;
      }
      if (data != nullptr && (Requested() || data->type == 0)) {
        {
          std::unique_lock lock(state.mutex);
          if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
            context = it->second;
            frame = GetFrame(context.get(), id);
            if (frame != nullptr && !Requested()) frame->rejected = true;
          }
        }
        if (Requested() && frame != nullptr && data->type >= 0 && data->type < 4 && data->resource != nullptr
            && (data->validity == 0 || data->validity == 1)) {
          tagged.data = *data;
          tagged.resource = data->resource;
          const auto description = data->resource->GetDesc();
          if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.DepthOrArraySize != 1
              || description.SampleDesc.Count != 1 || data->size.x == 0 || data->size.y == 0
              || uint64_t{data->base.x} + data->size.x > description.Width
              || uint64_t{data->base.y} + data->size.y > description.Height) {
            tagged.resource.Reset();
          } else if (data->validity == 1) {
            ComPtr<ID3D12GraphicsCommandList> graphics;
            const auto prepare = prepare_capture.load(std::memory_order_acquire);
            if (list != nullptr && SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&graphics)))
                && prepare != nullptr && prepare(graphics.Get())) {
              tagged.copy = AcquireSurface(context, description);
            }
            if (tagged.copy != nullptr) {
              {
                std::unique_lock lock(state.mutex);
                frame->captures.push_back(tagged.copy);
              }
              // Retention is registered before recording. If a later SDK tag
              // fails, these GPU operands still belong to the original list.
              submission::TrackUse(graphics.Get(), tagged.copy->texture.Get());
              std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
              for (auto& barrier : barriers) {
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.Subresource = 0;
              }
              barriers[0].Transition.pResource = data->resource;
              barriers[0].Transition.StateBefore = data->state;
              barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
              barriers[1].Transition.pResource = tagged.copy->texture.Get();
              barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
              barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
              if (data->state != D3D12_RESOURCE_STATE_COPY_SOURCE) graphics->ResourceBarrier(1, &barriers[0]);
              graphics->ResourceBarrier(1, &barriers[1]);
              D3D12_TEXTURE_COPY_LOCATION source{};
              source.pResource = data->resource;
              source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
              D3D12_TEXTURE_COPY_LOCATION destination{};
              destination.pResource = tagged.copy->texture.Get();
              destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
              graphics->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
              for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
              if (data->state != D3D12_RESOURCE_STATE_COPY_SOURCE) graphics->ResourceBarrier(1, &barriers[0]);
              graphics->ResourceBarrier(1, &barriers[1]);
              tagged.resource = tagged.copy->texture;
              tagged.data.resource = tagged.resource.Get();
              tagged.data.state = D3D12_RESOURCE_STATE_COMMON;
              tagged.data.validity = 0;
              if (data->type == 0) {
                // The SDK must consume the owned color after late NR, rather
                // than its earlier private ONLY_NOW copy of unprocessed color.
                replacement = tagged.data;
                forwarded = &replacement;
              }
            } else {
              tagged.resource.Reset();
            }
          }
        }
      }
    } catch (...) { tagged.resource.Reset(); PublishResult(Result::FAILED); }
#if defined(NR_NEXUS_DEV_CONTROL)
    edit_fg_observation::Ticket edit_fg_ticket;
    try {
      if (const auto generation = edit_fg_observation::CaptureGeneration(); generation != 0) {
        edit_fg_observation::XefgTagObservation observation{
            .sdk_slot = static_cast<uint32_t>(Index), .sdk_context = reinterpret_cast<uintptr_t>(handle), .present_id = id,
            .recording = {.raw = reinterpret_cast<uintptr_t>(list)}};
        {
          std::shared_lock lock(state.mutex);
          if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
            observation.context_identity = reinterpret_cast<uintptr_t>(it->second.get());
            observation.epoch = it->second->epoch;
            observation.generation = it->second->fg_environment_generation;
            observation.ui_mode = static_cast<uint32_t>(it->second->init.ui_mode);
            observation.ui_composition = it->second->ui_composition;
          }
        }
        if (forwarded != nullptr) {
          observation.type = forwarded->type;
          observation.validity = forwarded->validity;
          observation.resource.raw = reinterpret_cast<uintptr_t>(forwarded->resource);
          observation.base_x = forwarded->base.x;
          observation.base_y = forwarded->base.y;
          observation.width = forwarded->size.x;
          observation.height = forwarded->size.y;
          observation.resource_state = static_cast<uint32_t>(forwarded->state);
          if ((forwarded->type == 0 || forwarded->type == 3) && forwarded->resource != nullptr
              && (forwarded->validity == 0 || forwarded->validity == 1)) {
            observation.resource = edit_fg_observation::ObserveResource(forwarded->resource);
            if (forwarded->validity == 1 && list != nullptr) {
              ComPtr<ID3D12GraphicsCommandList> graphics;
              if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&graphics)))) {
                observation.recording = edit_fg_observation::ObserveRecording(graphics.Get());
                observation.recording.raw = reinterpret_cast<uintptr_t>(list);
              }
            }
          }
        }
        edit_fg_ticket = edit_fg_observation::Record(generation,
            {.kind = edit_fg_observation::Kind::XEFG_TAG_ENTRY, .xefg_tag = observation});
      }
    } catch (...) {
      edit_fg_observation::Stop("observation_failed");
    }
#endif
    const auto status = reinterpret_cast<int32_t (*)(void*, ID3D12CommandList*, uint32_t, const abi::Resource*)>(
        modules[Index].real[3])(handle, list, id, forwarded);
#if defined(NR_NEXUS_DEV_CONTROL)
    if (edit_fg_ticket.call_id != 0) {
      const auto edit_fg_return_ns = edit_fg_observation::NowNs();
      edit_fg_observation::RecordResult(edit_fg_ticket, status, edit_fg_return_ns);
    }
#endif
    TraceResourceContract(handle, list, id, data, forwarded, true, status);
    if (data != nullptr && data->type == 0) {
      std::shared_lock lock(state.mutex);
      if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
        it->second->interval_hudless_sdk_calls.fetch_add(1, std::memory_order_relaxed);
        if (status == 0) it->second->interval_hudless_sdk_success.fetch_add(1, std::memory_order_relaxed);
      }
    }
    try {
      if (frame != nullptr && data != nullptr) {
        std::unique_lock lock(state.mutex);
        if (data->type == 4 && status == 0) {
          frame->region = *data;
          frame->has_region = true;
        } else if (data->type >= 0 && data->type < 4) {
          if (status == 0 && tagged.resource != nullptr) {
            frame->resources[data->type] = std::move(tagged);
          } else {
            frame->rejected = true;
          }
        }
        if (tagged.copy != nullptr) tagged.copy->reserved = false;
      }
    } catch (...) { PublishResult(Result::FAILED); }
    return status;
  }
  static int32_t Constants(void* handle, uint32_t id, const abi::Constants* data) {
    if (!stopped.load()) PinRuntimeCallback();
    const auto status = reinterpret_cast<int32_t (*)(void*, uint32_t, const abi::Constants*)>(modules[Index].real[4])(
        handle, id, data);
    if (Requested() && data != nullptr) {
      try {
        std::unique_lock lock(state.mutex);
        if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
          if (const auto frame = GetFrame(it->second.get(), id)) {
            frame->has_constants = status == 0;
            if (status == 0) frame->constants = *data;
          }
        }
      } catch (...) { PublishResult(Result::FAILED); }
    }
    return status;
  }
  static int32_t PresentId(void* handle, uint32_t id) {
    if (!stopped.load()) PinRuntimeCallback();
    const auto status = reinterpret_cast<int32_t (*)(void*, uint32_t)>(modules[Index].real[5])(handle, id);
    if (status == 0) {
      std::unique_lock lock(state.mutex);
      if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
        it->second->present_id = id;
        it->second->fresh_id = true;
      }
    }
    return status;
  }
  static int32_t Enable(void* handle, uint32_t enabled) {
    if (!stopped.load()) PinRuntimeCallback();
    std::shared_ptr<Context> attempt;
    uint64_t declaration = 0;
    uint64_t epoch = 0;
    uintptr_t device11 = 0;
    uintptr_t device12 = 0;
    uintptr_t proxy = 0;
    bool uses_source_link = false;
    uint64_t link_generation = 0;
    BridgeSourceIdentity linked_source;
#if defined(NR_NEXUS_DEV_CONTROL)
    uintptr_t source_swapchain = 0;
    uint64_t source_swapchain_generation = 0;
    uint64_t initial_link_generation = 0;
#endif
    {
      std::unique_lock lock(state.mutex);
      if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
        attempt = it->second;
        attempt->bridge_off_observed = false;
        attempt->bridge_before_observation = {};
        attempt->bridge_after_observation = {};
        attempt->bridge_off_reason = "set_enabled_in_flight";
        declaration = attempt->bridge_declaration = ++state.bridge_declaration;
        epoch = attempt->epoch;
        proxy = attempt->proxy_identity;
#if defined(NR_NEXUS_DEV_CONTROL)
        source_swapchain = attempt->bridge_source_swapchain;
        source_swapchain_generation = attempt->bridge_source_generation;
        initial_link_generation = attempt->bridge_link_generation;
#endif
        ++attempt->bridge_enable_inflight;
        auto* application_device = attempt->bridge_application_device11.Get();
        auto* proxy_device = attempt->bridge_proxy_device11.Get();
        if (application_device == nullptr || proxy_device == nullptr || application_device == proxy_device) {
          device11 = reinterpret_cast<uintptr_t>(application_device != nullptr ? application_device : proxy_device);
        }
        if (BridgeSourceLinkCurrentLocked(*attempt) && attempt->bridge_link_source.device12 != 0
            && attempt->bridge_link_source.device11 == 0 && application_device == nullptr && proxy_device == nullptr) {
          uses_source_link = true;
          linked_source = attempt->bridge_link_source;
          link_generation = attempt->bridge_link_generation;
          device12 = linked_source.device12;
        } else if (BridgeSourceLinkCurrentLocked(*attempt) && device11 != attempt->bridge_link_source.device11
            && (application_device == nullptr
                || reinterpret_cast<uintptr_t>(application_device) == attempt->bridge_link_source.device11)) {
          uses_source_link = true;
          linked_source = attempt->bridge_link_source;
          link_generation = attempt->bridge_link_generation;
          device11 = linked_source.device11;
        }
      }
    }
    const auto observer = (device12 != 0 ? d3d12_source_observer.load(std::memory_order_acquire)
                                         : bridge_source_observer.load(std::memory_order_acquire));
    const auto observed_device = (device12 != 0 ? device12 : device11);
    // The observer may try runtime_mutex; no XeFG state lock crosses it or the SDK.
    const auto before_observation = (observer != nullptr && observed_device != 0 && !stopped.load()
        ? observer(observed_device) : BridgeSourceObservation{});
    const auto before = before_observation.identity;
#if defined(NR_NEXUS_DEV_CONTROL)
    {
      std::unique_lock lock(state.mutex);
      RecordBridgeEventLocked({
          .kind = BridgeEventKind::ENABLE_BEGIN,
          .context_identity = reinterpret_cast<uintptr_t>(attempt.get()),
          .declaration = declaration,
          .epoch = epoch,
          .link_generation = initial_link_generation,
          .source_swapchain = source_swapchain,
          .source_swapchain_generation = source_swapchain_generation,
          .source = before,
          .reason = "set_enabled_in_flight",
          .enabled_request = enabled});
    }
#endif
    const auto status = reinterpret_cast<int32_t (*)(void*, uint32_t)>(modules[Index].real[6])(handle, enabled);
#if defined(NR_NEXUS_DEV_CONTROL)
    const auto sdk_return_ns = output_framerate::NowNs();
#endif
    const auto after_observation = (status == 0 && enabled == 0 && observer != nullptr && observed_device != 0 && !stopped.load()
        ? observer(observed_device) : BridgeSourceObservation{});
    const auto after = after_observation.identity;
    {
      std::unique_lock lock(state.mutex);
      if (attempt != nullptr && attempt->bridge_enable_inflight != 0) {
        --attempt->bridge_enable_inflight;
      }
      const auto it = state.contexts.find(handle);
      const bool same_context = it != state.contexts.end() && it->second == attempt;
      const bool current = same_context && attempt->bridge_context_ready && attempt->bridge_proxy_ready
          && attempt->bridge_declaration == declaration
          && attempt->bridge_enable_inflight == 0 && attempt->epoch == epoch && attempt->proxy_identity == proxy
          && (!uses_source_link || (BridgeSourceLinkCurrentLocked(*attempt)
              && attempt->bridge_link_generation == link_generation && attempt->bridge_link_source == linked_source));
      if (status == 0 && it != state.contexts.end()) {
        if (it->second->enabled != (enabled != 0)) {
          ReleaseFrames(it->second.get());
        }
        it->second->enabled = enabled != 0;
      }
      if (same_context) {
        if (attempt->bridge_declaration == declaration) {
          attempt->bridge_before_observation = before_observation;
          attempt->bridge_after_observation = after_observation;
        }
        if (current) {
          const bool source_device_matches = (device12 != 0
              ? before.device11 == 0 && before.device12 == device12
              : before.device12 == 0 && before.device11 == device11);
          attempt->bridge_off_reason = (status != 0 ? "set_enabled_failed"
              : enabled != 0 ? "enabled" : observed_device == 0 ? "device11_identity_missing_or_conflicting"
              : proxy == 0 ? "proxy_identity_missing"
              : state.contexts.size() != 1 ? "multiple_contexts"
              : before.handle == 0 || before.generation == 0 || !source_device_matches ? "source_missing_before_disable"
              : uses_source_link && before != linked_source ? "source_changed_since_mapping"
              : before != after ? "source_changed_during_disable" : "observed_disable");
          attempt->bridge_off_observed = status == 0 && enabled == 0 && observed_device != 0 && proxy != 0
              && before.handle != 0 && before.generation != 0
              && source_device_matches && before == after && (!uses_source_link || before == linked_source)
              && state.contexts.size() == 1 && !stopped.load();
          attempt->bridge_off_uses_source_link = uses_source_link;
          attempt->bridge_off_link_generation = link_generation;
          if (attempt->bridge_off_observed) {
            attempt->bridge_off_source = before;
            attempt->bridge_off_epoch = attempt->epoch;
          }
        } else if (attempt->bridge_declaration == declaration && attempt->bridge_enable_inflight == 0) {
          attempt->bridge_off_reason = (!attempt->bridge_context_ready ? "context_not_initialized"
              : !attempt->bridge_proxy_ready ? "proxy_contract_unavailable" : "context_changed_during_disable");
        }
      }
#if defined(NR_NEXUS_DEV_CONTROL)
      // The event reports this call's acceptance, independently of the context's latest declaration/reason.
      RecordBridgeEventLocked({
          .kind = BridgeEventKind::ENABLE_END,
          .sdk_return_ns = sdk_return_ns,
          .context_identity = reinterpret_cast<uintptr_t>(attempt.get()),
          .declaration = declaration,
          .epoch = (attempt != nullptr ? attempt->epoch : epoch),
          .link_generation = (attempt != nullptr ? attempt->bridge_link_generation : link_generation),
          .source_swapchain = (attempt != nullptr ? attempt->bridge_source_swapchain : source_swapchain),
          .source_swapchain_generation = (attempt != nullptr ? attempt->bridge_source_generation : source_swapchain_generation),
          .source = before,
          .reason = (current ? attempt->bridge_off_reason
                            : (attempt != nullptr ? "context_changed_during_disable" : "context_unobserved")),
          .enabled_request = enabled,
          .sdk_status = status,
          .off_observed = current && attempt->bridge_off_observed});
#endif
    }
    if (status == 0) {
      RefreshOwnership();
    }
    return status;
  }
  static int32_t UiState(void* handle, int32_t composition) {
    if (!stopped.load()) PinRuntimeCallback();
    const auto status = reinterpret_cast<int32_t (*)(void*, int32_t)>(modules[Index].real[7])(handle, composition);
    bool trace_call = false;
    bool applied = false;
    {
      std::unique_lock lock(state.mutex);
      if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
        if (status == 0) {
          if (it->second->ui_composition != (composition != 0)) ReleaseFrames(it->second.get());
          it->second->ui_composition = composition != 0;
        }
        applied = it->second->ui_composition;
        if (!stopped.load(std::memory_order_acquire) && sdk_detours_enabled.load(std::memory_order_acquire)) {
          trace_call = it->second->ui_trace.Observe(0, {composition, status, applied});
        }
      }
    }
    if (trace_call) {
      char message[256];
      std::snprintf(message, sizeof(message),
          "DLSS5 XeFG ui_composition_contract context=%llx requested=%d sdk_called=1 sdk_status=%d applied=%u nr_requested=%u budget=1+8_changes",
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(handle)), composition, status,
          static_cast<unsigned>(applied), static_cast<unsigned>(Requested()));
      LogDiagnostic(message);
    }
    return status;
  }
  static int32_t Destroy(void* handle) {
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
    const std::lock_guard present_probe_lifecycle(xefg_present_probe::lifecycle_mutex);
    xefg_present_probe::Observe([&] {
      xefg_present_probe::RestoreForHandle(static_cast<uint32_t>(Index), reinterpret_cast<uintptr_t>(handle));
      xefg_present_probe::Invalidate(static_cast<uint32_t>(Index), reinterpret_cast<uintptr_t>(handle), "context_destroying");
    });
#endif
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
    const std::lock_guard probe_lifecycle(fg_output_probe::xefg_lifecycle_mutex);
#endif
    if (!stopped.load()) PinRuntimeCallback();
    InvalidateBridgeOff(handle, "context_destroying", true);
    const auto status = reinterpret_cast<int32_t (*)(void*)>(modules[Index].real[8])(handle);
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
    xefg_present_probe::Observe([&] {
      xefg_present_probe::ContextDestroyed(static_cast<uint32_t>(Index), reinterpret_cast<uintptr_t>(handle), status);
    });
#endif
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
    fg_output_probe::DestroyXefg(static_cast<uint32_t>(Index), reinterpret_cast<uintptr_t>(handle), status);
#endif
    if (status == 0) {
      std::unique_lock lock(state.mutex);
      if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
        output_framerate::Invalidate();
        ReleaseFrames(it->second.get());
        // Keep recordings that can still replay alive; submission owns only
        // tokens, never COM references to their operands.
        for (auto& surface : it->second->surfaces) surface->unproven |= !submission::ResourceReleasable(surface->texture.Get());
        it->second->enabled = false;
        it->second->proxy_identity = 0;
        state.retired_contexts.push_back(it->second);
        state.contexts.erase(it);
      }
      lock.unlock();
      RefreshOwnership();
    }
    return status;
  }
  static int32_t NumInterpolated(void* handle, uint32_t count) {
    if (!stopped.load()) PinRuntimeCallback();
    const auto status = reinterpret_cast<int32_t (*)(void*, uint32_t)>(modules[Index].real[9])(handle, count);
    if (status == 0) {
      std::unique_lock lock(state.mutex);
      if (const auto it = state.contexts.find(handle); it != state.contexts.end()) {
        auto& context = *it->second;
        if (!context.requested_interpolated_known || context.requested_interpolated != count) {
          ++context.framerate_configuration;
          output_framerate::Invalidate();
        }
        context.requested_interpolated = count;
        context.requested_interpolated_known = true;
      }
    }
    return status;
  }
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
  static int32_t Debug(void* handle, int32_t feature, uint32_t enabled, void* argument) {
    if (!stopped.load()) {
      PinRuntimeCallback();
    }
    const auto caller_address = reinterpret_cast<uintptr_t>(_ReturnAddress());
    xefg_present_probe::DebugCall ticket;
    xefg_present_probe::Observe([&] {
      HMODULE caller_module = nullptr;
      std::string caller_path;
      if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
              reinterpret_cast<LPCWSTR>(caller_address), &caller_module)) {
        wchar_t path[512]{};
        const auto length = GetModuleFileNameW(caller_module, path, static_cast<DWORD>(std::size(path)));
        if (length != 0 && length < std::size(path)) {
          const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
          if (bytes > 0 && bytes <= 256) {
            caller_path.resize(bytes);
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, static_cast<int>(length), caller_path.data(), bytes, nullptr, nullptr);
          }
        }
      }
      ticket = xefg_present_probe::BeginDebug(static_cast<uint32_t>(Index),
          reinterpret_cast<uintptr_t>(modules[Index].module), reinterpret_cast<uintptr_t>(handle), feature, enabled, argument,
          caller_address, reinterpret_cast<uintptr_t>(caller_module), caller_path);
    });
    const auto result = reinterpret_cast<xefg_present_probe::DebugFn>(modules[Index].real[10])(handle, feature, enabled, argument);
    xefg_present_probe::Observe([&] { xefg_present_probe::FinishDebug(ticket, result); });
    return result;
  }
#endif
};

template <size_t Index>
inline std::array<void*, EXPORTS.size()> SdkDetours() {
  return {reinterpret_cast<void*>(&SdkHooks<Index>::Init), reinterpret_cast<void*>(&SdkHooks<Index>::InitDesc),
      reinterpret_cast<void*>(&SdkHooks<Index>::GetSwapchain), reinterpret_cast<void*>(&SdkHooks<Index>::Resource),
      reinterpret_cast<void*>(&SdkHooks<Index>::Constants), reinterpret_cast<void*>(&SdkHooks<Index>::PresentId),
      reinterpret_cast<void*>(&SdkHooks<Index>::Enable), reinterpret_cast<void*>(&SdkHooks<Index>::UiState),
      reinterpret_cast<void*>(&SdkHooks<Index>::Destroy), reinterpret_cast<void*>(&SdkHooks<Index>::NumInterpolated)
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
      , reinterpret_cast<void*>(&SdkHooks<Index>::Debug)
#endif
  };
}
}  // namespace internal

#if defined(NR_NEXUS_DEV_CONTROL)
// Snapshot only: reading events neither consumes them nor changes source/Off evidence.
inline BridgeEventSnapshot CopyBridgeEvents() {
  BridgeEventSnapshot snapshot;
  std::shared_lock lock(internal::state.mutex);
  snapshot.last_sequence = internal::state.bridge_event_sequence;
  snapshot.count = static_cast<uint32_t>(std::min<uint64_t>(snapshot.last_sequence, snapshot.events.size()));
  snapshot.overwritten = snapshot.last_sequence - snapshot.count;
  const auto first_sequence = snapshot.last_sequence - snapshot.count + 1;
  for (uint32_t index = 0; index < snapshot.count; ++index) {
    snapshot.events[index] = internal::state.bridge_events[(first_sequence + index - 1) % snapshot.events.size()];
  }
  return snapshot;
}

// Environment metadata only. It neither proves an SR-to-FG input link nor grants an Off receipt.
inline EnabledContextMetadata QueryEnabledContext() {
  EnabledContextMetadata metadata;
  if (internal::stopped.load(std::memory_order_acquire)
      || !internal::sdk_detours_enabled.load(std::memory_order_acquire)) {
    metadata.reason = "sdk_observation_unavailable";
    return metadata;
  }
  std::shared_lock lock(internal::state.mutex);
  if (internal::state.contexts.size() != 1) {
    metadata.reason = (internal::state.contexts.empty() ? "context_unobserved" : "multiple_contexts");
    return metadata;
  }
  const auto& [handle, context] = *internal::state.contexts.begin();
  metadata.context_identity = reinterpret_cast<uintptr_t>(context.get());
  metadata.epoch = context->epoch;
  metadata.generation = context->fg_environment_generation;
  if (!context->enabled) {
    metadata.reason = "fg_off";
  } else if (!context->bridge_context_ready || !context->bridge_proxy_ready || context->queue == nullptr
      || context->proxy_identity == 0 || context->bridge_enable_inflight != 0 || metadata.generation == 0) {
    metadata.reason = "context_not_ready";
  } else {
    metadata.available = true;
    metadata.reason = "enabled_context_environment";
  }
  return metadata;
}
#endif

inline void Configure(Service service, PrepareCapture capture, Retire retire, bool (*requested)() noexcept,
                      bool block_optional_hudless = false, bool enable_sdk_detours = true) {
  static std::atomic_bool disabled_logged{false};
  if (!enable_sdk_detours && !disabled_logged.exchange(true, std::memory_order_relaxed)) {
    internal::LogDiagnostic(
        "DLSS5 XeFG SDK detours disabled: no SDK/proxy interception; resource/UI contract trace unavailable");
  }
  internal::block_optional_hudless.store(block_optional_hudless, std::memory_order_release);
  internal::sdk_detours_enabled.store(enable_sdk_detours, std::memory_order_release);
  internal::service.store(service, std::memory_order_release);
  internal::prepare_capture.store(capture, std::memory_order_release);
  internal::retire.store(retire, std::memory_order_release);
  internal::requested.store(requested, std::memory_order_release);
}

inline void ConfigureBridgeSource(BridgeSourceObserver observer) noexcept {
  internal::bridge_source_observer.store(observer, std::memory_order_release);
}

inline void ConfigureD3D12Source(BridgeSourceObserver observer) noexcept {
  internal::d3d12_source_observer.store(observer, std::memory_order_release);
}

inline void ForgetBridgeSourceSwapchain(uintptr_t owner) noexcept {
  try {
    std::shared_ptr<internal::BridgeSourceSwapchain> removed;
    std::shared_ptr<internal::Context> context;
    {
      std::unique_lock lock(internal::state.mutex);
      const auto found = internal::state.bridge_swapchains.find(owner);
      if (found == internal::state.bridge_swapchains.end()) return;
      context = found->second->context.lock();
      if (context != nullptr && context->bridge_source_swapchain == owner
          && context->bridge_source_generation == found->second->generation) {
        internal::RevokeBridgeSourceLinkLocked(context.get(), "source_swapchain_removed");
      }
      removed = std::move(found->second);
      internal::state.bridge_swapchains.erase(found);
    }
  } catch (...) {
    internal::stopped.store(true, std::memory_order_release);
  }
}

namespace internal {
inline bool RegisterSourceSwapchain(uintptr_t owner, IDXGISwapChain* swapchain, bool native12) noexcept {
  ForgetBridgeSourceSwapchain(owner);
  if (owner == 0 || swapchain == nullptr || stopped.load() || !sdk_detours_enabled.load()) return false;
  try {
    renodx::utils::directx::NativeFromReShadeProxy(&swapchain);
    auto source = std::make_shared<BridgeSourceSwapchain>();
    source->owner = owner;
    source->identity = Identity(swapchain);
    if (native12) {
      source->device12 = D3D12SourceDeviceIdentity(swapchain);
    } else {
      source->device11 = BridgeDeviceIdentity(swapchain);
    }
    if (source->identity == 0 || (native12 ? source->device12 == nullptr : source->device11 == nullptr)) return false;
    source->hook_ready = InstallSourcePresent(swapchain);
    source->reason = (source->hook_ready ? "source_present_unobserved" : "source_hook_unavailable");
    const bool ready = source->hook_ready;
    std::shared_ptr<BridgeSourceSwapchain> replaced;
    std::shared_ptr<Context> replaced_context;
    {
      std::unique_lock lock(state.mutex);
      source->generation = ++state.bridge_source_generation;
      auto [found, inserted] = state.bridge_swapchains.try_emplace(owner, source);
      if (!inserted) {
        replaced = std::exchange(found->second, source);
        replaced_context = replaced->context.lock();
        if (replaced_context != nullptr && replaced_context->bridge_source_swapchain == owner
            && replaced_context->bridge_source_generation == replaced->generation) {
          RevokeBridgeSourceLinkLocked(replaced_context.get(), "source_swapchain_replaced");
        }
      }
      if (native12) {
        for (const auto& [handle, context] : state.contexts) {
          if (context->bridge_link_source.device12 == reinterpret_cast<uintptr_t>(source->device12.Get())
              && !BridgeSourceLinkCurrentLocked(*context)) {
            RevokeBridgeSourceLinkLocked(context.get(), "multiple_source_swapchains");
          }
        }
      }
    }
    return ready;
  } catch (...) {
    return false;
  }
}
}  // namespace internal

// DEV callers register the real API swapchain during init and revoke its
// owner token before every resize/destroy early return.
inline bool RegisterBridgeSourceSwapchain(uintptr_t owner, IDXGISwapChain* swapchain) noexcept {
  return internal::RegisterSourceSwapchain(owner, swapchain, false);
}

inline bool RegisterD3D12SourceSwapchain(uintptr_t owner, IDXGISwapChain* swapchain) noexcept {
  return internal::RegisterSourceSwapchain(owner, swapchain, true);
}

// runtime_mutex may already be held. This query reads the captured contract
// only: it never calls COM, the source observer, or an SDK entrypoint.
inline BridgeOffEvidence QueryBridgeOff(const BridgeSourceIdentity& source) {
  BridgeOffEvidence evidence;
  if (internal::stopped.load(std::memory_order_acquire)
      || !internal::sdk_detours_enabled.load(std::memory_order_acquire)) {
    evidence.reason = "sdk_observation_unavailable";
    return evidence;
  }
  std::shared_lock lock(internal::state.mutex);
  if (internal::state.contexts.size() != 1) {
    evidence.reason = internal::state.contexts.empty() ? "context_unobserved" : "multiple_contexts";
    return evidence;
  }
  const auto& [handle, context] = *internal::state.contexts.begin();
  evidence.context = reinterpret_cast<uintptr_t>(handle);
  evidence.context_identity = reinterpret_cast<uintptr_t>(context.get());
  evidence.proxy = context->proxy_identity;
  evidence.application_device11 = reinterpret_cast<uintptr_t>(context->bridge_application_device11.Get());
  evidence.proxy_device11 = reinterpret_cast<uintptr_t>(context->bridge_proxy_device11.Get());
  evidence.declaration = context->bridge_declaration;
  evidence.epoch = context->epoch;
  evidence.source = context->bridge_off_source;
  evidence.before = context->bridge_before_observation;
  evidence.after = context->bridge_after_observation;
  evidence.init_kind = context->bridge_init_kind;
  evidence.application = context->bridge_application_observation;
  evidence.proxy_device = context->bridge_proxy_observation;
  evidence.source_link_reason = context->bridge_link_reason;
  evidence.source_swapchain = context->bridge_source_swapchain;
  evidence.source_swapchain_generation = context->bridge_source_generation;
  evidence.source_link_generation = context->bridge_link_generation;
  evidence.uses_source_link = context->bridge_off_uses_source_link;
  if (evidence.source_swapchain == 0) {
    for (const auto& [owner, registered] : internal::state.bridge_swapchains) {
      const bool same_device = (source.device12 != 0
          ? source.device11 == 0 && reinterpret_cast<uintptr_t>(registered->device12.Get()) == source.device12
          : source.device11 != 0 && reinterpret_cast<uintptr_t>(registered->device11.Get()) == source.device11);
      if (!same_device) continue;
      if (evidence.source_swapchain != 0) {
        evidence.source_link_reason = "multiple_source_swapchains";
        evidence.source_swapchain = 0;
        evidence.source_swapchain_generation = 0;
        break;
      }
      evidence.source_link_reason = registered->reason;
      evidence.source_swapchain = owner;
      evidence.source_swapchain_generation = registered->generation;
    }
  }
  if (const auto registered = internal::state.bridge_swapchains.find(evidence.source_swapchain);
      registered != internal::state.bridge_swapchains.end()
      && registered->second->generation == evidence.source_swapchain_generation) {
    evidence.source_present_thread_id = registered->second->source_present_thread_id;
    evidence.source_present_proxy_entry_depth = registered->second->source_present_proxy_entry_depth;
  }
  evidence.reason = context->bridge_off_reason;
  if (!context->bridge_context_ready || !context->bridge_proxy_ready || context->bridge_enable_inflight != 0
      || !context->bridge_off_observed) return evidence;
  if (context->bridge_off_epoch != context->epoch || context->enabled) {
    evidence.reason = "context_changed_after_disable";
  } else if (context->bridge_off_uses_source_link && (!internal::BridgeSourceLinkCurrentLocked(*context)
      || context->bridge_off_link_generation != context->bridge_link_generation)) {
    evidence.reason = "source_mapping_changed_after_disable";
  } else if ((source.device11 == 0 && source.device12 == 0) || (source.device11 != 0 && source.device12 != 0)
      || source.handle == 0 || source.generation == 0) {
    evidence.reason = "source_unobserved_or_ambiguous";
  } else if (source.device12 != 0 && (!context->bridge_off_uses_source_link
      || !internal::BridgeSourceLinkCurrentLocked(*context)
      || source.device12 != reinterpret_cast<uintptr_t>(context->device_identity.Get()))) {
    evidence.reason = "source_device12_link_unproven";
  } else if (source != context->bridge_off_source) {
    evidence.reason = "source_does_not_match_disable";
  } else {
    evidence.available = true;
  }
  return evidence;
}

// Public SDK enable state, independent of whether NR requested ownership.
inline bool Enabled() noexcept {
  return internal::owns_stream.load(std::memory_order_acquire);
}

inline bool OwnsStream() noexcept {
  return internal::Requested() && (internal::proxy_depth != 0 || internal::owns_stream.load(std::memory_order_acquire));
}

inline Result CurrentResult() noexcept {
  return OwnsStream() ? internal::result.load(std::memory_order_acquire) : Result::NOT_SERVED;
}

// UI service state only: CurrentResult remains the exact current handoff.
inline Result DisplayResult() noexcept {
  return OwnsStream() ? internal::display_result.load(std::memory_order_acquire) : Result::NOT_SERVED;
}

inline uint64_t SubmittedFrames() noexcept {
  return internal::submitted_frames.load(std::memory_order_relaxed);
}

inline void Stop() noexcept {
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
  xefg_present_probe::Observe([] { xefg_present_probe::Stop(); });
#endif
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
  fg_output_probe::StopXefg();
#endif
  internal::stopped.store(true, std::memory_order_release);
  std::map<uintptr_t, std::shared_ptr<internal::BridgeSourceSwapchain>> removed_sources;
  {
    std::unique_lock lock(internal::state.mutex);
    for (const auto& [handle, context] : internal::state.contexts) {
      internal::RevokeBridgeSourceLinkLocked(context.get(), "source_observation_stopped");
    }
    removed_sources.swap(internal::state.bridge_swapchains);
  }
  for (size_t index = 0; index < internal::modules.size(); ++index) {
    const auto& slot = internal::modules[index];
    if (!slot.installed) continue;
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
    // An entered synchronous SDK call cannot be cancelled. Keep its lifecycle
    // forwarding guards until process exit; the pinned worker retains code.
    if (xefg_present_probe::ControlBusy(xefg_present_probe::CopyControl())) continue;
#endif
    const auto detours = (index == 0 ? internal::SdkDetours<0>() : internal::SdkDetours<1>());
    for (size_t i = 0; i < detours.size(); ++i) {
      if (slot.real[i] != nullptr) detour_guard::Neutralize(&slot.real[i], detours[i], slot.records[i]);
    }
  }
  for (size_t index = 0; index < internal::source_present_slots.size(); ++index) {
    const auto& slot = internal::source_present_slots[index];
    if (!slot.installed) continue;
    const auto detours = (index == 0 ? internal::SourcePresentDetours<0>() : index == 1 ? internal::SourcePresentDetours<1>()
        : index == 2 ? internal::SourcePresentDetours<2>() : internal::SourcePresentDetours<3>());
    for (size_t i = 0; i < detours.size(); ++i) {
      if (slot.real[i] != nullptr) detour_guard::Neutralize(&slot.real[i], detours[i], slot.records[i]);
    }
  }
}

inline void Poll(bool force_discovery = true) {
  if (internal::stopped.load(std::memory_order_acquire)) return;
  if (!internal::sdk_detours_enabled.load(std::memory_order_acquire)) return;
  {
    std::unique_lock lock(internal::state.mutex);
    std::erase_if(internal::state.retired_contexts, [](const auto& context) {
      for (const auto& surface : context->surfaces) {
        if (!submission::ResourceReleasable(surface->texture.Get())) return false;
        if (surface->consumer_fence != nullptr
            && surface->consumer_fence->GetCompletedValue() < surface->consumer_value) return false;
      }
      // Destroy succeeded: Intel requires the caller to finish tag recordings
      // before that boundary. Remaining plugin recordings still gate release.
      return true;
    });
  }
  HMODULE first = nullptr;
  DWORD needed = 0;
  if (!EnumProcessModules(GetCurrentProcess(), &first, sizeof(first), &needed)) return;
  if (!force_discovery && internal::module_bytes.load(std::memory_order_acquire) == needed) return;
  std::vector<HMODULE> loaded(needed / sizeof(HMODULE) + 16);
  if (!EnumProcessModules(GetCurrentProcess(), loaded.data(), static_cast<DWORD>(loaded.size() * sizeof(HMODULE)), &needed)
      || needed > loaded.size() * sizeof(HMODULE)) return;
  loaded.resize(needed / sizeof(HMODULE));
  bool retry = false;
  for (HMODULE borrowed : loaded) {
    wchar_t path[32768] = {};
    if (GetModuleFileNameW(borrowed, path, static_cast<DWORD>(std::size(path))) == 0) continue;
    const wchar_t* name = std::wcsrchr(path, L'\\');
    name = name == nullptr ? path : name + 1;
    if (_wcsicmp(name, L"libxess_fg.dll") != 0 && _wcsicmp(name, L"libxess_fg_dx12.dll") != 0) continue;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(borrowed), &module)) continue;
    const std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> module_reference(module, FreeLibrary);
    std::unique_lock install(internal::install_mutex, std::try_to_lock);
    if (!install.owns_lock()) return;
    for (size_t index = 0; index < internal::modules.size(); ++index) {
      auto& slot = internal::modules[index];
      if (slot.installed) {
        if (slot.module == module) break;
        continue;
      }
      std::array<void*, internal::EXPORTS.size()> exports{};
      bool complete = true;
      size_t available = 0;
      for (size_t i = 0; i < exports.size(); ++i) {
        exports[i] = reinterpret_cast<void*>(GetProcAddress(module, internal::EXPORTS[i]));
        available += exports[i] != nullptr;
        // Older public runtimes always interpolate UI and have no dynamic
        // composition setter. Absence keeps the documented disabled default.
        if (i != 7 && i != 9
#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
            && i != 10
#endif
        ) {
          complete &= exports[i] != nullptr;
        }
      }
      if (!complete) break;
      HMODULE pinned = nullptr;
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
              reinterpret_cast<LPCWSTR>(module), &pinned)) break;
      slot.module = module;
      slot.real = exports;
      const auto detours = (index == 0 ? internal::SdkDetours<0>() : internal::SdkDetours<1>());
      std::array<detour_guard::Hook, internal::EXPORTS.size()> hooks;
      for (size_t i = 0; i < hooks.size(); ++i) hooks[i] = {
          .name = internal::EXPORTS[i], .real = &slot.real[i], .detour = detours[i], .record = &slot.records[i]};
      size_t failed = 0;
      const auto attached = detour_guard::AttachAll(hooks, &failed);
      slot.installed = attached != 0;
      slot.complete = attached == available;
      retry |= attached == 0;
      break;
    }
  }
  if (!retry) internal::module_bytes.store(needed, std::memory_order_release);
}

}  // namespace renodx::addons::dlss5::fg::xefg
