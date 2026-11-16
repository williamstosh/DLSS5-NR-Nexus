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
    if (!tracked) return;
    std::unique_lock lock(state.mutex);
    auto& metadata = state.metadata;
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

  explicit SdkScope(uintptr_t output, bool enabled = true) : previous(sdk_binding) {
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

  ~SdkScope() { sdk_binding = previous; }
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
