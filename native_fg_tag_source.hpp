/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#if defined(NR_NEXUS_DEV_CONTROL)
// Composed inside dlss5::internal, after the Present retirement primitives.
namespace native_tag_source {

struct Version {
  uint32_t frame = 0;
  uint32_t viewport = 0;
  uint64_t epoch = 0;
  uint64_t generation = 0;
  ID3D12Resource* clone = nullptr;  // Owned until exact-use retirement.
  const void* identity = nullptr;
  const void* producer = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;  // Identity only; never dereferenced.
  std::pair<uint64_t, uint64_t> recording{};
  std::shared_ptr<submission::RecordingStatus> recording_status;
  FenceRef completion;
  uint64_t completion_value = 0;
  uint32_t copies = 0;
  bool published = false;
  bool valid = true;
  const char* reason = "tag_copy_unobserved";
};

struct Observation {
  uint64_t legacy_calls = 0;
  uint64_t for_frame_calls = 0;
  uint64_t hudless_calls = 0;
  uint64_t legacy_hudless_calls = 0;
  uint64_t for_frame_hudless_calls = 0;
  uint64_t scope_enters = 0;
  uint64_t copy_calls = 0;
  uint64_t scoped_copies = 0;
  uint64_t source_matches = 0;
  uint64_t publish_count = 0;
  uint64_t removal_count = 0;
  uint64_t reset_count = 0;
  uint64_t sequence = 0;
  uint64_t tag_sequence = 0;
  uint64_t gate_sequence = 0;
  uint64_t scope_sequence = 0;
  uint64_t copy_sequence = 0;
  uint64_t finish_sequence = 0;
  uint64_t publish_sequence = 0;
  uint64_t removal_sequence = 0;
  uint64_t query_sequence = 0;
  uint32_t tag_frame = 0;
  uint32_t tag_viewport = 0;
  uint32_t frame = 0;
  uint32_t viewport = 0;
  uint32_t lifecycle = UINT32_MAX;
  uintptr_t tag_native = 0;
  uintptr_t source_identity = 0;
  uintptr_t list_identity = 0;
  uint64_t recording_list = 0;
  uint64_t recording_generation = 0;
  uint64_t scope_epoch = 0;
  uintptr_t copy_list = 0;
  uintptr_t copy_source_native = 0;
  uintptr_t copy_destination_identity = 0;
  uintptr_t clone_identity = 0;
  uint64_t copy_epoch = 0;
  uint64_t publish_epoch = 0;
  uint64_t removal_epoch = 0;
  uintptr_t removal_identity = 0;
  uintptr_t query_identity = 0;
  size_t live_versions = 0;
  bool last_frame_bound = false;
  bool hooks_ready = false;
  bool copy_full = false;
  bool finish_success = false;
  const char* gate_reason = "gate_unobserved";
  const char* scope_reason = "scope_unobserved";
  const char* copy_reason = "copy_unobserved";
  const char* finish_reason = "finish_unobserved";
  const char* removal_reason = "removal_unobserved";
};

struct State {
  // Protected by the existing runtime_mutex, including the prune callback.
  std::vector<Version> versions;
  std::vector<std::pair<uint32_t, uint64_t>> viewports;
  uint64_t next_epoch = 0;
  uint64_t next_generation = 0;
  Observation observation;
};
static State state;

// Diagnostic callers hold runtime_mutex and enable the existing DEV experiment.
// Native pointers are values only; this observation acquires no COM references.
inline void ObserveTags(bool frame_bound, uint32_t frame, uint32_t viewport,
    const sl::ResourceTag* tags, uint32_t count) {
  auto& observation = state.observation;
  if (frame_bound) {
    ++observation.for_frame_calls;
  } else {
    ++observation.legacy_calls;
  }
  if (tags == nullptr) return;
  for (uint32_t index = 0; index < count; ++index) {
    if (tags[index].type != sl::kBufferTypeHUDLessColor) continue;
    observation.tag_sequence = ++observation.sequence;
    observation.last_frame_bound = frame_bound;
    observation.tag_frame = frame;
    observation.tag_viewport = viewport;
    observation.tag_native = 0;
    ++observation.hudless_calls;
    if (frame_bound) {
      ++observation.for_frame_hudless_calls;
    } else {
      ++observation.legacy_hudless_calls;
    }
    observation.lifecycle = static_cast<uint32_t>(tags[index].lifecycle);
    if (tags[index].resource != nullptr) {
      observation.tag_native = reinterpret_cast<uintptr_t>(tags[index].resource->native);
    }
    break;
  }
}

inline void NoteGate(const char* reason, ID3D12GraphicsCommandList* list, bool hooks_ready) {
  auto& observation = state.observation;
  observation.gate_sequence = ++observation.sequence;
  observation.gate_reason = reason;
  observation.list_identity = reinterpret_cast<uintptr_t>(list);
  observation.hooks_ready = hooks_ready;
}

inline Observation Snapshot() {
  auto observation = state.observation;
  observation.live_versions = state.versions.size();
  return observation;
}

inline void NoteRemoval(const Version& version, const char* reason) {
  auto& observation = state.observation;
  ++observation.removal_count;
  observation.removal_sequence = ++observation.sequence;
  observation.removal_epoch = version.epoch;
  observation.removal_identity = reinterpret_cast<uintptr_t>(version.identity);
  observation.removal_reason = reason;
}

struct Scope {
  static inline thread_local Scope* active = nullptr;
  Scope* previous = nullptr;
  StreamlineResourceRef source;
  const void* source_identity = nullptr;
  uint64_t epoch = 0;
  bool finished = false;

  Scope(ID3D12GraphicsCommandList* list, uint32_t frame, uint32_t viewport,
      const sl::ResourceTag* tags, uint32_t count) {
    std::optional<RuntimeLock> lock;
    if (!HoldingRuntimeLock()) lock.emplace(runtime_mutex);
    auto& observation = state.observation;
    ++observation.scope_enters;
    observation.scope_sequence = ++observation.sequence;
    observation.frame = frame;
    observation.viewport = viewport;
    observation.source_identity = 0;
    observation.list_identity = reinterpret_cast<uintptr_t>(list);
    observation.recording_list = 0;
    observation.recording_generation = 0;
    observation.scope_epoch = 0;
    observation.scope_reason = "scope_entered";
    previous = active;
    active = this;

    // Replacing this frame/role revokes its old tag, while another frame's
    // different clone remains valid for the delayed FG worker.
    std::erase_if(state.versions, [&](Version& version) {
      if (version.frame != frame || version.viewport != viewport) return false;
      NoteRemoval(version, "same_frame_role_retagged");
      RetirePresentResource(&version.clone);
      return true;
    });
    if (previous != nullptr) {
      observation.scope_reason = "nested_tag_call";
      for (auto& version : state.versions) {
        if (version.epoch == previous->epoch) {
          version.valid = false;
          version.reason = "tag_copy_nested_tag_call";
        }
      }
      return;
    }
    if (list == nullptr || tags == nullptr) {
      observation.scope_reason = (list == nullptr ? "native_list_missing" : "tags_missing");
      return;
    }
    const sl::ResourceTag* hudless = nullptr;
    for (uint32_t index = 0; index < count; ++index) {
      if (tags[index].type != sl::kBufferTypeHUDLessColor) continue;
      if (hudless != nullptr) {
        observation.scope_reason = "hudless_tags_ambiguous";
        return;
      }
      hudless = &tags[index];
      observation.lifecycle = static_cast<uint32_t>(hudless->lifecycle);
      observation.tag_native = (hudless->resource != nullptr
          ? reinterpret_cast<uintptr_t>(hudless->resource->native) : 0);
    }
    if (hudless == nullptr
        || (hudless->lifecycle != sl::ResourceLifecycle::eOnlyValidNow
            && hudless->lifecycle != sl::ResourceLifecycle::eValidUntilEvaluate)) {
      observation.scope_reason = (hudless == nullptr ? "hudless_tag_missing" : "tag_lifecycle_ineligible");
      return;
    }
    source = ToStreamlineResourceRef(*hudless);
    if (source.resource == nullptr) {
      observation.scope_reason = "source_resource_unavailable";
      return;
    }
    source_identity = renodx::utils::directx::NativeIdentity(source.resource);
    observation.source_identity = reinterpret_cast<uintptr_t>(source_identity);
    if (source_identity == nullptr) {
      observation.scope_reason = "source_identity_unavailable";
      return;
    }
    const auto desc = source.resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels != 1
        || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1
        || source.extent.left != 0 || source.extent.top != 0
        || source.extent.width != desc.Width || source.extent.height != desc.Height) {
      observation.scope_reason = "source_shape_or_extent_ineligible";
      return;
    }
    const auto recording = submission::RecordingIdentity(list);
    observation.recording_list = recording.first;
    observation.recording_generation = recording.second;
    if (recording.first == 0 || (recording.first >> 63) != 0 || recording.second == 0) {
      observation.scope_reason = "recording_identity_unavailable";
      return;
    }
    auto viewport_source = std::ranges::find(state.viewports, viewport,
        &std::pair<uint32_t, uint64_t>::first);
    if (viewport_source == state.viewports.end()) {
      if (state.viewports.size() == StreamlineViewportState::kFrames) {
        observation.scope_reason = "viewport_capacity";
        return;
      }
      state.viewports.emplace_back(viewport, ++state.next_generation);
      viewport_source = std::prev(state.viewports.end());
    }
    if (state.versions.size() == StreamlineViewportState::kFrames) {
      NoteRemoval(state.versions.front(), "version_capacity_evicted");
      RetirePresentResource(&state.versions.front().clone);
      state.versions.erase(state.versions.begin());
    }
    epoch = ++state.next_epoch;
    state.versions.push_back({
        .frame = frame,
        .viewport = viewport,
        .epoch = epoch,
        .generation = viewport_source->second,
        .producer = reinterpret_cast<const void*>(0xFFFC'0000'0000'0000ull | epoch),
        .list = list,
        .recording = recording,
        .recording_status = submission::ObserveRecording(list),
    });
    observation.scope_epoch = epoch;
    observation.scope_reason = "copy_pending";
  }

  void Finish(bool success) {
    if (finished) return;
    std::optional<RuntimeLock> lock;
    if (!HoldingRuntimeLock()) lock.emplace(runtime_mutex);
    auto& observation = state.observation;
    observation.finish_sequence = ++observation.sequence;
    observation.finish_success = success;
    observation.finish_reason = (epoch == 0 ? "scope_ineligible" : "version_missing_at_finish");
    finished = true;
    for (auto version = state.versions.begin(); version != state.versions.end(); ++version) {
      if (version->epoch != epoch) continue;
      if (success && version->valid && version->copies == 1 && version->clone != nullptr) {
        version->published = true;
        version->reason = "tag_copy_producer_unfinished";
        ++observation.publish_count;
        observation.publish_sequence = ++observation.sequence;
        observation.publish_epoch = version->epoch;
        observation.finish_reason = "published";
      } else {
        if (!success) {
          observation.finish_reason = "sdk_call_failed";
        } else if (!version->valid) {
          observation.finish_reason = version->reason;
        } else {
          observation.finish_reason = (version->copies == 0 ? "copy_unobserved" : "copy_not_unique_or_clone_missing");
        }
        NoteRemoval(*version, "finish_failed");
        RetirePresentResource(&version->clone);
        state.versions.erase(version);
      }
      break;
    }
    epoch = 0;
  }

  ~Scope() {
    Finish(false);
    active = previous;
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
  Scope(Scope&&) = delete;
  Scope& operator=(Scope&&) = delete;
};

inline void ObserveCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
    ID3D12Resource* destination, bool full) {
  if (list == nullptr || destination == nullptr) return;
  std::optional<RuntimeLock> lock;
  if (!HoldingRuntimeLock()) lock.emplace(runtime_mutex);
  auto& observation = state.observation;
  ++observation.copy_calls;
  observation.copy_sequence = ++observation.sequence;
  observation.copy_list = reinterpret_cast<uintptr_t>(list);
  observation.copy_source_native = reinterpret_cast<uintptr_t>(source);
  observation.copy_full = full;
  observation.copy_epoch = 0;
  observation.copy_reason = "destination_identity_unavailable";
  const auto destination_identity = renodx::utils::directx::NativeIdentity(destination);
  observation.copy_destination_identity = reinterpret_cast<uintptr_t>(destination_identity);
  if (destination_identity == nullptr) return;
  // An overwrite is a new content version even when it is not an eligible
  // SDK tag copy. Track that write before retiring our clone reference.
  std::erase_if(state.versions, [&](Version& version) {
    if (version.identity != destination_identity) return false;
    submission::TrackUse(list, version.clone);
    NoteRemoval(version, "clone_overwritten");
    RetirePresentResource(&version.clone);
    return true;
  });
  auto* scope = Scope::active;
  observation.copy_reason = "outside_tag_scope";
  if (scope == nullptr) return;
  ++observation.scoped_copies;
  observation.copy_reason = "scope_or_source_ineligible";
  if (scope->epoch == 0 || source == nullptr) return;
  observation.copy_reason = "tag_source_identity_mismatch";
  if (scope->source_identity != renodx::utils::directx::NativeIdentity(source)) return;
  ++observation.source_matches;
  observation.copy_epoch = scope->epoch;
  observation.copy_reason = "scope_version_missing_or_invalid";
  const auto version = std::ranges::find(state.versions, scope->epoch, &Version::epoch);
  if (version == state.versions.end() || !version->valid) return;
  ++version->copies;
  if (!full || version->copies != 1 || list != version->list
      || submission::RecordingIdentity(list) != version->recording
      || destination_identity == scope->source_identity) {
    version->valid = false;
    version->reason = "tag_copy_ambiguous_or_partial";
    if (!full) {
      observation.copy_reason = "partial_copy";
    } else if (version->copies != 1) {
      observation.copy_reason = "multiple_source_copies";
    } else if (list != version->list) {
      observation.copy_reason = "native_list_mismatch";
    } else {
      observation.copy_reason = (destination_identity == scope->source_identity
          ? "source_destination_alias" : "recording_mismatch");
    }
    return;
  }
  const auto from = source->GetDesc();
  const auto to = destination->GetDesc();
  if (to.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || to.MipLevels != 1
      || to.DepthOrArraySize != 1 || to.SampleDesc.Count != 1
      || from.Width != to.Width || from.Height != to.Height || from.Format != to.Format) {
    version->valid = false;
    version->reason = "tag_copy_resource_shape";
    observation.copy_reason = "clone_shape_ineligible";
    return;
  }
  destination->AddRef();
  version->clone = destination;
  version->identity = destination_identity;
  submission::TrackUse(list, destination);
  submission::TrackUse(list, version->producer);
  observation.clone_identity = reinterpret_cast<uintptr_t>(destination_identity);
  observation.copy_reason = "clone_recorded";
}

// Called with runtime_mutex while the submission tracker holds its own lock.
// Preserve receipts only: retirement and tracker queries belong to other paths.
inline void PreserveProof(const void* marker,
    const std::vector<submission::GenerationSubmission>& receipts) {
  for (auto& version : state.versions) {
    if (version.producer != marker) continue;
    if (receipts.size() != 1 || receipts.front().pending || receipts.front().fence.empty()
        || std::pair{receipts.front().list_identity, receipts.front().recording_generation} != version.recording) {
      version.valid = false;
      version.reason = (receipts.empty() ? "tag_copy_producer_discarded" : "tag_copy_producer_replayed");
      continue;
    }
    version.completion = receipts.front().fence;
    version.completion_value = receipts.front().value;
  }
}

// Caller holds runtime_mutex. No waits and no global-latest-frame fallback.
inline fg::native_options::frame_source::Frame CurrentFrame(
    ID3D12Resource* hudless, ID3D12GraphicsCommandList* list) {
  auto& observation = state.observation;
  observation.query_sequence = ++observation.sequence;
  observation.query_identity = 0;
  fg::native_options::frame_source::Frame result{.reason = "tag_copy_source_unobserved"};
  if (hudless == nullptr || list == nullptr) return result;
  const auto identity = renodx::utils::directx::NativeIdentity(hudless);
  observation.query_identity = reinterpret_cast<uintptr_t>(identity);
  if (identity == nullptr) return result;
  const auto version = std::ranges::find(state.versions, identity, &Version::identity);
  if (version == state.versions.end()) return result;
  result.frame = version->frame;
  result.epoch = version->epoch;
  result.generation = version->generation;
  result.reason = version->reason;
  if (!version->valid || !version->published) return result;
  if (version->recording_status == nullptr
      || (version->recording_status->closed.load() && !version->recording_status->submitted.load())) {
    result.reason = "tag_copy_producer_discarded";
    return result;
  }
  if (!submission::OnlyRecordingUses(version->producer, version->recording)) {
    result.reason = "tag_copy_producer_replayed";
    return result;
  }
  std::vector<std::pair<FenceRef, uint64_t>> proofs;
  const auto proof = submission::UseSubmissionTokens(version->producer, &proofs);
  if (proofs.size() > 1) {
    result.reason = "tag_copy_producer_replayed";
    return result;
  }
  const auto recording = submission::RecordingIdentity(list);
  if (list == version->list && recording == version->recording
      && !version->recording_status->closed.load() && !version->recording_status->submitted.load()) {
    result.available = true;
    result.reason = "tag_copy_same_recording";
    return result;
  }
  const FenceRef* fence = nullptr;
  uint64_t value = 0;
  if (proof == submission::UseSubmission::kTokens && proofs.size() == 1) {
    fence = &proofs.front().first;
    value = proofs.front().second;
  } else if (proof == submission::UseSubmission::kComplete && !version->completion.empty()) {
    fence = &version->completion;
    value = version->completion_value;
  }
  if (fence == nullptr || fence->empty()) {
    result.reason = "tag_copy_producer_unfinished";
    return result;
  }
  const auto completed = fence->fence->GetCompletedValue();
  if (completed == UINT64_MAX || completed < value) {
    result.reason = "tag_copy_producer_unfinished";
    return result;
  }
  result.available = true;
  result.reason = "tag_copy_completed_producer";
  return result;
}

// Caller holds runtime_mutex; an in-flight Scope can no longer publish after
// its version is erased. Serial identities are never recycled by Reset.
inline void Reset() {
  ++state.observation.reset_count;
  for (auto& version : state.versions) {
    NoteRemoval(version, "reset");
    RetirePresentResource(&version.clone);
  }
  state.versions.clear();
  state.viewports.clear();
}

}  // namespace native_tag_source
#endif
