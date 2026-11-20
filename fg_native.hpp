/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

// Included inside dlss5::internal after present_path.hpp's resource helpers.
namespace fg_service {

struct Slot {
  present_frame_policy::Frame base_frame;
  present_frame_policy::GroupResult base_frame_group;
  AdaptiveDelivery adaptive_delivery;
  std::pair<uint64_t, uint64_t> adaptive_recording{};
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Resource* color[2] = {};
  ID3D12Resource* original_backbuffer = nullptr;
  ID3D12Resource* original_hudless = nullptr;
  ID3D12Resource* coupled_hudless = nullptr;
  ID3D12Resource* coupled_ui = nullptr;
  ID3D12DescriptorHeap* color_heap = nullptr;
  bool color_inputs_prepared = false;
  bool pipeline_pending = false;
#if defined(NR_NEXUS_DEV_CONTROL)
  bool native_edit_fixed_workset = false;
#endif
  uint64_t fence_value = 0;
  bool quarantined = false;
};

struct Stream {
  present_frame_policy::Clock base_frame_clock;
  std::shared_ptr<adaptive_resolution::Session> adaptive_session;
  adaptive_resolution::Frame adaptive_frame;
  AdaptiveDelivery pending_xefg_delivery;
  output_framerate::OutputIdentity xefg_delivery_output;
  bool awaiting_xefg_delivery = false;
  uint32_t adaptive_model_roles = 0;
  uint32_t adaptive_required_roles = 0;
  bool adaptive_role_unavailable = false;
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ID3D12Fence* fence = nullptr;
  uint64_t submitted = 0;
  uint64_t epoch = 0;
  uint64_t application_frame = 0;
  std::array<EditReuseMonitorFrame, 2> edit_reuse_monitor;
  uint64_t native_serial = 0;
  uint64_t native_display_epoch = 0;
  uint64_t native_configuration = 0;
  uint64_t native_options_generation = 0;
  bool native_resolution_pair = false;
#if defined(NR_NEXUS_DEV_CONTROL)
  bool native_edit_source = false;
  bool native_edit_history = false;
  uint64_t native_edit_frame = 0;
  uint64_t native_edit_source_generation = 0;
  uint64_t native_edit_source_epoch = 0;
  bool native_edit_source_is_tagged = false;
  ID3D12Resource* native_edit_ui_alpha = nullptr;
#endif
  uint64_t native_pair_attempt_frame = 0;
  std::pair<uint64_t, uint64_t> native_pair_recording{};
  const void* native_pair_producer_marker = nullptr;
  std::array<PassResolutionSettings, 2> native_pair_resolutions{};
  FenceRef native_pair_completion;
  uint64_t native_pair_completion_value = 0;
  bool native_pair_discarded = false;
  bool native_pair_replayed = false;
  const NVSDK_NGX_Handle* native_handle = nullptr;
  bool continuing_submission = false;
  uint64_t last_model_frame[2] = {};
  uint64_t last_output_frame[2] = {};
  int64_t diagnostic_logged_ns = 0;
  int64_t gpu_capture_entry_ns = 0;
  bool has_frame = false;
  fg::Result result = fg::Result::NOT_SERVED;
  PresentStream roles[2];
  // Native inputs live as long as the game's replayable list generations,
  // which need not fit the private XeFG queue's three-slot ring. Appending
  // must preserve cached MFG slot addresses.
  std::deque<Slot> slots{3};
  Slot* cached = nullptr;
  ID3D12Resource* source_backbuffer = nullptr;  // identities only
  ID3D12Resource* source_hudless = nullptr;
  ID3D12Resource* source_ui = nullptr;
  uint64_t source_frame_id = 0;
  uint64_t source_sdk_decision = 0;
  ID3D12PipelineState* color_pipeline = nullptr;
  bool native = false;
};

// All access is under runtime_mutex; nodes also key NR worksets/history.
static std::map<std::pair<bool, uintptr_t>, Stream> streams;
static std::map<const NVSDK_NGX_Handle*, uintptr_t> native_bindings;

inline void ResetNativeFrame(Stream* stream) {
  stream->base_frame_clock.complete = false;
  stream->cached = nullptr;
  stream->native_pair_producer_marker = nullptr;
  stream->native_pair_completion = {};
  stream->native_serial = 0;
  stream->continuing_submission = false;
  stream->result = fg::Result::PREPARED;
  for (auto& role : stream->roles) {
    ResetPresentStreamHistory(&role);
  }
}

inline bool Requested() noexcept {
  return enabled.load(std::memory_order_relaxed)
      && hooks_enabled.load(std::memory_order_acquire)
      && !shutting_down.load(std::memory_order_acquire)
      && !NrYieldsToForeign() && hook_point.load(std::memory_order_relaxed) == kHookPresent
      && present_pre_fg.load(std::memory_order_relaxed) != kPresentPreFgOff;
}

inline bool PrepareCapture(ID3D12GraphicsCommandList* list) noexcept {
  try {
    if (list == nullptr || !IsSupportedInjectionCommandList(list)) return false;
    EnsureForeignEntryInstalls(list);
    if (!queue_tracking_active.load(std::memory_order_acquire)) {
      ID3D12Device* device = nullptr;
      ID3D12CommandQueue* queue = nullptr;
      if (SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&device)))) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = list->GetType();
        if (SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)))) {
          ID3D12CommandQueue* native_queue = queue;
          renodx::utils::directx::NativeFromReShadeProxy(&native_queue);
          InstallQueueCompletionHooks(native_queue);
        }
      }
      ReleaseCom(queue);
      ReleaseCom(device);
    }
    return queue_tracking_active.load(std::memory_order_acquire)
        && real_queue_execute_command_lists != nullptr && InstallCommandListStateHooks(list);
  } catch (...) {
    return false;
  }
}

// An unsubmitted recording can have created handles and advanced NGX CPU
// history. Rebuild the feature and workset, including their GPU state ledger.
inline void Invalidate(Stream* stream) {
  stream->base_frame_clock.complete = false;
  stream->adaptive_role_unavailable = false;
#if defined(NR_NEXUS_DEV_CONTROL)
  stream->native_edit_history = false;
#endif
  stream->cached = nullptr;
  stream->native_pair_producer_marker = nullptr;
  stream->native_pair_completion = {};
  stream->native_serial = 0;
  stream->continuing_submission = false;
  stream->last_model_frame[0] = stream->last_model_frame[1] = 0;
  stream->last_output_frame[0] = stream->last_output_frame[1] = 0;
  stream->result = fg::Result::NOT_SERVED;
  for (auto& role : stream->roles) {
    if (const auto it = features.find(role.Handle()); it != features.end()) {
      ReleaseAdaptiveCache(&it->second, true);
      ReleaseAllNrSlots(it->second, true);
      features.erase(it);
    }
    RetireWorkset({role.Handle(), reinterpret_cast<const ID3D12Resource*>(&role)});
  }
}

inline void Release(Stream* stream) {
  for (auto& role : stream->roles) {
    owner_resident_nr_handles.erase(role.Handle());
  }
  for (auto& slot : stream->slots) {
    if (slot.list != nullptr && !slot.quarantined) {
      // This closes replay eligibility only. Exact submission proofs still
      // retain the objects until the GPU finishes their last submission.
      submission::OnCommandListReset(slot.list);
    }
    for (auto& color : slot.color) RetirePresentResource(&color);
    RetirePresentResource(&slot.original_backbuffer);
    RetirePresentResource(&slot.original_hudless);
    RetirePresentResource(&slot.coupled_hudless);
    RetirePresentResource(&slot.coupled_ui);
    RetirePresentResource(&slot.color_heap);
    // Lists and allocators are referenced by the same exact submission as
    // their resources, rather than released when the callback returns.
    if (slot.list != nullptr) {
      RetirePresentResource(&slot.list);
    }
    if (slot.allocator != nullptr) {
      RetirePresentResource(&slot.allocator);
    }
  }
  Invalidate(stream);
  RetirePresentResource(&stream->color_pipeline);
  for (auto& role : stream->roles) {
    RetirePresentResource(&role.neutral_motion);
    RetirePresentResource(&role.neutral_depth);
  }
  ReleaseCom(stream->fence);
  ReleaseCom(stream->queue);
  ReleaseCom(stream->device);
}

// Caller owns the NGX PresentTurn and runtime_mutex. These private XeFG lists
// cannot be replayed after their pool is removed; outstanding GPU uses still
// retain their retired resources. Native game recordings are never closed here.
inline void RetireServiceStreamsLocked() {
  for (auto it = streams.begin(); it != streams.end();) {
    if (it->first.first) {
      ++it;
      continue;
    }
    Release(&it->second);
    it = streams.erase(it);
  }
}

inline void Retire() noexcept {
  try {
    const ngx_serial::PresentTurn turn(true);
    if (!turn.owns()) return;
    RuntimeLock lock(runtime_mutex);
    RetireServiceStreamsLocked();
  } catch (...) {
  }
}

inline void CopyColor(ID3D12GraphicsCommandList* list, ID3D12Resource* destination,
                      D3D12_RESOURCE_STATES destination_state, ID3D12Resource* source,
                      D3D12_RESOURCE_STATES source_state, uint32_t source_x = 0,
                      uint32_t source_y = 0, uint32_t destination_x = 0, uint32_t destination_y = 0,
                      uint32_t width = 0, uint32_t height = 0) {
  submission::TrackUse(list, source);
  submission::TrackUse(list, destination);
  Transition(list, source, source_state, D3D12_RESOURCE_STATE_COPY_SOURCE, 0u);
  Transition(list, destination, destination_state, D3D12_RESOURCE_STATE_COPY_DEST, 0u);
  D3D12_TEXTURE_COPY_LOCATION from{};
  from.pResource = source;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION to{};
  to.pResource = destination;
  to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  if (width == 0) width = static_cast<uint32_t>(destination->GetDesc().Width);
  if (height == 0) height = destination->GetDesc().Height;
  const D3D12_BOX box{source_x, source_y, 0, source_x + width, source_y + height, 1};
  list->CopyTextureRegion(&to, destination_x, destination_y, 0, &from, &box);
  Transition(list, destination, D3D12_RESOURCE_STATE_COPY_DEST, destination_state, 0u);
  Transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, source_state, 0u);
}

inline bool NeedsNativeColorCoupling(const fg::FrameInput& input,
                                     const NVSDK_NGX_Parameter* parameters, bool ui_recomposition) {
  return parameters != nullptr && !ui_recomposition && input.hudless != nullptr
      && input.hudless != input.backbuffer && input.ui != nullptr
      && PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_UIAlpha) == nullptr
      && PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_UI) == input.ui;
}

inline bool PrepareNativeColorInputs(Stream* stream, Slot* slot, const fg::FrameInput& input) {
  if (input.color_x != 0 || input.color_y != 0) return false;
  for (auto* source : {input.backbuffer, input.hudless, input.ui}) {
    if (source == nullptr) return false;
    const auto desc = source->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
        || desc.Width != input.color_width || desc.Height != input.color_height
        || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1
        || (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) return false;
  }
  // These FG colors are normalized code values, including HDR10/PQ. Float
  // scene-linear inputs require a different range contract, not a clamp.
  for (auto* source : {input.backbuffer, input.hudless}) {
    switch (CodecViewFormat(ConcreteResourceFormat(source->GetDesc().Format))) {
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_R10G10B10A2_UNORM:
      case DXGI_FORMAT_R16G16B16A16_UNORM: break;
      default: return false;
    }
  }
  switch (CodecViewFormat(ConcreteResourceFormat(input.ui->GetDesc().Format))) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UNORM: break;
    default: return false;
  }
  constexpr auto kFormat = DXGI_FORMAT_R16G16B16A16_UNORM;
  if (!SupportsCodecFormat(stream->device, kFormat)
      || !EnsureCodecRootSignature(stream->device)
      || !CreateCodecPipelineState(stream->device, __native_fg_color, &stream->color_pipeline,
                                  L"DLSS5 native FG color inputs", "native FG color inputs")) return false;
  for (auto** resource : {&slot->coupled_hudless, &slot->coupled_ui}) {
    if (*resource != nullptr && ((*resource)->GetDesc().Width != input.color_width
        || (*resource)->GetDesc().Height != input.color_height)) {
      RetirePresentResource(resource);
    }
    if (*resource == nullptr && !CreateScratchTexture(stream->device, input.color_width, input.color_height,
            kFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, resource, L"DLSS5 coupled FG color")) return false;
  }
  if (slot->color_heap == nullptr) {
    const D3D12_DESCRIPTOR_HEAP_DESC desc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = 9, .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    if (FAILED(stream->device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&slot->color_heap)))) return false;
    const auto cpu = slot->color_heap->GetCPUDescriptorHandleForHeapStart();
    const auto stride = stream->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC null_srv{
        .Format = DXGI_FORMAT_R32_FLOAT,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, .Texture2D = {.MipLevels = 1}};
    stream->device->CreateShaderResourceView(nullptr, &null_srv, DescriptorHeapSlot(cpu, 6, stride));
    null_srv.Format = DXGI_FORMAT_R16G16_FLOAT;
    stream->device->CreateShaderResourceView(nullptr, &null_srv, DescriptorHeapSlot(cpu, 7, stride));
    stream->device->CreateConstantBufferView(nullptr, DescriptorHeapSlot(cpu, 8, stride));
  }
  return true;
}

inline void RecordNativeColorInputs(Stream* stream, Slot* slot, ID3D12GraphicsCommandList* list,
                                    const fg::FrameInput& input) {
  constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  const auto cpu = slot->color_heap->GetCPUDescriptorHandleForHeapStart();
  const auto stride = stream->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  uint32_t index = 0;
  for (auto* resource : {slot->color[0], input.backbuffer, input.hudless, input.ui}) {
    const D3D12_SHADER_RESOURCE_VIEW_DESC desc{
        .Format = CodecViewFormat(ConcreteResourceFormat(resource->GetDesc().Format)),
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, .Texture2D = {.MipLevels = 1}};
    stream->device->CreateShaderResourceView(resource, &desc, DescriptorHeapSlot(cpu, index++, stride));
    submission::TrackUse(list, resource);
  }
  for (auto* resource : {slot->coupled_hudless, slot->coupled_ui}) {
    const D3D12_UNORDERED_ACCESS_VIEW_DESC desc{
        .Format = DXGI_FORMAT_R16G16B16A16_UNORM, .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D};
    stream->device->CreateUnorderedAccessView(resource, nullptr, &desc, DescriptorHeapSlot(cpu, index++, stride));
    submission::TrackUse(list, resource);
    Transition(list, resource, kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  submission::TrackUse(list, slot->color_heap);
  submission::TrackUse(list, stream->color_pipeline);
  submission::TrackUse(list, codec_pipeline.root_signature);
  list->SetDescriptorHeaps(1, &slot->color_heap);
  list->SetComputeRootSignature(codec_pipeline.root_signature);
  list->SetPipelineState(stream->color_pipeline);
  const auto gpu = slot->color_heap->GetGPUDescriptorHandleForHeapStart();
  list->SetComputeRootDescriptorTable(0, gpu);
  list->SetComputeRootDescriptorTable(1, {gpu.ptr + static_cast<uint64_t>(4) * stride});
  // The shared codec root signature also retains guide/proof tables across
  // heap switches, even though this color shader does not read them.
  list->SetComputeRootDescriptorTable(3, {gpu.ptr + static_cast<uint64_t>(6) * stride});
  list->SetComputeRootDescriptorTable(4, {gpu.ptr + static_cast<uint64_t>(7) * stride});
  list->SetComputeRootDescriptorTable(5, {gpu.ptr + static_cast<uint64_t>(8) * stride});
  const uint32_t size[4]{input.color_width, input.color_height};
  list->SetComputeRoot32BitConstants(2, 4, size, 0);
  list->Dispatch((input.color_width + 15) / 16, (input.color_height + 15) / 16, 1);
  for (auto* resource : {slot->coupled_hudless, slot->coupled_ui}) {
    Transition(list, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
  }
  slot->color_inputs_prepared = true;
}

inline bool PrepareSurfaces(Stream* stream, Slot* slot, const fg::FrameInput& input,
                             bool coupled_ui = false, bool need_original_hudless = true) {
  const uint32_t width = input.color_width;
  const uint32_t height = input.color_height;
  if (input.hudless != nullptr && input.hudless != input.backbuffer) {
    const auto hudless = input.hudless->GetDesc();
    const auto back = input.backbuffer->GetDesc();
    if (input.color_x != 0 || input.color_y != 0 || back.Width != width || back.Height != height
        || hudless.Width != width || hudless.Height != height) return false;
  }
  ID3D12Resource** outputs[]{&slot->color[0], &slot->color[1],
                            &slot->original_hudless, &slot->original_backbuffer};
  for (uint32_t i = 0; i < 4; ++i) {
    if (i == 3 && !stream->native) continue;
    if (i == 2 && !need_original_hudless) continue;
    if (coupled_ui && (i == 1 || i == 2)) continue;
    auto* source = (i == 0 || i == 3 ? input.backbuffer : input.hudless);
    if (source == nullptr || ((i == 1 || i == 2) && source == input.backbuffer)) continue;
    auto** output = outputs[i];
    const auto desc = source->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1
        || desc.SampleDesc.Count != 1 || input.color_x > desc.Width || input.color_y > desc.Height
        || width > desc.Width - input.color_x || height > desc.Height - input.color_y) return false;
    if (*output != nullptr) {
      const auto have = (*output)->GetDesc();
      if (have.Width != width || have.Height != height || have.Format != desc.Format) {
        RetirePresentResource(output);
      }
    }
    if (*output == nullptr
        && !CreateScratchTexture(stream->device, width, height, desc.Format,
                                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                output, L"DLSS5 FG private color")) return false;
  }
  return true;
}

inline fg::Result Record(Stream* stream, Slot* slot, ID3D12GraphicsCommandList* list,
                         const fg::FrameInput& input, const PresentTarget& base,
                         bool* output_ready, std::array<NrRecording, 2>* recordings,
                         const NVSDK_NGX_Parameter* native_parameters = nullptr,
                         bool ui_recomposition = true) {
  *output_ready = false;
  *recordings = {};
  slot->adaptive_delivery = {};
  slot->adaptive_recording = {};
  slot->color_inputs_prepared = false;
  slot->pipeline_pending = false;
#if defined(NR_NEXUS_DEV_CONTROL)
  slot->native_edit_fixed_workset = false;
#endif
  const PipelinePreparationScope pipeline_scope;
  ResolveAdaptiveBorrowedDeliveries();
  if (!enabled.load(std::memory_order_relaxed)) return fg::Result::NOT_SERVED;
  if (nr_adaptive_enabled.load(std::memory_order_relaxed) || codec_pipeline.incremental_preparation) {
    // Runtime/device adoption can release the old global codec. It must run
    // before this CPU-only gate, while no FeatureState references are held.
    auto decline = NrDeclineReason::kNrRuntimeUnavailable;
    if (!EnsureDirectRuntime(list, &decline, base.private_signed_runtime)) {
      CountNrDecline(decline);
      return fg::Result::NOT_SERVED;
    }
  }
  if (!nr_adaptive_enabled.load()) {
    stream->adaptive_role_unavailable = false;
  }
  constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  const bool has_hudless = input.hudless != nullptr && input.hudless != input.backbuffer;
  const bool coupled_ui = NeedsNativeColorCoupling(input, native_parameters, ui_recomposition);
  const bool may_compose = ui_recomposition && has_hudless && native_parameters != nullptr && input.ui != nullptr
      && input.color_x == 0 && input.color_y == 0;
  const uint32_t guide_mode = diagnostics::GuideMode(
      base.diagnostic.guides, present_guides.load(std::memory_order_relaxed));
  const bool real_guides = input.depth != nullptr && input.motion != nullptr
      && input.guide_width != 0 && input.guide_height != 0 && guide_mode != kPresentGuidesNever;
  if (!real_guides && guide_mode == kPresentGuidesRequired) return fg::Result::NOT_SERVED;
  auto base_frame = base.base_frame;
  if (!stream->native && input.has_present_id && input.has_constants && input.application_frame != 0) {
    base_frame = {.kind = present_frame_policy::SourceKind::APPLICATION,
        .source = input.context, .generation = input.epoch, .sequence = input.application_frame,
        .pass_count = EvaluationStagePlan(stream->roles[0].Handle(), false).pass_count,
        .roles = (has_hudless && !coupled_ui ? 3u : 1u)};
  }
  const auto previous_base = stream->base_frame_clock.frame;
  const auto base_claim = present_frame_policy::Observe(&stream->base_frame_clock, base_frame);
  if (base_claim == present_frame_policy::Claim::REPEATED || base_claim == present_frame_policy::Claim::STALE) {
    return fg::Result::NOT_SERVED;
  }
  const uint64_t base_sequence = (present_frame_policy::Known(base_frame) ? base_frame.sequence : input.application_frame);
  FeatureState contract;
  contract.input_width = (real_guides ? input.guide_width : input.color_width);
  contract.input_height = (real_guides ? input.guide_height : input.color_height);
  contract.motion_x = (real_guides ? input.motion_x : 0u);
  contract.motion_y = (real_guides ? input.motion_y : 0u);
  contract.depth_x = (real_guides ? input.depth_x : 0u);
  contract.depth_y = (real_guides ? input.depth_y : 0u);
  contract.has_motion_scale_x = true;
  contract.has_motion_scale_y = true;
  contract.motion_scale_x = (real_guides ? input.motion_scale_x : static_cast<float>(input.color_width));
  contract.motion_scale_y = (real_guides ? input.motion_scale_y : static_cast<float>(input.color_height));
  contract.create_flags = (real_guides ? input.init_flags : NVSDK_NGX_DLSS_Feature_Flags_DepthInverted);
  contract.frame_reset = (input.reset ? 1 : 0);
  const uint64_t previous_adaptive_layout = stream->adaptive_frame.layout_identity;
  uint32_t required_model_roles = (may_compose && stream->adaptive_model_roles == 2
      ? 2u : (has_hudless && !coupled_ui ? 3u : 1u));
  const bool missing_prepared_role = nr_adaptive_enabled.load() && (stream->adaptive_required_roles & 2u) != 0 && !has_hudless;
  const auto adaptive_plan = EvaluationStagePlan(stream->roles[0].Handle(), false);
  const auto freeze_adaptive = [&](uint32_t roles) {
    return FreezeAdaptiveFrame(&stream->adaptive_session, adaptive_plan,
      {.stage = stage_plan::Stage::PRESENT, .count = adaptive_plan.pass_count},
      input.color_width, input.color_height, contract.input_width, contract.input_height,
      reinterpret_cast<uintptr_t>(stream) ^ reinterpret_cast<uintptr_t>(stream->device)
          ^ reinterpret_cast<uintptr_t>(stream->queue) ^ input.epoch ^ static_cast<uint64_t>(base.hdr_mode)
           ^ (static_cast<uint64_t>(input.backbuffer->GetDesc().Format) * 1099511628211ull) ^ contract.create_flags
          ^ (static_cast<uint64_t>(coupled_ui) << 40) ^ (static_cast<uint64_t>(may_compose) << 41),
      !base.d3d11_bridge && !base.native_prefg_resolution_pair,
      input.application_frame, roles, true, 1, stream->adaptive_model_roles, false, nullptr, nullptr,
      input.output, input.epoch);
  };
  stream->adaptive_frame = freeze_adaptive(missing_prepared_role ? stream->adaptive_required_roles : required_model_roles);
  if (missing_prepared_role && previous_adaptive_layout == stream->adaptive_frame.layout_identity
      && stream->adaptive_session != nullptr) {
    auto& session = *stream->adaptive_session;
    if (!stream->adaptive_role_unavailable) {
      if (session.trial) {
        session.Request(session.previous_level, SteadyNowNs());
      }
      session.trial = false;
      session.ResetMeasurements();
      session.UpdatePreparation(stream->adaptive_frame.preparation_epoch, 0,
          static_cast<uint32_t>(session.ladder.count), adaptive_resolution::Reason::PREPARING, SteadyNowNs());
      stream->adaptive_role_unavailable = true;
      LogPresent(reshade::log::level::info,
          "NR adaptive paused: required HUDLess input is unavailable; retaining compatible selected resources");
    }
    if (!session.failed && session.preparation_reason != adaptive_resolution::Reason::BUDGET) {
      session.status.reason = adaptive_resolution::Reason::UNSUPPORTED;
    }
    PublishAdaptiveStatus(session.status);
    // Absence pauses only the same resource/composition contract. Real layout
    // changes below retain their normal invalidation and preparation path.
    // PREPARED preserves the Service recording contract, not color readiness.
    return fg::Result::PREPARED;
  }
  if (missing_prepared_role) stream->adaptive_frame = freeze_adaptive(required_model_roles);
  if (stream->adaptive_role_unavailable) {
    stream->adaptive_role_unavailable = false;
    if (previous_adaptive_layout == stream->adaptive_frame.layout_identity && stream->adaptive_frame.enabled) {
      for (uint32_t i = 0; i < 2; ++i) {
        if ((required_model_roles & (1u << i)) != 0) {
          ResetPresentStreamHistory(&stream->roles[i]);
        }
      }
      if (stream->adaptive_session->settings.legacy_preparation) {
        stream->adaptive_session->ResetMeasurements();
        stream->adaptive_session->stable_after_ns = SteadyNowNs() + adaptive_resolution::WARMUP_NS;
      }
    }
  }
  if (!nr_adaptive_enabled.load() && stream->adaptive_session != nullptr) {
    for (auto& role : stream->roles) {
      if (const auto feature = features.find(role.Handle()); feature != features.end()
          && feature->second.adaptive_cache != nullptr) {
        auto inactive = stream->adaptive_frame;
        SelectAdaptiveTier(&feature->second, role.Handle(), &inactive, true,
            reinterpret_cast<const ID3D12Resource*>(&role));
      }
    }
  }
  if (previous_adaptive_layout != stream->adaptive_frame.layout_identity) {
    stream->adaptive_model_roles = 0;
    stream->adaptive_required_roles = 0;
    for (auto& role : stream->roles) {
      if (const auto feature = features.find(role.Handle()); feature != features.end()
          && feature->second.adaptive_cache != nullptr
          && feature->second.adaptive_cache->identity != stream->adaptive_frame.layout_identity) {
        ReleaseAdaptiveCache(&feature->second, true);
      }
    }
  }
  if (stream->adaptive_session != nullptr && stream->adaptive_frame.enabled) {
    bool selection_pending = false;
    for (uint32_t i = 0; i < 2; ++i) {
      const auto feature = features.find(stream->roles[i].Handle());
      if (feature == features.end()) continue;
      if (feature->second.adaptive_cache == nullptr) {
        selection_pending |= (required_model_roles & (1u << i)) != 0;
        continue;
      }
      if (!feature->second.adaptive_cache->resident) continue;
      auto& cache = *feature->second.adaptive_cache;
      if ((required_model_roles & (1u << i)) == 0) {
        if (stream->adaptive_session->settings.selected_levels == 0) {
          // Default uses the original ordinary pool. Preserve the inactive
          // upper model, but retire its optional collection and scratch so
          // an old explicit selection cannot permanently consume that pool.
          ActivateAdaptiveTier(&feature->second, 0);
          ReleaseAdaptiveStandby(&cache, true);
          cache.resident = false;
          feature->second.adaptive_cache.reset();
          for (auto it = worksets.begin(); it != worksets.end();) {
            if (it->first.handle != stream->roles[i].Handle()) {
              ++it;
              continue;
            }
            RetireWorksetResources(std::move(it->second));
            it = worksets.erase(it);
          }
          for (auto& model : feature->second.slots) {
            model.pending_reset = true;
            model.pending_reset_reasons |= kResetDimensions;
          }
        }
        continue;
      }
      selection_pending |= cache.count != stream->adaptive_session->ladder.count
          || cache.ladder.sizes != stream->adaptive_session->ladder.sizes;
    }
    if (selection_pending) {
      // A role's normal ProcessInline advances preparation before Record
      // reaches the next role. Adopt existing manual roles and reconcile the
      // shared selection first, so their old scratch is not counted outside
      // the group and an intermediate count cannot latch a false budget failure.
      for (uint32_t i = 0; i < 2; ++i) {
        if ((required_model_roles & (1u << i)) == 0) continue;
        const auto feature = features.find(stream->roles[i].Handle());
        if (feature == features.end() || (feature->second.adaptive_cache != nullptr
            && !feature->second.adaptive_cache->resident)) continue;
        auto selected = stream->adaptive_frame;
        selected.role = 1u << i;
        selected.slot = 0;
        SelectAdaptiveTier(&feature->second, stream->roles[i].Handle(), &selected, true,
            reinterpret_cast<const ID3D12Resource*>(&stream->roles[i]));
      }
    }
  }
  const bool mask_pipeline = control_mask_enabled.load(std::memory_order_relaxed)
      && nr_model::SupportsControlMask(direct_runtime_requirements.file_version)
      && !base.native_prefg_resolution_pair;
  const auto pipeline_wait = [&](PipelinePreparationResult result) {
    CountNrDecline(result == PipelinePreparationResult::PENDING
        ? NrDeclineReason::kSlotWarmupMaturation : NrDeclineReason::kFailureBackoff);
    slot->pipeline_pending = true;
    return fg::Result::PREPARED;
  };
  const auto pipeline_preparation = PrepareAdaptivePipelines(stream->device, stream->adaptive_frame, base.hdr_mode, mask_pipeline);
  if (pipeline_preparation != PipelinePreparationResult::READY) {
    return pipeline_wait(pipeline_preparation);
  }
  if (coupled_ui && stream->color_pipeline == nullptr && stream->adaptive_session != nullptr
      && stream->adaptive_session->failed) {
    // Generic device pipelines may already be healthy, but that does not
    // authorize another attempt at this failed stream's missing color PSO.
    return pipeline_wait(PipelinePreparationResult::FAILED);
  }
  if (coupled_ui && !PrepareNativeColorInputs(stream, slot, input)) {
    if (codec_pipeline.incremental_preparation
        && (codec_pipeline.preparation_failed || pipeline_preparation_opportunity->deferred)) {
      return pipeline_wait(PrepareAdaptivePipelines(stream->device, stream->adaptive_frame, base.hdr_mode, mask_pipeline));
    }
    return fg::Result::NOT_SERVED;
  }
  if (codec_pipeline.incremental_preparation && pipeline_preparation_opportunity->created) {
    // The coupled-color PSO shares this source's budget with codec/look.
    return pipeline_wait(PrepareAdaptivePipelines(stream->device, stream->adaptive_frame, base.hdr_mode, mask_pipeline));
  }
  if (stream->adaptive_session != nullptr && stream->adaptive_frame.enabled) {
    const size_t requested_slot = stream->adaptive_session->level;
    bool group_ready = true;
    bool missing_role = false;
    bool group_failed = false, group_budget = false;
    bool upper_ready = true;
    std::array<bool, adaptive_resolution::MAX_PREPARED_LEVELS> selected_ready;
    selected_ready.fill(true);
    size_t current_slot = adaptive_resolution::MAX_LEVELS;
    for (uint32_t i = 0; i < 2; ++i) {
      if ((required_model_roles & (1u << i)) == 0) continue;
      const auto feature = features.find(stream->roles[i].Handle());
      if (feature == features.end() || feature->second.adaptive_cache == nullptr
          || feature->second.adaptive_cache->identity != stream->adaptive_frame.layout_identity) {
        group_ready = false;
        missing_role = true;
        upper_ready = false;
        selected_ready.fill(false);
        continue;
      }
      auto& cache = *feature->second.adaptive_cache;
      PollAdaptiveCache(&cache);
      const size_t active = (AdaptiveTierUsable(cache, *stream->adaptive_session, cache.active) ? cache.active : 0u);
      if (current_slot == adaptive_resolution::MAX_LEVELS) {
        current_slot = active;
      } else if (current_slot != active) {
        current_slot = 0;
      }
      group_ready &= !cache.failed && !cache.budget_blocked
          && AdaptiveTierUsable(cache, *stream->adaptive_session, requested_slot);
      group_failed |= cache.failed;
      group_budget |= cache.budget_blocked;
      const auto& upper_state = (cache.active == 0 ? feature->second : cache.tiers[0].state);
      upper_ready &= cache.tiers[0].ready
          && std::none_of(std::begin(upper_state.slots), std::end(upper_state.slots),
              [](const auto& model) { return model.failed; });
      for (size_t tier = 0; tier < stream->adaptive_session->ladder.count; ++tier) {
        selected_ready[tier] &= tier < cache.count && cache.tiers[tier].ready
            && cache.ladder.sizes[tier] == stream->adaptive_session->ladder.sizes[tier];
      }
    }
    if (!stream->adaptive_session->settings.legacy_preparation) {
      uint32_t ready = 0;
      for (size_t tier = 0; tier < stream->adaptive_session->ladder.count; ++tier) ready += selected_ready[tier];
      const auto reason = (group_failed ? adaptive_resolution::Reason::FAILED
          : (group_budget ? adaptive_resolution::Reason::BUDGET : adaptive_resolution::Reason::PREPARING));
      if (stream->adaptive_session->settings.limited_residency) {
        stream->adaptive_session->UpdateTargetPreparation(stream->adaptive_frame.preparation_epoch,
            stream->adaptive_frame.generation, ready, static_cast<uint32_t>(stream->adaptive_session->ladder.count),
            group_ready, reason, SteadyNowNs());
      } else {
        stream->adaptive_session->UpdatePreparation(stream->adaptive_frame.preparation_epoch, ready,
            static_cast<uint32_t>(stream->adaptive_session->ladder.count), reason, SteadyNowNs());
      }
      group_ready &= stream->adaptive_session->TargetReady();
      PublishAdaptiveStatus(stream->adaptive_session->status);
    }
    const bool return_upper = upper_ready && (group_failed || group_budget
        || stream->adaptive_session->preparation_reason == adaptive_resolution::Reason::BUDGET);
    const size_t slot = (return_upper ? 0u : (group_ready ? requested_slot : (missing_role ? 0u : current_slot)));
    stream->adaptive_frame.slot = static_cast<uint32_t>(slot);
    stream->adaptive_frame.sizes = stream->adaptive_session->ladder.sizes[slot];
  }
  if (!real_guides) {
    auto& role = stream->roles[0];
    for (auto [surface, format] : {std::pair{&role.neutral_motion, DXGI_FORMAT_R16G16_FLOAT},
                                 std::pair{&role.neutral_depth, DXGI_FORMAT_R32_FLOAT}}) {
      if (*surface != nullptr && ((*surface)->GetDesc().Width != input.color_width
          || (*surface)->GetDesc().Height != input.color_height)) RetirePresentResource(surface);
      if (*surface == nullptr && !CreateScratchTexture(stream->device, input.color_width, input.color_height,
          format, D3D12_RESOURCE_FLAG_NONE, kRead, surface, L"DLSS5 FG neutral NR guides")) {
        return fg::Result::FAILED;
      }
    }
  }
  auto* motion = (real_guides ? input.motion : stream->roles[0].neutral_motion);
  auto* depth = (real_guides ? input.depth : stream->roles[0].neutral_depth);
  const auto motion_state = (real_guides ? input.motion_state : kRead);
  const auto depth_state = (real_guides ? input.depth_state : kRead);
  if (motion == depth || motion == input.backbuffer || depth == input.backbuffer
      || motion == input.hudless || depth == input.hudless) return fg::Result::NOT_SERVED;
  submission::TrackUse(list, motion);
  submission::TrackUse(list, depth);
  Transition(list, motion, motion_state, kRead, 0u);
  Transition(list, depth, depth_state, kRead, 0u);
  const auto restore_guides = [&] {
    Transition(list, depth, kRead, depth_state, 0u);
    Transition(list, motion, kRead, motion_state, 0u);
  };
  // XeFG full-frame inputs can feed the core's original copy directly while
  // output stays private. Composition needs the unmodified staged backbuffer.
  const auto backbuffer_desc = input.backbuffer->GetDesc();
  const bool xefg_edit_reuse_frame = edit_reuse_enabled.load(std::memory_order_relaxed)
      && !stream->native && native_parameters == nullptr && !has_hudless && !input.ui_composition
      && input.fg_enabled && input.has_present_id && input.has_constants && input.application_frame != 0
      && present_frame_policy::Known(base_frame)
      && real_guides && input.color_x == 0 && input.color_y == 0
      && backbuffer_desc.Width == input.color_width && backbuffer_desc.Height == input.color_height;
  bool native_edit_reuse_frame = false;
#if defined(NR_NEXUS_DEV_CONTROL)
  auto fixed_workset = stream->adaptive_frame;
  if (stream->native && native_parameters != nullptr && base.base_frame.reuse_edits
      && present_frame_policy::Known(base.base_frame)
      && real_guides && !coupled_ui) {
    if (const auto resident = adaptive_resident_session.lock();
        fixed_workset.enabled && resident != nullptr && resident != fixed_workset.session) {
      // SelectAdaptiveTier falls back to the manual upper for each role when
      // a different session owns residency. Keep its per-role selection intact.
      fixed_workset.enabled = false;
      fixed_workset.slot = 0;
    }
    native_edit_reuse_frame = !fixed_workset.enabled && !fixed_workset.preparing && fixed_workset.slot == 0;
  }
  if (native_edit_reuse_frame && !stream->native_edit_history) {
    for (auto& role : stream->roles) {
      if (features.contains(role.Handle())) ResetPresentStreamHistory(&role, kResetSourceChange);
    }
  }
  stream->native_edit_history = native_edit_reuse_frame;
#endif
  const bool edit_reuse_frame = xefg_edit_reuse_frame || native_edit_reuse_frame;
  base_frame.reuse_edits = edit_reuse_frame;
  if (base_claim == present_frame_policy::Claim::NEW_FRAME) {
    stream->base_frame_clock.frame.reuse_edits = edit_reuse_frame;
  }
  // Cache descriptors retain their sources. Stage the current original in
  // the existing private slot instead of retaining a DXGI backbuffer across ResizeBuffers.
  const bool borrow_color_inputs = native_parameters == nullptr
      && input.color_x == 0 && input.color_y == 0
      && backbuffer_desc.Width == input.color_width && backbuffer_desc.Height == input.color_height
      && !xefg_edit_reuse_frame;
  if (!borrow_color_inputs) {
    CopyColor(list, slot->color[0], kRead, input.backbuffer, input.backbuffer_state,
              input.color_x, input.color_y);
    if (has_hudless && !coupled_ui) {
      CopyColor(list, slot->color[1], kRead, input.hudless, input.hudless_state,
                input.color_x, input.color_y);
    }
  }
  if (has_hudless) {
    // Only UI composition reads the pre-NR snapshot; dual NR keeps its own inputs.
    if (may_compose) CopyColor(list, slot->original_hudless, kRead, slot->color[1], kRead);
  }
  gate_ever_opened.store(true, std::memory_order_relaxed);
  bool prepared = false;
  uint32_t composed_roles = 0;
  const uint32_t diagnostic_interval = telemetry_seconds.load(std::memory_order_relaxed);
  const int64_t diagnostic_now = (gpu_timers_enabled.load(std::memory_order_relaxed)
      && diagnostic_interval != 0 ? SteadyNowNs() : 0);
  const bool log_frame = diagnostic_now != 0
      && diagnostic_now - stream->diagnostic_logged_ns >= static_cast<int64_t>(diagnostic_interval) * 1'000'000'000;
  if (log_frame) {
    stream->diagnostic_logged_ns = diagnostic_now;
  }
  // Both colors represent the same application frame. A second observation
  // after HUDLess evaluated would advance the global epoch behind its history
  // and reset that role again on the following frame. Model-only preparation
  // below also carries a local Reset which must not become a game scene cut.
  if (!base.game_frame_observed) ObserveGameFrame(contract);
  for (const uint32_t i : {1u, 0u}) {
    if (i == 1 && (!has_hudless || coupled_ui)) continue;
    auto& role = stream->roles[i];
    // Public motion relates adjacent application frames, not the last frame
    // for which a busy service could obtain a recording slot or NGX turn.
    // A cache output is a completed base frame without a new model call.
    const uint64_t previous_frame = (edit_reuse_frame ? stream->last_output_frame[i] : stream->last_model_frame[i]);
    if (present_frame_policy::NeedsReset(previous_base, base_frame, previous_frame,
            input.reset || stream->epoch != input.epoch, role.real_guides != real_guides)
        || (!present_frame_policy::Known(base_frame) && input.application_frame != 0
            && previous_frame != 0 && input.application_frame != previous_frame + 1)) {
      ResetPresentStreamHistory(&role);
    }
    role.real_guides = real_guides;
    FillPresentBlock(&role, slot->color[i], motion, depth, contract,
                     input.color_width, input.color_height, 1.f, input.reset ? 1 : 0);
    role.parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, input.jitter_x);
    role.parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, input.jitter_y);
    PresentTarget target = base;
    target.base_frame = base_frame;
    target.adaptive_output = input.output;
    target.native_fg_inputs = native_parameters != nullptr;
    auto adaptive_role = stream->adaptive_frame;
    adaptive_role.role = (i == 0 ? 1u : 2u);
    target.adaptive_frame = &adaptive_role;
    target.game_frame_observed = true;
    target.output_state = kRead;
    target.source_color = (borrow_color_inputs ? (i == 0 ? input.backbuffer : input.hudless) : nullptr);
    target.source_color_state = (i == 0 ? input.backbuffer_state : input.hudless_state);
    target.workset_key = reinterpret_cast<const ID3D12Resource*>(&role);
    target.capture_display = false;
    target.ui_correction = i == 0 && base.ui_correction;
    target.guide_serial = (real_guides ? input.application_frame + 1 : 0u);
    target.domain = (i == 0 ? present_restore_scope::Domain::PRE_FG_BACKBUFFER
                            : present_restore_scope::Domain::PRE_FG_HUDLESS);
    target.original_identity = reinterpret_cast<uintptr_t>(i == 0 ? input.backbuffer : input.hudless);
    auto& recording = (*recordings)[i];
    bool touched = false;
    const EvaluateInFlightScope attempt(true);
    const uint64_t previous_capture_serial = screenshot::PendingModelCaptureSerial(list);
    const bool accepted = ProcessInline(list, role.Handle(), &role.parameters, &touched, &target, &recording);
    if (recording.adaptive_delivery.frame.delivery != nullptr) {
      stream->adaptive_frame.delivery = recording.adaptive_delivery.frame.delivery;
    }
    if (log_frame) {
      std::ostringstream line;
      line << "NR FG frame: route=" << (stream->native ? "native" : "xefg")
           << " role=" << (i == 1 ? "hudless" : "backbuffer")
           << " context=" << input.context << " epoch=" << input.epoch
           << " application_frame=" << input.application_frame
           << " present_id=" << input.present_id << " tagged_id=" << input.has_present_id
           << " previous_model_frame=" << stream->last_model_frame[i]
           << " previous_output_frame=" << stream->last_output_frame[i]
           << " guides=" << (real_guides ? "real" : "neutral")
           << " guide_frame_basis=" << (stream->native ? "native_fg_input" : "present_id")
           << " guide_serial=" << target.guide_serial << " constants=" << input.has_constants
           << " guide_size=" << contract.input_width << 'x' << contract.input_height
           << " color_size=" << input.color_width << 'x' << input.color_height
           << " color_format=" << backbuffer_desc.Format
           << " motion_format=" << motion->GetDesc().Format
           << " motion_size=" << motion->GetDesc().Width << 'x' << motion->GetDesc().Height
           << " motion_flags=" << motion->GetDesc().Flags
           << " depth_format=" << depth->GetDesc().Format
           << " depth_size=" << depth->GetDesc().Width << 'x' << depth->GetDesc().Height
           << " depth_flags=" << depth->GetDesc().Flags
           << " edit_requested=" << edit_reuse_enabled.load(std::memory_order_relaxed)
           << " edit_frame=" << (xefg_edit_reuse_frame ? base_sequence : 0u)
#if defined(NR_NEXUS_DEV_CONTROL)
           << " native_edit_frame=" << (native_edit_reuse_frame ? base_sequence : 0u)
#endif
           << " edit_refresh=" << edit_reuse_refreshes.load(std::memory_order_relaxed)
           << " edit_reuse=" << edit_reuse_reuses.load(std::memory_order_relaxed)
           << " edit_fallback=" << edit_reuse_fallbacks.load(std::memory_order_relaxed)
           << " motion_base=" << contract.motion_x << ',' << contract.motion_y
           << " depth_base=" << contract.depth_x << ',' << contract.depth_y
           << " motion_scale=" << contract.motion_scale_x << ',' << contract.motion_scale_y
           << " ngx_flags=" << contract.create_flags
           << " jitter=" << input.jitter_x << ',' << input.jitter_y << " input_reset=" << input.reset
           << " evaluate_mask=" << recording.model_evaluate_mask
           << " reset_mask=" << recording.model_reset_mask << " pass_mask=" << recording.model_pass_mask
           << " accepted=" << accepted << " output_ready=" << recording.output_ready
           << " reset_reasons=";
      for (uint32_t pass = 0; pass < kMaxNrPasses; ++pass) {
        if ((recording.model_evaluate_mask & (1u << pass)) == 0) continue;
        line << pass + 1 << ':' << HistoryResetReasonsText(recording.model_reset_reasons[pass]) << ';';
      }
      line << " counter_basis=recorded_model_calls_not_GPU_completion";
      Log(reshade::log::level::info, line.str());
    }
    prepared |= recording.prepared;
    const bool complete_pair = !base.native_prefg_resolution_pair
        || (NativePreFgResolutionPairAllowed(target)
            && recording.group_complete && recording.model_pass_mask == 3u);
    if (!accepted || !recording.output_ready || !complete_pair) {
      // Admission and History-order guards can defer a frame before any model
      // call. Actual Evaluate failures latch their own adaptive session.
      restore_guides();
      return (accepted && prepared ? fg::Result::PREPARED : fg::Result::FAILED);
    }
    {
      const auto capture_serial = screenshot::PendingModelCaptureSerial(list);
      if (capture_serial != 0 && capture_serial != previous_capture_serial) {
        const auto capture_guide = [&](const char* name, ID3D12Resource* resource) {
          const auto desc = resource->GetDesc();
          if (!screenshot::PrepareDiagnosticPlane(stream->device, name, resource,
                  static_cast<uint32_t>(desc.Width), desc.Height, std::nullopt, true, capture_serial)) return;
          Transition(list, resource, kRead, D3D12_RESOURCE_STATE_COPY_SOURCE, 0u);
          screenshot::RecordDiagnosticPlaneCopy(list, name, resource, capture_serial);
          Transition(list, resource, D3D12_RESOURCE_STATE_COPY_SOURCE, kRead, 0u);
        };
        capture_guide("guide_depth", depth);
        capture_guide("guide_motion", motion);
        uint32_t no_game = 0;
        const bool declared = GetUIntIfPresent(native_parameters,
            NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, &no_game);
        std::scoped_lock lock(screenshot::internal::state_mutex);
        if (screenshot::internal::pending && screenshot::internal::pending->serial == capture_serial) {
          screenshot::internal::pending->meta += "guide_source=native_fg\nguide_real="
              + std::to_string(real_guides) + "\nno_game_declared=" + std::to_string(declared)
              + "\nno_game_value=" + std::to_string(no_game) + "\n";
        }
      }
    }
    // Exact public UI alpha permits one-model composition. Without alpha
    // each consumed color has its own NR history and result.
    if (i == 1 && may_compose) {
      const uint64_t pending_capture_serial = screenshot::PendingModelCaptureSerial(list);
      const uint64_t capture_serial = (pending_capture_serial != previous_capture_serial ? pending_capture_serial : 0);
      bool capture_was_complete = false;
      if (capture_serial != 0) {
        std::scoped_lock lock(screenshot::internal::state_mutex);
        if (screenshot::internal::pending && screenshot::internal::pending->serial == capture_serial) {
          capture_was_complete = screenshot::internal::pending->required_planes_complete;
          // A present may hand off this capture before all five copies are recorded.
          screenshot::internal::pending->required_planes_complete = false;
        }
      }
      const auto capture_plane = [&](const char* name, ID3D12Resource* source, D3D12_RESOURCE_STATES state) {
        if (capture_serial == 0 || source == nullptr) return;
        const auto desc = source->GetDesc();
        if (!screenshot::PrepareDiagnosticPlane(stream->device, name, source,
                static_cast<uint32_t>(desc.Width), desc.Height, std::nullopt, true, capture_serial)) return;
        submission::TrackUse(list, source);
        Transition(list, source, state, D3D12_RESOURCE_STATE_COPY_SOURCE, 0u);
        screenshot::RecordDiagnosticPlaneCopy(list, name, source, capture_serial);
        Transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, state, 0u);
      };
      const char* ui_plane = (input.ui == PreFgResource(native_parameters, NVSDK_NGX_DLSSG_Parameter_UIAlpha)
                                  ? "native_ui_alpha" : "native_ui_color");
      capture_plane(ui_plane, input.ui, input.ui_state);
      capture_plane("native_original_hudless", slot->original_hudless, kRead);
      capture_plane("native_enhanced_hdrhl", slot->color[1], kRead);
      capture_plane("native_pre_compose", slot->color[0], kRead);
      Transition(list, input.ui, input.ui_state, kRead, 0u);
      submission::TrackUse(list, input.ui);
      const uint32_t composed = ComposePresentPreFg(list, native_parameters, slot->color[0],
          slot->original_hudless, slot->color[0]->GetDesc(), kRead, slot->color[1]);
      Transition(list, input.ui, kRead, input.ui_state, 0u);
      capture_plane("native_post_compose", slot->color[0], kRead);
      if (capture_serial != 0) {
        std::scoped_lock lock(screenshot::internal::state_mutex);
        if (screenshot::internal::pending && screenshot::internal::pending->serial == capture_serial) {
          size_t recorded_planes = 0;
          for (const auto& plane : screenshot::internal::pending->planes) {
            if ((plane.name == ui_plane || plane.name == "native_original_hudless"
                 || plane.name == "native_enhanced_hdrhl" || plane.name == "native_pre_compose"
                 || plane.name == "native_post_compose") && plane.recorded && plane.error.empty()) {
              ++recorded_planes;
            }
          }
          screenshot::internal::pending->required_planes_complete = capture_was_complete && recorded_planes == 5;
        }
      }
      if (composed == 0) {
        composed_roles |= 1u;
        break;
      }
      if (native_edit_reuse_frame) {
        // The active edit history belongs to HUDLess. Do not publish it next
        // to an independently refreshed BB when this frame's composition fails.
        restore_guides();
        return fg::Result::FAILED;
      }
      stream->adaptive_model_roles = 3;
      if (stream->adaptive_frame.enabled && stream->adaptive_frame.slot != 0) {
        const auto backbuffer = features.find(stream->roles[0].Handle());
        const auto cache = (backbuffer != features.end() ? backbuffer->second.adaptive_cache : nullptr);
        if (cache == nullptr || cache->identity != stream->adaptive_frame.layout_identity
            || cache->failed || cache->budget_blocked
            || !AdaptiveTierUsable(*cache, *stream->adaptive_session, stream->adaptive_frame.slot)
            || !cache->tiers[stream->adaptive_frame.slot].ready) {
          // A new fallback role cannot publish half a lower-resolution frame.
          // Its upper tier matures on the next application frame as a complete pair.
          restore_guides();
          return fg::Result::PREPARED;
        }
      }
    }
  }
  if (coupled_ui) {
    RecordNativeColorInputs(stream, slot, list, input);
    auto& role = stream->roles[1];
    const auto primary = features.find(stream->roles[0].Handle());
    const auto auxiliary = features.find(role.Handle());
    bool models_ready = primary != features.end() && auxiliary != features.end()
        && primary->second.input_width == auxiliary->second.input_width
        && primary->second.input_height == auxiliary->second.input_height;
    if (models_ready) {
      for (uint32_t pass = 0; pass < std::clamp(stack_passes.load(), 1u, kMaxNrPasses); ++pass) {
        const auto& model = auxiliary->second.slots[pass];
        models_ready &= model.handle != nullptr && model.nr_hdr_mode == base.hdr_mode
            && model.nr_create_flags == NrCreateFlags(contract.create_flags, base.hdr_mode != 0)
            && model.configuration_generation == configuration_generation.load();
      }
    }
    if (!models_ready) {
      FillPresentBlock(&role, input.hudless, motion, depth, contract,
                       input.color_width, input.color_height, 1.f, 1);
      PresentTarget target = base;
      auto adaptive_role = stream->adaptive_frame;
      adaptive_role.role = 2;
      target.adaptive_frame = &adaptive_role;
      target.game_frame_observed = true;
      target.source_color = nullptr;
      target.output_state = kRead;
      target.workset_key = reinterpret_cast<const ID3D12Resource*>(&role);
      target.capture_display = false;
      target.ui_correction = false;
      target.domain = present_restore_scope::Domain::PRE_FG_HUDLESS;
      target.original_identity = reinterpret_cast<uintptr_t>(input.hudless);
      ProcessInline(list, role.Handle(), &role.parameters, nullptr, &target, nullptr, true);
    }
  }
  restore_guides();
  const uint32_t output_roles = (has_hudless && !coupled_ui ? 3u : 1u);
  const auto group_result = PresentGroupResult(*recordings, output_roles, edit_reuse_frame, composed_roles);
  const bool recorded_edit = std::ranges::any_of(*recordings, [](const auto& recording) {
    return recording.edit_refreshed || recording.edit_reused;
  });
  if (present_frame_policy::Known(base_frame) && !group_result.complete && (native_edit_reuse_frame || recorded_edit)) {
    return fg::Result::FAILED;
  }
#if defined(NR_NEXUS_DEV_CONTROL)
  if (native_edit_reuse_frame) {
    // Bind duplicate delivery to the worksets that actually produced this
    // base's colors. Exact-alpha composition may have recorded only HUDLess.
    slot->native_edit_fixed_workset = true;
    for (uint32_t i = 0; i < recordings->size(); ++i) {
      if (!(*recordings)[i].output_ready) continue;
      const auto feature = features.find(stream->roles[i].Handle());
      if (feature == features.end() || !UsesFixedEditReuseWorkset(feature->second, fixed_workset)
          || (stream->adaptive_frame.enabled && feature->second.adaptive_cache != nullptr)
          || feature->second.input_width != input.color_width || feature->second.input_height != input.color_height) {
        slot->native_edit_fixed_workset = false;
        break;
      }
    }
  }
#endif
  // Composition can consume only HUDLess; a failed composition falls back to
  // two model colors. Freeze the actual complete role set before submission.
  const uint32_t model_roles = output_roles & ~composed_roles;
  const auto& primary_delivery = (*recordings)[(model_roles & 1u) != 0 ? 0u : 1u].adaptive_delivery;
  auto& delivery = slot->adaptive_delivery;
  delivery = primary_delivery;
  delivery.frame.delivery = stream->adaptive_frame.delivery;
  delivery.complete &= group_result.complete;
  for (uint32_t i = 0; i < recordings->size(); ++i) {
    if ((model_roles & (1u << i)) == 0) continue;
    const auto& role = (*recordings)[i].adaptive_delivery;
    delivery.complete &= role.complete && role.frame.role == (1u << i)
        && role.source == stream->roles[i].Handle() && role.output == input.output
        && role.frame.session == delivery.frame.session
        && role.frame.application_frame == delivery.frame.application_frame
        && role.frame.generation == delivery.frame.generation
        && role.frame.preparation_epoch == delivery.frame.preparation_epoch
        && role.frame.layout_identity == delivery.frame.layout_identity
        && (role.frame.delivery == nullptr || role.frame.delivery == delivery.frame.delivery)
        && role.actual == delivery.actual;
  }
  stream->adaptive_model_roles = model_roles;
  stream->adaptive_required_roles = model_roles;
  if (delivery.frame.delivery != nullptr) {
    for (auto& segment : gpu_timers::internal::segments) {
      if (segment.adaptive.delivery == delivery.frame.delivery) {
        segment.adaptive.required_roles = model_roles;
      }
    }
  }
  if (input.application_frame != 0) {
    for (uint32_t i = 0; i < recordings->size(); ++i) {
      if ((*recordings)[i].model_recorded) {
        stream->last_model_frame[i] = base_sequence;
      }
      if ((*recordings)[i].output_ready) {
        stream->last_output_frame[i] = base_sequence;
      }
    }
  }
  *output_ready = true;
  slot->base_frame = base_frame;
  slot->base_frame_group = group_result;
  return fg::Result::PREPARED;
}

inline void OnXeFGPresentDelivery(const fg::FrameInput& input, bool succeeded) noexcept {
  GuardHook([&] {
    RuntimeLock lock(runtime_mutex);
    const auto it = streams.find({false, input.context});
    if (it == streams.end()) return;
    auto& stream = it->second;
    if (stream.epoch != input.epoch || stream.application_frame != input.application_frame
        || stream.result != fg::Result::SUBMITTED || stream.xefg_delivery_output != input.output
        || !std::exchange(stream.awaiting_xefg_delivery, false)) return;
    const auto delivery = std::exchange(stream.pending_xefg_delivery, {});
    ResolveAdaptiveDelivery(delivery, succeeded);
    if (succeeded) return;
    // The private list was submitted on the SDK queue. Keep its slot and
    // receipts while making the next application frame start fresh history.
    stream.base_frame_clock.complete = false;
    stream.result = fg::Result::FAILED;
    for (auto& receipt : stream.edit_reuse_monitor) {
      if (receipt.delivery_epoch == input.epoch && receipt.sequence == input.application_frame) {
        receipt = {};
      }
    }
    ResetPresentStreamHistory(&stream.roles[0]);
    if (input.hudless != nullptr && input.hudless != input.backbuffer) {
      ResetPresentStreamHistory(&stream.roles[1]);
    }
  });
}

inline fg::Result Service(const fg::FrameInput& input) noexcept {
  try {
    const int64_t service_entry_ns = (xefg_gpu_capture_enabled.load(std::memory_order_relaxed) ? SteadyNowNs() : 0);
    if (!Requested() || input.queue == nullptr || input.backbuffer == nullptr
        || input.color_width == 0 || input.color_height == 0 || !input.fg_enabled) return fg::Result::NOT_SERVED;
    // AUTO composition may consume HUDLess too. If a mode change happened
    // after an omitted tag, retain the original frame instead of enhancing
    // only one of the SDK's color inputs.
    if (input.ui_composition && (input.ui_mode == 0 || input.ui_mode >= 3) && input.hudless == nullptr) {
      return fg::Result::NOT_SERVED;
    }
    ID3D12CommandQueue* queue = input.queue;
    renodx::utils::directx::NativeFromReShadeProxy(&queue);
    if (!InstallQueueCompletionHooks(queue)) return fg::Result::NOT_SERVED;
    ID3D12Device* device = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) return fg::Result::FAILED;
    PrimeNgxLoaderSymbols(device, true);
    auto device_release = std::unique_ptr<ID3D12Device, void (*)(ID3D12Device*)>(device,
        [](ID3D12Device* value) { value->Release(); });
    const BridgeDeviceLock device_lock;
    const ngx_serial::PresentTurn turn(true);
    if (!turn.owns()) return fg::Result::NOT_SERVED;
    RuntimeLock lock(runtime_mutex);
    // The previous Present route no longer rotates these owned recordings.
    if ((present_ring.device != nullptr && !ReleasePresentRing()) || !RetireBridgeRecordings()) {
      return fg::Result::NOT_SERVED;
    }
    auto& stream = streams[{false, input.context}];
    if (stream.device != nullptr && (stream.device != device || stream.queue != queue || stream.epoch != input.epoch)) {
      Release(&stream);
      stream = {};
    }
    if (stream.device == nullptr) {
      stream.device = device;
      device->AddRef();
      stream.queue = queue;
      queue->AddRef();
      stream.epoch = input.epoch;
      if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&stream.fence)))) {
        Release(&stream);
        stream = {};
        return fg::Result::FAILED;
      }
    }
    const present_frame_policy::Frame application_base{
        .kind = present_frame_policy::SourceKind::APPLICATION,
        .source = input.context, .generation = input.epoch, .sequence = input.application_frame,
        .pass_count = EvaluationStagePlan(stream.roles[0].Handle(), false).pass_count,
        .roles = (input.hudless != nullptr && input.hudless != input.backbuffer ? 3u : 1u)};
    const auto application_claim = present_frame_policy::Classify(stream.base_frame_clock, application_base);
    if (application_claim == present_frame_policy::Claim::STALE) return fg::Result::NOT_SERVED;
    if (application_claim == present_frame_policy::Claim::REPEATED
        || (!present_frame_policy::Known(application_base) && stream.has_frame
            && stream.application_frame == input.application_frame)) return stream.result;
    const bool service_interval_available = service_entry_ns > stream.gpu_capture_entry_ns
        && stream.gpu_capture_entry_ns != 0 && stream.has_frame
        && input.application_frame == stream.application_frame + 1;
    const double service_interval_ms = (service_interval_available
        ? (service_entry_ns - stream.gpu_capture_entry_ns) / 1e6 : 0.);
    stream.gpu_capture_entry_ns = service_entry_ns;
    stream.has_frame = true;
    stream.application_frame = input.application_frame;
    ResolveAdaptiveDelivery(stream.pending_xefg_delivery, false);
    stream.pending_xefg_delivery = {};
    stream.awaiting_xefg_delivery = false;
    stream.result = fg::Result::NOT_SERVED;
    const uint64_t completed = stream.fence->GetCompletedValue();
    if (completed == UINT64_MAX) return fg::Result::FAILED;
    bool discarded = false;
    for (auto& candidate : stream.slots) {
      if (candidate.quarantined || candidate.fence_value == 0 || completed < candidate.fence_value
          || candidate.list == nullptr) continue;
      // These lists belong to the XeFG service and are submitted exactly once.
      // Discard all completed recordings, including slots a fast GPU no longer
      // selects. Their replayable uses otherwise block mask-off -> on adoption.
      if (FAILED(candidate.list->Reset(candidate.allocator, nullptr))) {
        candidate.quarantined = true;
        continue;
      }
      submission::OnCommandListReset(candidate.list);
      discarded = true;
      if (FAILED(candidate.list->Close())) {
        candidate.quarantined = true;
        continue;
      }
      candidate.fence_value = 0;
    }
    if (discarded) {
      PruneCompletedSubmissionUses();
    }
    Slot* slot = nullptr;
    for (auto& candidate : stream.slots) {
      if (!candidate.quarantined && (candidate.fence_value == 0
          || completed >= candidate.fence_value)) {
        slot = &candidate;
        break;
      }
    }
    if (slot == nullptr) return stream.result;
    const auto type = queue->GetDesc().Type;
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) return stream.result;
    if (slot->allocator == nullptr
        && FAILED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&slot->allocator)))) {
      stream.result = fg::Result::FAILED;
      return stream.result;
    }
    if (slot->list == nullptr) {
      if (FAILED(device->CreateCommandList(0, type, slot->allocator, nullptr, IID_PPV_ARGS(&slot->list)))) {
        stream.result = fg::Result::FAILED;
        return stream.result;
      }
      slot->list->Close();
    }
    if (FAILED(slot->allocator->Reset()) || FAILED(slot->list->Reset(slot->allocator, nullptr))) {
      stream.result = fg::Result::FAILED;
      return stream.result;
    }
    submission::OnCommandListReset(slot->list);
    bool submitted = false;
    bool close_attempted = false;
    const auto close_recording = [&] {
      close_attempted = true;
      return slot->list->Close();
    };
    const auto cancel_recording = [&](Slot*) noexcept {
      if (submitted) return;
      try {
        if (!close_attempted) {
          if (gpu_timers::internal::current.active) {
            const auto& timing = gpu_timers::internal::segments[gpu_timers::internal::current.idx];
            if (timing.service_scope && timing.service_list == slot->list) {
              gpu_timers::End(slot->list, true);
            }
          }
          close_recording();
        }
        submission::OnCommandListReset(slot->list);
        Invalidate(&stream);
      } catch (...) {
      }
    };
    const std::unique_ptr<Slot, decltype(cancel_recording)> cancellation(slot, cancel_recording);
    // Switching providers requires closed, completed NR work. Adopt the
    // private runtime before this frame adds uses to its open recording.
    auto decline = NrDeclineReason::kNrRuntimeUnavailable;
    if (!EnsureDirectRuntime(slot->list, &decline, true)) {
      CountNrDecline(decline);
      return stream.result;
    }
    fg::FrameInput nr_input = input;
    const bool exact_ui = nr_input.hudless != nullptr && input.ui_composition && input.ui_mode == 3
        && input.ui != nullptr
        && input.color_x == 0 && input.color_y == 0
        && input.ui->GetDesc().Width == input.color_width && input.ui->GetDesc().Height == input.color_height;
    if (!PrepareSurfaces(&stream, slot, nr_input, false, exact_ui)) {
      close_recording();
      stream.result = fg::Result::FAILED;
      return stream.result;
    }
    PresentTarget target;
    target.private_signed_runtime = true;
    SnapshotPresentRestoreConfiguration(&target);
    target.swapchain_identity = reinterpret_cast<uintptr_t>(input.swapchain);
    target.swapchain_format = input.backbuffer->GetDesc().Format;
    if (input.color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
      target.hdr_mode = 2;
      target.units = {codec::Encoding::Pq, true, 10000.f, true};
    } else if (input.color_space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) {
      target.hdr_mode = 1;
      target.units = {codec::Encoding::Linear, true, 80.f, false};
    } else if (input.color_space != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
      close_recording();
      return stream.result;
    }
    const float mod_white = (present_white_source.load() == 0u ? present_mod_white.load() : 0.f);
    const float white = (present_white_nits.load() > 0.f ? present_white_nits.load()
        : (mod_white > 0.f ? mod_white : (present_sdr_white.load() > 0.f
            ? present_sdr_white.load() : diffuse_white_nits.load())));
    const uint32_t mode = codec_mode.load();
    target.hdr_codec_mode = mode;
    target.hdr_curve = (FrameDisplayCodec(target, mode) ? 1u : (neural_floor_guard.load() ? 2u : 0u));
    target.hdr_white_nits = white;
    target.hdr_white_source = "configured_present_white";
    target.hdr_encoding = V6EncodingConstant(target.units);
    target.hdr_dark_gate = V6DarkGate(target.units);
    target.hdr_pedestal_cap = codec::BlackPedestalCap(target.units);
    target.hdr_pedestal = present_hdr_pedestal::Decide(target.hdr_pedestal.configuration,
        {.actual_present = true, .hdr_mode = target.hdr_mode, .v6_available = mode != 0u,
         .encoding_valid = target.hdr_mode == 2 || target.hdr_mode == 1,
         .absolute_units = target.units.absolute, .unit_nits = target.units.unit_nits,
         .display_codec = target.hdr_curve == 1u, .legacy_mode = target.hdr_pedestal.legacy_mode,
         .debug_bypass = target.diagnostic.bypass_hdr_pedestal});
    target.divisor = (target.hdr_mode != 0 && mode != 1u && mode != 3u
        ? codec::PresentDivisor(target.units, white, paper_white_scale.load(), pq_calibration.load()) : 0.f);
    target.ui_correction = present_ui_correction.load();
    const EvaluateChainScope chain;
    // XeFG init flags are a different ABI from NGX feature flags. Motion
    // scale is already supplied in pixels by XeFG's public constants.
    nr_input.init_flags = ((input.init_flags & 1u) != 0 ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0)
        | ((input.init_flags & (1u << 4)) != 0 ? NVSDK_NGX_DLSS_Feature_Flags_MVJittered : 0)
        | (input.guide_width < input.color_width ? NVSDK_NGX_DLSS_Feature_Flags_MVLowRes : 0);
    if ((input.init_flags & (1u << 3)) != 0) {
      nr_input.motion_scale_x *= static_cast<float>(input.guide_width) * 0.5f;
      nr_input.motion_scale_y *= -static_cast<float>(input.guide_height) * 0.5f;
    }
    bridge::OwnedParameters composition;
    if (exact_ui) composition.Set(NVSDK_NGX_DLSSG_Parameter_UI, input.ui);
    bool output_ready = false;
    std::array<NrRecording, 2> recordings;
    const auto color_desc = input.backbuffer->GetDesc();
    const bool capture_service = xefg_gpu_capture_enabled.load(std::memory_order_relaxed)
        && control_mask_capture::Enabled() && gpu_timers_enabled.load(std::memory_order_relaxed)
        && nr_input.hudless == nullptr && !input.ui_composition && input.has_present_id && input.has_constants
        && input.depth != nullptr && input.motion != nullptr && input.guide_width != 0 && input.guide_height != 0
        && input.color_x == 0 && input.color_y == 0
        && color_desc.Width == input.color_width && color_desc.Height == input.color_height
        && stack_passes.load(std::memory_order_relaxed) == 1;
    const bool capture_timer = capture_service && gpu_timers::Begin(slot->list, device, "present",
        input.color_width, input.color_height, 0, 1, true, true);
    const uint32_t capture_index = gpu_timers::internal::current.idx;
    const auto finish_capture = [&] {
      if (!capture_service) return;
      uint32_t model_evaluates = 0;
      for (uint32_t mask = recordings[0].model_evaluate_mask; mask != 0; mask >>= 1) {
        model_evaluates += mask & 1u;
      }
      control_mask_capture::Context context{
          .requested_model = preset.load(std::memory_order_relaxed),
          .stage = static_cast<uint32_t>(stage_plan::Stage::PRESENT),
          .width = input.color_width, .height = input.color_height,
          .xefg_service = true, .application_frame = input.application_frame,
          .guide_serial = input.application_frame + 1, .stream_epoch = input.epoch,
          .service_entry_ns = service_entry_ns, .service_interval_ms = service_interval_ms,
          .service_interval_available = service_interval_available,
          .model_evaluates = model_evaluates, .refresh = recordings[0].model_recorded,
          .reuse = recordings[0].edit_reused, .output_ready = output_ready};
      std::snprintf(context.runtime_version, sizeof(context.runtime_version), "%s",
                    direct_runtime_requirements.file_version.c_str());
      const uint64_t token = control_mask_capture::Reserve(context,
          (capture_timer ? gpu_timers::internal::segments[capture_index].sequence : 0), service_entry_ns);
      if (capture_timer) {
        gpu_timers::internal::segments[capture_index].diagnostic_token = token;
        gpu_timers::End(slot->list, true);
      } else {
        control_mask_capture::Observe(gpu_timers::Stats{
            .state = gpu_timers::SampleState::PROOF_UNAVAILABLE, .measured_ns = gpu_timers::NowNs()}, token);
      }
    };
    const auto recorded = Record(&stream, slot, slot->list, nr_input, target,
                                 &output_ready, &recordings, exact_ui ? &composition : nullptr);
    uint32_t process_calls = 0;
    uint32_t evaluates = 0;
    uint32_t hudless_processes = 0;
    uint32_t backbuffer_processes = 0;
    const auto count_recording = [&](uint32_t role, const NrRecording& recording) {
      const bool attempted = recording.prepared || recording.model_invoked || recording.model_recorded
          || recording.output_ready || recording.group_complete || recording.model_pass_mask != 0;
      if (!attempted) return;
      ++process_calls;
      if (role == 1) {
        ++hudless_processes;
      } else {
        ++backbuffer_processes;
      }
      uint32_t mask = recording.model_evaluate_mask;
      while (mask != 0) {
        evaluates += mask & 1u;
        mask >>= 1u;
      }
    };
    count_recording(1, recordings[1]);
    count_recording(0, recordings[0]);
    fg::xefg::internal::RecordServiceStats(nr_input.diagnostic_context, process_calls, evaluates,
        hudless_processes, backbuffer_processes,
        hudless_processes != 0 && backbuffer_processes != 0,
        exact_ui && hudless_processes != 0 && backbuffer_processes == 0 && output_ready);
    if (!output_ready) {
      for (const auto& recording : recordings) {
        ResolveAdaptiveDelivery(recording.adaptive_delivery, false);
      }
      finish_capture();
      const bool closed = SUCCEEDED(close_recording());
      if (recorded == fg::Result::PREPARED && closed) {
        // Feature creation/maturation is real GPU work, but no color is
        // committed and the application frame is explicitly not served.
        for (uint32_t i = 0; i < recordings.size(); ++i) {
          if (recordings[i].model_invoked) ResetPresentStreamHistory(&stream.roles[i]);
        }
        ID3D12CommandList* list = slot->list;
        renodx::utils::directx::NativeFromReShadeProxy(&list);
        submission::TrackUse(slot->list, slot->allocator);
        submission::TrackUse(slot->list, slot->list);
        queue->ExecuteCommandLists(1, &list);
        submitted = true;
        slot->fence_value = ++stream.submitted;
        if (FAILED(queue->Signal(stream.fence, slot->fence_value))) slot->quarantined = true;
      } else {
        submission::OnCommandListReset(slot->list);
        Invalidate(&stream);
      }
      stream.result = (slot->quarantined ? fg::Result::FAILED : recorded);
      return stream.result;
    }
    CopyColor(slot->list, input.backbuffer, input.backbuffer_state, slot->color[0],
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0, 0, input.color_x, input.color_y,
              input.color_width, input.color_height);
    if (nr_input.hudless != nullptr && nr_input.hudless != input.backbuffer) {
      CopyColor(slot->list, input.hudless, input.hudless_state, slot->color[1],
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0, 0, input.color_x, input.color_y,
                input.color_width, input.color_height);
    }
    finish_capture();
    if (FAILED(close_recording())) {
      ResolveAdaptiveDelivery(slot->adaptive_delivery, false);
      submission::OnCommandListReset(slot->list);
      Invalidate(&stream);
      stream.result = fg::Result::FAILED;
      return stream.result;
    }
    submission::TrackUse(slot->list, slot->allocator);
    submission::TrackUse(slot->list, slot->list);
    ID3D12CommandList* list = slot->list;
    renodx::utils::directx::NativeFromReShadeProxy(&list);
    queue->ExecuteCommandLists(1, &list);
    submitted = true;
    slot->fence_value = ++stream.submitted;
    if (FAILED(queue->Signal(stream.fence, slot->fence_value))) {
      ResolveAdaptiveDelivery(slot->adaptive_delivery, false);
      slot->quarantined = true;
      if (slot->adaptive_delivery.frame.enabled) {
        stream.base_frame_clock.complete = false;
        for (auto& role : stream.roles) {
          ResetPresentStreamHistory(&role);
        }
      } else {
        Invalidate(&stream);
      }
      stream.result = fg::Result::FAILED;
      return stream.result;
    }
    stream.result = fg::Result::SUBMITTED;
    stream.pending_xefg_delivery = std::exchange(slot->adaptive_delivery, {});
    stream.xefg_delivery_output = input.output;
    stream.awaiting_xefg_delivery = true;
    present_frame_policy::Publish(&stream.base_frame_clock, slot->base_frame, true, slot->base_frame_group.complete);
    fg::xefg::RegisterDeliveryCallback(OnXeFGPresentDelivery);
    if (recordings[0].edit_monitor_delivery != EditReuseDelivery::NONE && nr_input.hudless == nullptr) {
      const auto surface = renodx::utils::directx::NativeIdentity(input.backbuffer);
      StoreEditReuseMonitorFrame(&stream.edit_reuse_monitor, {
          .delivery = recordings[0].edit_monitor_delivery, .reason = recordings[0].edit_monitor_reason,
          .stage = stage_plan::Stage::PRESENT, .configuration = configuration_generation.load(),
          .reset_epoch = history_reset_epoch.load(), .source_generation = input.epoch,
          .sequence = input.application_frame, .delivery_epoch = input.epoch, .recorded_ns = SteadyNowNs(),
          .device = renodx::utils::directx::NativeIdentity(device), .surface = surface,
          .output_width = input.color_width, .output_height = input.color_height, .handed_off = surface != nullptr});
    }
    PublishPresentRestoreOwner(present_restore_scope::Domain::PRE_FG_BACKBUFFER,
        target.restore_decision.reason, target.restore_contract_compatible,
        input.application_frame, target.restore_config_epoch, "fg_service_submitted");
    return stream.result;
  } catch (...) {
    return fg::Result::FAILED;
  }
}

}  // namespace fg_service

inline void PreserveNativePreFgProof(
    const void* marker, const std::vector<submission::GenerationSubmission>& submissions) {
#if defined(NR_NEXUS_DEV_CONTROL)
  native_tag_source::PreserveProof(marker, submissions);
#endif
  if (IsPresentGuideMarker(marker) || screenshot::WantsFinalPresent() || experimental_present_chain.load()) {
    PreservePresentCaptureProof(marker, submissions);
  }
  // An opt-out must still retain the previous producer's exact receipt so
  // a same-frame rollback never treats a pruned recording as unproven work.
  for (auto& [key, stream] : fg_service::streams) {
    if (stream.native_pair_producer_marker == nullptr || marker != stream.native_pair_producer_marker) continue;
    if (submissions.empty()) {
      stream.native_pair_discarded = true;
      continue;
    }
    for (const auto& proof : submissions) {
      if (std::pair{proof.list_identity, proof.recording_generation} != stream.native_pair_recording) continue;
      if (submissions.size() != 1 || proof.pending || proof.fence.empty()) {
        stream.native_pair_replayed = true;
        continue;
      }
      stream.native_pair_completion = proof.fence;
      stream.native_pair_completion_value = proof.value;
    }
  }
}

inline void OnNativePresentPassthrough(const PresentStream* owner) {
  const auto binding = fg_service::native_bindings.find(native_fg_owner);
  if (binding == fg_service::native_bindings.end()
      || binding->second != reinterpret_cast<uintptr_t>(owner)) return;
  native_fg_owner = nullptr;
  native_fg_owner_identity.store(0, std::memory_order_release);
  native_fg_owns.store(false, std::memory_order_release);
  native_fg_marker.store(nullptr, std::memory_order_release);
  native_fg_continuing_submission.store(false, std::memory_order_release);
  native_fg_result.store(fg::Result::NOT_SERVED, std::memory_order_release);
}

inline void OnNativeFgFeatureReleased(const NVSDK_NGX_Handle* handle) {
  if (handle == nullptr) return;
  output_framerate::ReleaseNativeFeature(reinterpret_cast<uintptr_t>(handle));
#if defined(NR_NEXUS_DEV_CONTROL)
  if (const auto source = created_feature_ids.find(handle);
      source != created_feature_ids.end() && source->second == NVSDK_NGX_Feature_FrameGeneration) {
    native_ui_alpha_observation::EndScope("fg_handle_released");
    native_tag_source::Reset();
  }
#endif
  if (native_fg_owner == handle) {
    native_fg_owner = nullptr;
    native_fg_owner_identity.store(0, std::memory_order_release);
    native_fg_owns.store(false, std::memory_order_release);
    native_fg_marker.store(nullptr, std::memory_order_release);
    native_fg_continuing_submission.store(false, std::memory_order_release);
    native_fg_result.store(fg::Result::NOT_SERVED, std::memory_order_release);
  }
  const auto binding = fg_service::native_bindings.find(handle);
  if (binding == fg_service::native_bindings.end()) return;
  if (const auto entry = fg_service::streams.find({true, binding->second});
      entry != fg_service::streams.end() && entry->second.native_handle == handle) {
    fg_service::ResetNativeFrame(&entry->second);
    entry->second.native_handle = nullptr;
  }
  fg_service::native_bindings.erase(binding);
}

// Caller holds runtime_mutex. Native game list generations retain their uses.
inline void RetireNativePresentOwner(const void* owner) {
  const auto context = reinterpret_cast<uintptr_t>(owner);
  for (auto binding = fg_service::native_bindings.begin(); binding != fg_service::native_bindings.end();) {
    if (binding->second == context) {
      output_framerate::ReleaseNativeFeature(reinterpret_cast<uintptr_t>(binding->first));
      if (native_fg_owner == binding->first) {
        native_fg_owner = nullptr;
        native_fg_owner_identity.store(0, std::memory_order_release);
        native_fg_owns.store(false, std::memory_order_release);
        native_fg_marker.store(nullptr, std::memory_order_release);
        native_fg_continuing_submission.store(false, std::memory_order_release);
        native_fg_result.store(fg::Result::NOT_SERVED, std::memory_order_release);
      }
      binding = fg_service::native_bindings.erase(binding);
    } else {
      ++binding;
    }
  }
  if (const auto entry = fg_service::streams.find({true, context}); entry != fg_service::streams.end()) {
    fg_service::Release(&entry->second);
    fg_service::streams.erase(entry);
  }
}

inline void OnNativePresentStreamRetired(const void* owner) {
#if defined(NR_NEXUS_DEV_CONTROL)
  native_tag_source::Reset();
#endif
  RetireNativePresentOwner(owner);
}

inline ID3D12Resource* RunNativePreFg(ID3D12GraphicsCommandList* list,
                                     const NVSDK_NGX_Parameter* parameters,
                                     const NVSDK_NGX_Handle* handle, bool* backbuffer_replaced) {
  // Observation only. Log the first unmet condition, rather than attributing
  // a downstream NOT_SERVED verdict to an input that was never inspected.
  struct ExperimentalDiagnostic {
    bool enabled;
    ID3D12GraphicsCommandList* list;
    const NVSDK_NGX_Parameter* parameters;
    const NVSDK_NGX_Handle* handle;
    const char* reason = "entry_contract";
    const PresentStream* owner = nullptr;
    fg_service::Stream* stream = nullptr;
    std::array<NrRecording, 2> recordings{};
#if defined(NR_NEXUS_DEV_CONTROL)
    bool tag_input_eligible = false;
    bool tag_input_claimed = false;
    fg::native_options::frame_source::Frame tag_input_frame{.reason = "input_not_observed"};
    const char* tag_input_claim_reason = "not_attempted";
#endif
    ~ExperimentalDiagnostic() noexcept {
      if (!enabled) return;
      try {
        static std::string last_signature;
        static ULONGLONG last_ms = 0;
        static uint32_t emitted = 0;
        const auto now = GetTickCount64();
        if (emitted >= 64 || (emitted != 0 && now - last_ms < 1000)) return;
        // Throttle observation too: an unchanged signature must not cause
        // resource/parameter queries on every later frame.
        last_ms = now;
        std::ostringstream line;
        line << "native-prefg-experiment reason=" << reason;
        const auto source = created_feature_ids.find(handle);
        line << " feature=" << (source == created_feature_ids.end() ? -1 : static_cast<int>(source->second));
        const auto uir = native_fg_ui_recomposition.find(handle);
        line << " uir=" << (uir == native_fg_ui_recomposition.end() ? -1 : static_cast<int>(uir->second));
        unsigned long long frame = 0;
        const auto frame_result = (parameters != nullptr ? parameters->Get(
            NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, &frame) : NVSDK_NGX_Result_FAIL_InvalidParameter);
        static uint32_t identity_probes = 0;
        if (experimental_native_prefg_resolution.load() && parameters != nullptr && source != created_feature_ids.end()
            && source->second == NVSDK_NGX_Feature_FrameGeneration
            && GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames) == 0
            && identity_probes < 3) {
          // The public interface has no key/type enumeration. Compare all
          // documented Get overloads with an unset control, without writing
          // either key or treating a readable value as a proven frame token.
          bool alternate_readable = false;
          const auto probe = [&]<typename T>(const char* key, const char* type, T value) {
            const auto result = parameters->Get(key, &value);
            line << ' ' << type << "=[result=0x" << std::hex << static_cast<uint32_t>(result) << std::dec;
            if (NVSDK_NGX_SUCCEED(result)) {
              if constexpr (std::is_pointer_v<T>) {
                line << " nonnull=" << (value != nullptr);
              } else {
                line << " value=" << value;
              }
            }
            line << ']';
            return NVSDK_NGX_SUCCEED(result);
          };
          line << " identity_probe=" << ++identity_probes << "/3 frame_id_getters={";
          for (const char* key : {NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, "NRNexus.AbsentFrameIdentityControl"}) {
            line << " key=" << key;
            const bool readable_ull = probe(key, "ull", 0ull);
            const bool readable_ui = probe(key, "uint", 0u);
            const bool readable_i = probe(key, "int", 0);
            const bool readable_f = probe(key, "float", 0.f);
            const bool readable_d = probe(key, "double", 0.);
            const bool readable_p = probe(key, "voidp", static_cast<void*>(nullptr));
            const bool readable_11 = probe(key, "d3d11", static_cast<ID3D11Resource*>(nullptr));
            const bool readable_12 = probe(key, "d3d12", static_cast<ID3D12Resource*>(nullptr));
            if (std::string_view(key) == NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID) {
              alternate_readable = readable_ull || readable_ui || readable_i || readable_f
                  || readable_d || readable_p || readable_11 || readable_12;
            }
          }
          line << " } frame_id_class="
               << (NVSDK_NGX_SUCCEED(frame_result) ? "uint64_readable_semantics_unverified"
                   : (alternate_readable ? "alternate_getter_readable_semantics_unverified"
                                         : "unreadable_absence_or_type_unproven"));
          line << " known_backbuffer_control={";
          probe(NVSDK_NGX_DLSSG_Parameter_Backbuffer, "d3d12", static_cast<ID3D12Resource*>(nullptr));
          probe(NVSDK_NGX_DLSSG_Parameter_Backbuffer, "voidp", static_cast<void*>(nullptr));
          line << " }";
        }
        uint32_t index = 1;
        const bool index_available = GetUIntIfPresent(parameters, NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, &index);
        const auto plan = RequestedStagePlan();
        line << " plan_valid=" << plan.valid << " mixed=" << plan.mixed << " passes=" << plan.pass_count
             << " present_passes=" << plan.At(stage_plan::Stage::PRESENT).count
             << " frame_id_available=" << NVSDK_NGX_SUCCEED(frame_result)
             << " frame_id_nonzero=" << (frame != 0)
             << " index_available=" << index_available << " index=" << index
             << " no_game=" << GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames);
        auto* back = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_Backbuffer);
        auto* hudless = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess);
        for (const auto& [name, key] : {std::pair{"back", NVSDK_NGX_DLSSG_Parameter_Backbuffer},
                 {"hudless", NVSDK_NGX_DLSSG_Parameter_HUDLess}, {"ui", NVSDK_NGX_DLSSG_Parameter_UI},
                 {"alpha", NVSDK_NGX_DLSSG_Parameter_UIAlpha}, {"depth", NVSDK_NGX_DLSSG_Parameter_Depth},
                 {"motion", NVSDK_NGX_DLSSG_Parameter_MVecs}}) {
          auto* resource = PreFgResource(parameters, key);
          const auto desc = (resource != nullptr ? resource->GetDesc() : D3D12_RESOURCE_DESC{});
          line << ' ' << name << "=[present=" << (resource != nullptr) << " size=" << desc.Width << 'x' << desc.Height
               << " format=" << static_cast<uint32_t>(desc.Format) << " back_alias="
               << (resource != nullptr && back != nullptr && renodx::utils::directx::SameNativeObject(resource, back))
               << " hudless_alias=" << (resource != nullptr && hudless != nullptr
                   && renodx::utils::directx::SameNativeObject(resource, hudless)) << ']';
        }
        for (uint32_t pass = 0; pass < 2; ++pass) {
          const auto resolution = CurrentPassResolution(pass);
          line << " pass" << pass + 1 << "=[follow=" << resolution.follow_input << " scale=" << resolution.scale << ']';
        }
        for (const char* key : {"DLSSG.MVecsSubrectBaseX", "DLSSG.MVecsSubrectBaseY",
                 "DLSSG.MVecsSubrectWidth", "DLSSG.MVecsSubrectHeight",
                 "DLSSG.DepthSubrectBaseX", "DLSSG.DepthSubrectBaseY"}) {
          uint32_t value = 0;
          const bool available = GetUIntIfPresent(parameters, key, &value);
          line << ' ' << key << "=[available=" << available << " value=" << value << ']';
        }
        for (uint32_t role = 0; role < recordings.size(); ++role) {
          const auto& record = recordings[role];
          line << " plane" << role << "=[prepared=" << record.prepared << " output_ready=" << record.output_ready
               << " model_invoked=" << record.model_invoked << " model_recorded=" << record.model_recorded
               << " edit_reused=" << record.edit_reused
               << " group_complete=" << record.group_complete
               << " model_evaluate_mask=" << record.model_evaluate_mask
               << " model_pass_mask=" << record.model_pass_mask << ']';
        }
        line << " fg_handle=" << handle << " owner=" << owner
             << " frame_id_get_result=" << static_cast<uint32_t>(frame_result)
             << " frame_id_type=uint64"
             << " guide_mode=" << present_guides.load()
             << " queue_tracking=" << queue_tracking_active.load();
#if defined(NR_NEXUS_DEV_CONTROL)
        if (std::string_view(reason) == "owner_missing") {
          const auto tag_observation = native_tag_source::Snapshot();
          line << " tag_input=[eligible:" << tag_input_eligible
               << ",available:" << tag_input_frame.available << ",claimed:" << tag_input_claimed
               << ",token:" << tag_input_frame.frame << ",epoch:" << tag_input_frame.epoch
               << ",reason:" << tag_input_frame.reason << ",pending:" << tag_observation.query_pending
               << ",claim:" << tag_input_claim_reason << ']';
        }
#endif
        if (stream != nullptr && stream->native_pair_producer_marker != nullptr) {
          std::vector<std::pair<FenceRef, uint64_t>> proofs;
          const auto state = submission::UseSubmissionTokens(stream->native_pair_producer_marker, &proofs);
          bool complete = !proofs.empty();
          for (const auto& [fence, value] : proofs) {
            const auto done = fence.fence->GetCompletedValue();
            complete &= done != UINT64_MAX && done >= value;
          }
          line << " producer_state=" << static_cast<uint32_t>(state) << " receipts=" << proofs.size()
               << " receipts_complete=" << complete << " cached=" << (stream->cached != nullptr)
               << " replayed=" << stream->native_pair_replayed << " discarded=" << stream->native_pair_discarded;
        } else {
          line << " producer=unavailable";
        }
        // Frame IDs and recording generations advance every frame. Keep
        // identity/contract/proof changes in the signature, but not counters.
        const auto signature = line.str();
        if (signature == last_signature) return;
        last_signature = signature;
        std::pair<uint64_t, uint64_t> recording{};
        {
          // Never create or advance a recording merely to explain a rejection.
          std::lock_guard lock(submission::tracker_mutex);
          const auto identity = submission::identity_by_pointer.find(list);
          if (identity != submission::identity_by_pointer.end()) {
            const auto state = submission::tracked_lists.find(identity->second);
            if (state != submission::tracked_lists.end()) recording = {identity->second, state->second.current};
          }
        }
        line << " frame_id=" << frame << " recording=" << recording.first << ':' << recording.second;
        line << " gpu_execution=unverified log_budget=" << ++emitted << "/64";
        LogPresent(reshade::log::level::info, line.str());
      } catch (...) {
        // Diagnostic allocation/logging must not change FG execution or unwind.
      }
    }
  } diagnostic{.enabled = experimental_native_prefg_resolution.load(), .list = list,
               .parameters = parameters, .handle = handle};
#if defined(NR_NEXUS_DEV_CONTROL)
  diagnostic.enabled |= edit_reuse_enabled.load() && edit_reuse_fg_experiment.load();
#endif
  if (backbuffer_replaced != nullptr) {
    *backbuffer_replaced = false;
  }
  // The overlay reads atomics without runtime_mutex. Publish a declined
  // recording on exit, not a transient outage before the model is called.
  struct DisplayRecording {
    bool preserve_current = false;
    fg::Result result = fg::Result::NOT_SERVED;
    ~DisplayRecording() {
      if (!preserve_current) {
        native_fg_continuing_submission.store(false, std::memory_order_release);
        native_fg_result.store(result, std::memory_order_release);
      }
    }
  } display_recording;
  diagnostic.reason = "xefg_owns_stream";
  if (fg::xefg::OwnsStream()) return nullptr;
  diagnostic.reason = "prefg_not_requested";
  if (!fg_service::Requested()) return nullptr;
  diagnostic.reason = "command_list_missing";
  if (list == nullptr) return nullptr;
  diagnostic.reason = "public_fg_off_without_present_decisions";
  if (native_fg_public_off.load(std::memory_order_acquire) && !fg::native_options::ObservesPresentDecisions()) return nullptr;
  diagnostic.reason = "queue_tracking_unavailable";
  if (!queue_tracking_active.load(std::memory_order_acquire)) return nullptr;
  auto* backbuffer = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_Backbuffer);
  auto* hudless = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess);
  auto* ui = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_UI);
  diagnostic.reason = "backbuffer_missing";
  if (backbuffer == nullptr) return nullptr;
  // FG inputs are NPS and outputs are UAV. Restoring an aliased input would
  // also overwrite FG's output, so reject the unsupported contract up front.
  for (const char* key : {NVSDK_NGX_DLSSG_Parameter_OutputReal,
                          NVSDK_NGX_DLSSG_Parameter_OutputInterpolated}) {
    diagnostic.reason = "fg_output_alias";
    if (auto* output = PreFgResource(parameters, key); output != nullptr
        && (output == backbuffer || renodx::utils::directx::SameNativeObject(output, backbuffer))) return nullptr;
  }
  const auto desc = backbuffer->GetDesc();
  ID3D12Device* device = nullptr;
  diagnostic.reason = "list_device";
  if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return nullptr;
  auto device_release = std::unique_ptr<ID3D12Device, void (*)(ID3D12Device*)>(device,
      [](ID3D12Device* value) { value->Release(); });
  const auto identity = renodx::utils::directx::NativeIdentity(device);
#if defined(NR_NEXUS_DEV_CONTROL)
  uint32_t input_index = 0, input_no_game = 0;
  const auto input_feature = created_feature_ids.find(handle);
  if (edit_reuse_enabled.load() && edit_reuse_fg_experiment.load()
      && hook_point.load() == kHookPresent && !fg::xefg::Enabled()
      && handle != nullptr && input_feature != created_feature_ids.end()
      && input_feature->second == NVSDK_NGX_Feature_FrameGeneration
      && hudless != nullptr && identity != nullptr
      && GetUIntIfPresent(parameters, NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, &input_index) && input_index == 1
      && GetUIntIfPresent(parameters, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, &input_no_game) && input_no_game == 0) {
    const auto recording = submission::RecordingIdentity(list);
    diagnostic.tag_input_eligible = recording.first != 0 && (recording.first >> 63) == 0 && recording.second != 0;
    diagnostic.tag_input_frame.reason = "recording_identity_unavailable";
    if (diagnostic.tag_input_eligible) {
      // This is the actual FG input even if NR has no Present owner or is
      // still warming. Claim identity only; all NR admission stays below.
      diagnostic.tag_input_frame = native_tag_source::CurrentFrame(hudless, list);
      if (diagnostic.tag_input_frame.available) {
        diagnostic.tag_input_claimed = native_tag_source::ClaimFrame(diagnostic.tag_input_frame, hudless, list);
        diagnostic.tag_input_claim_reason = native_tag_source::Snapshot().claim_reason;
      }
    }
  }
#endif
  const PresentStream* owner = nullptr;
  for (const auto& [key, candidate] : present_streams) {
    if (!candidate.d3d11 && candidate.prefg_target_valid
        && (candidate.device_identity == identity || candidate.streamline_identity == identity)
        && candidate.backbuffer_width == desc.Width && candidate.backbuffer_height == desc.Height
        && ConcreteResourceFormat(candidate.backbuffer_format) == ConcreteResourceFormat(desc.Format)) {
      diagnostic.reason = "owner_ambiguous";
      if (owner != nullptr) return nullptr;
      owner = &candidate;
    }
  }
  diagnostic.reason = "owner_missing";
  if (owner == nullptr) return nullptr;
  diagnostic.owner = owner;
  auto* current_depth = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_Depth);
  auto* current_motion = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_MVecs);
  const auto motion_desc = (current_motion != nullptr ? current_motion->GetDesc() : D3D12_RESOURCE_DESC{});
  const bool current_guides = current_depth != nullptr && current_motion != nullptr
      && GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth,
                 static_cast<uint32_t>(motion_desc.Width)) != 0
      && GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, motion_desc.Height) != 0
      && GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames) == 0;
  const auto pair_plan = RequestedStagePlan();
  const bool resolution_pair = experimental_native_prefg_resolution.load()
      && pair_plan.valid && !pair_plan.mixed && pair_plan.pass_count == 2
      && pair_plan.At(stage_plan::Stage::PRESENT).count == 2;
  const uint32_t index = GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1);
  unsigned long long frame_id = 0;
  if (NVSDK_NGX_FAILED(parameters->Get(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, &frame_id))) frame_id = 0;
#if defined(NR_NEXUS_DEV_CONTROL)
  uint64_t claimed_tag_epoch = (diagnostic.tag_input_claimed ? diagnostic.tag_input_frame.epoch : 0);
  if (index > 1) {
    const auto previous = fg_service::streams.find({true, reinterpret_cast<uintptr_t>(owner)});
    if (previous != fg_service::streams.end() && previous->second.native_edit_source
        && previous->second.native_edit_source_is_tagged && previous->second.native_handle == handle) {
      claimed_tag_epoch = previous->second.native_edit_source_epoch;
    }
  }
  const auto tagged_frame = native_tag_source::CurrentFrame(hudless, list, claimed_tag_epoch);
  const bool native_tag_frame = tagged_frame.available;
  const auto native_frame = (native_tag_frame ? tagged_frame
      : fg::native_options::CurrentFrame(reinterpret_cast<uintptr_t>(owner->native_output_identity)));
  auto* ui_alpha = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_UIAlpha);
  uint32_t no_game = 0, declared_index = 0, motion_jittered = 0;
  float jitter_x = 0.f, jitter_y = 0.f, motion_scale_x = 0.f, motion_scale_y = 0.f;
  const bool frame_parameters = GetUIntIfPresent(parameters, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, &no_game)
      && no_game == 0 && GetUIntIfPresent(parameters, NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, &declared_index)
      && declared_index >= 1
      && GetFloatIfPresent(parameters, NVSDK_NGX_DLSSG_Parameter_JitterOffsetX, jitter_x)
      && GetFloatIfPresent(parameters, NVSDK_NGX_DLSSG_Parameter_JitterOffsetY, jitter_y)
      && GetFloatIfPresent(parameters, NVSDK_NGX_DLSSG_Parameter_MvecScaleX, motion_scale_x)
      && GetFloatIfPresent(parameters, NVSDK_NGX_DLSSG_Parameter_MvecScaleY, motion_scale_y)
      && GetUIntIfPresent(parameters, NVSDK_NGX_DLSSG_Parameter_MvecJittered, &motion_jittered) && motion_jittered == 0;
  const auto source_feature = created_feature_ids.find(handle);
  const bool native_edit_requested = edit_reuse_enabled.load() && edit_reuse_fg_experiment.load();
  bool native_edit_source = native_edit_requested && native_frame.available && frame_parameters
      && source_feature != created_feature_ids.end() && source_feature->second == NVSDK_NGX_Feature_FrameGeneration
      && pair_plan.valid && !pair_plan.mixed && pair_plan.pass_count == 1 && !resolution_pair
      && !native_fg_public_off.load() && current_guides && !fg::xefg::Enabled();
  if (native_edit_source && native_tag_frame && index == 1) {
    native_edit_source = diagnostic.tag_input_claimed
        && tagged_frame.epoch == diagnostic.tag_input_frame.epoch
        && tagged_frame.frame == diagnostic.tag_input_frame.frame
        && tagged_frame.generation == diagnostic.tag_input_frame.generation;
  }
  // Preserve the producer's uint32 identity including frame zero. Wrap is a
  // discontinuity to the cache, rather than an invented application clock.
  const uint64_t native_edit_sequence = (native_edit_source ? static_cast<uint64_t>(native_frame.frame) + 1 : 0);
  const present_frame_policy::Frame native_base_frame{
      .kind = (native_tag_frame ? present_frame_policy::SourceKind::TAGGED_RESOURCE : present_frame_policy::SourceKind::SDK_SCOPE),
      .source = (native_edit_source ? reinterpret_cast<uintptr_t>(owner->native_output_identity) : 0),
      .generation = native_frame.generation, .sequence = native_edit_sequence,
      .sequence_period = uint64_t{1} << 32,
      .pass_count = pair_plan.pass_count, .roles = (hudless != nullptr && hudless != backbuffer ? 3u : 1u),
      .reuse_edits = true};
  if (native_edit_requested) {
    static int64_t last_observation_ns = 0;
    static uint32_t observations = 0;
    const auto now = SteadyNowNs();
    if (observations < 32 && now - last_observation_ns >= 1'000'000'000) {
      last_observation_ns = now;
      const auto observation = fg::native_options::frame_source::Snapshot();
      const auto tag_observation = native_tag_source::Snapshot();
      std::ostringstream line;
      line << "NR native edit source: available=" << native_frame.available
           << " reason=" << native_frame.reason << " token=" << native_frame.frame
           << " epoch=" << native_frame.epoch << " generation=" << native_frame.generation
           << " parameters=" << frame_parameters << " index=" << declared_index
           << " real_guides=" << current_guides << " route=" << native_edit_source
           << " source_provider=" << (native_tag_frame ? "tagged_resource" : "sdk_scope")
           << " tag_source_reason=" << tagged_frame.reason
           << " tag=[legacy:" << tag_observation.legacy_calls << ",framed:" << tag_observation.for_frame_calls
           << ",legacy_hudless:" << tag_observation.legacy_hudless_calls
           << ",framed_hudless:" << tag_observation.for_frame_hudless_calls
           << ",hudless:" << tag_observation.hudless_calls << ",scopes:" << tag_observation.scope_enters
           << ",copies:" << tag_observation.copy_calls << ",scoped:" << tag_observation.scoped_copies
           << ",matches:" << tag_observation.source_matches << ",published:" << tag_observation.publish_count
           << ",claimed:" << tag_observation.claim_count << ",pending:" << tag_observation.query_pending
           << ",removed:" << tag_observation.removal_count << ",live:" << tag_observation.live_versions
           << ",framed_last:" << tag_observation.last_frame_bound << ",tag_token:" << tag_observation.tag_frame
           << ",tag_viewport:" << tag_observation.tag_viewport << ",token:" << tag_observation.frame
           << ",viewport:" << tag_observation.viewport << ",life:" << tag_observation.lifecycle
           << ",native:" << tag_observation.tag_native << ",source:" << tag_observation.source_identity
           << ",clone:" << tag_observation.clone_identity << ",input:" << tag_observation.query_identity
           << ",list:" << tag_observation.list_identity << ",recording:" << tag_observation.recording_list
           << '/' << tag_observation.recording_generation << ",hooks:" << tag_observation.hooks_ready
           << ",gate:" << tag_observation.gate_reason << ",scope:" << tag_observation.scope_reason
           << ",copy:" << tag_observation.copy_reason << ",finish:" << tag_observation.finish_reason
           << ",claim:" << tag_observation.claim_reason << ",direct_block:" << tag_observation.direct_block_reason
           << ",remove:" << tag_observation.removal_reason << ",resets:" << tag_observation.reset_count << ']'
           << " hook_facts=" << fg::native_options::FrameHookFacts()
           << " sdk_ready=" << fg::native_options::ObservesPresentDecisions()
           << " current_thread=" << observation.current_thread << " tls_depth=" << observation.tls_depth
           << " binding_active=" << observation.binding_active << " binding_available=" << observation.binding_available
           << " marker_published=" << observation.marker_published << " marker_thread=" << observation.marker_thread
           << " sampled_epoch=" << observation.marker_epoch << " marker_reason=" << observation.marker_reason
           << " sdk_enters=" << observation.events.sdk_enters << " sdk_exits=" << observation.events.sdk_exits
           << " sdk_active=" << observation.events.sdk_active
           << " event_sequence=" << observation.events.sequence << " sampled_qpc=" << observation.qpc
           << " feature=" << handle << " output=" << owner->native_output_identity << " list=" << list;
      const auto append_event = [&](const char* name, const fg::native_options::frame_source::Event& event) {
        line << ' ' << name << "=[seq:" << event.sequence << ",qpc:" << event.qpc
             << ",thread:" << event.thread << ",marker:" << event.marker << ",frame:" << event.frame
             << ",output:" << event.output << ",depth:" << event.depth << ",ok:" << event.success << ']';
      };
      append_event("marker_enter", observation.events.marker_enter);
      append_event("marker_return", observation.events.marker_return);
      append_event("sdk_enter", observation.events.sdk_enter);
      append_event("sdk_exit", observation.events.sdk_exit);
      const auto append_gateway = [&](const char* name, const fg::native_options::frame_source::EvaluateBinding& request) {
        line << ' ' << name << "=[call:" << request.call << ",parent:" << request.parent
             << ",feature:" << request.feature << ",frame:" << request.frame
             << ",command_buffer:" << request.command_buffer << ",inputs:" << request.inputs
             << ",depth:" << request.depth << ",dlssg_call:" << request.dlssg_call
             << ",dlssg_frame:" << request.dlssg_frame << ']';
      };
      append_gateway("evaluate_current", observation.current_evaluate);
      const auto& evaluated = observation.evaluations;
      line << " evaluate_calls=" << evaluated.enters << " evaluate_returns=" << evaluated.returns
           << " evaluate_active=" << evaluated.active << " ngx_observed_calls=" << evaluated.ngx_calls
           << " ngx_fg_calls=" << evaluated.ngx_fg_calls << " ngx_fg_nested=" << evaluated.ngx_fg_nested
           << " ngx_fg_under_dlssg=" << evaluated.ngx_fg_under_dlssg;
      for (uint32_t index = 0; index < evaluated.features.size(); ++index) {
        const auto& feature = evaluated.features[index];
        line << " evaluate_bucket" << index << "=[calls:" << feature.enters << ",returns:" << feature.returns
             << ",active:" << feature.active << ",ngx_calls:" << feature.ngx_calls
             << ",ngx_fg_calls:" << feature.ngx_fg_calls << ']';
        for (const auto& [name, event] : {std::pair{"evaluate_enter", &feature.enter},
                 {"evaluate_exit", &feature.returned}}) {
          if (event->request.call == 0) continue;
          const auto event_name = std::string(name) + std::to_string(index);
          line << ' ' << event_name << "=[seq:" << event->cpu.sequence << ",qpc:" << event->cpu.qpc
               << ",thread:" << event->cpu.thread << ",returned:" << event->returned
               << ",result:" << event->result << ']';
          append_gateway((event_name + "_request").c_str(), event->request);
        }
      }
      for (const auto& [name, event] : {std::pair{"ngx_enter", &evaluated.ngx_enter},
               {"ngx_exit", &evaluated.ngx_return}, {"ngx_fg_enter", &evaluated.ngx_fg_enter},
               {"ngx_fg_exit", &evaluated.ngx_fg_return}}) {
        line << ' ' << name << "=[seq:" << event->cpu.sequence << ",qpc:" << event->cpu.qpc
             << ",thread:" << event->cpu.thread << ",call:" << event->call << ",parent:" << event->parent
             << ",depth:" << event->depth << ",slot:" << event->slot << ",c_export:" << event->c_export
             << ",wrapper_delegate:" << event->wrapper_delegate << ",fg_classified:" << event->frame_generation
             << ",handle:" << event->handle << ",list:" << event->command_list
             << ",returned:" << event->returned << ",result:" << event->result << ']';
        append_gateway((std::string(name) + "_gateway").c_str(), event->gateway);
      }
      line << " evaluate_evidence=cpu_api_arguments_and_nesting_only"
           << " ngx_observation_scope=classified_d3d12_forward_paths";
      native_tag_source::AppendPremiseObservation(line);
      line << " log_budget=" << ++observations << "/32";
      LogPresent(reshade::log::level::info, line.str());
    }
  }
#endif
  if (resolution_pair) {
    const auto source = created_feature_ids.find(handle);
    diagnostic.reason = "fg_feature_unrecognized";
    if (handle == nullptr || source == created_feature_ids.end()
        || source->second != NVSDK_NGX_Feature_FrameGeneration) return nullptr;
    diagnostic.reason = "frame_id_uint64_unreadable_or_zero";
    if (frame_id == 0) return nullptr;
    diagnostic.reason = "single_backbuffer_requires_no_hudless_ui";
    if (hudless != nullptr || ui != nullptr
        || PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_UIAlpha) != nullptr) return nullptr;
    diagnostic.reason = "real_guides_unavailable";
    if (!current_guides) return nullptr;
    diagnostic.reason = "xefg_enabled";
    if (fg::xefg::Enabled()) return nullptr;
    diagnostic.reason = "guide_mode_never";
    if (diagnostics::GuideMode(owner->prefg_target.diagnostic.guides,
                             present_guides.load()) == kPresentGuidesNever) return nullptr;
    const uint32_t guide_width = GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth,
                                        static_cast<uint32_t>(motion_desc.Width));
    const uint32_t guide_height = GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight,
                                         motion_desc.Height);
    diagnostic.reason = "guide_subrect_invalid";
    if (!SubrectFits(GetUInt(parameters, "DLSSG.MVecsSubrectBaseX"),
                    GetUInt(parameters, "DLSSG.MVecsSubrectBaseY"), guide_width, guide_height, motion_desc)
        || !SubrectFits(GetUInt(parameters, "DLSSG.DepthSubrectBaseX"),
                       GetUInt(parameters, "DLSSG.DepthSubrectBaseY"), guide_width, guide_height,
                       current_depth->GetDesc())) return nullptr;
  }
  if (native_sdk_present_input.decision_id != 0
      && native_sdk_present_input.identity == owner->native_output_identity) {
    if (!current_guides) {
      native_sdk_present_input = {.identity = native_sdk_present_input.identity,
          .decision_id = native_sdk_present_input.decision_id, .owner = owner,
          .route_id = owner->native_route_id, .backbuffer = backbuffer, .guides_unavailable = true};
    } else {
      native_sdk_present_input.guides_unavailable = false;
    }
  }
  // Temporary FG stops can return this output to its existing Final Present
  // model. Retain that model, without inventing a new guide contract here.
  owner_resident_nr_handles.insert(owner->Handle());
  owner_resident_nr_handles.insert(owner->Handle(true));
  ComputeStateEnvelope envelope(list);
  envelope.StopCapture();
  diagnostic.reason = "command_state_unproven";
  if (!envelope.CanInject()) return nullptr;
  // NR models belong to the matched output. A game FG feature may restart
  // on focus changes without changing the NR image contract.
  const auto context = (handle != nullptr ? reinterpret_cast<uintptr_t>(owner) : reinterpret_cast<uintptr_t>(parameters));
  auto& stream = fg_service::streams[{true, context}];
  diagnostic.stream = &stream;
#if defined(NR_NEXUS_DEV_CONTROL)
  if (native_edit_source && present_frame_policy::Classify(stream.base_frame_clock, native_base_frame)
          == present_frame_policy::Claim::STALE) {
    diagnostic.reason = "base_frame_stale";
    display_recording.preserve_current = true;
    return nullptr;
  }
#endif
  if (stream.result == fg::Result::FAILED) {
    fg_service::Invalidate(&stream);
    stream.result = fg::Result::FAILED;
  }
  if (stream.device != nullptr && stream.device != device) {
    fg_service::Release(&stream);
    stream = {};
  }
  if (stream.device == nullptr) {
    stream.device = device;
    device->AddRef();
    stream.native = true;
    if (handle != nullptr) {
      for (auto& role : stream.roles) {
        owner_resident_nr_handles.insert(role.Handle());
      }
    }
  }
  if (handle != nullptr) {
    fg_service::native_bindings[handle] = context;
  }
#if defined(NR_NEXUS_DEV_CONTROL)
  if (stream.native_edit_source != native_edit_source
      || (native_edit_source && (stream.native_edit_source_generation != native_frame.generation
          || stream.native_edit_source_is_tagged != native_tag_frame))) {
    // Source changes invalidate fixed edit history. Adaptive worksets keep
    // their own history across missing or changed SDK token scopes.
    if (stream.native_edit_history) fg_service::Invalidate(&stream);
    stream.native_edit_source = native_edit_source;
    stream.native_edit_frame = 0;
    stream.native_edit_source_epoch = 0;
    stream.native_edit_source_generation = native_frame.generation;
    stream.native_edit_source_is_tagged = native_tag_frame;
  }
  if (native_edit_source && native_tag_frame && stream.native_edit_frame == native_edit_sequence
      && stream.native_edit_source_epoch != native_frame.epoch) {
    // A replacement tag for the same token cannot borrow the old pixels.
    fg_service::Invalidate(&stream);
    diagnostic.reason = "native_tag_content_replaced";
    return nullptr;
  }
#endif
  const auto configuration = configuration_generation.load(std::memory_order_relaxed);
  const auto options_generation = native_fg_options_generation.load(std::memory_order_acquire);
  const std::array pair_resolutions{CurrentPassResolution(0), CurrentPassResolution(1)};
  const auto same_resolutions = [](const auto& first, const auto& second) {
    for (size_t i = 0; i < first.size(); ++i) {
      if (first[i].follow_input != second[i].follow_input || first[i].scale != second[i].scale
          || first[i].commit_source != second[i].commit_source || first[i].commit_epoch != second[i].commit_epoch) return false;
    }
    return true;
  };
  if (stream.native_pair_producer_marker != nullptr) {
    const bool same_recording = stream.native_pair_recording == submission::RecordingIdentity(list);
    std::vector<std::pair<FenceRef, uint64_t>> proofs;
    const auto proof = submission::UseSubmissionTokens(stream.native_pair_producer_marker, &proofs);
    bool completed_producer = proof == submission::UseSubmission::kTokens && proofs.size() == 1;
    if (proof == submission::UseSubmission::kComplete && !stream.native_pair_completion.empty()
        && !stream.native_pair_discarded && !stream.native_pair_replayed) {
      const auto completed = stream.native_pair_completion.fence->GetCompletedValue();
      completed_producer = completed != UINT64_MAX && completed >= stream.native_pair_completion_value;
    }
    for (const auto& [fence, value] : proofs) {
      const auto completed = fence.fence->GetCompletedValue();
      completed_producer &= completed != UINT64_MAX && completed >= value;
    }
    // Another list has no known queue ordering here. Poll a single genuine
    // producer completion; an empty/discarded recording is never production.
    if (stream.native_pair_replayed || stream.native_pair_discarded || proofs.size() > 1
        || (!same_recording && !completed_producer)) {
      diagnostic.reason = "producer_discarded_replayed_or_unfinished";
      bool same_frame = frame_id == stream.source_frame_id;
#if defined(NR_NEXUS_DEV_CONTROL)
      if (stream.native_edit_source) same_frame = native_edit_sequence == stream.native_edit_frame;
#endif
      if (same_frame || (!proofs.empty() && !completed_producer)) return nullptr;
      fg_service::Invalidate(&stream);
    }
    bool cached_same_frame = stream.native_resolution_pair && frame_id == stream.source_frame_id;
#if defined(NR_NEXUS_DEV_CONTROL)
    cached_same_frame |= stream.native_edit_source && native_edit_sequence == stream.native_edit_frame;
#endif
    if (stream.cached != nullptr && cached_same_frame
        && (stream.native_resolution_pair != resolution_pair || stream.native_configuration != configuration
            || stream.native_handle != handle || stream.native_options_generation != options_generation
            || stream.native_display_epoch != history_reset_epoch.load()
            || !same_resolutions(stream.native_pair_resolutions, pair_resolutions))) {
      const auto original_desc = stream.cached->original_backbuffer->GetDesc();
      if (backbuffer == stream.source_backbuffer && desc.Width == original_desc.Width
          && desc.Height == original_desc.Height && desc.Format == original_desc.Format
          && desc.SampleDesc.Count == original_desc.SampleDesc.Count) {
        fg_service::CopyColor(list, backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                              stream.cached->original_backbuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      }
      fg_service::ResetNativeFrame(&stream);
      diagnostic.reason = "cached_configuration_changed";
      return nullptr;
    }
  }
  if (stream.native_resolution_pair != resolution_pair) fg_service::ResetNativeFrame(&stream);
  if (stream.native_configuration != configuration || stream.native_handle != handle
      || stream.native_options_generation != options_generation) {
    fg_service::ResetNativeFrame(&stream);
    stream.native_configuration = configuration;
    stream.native_options_generation = options_generation;
    stream.native_handle = handle;
    native_fg_marker.store(nullptr, std::memory_order_release);
    native_fg_continuing_submission.store(false, std::memory_order_release);
    native_fg_result.store(fg::Result::PREPARED, std::memory_order_release);
  }
  // NGX DLSS-FG requires NPS for every input on both direct and compute lists.
  constexpr auto input_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  const uint64_t sdk_decision = (owner->native_output_identity != nullptr
      && native_sdk_present_input.identity == owner->native_output_identity
      ? native_sdk_present_input.decision_id : 0);
  const bool same_sdk_frame = frame_id == 0 && stream.source_frame_id == 0
      && sdk_decision != 0 && stream.source_sdk_decision == sdk_decision;
  bool repeated_frame = ((frame_id != 0 && frame_id == stream.source_frame_id) || same_sdk_frame)
      && stream.cached != nullptr && stream.result == fg::Result::PREPARED
      && stream.source_backbuffer == backbuffer && stream.source_hudless == hudless && stream.source_ui == ui;
#if defined(NR_NEXUS_DEV_CONTROL)
  if (native_edit_source) {
    const bool same_native_frame = present_frame_policy::Classify(stream.base_frame_clock, native_base_frame)
            == present_frame_policy::Claim::REPEATED
        && (!native_tag_frame || native_frame.epoch == stream.native_edit_source_epoch) && stream.cached != nullptr
        && stream.result == fg::Result::PREPARED && stream.source_backbuffer == backbuffer
        && stream.source_hudless == hudless && stream.source_ui == ui && stream.native_edit_ui_alpha == ui_alpha;
    // Only the identified base's completed fixed worksets can use the x2
    // delivery path. Adaptive output still needs its normal frame identity.
    repeated_frame = same_native_frame && (stream.cached->native_edit_fixed_workset || repeated_frame);
    if ((index > 1 || native_edit_sequence == stream.native_edit_frame) && !repeated_frame) return nullptr;
  }
#endif
  if (index > 1 || repeated_frame) {
    if (!repeated_frame && frame_id == 0 && sdk_decision != 0 && stream.source_sdk_decision != 0
        && stream.source_sdk_decision != sdk_decision) {
      diagnostic.reason = "mfg_sdk_decision_mismatch";
      display_recording.preserve_current = true;
      return nullptr;
    }
    diagnostic.reason = "mfg_cache_unavailable";
    if (stream.cached == nullptr && stream.result == fg::Result::PREPARED) {
      display_recording.result = fg::Result::PREPARED;
      return nullptr;
    }
    if (stream.result == fg::Result::FAILED) {
      display_recording.result = fg::Result::FAILED;
    }
    if (stream.cached == nullptr || stream.result != fg::Result::PREPARED
        || stream.source_backbuffer != backbuffer || stream.source_hudless != hudless
        || stream.source_ui != ui
        || (frame_id != 0 && frame_id != stream.source_frame_id)) return nullptr;
    // Reuse bypasses Record. A real-guide cache does not authorize NR after
    // this invocation withdraws its guides or declares no game frame.
    bool cached_real_guides = stream.roles[0].real_guides;
#if defined(NR_NEXUS_DEV_CONTROL)
    if (native_edit_source) {
      // Exact-alpha composition may have delivered only a HUDLess NR role;
      // a reuse frame also has no newly recorded model to count as its role.
      cached_real_guides = false;
      for (const uint32_t role : {0u, 1u}) {
        if (stream.last_output_frame[role] != stream.base_frame_clock.frame.sequence) continue;
        cached_real_guides = stream.roles[role].real_guides;
        if (!cached_real_guides) break;
      }
    }
#endif
    if (diagnostics::GuideMode(owner->prefg_target.diagnostic.guides,
                              present_guides.load(std::memory_order_relaxed)) == kPresentGuidesRequired
        && (!cached_real_guides || !current_guides)) {
      fg_service::ResetNativeFrame(&stream);
      stream.result = fg::Result::NOT_SERVED;
      native_fg_marker.store(nullptr, std::memory_order_release);
      diagnostic.reason = "mfg_guides_withdrawn";
      return nullptr;
    }
  } else {
#if defined(NR_NEXUS_DEV_CONTROL)
    if (native_edit_source) {
      if (stream.native_edit_history && stream.native_edit_frame != 0
          && native_edit_sequence != stream.native_edit_frame + 1) {
        for (auto& role : stream.roles) ResetPresentStreamHistory(&role);
      }
      stream.native_edit_frame = native_edit_sequence;
      stream.native_edit_source_epoch = native_frame.epoch;
    }
#endif
    // A failed or invalidated pair cannot become a second attempt on the same
    // game frame by changing its pass count or disabling the experiment.
    diagnostic.reason = "frame_already_attempted_or_nonmonotonic";
    if (stream.native_pair_attempt_frame != 0 && frame_id <= stream.native_pair_attempt_frame) return nullptr;
    bool protect_recording = resolution_pair;
#if defined(NR_NEXUS_DEV_CONTROL)
    protect_recording |= native_edit_source;
#endif
    if (protect_recording) {
      diagnostic.reason = "first_index_not_one";
      if (index != 1) return nullptr;
      if (resolution_pair) stream.native_pair_attempt_frame = frame_id;
    }
#if defined(NR_NEXUS_DEV_CONTROL)
    if (native_edit_source) {
      // Latch this base's own Reset before freezing the epoch below. Record
      // must not mistake that expected change for an external invalidation.
      FeatureState game_frame;
      game_frame.frame_reset = GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_Reset);
      ObserveGameFrame(game_frame);
    }
#endif
    // Only the same stream's adjacent, queue-proven recording can keep the
    // service active while this list records. A skipped recording breaks it.
    const auto display_epoch = history_reset_epoch.load(std::memory_order_relaxed);
    stream.continuing_submission = stream.native_serial != 0
        && stream.native_serial == native_fg_recording_serial.load(std::memory_order_acquire)
        && stream.native_serial == native_fg_counted_serial.load(std::memory_order_relaxed)
        && stream.native_display_epoch == display_epoch;
    stream.native_serial = 0;
    native_fg_marker.store(nullptr, std::memory_order_release);
    native_fg_result.store(fg::Result::PREPARED, std::memory_order_release);
    stream.cached = nullptr;
    stream.result = fg::Result::NOT_SERVED;
    fg_service::Slot* slot = nullptr;
    for (auto& candidate : stream.slots) {
      if (submission::ResourceReleasable(candidate.color[0])
          && submission::ResourceReleasable(candidate.color[1])
          && submission::ResourceReleasable(candidate.original_backbuffer)
          && submission::ResourceReleasable(candidate.original_hudless)
          && submission::ResourceReleasable(candidate.coupled_hudless)
          && submission::ResourceReleasable(candidate.coupled_ui)
          && submission::ResourceReleasable(candidate.color_heap)) {
        slot = &candidate;
        break;
      }
    }
    if (slot == nullptr) {
      stream.slots.emplace_back();
      slot = &stream.slots.back();
    }
    fg::FrameInput input;
    input.backbuffer = backbuffer;
    input.output = output_framerate::MatchNativeSwapchain(reinterpret_cast<uintptr_t>(owner->native_output_identity));
    input.hudless = hudless;
    input.backbuffer_state = input_state;
    input.hudless_state = input_state;
    input.depth_state = input_state;
    input.motion_state = input_state;
    input.ui_state = input_state;
    input.depth = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_Depth);
    input.motion = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_MVecs);
    if (GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames) != 0) {
      input.depth = nullptr;
      input.motion = nullptr;
    }
    input.ui = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_UIAlpha);
    if (input.ui == nullptr) input.ui = PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_UI);
    input.color_width = static_cast<uint32_t>(desc.Width);
    input.color_height = desc.Height;
    if (input.motion != nullptr && input.depth != nullptr) {
      input.guide_width = GetUInt(parameters, "DLSSG.MVecsSubrectWidth", static_cast<uint32_t>(input.motion->GetDesc().Width));
      input.guide_height = GetUInt(parameters, "DLSSG.MVecsSubrectHeight", input.motion->GetDesc().Height);
    }
    input.motion_x = GetUInt(parameters, "DLSSG.MVecsSubrectBaseX");
    input.motion_y = GetUInt(parameters, "DLSSG.MVecsSubrectBaseY");
    input.depth_x = GetUInt(parameters, "DLSSG.DepthSubrectBaseX");
    input.depth_y = GetUInt(parameters, "DLSSG.DepthSubrectBaseY");
    // FG's declared factors already convert motion to pixels. Only the
    // explicit normalized override applies another resource-size factor.
    const bool normalized_override = present_mvec_units.load() == 1;
    input.motion_scale_x = GetFloat(parameters, NVSDK_NGX_DLSSG_Parameter_MvecScaleX, 1.f)
        * (normalized_override ? static_cast<float>(input.guide_width) : 1.f);
    input.motion_scale_y = GetFloat(parameters, NVSDK_NGX_DLSSG_Parameter_MvecScaleY, 1.f)
        * (normalized_override ? static_cast<float>(input.guide_height) : 1.f);
    input.init_flags = (GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_DepthInverted) != 0
        ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0)
        | (GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_MvecJittered) != 0
            ? NVSDK_NGX_DLSS_Feature_Flags_MVJittered : 0)
        | (input.guide_width < input.color_width ? NVSDK_NGX_DLSS_Feature_Flags_MVLowRes : 0);
    input.reset = GetUInt(parameters, NVSDK_NGX_DLSSG_Parameter_Reset) != 0;
    input.jitter_x = GetFloat(parameters, "DLSSG.JitterOffsetX");
    input.jitter_y = GetFloat(parameters, "DLSSG.JitterOffsetY");
    input.application_frame = ++stream.application_frame;
    // A non-UIR integration can supply an opaque scene as its UI texture;
    // the texture's presence alone does not justify one-model composition.
    const auto uir = native_fg_ui_recomposition.find(handle);
    const bool ui_recomposition = uir != native_fg_ui_recomposition.end() && uir->second;
    diagnostic.reason = "private_surfaces_unavailable";
    if (!fg_service::PrepareSurfaces(&stream, slot, input,
            fg_service::NeedsNativeColorCoupling(input, parameters, ui_recomposition),
            ui_recomposition)) return nullptr;
    if (protect_recording) {
      // Cover feature-only and partial warm-up recordings as well as a final
      // image. Discarding any such recording invalidates its GPU state ledger.
      stream.native_resolution_pair = resolution_pair;
      stream.native_pair_producer_marker = slot->original_backbuffer;
      stream.native_pair_recording = submission::RecordingIdentity(list);
      stream.native_pair_completion = {};
      stream.native_pair_completion_value = 0;
      stream.native_pair_discarded = false;
      stream.native_pair_replayed = false;
      submission::TrackUse(list, stream.native_pair_producer_marker);
    }
    bool output_ready = false;
    std::array<NrRecording, 2> recordings;
    PresentTarget target = owner->prefg_target;
    target.native_prefg_resolution_pair = resolution_pair;
#if defined(NR_NEXUS_DEV_CONTROL)
    target.game_frame_observed = native_edit_source;
    // Coupled opaque-UI inputs retain their current per-frame service. This
    // first native route preserves exact-alpha composition or independent roles.
    if (native_edit_source && !fg_service::NeedsNativeColorCoupling(input, parameters, ui_recomposition)) {
      target.base_frame = native_base_frame;
    }
#endif
    const auto result = fg_service::Record(&stream, slot, list, input, target,
                                          &output_ready, &recordings, parameters, ui_recomposition);
    diagnostic.recordings = recordings;
    // These commands belong to the game's list and cannot be rolled back.
    // Private colors preserve both game surfaces on any partial failure;
    // discontinuous model history is rebuilt before the next application frame.
    if (!slot->pipeline_pending) {
      envelope.MarkInjectionCommandsRecorded();
    }
#if defined(NR_NEXUS_DEV_CONTROL)
    if (native_edit_source) {
      const auto current_frame = (native_tag_frame ? native_tag_source::CurrentFrame(hudless, list, native_frame.epoch)
          : fg::native_options::CurrentFrame(reinterpret_cast<uintptr_t>(owner->native_output_identity)));
      if (!current_frame.available || current_frame.epoch != native_frame.epoch
          || current_frame.generation != native_frame.generation || current_frame.frame != native_frame.frame) {
        fg_service::Invalidate(&stream);
        diagnostic.reason = "native_frame_changed_during_record";
        return nullptr;
      }
    }
#endif
    if (protect_recording && (configuration != configuration_generation.load()
        || options_generation != native_fg_options_generation.load()
        || display_epoch != history_reset_epoch.load()
        || (resolution_pair && !experimental_native_prefg_resolution.load())
        || !same_resolutions(pair_resolutions, std::array{CurrentPassResolution(0), CurrentPassResolution(1)}))) {
      fg_service::Invalidate(&stream);
      diagnostic.reason = "configuration_changed_during_record";
      return nullptr;
    }
    if (!output_ready) {
      for (const auto& recording : recordings) {
        ResolveAdaptiveDelivery(recording.adaptive_delivery, false);
      }
      diagnostic.reason = (result == fg::Result::PREPARED ? "nr_group_warming_or_incomplete" : "nr_group_failed");
      if (result != fg::Result::PREPARED) fg_service::Invalidate(&stream);
      for (uint32_t i = 0; i < recordings.size(); ++i) {
        if (recordings[i].model_invoked) ResetPresentStreamHistory(&stream.roles[i]);
      }
      stream.result = result;
      display_recording.result = result;
      return nullptr;
    }
    // MFG lists share this application image read-only. Save raw B once so
    // their recordings cannot overwrite a snapshot used by another queue.
    fg_service::CopyColor(list, slot->original_backbuffer, input_state, backbuffer, input_state);
    stream.cached = slot;
    if (slot->adaptive_delivery.frame.session != nullptr) {
      slot->adaptive_recording = submission::RecordingIdentity(list);
    }
    stream.native_resolution_pair = resolution_pair;
    stream.native_pair_resolutions = pair_resolutions;
    if (!protect_recording) {
      // This output supersedes the protected producer without inheriting its receipt.
      stream.native_pair_producer_marker = nullptr;
      stream.native_pair_recording = {};
      stream.native_pair_completion = {};
      stream.native_pair_completion_value = 0;
      stream.native_pair_discarded = false;
      stream.native_pair_replayed = false;
    }
    stream.source_backbuffer = backbuffer;
    stream.source_hudless = hudless;
    stream.source_ui = ui;
#if defined(NR_NEXUS_DEV_CONTROL)
    stream.native_edit_ui_alpha = ui_alpha;
#endif
    stream.source_frame_id = frame_id;
    stream.source_sdk_decision = sdk_decision;
    stream.result = fg::Result::PREPARED;
    stream.native_serial = native_fg_recording_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    stream.native_display_epoch = display_epoch;
    native_fg_marker.store(slot->color[0], std::memory_order_release);
  }
  native_fg_display_epoch.store(stream.native_display_epoch, std::memory_order_relaxed);
  native_fg_continuing_submission.store(stream.continuing_submission, std::memory_order_release);
  native_fg_result.store(fg::Result::PREPARED, std::memory_order_release);
  fg_service::CopyColor(list, backbuffer, input_state, stream.cached->color[0],
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (backbuffer_replaced != nullptr) {
    *backbuffer_replaced = true;
  }
  envelope.MarkInjectionCommandsRecorded();
  if (hudless != nullptr && hudless != backbuffer) {
    auto* replacement = (stream.cached->color_inputs_prepared
        ? stream.cached->coupled_hudless : stream.cached->color[1]);
    submission::TrackUse(list, replacement);
    Transition(list, replacement, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, input_state);
    NVSDK_NGX_Parameter_SetVoidPointer(const_cast<NVSDK_NGX_Parameter*>(parameters),
        NVSDK_NGX_DLSSG_Parameter_HUDLess, replacement);
  }
  if (stream.cached->color_inputs_prepared) {
    submission::TrackUse(list, stream.cached->coupled_ui);
    NVSDK_NGX_Parameter_SetVoidPointer(const_cast<NVSDK_NGX_Parameter*>(parameters),
                                      NVSDK_NGX_DLSSG_Parameter_UI, stream.cached->coupled_ui);
  }
  display_recording.preserve_current = true;
  diagnostic.reason = (repeated_frame || index > 1 ? "mfg_cached_output_recorded" : "group_output_recorded");
  return (hudless != backbuffer ? hudless : nullptr);
}

inline void RestoreNativePreFg(ID3D12GraphicsCommandList* list,
                               const NVSDK_NGX_Parameter* parameters, ID3D12Resource* hudless, bool succeeded,
                               const NVSDK_NGX_Handle* handle, bool backbuffer_replaced) {
  if (InsideDirectCall() || parameters == nullptr) return;
  RuntimeLock lock(runtime_mutex);
  auto context = reinterpret_cast<uintptr_t>(parameters);
  if (handle != nullptr) {
    const auto binding = fg_service::native_bindings.find(handle);
    if (binding == fg_service::native_bindings.end()) return;
    context = binding->second;
  }
  const auto it = fg_service::streams.find({true, context});
  if (it == fg_service::streams.end()) return;
  auto& stream = it->second;
  if (succeeded && backbuffer_replaced && stream.cached != nullptr) {
    present_frame_policy::Publish(&stream.base_frame_clock, stream.cached->base_frame, true, true);
  }
  // Evaluate returning does not end the SDK's real-frame consumption of B.
  // Keep successful NR pixels; rollback only this invocation's failed work.
  if (!succeeded && backbuffer_replaced && list != nullptr && stream.cached != nullptr
      && stream.cached->original_backbuffer != nullptr && stream.source_backbuffer != nullptr) {
    constexpr auto input_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    fg_service::CopyColor(list, stream.source_backbuffer, input_state,
                          stream.cached->original_backbuffer, input_state);
  }
  if (succeeded && backbuffer_replaced && stream.cached != nullptr && handle != nullptr) {
    const auto* owner = reinterpret_cast<const PresentStream*>(context);
    if (native_sdk_present_input.identity != nullptr
        && native_sdk_present_input.identity == owner->native_output_identity) {
      native_sdk_present_input = {.identity = native_sdk_present_input.identity,
          .decision_id = native_sdk_present_input.decision_id,
          .owner = owner, .route_id = owner->native_route_id,
          .backbuffer = stream.source_backbuffer, .marker = stream.cached->color[0], .serial = stream.native_serial,
          .guides_unavailable = native_sdk_present_input.guides_unavailable};
    }
  }
  if (hudless != nullptr && stream.cached != nullptr) {
    NVSDK_NGX_Parameter_SetVoidPointer(const_cast<NVSDK_NGX_Parameter*>(parameters),
                                      NVSDK_NGX_DLSSG_Parameter_HUDLess, hudless);
    if (stream.cached->color_inputs_prepared) {
      NVSDK_NGX_Parameter_SetVoidPointer(const_cast<NVSDK_NGX_Parameter*>(parameters),
                                        NVSDK_NGX_DLSSG_Parameter_UI, stream.source_ui);
    }
  }
  bool adaptive_rejected = false;
  if (backbuffer_replaced && list != nullptr && stream.cached != nullptr
      && stream.cached->adaptive_delivery.frame.session != nullptr
      && stream.cached->adaptive_recording == submission::RecordingIdentity(list)) {
    const auto delivery = std::exchange(stream.cached->adaptive_delivery, {});
    const bool restored = (hudless == nullptr || PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess) == hudless)
        && (!stream.cached->color_inputs_prepared || PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_UI) == stream.source_ui);
    if (succeeded && restored) {
      StageAdaptiveBorrowedDelivery(list, delivery);
    } else {
      ResolveAdaptiveDelivery(delivery, false);
      adaptive_rejected = delivery.frame.enabled;
    }
  }
  // Older internal consumers return the borrowed HUDLess without the BB flag.
  // A declined invocation owns neither binding and cannot retract another base.
  if (!succeeded && (backbuffer_replaced || hudless != nullptr)) {
    if (native_sdk_present_input.owner == reinterpret_cast<const PresentStream*>(context)) {
      const auto input = native_sdk_present_input;
      native_sdk_present_input = {.identity = input.identity, .decision_id = input.decision_id,
          .owner = (input.guides_unavailable ? input.owner : nullptr),
          .route_id = (input.guides_unavailable ? input.route_id : 0),
          .backbuffer = (input.guides_unavailable ? input.backbuffer : nullptr),
          .guides_unavailable = input.guides_unavailable};
    }
    native_fg_continuing_submission.store(false, std::memory_order_release);
    if (adaptive_rejected) {
      fg_service::ResetNativeFrame(&stream);
    } else {
      fg_service::Invalidate(&stream);
    }
    stream.result = fg::Result::FAILED;
    native_fg_marker.store(nullptr, std::memory_order_release);
    native_fg_result.store(fg::Result::FAILED, std::memory_order_release);
  }
}
