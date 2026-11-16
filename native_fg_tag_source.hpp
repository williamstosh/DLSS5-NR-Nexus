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

struct State {
  // Protected by the existing runtime_mutex, including the prune callback.
  std::vector<Version> versions;
  std::vector<std::pair<uint32_t, uint64_t>> viewports;
  uint64_t next_epoch = 0;
  uint64_t next_generation = 0;
};
static State state;

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
    previous = active;
    active = this;

    // Replacing this frame/role revokes its old tag, while another frame's
    // different clone remains valid for the delayed FG worker.
    std::erase_if(state.versions, [&](Version& version) {
      if (version.frame != frame || version.viewport != viewport) return false;
      RetirePresentResource(&version.clone);
      return true;
    });
    if (previous != nullptr) {
      for (auto& version : state.versions) {
        if (version.epoch == previous->epoch) {
          version.valid = false;
          version.reason = "tag_copy_nested_tag_call";
        }
      }
      return;
    }
    if (list == nullptr || tags == nullptr) return;
    const sl::ResourceTag* hudless = nullptr;
    for (uint32_t index = 0; index < count; ++index) {
      if (tags[index].type != sl::kBufferTypeHUDLessColor) continue;
      if (hudless != nullptr) return;
      hudless = &tags[index];
    }
    if (hudless == nullptr
        || (hudless->lifecycle != sl::ResourceLifecycle::eOnlyValidNow
            && hudless->lifecycle != sl::ResourceLifecycle::eValidUntilEvaluate)) return;
    source = ToStreamlineResourceRef(*hudless);
    if (source.resource == nullptr) return;
    source_identity = renodx::utils::directx::NativeIdentity(source.resource);
    if (source_identity == nullptr) return;
    const auto desc = source.resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels != 1
        || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1
        || source.extent.left != 0 || source.extent.top != 0
        || source.extent.width != desc.Width || source.extent.height != desc.Height) return;
    const auto recording = submission::RecordingIdentity(list);
    if (recording.first == 0 || (recording.first >> 63) != 0 || recording.second == 0) return;
    auto viewport_source = std::ranges::find(state.viewports, viewport,
        &std::pair<uint32_t, uint64_t>::first);
    if (viewport_source == state.viewports.end()) {
      if (state.viewports.size() == StreamlineViewportState::kFrames) return;
      state.viewports.emplace_back(viewport, ++state.next_generation);
      viewport_source = std::prev(state.viewports.end());
    }
    if (state.versions.size() == StreamlineViewportState::kFrames) {
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
  }

  void Finish(bool success) {
    if (finished) return;
    std::optional<RuntimeLock> lock;
    if (!HoldingRuntimeLock()) lock.emplace(runtime_mutex);
    finished = true;
    for (auto version = state.versions.begin(); version != state.versions.end(); ++version) {
      if (version->epoch != epoch) continue;
      if (success && version->valid && version->copies == 1 && version->clone != nullptr) {
        version->published = true;
        version->reason = "tag_copy_producer_unfinished";
      } else {
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
  const auto destination_identity = renodx::utils::directx::NativeIdentity(destination);
  if (destination_identity == nullptr) return;
  // An overwrite is a new content version even when it is not an eligible
  // SDK tag copy. Track that write before retiring our clone reference.
  std::erase_if(state.versions, [&](Version& version) {
    if (version.identity != destination_identity) return false;
    submission::TrackUse(list, version.clone);
    RetirePresentResource(&version.clone);
    return true;
  });
  auto* scope = Scope::active;
  if (scope == nullptr || scope->epoch == 0 || source == nullptr
      || scope->source_identity != renodx::utils::directx::NativeIdentity(source)) return;
  const auto version = std::ranges::find(state.versions, scope->epoch, &Version::epoch);
  if (version == state.versions.end() || !version->valid) return;
  ++version->copies;
  if (!full || version->copies != 1 || list != version->list
      || submission::RecordingIdentity(list) != version->recording
      || destination_identity == scope->source_identity) {
    version->valid = false;
    version->reason = "tag_copy_ambiguous_or_partial";
    return;
  }
  const auto from = source->GetDesc();
  const auto to = destination->GetDesc();
  if (to.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || to.MipLevels != 1
      || to.DepthOrArraySize != 1 || to.SampleDesc.Count != 1
      || from.Width != to.Width || from.Height != to.Height || from.Format != to.Format) {
    version->valid = false;
    version->reason = "tag_copy_resource_shape";
    return;
  }
  destination->AddRef();
  version->clone = destination;
  version->identity = destination_identity;
  submission::TrackUse(list, destination);
  submission::TrackUse(list, version->producer);
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
  fg::native_options::frame_source::Frame result{.reason = "tag_copy_source_unobserved"};
  if (hudless == nullptr || list == nullptr) return result;
  const auto identity = renodx::utils::directx::NativeIdentity(hudless);
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
  for (auto& version : state.versions) {
    RetirePresentResource(&version.clone);
  }
  state.versions.clear();
  state.viewports.clear();
}

}  // namespace native_tag_source
#endif
