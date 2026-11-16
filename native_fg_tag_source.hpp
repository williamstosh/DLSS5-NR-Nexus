/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#if defined(NR_NEXUS_DEV_CONTROL)
// Composed inside dlss5::internal, after the Present retirement primitives.
namespace native_tag_source {

struct PremiseRecording {
  uint64_t list = 0, generation = 0;
  uint32_t receipts = 0;
  bool submitted = false, pending = true;
  uintptr_t fence = 0;
  uint64_t value = 0;
};
// Never creates a tracker entry, private identity, use, or completion signal.
inline PremiseRecording ReadPremiseRecording(uint64_t identity, uint64_t generation_id) {
  std::lock_guard lock(submission::tracker_mutex);
  const auto entry = submission::tracked_lists.find(identity);
  if (entry == submission::tracked_lists.end()) return {.list = identity, .generation = generation_id};
  PremiseRecording result{.list = identity, .generation = (generation_id != 0 ? generation_id : entry->second.current)};
  const auto generation = entry->second.generations.find(result.generation);
  if (generation == entry->second.generations.end()) return result;
  result.receipts = static_cast<uint32_t>(generation->second.submissions.size());
  result.submitted = generation->second.status != nullptr && generation->second.status->submitted.load();
  if (result.receipts == 1) {
    const auto& receipt = generation->second.submissions.front();
    result.pending = receipt.pending;
    result.fence = reinterpret_cast<uintptr_t>(receipt.fence.fence);
    result.value = receipt.value;
  }
  return result;
}
inline PremiseRecording ReadPremiseRecording(ID3D12CommandList* list) {
  if (list == nullptr) return {};
  const auto identity = submission::ListIdentity(list, false);
  return ((identity >> 63) != 0 ? PremiseRecording{} : ReadPremiseRecording(identity, 0));
}
enum class PremiseKind : uint32_t { TAG, LEGACY, ENHANCED, COPY, RESET, SUBMIT };
struct PremiseEvent {
  uint64_t sequence = 0;
  int64_t qpc = 0;
  DWORD thread = 0;
  PremiseKind kind = PremiseKind::TAG;
  uintptr_t list = 0;
  PremiseRecording recording;
  uintptr_t source = 0, destination = 0;
  bool full = false;
  D3D12_RESOURCE_BARRIER legacy{};
  D3D12_TEXTURE_BARRIER enhanced{};
  uintptr_t queue = 0;
};
struct PremiseHooks {
  bool legacy = false, list7_acquired = false, enhanced = false;
};
struct PremiseSnapshot {
  uint64_t epoch = 0;
  uintptr_t native = 0, identity = 0;
  uint32_t viewport = 0, frame = 0, declared_state = UINT32_MAX, list_type = UINT32_MAX;
  PremiseHooks hooks;
  std::array<uint64_t, 2> barrier_calls{};
  PremiseEvent tag;
  uint64_t return_sequence = 0;
  bool returned = false, success = false;
  std::array<PremiseEvent, 5> before{};
  PremiseEvent submitted_after_tag;
  const char* boundary = "unbound";
};
struct PremiseState {
  std::shared_mutex mutex;
  uint64_t sequence = 0, epoch = 0;
  std::array<uint64_t, 2> barrier_calls{};
  PremiseSnapshot snapshot;
  std::array<PremiseEvent, 5> live{};
};
static PremiseState premise;

inline uintptr_t PremiseNative() {
  std::shared_lock lock(premise.mutex);
  return premise.snapshot.native;
}
inline uintptr_t ObservePremiseBarrierCall(PremiseKind kind) {
  std::unique_lock lock(premise.mutex);
  if (premise.snapshot.native != 0) {
    ++premise.barrier_calls[kind == PremiseKind::LEGACY ? 0 : 1];
  }
  return premise.snapshot.native;
}
inline void ObservePremiseEvent(PremiseEvent event) {
  event.qpc = fg::native_options::frame_source::Qpc();
  event.thread = GetCurrentThreadId();
  std::unique_lock lock(premise.mutex);
  auto& sample = premise.snapshot;
  if (sample.native == 0 || event.kind == PremiseKind::TAG) return;
  if ((event.kind == PremiseKind::LEGACY || event.kind == PremiseKind::ENHANCED) && event.source != sample.native) return;
  if (event.kind == PremiseKind::COPY && event.destination != sample.native) return;
  if (event.kind == PremiseKind::SUBMIT && (event.recording.list == 0 || event.recording.generation == 0
      || event.recording.list != sample.tag.recording.list || event.recording.generation != sample.tag.recording.generation)) return;
  if ((event.kind == PremiseKind::RESET || event.kind == PremiseKind::SUBMIT) && event.list != sample.tag.list
      && std::none_of(premise.live.begin(), premise.live.end(), [&](const PremiseEvent& previous) { return previous.list == event.list; })) return;
  event.sequence = ++premise.sequence;
  if (event.kind == PremiseKind::RESET) {
    for (auto& previous : premise.live) {
      if (previous.list == event.list) {
        previous = {};
      }
    }
    if (event.list == sample.tag.list) {
      sample.boundary = (event.full ? "list_reset" : "reset_failed_or_destroyed");
    }
  }
  premise.live[static_cast<size_t>(event.kind) - 1] = event;
  if (event.kind == PremiseKind::SUBMIT && event.list == sample.tag.list) {
    sample.submitted_after_tag = event;
  }
}
// Freeze the commands seen before this tag, not the latest commands when a
// later FG worker samples them. There is one watched resource, no state grant.
inline uint64_t FreezePremiseTag(PremiseSnapshot sample) {
  sample.tag.qpc = fg::native_options::frame_source::Qpc();
  sample.tag.thread = GetCurrentThreadId();
  {
    std::unique_lock lock(premise.mutex);
    if (premise.snapshot.native != sample.native || premise.snapshot.viewport != sample.viewport
        || premise.snapshot.identity != sample.identity) {
      premise.live = {};
      premise.barrier_calls = {};
      ++premise.epoch;
      sample.boundary = "resource_or_viewport_changed";
    } else {
      sample.boundary = "tag_entry_recording_only";
    }
    sample.epoch = premise.epoch;
    sample.barrier_calls = premise.barrier_calls;
    sample.before = premise.live;
    for (auto& event : sample.before) {
      if (event.qpc > sample.tag.qpc) {
        event = {};
      }
    }
    sample.tag.sequence = ++premise.sequence;
    premise.snapshot = sample;
  }
  return sample.tag.sequence;
}
inline void FinishPremiseTag(uint64_t ticket, bool success) {
  if (ticket == 0) return;
  std::unique_lock lock(premise.mutex);
  if (premise.snapshot.tag.sequence != ticket) return;
  premise.snapshot.return_sequence = ++premise.sequence;
  premise.snapshot.returned = true;
  premise.snapshot.success = success;
}
inline void ObservePremiseReset(ID3D12GraphicsCommandList* list, bool success) {
  if (PremiseNative() == 0) return;
  renodx::utils::directx::NativeFromReShadeProxy(&list);
  ObservePremiseEvent({.kind = PremiseKind::RESET, .list = reinterpret_cast<uintptr_t>(list),
      .recording = ReadPremiseRecording(list), .full = success});
}
inline void ObservePremiseCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
    ID3D12Resource* destination, bool full) {
  if (!NativePremiseEnabled()) return;
  const auto watched = PremiseNative();
  if (watched == 0 || reinterpret_cast<uintptr_t>(destination) != watched) return;
  ObservePremiseEvent({.kind = PremiseKind::COPY, .list = reinterpret_cast<uintptr_t>(list),
      .recording = ReadPremiseRecording(list), .source = reinterpret_cast<uintptr_t>(source),
      .destination = watched, .full = full});
}
inline void ObservePremiseSubmit(ID3D12CommandQueue* queue, const std::vector<submission::PendingSubmission>& pending) {
  PremiseEvent target;
  {
    std::shared_lock lock(premise.mutex);
    target = premise.snapshot.tag;
  }
  if (target.recording.list == 0 || target.recording.generation == 0) return;
  for (const auto& captured : pending) {
    if (captured.list != target.recording.list || captured.generation != target.recording.generation) continue;
    ObservePremiseEvent({.kind = PremiseKind::SUBMIT, .list = target.list,
        .recording = ReadPremiseRecording(captured.list, captured.generation), .queue = reinterpret_cast<uintptr_t>(queue)});
  }
}
inline void ResetPremise() {
  std::unique_lock lock(premise.mutex);
  premise.snapshot = {};
  premise.snapshot.boundary = "source_reset";
  premise.live = {};
  premise.barrier_calls = {};
  ++premise.epoch;
}
inline void ForgetPremiseResource(uintptr_t resource) {
  std::unique_lock lock(premise.mutex);
  if (resource != premise.snapshot.native && resource != premise.snapshot.identity) return;
  premise.snapshot = {};
  premise.snapshot.boundary = "resource_destroyed";
  premise.live = {};
  premise.barrier_calls = {};
  ++premise.epoch;
}

inline void AppendPremiseObservation(std::ostream& line) {
  PremiseSnapshot sample;
  {
    std::shared_lock lock(premise.mutex);
    sample = premise.snapshot;
  }
  line << " premise=[epoch:" << sample.epoch << ",native:" << sample.native << ",identity:" << sample.identity
       << ",viewport:" << sample.viewport << ",frame:" << sample.frame << ",declared_state:" << sample.declared_state
       << ",list_type:" << sample.list_type << ",legacy_hook:" << sample.hooks.legacy
       << ",list7_acquired:" << sample.hooks.list7_acquired << ",enhanced_hook:" << sample.hooks.enhanced
       << ",legacy_calls:" << sample.barrier_calls[0] << ",enhanced_calls:" << sample.barrier_calls[1]
       << ",boundary:" << sample.boundary << ",return_seq:" << sample.return_sequence
       << ",returned:" << sample.returned << ",ok:" << sample.success << ']';
  const auto append = [&](const char* name, const PremiseEvent& event) {
    line << ' ' << name << "=[seq:" << event.sequence << ",qpc:" << event.qpc << ",thread:" << event.thread
         << ",list:" << event.list << ",recording:" << event.recording.list << '/' << event.recording.generation
         << ",receipts:" << event.recording.receipts << ",submitted:" << event.recording.submitted
         << ",pending:" << event.recording.pending << ",fence:" << event.recording.fence << ",value:" << event.recording.value;
    if (event.kind == PremiseKind::LEGACY) {
      line << ",type:" << event.legacy.Type << ",flags:" << event.legacy.Flags;
      if (event.legacy.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
        line << ",subresource:" << event.legacy.Transition.Subresource << ",before:" << event.legacy.Transition.StateBefore
             << ",after:" << event.legacy.Transition.StateAfter;
      } else if (event.legacy.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
        line << ",alias_before:" << reinterpret_cast<uintptr_t>(event.legacy.Aliasing.pResourceBefore)
             << ",alias_after:" << reinterpret_cast<uintptr_t>(event.legacy.Aliasing.pResourceAfter);
      } else {
        line << ",uav_resource:" << reinterpret_cast<uintptr_t>(event.legacy.UAV.pResource);
      }
    } else if (event.kind == PremiseKind::ENHANCED) {
      const auto& barrier = event.enhanced;
      const auto& range = barrier.Subresources;
      line << ",sync_before:" << barrier.SyncBefore << ",sync_after:" << barrier.SyncAfter
           << ",access_before:" << barrier.AccessBefore << ",access_after:" << barrier.AccessAfter
           << ",layout_before:" << barrier.LayoutBefore << ",layout_after:" << barrier.LayoutAfter
           << ",flags:" << barrier.Flags << ",range:" << range.IndexOrFirstMipLevel << '/' << range.NumMipLevels
           << '/' << range.FirstArraySlice << '/' << range.NumArraySlices << '/' << range.FirstPlane << '/' << range.NumPlanes;
    } else {
      line << ",source:" << event.source << ",destination:" << event.destination << ",full:" << event.full << ",queue:" << event.queue;
    }
    line << ']';
  };
  append("premise_tag", sample.tag);
  for (size_t index = 0; index < sample.before.size(); ++index) {
    if (sample.before[index].sequence != 0) {
      append(("premise_before" + std::to_string(index)).c_str(), sample.before[index]);
    }
  }
  append("premise_submit_after_tag", sample.submitted_after_tag);
  line << " premise_evidence=recorded_api_order_only gpu_completion=unqueried enhanced_state_mapping=none"
       << " region_full_write=unknown";
}

struct Version {
  uint32_t frame = 0;
  uint32_t viewport = 0;
  uint64_t epoch = 0;
  uint64_t generation = 0;
  // Owned SDK clone, or original direct-tag resource; exact-use retirement.
  ID3D12Resource* clone = nullptr;
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
  bool direct = false;
  bool claimed = false;
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
  uint64_t claim_count = 0;
  uint64_t sequence = 0;
  uint64_t tag_sequence = 0;
  uint64_t gate_sequence = 0;
  uint64_t scope_sequence = 0;
  uint64_t copy_sequence = 0;
  uint64_t finish_sequence = 0;
  uint64_t publish_sequence = 0;
  uint64_t removal_sequence = 0;
  uint64_t query_sequence = 0;
  uint64_t claim_sequence = 0;
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
  uint64_t claim_epoch = 0;
  uintptr_t removal_identity = 0;
  uintptr_t query_identity = 0;
  size_t live_versions = 0;
  size_t query_pending = 0;
  bool last_frame_bound = false;
  bool hooks_ready = false;
  bool copy_full = false;
  bool finish_success = false;
  bool query_direct = false;
  bool direct_capacity_blocked = false;
  const char* gate_reason = "gate_unobserved";
  const char* scope_reason = "scope_unobserved";
  const char* copy_reason = "copy_unobserved";
  const char* finish_reason = "finish_unobserved";
  const char* removal_reason = "removal_unobserved";
  const char* claim_reason = "claim_unobserved";
  const char* direct_block_reason = "none";
};

struct State {
  // Protected by the existing runtime_mutex, including the prune callback.
  std::vector<Version> versions;
  std::vector<std::pair<uint32_t, uint64_t>> viewports;
  uint64_t next_epoch = 0;
  uint64_t next_generation = 0;
  bool direct_capacity_blocked = false;
  const char* direct_unresolved_reason = nullptr;
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
  observation.direct_capacity_blocked = state.direct_capacity_blocked;
  if (state.direct_unresolved_reason != nullptr) {
    observation.direct_block_reason = state.direct_unresolved_reason;
  }
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
  bool has_hudless = false;
  bool direct_lifecycle = false;
  uint64_t premise_ticket = 0;

  Scope(ID3D12GraphicsCommandList* list, uint32_t frame, uint32_t viewport,
      const sl::ResourceTag* tags, uint32_t count, PremiseHooks premise_hooks = {}) {
    std::optional<RuntimeLock> lock;
    if (!HoldingRuntimeLock()) {
      lock.emplace(runtime_mutex);
    }
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
    direct_lifecycle = std::ranges::any_of(state.versions, &Version::direct);
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
    if (tags == nullptr) {
      observation.scope_reason = "tags_missing";
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
      has_hudless = true;
      observation.lifecycle = static_cast<uint32_t>(hudless->lifecycle);
      observation.tag_native = (hudless->resource != nullptr
          ? reinterpret_cast<uintptr_t>(hudless->resource->native) : 0);
    }
    if (hudless == nullptr
        || (hudless->lifecycle != sl::ResourceLifecycle::eOnlyValidNow
            && hudless->lifecycle != sl::ResourceLifecycle::eValidUntilEvaluate
            && hudless->lifecycle != sl::ResourceLifecycle::eValidUntilPresent)) {
      observation.scope_reason = (hudless == nullptr ? "hudless_tag_missing" : "tag_lifecycle_ineligible");
      return;
    }
    const bool direct = hudless->lifecycle == sl::ResourceLifecycle::eValidUntilPresent;
    direct_lifecycle = direct_lifecycle || direct;
    if (!direct && list == nullptr) {
      observation.scope_reason = "native_list_missing";
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
    if (NativePremiseEnabled()) {
      auto* native = source.resource;
      renodx::utils::directx::NativeFromReShadeProxy(&native);
      premise_ticket = FreezePremiseTag({.native = reinterpret_cast<uintptr_t>(native),
          .identity = reinterpret_cast<uintptr_t>(source_identity), .viewport = viewport, .frame = frame,
          .declared_state = source.state, .list_type = (list != nullptr ? static_cast<uint32_t>(list->GetType()) : UINT32_MAX),
          .hooks = premise_hooks, .tag = {.list = reinterpret_cast<uintptr_t>(list), .recording = ReadPremiseRecording(list)}});
    }
    std::pair<uint64_t, uint64_t> recording{};
    if (!direct) {
      recording = submission::RecordingIdentity(list);
      observation.recording_list = recording.first;
      observation.recording_generation = recording.second;
      if (recording.first == 0 || (recording.first >> 63) != 0 || recording.second == 0) {
        observation.scope_reason = "recording_identity_unavailable";
        return;
      }
    }
    // A new tag reusing a consumed original resource cannot inherit the old
    // pair's direct ticket. Unclaimed tokens survive, including ambiguity.
    std::erase_if(state.versions, [&](Version& version) {
      if (!version.direct || !version.claimed || version.identity != source_identity) return false;
      NoteRemoval(version, "claimed_direct_resource_retagged");
      RetirePresentResource(&version.clone);
      return true;
    });
    auto viewport_source = std::ranges::find(state.viewports, viewport,
        &std::pair<uint32_t, uint64_t>::first);
    if (viewport_source == state.viewports.end()) {
      if (state.viewports.size() == StreamlineViewportState::kFrames) {
        if (direct) {
          state.direct_capacity_blocked = true;
        }
        observation.scope_reason = "viewport_capacity";
        return;
      }
      state.viewports.emplace_back(viewport, ++state.next_generation);
      viewport_source = std::prev(state.viewports.end());
    }
    if (state.versions.size() == StreamlineViewportState::kFrames) {
      auto oldest = state.versions.begin();
      if (direct) {
        oldest = std::ranges::find_if(state.versions, [](const Version& version) {
          return !version.direct || version.claimed;
        });
        if (oldest == state.versions.end()) {
          state.direct_capacity_blocked = true;
          observation.scope_reason = "unclaimed_direct_capacity";
          return;
        }
      } else if (oldest->direct && !oldest->claimed) {
        // Losing an unclaimed token must never make another direct token
        // look unique. Clone admission retains its existing bounded policy.
        state.direct_capacity_blocked = true;
      }
      NoteRemoval(*oldest, "version_capacity_evicted");
      RetirePresentResource(&oldest->clone);
      state.versions.erase(oldest);
    }
    epoch = ++state.next_epoch;
    state.versions.push_back({
        .frame = frame,
        .viewport = viewport,
        .epoch = epoch,
        .generation = viewport_source->second,
        .producer = (direct ? nullptr : reinterpret_cast<const void*>(0xFFFC'0000'0000'0000ull | epoch)),
        .list = list,
        .recording = recording,
        .recording_status = (direct ? nullptr : submission::ObserveRecording(list)),
        .direct = direct,
    });
    if (direct) {
      source.resource->AddRef();
      state.versions.back().clone = source.resource;
      state.versions.back().identity = source_identity;
    }
    observation.scope_epoch = epoch;
    observation.scope_reason = (direct ? "direct_tag_pending" : "copy_pending");
  }

  void Finish(bool success) {
    if (finished) return;
    FinishPremiseTag(premise_ticket, success);
    std::optional<RuntimeLock> lock;
    if (!HoldingRuntimeLock()) {
      lock.emplace(runtime_mutex);
    }
    auto& observation = state.observation;
    observation.finish_sequence = ++observation.sequence;
    observation.finish_success = success;
    observation.finish_reason = (epoch == 0 ? "scope_ineligible" : "version_missing_at_finish");
    finished = true;
    bool published = false;
    for (auto version = state.versions.begin(); version != state.versions.end(); ++version) {
      if (version->epoch != epoch) continue;
      const bool content_observed = (version->direct ? version->copies == 0 : version->copies == 1);
      if (success && version->valid && content_observed && version->clone != nullptr) {
        version->published = true;
        published = true;
        version->reason = (version->direct ? "direct_tag_unclaimed" : "tag_copy_producer_unfinished");
        ++observation.publish_count;
        observation.publish_sequence = ++observation.sequence;
        observation.publish_epoch = version->epoch;
        observation.finish_reason = (version->direct ? "direct_published" : "published");
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
    if (success && has_hudless && direct_lifecycle && !published) {
      // A real successful tag which cannot be represented may have replaced
      // SDK input. Do not grandfather a previous direct pair's resource ticket.
      state.direct_unresolved_reason = "direct_tag_unresolved_successful_tag";
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
  if (!HoldingRuntimeLock()) {
    lock.emplace(runtime_mutex);
  }
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
    if (version.direct && !version.claimed) {
      // A recorded overwrite may execute after an older SDK consumer on a
      // different queue. Keep the unresolved token so a later tag cannot
      // become uniquely eligible just because this CPU copy was observed.
      version.valid = false;
      version.reason = "direct_tag_unclaimed_overwrite";
      return false;
    }
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
  if (version->direct) {
    version->valid = false;
    version->reason = "direct_tag_unexpected_source_copy";
    observation.copy_reason = version->reason;
    return;
  }
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
    if (version.direct || version.producer != marker) continue;
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
    ID3D12Resource* hudless, ID3D12GraphicsCommandList* list, uint64_t claimed_epoch = 0) {
  auto& observation = state.observation;
  observation.query_sequence = ++observation.sequence;
  observation.query_identity = 0;
  observation.query_pending = 0;
  observation.query_direct = false;
  fg::native_options::frame_source::Frame result{.reason = "tag_copy_source_unobserved"};
  if (hudless == nullptr || list == nullptr) return result;
  const auto identity = renodx::utils::directx::NativeIdentity(hudless);
  observation.query_identity = reinterpret_cast<uintptr_t>(identity);
  if (identity == nullptr) return result;
  auto version = state.versions.end();
  size_t matching = 0;
  for (auto candidate = state.versions.begin(); candidate != state.versions.end(); ++candidate) {
    if (candidate->identity != identity) continue;
    ++matching;
    if (candidate->direct && !candidate->claimed) {
      ++observation.query_pending;
    }
    if (claimed_epoch == 0 || claimed_epoch == candidate->epoch) {
      version = candidate;
    }
  }
  if (matching > 1) {
    result.reason = "tagged_resource_tokens_ambiguous";
    return result;
  }
  if (version == state.versions.end()) return result;
  observation.query_direct = version->direct;
  result.frame = version->frame;
  result.epoch = version->epoch;
  result.generation = version->generation;
  result.reason = version->reason;
  if (!version->valid || !version->published) return result;
  if (version->direct) {
    if (state.direct_unresolved_reason != nullptr) {
      result.reason = state.direct_unresolved_reason;
      return result;
    }
    if (state.direct_capacity_blocked) {
      result.reason = "direct_tag_capacity_unresolved";
      return result;
    }
    if (version->claimed != (claimed_epoch != 0)) {
      result.reason = (version->claimed ? "direct_tag_requires_claimed_epoch" : "direct_tag_not_claimed");
      return result;
    }
    // This is the actual NGX input resource, not a claim that tagging
    // completed any GPU work. The caller retains NGX's consumer-list contract.
    result.available = true;
    result.reason = (version->claimed ? "direct_tag_claimed" : "direct_tag_unique_unclaimed");
    return result;
  }
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

// Caller holds runtime_mutex. Freeze only the exact ticket just inspected;
// a second pending token on the same resource cannot be consumed by guessing.
inline bool ClaimFrame(const fg::native_options::frame_source::Frame& frame,
    ID3D12Resource* hudless, ID3D12GraphicsCommandList* list) {
  auto& observation = state.observation;
  observation.claim_sequence = ++observation.sequence;
  observation.claim_epoch = frame.epoch;
  observation.claim_reason = "frame_ticket_invalid";
  if (!frame.available || frame.epoch == 0) return false;
  const auto version = std::ranges::find(state.versions, frame.epoch, &Version::epoch);
  if (version == state.versions.end()) return false;
  const uint64_t preferred = (version->direct && !version->claimed ? 0 : frame.epoch);
  const auto current = CurrentFrame(hudless, list, preferred);
  if (!current.available || current.epoch != frame.epoch || current.frame != frame.frame
      || current.generation != frame.generation) {
    observation.claim_reason = "frame_ticket_changed_or_ambiguous";
    return false;
  }
  // This reference can retire only after the actual consuming recording is
  // discarded or all of its genuine submissions complete.
  submission::TrackUse(list, version->clone);
  version->claimed = true;
  ++observation.claim_count;
  observation.claim_reason = (version->direct ? "direct_claimed" : "clone_claimed");
  return true;
}

// Caller holds runtime_mutex; an in-flight Scope can no longer publish after
// its version is erased. Serial identities are never recycled by Reset.
inline void Reset() {
  ResetPremise();
  ++state.observation.reset_count;
  for (auto& version : state.versions) {
    NoteRemoval(version, "reset");
    RetirePresentResource(&version.clone);
  }
  state.versions.clear();
  state.viewports.clear();
  state.direct_capacity_blocked = false;
  state.direct_unresolved_reason = nullptr;
}

}  // namespace native_tag_source
#endif
