/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#if defined(NR_NEXUS_DEV_CONTROL)
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "../../utils/directx.hpp"
#include "submission_tracker.hpp"

namespace renodx::addons::dlss5::edit_fg_observation {

enum class State : uint8_t { DISABLED, ARMED, CAPTURING, STOPPED };
enum class Kind : uint8_t {
  SR_COPYBACK, NATIVE_FG_ENTRY, NATIVE_FG_RETURN,
  XEFG_TAG_ENTRY, XEFG_TAG_RETURN, XEFG_PRESENT_ENTRY, XEFG_PRESENT_RETURN,
  XEFG_CONSTANTS, SR_QUEUE_SUBMISSION, SOURCE_SUCCESS,
};
struct ResourceIdentity {
  uintptr_t raw = 0;
  uintptr_t canonical = 0;
};
struct RecordingIdentity {
  uintptr_t raw = 0;
  uintptr_t reshade_base = 0;
  uint64_t list_id = 0;
  uint64_t generation = 0;
};
struct SrCopyback {
  uintptr_t source_handle = 0;
  uint64_t source_generation = 0;
  uint64_t sr_sequence = 0;
  bool reused = false;
  ResourceIdentity output;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  RecordingIdentity recording;
  uint64_t environment_provider = 0;
  uintptr_t environment_context = 0;
  uint64_t environment_generation = 0;
};
struct NativeFgObservation {
  uintptr_t handle = 0;
  // Existing native FG environment revision, not a viewport or pixel binding.
  uint64_t generation = 0;
  uint32_t slot = 0;
  bool c_export = false;
  RecordingIdentity recording;
  ResourceIdentity backbuffer;
  ResourceIdentity hudless;
  ResourceIdentity ui;
  ResourceIdentity ui_alpha;
  uint32_t ui_mode = 0;
  bool has_ui_mode = false;
  uint64_t frame_id = 0;
  // Success of the documented uint64 getter only; not verified frame semantics.
  bool has_frame_id = false;
};
struct XefgTagObservation {
  uint32_t sdk_slot = 0;
  uintptr_t sdk_context = 0;
  uintptr_t context_identity = 0;
  uint64_t epoch = 0;
  uint64_t generation = 0;
  uint32_t present_id = 0;
  int32_t type = -1;
  int32_t validity = -1;
  ResourceIdentity resource;
  // validity ONLY_NOW permits the SDK command-list interface. BackbufferRegion
  // and UNTIL_NEXT_PRESENT preserve raw values without invoking those objects.
  RecordingIdentity recording;
  uint32_t base_x = 0;
  uint32_t base_y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t resource_state = 0;
  uint32_t ui_mode = 0;
  bool ui_composition = false;
};
struct XefgPresentObservation {
  uintptr_t sdk_context = 0;
  uintptr_t context_identity = 0;
  uint64_t epoch = 0;
  uint64_t generation = 0;
  uint32_t present_id = 0;
  bool has_present_id = false;
  bool repeated = false;
  ResourceIdentity proxy;
  ResourceIdentity backbuffer;
  uint32_t method = 0;
  uint32_t interval = 0;
  uint32_t flags = 0;
};
struct PresentStatus {
  bool queried = false;
  int32_t api_result = 0;
  uint32_t frames_presented = 0;
  int32_t frame_gen_result = 0;
  uint32_t enabled = 0;
};
struct XefgConstantsObservation {
  uint32_t sdk_slot = 0;
  uintptr_t sdk_context = 0;
  uintptr_t context_identity = 0;
  uint64_t epoch = 0;
  uint32_t present_id = 0;
  float frame_time_ms = 0.f;
  bool has_frame_time = false;
};
struct QueueExecutionObservation {
  int64_t execute_entry_ns = 0;
  int64_t execute_return_ns = 0;
  uint64_t forward_calls = 0;
  uintptr_t signal_fence = 0;
  uint64_t signal_value = 0;
  bool signal_succeeded = false;
};
struct QueueSubmissionObservation {
  uintptr_t queue = 0;
  uintptr_t list = 0;
  uint64_t list_id = 0;
  uint64_t generation = 0;
  size_t submission_index = 0;
  uint32_t batch_index = 0;
  bool first_submission = false;
  QueueExecutionObservation execution;
};
struct Event {
  Kind kind = Kind::SR_COPYBACK;
  uint64_t sequence = 0;
  int64_t observed_ns = 0;
  int64_t api_return_ns = 0;
  uint32_t thread_id = 0;
  uint64_t call_id = 0;
  int64_t api_result = 0;
  SrCopyback sr;
  NativeFgObservation native_fg;
  XefgTagObservation xefg_tag;
  XefgPresentObservation xefg_present;
  PresentStatus present_status;
  XefgConstantsObservation xefg_constants;
  QueueSubmissionObservation queue_submission;
};
struct Snapshot {
  bool enabled = false;
  State state = State::DISABLED;
  const char* stop_reason = "disabled";
  uint64_t request_generation = 0;
  uint32_t source_frames = 0;
  uint32_t count = 0;
  uint64_t overflow = 0;
  bool source_copyback_observed = false;
  SrCopyback source;
  // These are CPU observations. Neither a recorded copy nor an SDK return
  // supplies GPU submission/completion or source-to-FG pixel-consumption proof.
  std::array<Event, 64> events{};
};
struct Ticket {
  uint64_t request_generation = 0;
  uint64_t call_id = 0;
  Kind kind = Kind::SR_COPYBACK;
};
struct ObservationState {
  std::shared_mutex mutex;
  Snapshot snapshot;
  uint64_t next_generation = 0;
  uint64_t last_sr_sequence = 0;
  bool on_demand = false;
};
static ObservationState observation_state;

inline uint64_t ArmLocked(bool on_demand) {
  const auto generation = ++observation_state.next_generation;
  observation_state.snapshot = {.enabled = true, .state = State::ARMED,
      .stop_reason = (on_demand ? "awaiting_successful_sr" : "awaiting_sr_copyback"),
      .request_generation = generation};
  observation_state.last_sr_sequence = 0;
  observation_state.on_demand = on_demand;
  return generation;
}

inline uint64_t Arm() {
  std::unique_lock lock(observation_state.mutex);
  return ArmLocked(true);
}

inline void SetEnabled(bool enabled) {
  std::unique_lock lock(observation_state.mutex);
  auto& snapshot = observation_state.snapshot;
  if (snapshot.enabled == enabled && (!enabled || !observation_state.on_demand)) return;
  if (enabled) {
    ArmLocked(false);
  } else {
    snapshot.enabled = false;
    if (snapshot.state == State::ARMED || snapshot.state == State::CAPTURING) {
      snapshot.state = State::STOPPED;
      snapshot.stop_reason = "experiment_disabled";
    }
  }
}

inline void Stop(const char* reason) {
  std::unique_lock lock(observation_state.mutex);
  auto& snapshot = observation_state.snapshot;
  if (snapshot.state == State::ARMED || snapshot.state == State::CAPTURING) {
    snapshot.state = State::STOPPED;
    snapshot.stop_reason = reason;
  }
}

inline void StopSource(uintptr_t handle) {
  std::unique_lock lock(observation_state.mutex);
  auto& snapshot = observation_state.snapshot;
  if (snapshot.state == State::CAPTURING && snapshot.source.source_handle == handle) {
    snapshot.state = State::STOPPED;
    snapshot.stop_reason = "source_released";
  }
}

inline void StopEnvironment(uint32_t provider, uintptr_t context) {
  std::unique_lock lock(observation_state.mutex);
  auto& snapshot = observation_state.snapshot;
  if (snapshot.state == State::CAPTURING && snapshot.source.environment_provider == provider
      && (context == 0 || snapshot.source.environment_context == context)) {
    snapshot.state = State::STOPPED;
    snapshot.stop_reason = "environment_changed";
  }
}

inline uint64_t CaptureGeneration(bool include_armed = false) {
  std::shared_lock lock(observation_state.mutex);
  const auto& snapshot = observation_state.snapshot;
  if (snapshot.state == State::CAPTURING || (include_armed && snapshot.state == State::ARMED)) {
    return snapshot.request_generation;
  }
  return 0;
}

inline uint64_t CaptureSrCopyback(bool legacy_eligible) {
  std::shared_lock lock(observation_state.mutex);
  const auto& snapshot = observation_state.snapshot;
  if (snapshot.state == State::CAPTURING && (observation_state.on_demand || legacy_eligible)) {
    return snapshot.request_generation;
  }
  if (snapshot.state == State::ARMED && legacy_eligible && !observation_state.on_demand) {
    return snapshot.request_generation;
  }
  return 0;
}

inline int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Only call while the API owns a genuine resource argument. The numbers do not
// retain it or establish object lifetime across unrelated requests.
inline ResourceIdentity ObserveResource(IUnknown* resource) {
  return {.raw = reinterpret_cast<uintptr_t>(resource),
      .canonical = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(resource))};
}

inline RecordingIdentity ObserveRecording(ID3D12CommandList* list) {
  RecordingIdentity observation{.raw = reinterpret_cast<uintptr_t>(list)};
  if (list == nullptr) return observation;
  auto* base = list;
  if (renodx::utils::directx::NativeFromReShadeProxy(&base)) {
    observation.reshade_base = reinterpret_cast<uintptr_t>(base);
  }
  // Do not assign a fresh initial generation to an unobserved foreign list.
  const auto identity = submission::ListIdentity(list, false);
  std::lock_guard lock(submission::tracker_mutex);
  if (const auto it = submission::tracked_lists.find(identity);
      identity != 0 && (identity & (uint64_t{1} << 63)) == 0
      && it != submission::tracked_lists.end() && !it->second.destroyed) {
    observation.list_id = identity;
    observation.generation = it->second.current;
  }
  return observation;
}

inline std::vector<QueueSubmissionObservation> CaptureQueueSubmissions(
    uint64_t request_generation, ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists,
    const std::vector<submission::PendingSubmission>& pending) {
  std::vector<QueueSubmissionObservation> observations;
  if (request_generation == 0 || pending.empty() || lists == nullptr) return observations;
  std::array<std::pair<uint64_t, uint64_t>, 64> captured;
  auto captured_end = captured.begin();
  {
    std::shared_lock lock(observation_state.mutex);
    const auto& snapshot = observation_state.snapshot;
    if (snapshot.state != State::CAPTURING || snapshot.request_generation != request_generation) return observations;
    for (uint32_t index = 0; index < snapshot.count; ++index) {
      const auto& event = snapshot.events[index];
      if (event.kind == Kind::SR_COPYBACK && event.sr.recording.list_id != 0 && event.sr.recording.generation != 0) {
        *captured_end++ = {event.sr.recording.list_id, event.sr.recording.generation};
      }
    }
  }
  if (captured_end == captured.begin()) return observations;

  // BeginSubmission reserves in batch order and registers each observed native
  // list interface as an alias. Read those aliases before forwarding the batch.
  std::lock_guard lock(submission::tracker_mutex);
  auto reservation = pending.begin();
  for (UINT index = 0; index < count && reservation != pending.end(); ++index) {
    const auto alias = submission::identity_by_pointer.find(lists[index]);
    if (alias == submission::identity_by_pointer.end() || alias->second != reservation->list) continue;
    const auto& item = *reservation++;
    if (std::find(captured.begin(), captured_end, std::pair{item.list, item.generation}) == captured_end) continue;
    const auto tracked = submission::tracked_lists.find(item.list);
    if (tracked == submission::tracked_lists.end()) continue;
    const auto recording = tracked->second.generations.find(item.generation);
    if (recording == tracked->second.generations.end()
        || item.submission_index >= recording->second.submissions.size()) continue;
    observations.push_back({.queue = reinterpret_cast<uintptr_t>(queue),
        .list = reinterpret_cast<uintptr_t>(lists[index]), .list_id = item.list, .generation = item.generation,
        .submission_index = item.submission_index, .batch_index = index,
        .first_submission = recording->second.first_submission_index == item.submission_index});
    if (observations.size() == captured.size()) break;
  }
  return observations;
}

inline void ObserveSourceFrame(uintptr_t handle, uint64_t generation, uint64_t sequence) {
  std::unique_lock lock(observation_state.mutex);
  auto& snapshot = observation_state.snapshot;
  if (observation_state.on_demand || snapshot.state != State::CAPTURING) return;
  if (snapshot.source.source_handle != handle || snapshot.source.source_generation != generation
      || sequence < observation_state.last_sr_sequence) {
    snapshot.state = State::STOPPED;
    snapshot.stop_reason = "source_changed";
    return;
  }
  if (sequence == observation_state.last_sr_sequence) return;
  if (snapshot.source_frames == 4) {
    snapshot.state = State::STOPPED;
    snapshot.stop_reason = "source_frame_limit";
    return;
  }
  observation_state.last_sr_sequence = sequence;
  ++snapshot.source_frames;
}

// The caller holds the trace lock. No COM calls, SDK calls or tracker locks
// occur while it is held; every stored object is a scalar borrowed observation.
inline Ticket Append(Event event) {
  auto& snapshot = observation_state.snapshot;
  if (snapshot.count == snapshot.events.size()) {
    snapshot.state = State::STOPPED;
    snapshot.stop_reason = "event_limit";
    ++snapshot.overflow;
    return {};
  }
  event.sequence = snapshot.count + 1;
  event.observed_ns = NowNs();
  event.thread_id = GetCurrentThreadId();
  if (event.call_id == 0 && event.kind != Kind::SR_COPYBACK && event.kind != Kind::SOURCE_SUCCESS) {
    event.call_id = event.sequence;
  }
  snapshot.events[snapshot.count++] = event;
  return {.request_generation = snapshot.request_generation, .call_id = event.call_id, .kind = event.kind};
}

inline void ObserveSourceSuccess(uintptr_t handle, uint64_t generation, uint64_t sequence) {
  std::unique_lock lock(observation_state.mutex);
  auto& snapshot = observation_state.snapshot;
  if (!observation_state.on_demand || handle == 0 || generation == 0 || sequence == 0
      || (snapshot.state != State::ARMED && snapshot.state != State::CAPTURING)) return;
  if (snapshot.state == State::ARMED) {
    snapshot.state = State::CAPTURING;
    snapshot.stop_reason = "capturing";
    snapshot.source = {.source_handle = handle, .source_generation = generation, .sr_sequence = sequence};
  } else if (snapshot.source.source_handle != handle || snapshot.source.source_generation != generation
             || sequence < observation_state.last_sr_sequence) {
    snapshot.state = State::STOPPED;
    snapshot.stop_reason = "source_changed";
    return;
  }
  if (sequence == observation_state.last_sr_sequence) return;
  if (snapshot.source_frames == 4) {
    snapshot.state = State::STOPPED;
    snapshot.stop_reason = "source_frame_limit";
    return;
  }
  observation_state.last_sr_sequence = sequence;
  ++snapshot.source_frames;
  Append({.kind = Kind::SOURCE_SUCCESS, .sr = {.source_handle = handle, .source_generation = generation, .sr_sequence = sequence}});
}

inline void RecordSrCopyback(uint64_t generation, const SrCopyback& source) {
  std::unique_lock lock(observation_state.mutex);
  auto& snapshot = observation_state.snapshot;
  if (generation == 0 || generation != snapshot.request_generation
      || source.source_handle == 0 || source.source_generation == 0 || source.sr_sequence == 0
      || (snapshot.state != State::ARMED && snapshot.state != State::CAPTURING)) return;
  if (observation_state.on_demand
      && (snapshot.state != State::CAPTURING || source.sr_sequence != observation_state.last_sr_sequence)) return;
  if (snapshot.state == State::ARMED) {
    snapshot.state = State::CAPTURING;
    snapshot.stop_reason = "capturing";
    snapshot.source_frames = 1;
    observation_state.last_sr_sequence = source.sr_sequence;
  } else if (snapshot.source.source_handle != source.source_handle
      || snapshot.source.source_generation != source.source_generation
      || (snapshot.source_copyback_observed
          && (snapshot.source.environment_provider != source.environment_provider
              || snapshot.source.environment_context != source.environment_context
              || snapshot.source.environment_generation != source.environment_generation))) {
    snapshot.state = State::STOPPED;
    snapshot.stop_reason = "source_or_environment_changed";
    return;
  }
  if (!snapshot.source_copyback_observed) {
    snapshot.source = source;
    snapshot.source_copyback_observed = true;
  }
  Append({.kind = Kind::SR_COPYBACK, .sr = source});
}

inline Ticket Record(uint64_t generation, Event event) {
  std::unique_lock lock(observation_state.mutex);
  const auto& snapshot = observation_state.snapshot;
  if (generation == 0 || generation != snapshot.request_generation || snapshot.state != State::CAPTURING) return {};
  if (event.kind != Kind::NATIVE_FG_ENTRY && event.kind != Kind::XEFG_TAG_ENTRY
      && event.kind != Kind::XEFG_PRESENT_ENTRY && event.kind != Kind::XEFG_CONSTANTS) return {};
  return Append(event);
}

inline void RecordQueueSubmissions(uint64_t generation, const std::vector<QueueSubmissionObservation>& observations,
                                  const QueueExecutionObservation& execution) {
  std::unique_lock lock(observation_state.mutex);
  const auto& snapshot = observation_state.snapshot;
  if (generation == 0 || generation != snapshot.request_generation || snapshot.state != State::CAPTURING) return;
  for (auto observation : observations) {
    observation.execution = execution;
    if (Append({.kind = Kind::SR_QUEUE_SUBMISSION, .queue_submission = observation}).call_id == 0) break;
  }
}

inline void RecordResult(const Ticket& ticket, int64_t result, int64_t api_return_ns,
                         const PresentStatus& present_status = {}) {
  std::unique_lock lock(observation_state.mutex);
  const auto& snapshot = observation_state.snapshot;
  if (ticket.call_id == 0 || ticket.request_generation != snapshot.request_generation
      || snapshot.state != State::CAPTURING) return;
  Kind kind;
  switch (ticket.kind) {
    case Kind::NATIVE_FG_ENTRY: kind = Kind::NATIVE_FG_RETURN; break;
    case Kind::XEFG_TAG_ENTRY: kind = Kind::XEFG_TAG_RETURN; break;
    case Kind::XEFG_PRESENT_ENTRY: kind = Kind::XEFG_PRESENT_RETURN; break;
    default: return;
  }
  Append({.kind = kind, .api_return_ns = api_return_ns, .call_id = ticket.call_id,
      .api_result = result, .present_status = present_status});
}

inline void CopySnapshot(Snapshot* destination) {
  std::shared_lock lock(observation_state.mutex);
  *destination = observation_state.snapshot;
}

}  // namespace renodx::addons::dlss5::edit_fg_observation
#endif
