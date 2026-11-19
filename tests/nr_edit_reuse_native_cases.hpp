/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include <DirectXPackedVector.h>

#include <edit_reuse.hpp>
#include <native_fg_frame.hpp>

// Included after the native host, shared readbacks and edit-model substitutes.
namespace nr_edit_reuse_native_cases {
namespace addon = renodx::addons::dlss5;
namespace frame_source = addon::fg::native_options::frame_source;
namespace tag_source = native::native_tag_source;
using stage_execution_cases::Capture;
using stage_execution_cases::Snapshot;

struct FakeFrameToken final : sl::FrameToken {
  uint32_t frame = 0;
  operator uint32_t() const override { return frame; }
};

int Run() {
  ComPtr<ID3D12InfoQueue> info;
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    Check(renodx::utils::directx::Initialize(), "native edit reuse DirectX entry points");
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
    }
    constexpr uint32_t width = 16, height = 16;
    Gpu gpu(false, width, height);
    model_gpu = &gpu;
    gpu.device.As(&info);
    Check(info != nullptr, "native edit requires the D3D12 debug InfoQueue");
    UINT64 checked_messages = 0;
    const auto check_debug = [&] {
      if (info != nullptr) {
        while (checked_messages < info->GetNumStoredMessages()) {
          SIZE_T bytes = 0;
          info->GetMessage(checked_messages, nullptr, &bytes);
          std::vector<uint8_t> storage(bytes);
          auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
          Check(SUCCEEDED(info->GetMessage(checked_messages++, message, &bytes)), "native edit debug message");
          Check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
        }
      }
      Check(SUCCEEDED(gpu.device->GetDeviceRemovedReason()), "native edit WARP device remains healthy");
    };
    model_calls = 0;
    model_resets.clear();
    model_evaluated_handles.clear();
    model_released_handles.clear();
    fail_call = 0;
    Check(!native::edit_reuse_fg_experiment.load(), "native Pre-FG edit experiment defaults off");
    native::enabled = true;
    native::hooks_enabled = true;
    native::shutdown_quiesce_enabled = false;
    native::hook_point = native::kHookPresent;
    native::present_pre_fg = 1;
    native::present_guides = native::kPresentGuidesRequired;
    native::present_ui_correction = false;
    native::OnNativeFgOptions(true);
    native::stack_passes = 1;
    native::pass_stage[0] = 0;
    native::nr_follow_input_res = false;
    native::nr_resolution_scale = 1.f;
    native::nr_adaptive_enabled = false;
    native::codec_mode = 3;
    native::feed_mode = 1;
    native::norm_governor = 0;
    native::look_mode = false;
    native::detail_stability = 1;
    native::control_mask_enabled = false;
    native::use_auto_mask = false;
    native::cost_meter_enabled = false;
    native::gpu_timers_enabled = false;
    native::transfer_mode = 0;
    native::style_highlight_guard = 0.f;
    native::chroma_clamp_stops = 2.f;
    native::diffuse_white_nits = 200.f;
    native::transfer_strength = 1.f;
    native::color_strength = 1.f;
    native::intensity = 1.f;
    native::preset = 2;
    native::edit_reuse_enabled = true;
    native::edit_reuse_fg_experiment = true;
    native::direct_api.private_instance = true;
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = CreateModel;
    native::direct_api.evaluate = nr_edit_reuse_gpu_cases::NeutralModel;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::direct_runtime_requirements.dual_models = true;
    native::direct_runtime_requirements.file_version = "310.9.16.0";
    native::ngx_loader_staging = {};
    native::ngx_loader_staging.nr_ready = true;
    native::ngx_loader_staging.native_ready = true;
    native::ngx_loader_staging.native_api = native::direct_api;
    native::ngx_loader_staging.nr_requirements = native::direct_runtime_requirements;
    native::PublishNgxLoaderSymbols();
    native::parameter_runtime_ready = true;
    native::parameter_runtime_via_core = false;
    native::core_allocate_parameters = addon::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = addon::bridge::DestroyOwnedParameters;
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "native edit production Execute hook");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "native edit production command-list hooks");
    Check(native::InstallMixedPresentCopyHooks(gpu.list.Get()), "native tag-copy production CopyResource observer");
    const auto submit = [&] {
      // Let production Execute hooks attach the proof; Gpu::Submit also
      // reserves a synthetic host proof and is deliberately not used here.
      Check(SUCCEEDED(gpu.list->Close()), "native edit Close");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      gpu.submission_pending = true;
      gpu.queue->ExecuteCommandLists(1, lists);
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "native edit Fence Signal");
      const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr, "native edit completion event");
      const auto armed = gpu.fence->SetEventOnCompletion(gpu.serial, event);
      const auto waited = (SUCCEEDED(armed) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
      CloseHandle(event);
      Check(waited == WAIT_OBJECT_0 && gpu.fence->GetCompletedValue() >= gpu.serial,
          "native edit real bounded Fence completion");
      gpu.submission_pending = false;
      check_debug();
      Check(SUCCEEDED(gpu.allocator->Reset()), "native edit allocator Reset");
      Check(SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "native edit list Reset");
      // Keep the production callback that saves the exact completed native
      // producer receipt before its list generation is pruned.
      {
        const addon::RuntimeLock lock(native::runtime_mutex);
        native::PruneCompletedSubmissionUses();
      }
      gpu.uploads.clear();
      gpu.retained_resources.clear();
      {
        const addon::RuntimeLock lock(native::runtime_mutex);
        native::DrainRetiredResources(true);
      }
    };
    submit();
    const auto upload = [&](ID3D12Resource* resource, const auto& values, D3D12_RESOURCE_STATES before) {
      auto transfer = gpu.CreateTextureTransfer(resource);
      gpu.Transition(resource, before, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.UploadTexture(resource, transfer, reinterpret_cast<const uint8_t*>(values.data()),
          values.size() * sizeof(values[0]), transfer.row_bytes);
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, NATIVE_INPUT_STATE);
    };
    auto backbuffer = gpu.Texture(width, height, DXGI_FORMAT_R10G10B10A2_UNORM);
    auto hudless = gpu.Texture(width, height, DXGI_FORMAT_R10G10B10A2_UNORM);
    auto hudless_clone = gpu.Texture(width, height, DXGI_FORMAT_R10G10B10A2_UNORM);
    auto ui = gpu.Texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto depth = gpu.Texture(width, height, DXGI_FORMAT_R32G8X24_TYPELESS);
    auto motion = gpu.Texture(width, height, DXGI_FORMAT_R16G16_FLOAT);
    auto output = gpu.Swapchain(DXGI_FORMAT_R10G10B10A2_UNORM);
    auto& owner = native::present_streams[nullptr];
    owner.device_identity = renodx::utils::directx::NativeIdentity(gpu.device.Get());
    owner.native_output_identity = renodx::utils::directx::NativeIdentity(output.Get());
    owner.backbuffer_width = width;
    owner.backbuffer_height = height;
    owner.backbuffer_format = DXGI_FORMAT_R10G10B10A2_UNORM;
    owner.prefg_target_valid = true;
    auto& target = owner.prefg_target;
    native::SnapshotPresentRestoreConfiguration(&target);
    target.private_signed_runtime = true;
    target.hdr_mode = 2;
    target.units = {renodx::dlss5::codec::Encoding::Pq, true, 10000.f, true};
    target.hdr_codec_mode = 3;
    target.hdr_curve = 1;
    target.hdr_white_nits = 200.f;
    target.hdr_encoding = 4;
    target.hdr_dark_gate = native::V6DarkGate(target.units);
    target.hdr_pedestal_cap = renodx::dlss5::codec::BlackPedestalCap(target.units);
    target.diagnostic.bypass_host_look = true;
    target.ui_correction = false;
    target.pass_transfer_strength[0] = 1.f;
    target.pass_color_strength[0] = 1.f;
    addon::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, backbuffer.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, hudless.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_UI, static_cast<ID3D12Resource*>(nullptr));
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_UIAlpha, ui.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_UserInterfaceRecompositionEnabled, 1u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Depth, depth.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecs, motion.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth, width);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, height);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleX, 1.f);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MvecScaleY, 1.f);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MvecJittered, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, 0u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, 2u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_JitterOffsetX, 0.f);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_JitterOffsetY, 0.f);
    unsigned long long absent_frame_id = 0;
    Check(NVSDK_NGX_FAILED(parameters.Get(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, &absent_frame_id)),
        "native fixture has no numeric NGX FrameID");
    const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xF044});
    native::RegisterCreatedFeatureLocked(handle, NVSDK_NGX_Feature_FrameGeneration, &parameters);
    native::native_fg_owner = handle;
    native::native_fg_owns = true;
    const auto context = reinterpret_cast<uintptr_t>(&owner);
    const auto output_identity = reinterpret_cast<uintptr_t>(owner.native_output_identity);
    ComPtr<ID3D12CommandAllocator> proof_allocator;
    ComPtr<ID3D12GraphicsCommandList> proof_list;
    Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&proof_allocator)))
            && SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, proof_allocator.Get(),
                nullptr, IID_PPV_ARGS(&proof_list)))
            && native::InstallCommandListStateHooks(proof_list.Get()),
        "tag-copy proof query uses a separate real recording");
    frame_source::Activate();
    std::vector<uint32_t> source_back(width * height), source_hudless(width * height);
    std::vector<uint8_t> source_ui(width * height * 4);
    bool initialized = false;
    bool clone_initialized = false;
    enum class TagCopy { NONE, SUCCESS, FAILED, OVERWRITTEN, DIRECT, DIRECT_AMBIGUOUS };
    struct Request {
      uint32_t token = 0;
      bool reset = false;
      uint32_t index = 1;
      bool source_scope = true;
      bool fg_success = true;
      bool update_source = true;
      bool reject_motion = false;
      TagCopy tag_copy = TagCopy::NONE;
      bool owner_ready = true;
    };
    struct Observation {
      bool replaced = false;
      uint32_t evaluates = 0;
      uint64_t reuses = 0;
      int reset = -1;
      std::vector<uint8_t> consumed_back;
      std::vector<uint8_t> consumed_hudless;
      std::vector<uint8_t> restored_back;
    };
    const auto record = [&](const Request& request) {
      if (request.update_source) {
        for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
          const uint32_t x = pixel % width;
          const uint32_t delta = request.token % 3;
          const uint32_t back_rgb = (x < 2 ? (1u << 10) | (2u << 20)
              : (460u + delta) | ((500u + delta) << 10) | ((530u + delta) << 20));
          const uint32_t hudless_rgb = (x < 2 ? (1u << 10) | (2u << 20)
              : (440u + delta) | ((480u + delta) << 10) | ((510u + delta) << 20));
          source_back[pixel] = back_rgb | (((pixel + request.token) % 4) << 30);
          source_hudless[pixel] = hudless_rgb | (((pixel + request.token + 1) % 4) << 30);
          source_ui[pixel * 4] = 64;
          source_ui[pixel * 4 + 1] = 96;
          source_ui[pixel * 4 + 2] = 128;
          source_ui[pixel * 4 + 3] = std::array<uint8_t, 4>{0, 64, 128, 255}[(x + request.token) % 4];
        }
        std::vector<uint16_t> motion_values(width * height * 2, 0);
        if (request.reject_motion) {
          for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
            if (pixel % width >= width / 2) {
              motion_values[pixel * 2] = DirectX::PackedVector::XMConvertFloatToHalf(32.f);
            }
          }
        }
        const auto before = (initialized ? NATIVE_INPUT_STATE : D3D12_RESOURCE_STATE_COMMON);
        upload(backbuffer.Get(), source_back, before);
        upload(hudless.Get(), source_hudless, before);
        upload(ui.Get(), source_ui, before);
        upload(depth.Get(), std::vector<float>(width * height, .5f), before);
        upload(motion.Get(), motion_values, before);
        initialized = true;
      }
      const bool direct_tag = request.tag_copy == TagCopy::DIRECT || request.tag_copy == TagCopy::DIRECT_AMBIGUOUS;
      auto* input_hudless = (request.tag_copy != TagCopy::NONE && !direct_tag ? hudless_clone.Get() : hudless.Get());
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, input_hudless);
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_Reset, request.reset ? 1u : 0u);
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, request.index);
      Observation observed;
      const auto before_calls = model_calls;
      const auto before_reuses = native::edit_reuse_reuses.load();
      Snapshot consumed_back, consumed_hudless, restored_back, original_hudless, unchanged_ui;
      const auto consume = [&] {
        ID3D12Resource* original = nullptr;
        {
          const addon::RuntimeLock lock(native::runtime_mutex);
          if (request.owner_ready) {
            original = native::RunNativePreFg(gpu.list.Get(), &parameters, handle, &observed.replaced);
          } else {
            const auto streams_before = native::fg_service::streams.size();
            const auto features_before = native::features.size();
            const auto creates_before = native::nr_features_created;
            const auto evaluates_before = model_calls;
            const auto claims_before = tag_source::Snapshot().claim_count;
            const auto& existing = native::fg_service::streams.at({true, context});
            auto* cached_before = existing.cached;
            const auto serial_before = existing.native_serial;
            const auto frame_before = existing.application_frame;
            const bool owner_valid = owner.prefg_target_valid;
            owner.prefg_target_valid = false;
            try {
              original = native::RunNativePreFg(gpu.list.Get(), &parameters, handle, &observed.replaced);
            } catch (...) {
              owner.prefg_target_valid = owner_valid;
              throw;
            }
            owner.prefg_target_valid = owner_valid;
            Check(original == nullptr && !observed.replaced && model_calls == evaluates_before
                    && native::features.size() == features_before && native::nr_features_created == creates_before
                    && native::fg_service::streams.size() == streams_before
                    && native::fg_service::streams.contains({true, context}),
                "owner-unavailable direct consumption creates neither NR work nor a native service stream");
            const auto& unchanged = native::fg_service::streams.at({true, context});
            Check(unchanged.cached == cached_before && unchanged.native_serial == serial_before
                    && unchanged.application_frame == frame_before,
                "owner-unavailable consumption cannot create or advance the previous output cache");
            const auto claimed = tag_source::Snapshot();
            const auto exact = tag_source::CurrentFrame(input_hudless, gpu.list.Get(), claimed.claim_epoch);
            Check(claimed.claim_count == claims_before + 1 && claimed.claim_epoch != 0
                    && std::strcmp(claimed.claim_reason, "direct_claimed") == 0
                    && exact.available && exact.frame == request.token && exact.epoch == claimed.claim_epoch
                    && !tag_source::CurrentFrame(input_hudless, gpu.list.Get()).available,
                "real index-one consumption claims its unique direct tag even before an NR owner is ready");
            Check(!submission::ResourceReleasable(input_hudless)
                    && submission::OnlyRecordingUses(input_hudless, submission::RecordingIdentity(gpu.list.Get())),
                "owner-unavailable direct claim tracks the actual original resource in its consuming recording");
          }
        }
        auto* bound_hudless = native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess);
        if (observed.replaced) {
          Check(original == input_hudless && bound_hudless != input_hudless && bound_hudless != backbuffer.Get(),
              "native FG consumes a private HUDLess result and returns the borrowed original binding");
        } else {
          Check(original == nullptr && bound_hudless == input_hudless,
              "unprepared native input keeps its original HUDLess binding");
        }
        Check(native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_UIAlpha) == ui.Get(),
            "native exact alpha stays bound at FG consumption");
        consumed_back = Capture(gpu, backbuffer.Get(), NATIVE_INPUT_STATE);
        consumed_hudless = Capture(gpu, bound_hudless, NATIVE_INPUT_STATE);
        native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, request.fg_success, handle, observed.replaced);
        Check(native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess) == input_hudless
                && native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_UI) == nullptr
                && native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_UIAlpha) == ui.Get(),
            "native FG success/failure restores original HUDLess/UI/alpha bindings");
        restored_back = Capture(gpu, backbuffer.Get(), NATIVE_INPUT_STATE);
        original_hudless = Capture(gpu, input_hudless, NATIVE_INPUT_STATE);
        unchanged_ui = Capture(gpu, ui.Get(), NATIVE_INPUT_STATE);
      };
      if (request.tag_copy != TagCopy::NONE) {
        const auto producer_thread = GetCurrentThreadId();
        if (request.update_source) {
          if (direct_tag) {
            // UntilPresent does not create a clone producer. Complete the
            // source uploads before the SDK receives the unchanged resource.
            submit();
          }
          const uint32_t pending_tags = (request.tag_copy == TagCopy::DIRECT_AMBIGUOUS ? 2u : 1u);
          for (uint32_t pending = 0; pending < pending_tags; ++pending) {
            FakeFrameToken frame;
            frame.frame = request.token + pending;
            Check(frame_source::ObserveMarker(+[](sl::PCLMarker, const sl::FrameToken&) { return sl::Result::eOk; },
                      sl::PCLMarker::ePresentStart, frame) == sl::Result::eOk,
                "tag producer enters the actual successful Reflex marker observer");
            {
              const frame_source::SdkScope sdk_scope(output_identity);
              sl::Resource resource(sl::ResourceType::eTex2d, hudless.Get(), NATIVE_INPUT_STATE);
              const sl::ResourceTag tag(&resource, sl::kBufferTypeHUDLessColor,
                  direct_tag ? sl::ResourceLifecycle::eValidUntilPresent : sl::ResourceLifecycle::eOnlyValidNow);
              const sl::ViewportHandle viewport(0u);
              tag_source::Scope tag_scope(gpu.list.Get(), static_cast<uint32_t>(frame),
                  static_cast<uint32_t>(viewport), &tag, 1);
              if (!direct_tag) {
                gpu.Transition(hudless.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
                gpu.Transition(hudless_clone.Get(), clone_initialized ? NATIVE_INPUT_STATE : D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_COPY_DEST);
                // Only the installed production CopyResource observer may bind
                // this destination to the tag. Do not call ObserveCopy directly.
                gpu.list->CopyResource(hudless_clone.Get(), hudless.Get());
                gpu.Transition(hudless_clone.Get(), D3D12_RESOURCE_STATE_COPY_DEST, NATIVE_INPUT_STATE);
                gpu.Transition(hudless.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
                clone_initialized = true;
              }
              tag_scope.Finish(request.tag_copy != TagCopy::FAILED);
            }
            Check(frame_source::ObserveMarker(+[](sl::PCLMarker, const sl::FrameToken&) { return sl::Result::eOk; },
                      sl::PCLMarker::ePresentEnd, frame) == sl::Result::eOk,
                "producer Present and Reflex scopes finish before deferred NGX consumption");
            if (request.tag_copy == TagCopy::DIRECT_AMBIGUOUS && pending == 0) {
              uint64_t unclaimed_epoch = 0;
              {
                const addon::RuntimeLock lock(native::runtime_mutex);
                const auto unique = tag_source::CurrentFrame(input_hudless, gpu.list.Get());
                Check(unique.available && unique.frame == request.token
                        && std::strcmp(unique.reason, "direct_tag_unique_unclaimed") == 0,
                    "first direct tag is uniquely pending before a recorded overwrite");
                unclaimed_epoch = unique.epoch;
              }
              gpu.Transition(backbuffer.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
              gpu.Transition(hudless.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_DEST);
              gpu.list->CopyResource(hudless.Get(), backbuffer.Get());
              gpu.Transition(hudless.Get(), D3D12_RESOURCE_STATE_COPY_DEST, NATIVE_INPUT_STATE);
              gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
              source_hudless = source_back;
              const auto overwrite_recording = submission::ObserveRecording(gpu.list.Get());
              Check(overwrite_recording != nullptr && !overwrite_recording->submitted.load(),
                  "the intervening direct-resource overwrite is recorded without a GPU submission");
              {
                const addon::RuntimeLock lock(native::runtime_mutex);
                const auto overwritten = tag_source::CurrentFrame(input_hudless, gpu.list.Get());
                Check(!overwritten.available && overwritten.epoch == unclaimed_epoch
                        && std::strcmp(overwritten.reason, "direct_tag_unclaimed_overwrite") == 0,
                    "unsubmitted overwrite invalidates but retains the unclaimed direct token");
              }
            }
          }
          if (!direct_tag) {
            {
              const addon::RuntimeLock lock(native::runtime_mutex);
              Check(!tag_source::CurrentFrame(hudless_clone.Get(), proof_list.Get()).available,
                  "an unsubmitted or failed tag-copy cannot authorize a foreign recording");
            }
            submit();
          }
          if (request.tag_copy == TagCopy::OVERWRITTEN) {
            gpu.Transition(backbuffer.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            gpu.Transition(hudless_clone.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_DEST);
            gpu.list->CopyResource(hudless_clone.Get(), backbuffer.Get());
            gpu.Transition(hudless_clone.Get(), D3D12_RESOURCE_STATE_COPY_DEST, NATIVE_INPUT_STATE);
            gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
            submit();
          }
        }
        std::exception_ptr consumer_failure;
        std::thread consumer([&] {
          try {
            Check(GetCurrentThreadId() != producer_thread
                    && !addon::fg::native_options::CurrentFrame(output_identity).available,
                "deferred NGX consumes on another thread with no producer TLS ticket");
            {
              const addon::RuntimeLock lock(native::runtime_mutex);
              uint64_t claimed_epoch = 0;
              if (request.index > 1) {
                const auto source = native::fg_service::streams.find({true, context});
                Check(source != native::fg_service::streams.end(), "repeated MFG has an existing native owner");
                claimed_epoch = source->second.native_edit_source_epoch;
                Check(claimed_epoch != 0, "repeated MFG queries the production pair's claimed source epoch");
              }
              const auto tagged = tag_source::CurrentFrame(input_hudless, gpu.list.Get(), claimed_epoch);
              const bool expected = request.tag_copy == TagCopy::SUCCESS || request.tag_copy == TagCopy::DIRECT;
              Check(tagged.available == expected && (!expected || tagged.frame == request.token),
                  "only a successful unique direct tag or unchanged completed clone retains its actual FrameToken");
              if (request.tag_copy == TagCopy::DIRECT_AMBIGUOUS) {
                Check(std::strcmp(tagged.reason, "tagged_resource_tokens_ambiguous") == 0,
                    "two direct tokens are refused specifically for ambiguity rather than a missing source");
              }
            }
            consume();
          } catch (...) {
            consumer_failure = std::current_exception();
          }
        });
        consumer.join();
        if (consumer_failure != nullptr) std::rethrow_exception(consumer_failure);
      } else if (request.source_scope) {
        FakeFrameToken token;
        token.frame = request.token;
        Check(frame_source::ObserveMarker(+[](sl::PCLMarker, const sl::FrameToken&) { return sl::Result::eOk; },
                  sl::PCLMarker::ePresentStart, token) == sl::Result::eOk,
            "real Reflex observer sees successful PresentStart");
        const frame_source::SdkScope scope(output_identity);
        const auto ticket = addon::fg::native_options::CurrentFrame(output_identity);
        Check(ticket.available && ticket.frame == request.token, "SDK scope exposes this producer token");
        consume();
      } else {
        Check(!addon::fg::native_options::CurrentFrame(output_identity).available,
            "missing SDK source scope cannot expose an old token");
        consume();
      }
      observed.evaluates = model_calls - before_calls;
      observed.reuses = native::edit_reuse_reuses.load() - before_reuses;
      observed.reset = (observed.evaluates != 0 ? model_resets.back() : -1);
      submit();
      observed.consumed_back = consumed_back.Pixels();
      observed.consumed_hudless = consumed_hudless.Pixels();
      observed.restored_back = restored_back.Pixels();
      const auto original_hudless_pixels = original_hudless.Pixels();
      const auto& current_hudless = (request.tag_copy == TagCopy::OVERWRITTEN ? source_back : source_hudless);
      Check(original_hudless_pixels.size() == current_hudless.size() * sizeof(uint32_t)
              && std::memcmp(original_hudless_pixels.data(), current_hudless.data(), current_hudless.size() * sizeof(uint32_t)) == 0,
          "FG never mutates the game's original HUDLess pixels");
      Check(unchanged_ui.Pixels() == source_ui, "FG never mutates original UI/alpha pixels");
      return observed;
    };
    const auto matches = [](const std::vector<uint8_t>& pixels, const std::vector<uint32_t>& source) {
      return pixels.size() == source.size() * sizeof(uint32_t)
          && std::memcmp(pixels.data(), source.data(), pixels.size()) == 0;
    };
    uint32_t token = 100;
    bool direct_warmup_mfg_checked = false;
    const auto recover = [&](TagCopy tag_copy = TagCopy::NONE) {
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 12; ++attempt) {
        const auto observed = record({.token = ++token, .reset = true, .tag_copy = tag_copy});
        Check(observed.reuses == 0, "native recovery cannot reuse an invalidated edit anchor");
        if (tag_copy == TagCopy::DIRECT && !direct_warmup_mfg_checked && !observed.replaced) {
          uint64_t claimed_epoch = 0, native_frame = 0;
          {
            const addon::RuntimeLock lock(native::runtime_mutex);
            const auto source = native::fg_service::streams.find({true, context});
            Check(source != native::fg_service::streams.end() && source->second.native_edit_source
                    && source->second.native_edit_source_is_tagged && source->second.native_edit_source_epoch != 0,
                "direct warmup claims its tagged source before a final HUDLess cache exists");
            claimed_epoch = source->second.native_edit_source_epoch;
            native_frame = source->second.native_edit_frame;
          }
          const auto warmup_mfg = record({.token = token, .index = 2, .update_source = false, .tag_copy = TagCopy::DIRECT});
          Check(!warmup_mfg.replaced && warmup_mfg.evaluates == 0 && warmup_mfg.reuses == 0,
              "MFG immediately after direct index-one warmup does not record another NR evaluation");
          {
            const addon::RuntimeLock lock(native::runtime_mutex);
            const auto source = native::fg_service::streams.find({true, context});
            Check(source != native::fg_service::streams.end() && source->second.native_edit_source
                    && source->second.native_edit_source_is_tagged
                    && source->second.native_edit_source_epoch == claimed_epoch
                    && source->second.native_edit_frame == native_frame,
                "direct warmup MFG preserves the exact claimed epoch and tagged source mode");
          }
          direct_warmup_mfg_checked = true;
        }
        if (observed.replaced && observed.evaluates == 1) return;
      }
      Check(false, "bounded native warmup reaches a one-model exact-alpha refresh");
    };
    recover();
    const auto refreshed = record({.token = ++token, .reset = true});
    Check(refreshed.replaced && refreshed.evaluates == 1 && refreshed.reuses == 0 && refreshed.reset == 1,
        "native refresh uses exactly one HUDLess model and current exact-alpha composition");
    auto& stream = native::fg_service::streams.at({true, context});
    const auto* role_handle = stream.roles[1].Handle();
    auto anchor = native::features.at(role_handle).slots[0].edit_reuse;
    Check(stream.native_edit_source && stream.native_edit_frame == static_cast<uint64_t>(token) + 1
            && anchor != nullptr && anchor->valid && target.xefg_edit_reuse_frame == 0,
        "native service derives a private edit anchor from the observed token without a XeFG marker");
    for (const auto& binding : anchor->bindings) {
      for (const auto& resource : binding.resources) {
        Check(resource.Get() != backbuffer.Get() && resource.Get() != hudless.Get(),
            "native edit descriptors retain private staged originals rather than game color inputs");
      }
    }
    const auto anchor_generation = anchor->anchor_generation;
    const auto model_frame = stream.last_model_frame[1];
    const auto reused = record({.token = ++token});
    Check(reused.replaced && reused.evaluates == 0 && reused.reuses == 1
            && matches(reused.consumed_back, source_back) && matches(reused.consumed_hudless, source_hudless),
        "native reuse delivers this frame's changed PQ colors and both current packed alpha planes exactly");
    Check(native::features.at(role_handle).slots[0].edit_reuse == anchor
            && anchor->anchor_generation == anchor_generation && stream.last_model_frame[1] == model_frame
            && stream.last_output_frame[1] == stream.application_frame,
        "native reuse advances delivered output without advancing the model or edit anchor");
    const auto application_frame = stream.application_frame;
    const auto native_frame = stream.native_edit_frame;
    const auto duplicated = record({.token = token, .index = 2, .update_source = false});
    Check(duplicated.replaced && duplicated.evaluates == 0 && duplicated.reuses == 0
            && stream.application_frame == application_frame && stream.native_edit_frame == native_frame
            && anchor->anchor_generation == anchor_generation,
        "repeated MFG consumption of one real token advances neither NR, reuse, source cadence nor cache");
    const auto next = record({.token = ++token});
    Check(next.replaced && next.evaluates == 1 && next.reuses == 0 && next.reset == 0,
        "next real native token refreshes after reuse without a false history Reset");
    token += 2;
    const auto gap = record({.token = token});
    Check(gap.replaced && gap.evaluates == 1 && gap.reuses == 0 && gap.reset == 1,
        "a real native token gap refreshes with Reset instead of borrowing adjacent-frame motion");
    const auto reset = record({.token = ++token, .reset = true});
    Check(reset.replaced && reset.evaluates == 1 && reset.reuses == 0 && reset.reset == 1,
        "explicit native Reset interrupts the x2 reuse phase");
    anchor.reset();

    {
      Check(native::EnsureLookPipeline(gpu.device.Get()), "warm Look pipelines before Adaptive admission");
      const auto fixed_anchor = native::features.at(role_handle).slots[0].edit_reuse;
      const auto resident = std::make_shared<addon::adaptive_resolution::Session>();
      native::adaptive_resident_session = resident;
      native::nr_adaptive_enabled = true;
      native::nr_adaptive_selected_levels = 2;
      const auto fixed = record({.token = ++token});
      const auto& fixed_role = native::features.at(role_handle);
      Check(fixed.replaced && fixed.evaluates == 0 && fixed.reuses == 1
              && fixed_role.adaptive_cache == nullptr && fixed_role.slots[0].edit_reuse == fixed_anchor
              && fixed_role.input_width == width && fixed_role.input_height == height,
          "an unacquired Adaptive request retains the current native fixed HUDLess workset and reuses its anchor");
      const auto fixed_mfg = record({.token = token, .index = 2, .update_source = false});
      Check(fixed_mfg.replaced && fixed_mfg.evaluates == 0 && fixed_mfg.reuses == 0,
          "MFG re-delivers the identified fixed output without advancing edit history");

      native::adaptive_resident_session.reset();
      const auto next_mfg = record({.token = token + 1, .index = 2, .update_source = false});
      Check(!next_mfg.replaced && next_mfg.evaluates == 0 && next_mfg.reuses == 0,
          "a new base token cannot re-deliver the previous fixed slot while Adaptive can acquire residency");
      const auto preparing = record({.token = ++token});
      Check(preparing.reuses == 0 && stream.adaptive_frame.slot == 0
              && native::features.at(role_handle).adaptive_cache != nullptr
              && native::features.at(role_handle).slots[0].edit_reuse == nullptr,
          "actual Adaptive preparation stays on the upper tier and retires the fixed edit anchor");
      for (uint32_t attempt = 0; attempt < 16 && !stream.adaptive_session->TargetReady(); ++attempt) {
        Check(record({.token = ++token}).reuses == 0, "preparing Adaptive tiers cannot publish x2 reuse");
      }
      Check(stream.adaptive_session->TargetReady() && stream.adaptive_session->ladder.count == 2,
          "two native Adaptive tiers become ready through normal recording and Fence completion");
      stream.adaptive_session->Request(1, native::SteadyNowNs());
      const auto switched = record({.token = ++token});
      const auto& switched_role = native::features.at(role_handle);
      const auto switched_size = stream.adaptive_session->ladder.sizes[1][0];
      Check(switched.replaced && switched.evaluates > 0 && switched.reuses == 0
              && stream.adaptive_frame.slot == 1 && switched_role.adaptive_cache != nullptr
              && switched_role.adaptive_cache->active == 1
              && switched_role.input_width == switched_size.width && switched_role.input_height == switched_size.height
              && switched_size.width < width && switched_role.slots[0].edit_reuse == nullptr,
          "native Adaptive switch delivers the actual smaller HUDLess tier without creating an edit anchor");
      const auto switched_mfg = record({.token = token, .index = 2, .update_source = false});
      Check(!switched_mfg.replaced && switched_mfg.evaluates == 0 && switched_mfg.reuses == 0,
          "an Adaptive output without a conventional frame identity cannot use the fixed x2 duplicate path");
      const auto switched_cache = switched_role.adaptive_cache;
      const auto* switched_slot = stream.cached;
      std::vector<std::pair<addon::FenceRef, uint64_t>> proofs;
      Check(stream.native_pair_producer_marker == switched_slot->original_backbuffer
              && submission::UseSubmissionTokens(switched_slot->original_backbuffer, &proofs)
                  == submission::UseSubmission::kComplete
              && !stream.native_pair_completion.empty(),
          "the pruned Adaptive producer retains its completed Fence receipt");
      // Keep the old color slot busy without recording another use of its producer marker.
      native::fg_service::CopyColor(proof_list.Get(), hudless_clone.Get(), D3D12_RESOURCE_STATE_COMMON,
          switched_slot->color[0], NATIVE_INPUT_STATE);
      const auto no_token = record({.token = ++token, .source_scope = false});
      Check(no_token.replaced && no_token.evaluates > 0 && no_token.reuses == 0
              && stream.cached != switched_slot && stream.source_frame_id == 0
              && native::features.at(role_handle).adaptive_cache == switched_cache && switched_cache->active == 1,
          "withdrawing the SDK token keeps the completed Adaptive tier and its ordinary per-frame delivery");
      Check(SUCCEEDED(proof_list->Close()) && SUCCEEDED(proof_list->Reset(proof_allocator.Get(), nullptr)),
          "discard the foreign color recording after the tokenless producer rotates slots");
      const auto next_no_token = record({.token = ++token, .source_scope = false});
      Check(next_no_token.replaced && next_no_token.evaluates > 0 && next_no_token.reuses == 0
              && native::features.at(role_handle).adaptive_cache == switched_cache && switched_cache->active == 1,
          "second tokenless Adaptive frame evaluates and delivers after the prior producer was pruned");
      Check(stream.native_pair_producer_marker == nullptr && stream.native_pair_completion.empty(),
          "ordinary tokenless delivery leaves no protected producer receipt");
      native::nr_adaptive_enabled = false;
      native::nr_adaptive_selected_levels = 0;
      recover();
      const auto& recovered_role = native::features.at(role_handle);
      const auto recovered_anchor = recovered_role.slots[0].edit_reuse;
      Check(recovered_role.adaptive_cache == nullptr && recovered_anchor != nullptr && recovered_anchor->valid,
          "fixed admission after Adaptive and missing tokens creates a fresh anchor");
      const auto recovered_reuse = record({.token = ++token});
      Check(recovered_reuse.replaced && recovered_reuse.evaluates == 0 && recovered_reuse.reuses == 1
              && native::features.at(role_handle).slots[0].edit_reuse == recovered_anchor,
          "the next fixed token reuses the recovered anchor");
      const auto recovered_mfg = record({.token = token, .index = 2, .update_source = false});
      Check(recovered_mfg.replaced && recovered_mfg.evaluates == 0 && recovered_mfg.reuses == 0,
          "the recovered fixed producer retains its exact pruned receipt for repeated MFG");
    }

    native::direct_api.evaluate = nr_edit_reuse_xefg_cases::EditedModel;
    nr_edit_reuse_xefg_cases::model_pixels.resize(width * height * 4);
    for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
      for (uint32_t channel = 0; channel < 3; ++channel) {
        nr_edit_reuse_xefg_cases::model_pixels[pixel * 4 + channel]
            = DirectX::PackedVector::XMConvertFloatToHalf(.75f - .05f * channel);
      }
      nr_edit_reuse_xefg_cases::model_pixels[pixel * 4 + 3] = DirectX::PackedVector::XMConvertFloatToHalf(1.f);
    }
    Check(record({.token = ++token, .reset = true}).evaluates == 1, "refresh a non-neutral native edit anchor");
    const auto rejected = record({.token = ++token, .reject_motion = true});
    Check(rejected.replaced && rejected.evaluates == 0 && rejected.reuses == 1,
        "non-neutral native reuse executes without another model evaluation");
    bool edited_valid_region = false;
    for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
      uint32_t actual_back = 0, actual_hudless = 0;
      std::memcpy(&actual_back, rejected.consumed_back.data() + pixel * sizeof(uint32_t), sizeof(actual_back));
      std::memcpy(&actual_hudless, rejected.consumed_hudless.data() + pixel * sizeof(uint32_t), sizeof(actual_hudless));
      Check((actual_back >> 30) == (source_back[pixel] >> 30)
              && (actual_hudless >> 30) == (source_hudless[pixel] >> 30),
          "native refresh/reuse composition preserves each current two-bit color alpha");
      if (pixel % width >= width / 2) {
        Check(actual_back == source_back[pixel] && actual_hudless == source_hudless[pixel],
            "motion-rejected native region selects exact current original color and alpha");
      } else if (pixel % width >= 2) {
        edited_valid_region |= (actual_hudless & 0x3fffffffu) != (source_hudless[pixel] & 0x3fffffffu);
      }
    }
    Check(edited_valid_region, "native rejection check also exercises accepted non-neutral edit pixels");
    const auto failed = record({.token = ++token, .reset = true, .fg_success = false});
    Check(failed.replaced && failed.evaluates == 1 && failed.reuses == 0
            && !matches(failed.consumed_back, source_back) && matches(failed.restored_back, source_back)
            && native::fg_service::streams.at({true, context}).cached == nullptr,
        "FG failure rolls back the edited BB and invalidates the cache after restoring HUDLess/UI bindings");
    native::direct_api.evaluate = nr_edit_reuse_gpu_cases::NeutralModel;
    recover();
    const auto no_scope = record({.token = ++token, .source_scope = false});
    Check(no_scope.reuses == 0 && !native::fg_service::streams.at({true, context}).native_edit_source,
        "missing native SDK scope returns to the existing full-rate path and cannot consume a previous edit anchor");
    recover();
    native::OnNativeFgOptions(false);
    const auto off = record({.token = ++token});
    Check(!off.replaced && off.evaluates == 0 && off.reuses == 0
            && matches(off.restored_back, source_back),
        "explicit native FG Off retains the original safe path without cached edits");
    native::OnNativeFgOptions(true);
    recover();
    {
      const addon::RuntimeLock lock(native::runtime_mutex);
      tag_source::Reset();
    }
    recover(TagCopy::SUCCESS);
    const auto tagged_refresh = record({.token = ++token, .reset = true, .tag_copy = TagCopy::SUCCESS});
    Check(tagged_refresh.replaced && tagged_refresh.evaluates == 1 && tagged_refresh.reuses == 0
            && tagged_refresh.reset == 1,
        "deferred native tag-copy produces one actual-model refresh after producer scopes end");
    auto tagged_anchor = native::features.at(role_handle).slots[0].edit_reuse;
    Check(tagged_anchor != nullptr && tagged_anchor->valid, "deferred native tag-copy establishes a private edit anchor");
    const auto tagged_anchor_generation = tagged_anchor->anchor_generation;
    const auto tagged_reuse = record({.token = ++token, .tag_copy = TagCopy::SUCCESS});
    Check(tagged_reuse.replaced && tagged_reuse.evaluates == 0 && tagged_reuse.reuses == 1
            && matches(tagged_reuse.consumed_back, source_back)
            && matches(tagged_reuse.consumed_hudless, source_hudless),
        "a subsequent completed tag-copy reuses edits on another thread with current PQ colors and alpha");
    const auto tagged_application_frame = stream.application_frame;
    const auto tagged_native_frame = stream.native_edit_frame;
    const auto tagged_mfg = record({.token = token, .index = 2, .update_source = false, .tag_copy = TagCopy::SUCCESS});
    Check(tagged_mfg.replaced && tagged_mfg.evaluates == 0 && tagged_mfg.reuses == 0
            && stream.application_frame == tagged_application_frame && stream.native_edit_frame == tagged_native_frame
            && tagged_anchor->anchor_generation == tagged_anchor_generation,
        "deferred MFG reconsumes the same tagged clone without advancing NR or cache cadence");
    const auto tagged_next = record({.token = ++token, .tag_copy = TagCopy::SUCCESS});
    Check(tagged_next.replaced && tagged_next.evaluates == 1 && tagged_next.reuses == 0 && tagged_next.reset == 0,
        "next real tagged token refreshes after deferred reuse without a false history Reset");
    tagged_anchor.reset();
    for (const auto denied : {TagCopy::FAILED, TagCopy::OVERWRITTEN}) {
      const auto fallback = record({.token = ++token, .tag_copy = denied});
      Check(!fallback.replaced && fallback.evaluates == 0 && fallback.reuses == 0
              && matches(fallback.restored_back, source_back),
          "failed or overwritten tag-copy rejects the old edit cache and preserves the current original BB");
      recover(TagCopy::SUCCESS);
    }
    {
      const addon::RuntimeLock lock(native::runtime_mutex);
      tag_source::Reset();
    }
    const auto owner_unavailable = record({.token = ++token, .tag_copy = TagCopy::DIRECT, .owner_ready = false});
    Check(!owner_unavailable.replaced && owner_unavailable.evaluates == 0 && owner_unavailable.reuses == 0
            && matches(owner_unavailable.consumed_back, source_back)
            && matches(owner_unavailable.restored_back, source_back),
        "direct FG consumption before owner readiness leaves current color intact without NR or cache output");
    recover(TagCopy::DIRECT);
    Check(direct_warmup_mfg_checked, "direct source completes preparation after an observed warmup MFG turn");
    const auto direct_refresh = record({.token = ++token, .reset = true, .tag_copy = TagCopy::DIRECT});
    Check(direct_refresh.replaced && direct_refresh.evaluates == 1 && direct_refresh.reuses == 0
            && direct_refresh.reset == 1,
        "UntilPresent directly tagged original HUDLess refreshes on the deferred NGX thread without a clone");
    auto direct_anchor = native::features.at(role_handle).slots[0].edit_reuse;
    Check(direct_anchor != nullptr && direct_anchor->valid, "a directly tagged native input establishes its edit anchor");
    const auto direct_anchor_generation = direct_anchor->anchor_generation;
    const auto direct_reuse = record({.token = ++token, .tag_copy = TagCopy::DIRECT});
    Check(direct_reuse.replaced && direct_reuse.evaluates == 0 && direct_reuse.reuses == 1
            && matches(direct_reuse.consumed_back, source_back)
            && matches(direct_reuse.consumed_hudless, source_hudless),
        "next unique direct FrameToken reuses the current original HUDLess colors and alpha");
    const auto direct_application_frame = stream.application_frame;
    const auto direct_native_frame = stream.native_edit_frame;
    const auto direct_epoch = stream.native_edit_source_epoch;
    const auto direct_mfg = record({.token = token, .index = 2, .update_source = false, .tag_copy = TagCopy::DIRECT});
    Check(direct_mfg.replaced && direct_mfg.evaluates == 0 && direct_mfg.reuses == 0
            && stream.application_frame == direct_application_frame && stream.native_edit_frame == direct_native_frame
            && stream.native_edit_source_epoch == direct_epoch && direct_anchor->anchor_generation == direct_anchor_generation,
        "direct-tag MFG keeps its claimed epoch without advancing NR, source cadence or cache");
    const auto direct_next = record({.token = ++token, .tag_copy = TagCopy::DIRECT});
    Check(direct_next.replaced && direct_next.evaluates == 1 && direct_next.reuses == 0 && direct_next.reset == 0,
        "the next real direct FrameToken refreshes after reuse without a false history Reset");
    direct_anchor.reset();
    const auto ambiguous_direct = record({.token = ++token, .tag_copy = TagCopy::DIRECT_AMBIGUOUS});
    Check(!ambiguous_direct.replaced && ambiguous_direct.evaluates == 0 && ambiguous_direct.reuses == 0
            && matches(ambiguous_direct.restored_back, source_back),
        "a recorded overwrite cannot make a later direct token unique; ambiguity preserves the current BB");
    ++token;
    {
      const addon::RuntimeLock lock(native::runtime_mutex);
      tag_source::Reset();
    }
    recover(TagCopy::DIRECT);
#if defined(NR_NEXUS_DEV_CONTROL)
    {
      using Json = native::dev_control::Json;
      const auto control = [](const char* action) {
        return native::dev_control::Execute(
            {{"id", "warp-ui-alpha"}, {"op", "native_ui_alpha_preflight"}, {"action", action}});
      };
      const auto events_since = [](size_t start) {
        constexpr std::string_view prefix = "NR_UIALPHA ";
        std::vector<Json> events;
        for (size_t i = start; i < captured_logs.size(); ++i) {
          const std::string_view message = captured_logs[i].second;
          if (message.starts_with(prefix)) events.push_back(Json::parse(message.substr(prefix.size())));
        }
        return events;
      };
      auto alpha = gpu.Texture(width, height, DXGI_FORMAT_R8_UNORM);
      auto alpha_clone = gpu.Texture(width, height, DXGI_FORMAT_R8_UNORM, NATIVE_INPUT_STATE);
      const std::vector<uint8_t> alpha_pixels(width * height, 0x93);
      upload(alpha.Get(), alpha_pixels, D3D12_RESOURCE_STATE_COMMON);
      const auto alpha_identity = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(alpha.Get()));
      const auto clone_identity = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(alpha_clone.Get()));

      struct TagRequest {
        uint32_t copies = 0;
        bool partial = false;
        sl::Result result = sl::Result::eOk;
      } sdk_request;
      constexpr uint32_t alpha_viewport = 17;
      uint32_t events_before_tag = 0;
      uint32_t forwarded_tags = 0;
      const auto sdk_tag = [&](const sl::FrameToken& frame, const sl::ViewportHandle& viewport,
                               const sl::ResourceTag* tags, uint32_t count, sl::CommandBuffer* command_buffer) {
        Check(static_cast<uint32_t>(frame) == token && static_cast<uint32_t>(viewport) == alpha_viewport
                && count == 1 && tags != nullptr && tags[0].type == sl::kBufferTypeUIAlpha
                && tags[0].resource != nullptr && tags[0].resource->native == alpha.Get()
                && command_buffer == gpu.list.Get(),
            "original SL tag receives the unmodified R8 resource and list");
        auto* list = static_cast<ID3D12GraphicsCommandList*>(command_buffer);
        for (uint32_t copy = 0; copy < sdk_request.copies; ++copy) {
          gpu.Transition(alpha.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
          gpu.Transition(alpha_clone.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_DEST);
          if (sdk_request.partial) {
            const D3D12_TEXTURE_COPY_LOCATION from{.pResource = alpha.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            const D3D12_TEXTURE_COPY_LOCATION to{.pResource = alpha_clone.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            const D3D12_BOX box{0, 0, 0, width - 1, height, 1};
            list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
          } else {
            list->CopyResource(alpha_clone.Get(), alpha.Get());
          }
          gpu.Transition(alpha_clone.Get(), D3D12_RESOURCE_STATE_COPY_DEST, NATIVE_INPUT_STATE);
          gpu.Transition(alpha.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
        }
        Check(control("status").at("events") == events_before_tag, "Alpha tag stays unpublished inside original SL call");
        ++forwarded_tags;
        return sdk_request.result;
      };

      auto preflight_parameters = parameters;
      ID3D12Resource* expected_alpha = nullptr;
      const auto check_parameters = [&](const NVSDK_NGX_Parameter* block) {
        Check(block == &preflight_parameters
                && native::GetD3D12Resource(block, NVSDK_NGX_DLSSG_Parameter_UIAlpha) == expected_alpha
                && native::GetD3D12Resource(block, NVSDK_NGX_DLSSG_Parameter_UI) == nullptr
                && native::GetD3D12Resource(block, NVSDK_NGX_DLSSG_Parameter_Backbuffer) == backbuffer.Get()
                && native::GetD3D12Resource(block, NVSDK_NGX_DLSSG_Parameter_HUDLess) == hudless.Get()
                && native::GetD3D12Resource(block, NVSDK_NGX_DLSSG_Parameter_Depth) == depth.Get()
                && native::GetD3D12Resource(block, NVSDK_NGX_DLSSG_Parameter_MVecs) == motion.Get(),
            "Alpha preflight preserves the original NGX block and typed input roles");
      };
      uint32_t forwarded_evaluates = 0;
      const auto sdk_evaluate = [&](ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* feature,
                                    const NVSDK_NGX_Parameter* block) {
        Check(list == gpu.list.Get() && feature == handle, "original NGX receives the same FG handle and recording");
        check_parameters(block);
        ++forwarded_evaluates;
        return NVSDK_NGX_Result_Success;
      };
      static thread_local decltype(sdk_tag)* original_tag = nullptr;
      static thread_local decltype(sdk_evaluate)* original_evaluate = nullptr;
      funnel_accounting_tests::Restore substitutions;
      substitutions.Value(&original_tag);
      substitutions.Value(&original_evaluate);
      substitutions.Value(&native::real_streamline_set_tag_for_frame);
      substitutions.Value(&native::ngx_slot_real[0].evaluate);
      substitutions.Atomic(&native::enabled);
      substitutions.Atomic(&native::last_present_steady_ns);
      substitutions.actions.emplace_back([&] {
        const addon::RuntimeLock lock(native::runtime_mutex);
        native::streamline_viewports.erase(alpha_viewport);
      });
      original_tag = &sdk_tag;
      original_evaluate = &sdk_evaluate;
      native::real_streamline_set_tag_for_frame = +[](const sl::FrameToken& frame, const sl::ViewportHandle& viewport,
          const sl::ResourceTag* tags, uint32_t count, sl::CommandBuffer* command_buffer) {
        return (*original_tag)(frame, viewport, tags, count, command_buffer);
      };
      native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(+[](ID3D12GraphicsCommandList* list,
          const NVSDK_NGX_Handle* feature, const NVSDK_NGX_Parameter* block, PFN_NVSDK_NGX_ProgressCallback) {
        return (*original_evaluate)(list, feature, block);
      });
      native::enabled = false;
      native::last_present_steady_ns = native::SteadyNowNs();
      const auto armed = control("arm");
      Check(armed.at("ok") == true && armed.at("active") == true, "public control arms NR-off Alpha preflight");
      const auto before_model_calls = model_calls;

      const auto run_tag = [&](TagRequest request) {
        sdk_request = request;
        FakeFrameToken frame;
        frame.frame = ++token;
        const auto lifetime = (request.copies == 0 ? sl::ResourceLifecycle::eValidUntilPresent : sl::ResourceLifecycle::eOnlyValidNow);
        sl::Resource resource(sl::ResourceType::eTex2d, alpha.Get(), NATIVE_INPUT_STATE);
        const sl::ResourceTag tag(&resource, sl::kBufferTypeUIAlpha, lifetime);
        events_before_tag = control("status").at("events").get<uint32_t>();
        const auto start = captured_logs.size();
        Check(native::HookedStreamlineSetTagForFrame(frame, sl::ViewportHandle(alpha_viewport), &tag, 1, gpu.list.Get())
                  == request.result,
            "UIAlpha tag wrapper preserves the original SDK result");
        const auto events = events_since(start);
        Check(events.size() == 1, "UIAlpha tag emits one observation after the SDK return");
        const auto& event = events.front();
        const auto& role = event.at("roles").at(0);
        const bool success = request.result == sl::Result::eOk;
        Check(event.at("token") == frame.frame && event.at("generation") == armed.at("generation")
                && event.at("kind") == (success ? "tag" : "tag_failed")
                && event.at("result") == static_cast<int32_t>(request.result)
                && event.at("matching_copy_calls") == request.copies
                && event.at("evidence") == "recorded_CPU_arguments_only"
                && role.at("role") == "ui_alpha" && role.at("lifetime") == static_cast<uint32_t>(lifetime)
                && role.at("resource").at("identity") == alpha_identity,
            "UIAlpha tag preserves the token, original texture and CPU metadata");
        const auto& copy = event.at("native_copy");
        if (success && request.copies == 1 && !request.partial) {
          const auto recording = addon::submission::RecordingIdentity(gpu.list.Get());
          Check(copy.at("source") == alpha_identity && copy.at("destination") == clone_identity
                  && copy.at("subresource") == 0 && copy.at("recording").at("id") == recording.first
                  && copy.at("recording").at("generation") == recording.second,
              "successful unique full R8 copy records the actual source, destination and list generation");
        } else {
          Check(copy.is_null(), "duplicate, partial, failed and direct tags publish no copy version");
        }
      };
      const auto consume_alpha = [&](ID3D12Resource* resource, uintptr_t identity) {
        expected_alpha = resource;
        preflight_parameters.Set(NVSDK_NGX_DLSSG_Parameter_UIAlpha, resource);
        const auto start = captured_logs.size();
        Check(native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), handle, &preflight_parameters, nullptr)
                  == NVSDK_NGX_Result_Success,
            "UIAlpha preflight forwards the original NGX result");
        check_parameters(&preflight_parameters);
        const auto events = events_since(start);
        Check(events.size() == 2 && events[0].at("kind") == "consumer"
                && events[0].at("ui_alpha").at("identity") == identity
                && events[0].at("ui_alpha").at("format") == static_cast<uint32_t>(DXGI_FORMAT_R8_UNORM)
                && events[0].at("sdk_scope_token").is_null()
                && events[1].at("kind") == "consumer_return" && events[1].at("entry") == events[0].at("sequence"),
            "NGX records the original typed R8 input without assigning a token outside SDK scope");
      };

      run_tag({.copies = 1});
      consume_alpha(alpha_clone.Get(), clone_identity);
      const auto cloned_pixels = Capture(gpu, alpha_clone.Get(), NATIVE_INPUT_STATE);
      run_tag({.copies = 2});
      run_tag({.copies = 1, .partial = true});
      run_tag({.copies = 1, .result = sl::Result::eErrorComputeFailed});
      run_tag({});
      run_tag({});
      consume_alpha(alpha.Get(), alpha_identity);
      const auto stopped = control("stop");
      Check(stopped.at("ok") == true && stopped.at("active") == false && stopped.at("consumer_calls") == 2
              && forwarded_tags == 6 && forwarded_evaluates == 2 && model_calls == before_model_calls,
          "UIAlpha preflight is bounded and never invokes NR");
      submit();
      Check(cloned_pixels.Pixels() == alpha_pixels, "the original full R8 copy completes on WARP");
      std::cout << "PASS native UIAlpha preflight evidence=WARP+MOCK_SDK full_R8_readback=checked"
                   " copy_publication=checked direct_two_tokens=metadata_only NGX_parameters=unchanged\n";
    }
#endif
    auto final_anchor = native::features.at(role_handle).slots[0].edit_reuse;
    Check(final_anchor != nullptr, "recovered native output owns its final private cache");
    std::weak_ptr<addon::edit_reuse::State> retired_anchor = final_anchor;
    final_anchor.reset();
    frame_source::Stop();
    {
      const addon::RuntimeLock lock(native::runtime_mutex);
      tag_source::Reset();
      Check(!tag_source::CurrentFrame(hudless_clone.Get(), gpu.list.Get()).available,
          "tag-source retirement cannot expose a completed old clone ticket");
      Check(!tag_source::CurrentFrame(hudless.Get(), gpu.list.Get()).available,
          "tag-source retirement cannot expose an old directly tagged resource ticket");
      native::RetirePresentStream(nullptr);
      native::ForgetSourceHandleLocked(handle);
    }
    submit();
    Check(!native::fg_service::streams.contains({true, context}) && !native::features.contains(role_handle)
            && retired_anchor.expired() && native::retired_nr_features.empty()
            && native::retired_final_resources.empty(),
        "native source retirement releases all edit caches, models and worksets after actual Fence completion");
    native::edit_reuse_fg_experiment = false;
    Check(SUCCEEDED(proof_list->Close()), "close the unsubmitted foreign proof-query list");
    nr_edit_reuse_xefg_cases::model_pixels.clear();
    model_gpu = nullptr;
    check_debug();
    std::cout << "PASS native Pre-FG x2 evidence=WARP+MOCK format=HDR10-PQ roles=BB+HUDLess+UIAlpha"
                 " frame_source=Reflex_PresentStart_and_SDK_scope numeric_NGX_FrameID=absent"
                 " deferred_frame_source=tag_scope_MOCK_CopyResource_real cross_thread_after_PresentEnd=checked"
                 " deferred_refresh_reuse_refresh=1/0/1 tag_unsubmitted/failed/overwritten=denied"
                 " direct_UntilPresent_no_clone=checked direct_refresh_reuse_refresh=1/0/1 direct_pending_ambiguity=denied"
                 " direct_warmup_MFG_claim=stable"
                 " direct_unclaimed_recorded_overwrite=invalidated_and_retained"
                 " direct_owner_unavailable=claimed_and_tracked_without_NR"
                 " refresh_reuse_refresh=1/0/1 repeated_MFG=stable current_color_alpha=checked"
                 " adaptive_unacquired=0_evaluate/1_reuse adaptive_preparation_and_switch=no_x2"
                 " adaptive_tokenless_after_prune=two_delivered fixed_recovery=refresh/reuse/MFG"
                 " motion_rejection/reset/gap/off/missing_scope/FG_failure/retirement=checked"
                 " production_Execute_and_Fence=real NR=MOCK FG_consumer=readback no_game_or_visual_acceptance\n";
    return 0;
  } catch (const std::exception& error) {
    fail_call = 0;
    frame_source::Stop();
    {
      const addon::RuntimeLock lock(native::runtime_mutex);
      tag_source::Reset();
    }
    native::edit_reuse_fg_experiment = false;
    std::cerr << "nr-edit-reuse-native-contracts: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace nr_edit_reuse_native_cases
