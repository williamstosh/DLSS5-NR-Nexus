/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#if defined(NR_NEXUS_DEV_CONTROL)
#include <windows.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <shared_mutex>

#include <sl_pcl.h>

namespace renodx::addons::dlss5::fg::native_options::frame_source {

struct Frame {
  bool available = false;
  uint32_t frame = 0;
  uint64_t epoch = 0;
  uint64_t generation = 0;
  const char* reason = "reflex_marker_unobserved";
};

struct Event {
  uint64_t sequence = 0;
  int64_t qpc = 0;
  DWORD thread = 0;
  uint32_t marker = 0;
  uint32_t frame = 0;
  uintptr_t output = 0;
  uint32_t depth = 0;
  // A real marker return only; SDK scope events do not report SDK success.
  bool success = false;
};
struct Events {
  uint64_t sequence = 0;
  Event marker_enter;
  Event marker_return;
  Event sdk_enter;
  Event sdk_exit;
  uint64_t sdk_enters = 0;
  uint64_t sdk_exits = 0;
  uint64_t sdk_active = 0;
};

// Borrowed SDK inputs are reduced to scalar observations before taking the
// state lock. These scopes never grant a Frame or retain SDK objects.
struct EvaluateBinding {
  uint64_t call = 0;
  uint64_t parent = 0;
  uint32_t feature = 0;
  uint32_t frame = 0;
  uintptr_t command_buffer = 0;
  uint32_t inputs = 0;
  uint32_t depth = 0;
  uint64_t dlssg_call = 0;
  uint32_t dlssg_frame = 0;
};
struct EvaluateEvent {
  Event cpu;
  EvaluateBinding request;
  bool returned = false;
  uint32_t result = 0;
};
struct EvaluateFeatureEvents {
  uint64_t enters = 0;
  uint64_t returns = 0;
  uint64_t active = 0;
  uint64_t ngx_calls = 0;
  uint64_t ngx_fg_calls = 0;
  EvaluateEvent enter;
  EvaluateEvent returned;
};
struct NgxEvent {
  Event cpu;
  uint64_t call = 0;
  uint64_t parent = 0;
  uint32_t depth = 0;
  uint32_t slot = 0;
  bool c_export = false;
  bool wrapper_delegate = false;
  bool frame_generation = false;
  uintptr_t handle = 0;
  uintptr_t command_list = 0;
  EvaluateBinding gateway;
  bool returned = false;
  uint32_t result = 0;
};
struct EvaluateEvents {
  // DLSS, RR and DLSS-G have reserved slots; all other feature IDs share
  // the final bucket. Its events still contain the actual requested ID.
  std::array<EvaluateFeatureEvents, 4> features{};
  uint64_t enters = 0;
  uint64_t returns = 0;
  uint64_t active = 0;
  uint64_t ngx_calls = 0;
  uint64_t ngx_fg_calls = 0;
  uint64_t ngx_fg_nested = 0;
  uint64_t ngx_fg_under_dlssg = 0;
  NgxEvent ngx_enter;
  NgxEvent ngx_return;
  NgxEvent ngx_fg_enter;
  NgxEvent ngx_fg_return;
};

struct Metadata {
  uint64_t generation = 1;
  uint64_t epoch = 0;
  uint32_t in_flight = 0;
  uint32_t frame = 0;
  DWORD thread = 0;
  bool published = false;
  bool claimed = false;
  bool stopped = false;
  const char* reason = "reflex_marker_unobserved";
  Events events;
  EvaluateEvents evaluations;
};
struct State {
  std::shared_mutex mutex;
  Metadata metadata;
};
static State state;

struct Binding {
  Frame source;
  uintptr_t output = 0;
  bool active = false;
};
static thread_local Binding sdk_binding;
static thread_local uint32_t sdk_depth = 0;
static thread_local EvaluateBinding evaluate_binding;
static thread_local uint64_t ngx_call = 0;
static thread_local uint32_t ngx_depth = 0;

inline int64_t Qpc() {
  LARGE_INTEGER value{};
  QueryPerformanceCounter(&value);
  return value.QuadPart;
}

// This sequence describes CPU observations only, never source-frame or GPU progress.
inline Event NextEvent(Metadata* metadata) {
  return {.sequence = ++metadata->events.sequence, .qpc = Qpc(), .thread = GetCurrentThreadId()};
}

inline uint32_t EvaluateFeatureIndex(uint32_t feature) {
  switch (feature) {
    case sl::kFeatureDLSS: return 0;
    case sl::kFeatureDLSS_RR: return 1;
    case sl::kFeatureDLSS_G: return 2;
    default: return 3;
  }
}

struct EvaluateCall {
  EvaluateBinding previous = evaluate_binding;
  EvaluateEvent event;

  EvaluateCall(bool enabled, sl::Feature feature, const sl::FrameToken& frame,
      uint32_t inputs, sl::CommandBuffer* command_buffer) {
    evaluate_binding = {};
    if (!enabled) return;
    event.request = {.parent = previous.call, .feature = feature,
        .frame = static_cast<uint32_t>(frame), .command_buffer = reinterpret_cast<uintptr_t>(command_buffer),
        .inputs = inputs, .depth = previous.depth + 1,
        .dlssg_call = previous.dlssg_call, .dlssg_frame = previous.dlssg_frame};
    std::unique_lock lock(state.mutex);
    auto& metadata = state.metadata;
    event.cpu = NextEvent(&metadata);
    event.request.call = event.cpu.sequence;
    if (feature == sl::kFeatureDLSS_G) {
      event.request.dlssg_call = event.request.call;
      event.request.dlssg_frame = event.request.frame;
    }
    auto& observed = metadata.evaluations;
    auto& bucket = observed.features[EvaluateFeatureIndex(feature)];
    ++observed.enters;
    ++observed.active;
    ++bucket.enters;
    ++bucket.active;
    bucket.enter = event;
    evaluate_binding = event.request;
  }

  void Finish(sl::Result result) {
    if (event.request.call == 0 || event.returned) return;
    event.returned = true;
    event.result = static_cast<uint32_t>(result);
  }

  ~EvaluateCall() {
    if (event.request.call != 0) {
      std::unique_lock lock(state.mutex);
      auto& metadata = state.metadata;
      event.cpu = NextEvent(&metadata);
      auto& observed = metadata.evaluations;
      auto& bucket = observed.features[EvaluateFeatureIndex(event.request.feature)];
      if (event.returned) {
        ++observed.returns;
      }
      --observed.active;
      if (event.returned) {
        ++bucket.returns;
      }
      --bucket.active;
      bucket.returned = event;
    }
    evaluate_binding = previous;
  }
  EvaluateCall(const EvaluateCall&) = delete;
  EvaluateCall& operator=(const EvaluateCall&) = delete;
};

// Covers the real D3D12 NGX forward, after the wrapper has classified FG.
// C/C++ exports and module slots are separate calls, not separate frames.
struct NgxCall {
  NgxEvent event;
  uint64_t previous_call = ngx_call;
  uint32_t previous_depth = ngx_depth;

  NgxCall(bool enabled, bool frame_generation, uint32_t slot, bool c_export, bool wrapper_delegate,
      uintptr_t handle, uintptr_t command_list) {
    ngx_call = 0;
    ngx_depth = 0;
    if (!enabled) return;
    event = {.parent = previous_call, .depth = previous_depth + 1,
        .slot = slot, .c_export = c_export, .wrapper_delegate = wrapper_delegate, .frame_generation = frame_generation,
        .handle = handle, .command_list = command_list, .gateway = evaluate_binding};
    std::unique_lock lock(state.mutex);
    auto& metadata = state.metadata;
    event.cpu = NextEvent(&metadata);
    event.call = event.cpu.sequence;
    ngx_call = event.call;
    ngx_depth = event.depth;
    auto& observed = metadata.evaluations;
    ++observed.ngx_calls;
    observed.ngx_enter = event;
    if (event.gateway.call != 0) {
      auto& bucket = observed.features[EvaluateFeatureIndex(event.gateway.feature)];
      ++bucket.ngx_calls;
      if (frame_generation) {
        ++bucket.ngx_fg_calls;
      }
    }
    if (frame_generation) {
      ++observed.ngx_fg_calls;
      observed.ngx_fg_enter = event;
      if (event.gateway.call != 0) {
        ++observed.ngx_fg_nested;
      }
      if (event.gateway.dlssg_call != 0) {
        ++observed.ngx_fg_under_dlssg;
      }
    }
  }

  void Finish(uint32_t result) {
    if (event.call == 0 || event.returned) return;
    event.returned = true;
    event.result = result;
  }

  ~NgxCall() {
    if (event.call != 0) {
      std::unique_lock lock(state.mutex);
      auto& metadata = state.metadata;
      event.cpu = NextEvent(&metadata);
      metadata.evaluations.ngx_return = event;
      if (event.frame_generation) {
        metadata.evaluations.ngx_fg_return = event;
      }
    }
    ngx_call = previous_call;
    ngx_depth = previous_depth;
  }
  NgxCall(const NgxCall&) = delete;
  NgxCall& operator=(const NgxCall&) = delete;
};

struct ObservationSnapshot {
  Events events;
  EvaluateEvents evaluations;
  EvaluateBinding current_evaluate;
  bool marker_published = false;
  bool marker_claimed = false;
  uint32_t marker_frame = 0;
  DWORD marker_thread = 0;
  uint32_t marker_in_flight = 0;
  uint64_t marker_epoch = 0;
  uint64_t generation = 0;
  const char* marker_reason = "reflex_marker_unobserved";
  DWORD current_thread = 0;
  uint32_t tls_depth = 0;
  uintptr_t current_output = 0;
  bool binding_active = false;
  // Captured grant only. CurrentFrame still revalidates the current epoch.
  bool binding_available = false;
  uint32_t binding_frame = 0;
  uint64_t binding_epoch = 0;
  int64_t qpc = 0;
};

inline ObservationSnapshot Snapshot() {
  std::shared_lock lock(state.mutex);
  const auto& metadata = state.metadata;
  return {.events = metadata.events, .evaluations = metadata.evaluations, .current_evaluate = evaluate_binding,
      .marker_published = metadata.published, .marker_claimed = metadata.claimed,
      .marker_frame = metadata.frame, .marker_thread = metadata.thread, .marker_in_flight = metadata.in_flight,
      .marker_epoch = metadata.epoch, .generation = metadata.generation, .marker_reason = metadata.reason,
      .current_thread = GetCurrentThreadId(), .tls_depth = sdk_depth, .current_output = sdk_binding.output,
      .binding_active = sdk_binding.active, .binding_available = sdk_binding.source.available,
      .binding_frame = sdk_binding.source.frame, .binding_epoch = sdk_binding.source.epoch, .qpc = Qpc()};
}

inline void Activate() {
  std::unique_lock lock(state.mutex);
  auto& metadata = state.metadata;
  if (metadata.stopped) return;
  ++metadata.generation;
  ++metadata.epoch;
  metadata.published = false;
  metadata.claimed = false;
  metadata.reason = "reflex_marker_unobserved";
}

inline void Stop() {
  std::unique_lock lock(state.mutex);
  auto& metadata = state.metadata;
  ++metadata.generation;
  ++metadata.epoch;
  metadata.stopped = true;
  metadata.published = false;
  metadata.claimed = false;
  metadata.reason = "frame_source_stopped";
}

// Only the fixed SDK's actual successful PresentStart writer grants a token.
// Entry and return both change the epoch: overlapping setters can write the
// SDK parameter in a different order from their returns, so neither may grant.
struct MarkerCall {
  sl::PCLMarker marker;
  uint32_t frame;
  uint64_t ticket = 0;
  uint64_t generation = 0;
  bool tracked = false;
  bool finished = false;

  MarkerCall(sl::PCLMarker value, uint32_t index) : marker(value), frame(index) {
    tracked = marker == sl::PCLMarker::ePresentStart || marker == sl::PCLMarker::ePresentEnd
        || marker == sl::PCLMarker::eRenderSubmitEnd
        || marker == sl::PCLMarker::eOutOfBandPresentStart || marker == sl::PCLMarker::eOutOfBandPresentEnd;
    std::unique_lock lock(state.mutex);
    auto& metadata = state.metadata;
    metadata.events.marker_enter = NextEvent(&metadata);
    metadata.events.marker_enter.marker = static_cast<uint32_t>(marker);
    metadata.events.marker_enter.frame = frame;
    if (!tracked) return;
    if (metadata.stopped) return;
    ticket = ++metadata.epoch;
    generation = metadata.generation;
    ++metadata.in_flight;
    metadata.published = false;
    metadata.claimed = false;
    metadata.reason = "reflex_marker_in_flight";
  }

  void Finish(bool success) {
    if (finished) return;
    finished = true;
    std::unique_lock lock(state.mutex);
    auto& metadata = state.metadata;
    metadata.events.marker_return = NextEvent(&metadata);
    metadata.events.marker_return.marker = static_cast<uint32_t>(marker);
    metadata.events.marker_return.frame = frame;
    metadata.events.marker_return.success = success;
    if (!tracked) {
      if (!success && !metadata.stopped) {
        ++metadata.epoch;
        metadata.published = false;
        metadata.claimed = false;
        metadata.reason = "reflex_marker_failed";
      }
      return;
    }
    if (ticket == 0) return;
    const bool uninterrupted = metadata.epoch == ticket && metadata.generation == generation
        && metadata.in_flight == 1;
    --metadata.in_flight;
    ++metadata.epoch;
    metadata.published = false;
    metadata.claimed = false;
    if (metadata.stopped) {
      metadata.reason = "frame_source_stopped";
    } else if (!success) {
      metadata.reason = "reflex_marker_failed";
    } else if (!uninterrupted) {
      metadata.reason = "reflex_marker_interleaved";
    } else if (marker == sl::PCLMarker::ePresentStart) {
      metadata.frame = frame;
      metadata.thread = GetCurrentThreadId();
      metadata.published = true;
      metadata.reason = "reflex_present_start";
    } else {
      metadata.reason = (marker == sl::PCLMarker::ePresentEnd ? "reflex_present_ended" : "reflex_alternate_marker");
    }
  }

  ~MarkerCall() {
    if (!finished) Finish(false);
  }
  MarkerCall(const MarkerCall&) = delete;
  MarkerCall& operator=(const MarkerCall&) = delete;
};

using SetMarker = sl::Result (*)(sl::PCLMarker, const sl::FrameToken&);

inline sl::Result ObserveMarker(SetMarker real, sl::PCLMarker marker, const sl::FrameToken& frame) {
  MarkerCall observation(marker, static_cast<uint32_t>(frame));
  const auto result = real(marker, frame);
  observation.Finish(result == sl::Result::eOk);
  return result;
}

struct SdkScope {
  Binding previous;
  uintptr_t diagnostic_output;

  explicit SdkScope(uintptr_t output, bool enabled = true) : previous(sdk_binding), diagnostic_output(output) {
    ++sdk_depth;
    {
      std::unique_lock lock(state.mutex);
      auto& metadata = state.metadata;
      ++metadata.events.sdk_enters;
      ++metadata.events.sdk_active;
      metadata.events.sdk_enter = NextEvent(&metadata);
      metadata.events.sdk_enter.output = output;
      metadata.events.sdk_enter.depth = sdk_depth;
    }
    if (previous.active && enabled && previous.output == output) return;
    sdk_binding = {.source = {.reason = "sdk_present_scope_unavailable"}, .output = output, .active = true};
    if (!enabled || output == 0) return;
    if (previous.active) {
      std::unique_lock lock(state.mutex);
      auto& metadata = state.metadata;
      ++metadata.epoch;
      metadata.published = false;
      metadata.claimed = false;
      metadata.reason = "nested_sdk_output_changed";
      sdk_binding.source = {.frame = metadata.frame, .epoch = metadata.epoch,
          .generation = metadata.generation, .reason = metadata.reason};
      return;
    }
    std::unique_lock lock(state.mutex);
    auto& metadata = state.metadata;
    sdk_binding.source = {.frame = metadata.frame, .epoch = metadata.epoch,
        .generation = metadata.generation, .reason = metadata.reason};
    if (metadata.stopped || !metadata.published || metadata.in_flight != 0) return;
    if (metadata.thread != GetCurrentThreadId()) {
      sdk_binding.source.reason = "reflex_marker_thread_mismatch";
      return;
    }
    if (metadata.claimed) {
      sdk_binding.source.reason = "reflex_marker_already_consumed";
      return;
    }
    metadata.claimed = true;
    sdk_binding.source.available = true;
  }

  ~SdkScope() {
    {
      std::unique_lock lock(state.mutex);
      auto& metadata = state.metadata;
      ++metadata.events.sdk_exits;
      --metadata.events.sdk_active;
      metadata.events.sdk_exit = NextEvent(&metadata);
      metadata.events.sdk_exit.output = diagnostic_output;
      metadata.events.sdk_exit.depth = sdk_depth;
    }
    --sdk_depth;
    sdk_binding = previous;
  }
  SdkScope(const SdkScope&) = delete;
  SdkScope& operator=(const SdkScope&) = delete;
};

inline Frame CurrentFrame(uintptr_t output) {
  std::shared_lock lock(state.mutex);
  const auto& metadata = state.metadata;
  Frame result{.frame = metadata.frame, .epoch = metadata.epoch,
      .generation = metadata.generation, .reason = metadata.reason};
  if (metadata.stopped) return result;
  if (!sdk_binding.active) {
    result.reason = "outside_sdk_present_scope";
    return result;
  }
  if (output == 0 || sdk_binding.output != output) {
    result.reason = "sdk_output_mismatch";
    return result;
  }
  if (!sdk_binding.source.available) {
    result.reason = sdk_binding.source.reason;
    return result;
  }
  if (!metadata.published || metadata.in_flight != 0
      || sdk_binding.source.generation != metadata.generation || sdk_binding.source.epoch != metadata.epoch
      || metadata.thread != GetCurrentThreadId()) {
    result.reason = "reflex_marker_epoch_changed";
    return result;
  }
  return sdk_binding.source;
}

}  // namespace renodx::addons::dlss5::fg::native_options::frame_source
#endif
