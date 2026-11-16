/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#if defined(NR_NEXUS_DEV_CONTROL)
#include <windows.h>

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

inline int64_t Qpc() {
  LARGE_INTEGER value{};
  QueryPerformanceCounter(&value);
  return value.QuadPart;
}

// This sequence describes CPU observations only, never source-frame or GPU progress.
inline Event NextEvent(Metadata* metadata) {
  return {.sequence = ++metadata->events.sequence, .qpc = Qpc(), .thread = GetCurrentThreadId()};
}

struct ObservationSnapshot {
  Events events;
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
  return {.events = metadata.events, .marker_published = metadata.published, .marker_claimed = metadata.claimed,
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
