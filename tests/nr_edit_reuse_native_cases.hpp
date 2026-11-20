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
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <DirectXPackedVector.h>

#include <edit_reuse.hpp>
#include <native_fg_frame.hpp>
#include <present_frame_policy.hpp>

// Included after the native host, shared readbacks and edit-model substitutes.
namespace nr_edit_reuse_native_cases {
namespace addon = renodx::addons::dlss5;
namespace frame_policy = native::present_frame_policy;
namespace tag_source = native::native_tag_source;
namespace native_adapter = addon::fg::native_options::internal;
namespace frame_source = addon::fg::native_options::frame_source;
namespace rate = addon::output_framerate;
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
#if defined(NR_NEXUS_DEV_CONTROL)
    Check(!native::edit_reuse_fg_experiment.load(), "native FG diagnostics default off");
#endif
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
    const auto* registered_tag = std::ranges::find_if(native::kStreamlineHooks, [](const auto& hook) {
      return std::string_view(hook.name) == "slSetTagForFrame";
    });
    Check(registered_tag != std::end(native::kStreamlineHooks) && registered_tag->detour != nullptr,
        "production Streamline hook inventory registers the per-frame tag entry");
    const auto set_tag_for_frame = reinterpret_cast<decltype(native::real_streamline_set_tag_for_frame)>(
        registered_tag->detour);
    const auto execute_recording = [&] {
      // Let production Execute hooks attach the proof; Gpu::Submit also
      // reserves a synthetic host proof and is deliberately not used here.
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
    };
    const auto retire_recording = [&] {
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
    const auto submit = [&](bool retain_recording = false) {
      Check(SUCCEEDED(gpu.list->Close()), "native edit Close");
      execute_recording();
      if (!retain_recording) {
        retire_recording();
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
    auto other_motion = gpu.Texture(width, height, DXGI_FORMAT_R16G16_FLOAT, NATIVE_INPUT_STATE);
    ComPtr<ReceiptSwapchain> output;
    output.Attach(new ReceiptSwapchain(nullptr));
    output->current_back_buffer = backbuffer.Get();
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
    const uintptr_t registered_buffers[]{reinterpret_cast<uintptr_t>(
        renodx::utils::directx::NativeIdentity(backbuffer.Get()))};
    rate::RegisterPresentOutput(context, reinterpret_cast<uintptr_t>(owner.device_identity),
        output_identity, registered_buffers);
    struct SdkConsumer {
      const void* context = nullptr;
      void (*evaluate)(const void*) = nullptr;
      HRESULT result = S_OK;
    };
    static SdkConsumer sdk_consumer;
    native_adapter::state.present = +[](IDXGISwapChain*, UINT, UINT, bool& skip) {
      skip = true;
      sdk_consumer.evaluate(sdk_consumer.context);
      return sdk_consumer.result;
    };
    native_adapter::decisions_ready = true;
    const auto* registered_present = std::ranges::find_if(native_adapter::PRESENT_HOOKS, [](const auto& hook) {
      return std::string_view(hook.name) == "slHookPresent";
    });
    Check(registered_present != std::end(native_adapter::PRESENT_HOOKS) && registered_present->detour != nullptr,
        "Native SDK installation registers the outer Present entry");
    const auto sdk_present = reinterpret_cast<native_adapter::Present>(registered_present->detour);
    const auto& registered_marker = native_adapter::REFLEX_MARKER_HOOK[0];
    Check(std::string_view(registered_marker.name) == "slReflexSetMarker" && registered_marker.detour != nullptr,
        "Native installation registers the Reflex PresentStart producer in this build");
    const auto sdk_marker = reinterpret_cast<frame_source::SetMarker>(registered_marker.detour);
    static thread_local sl::Result original_marker_result = sl::Result::eOk;
    const auto mock_marker = +[](sl::PCLMarker, const sl::FrameToken&) { return original_marker_result; };
    funnel_accounting_tests::Restore marker_substitutions;
    marker_substitutions.Value(&native_adapter::reflex_marker.real);
    marker_substitutions.Value(&native_adapter::test_now_ns);
    native_adapter::reflex_marker.real = mock_marker;
    frame_source::Activate();
    const auto mark_start = [&](uint32_t token, sl::Result result = sl::Result::eOk) {
      FakeFrameToken frame;
      frame.frame = token;
      original_marker_result = result;
      Check(sdk_marker(sl::PCLMarker::ePresentStart, frame) == result,
          "the registered Reflex wrapper returns the actual PresentStart outcome");
    };
    const auto tag_sdk = [&](uint32_t token) {
      FakeFrameToken frame;
      frame.frame = token;
      sl::Resource resource(sl::ResourceType::eTex2d, backbuffer.Get(), NATIVE_INPUT_STATE);
      const sl::ResourceTag tag(&resource, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent);
      const auto original = std::exchange(native::real_streamline_set_tag_for_frame,
          +[](const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*) {
            return sl::Result::eOk;
          });
      const auto tagged = set_tag_for_frame(frame, sl::ViewportHandle(0u), &tag, 1, gpu.list.Get());
      native::real_streamline_set_tag_for_frame = original;
      Check(tagged == sl::Result::eOk, "the registered Streamline wrapper returns the actual Backbuffer tag result");
    };
    const auto run_sdk = [&](uint32_t token, auto&& evaluate, bool success = true, bool tag_backbuffer = true,
                             bool issue_start = true, UINT flags = 0, IDXGISwapChain* sdk_output = nullptr) {
      if (tag_backbuffer) {
        tag_sdk(token);
      }
      if (issue_start) {
        mark_start(token);
      }
      using Evaluate = std::remove_reference_t<decltype(evaluate)>;
      const auto previous = std::exchange(sdk_consumer, SdkConsumer{
          .context = &evaluate, .evaluate = +[](const void* context) { (*static_cast<const Evaluate*>(context))(); },
          .result = (success ? S_OK : E_FAIL)});
      bool skip = false;
      Check(sdk_present(sdk_output != nullptr ? sdk_output : output.Get(), 0, flags, skip) == sdk_consumer.result && skip,
          "the registered outer SDK wrapper forwards the consumer and its routing result");
      sdk_consumer = previous;
      if (previous.context == nullptr) {
        Check(!addon::fg::native_options::CurrentPresentFrame(reinterpret_cast<const void*>(output_identity),
                  backbuffer.Get()).available,
            "the real SDK Present scope cannot supply a deferred token after its return");
      }
    };
    constexpr uint32_t scope_token = 0x8aa;
    const auto read_sync_frame = [&] {
      return addon::fg::native_options::CurrentPresentFrame(owner.native_output_identity, backbuffer.Get());
    };
    run_sdk(scope_token + 1, [&] {
      const auto frame = read_sync_frame();
      Check(frame.available && frame.frame == scope_token + 1,
          "the current registered SDK entry binds a successful PresentStart without a Backbuffer frame tag");
      Check(!addon::fg::native_options::CurrentPresentFrame(other_motion.Get(), backbuffer.Get()).available
                && !addon::fg::native_options::CurrentPresentFrame(owner.native_output_identity, other_motion.Get()).available,
          "the live SDK entry refuses a different output or Backbuffer");
      bool worker_claimed = true;
      std::jthread worker([&] { worker_claimed = read_sync_frame().available; });
      worker.join();
      Check(!worker_claimed, "another thread cannot borrow the current SDK PresentStart");
      run_sdk(scope_token + 1, [&] {
        const auto nested = read_sync_frame();
        Check(nested.available && nested.epoch == frame.epoch,
            "a genuine nested SDK callback for the same output keeps the outer PresentStart");
      }, true, false, false);
    }, true, false);
    run_sdk(scope_token, [&] {
      Check(!read_sync_frame().available, "a successful Backbuffer tag and SDK entry cannot replace PresentStart");
    }, true, true, false);
    run_sdk(scope_token + 1, [&] {
      Check(!read_sync_frame().available, "the next SDK call cannot redeem a previously bound PresentStart");
    }, true, false, false);
    ComPtr<ReceiptSwapchain> other_output;
    other_output.Attach(new ReceiptSwapchain(nullptr));
    run_sdk(scope_token + 20, [&] {
      Check(read_sync_frame().available, "the original output owns its live SDK scope before foreign nesting");
      run_sdk(scope_token + 20, [&] {
        Check(!read_sync_frame().available, "a real nested SDK callback for another output cannot borrow the outer frame");
      }, true, false, false, 0, other_output.Get());
      Check(!read_sync_frame().available, "the conflicting output revokes the outer PresentStart before later consumption");
    }, true, false);
    mark_start(scope_token + 2, sl::Result::eErrorInvalidState);
    run_sdk(scope_token + 2, [&] {
      Check(!read_sync_frame().available, "a failed PresentStart cannot use an otherwise valid Backbuffer frame tag");
    }, true, true, false);
    mark_start(scope_token + 3);
    run_sdk(scope_token + 4, [&] {
      Check(!read_sync_frame().available, "a conflicting valid frame tag and PresentStart cannot identify the NGX input");
    }, true, true, false);
    tag_sdk(scope_token + 5);
    tag_sdk(scope_token + 6);
    run_sdk(scope_token + 6, [&] {
      Check(!read_sync_frame().available, "PresentStart cannot resolve competing successful tags for the current Backbuffer");
    }, true, false);
    mark_start(scope_token + 7);
    mark_start(scope_token + 8);
    run_sdk(scope_token + 8, [&] {
      Check(!read_sync_frame().available, "two competing starts before the real SDK entry cannot select the latest token");
    }, true, false, false);
    static const frame_source::SetMarker reenter_marker = sdk_marker;
    native_adapter::reflex_marker.real = +[](sl::PCLMarker marker, const sl::FrameToken& frame) {
      if (static_cast<uint32_t>(frame) == scope_token + 9) {
        FakeFrameToken overlap;
        overlap.frame = scope_token + 10;
        return reenter_marker(marker, overlap);
      }
      return sl::Result::eOk;
    };
    mark_start(scope_token + 9);
    native_adapter::reflex_marker.real = mock_marker;
    run_sdk(scope_token + 9, [&] {
      Check(!read_sync_frame().available, "overlapping successful registered marker writes cannot authorize the SDK entry");
    }, true, false, false);
    native_adapter::test_now_ns = native_adapter::NowNs();
    mark_start(scope_token + 11);
    native_adapter::test_now_ns += frame_source::PRESENT_START_TTL_NS + 1;
    run_sdk(scope_token + 11, [&] {
      Check(!read_sync_frame().available, "an expired successful PresentStart cannot enter a new SDK scope");
    }, true, false, false);
    run_sdk(scope_token + 12, [&] {
      Check(read_sync_frame().available, "the registered SDK entry binds a fresh PresentStart");
      native_adapter::test_now_ns += frame_source::PRESENT_START_TTL_NS + 1;
      Check(!read_sync_frame().available, "an expired PresentStart cannot authorize later consumption inside a slow SDK scope");
    }, true, false);
    native_adapter::test_now_ns = 0;
    run_sdk(scope_token + 13, [&] {
      Check(!read_sync_frame().available, "TEST Present cannot bind the successful PresentStart");
    }, true, true, true, DXGI_PRESENT_TEST);
    run_sdk(scope_token + 13, [&] {
      const auto frame = read_sync_frame();
      Check(frame.available && frame.frame == scope_token + 13, "the real SDK entry binds the unconsumed matching start and tag");
    }, true, false, false);
    std::cout << "PASS synchronous SDK frame evidence successful-PresentStart+real-entry;"
                 " failed/conflicting/overlapped/expired/cross-thread/test/wrong-output/wrong-bb denied\n";
    ComPtr<ID3D12CommandAllocator> proof_allocator;
    ComPtr<ID3D12GraphicsCommandList> proof_list;
    Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&proof_allocator)))
            && SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, proof_allocator.Get(),
                nullptr, IID_PPV_ARGS(&proof_list)))
            && native::InstallCommandListStateHooks(proof_list.Get()),
        "tag-copy proof query uses a separate real recording");
    std::vector<uint32_t> source_back(width * height), source_hudless(width * height);
    std::vector<uint8_t> source_ui(width * height * 4);
    bool initialized = false;
    bool clone_initialized = false;
    enum class TagCopy { NONE, SUCCESS, FAILED, OVERWRITTEN, DIRECT, DIRECT_AMBIGUOUS };
    enum class TagChange { NONE, WRITE_ALPHA, PRESENT_END, RETAG_HUDLESS, RETAG_ALPHA };
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
      bool with_ui_alpha = true;
      bool retain_recording = false;
      bool omit_alpha_tag = false;
      bool sdk_wrapper = false;
      TagChange tag_change = TagChange::NONE;
      bool overwrite_sync_motion = false;
      bool without_backbuffer_tag = false;
      bool check_pending_mfg = false;
    };
    struct Observation {
      bool replaced = false;
      uint32_t evaluates = 0;
      uint64_t reuses = 0;
      uint64_t source_generation = 0;
      int reset = -1;
      std::vector<uint8_t> consumed_back;
      std::vector<uint8_t> consumed_hudless;
      std::vector<uint8_t> restored_back;
    };
    struct FrameProgress {
      uint64_t base_frame;
      uint64_t source_frame;
      uint64_t sequence_period;
      frame_policy::SourceKind kind;
      uintptr_t source;
      uint64_t source_generation;
      uint64_t output_epoch;
      uint32_t pass_begin;
      uint32_t pass_count;
      uint32_t roles;
      bool reuse_edits;
      bool complete;
      uint64_t claims;
      uint64_t outputs;
      uint64_t native_frame;
      uint64_t native_source_epoch;
      uint64_t observed_frames;
      uint64_t reset_epoch;
      uint64_t delivery_serial;
      std::array<uint64_t, 2> model_frames;
      std::array<uint64_t, 2> output_frames;
      const native::fg_service::Slot* cached;
      const addon::edit_reuse::State* anchor;
      uint64_t anchor_generation;
      uint64_t anchor_sequence;
      uint64_t reuse_count;
      bool reused;
      bool operator==(const FrameProgress&) const = default;
    };
    const auto read_progress = [&] {
      const addon::RuntimeLock lock(native::runtime_mutex);
      const auto& stream = native::fg_service::streams.at({true, context});
      const auto& clock = stream.base_frame_clock;
      const auto role = native::features.find(stream.roles[1].Handle());
      const auto* anchor = (role != native::features.end() ? role->second.slots[0].edit_reuse.get() : nullptr);
      return FrameProgress{
          .base_frame = stream.application_frame,
          .source_frame = clock.frame.sequence,
          .sequence_period = clock.frame.sequence_period,
          .kind = clock.frame.kind,
          .source = clock.frame.source,
          .source_generation = clock.frame.generation,
          .output_epoch = clock.frame.output_epoch,
          .pass_begin = clock.frame.pass_begin,
          .pass_count = clock.frame.pass_count,
          .roles = clock.frame.roles,
          .reuse_edits = clock.frame.reuse_edits,
          .complete = clock.complete,
          .claims = clock.claims,
          .outputs = clock.outputs,
          .native_frame = stream.native_edit_frame,
          .native_source_epoch = stream.native_edit_source_epoch,
          .observed_frames = native::game_window.frames,
          .reset_epoch = native::history_reset_epoch.load(),
          .delivery_serial = stream.native_serial,
          .model_frames = {stream.last_model_frame[0], stream.last_model_frame[1]},
          .output_frames = {stream.last_output_frame[0], stream.last_output_frame[1]},
          .cached = stream.cached,
          .anchor = anchor,
          .anchor_generation = (anchor != nullptr ? anchor->anchor_generation : 0),
          .anchor_sequence = (anchor != nullptr ? anchor->source_sequence : 0),
          .reuse_count = (anchor != nullptr ? anchor->reuse_count : 0),
          .reused = anchor != nullptr && anchor->reused};
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
      auto* input_alpha = (request.with_ui_alpha ? ui.Get() : nullptr);
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, input_hudless);
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_UIAlpha, input_alpha);
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_Reset, request.reset ? 1u : 0u);
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, request.index);
      Observation observed;
      const auto before_calls = model_calls;
      const auto before_reuses = native::edit_reuse_reuses.load();
      Snapshot consumed_back, consumed_hudless, restored_back, original_hudless, unchanged_ui;
      const auto consume = [&] {
        ID3D12Resource* original = nullptr;
        ID3D12Resource* bound_hudless = nullptr;
        const auto read_sdk_inputs = [&] {
          bound_hudless = native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess);
          if (observed.replaced) {
            const auto& stream = native::fg_service::streams.at({true, context});
            const auto* cached = stream.cached;
            Check(cached != nullptr && cached->color[0] != nullptr && stream.source_backbuffer == backbuffer.Get()
                    && stream.source_hudless == input_hudless,
                "native FG publishes both output colors for the actual borrowed input pair");
            auto* delivered_hudless = (cached->color_inputs_prepared ? cached->coupled_hudless : cached->color[1]);
            Check(original == input_hudless && bound_hudless == delivered_hudless
                    && bound_hudless != input_hudless && bound_hudless != backbuffer.Get(),
                "native FG consumes the group's private HUDLess result and returns the borrowed original binding");
          } else {
            Check(original == nullptr && bound_hudless == input_hudless,
                "unprepared native input keeps its original HUDLess binding");
          }
          Check(native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_Backbuffer) == backbuffer.Get()
                  && native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_UI) == nullptr
                  && native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_UIAlpha) == input_alpha,
              "native FG keeps the BB, UI and alpha parameter bindings at consumption");
          if (request.check_pending_mfg) {
            const auto pending = read_progress();
            const auto calls = model_calls;
            auto duplicate = parameters;
            duplicate.Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, input_hudless);
            duplicate.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 2u);
            bool replaced = false;
            ID3D12Resource* borrowed = nullptr;
            {
              const addon::RuntimeLock lock(native::runtime_mutex);
              borrowed = native::RunNativePreFg(gpu.list.Get(), &duplicate, handle, &replaced);
            }
            native::RestoreNativePreFg(gpu.list.Get(), &duplicate, borrowed, true, handle, replaced);
            Check(observed.replaced && !pending.complete && !replaced && borrowed == nullptr
                    && model_calls == calls && read_progress() == pending,
                "a low-tier candidate in the first SDK call cannot be consumed or settled by a second call");
          }
          consumed_back = Capture(gpu, backbuffer.Get(), NATIVE_INPUT_STATE);
          consumed_hudless = Capture(gpu, bound_hudless, NATIVE_INPUT_STATE);
        };
        if (request.sdk_wrapper) {
          const auto result = (request.fg_success ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail);
          uint32_t original_calls = 0;
          const auto original_evaluate = [&](ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* feature,
                                             const NVSDK_NGX_Parameter* block) {
            Check(list == gpu.list.Get() && feature == handle && block == &parameters,
                "the actual NGX detour forwards the real command list, FG handle and parameter block");
            ++original_calls;
            observed.replaced = native::PreFgResource(block, NVSDK_NGX_DLSSG_Parameter_HUDLess) != input_hudless;
            if (observed.replaced) {
              original = input_hudless;
            }
            read_sdk_inputs();
            return result;
          };
          static thread_local const decltype(original_evaluate)* handler = nullptr;
          handler = &original_evaluate;
          auto* previous = std::exchange(native::ngx_slot_real[0].evaluate, reinterpret_cast<void*>(+[](
              ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* feature, const NVSDK_NGX_Parameter* block,
              PFN_NVSDK_NGX_ProgressCallback) { return (*handler)(list, feature, block); }));
          const auto forwarded = native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), handle, &parameters, nullptr);
          native::ngx_slot_real[0].evaluate = previous;
          handler = nullptr;
          Check(forwarded == result && original_calls == 1, "the detour returns exactly one original SDK outcome");
        } else {
          const addon::RuntimeLock lock(native::runtime_mutex);
          if (request.owner_ready) {
            original = native::RunNativePreFg(gpu.list.Get(), &parameters, handle, &observed.replaced);
          } else {
            const auto streams_before = native::fg_service::streams.size();
            const auto features_before = native::features.size();
            const auto creates_before = native::nr_features_created;
            const auto evaluates_before = model_calls;
            const auto unclaimed = tag_source::CurrentFrame(input_hudless, gpu.list.Get());
            const auto& existing = native::fg_service::streams.at({true, context});
            auto* cached_before = existing.cached;
            const auto serial_before = existing.native_serial;
            const auto frame_before = existing.application_frame;
            const auto common_claims_before = existing.base_frame_clock.claims;
            const auto common_outputs_before = existing.base_frame_clock.outputs;
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
                    && unchanged.application_frame == frame_before
                    && unchanged.base_frame_clock.claims == common_claims_before
                    && unchanged.base_frame_clock.outputs == common_outputs_before,
                "owner-unavailable consumption cannot create or advance the previous output cache");
            const auto exact = tag_source::CurrentFrame(input_hudless, gpu.list.Get(), unclaimed.epoch);
            Check(unclaimed.available && unclaimed.epoch != 0
                    && std::strcmp(exact.reason, "direct_tag_claimed") == 0
                    && exact.available && exact.frame == request.token && exact.epoch == unclaimed.epoch
                    && !tag_source::CurrentFrame(input_hudless, gpu.list.Get()).available,
                "real index-one consumption claims its unique direct tag even before an NR owner is ready");
            Check(!submission::ResourceReleasable(input_hudless)
                    && submission::OnlyRecordingUses(input_hudless, submission::RecordingIdentity(gpu.list.Get())),
                "owner-unavailable direct claim tracks the actual original resource in its consuming recording");
          }
        }
        if (!request.sdk_wrapper) {
          read_sdk_inputs();
          native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, request.fg_success, handle, observed.replaced);
        }
        Check(native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_Backbuffer) == backbuffer.Get()
                && native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess) == input_hudless
                && native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_UI) == nullptr
                && native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_UIAlpha) == input_alpha
                && native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_Depth) == depth.Get()
                && native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_MVecs) == motion.Get()
                && native::GetUInt(&parameters, NVSDK_NGX_DLSSG_Parameter_Reset) == (request.reset ? 1u : 0u)
                && native::GetUInt(&parameters, NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex) == request.index,
            "native FG restores original typed color, guide and scalar parameters after consumption");
        restored_back = Capture(gpu, backbuffer.Get(), NATIVE_INPUT_STATE);
        original_hudless = Capture(gpu, input_hudless, NATIVE_INPUT_STATE);
        unchanged_ui = Capture(gpu, ui.Get(), NATIVE_INPUT_STATE);
        if (request.overwrite_sync_motion) {
          Check(observed.replaced && bound_hudless != input_hudless,
              "synchronous MFG overwrite test starts with an accepted index-one replacement");
          auto duplicate = parameters;
          duplicate.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 2u);
          ID3D12Resource* sdk_hudless = nullptr;
          const auto original_evaluate = [&](ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* feature,
                                             const NVSDK_NGX_Parameter* block) {
            Check(list == gpu.list.Get() && feature == handle && block == &duplicate
                      && native::PreFgResource(block, NVSDK_NGX_DLSSG_Parameter_MVecs) == motion.Get(),
                "the SDK receives the same FG call and motion binding after a same-address overwrite");
            sdk_hudless = native::PreFgResource(block, NVSDK_NGX_DLSSG_Parameter_HUDLess);
            return NVSDK_NGX_Result_Success;
          };
          static thread_local const decltype(original_evaluate)* handler = nullptr;
          funnel_accounting_tests::Restore operation;
          operation.Value(&handler);
          operation.Value(&native::ngx_slot_real[0].evaluate);
          handler = &original_evaluate;
          native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(+[](
              ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* feature, const NVSDK_NGX_Parameter* block,
              PFN_NVSDK_NGX_ProgressCallback) { return (*handler)(list, feature, block); });
          const auto before = read_progress();
          const auto calls = model_calls;
          const auto reuses = native::edit_reuse_reuses.load();
          const auto evaluate_mfg = [&](ID3D12Resource* expected, const char* message) {
            sdk_hudless = nullptr;
            Check(native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), handle, &duplicate, nullptr)
                          == NVSDK_NGX_Result_Success && sdk_hudless == expected,
                message);
            Check(native::PreFgResource(&duplicate, NVSDK_NGX_DLSSG_Parameter_HUDLess) == input_hudless
                      && model_calls == calls && native::edit_reuse_reuses.load() == reuses
                      && read_progress() == before,
                "MFG preserves the first delivery, history and original parameter bindings");
          };
          evaluate_mfg(bound_hudless, "the actual NGX wrapper re-delivers an unchanged synchronous input group");
          upload(other_motion.Get(), std::vector<uint16_t>(width * height * 2, 0x3c00), NATIVE_INPUT_STATE);
          gpu.Transition(other_motion.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
          gpu.Transition(motion.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_DEST);
          gpu.list->CopyResource(motion.Get(), other_motion.Get());
          gpu.Transition(motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, NATIVE_INPUT_STATE);
          gpu.Transition(other_motion.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
          evaluate_mfg(input_hudless, "the actual NGX wrapper rejects synchronous cache reuse after same-address motion overwrite");
        }
        if (request.retain_recording) {
          for (const auto* snapshot : {&consumed_back, &consumed_hudless, &restored_back, &original_hudless, &unchanged_ui}) {
            gpu.retained_resources.emplace_back(snapshot->readback.Get());
          }
          const auto produced = read_progress();
          const auto& stream = native::fg_service::streams.at({true, context});
          std::vector<std::pair<addon::FenceRef, uint64_t>> receipts;
          Check(observed.replaced && produced.cached != nullptr
                  && stream.native_pair_recording == submission::RecordingIdentity(gpu.list.Get())
                  && submission::UseSubmissionTokens(stream.native_pair_producer_marker, &receipts)
                      == submission::UseSubmission::kUnsubmitted,
              "complete Native color recording has no cross-list order before Execute");
          const auto check_mfg = [&](ID3D12GraphicsCommandList* consumer, ID3D12Resource* consumer_hudless,
                                     ID3D12Resource* consumer_motion, bool deliver) {
            auto duplicate = parameters;
            duplicate.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 2u);
            duplicate.Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, consumer_hudless);
            duplicate.Set(NVSDK_NGX_DLSSG_Parameter_MVecs, consumer_motion);
            const auto before = model_calls;
            const auto reuses = native::edit_reuse_reuses.load();
            const auto producer_result = native::native_fg_result.load();
            const auto* producer_marker = native::native_fg_marker.load();
            bool replaced = false;
            ID3D12Resource* original = nullptr;
            {
              const addon::RuntimeLock lock(native::runtime_mutex);
              original = native::RunNativePreFg(consumer, &duplicate, handle, &replaced);
            }
            auto* expected = (deliver ? bound_hudless : consumer_hudless);
            Check(replaced == deliver && original == (deliver ? consumer_hudless : nullptr)
                    && native::PreFgResource(&duplicate, NVSDK_NGX_DLSSG_Parameter_HUDLess) == expected,
                "same-token consumption requires every cached color role and proven producer order");
            native::RestoreNativePreFg(consumer, &duplicate, original, true, handle, replaced);
            Check(native::PreFgResource(&duplicate, NVSDK_NGX_DLSSG_Parameter_HUDLess) == consumer_hudless
                    && model_calls == before && native::edit_reuse_reuses.load() == reuses
                    && native::native_fg_result.load() == producer_result
                    && native::native_fg_marker.load() == producer_marker
                    && read_progress() == produced,
                "Native MFG restores its binding and preserves producer state and history on denial and re-delivery");
          };
          check_mfg(proof_list.Get(), input_hudless, motion.Get(), false);
          check_mfg(gpu.list.Get(), nullptr, motion.Get(), false);
          check_mfg(gpu.list.Get(), input_hudless, other_motion.Get(), false);
          check_mfg(gpu.list.Get(), input_hudless, motion.Get(), true);
        }
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
            {
              sl::Resource backbuffer_tag(sl::ResourceType::eTex2d, backbuffer.Get(), NATIVE_INPUT_STATE);
              sl::Resource hudless_tag(sl::ResourceType::eTex2d, hudless.Get(), NATIVE_INPUT_STATE);
              sl::Resource depth_tag(sl::ResourceType::eTex2d, depth.Get(), NATIVE_INPUT_STATE);
              sl::Resource motion_tag(sl::ResourceType::eTex2d, motion.Get(), NATIVE_INPUT_STATE);
              sl::Resource alpha_tag(sl::ResourceType::eTex2d, input_alpha, NATIVE_INPUT_STATE);
              constexpr auto until_present = sl::ResourceLifecycle::eValidUntilPresent;
              const sl::ResourceTag tags[] = {
                  {&backbuffer_tag, sl::kBufferTypeBackbuffer, until_present},
                  {&hudless_tag, sl::kBufferTypeHUDLessColor,
                      direct_tag ? until_present : sl::ResourceLifecycle::eOnlyValidNow},
                  {&depth_tag, sl::kBufferTypeDepth, until_present},
                  {&motion_tag, sl::kBufferTypeMotionVectors, until_present},
                  {&alpha_tag, sl::kBufferTypeUIAlpha, until_present},
              };
              const uint32_t tag_count = (input_alpha != nullptr && !request.omit_alpha_tag ? 5u : 4u);
              const sl::ViewportHandle viewport(0u);
              const auto original_tag = [&](const sl::FrameToken& original_frame, const sl::ViewportHandle& original_viewport,
                                            const sl::ResourceTag* original_tags, uint32_t count, sl::CommandBuffer* list) {
                Check(&original_frame == &frame && &original_viewport == &viewport && original_tags == tags
                          && count == tag_count && list == gpu.list.Get(),
                    "the registered wrapper passes the exact frame, viewport, tag and recording to Streamline");
                if (!direct_tag) {
                  gpu.Transition(hudless.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
                  gpu.Transition(hudless_clone.Get(), clone_initialized ? NATIVE_INPUT_STATE : D3D12_RESOURCE_STATE_COMMON,
                      D3D12_RESOURCE_STATE_COPY_DEST);
                  gpu.list->CopyResource(hudless_clone.Get(), hudless.Get());
                  gpu.Transition(hudless_clone.Get(), D3D12_RESOURCE_STATE_COPY_DEST, NATIVE_INPUT_STATE);
                  gpu.Transition(hudless.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
                  clone_initialized = true;
                }
                return (request.tag_copy != TagCopy::FAILED ? sl::Result::eOk : sl::Result::eErrorInvalidParameter);
              };
              static const decltype(original_tag)* handler = nullptr;
              handler = &original_tag;
              native::real_streamline_set_tag_for_frame = +[](const sl::FrameToken& frame, const sl::ViewportHandle& viewport,
                                                             const sl::ResourceTag* tags, uint32_t count, sl::CommandBuffer* list) {
                return (*handler)(frame, viewport, tags, count, list);
              };
              const auto result = set_tag_for_frame(frame, viewport, tags, tag_count, gpu.list.Get());
              handler = nullptr;
              native::real_streamline_set_tag_for_frame = nullptr;
              Check((result == sl::Result::eOk) == (request.tag_copy != TagCopy::FAILED),
                  "the registered frame-tag wrapper returns the SDK outcome");
            }
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
          if (request.tag_change == TagChange::PRESENT_END) {
            {
              const addon::RuntimeLock lock(native::runtime_mutex);
              Check(tag_source::CurrentFrame(input_hudless, gpu.list.Get()).available,
                  "the fresh completed role group is eligible before the SDK Present boundary");
            }
            addon::fg::native_options::Configure(nullptr, native::OnNativePresentObserved);
            run_sdk(request.token, [] {}, true, false, false);
            addon::fg::native_options::Configure(nullptr);
            {
              const addon::RuntimeLock lock(native::runtime_mutex);
              Check(!tag_source::CurrentFrame(input_hudless, gpu.list.Get()).available,
                  "the SDK Present boundary expires the role group before any subsequent texture overwrite");
            }
          }
          if (request.tag_change == TagChange::WRITE_ALPHA || request.tag_change == TagChange::PRESENT_END) {
            for (size_t alpha = 3; alpha < source_ui.size(); alpha += 4) {
              source_ui[alpha] ^= 0xff;
            }
            upload(ui.Get(), source_ui, NATIVE_INPUT_STATE);
            submit();
          }
          if (request.tag_change == TagChange::RETAG_HUDLESS || request.tag_change == TagChange::RETAG_ALPHA) {
            const bool tag_hudless = request.tag_change == TagChange::RETAG_HUDLESS;
            FakeFrameToken newer;
            newer.frame = request.token + 1;
            sl::Resource resource(sl::ResourceType::eTex2d,
                tag_hudless ? static_cast<void*>(hudless.Get()) : static_cast<void*>(ui.Get()), NATIVE_INPUT_STATE);
            const sl::ResourceTag replacement(&resource,
                tag_hudless ? sl::kBufferTypeHUDLessColor : sl::kBufferTypeUIAlpha,
                sl::ResourceLifecycle::eValidUntilPresent);
            const auto original = std::exchange(native::real_streamline_set_tag_for_frame,
                +[](const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer* list) {
                  Check(list == nullptr, "role-only retag forwards the SDK's null list");
                  return sl::Result::eOk;
                });
            const auto result = set_tag_for_frame(newer, sl::ViewportHandle(0u), &replacement, 1, nullptr);
            native::real_streamline_set_tag_for_frame = original;
            Check(result == sl::Result::eOk, "the registered role-only tag wrapper returns SDK success");
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
                    && !addon::fg::native_options::CurrentPresentFrame(owner.native_output_identity, backbuffer.Get()).available,
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
              const native::native_tag_source::FrameRoles roles{
                  .back_buffer = backbuffer.Get(), .depth = depth.Get(), .motion = motion.Get(), .alpha = input_alpha};
              const auto tagged = tag_source::CurrentFrame(input_hudless, gpu.list.Get(), claimed_epoch, &roles);
              observed.source_generation = tagged.generation;
              const bool expected = !request.omit_alpha_tag && request.tag_change == TagChange::NONE
                  && (request.tag_copy == TagCopy::SUCCESS || request.tag_copy == TagCopy::DIRECT);
              Check(tagged.available == expected && (!expected || tagged.frame == request.token),
                  "only a successful unique direct tag or unchanged completed clone retains its actual FrameToken");
              if (request.tag_copy == TagCopy::DIRECT_AMBIGUOUS) {
                Check(std::strcmp(tagged.reason, "tagged_resource_tokens_ambiguous") == 0,
                    "two direct tokens are refused specifically for ambiguity rather than a missing source");
              }
              if (request.omit_alpha_tag) {
                Check(std::strcmp(tagged.reason, "tagged_resource_group_mismatch") == 0,
                    "a tagged HUDLess version cannot substitute for an untagged required alpha version");
              }
              if (request.tag_change == TagChange::WRITE_ALPHA) {
                Check(std::strcmp(tagged.reason, "tagged_resource_group_overwritten") == 0,
                    "a same-address Alpha overwrite withdraws the complete async color group");
              }
              if (request.tag_change == TagChange::RETAG_HUDLESS || request.tag_change == TagChange::RETAG_ALPHA) {
                Check(std::strcmp(tagged.reason, "tagged_resource_group_retagged") == 0,
                    "a successful role-only tag revokes the older group without a new list-backed tag ticket");
              }
            }
            consume();
          } catch (...) {
            consumer_failure = std::current_exception();
          }
        });
        consumer.join();
        if (consumer_failure != nullptr) std::rethrow_exception(consumer_failure);
        if (request.update_source && request.tag_change != TagChange::PRESENT_END) {
          run_sdk(request.token, [] {}, true, false, false);
        }
      } else if (request.source_scope) {
        run_sdk(request.token, [&] {
          const auto ticket = addon::fg::native_options::CurrentPresentFrame(owner.native_output_identity, backbuffer.Get());
          Check(ticket.available && ticket.frame == request.token, "SDK scope exposes this output's proven PresentStart");
          observed.source_generation = ticket.generation;
          consume();
        }, true, !request.without_backbuffer_tag);
      } else {
        Check(!addon::fg::native_options::CurrentPresentFrame(owner.native_output_identity, backbuffer.Get()).available,
            "missing SDK source scope cannot expose an old token");
        consume();
      }
      if (request.retain_recording) {
        // The current SDK scope must end before the old token acquires its own scope.
        const auto prepared = read_progress();
        const auto marker = native::native_fg_marker.load();
        const auto serial = native::native_fg_recording_serial.load();
        const auto submitted = native::native_fg_submitted_frames.load();
        const auto calls = model_calls;
        const auto reuses = native::edit_reuse_reuses.load();
        std::vector<std::pair<addon::FenceRef, uint64_t>> receipts;
        Check(request.fg_success && prepared.complete && prepared.cached != nullptr
                && native::native_fg_result.load() == addon::fg::Result::PREPARED
                && marker == prepared.cached->color[0] && serial == prepared.delivery_serial
                && submission::UseSubmissionTokens(marker, &receipts) == submission::UseSubmission::kUnsubmitted,
            "SDK-accepted F2 remains PREPARED on the actual unsubmitted host recording");
        run_sdk(request.token - 1, [&] {
          const auto ticket = addon::fg::native_options::CurrentPresentFrame(owner.native_output_identity, backbuffer.Get());
          Check(ticket.available && ticket.frame == request.token - 1 && ticket.generation == prepared.source_generation,
              "the late F1 consumer acquires its own trusted SDK scope in F2's source generation");
          auto late_parameters = parameters;
          late_parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
          bool replaced = false;
          ID3D12Resource* original = nullptr;
          {
            const addon::RuntimeLock lock(native::runtime_mutex);
            original = native::RunNativePreFg(gpu.list.Get(), &late_parameters, handle, &replaced);
          }
          Check(original == nullptr && !replaced, "unsubmitted F2 declines late F1 without another NR output replacement");
          native::RestoreNativePreFg(gpu.list.Get(), &late_parameters, original, false, handle, replaced);
        }, false);
        Check(native::native_fg_result.load() == addon::fg::Result::PREPARED
                && native::native_fg_marker.load() == marker && native::native_fg_recording_serial.load() == serial
                && native::native_fg_submitted_frames.load() == submitted
                && model_calls == calls && native::edit_reuse_reuses.load() == reuses && read_progress() == prepared,
            "late F1 SDK failure preserves F2's PREPARED submission marker, serial, cache and history before Execute");
      }
      observed.evaluates = model_calls - before_calls;
      observed.reuses = native::edit_reuse_reuses.load() - before_reuses;
      observed.reset = (observed.evaluates != 0 ? model_resets.back() : -1);
      submit(request.retain_recording);
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
    uint32_t token = std::numeric_limits<uint32_t>::max() - (native::kContractDebounceFrames + 16);
    bool direct_warmup_mfg_checked = false;
    const auto recover = [&](TagCopy tag_copy = TagCopy::NONE, bool reset = true) {
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 12; ++attempt) {
        auto observed = record({.token = ++token, .reset = reset, .tag_copy = tag_copy, .sdk_wrapper = tag_copy == TagCopy::NONE});
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
        if (observed.replaced && observed.evaluates == 1) return observed;
      }
      throw std::runtime_error("bounded native warmup failed to reach a one-model exact-alpha refresh");
    };
    recover();
    auto& stream = native::fg_service::streams.at({true, context});
    const auto* role_handle = stream.roles[1].Handle();
    {
      constexpr uint32_t last_token = std::numeric_limits<uint32_t>::max();
      constexpr uint64_t token_period = static_cast<uint64_t>(last_token) + 1;
      const auto before = read_progress();
      const auto last = record({.token = last_token, .reset = true});
      const auto at_limit = read_progress();
      Check(last.replaced && last.evaluates == 1 && last.reuses == 0 && last.reset == 1
              && at_limit.kind == frame_policy::SourceKind::SDK_SCOPE && at_limit.source == output_identity
              && at_limit.source_generation == before.source_generation
              && last.source_generation == at_limit.source_generation
              && at_limit.sequence_period == token_period && at_limit.source_frame == token_period
              && at_limit.claims == before.claims + 1 && at_limit.outputs == before.outputs + 1,
          "a real SDK token reaches UINT32_MAX with its unchanged source generation and finite counter period");
      const auto wrapped = record({.token = 0});
      const auto at_zero = read_progress();
      Check(wrapped.replaced && wrapped.evaluates == 1 && wrapped.reuses == 0 && wrapped.reset == 1
              && at_zero.kind == at_limit.kind && at_zero.source == at_limit.source
              && at_zero.source_generation == at_limit.source_generation
              && wrapped.source_generation == at_limit.source_generation && at_zero.sequence_period == token_period
              && at_zero.source_frame == 1 && at_zero.native_frame == 1
              && at_zero.claims == at_limit.claims + 1 && at_zero.outputs == at_limit.outputs + 1
              && at_zero.base_frame == at_limit.base_frame + 1 && at_zero.observed_frames == at_limit.observed_frames + 1
              && at_zero.complete && at_zero.anchor != nullptr && at_zero.anchor->valid
              && !at_zero.reused && at_zero.anchor_sequence == 1
              && at_zero.model_frames[1] == 1 && at_zero.output_frames[1] == 1,
          "the actual SDK counter wrap claims raw token zero as sequence one and refreshes with a model Reset");
      const auto after_wrap = record({.token = 1});
      const auto at_one = read_progress();
      Check(after_wrap.replaced && after_wrap.evaluates == 0 && after_wrap.reuses == 1
              && at_one.kind == at_zero.kind && at_one.source == at_zero.source
              && at_one.source_generation == at_zero.source_generation && after_wrap.source_generation == at_zero.source_generation
              && at_one.sequence_period == token_period && at_one.source_frame == 2 && at_one.native_frame == 2
              && at_one.claims == at_zero.claims + 1 && at_one.outputs == at_zero.outputs + 1
              && at_one.base_frame == at_zero.base_frame + 1 && at_one.observed_frames == at_zero.observed_frames + 1
              && at_one.complete && at_one.anchor == at_zero.anchor && at_one.anchor_generation == at_zero.anchor_generation
              && at_one.anchor_sequence == 2 && at_one.reused && at_one.reuse_count == at_zero.reuse_count + 1
              && at_one.model_frames[1] == 1 && at_one.output_frames[1] == 2
              && matches(after_wrap.consumed_back, source_back) && matches(after_wrap.consumed_hudless, source_hudless),
          "the adjacent real SDK token one reuses the post-wrap anchor and delivers its current colors");
      const auto submissions = native::native_fg_submitted_frames.load();
      const auto late_limit = record({.token = last_token, .update_source = false});
      Check(!late_limit.replaced && late_limit.evaluates == 0 && late_limit.reuses == 0
              && late_limit.source_generation == at_one.source_generation && read_progress() == at_one
              && native::native_fg_submitted_frames.load() == submissions
              && late_limit.consumed_back == after_wrap.consumed_back
              && late_limit.consumed_hudless == after_wrap.consumed_hudless,
          "late UINT32_MAX after the wrap preserves the current cache, model, anchor, game history, clock and pixels");
      const auto following = record({.token = 2});
      Check(following.replaced && following.evaluates == 1 && following.reuses == 0 && following.reset == 0,
          "real token two retains the post-wrap refresh cadence after the old maximum token is refused");
      token = 2;
    }
    std::array<Observation, 4> baseline;
    enum class ExtraConsumption { NONE, REPEATED, LATE, LATE_SDK_FAILURE };
    for (const auto extra : {ExtraConsumption::NONE, ExtraConsumption::REPEATED, ExtraConsumption::LATE, ExtraConsumption::LATE_SDK_FAILURE}) {
      // The fixture pixels repeat every 12 tokens so the reset arms have
      // identical real-frame RGBA before inserting extra consumption.
      if (extra != ExtraConsumption::NONE) token += 8;
      for (size_t frame = 0; frame < baseline.size(); ++frame) {
        const bool refresh = frame % 2 == 0;
        const auto before = read_progress();
        const auto submissions_before = native::native_fg_submitted_frames.load();
        auto observed = record({.token = ++token, .reset = frame == 0, .sdk_wrapper = extra == ExtraConsumption::NONE,
            .without_backbuffer_tag = extra == ExtraConsumption::NONE});
        const auto after = read_progress();
        if (!observed.replaced || observed.evaluates != (refresh ? 1u : 0u)
            || observed.reuses != (refresh ? 0u : 1u)
            || observed.reset != (refresh ? (frame == 0 ? 1 : 0) : -1)) {
          throw std::runtime_error("Native refresh/reuse phase mismatch at token " + std::to_string(token)
              + ", replaced " + std::to_string(observed.replaced) + ", evaluates " + std::to_string(observed.evaluates)
              + ", reuses " + std::to_string(observed.reuses) + ", Reset " + std::to_string(observed.reset));
        }
        const auto& base = stream.base_frame_clock.frame;
        Check(stream.native_edit_source && after.source == output_identity && after.source_generation != 0
                && after.source_generation == observed.source_generation
                && after.source_frame == static_cast<uint64_t>(token) + 1
                && base.kind == frame_policy::SourceKind::SDK_SCOPE
                && base.pass_begin == 0 && base.pass_count == 1 && base.roles == 3 && base.reuse_edits
                && after.claims == before.claims + 1
                && after.base_frame == before.base_frame + 1 && after.observed_frames == before.observed_frames + 1
                && (frame == 0 || after.reset_epoch == before.reset_epoch),
            "each Native SDK token claims its actual source, Present pass and two delivered color roles once");
        const auto& model = native::features.at(role_handle).slots[0];
        Check(after.anchor == model.edit_reuse.get() && after.anchor != nullptr && after.anchor->valid
                && after.anchor_sequence == after.source_frame && after.reused == !refresh,
            "native edit history follows the observed token and records the current x2 age");
        if (frame != 0) {
          Check(after.anchor == before.anchor && after.anchor_generation == before.anchor_generation + (refresh ? 1 : 0)
                  && after.reuse_count == before.reuse_count + (refresh ? 0 : 1),
              "native refresh changes the anchor generation and reuse only consumes the existing anchor");
        }
        if (refresh) {
          Check(model_evaluated_handles.back() == model.handle,
              "exact-alpha native refresh evaluates the HUDLess model");
        } else {
          Check(matches(observed.consumed_back, source_back) && matches(observed.consumed_hudless, source_hudless),
              "native reuse delivers the current PQ colors and both packed alpha planes exactly");
        }
        Check(after.model_frames[0] == 0
                && after.model_frames[1] == (refresh ? after.source_frame : before.model_frames[1])
                && after.output_frames[1] == after.source_frame,
            "exact-alpha composition delivers both colors with only the current HUDLess model output");
        Check(after.cached != nullptr && stream.result == addon::fg::Result::PREPARED
                && stream.base_frame_clock.complete && after.outputs == before.outputs + 1
                && stream.native_pair_producer_marker == after.cached->original_backbuffer
                && !stream.native_pair_completion.empty()
                && stream.native_pair_completion.fence->GetCompletedValue() >= stream.native_pair_completion_value
                && native::native_fg_submitted_frames.load() == submissions_before + 1,
            "a new complete native color pair has a real ordered producer submission");
        for (const auto& binding : after.anchor->bindings) {
          for (const auto& resource : binding.resources) {
            Check(resource.Get() != backbuffer.Get() && resource.Get() != hudless.Get(),
                "native edit descriptors retain private staged originals rather than game color inputs");
          }
        }
        if (extra == ExtraConsumption::NONE) {
          baseline[frame] = std::move(observed);
          continue;
        }
        Check(observed.consumed_back == baseline[frame].consumed_back
                && observed.consumed_hudless == baseline[frame].consumed_hudless,
            "extra Native consumption preserves each real frame's baseline delivered RGBA");
        if (extra == ExtraConsumption::REPEATED && frame == 1) {
          const auto duplicated = record({.token = token, .index = 2, .update_source = false});
          Check(duplicated.replaced && duplicated.evaluates == 0 && duplicated.reuses == 0
                  && duplicated.consumed_back == observed.consumed_back
                  && duplicated.consumed_hudless == observed.consumed_hudless,
              "repeated MFG consumes the exact previously delivered Native color pair");
          Check(read_progress() == after && native::native_fg_submitted_frames.load() == submissions_before + 1,
              "repeated MFG advances neither model/anchor/history nor the application submission count");
        }
        if ((extra == ExtraConsumption::LATE || extra == ExtraConsumption::LATE_SDK_FAILURE) && frame == 1) {
          const bool sdk_succeeded = extra == ExtraConsumption::LATE;
          const auto marker = native::native_fg_marker.load();
          const auto serial = native::native_fg_recording_serial.load();
          const auto calls = model_calls;
          const auto reuses = native::edit_reuse_reuses.load();
          for (const bool keep_hudless : {true, false}) {
            auto late_parameters = parameters;
            auto* late_hudless = (keep_hudless ? hudless.Get() : nullptr);
            auto* late_alpha = (keep_hudless ? ui.Get() : nullptr);
            late_parameters.Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, late_hudless);
            late_parameters.Set(NVSDK_NGX_DLSSG_Parameter_UIAlpha, late_alpha);
            late_parameters.Set(NVSDK_NGX_DLSSG_Parameter_Reset, 1u);
            late_parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
            run_sdk(token - 1, [&] {
              const auto ticket = addon::fg::native_options::CurrentPresentFrame(owner.native_output_identity, backbuffer.Get());
              Check(ticket.available && ticket.frame == token - 1 && ticket.generation == after.source_generation,
                  "the late Native wrapper receives a trusted older token in F2's actual source generation");
              bool replaced = false;
              ID3D12Resource* original = nullptr;
              {
                const addon::RuntimeLock lock(native::runtime_mutex);
                original = native::RunNativePreFg(gpu.list.Get(), &late_parameters, handle, &replaced);
              }
              Check(original == nullptr && !replaced, "late F1 is declined for both its original and changed color-role group");
              native::RestoreNativePreFg(gpu.list.Get(), &late_parameters, original, sdk_succeeded, handle, replaced);
              Check(native::PreFgResource(&late_parameters, NVSDK_NGX_DLSSG_Parameter_Backbuffer) == backbuffer.Get()
                      && native::PreFgResource(&late_parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess) == late_hudless
                      && native::PreFgResource(&late_parameters, NVSDK_NGX_DLSSG_Parameter_UIAlpha) == late_alpha
                      && native::GetUInt(&late_parameters, NVSDK_NGX_DLSSG_Parameter_Reset) == 1u,
                  "declining late F1 preserves its borrowed SDK parameters");
            }, sdk_succeeded);
            const auto late_back = Capture(gpu, backbuffer.Get(), NATIVE_INPUT_STATE);
            const auto late_original_hudless = Capture(gpu, hudless.Get(), NATIVE_INPUT_STATE);
            submit();
            Check(read_progress() == after && model_calls == calls && native::edit_reuse_reuses.load() == reuses
                    && stream.result == addon::fg::Result::PREPARED
                    && native::native_fg_marker.load() == marker && native::native_fg_recording_serial.load() == serial
                    && native::native_fg_submitted_frames.load() == submissions_before + 1,
                sdk_succeeded
                    ? "late F1 SDK success preserves F2's cache, producer, model, anchor, game history and complete common clock"
                    : "late F1 SDK failure preserves F2's cache, producer, model, anchor, game history and complete common clock");
            Check(late_back.Pixels() == observed.consumed_back && matches(late_original_hudless.Pixels(), source_hudless),
                "late F1 leaves F2's delivered BB and the original HUDLess pixels intact");
          }
          const auto repeated_f2 = record({.token = token, .index = 2, .update_source = false});
          Check(repeated_f2.replaced && repeated_f2.evaluates == 0 && repeated_f2.reuses == 0
                  && read_progress() == after && repeated_f2.consumed_back == observed.consumed_back
                  && repeated_f2.consumed_hudless == observed.consumed_hudless,
              "F2 remains re-deliverable after late F1 and the next real frames retain the baseline cadence");
        }
      }
    }
    token += 2;
    const auto gap = record({.token = token});
    Check(gap.replaced && gap.evaluates == 1 && gap.reuses == 0 && gap.reset == 1,
        "a real native token gap refreshes with Reset instead of borrowing adjacent-frame motion");
    const auto reset = record({.token = ++token, .reset = true});
    Check(reset.replaced && reset.evaluates == 1 && reset.reuses == 0 && reset.reset == 1,
        "explicit native Reset interrupts the x2 reuse phase");

    const auto overwritten_refresh = record({.token = ++token, .reset = true,
        .sdk_wrapper = true, .overwrite_sync_motion = true});
    const auto overwritten_reuse = record({.token = ++token, .sdk_wrapper = true, .overwrite_sync_motion = true});
    const auto after_overwrite = record({.token = ++token, .sdk_wrapper = true});
    Check(overwritten_refresh.replaced && overwritten_refresh.evaluates == 1 && overwritten_refresh.reuses == 0
            && overwritten_reuse.replaced && overwritten_reuse.evaluates == 0 && overwritten_reuse.reuses == 1
            && after_overwrite.replaced && after_overwrite.evaluates == 1 && after_overwrite.reuses == 0,
        "same-address MFG rejection preserves the next legitimate refresh and reuse phase");
    std::cout << "PASS synchronous MFG input versions refresh/reuse redelivered before same-address motion copy;"
                 " both rejected after copy; next frame refreshed evidence=WARP+MOCK\n";

    {
      Check(SUCCEEDED(proof_list->Close()) && SUCCEEDED(proof_list->Reset(proof_allocator.Get(), nullptr)),
          "seed the foreign consumer recording's restorable command state");
      ComPtr<ID3D12QueryHeap> query;
      const D3D12_QUERY_HEAP_DESC query_desc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = 1};
      Check(SUCCEEDED(gpu.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&query))), "native edit replay query");
      const auto readback = gpu.Buffer(sizeof(uint64_t), D3D12_HEAP_TYPE_READBACK);
      const auto timestamp = [&] {
        uint64_t* mapped = nullptr;
        Check(SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "native edit replay timestamp");
        const auto value = *mapped;
        readback->Unmap(0, nullptr);
        return value;
      };
      funnel_accounting_tests::Restore substitutions;
      substitutions.Value(&model_execution_query);
      substitutions.Value(&model_execution_readback);
      model_execution_query = query.Get();
      model_execution_readback = readback.Get();
      const auto submissions_before = native::native_fg_submitted_frames.load();
      const auto retained = record({.token = ++token, .reset = true, .retain_recording = true});
      model_execution_query = nullptr;
      model_execution_readback = nullptr;
      const auto original_execution = timestamp();
      const auto recorded = read_progress();
      const auto calls = model_calls;
      const auto recording = submission::RecordingIdentity(gpu.list.Get());
      Check(retained.replaced && retained.evaluates == 1 && retained.reset == 1 && recorded.cached != nullptr
              && native::native_fg_result.load() == addon::fg::Result::SUBMITTED
              && native::native_fg_marker.load() == recorded.cached->color[0]
              && native::native_fg_recording_serial.load() == recorded.delivery_serial
              && native::native_fg_counted_serial.load() == recorded.delivery_serial
              && native::native_fg_submitted_frames.load() == submissions_before + 1,
          "the original host list reaches SUBMITTED and counts F2 exactly once after declining late F1 SDK failure");
      const auto* producer = recorded.cached->original_backbuffer;
      execute_recording();
      Check(original_execution != 0 && timestamp() > original_execution,
          "replaying the closed host list executes the GPU model stand-in again");
      Check(submission::RecordingIdentity(gpu.list.Get()) == recording && model_calls == calls
              && read_progress() == recorded && native::native_fg_submitted_frames.load() == submissions_before + 1,
          "host replay preserves the CPU model, frame, edit history and delivery claim");
      std::vector<std::pair<addon::FenceRef, uint64_t>> receipts;
      Check(submission::UseSubmissionTokens(producer, &receipts) == submission::UseSubmission::kTokens
              && receipts.size() == 2 && receipts[0].second < receipts[1].second
              && !submission::ResourceReleasable(producer),
          "two real submissions retain the original producer while its host list remains replayable");
      for (const auto& [fence, value] : receipts) {
        const auto completed = fence->GetCompletedValue();
        Check(completed != UINT64_MAX && completed >= value, "each host replay receipt has real GPU completion");
      }
      retire_recording();
      Check(stream.native_pair_replayed && submission::ResourceReleasable(producer),
          "after list Reset the producer retires and the stream preserves its replay ambiguity");
      const auto denied = record({.token = token, .index = 2, .update_source = false});
      Check(!denied.replaced && denied.evaluates == 0 && denied.reuses == 0 && read_progress() == recorded
              && denied.consumed_back == retained.consumed_back
              && native::native_fg_submitted_frames.load() == submissions_before + 1,
          "Native refuses the same-frame replayed cache without rewriting the borrowed BB or advancing history");
      const auto recovered = recover(TagCopy::NONE, false);
      Check(recovered.reset == 1 && !stream.native_pair_replayed,
          "a new properly ordered Native source frame rebuilds model history after replay without a caller Reset");
      const auto reuse_after_replay = record({.token = ++token});
      Check(reuse_after_replay.replaced && reuse_after_replay.evaluates == 0 && reuse_after_replay.reuses == 1,
          "the frame following replay recovery reuses the new anchor");
      // Leave a fresh anchor for the existing Adaptive residency regression.
      Check(record({.token = ++token}).evaluates == 1, "refresh the fixed anchor before Adaptive changes");
    }

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
              && native::features.at(role_handle).adaptive_cache != nullptr,
          "actual Adaptive preparation selects the upper tier after the preceding reuse");
      for (uint32_t attempt = 0; attempt < 16 && !stream.adaptive_session->TargetReady(); ++attempt) {
        record({.token = ++token});
      }
      Check(stream.adaptive_session->TargetReady() && stream.adaptive_session->ladder.count == 2,
          "two native Adaptive tiers become ready through normal recording and Fence completion");
      const auto upper_refresh = record({.token = ++token, .reset = true, .sdk_wrapper = true});
      const auto& upper_role = native::features.at(role_handle);
      const auto upper_anchor = upper_role.slots[0].edit_reuse;
      Check(upper_refresh.replaced && upper_refresh.evaluates == 1 && upper_refresh.reuses == 0
              && upper_role.adaptive_cache != nullptr && upper_role.adaptive_cache->active == 0
              && native::adaptive_resident_session.lock() == stream.adaptive_session
              && upper_role.input_width == width && upper_role.input_height == height
              && upper_anchor != nullptr && upper_anchor->valid,
          "the session's actual equivalent upper workset records a native edit anchor");
      const auto upper_reuse = record({.token = ++token, .sdk_wrapper = true});
      Check(upper_reuse.replaced && upper_reuse.evaluates == 0 && upper_reuse.reuses == 1
              && upper_role.slots[0].edit_reuse == upper_anchor,
          "the session's active equivalent upper workset delivers zero-model reuse");
      stream.adaptive_session->Request(1, native::SteadyNowNs());
      native::direct_api.evaluate = +[](ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* model,
                                          const NVSDK_NGX_Parameter* model_parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
        const auto desc = native::GetD3D12Resource(model_parameters, "DLSSNR.Output")->GetDesc();
        nr_edit_reuse_xefg_cases::model_pixels.assign(desc.Width * desc.Height * 4,
            DirectX::PackedVector::XMConvertFloatToHalf(.75f));
        return nr_edit_reuse_xefg_cases::EditedModel(list, model, model_parameters, callback);
      };
      const auto switched = record({.token = ++token, .sdk_wrapper = true, .check_pending_mfg = true});
      native::direct_api.evaluate = nr_edit_reuse_gpu_cases::NeutralModel;
      const auto& switched_role = native::features.at(role_handle);
      const auto switched_size = stream.adaptive_session->ladder.sizes[1][0];
      Check(switched.replaced && switched.evaluates > 0 && switched.reuses == 0
              && stream.adaptive_frame.slot == 1 && switched_role.adaptive_cache != nullptr
              && switched_role.adaptive_cache->active == 1
              && switched_role.input_width == switched_size.width && switched_role.input_height == switched_size.height
              && switched_size.width < width && switched_role.slots[0].edit_reuse == nullptr,
          "native Adaptive switch delivers the actual smaller HUDLess tier without creating an edit anchor");
      Check(switched.consumed_back.size() == source_back.size() * sizeof(uint32_t)
              && switched.consumed_hudless.size() == source_hudless.size() * sizeof(uint32_t),
          "the low tier supplies both complete color planes at SDK consumption");
      bool low_rgb_changed = false;
      constexpr uint32_t alpha_mask = 3u << 30;
      for (size_t pixel = 0; pixel < source_back.size(); ++pixel) {
        uint32_t delivered_back = 0, delivered_hudless = 0;
        std::memcpy(&delivered_back, switched.consumed_back.data() + pixel * sizeof(uint32_t), sizeof(uint32_t));
        std::memcpy(&delivered_hudless, switched.consumed_hudless.data() + pixel * sizeof(uint32_t), sizeof(uint32_t));
        Check((delivered_back & alpha_mask) == (source_back[pixel] & alpha_mask)
                && (delivered_hudless & alpha_mask) == (source_hudless[pixel] & alpha_mask),
            "the edited low-tier SDK color planes retain the original packed alpha values");
        low_rgb_changed |= (delivered_hudless & ~alpha_mask) != (source_hudless[pixel] & ~alpha_mask);
      }
      Check(low_rgb_changed, "the low-tier SDK output contains a non-neutral RGB edit");
      const auto switched_cache = switched_role.adaptive_cache;
      const auto* switched_slot = stream.cached;
      const auto switched_progress = read_progress();
      const auto switched_submissions = native::native_fg_submitted_frames.load();
      std::vector<std::pair<addon::FenceRef, uint64_t>> proofs;
      Check(stream.source_frame_id == 0 && stream.source_sdk_decision == 0 && switched_progress.complete
              && switched_slot->base_frame_group.complete && switched_slot->base_frame_group.ready_roles == 3u
              && stream.native_pair_producer_marker == switched_slot->original_backbuffer
              && submission::UseSubmissionTokens(switched_slot->original_backbuffer, &proofs)
                  == submission::UseSubmission::kComplete
              && !stream.native_pair_completion.empty(),
          "the SDK accepted the complete low-tier group and its pruned producer retains the completion receipt");
      native::edit_reuse_enabled = false;
      const auto switched_mfg = record({.token = token, .index = 2, .update_source = false, .sdk_wrapper = true});
      for (const auto& denied : {
               record({.token = token, .index = 2, .source_scope = false, .update_source = false, .sdk_wrapper = true}),
               record({.token = token + 1, .index = 2, .update_source = false, .sdk_wrapper = true}),
               record({.token = token, .index = 2, .update_source = false, .with_ui_alpha = false, .sdk_wrapper = true})}) {
        Check(!denied.replaced && denied.evaluates == 0 && denied.reuses == 0
                && matches(denied.consumed_hudless, source_hudless),
            "the completed low tier rejects a missing version, a new token or a changed alpha role");
      }
      Check(switched_mfg.replaced && switched_mfg.evaluates == 0 && switched_mfg.reuses == 0
              && switched_mfg.consumed_back == switched.consumed_back
              && switched_mfg.consumed_hudless == switched.consumed_hudless && read_progress() == switched_progress
              && native::native_fg_submitted_frames.load() == switched_submissions,
          "low-tier per-frame output re-delivers exact RGBA with no new model, history or base-frame count after x2 is disabled");
      const auto low_overwritten = record({.token = ++token, .sdk_wrapper = true, .overwrite_sync_motion = true});
      Check(low_overwritten.replaced && low_overwritten.evaluates == 1 && low_overwritten.reuses == 0,
          "per-frame low-tier MFG serves unchanged inputs and refuses the same-address guide overwrite");
      native::edit_reuse_enabled = true;
      const auto* overwritten_slot = stream.cached;
      // Keep the old color slot busy without recording another use of its producer marker.
      native::fg_service::CopyColor(proof_list.Get(), hudless_clone.Get(), D3D12_RESOURCE_STATE_COMMON,
          overwritten_slot->color[0], NATIVE_INPUT_STATE);
      const auto no_token = record({.token = ++token, .source_scope = false});
      Check(no_token.replaced && no_token.evaluates > 0 && no_token.reuses == 0
              && stream.cached != overwritten_slot && stream.source_frame_id == 0
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
      std::cout << "NR_RELEASE_PARITY native-prefg-present-x2 adaptive-worksets active-upper=1/0"
                   " lower-per-frame=edited-rgba repeated=identical zero-advance=1"
                   " missing-version/alpha-retag/guide-overwrite=original tokenless=two-deliveries evidence=WARP+MOCK\n";
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
    {
      const auto submitted_before = native::native_fg_submitted_frames.load();
      const auto accepted = record({.token = ++token, .reset = true, .sdk_wrapper = true});
      const auto accepted_progress = read_progress();
      const auto accepted_submissions = native::native_fg_submitted_frames.load();
      const auto& stream = native::fg_service::streams.at({true, context});
      Check(accepted.replaced && accepted.evaluates == 1 && accepted.reuses == 0
              && accepted_progress.complete && accepted_progress.cached != nullptr
              && stream.native_pair_producer_marker == accepted_progress.cached->original_backbuffer
              && !stream.native_pair_completion.empty()
              && stream.native_pair_completion.fence->GetCompletedValue() >= stream.native_pair_completion_value
              && accepted_submissions == submitted_before + 1,
          "the SDK accepts and submits a complete non-neutral native refresh");
      Check(accepted.restored_back == accepted.consumed_back && !matches(accepted.restored_back, source_back),
          "the accepted native refresh leaves edited BB RGBA in the game resource");

      // Record the entry readback on the same list as the re-delivery, with no new input writes.
      const auto before_sdk = Capture(gpu, backbuffer.Get(), NATIVE_INPUT_STATE);
      const auto refused = record({.token = token, .index = 2, .fg_success = false, .update_source = false,
          .sdk_wrapper = true});
      Check(refused.replaced && refused.evaluates == 0 && refused.reuses == 0
              && refused.consumed_back == accepted.consumed_back && refused.consumed_hudless == accepted.consumed_hudless,
          "the rejecting SDK call consumes the already delivered BB and HUDLess RGBA without new NR work");
      const auto entry_back = before_sdk.Pixels();
      Check(entry_back == accepted.restored_back, "the failing re-delivery starts with the committed edited BB RGBA");
      Check(refused.restored_back == entry_back, "failed SDK re-delivery preserves the pre-call BB RGBA exactly");
      Check(read_progress() == accepted_progress && native::native_fg_submitted_frames.load() == accepted_submissions,
          "failed SDK re-delivery preserves the accepted cache, model history and base-frame counts");

      const auto retried = record({.token = token, .index = 2, .update_source = false, .sdk_wrapper = true});
      Check(retried.replaced && retried.evaluates == 0 && retried.reuses == 0
              && retried.consumed_back == entry_back && retried.restored_back == entry_back
              && retried.consumed_hudless == accepted.consumed_hudless
              && read_progress() == accepted_progress && native::native_fg_submitted_frames.load() == accepted_submissions,
          "the same input version re-delivers successfully after SDK rejection without advancing the accepted frame");
    }
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
    const auto failed = record({.token = ++token, .reset = true, .fg_success = false, .sdk_wrapper = true});
    Check(failed.replaced && failed.evaluates == 1 && failed.reuses == 0
            && !matches(failed.consumed_back, source_back) && matches(failed.restored_back, source_back)
            && native::fg_service::streams.at({true, context}).cached == nullptr,
        "FG failure rolls back the edited BB and invalidates the cache after restoring HUDLess/UI bindings");
    {
      // Removing exact UI alpha gives the existing Native contract two
      // independent model roles. Fail only the BB after HUDLess records.
      bool dual_ready = false;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 12; ++attempt) {
        const auto outputs = stream.base_frame_clock.outputs;
        const auto warmup = record({.token = ++token, .reset = true, .with_ui_alpha = false});
        if (warmup.replaced) {
          Check(warmup.evaluates == 2 && warmup.reuses == 0 && stream.base_frame_clock.complete
                  && stream.base_frame_clock.frame.roles == 3 && stream.base_frame_clock.outputs == outputs + 1
                  && stream.last_output_frame[0] == stream.base_frame_clock.frame.sequence
                  && stream.last_output_frame[1] == stream.base_frame_clock.frame.sequence,
              "the distinct Native BB and HUDLess model roles publish only as a complete group");
          dual_ready = true;
          break;
        }
        Check(stream.cached == nullptr && stream.base_frame_clock.outputs == outputs
                && matches(warmup.consumed_back, source_back) && matches(warmup.consumed_hudless, source_hudless),
            "private preparation of either Native role leaves original inputs and publishes no group");
      }
      Check(dual_ready, "both independent Native color roles finish bounded warmup");
      const auto* hudless_model = native::features.at(role_handle).slots[0].handle;
      const auto* backbuffer_model = native::features.at(stream.roles[0].Handle()).slots[0].handle;
      const auto before_failure = read_progress();
      const auto submissions = native::native_fg_submitted_frames.load();
      const auto evaluations = model_evaluated_handles.size();
      fail_call = model_calls + 2;
      const auto partial = record({.token = ++token, .reset = true, .with_ui_alpha = false});
      fail_call = 0;
      const auto after_failure = read_progress();
      Check(!partial.replaced && partial.evaluates == 2 && partial.reuses == 0
              && model_evaluated_handles[evaluations] == hudless_model
              && model_evaluated_handles[evaluations + 1] == backbuffer_model,
          "actual BB Evaluate failure occurs after HUDLess has recorded on the borrowed list");
      Check(after_failure.cached == nullptr && stream.result == addon::fg::Result::FAILED
              && !stream.base_frame_clock.complete && after_failure.outputs == before_failure.outputs
              && native::native_fg_submitted_frames.load() == submissions
              && matches(partial.consumed_back, source_back) && matches(partial.consumed_hudless, source_hudless),
          "a failed necessary role leaves both original SDK inputs and publishes no partial output or submission");
    }
    native::direct_api.evaluate = nr_edit_reuse_gpu_cases::NeutralModel;
    Check(recover(TagCopy::NONE, false).reset == 1,
        "a complete Native group recovers with a model Reset after the previous second-role failure");
    const auto before_scope_loss = read_progress();
    const auto no_scope = record({.token = ++token, .source_scope = false});
    const auto after_scope_loss = read_progress();
    Check(no_scope.reuses == 0 && !stream.native_edit_source && after_scope_loss.anchor == nullptr
            && after_scope_loss.claims == before_scope_loss.claims && after_scope_loss.outputs == before_scope_loss.outputs,
        "missing native SDK scope preserves full-rate fallback without reusing an anchor or claiming a common frame");
    Check(recover(TagCopy::NONE, false).reset == 1,
        "restored SDK source starts Native model history without a caller Reset");
    const auto refreshed = read_progress();
    Check(refreshed.anchor != nullptr && refreshed.anchor->valid && stream.native_edit_history,
        "a fresh Native SDK frame tracks its live anchor before the Off transition");
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
    const auto tagged_refresh = record({.token = ++token, .reset = true, .tag_copy = TagCopy::SUCCESS, .sdk_wrapper = true});
    Check(tagged_refresh.replaced && tagged_refresh.evaluates == 1 && tagged_refresh.reuses == 0
            && tagged_refresh.reset == 1,
        "deferred native tag-copy produces one actual-model refresh after producer scopes end");
    auto tagged_anchor = native::features.at(role_handle).slots[0].edit_reuse;
    Check(tagged_anchor != nullptr && tagged_anchor->valid, "deferred native tag-copy establishes a private edit anchor");
    const auto tagged_anchor_generation = tagged_anchor->anchor_generation;
    const auto tagged_reuse = record({.token = ++token, .tag_copy = TagCopy::SUCCESS, .sdk_wrapper = true});
    Check(tagged_reuse.replaced && tagged_reuse.evaluates == 0 && tagged_reuse.reuses == 1
            && matches(tagged_reuse.consumed_back, source_back)
            && matches(tagged_reuse.consumed_hudless, source_hudless),
        "a subsequent completed tag-copy reuses edits on another thread with current PQ colors and alpha");
    const auto tagged_progress = read_progress();
    Check(tagged_progress.kind == frame_policy::SourceKind::TAGGED_RESOURCE
            && tagged_progress.source == output_identity && tagged_progress.source_generation == tagged_reuse.source_generation
            && tagged_progress.source_frame == static_cast<uint64_t>(token) + 1 && stream.base_frame_clock.complete,
        "the production tag-copy observation supplies the common Native frame identity for the complete output");
    const auto tagged_application_frame = stream.application_frame;
    const auto tagged_native_frame = stream.native_edit_frame;
    const auto tagged_mfg = record({.token = token, .index = 2, .update_source = false, .tag_copy = TagCopy::SUCCESS});
    Check(tagged_mfg.replaced && tagged_mfg.evaluates == 0 && tagged_mfg.reuses == 0
            && stream.application_frame == tagged_application_frame && stream.native_edit_frame == tagged_native_frame
            && tagged_anchor->anchor_generation == tagged_anchor_generation && read_progress() == tagged_progress
            && tagged_mfg.consumed_back == tagged_reuse.consumed_back
            && tagged_mfg.consumed_hudless == tagged_reuse.consumed_hudless,
        "deferred MFG reconsumes the same tagged clone without advancing NR or cache cadence");
    const auto tagged_next = record({.token = ++token, .tag_copy = TagCopy::SUCCESS, .sdk_wrapper = true});
    Check(tagged_next.replaced && tagged_next.evaluates == 1 && tagged_next.reuses == 0 && tagged_next.reset == 0,
        "next real tagged token refreshes after deferred reuse without a false history Reset");
    tagged_anchor.reset();
    for (const auto denied : {TagCopy::FAILED, TagCopy::OVERWRITTEN}) {
      const auto before_denial = read_progress();
      const auto fallback = record({.token = ++token, .tag_copy = denied});
      const auto after_denial = read_progress();
      Check(!fallback.replaced && fallback.evaluates == 0 && fallback.reuses == 0
              && after_denial.claims == before_denial.claims && after_denial.outputs == before_denial.outputs
              && matches(fallback.restored_back, source_back),
          "failed or overwritten tag-copy rejects the old edit cache and preserves the current original BB");
      recover(TagCopy::SUCCESS);
    }
    const auto sdk_generation = rate::NativeBindingGeneration(rate::MatchNativeSwapchain(output_identity));
    uint64_t direct_generation = 0;
    for (uint32_t attempt = 0; attempt < 8 && direct_generation < sdk_generation; ++attempt) {
      {
        const addon::RuntimeLock lock(native::runtime_mutex);
        tag_source::Reset();
      }
      const auto owner_unavailable = record({.token = ++token, .tag_copy = TagCopy::DIRECT, .owner_ready = false});
      Check(!owner_unavailable.replaced && owner_unavailable.evaluates == 0 && owner_unavailable.reuses == 0
              && matches(owner_unavailable.consumed_back, source_back)
              && matches(owner_unavailable.restored_back, source_back),
          "direct FG consumption before owner readiness leaves current color intact without NR or cache output");
      direct_generation = owner_unavailable.source_generation;
    }
    Check(direct_generation != 0 && direct_generation == sdk_generation,
        "real tag retirement and reacquisition exercise a collision between the independent generation counters");
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
    const auto direct_progress = read_progress();
    Check(direct_progress.kind == frame_policy::SourceKind::TAGGED_RESOURCE
            && direct_progress.source == output_identity && direct_progress.source_generation == direct_reuse.source_generation
            && direct_progress.source_frame == static_cast<uint64_t>(token) + 1 && stream.base_frame_clock.complete,
        "the uniquely claimed direct tag supplies the common Native frame identity for the complete output");
    const auto direct_mfg = record({.token = token, .index = 2, .update_source = false, .tag_copy = TagCopy::DIRECT});
    Check(direct_mfg.replaced && direct_mfg.evaluates == 0 && direct_mfg.reuses == 0
            && stream.application_frame == direct_application_frame && stream.native_edit_frame == direct_native_frame
            && stream.native_edit_source_epoch == direct_epoch && direct_anchor->anchor_generation == direct_anchor_generation
            && read_progress() == direct_progress && direct_mfg.consumed_back == direct_reuse.consumed_back
            && direct_mfg.consumed_hudless == direct_reuse.consumed_hudless,
        "direct-tag MFG keeps its claimed epoch without advancing NR, source cadence or cache");
    const auto direct_next = record({.token = ++token, .tag_copy = TagCopy::DIRECT});
    Check(direct_next.replaced && direct_next.evaluates == 1 && direct_next.reuses == 0 && direct_next.reset == 0,
        "the next real direct FrameToken refreshes after reuse without a false history Reset");
    direct_anchor.reset();
    {
      const auto tagged = read_progress();
      Check(tagged.kind == frame_policy::SourceKind::TAGGED_RESOURCE
              && tagged.source_generation == rate::NativeBindingGeneration(rate::MatchNativeSwapchain(output_identity)),
          "the direct-tag and SDK producers naturally have the same numeric generation");
      const auto sdk_first = record({.token = token, .update_source = false});
      const auto sdk_claim = read_progress();
      Check(sdk_first.source_generation == tagged.source_generation && sdk_claim.kind == frame_policy::SourceKind::SDK_SCOPE
              && sdk_claim.source == tagged.source && sdk_claim.source_generation == tagged.source_generation
              && sdk_claim.source_frame == tagged.source_frame && sdk_claim.claims == tagged.claims + 1
              && sdk_claim.observed_frames == tagged.observed_frames + 1,
          "an actual SDK scope claims a new source domain despite the direct tag's equal generation and token");
      Check(!sdk_first.replaced && sdk_first.evaluates == 0 && sdk_first.reuses == 0
              && sdk_claim.cached == nullptr && !sdk_claim.complete && sdk_claim.outputs == tagged.outputs,
          "switching to an equal-number SDK frame retires the tagged result and prepares its own model");
      const auto sdk_refresh = recover(TagCopy::NONE, false);
      const auto sdk_reuse = record({.token = ++token});
      Check(sdk_refresh.reset == 1 && sdk_reuse.replaced && sdk_reuse.evaluates == 0 && sdk_reuse.reuses == 1,
          "the SDK producer recovers with Reset and reuses only its own new edit anchor");
      const auto sdk = read_progress();
      const auto tagged_first = record({.token = --token, .tag_copy = TagCopy::DIRECT});
      const auto tagged_claim = read_progress();
      Check(tagged_first.source_generation == sdk.source_generation
              && tagged_claim.kind == frame_policy::SourceKind::TAGGED_RESOURCE
              && tagged_claim.source == sdk.source && tagged_claim.source_generation == sdk.source_generation
              && tagged_claim.source_frame + 1 == sdk.source_frame && tagged_claim.claims == sdk.claims + 1
              && tagged_claim.observed_frames == sdk.observed_frames + 1,
          "a real unique direct tag claims its distinct domain even with a smaller token in the same numeric generation");
      Check(!tagged_first.replaced && tagged_first.evaluates == 0 && tagged_first.reuses == 0
              && tagged_claim.cached == nullptr && !tagged_claim.complete && tagged_claim.outputs == sdk.outputs,
          "switching back to the smaller tagged token prepares fresh history without returning the SDK cache");
      const auto tagged_recovered = recover(TagCopy::DIRECT, false);
      const auto tagged_recovered_reuse = record({.token = ++token, .tag_copy = TagCopy::DIRECT});
      Check(tagged_recovered.reset == 1 && tagged_recovered_reuse.replaced
              && tagged_recovered_reuse.evaluates == 0 && tagged_recovered_reuse.reuses == 1,
          "the direct producer resumes refresh/reuse using its own restored source identity");
    }
    const auto before_ambiguity = read_progress();
    const auto ambiguous_direct = record({.token = ++token, .tag_copy = TagCopy::DIRECT_AMBIGUOUS});
    const auto after_ambiguity = read_progress();
    Check(!ambiguous_direct.replaced && ambiguous_direct.evaluates == 0 && ambiguous_direct.reuses == 0
            && after_ambiguity.claims == before_ambiguity.claims && after_ambiguity.outputs == before_ambiguity.outputs
            && matches(ambiguous_direct.restored_back, source_back),
        "a recorded overwrite cannot make a later direct token unique; ambiguity preserves the current BB");
    ++token;
    {
      const addon::RuntimeLock lock(native::runtime_mutex);
      tag_source::Reset();
    }
    const auto before_missing_role = read_progress();
    const auto missing_role = record({.token = ++token, .tag_copy = TagCopy::DIRECT, .omit_alpha_tag = true});
    Check(!missing_role.replaced && missing_role.evaluates == 0 && missing_role.reuses == 0
            && read_progress().claims == before_missing_role.claims && matches(missing_role.restored_back, source_back),
        "an incomplete async color group cannot claim, infer or replace a basic frame");
    for (const auto change : {TagChange::WRITE_ALPHA, TagChange::PRESENT_END, TagChange::RETAG_HUDLESS, TagChange::RETAG_ALPHA}) {
      {
        const addon::RuntimeLock lock(native::runtime_mutex);
        tag_source::Reset();
      }
      const auto before = read_progress();
      const auto stale = record({.token = ++token, .tag_copy = TagCopy::SUCCESS, .sdk_wrapper = true, .tag_change = change});
      Check(!stale.replaced && stale.evaluates == 0 && stale.reuses == 0
              && read_progress().claims == before.claims && read_progress().outputs == before.outputs
              && matches(stale.restored_back, source_back),
          "the actual asynchronous NGX detour keeps original inputs when a tagged role changes or expires");
      ++token;
    }
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
    rate::DestroyPresentOutput(context);
    backbuffer = gpu.Texture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    output->current_back_buffer = backbuffer.Get();
    auto& fp16_owner = native::present_streams[nullptr];
    fp16_owner.device_identity = renodx::utils::directx::NativeIdentity(gpu.device.Get());
    fp16_owner.native_output_identity = reinterpret_cast<const void*>(output_identity);
    fp16_owner.backbuffer_width = width;
    fp16_owner.backbuffer_height = height;
    fp16_owner.backbuffer_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    fp16_owner.prefg_target_valid = true;
    auto& fp16_target = fp16_owner.prefg_target;
    native::SnapshotPresentRestoreConfiguration(&fp16_target);
    fp16_target.private_signed_runtime = true;
    fp16_target.hdr_mode = 1;
    fp16_target.units = {renodx::dlss5::codec::Encoding::Linear, true, 80.f, false};
    fp16_target.hdr_codec_mode = 3;
    fp16_target.hdr_white_nits = 80.f;
    fp16_target.hdr_encoding = native::V6EncodingConstant(fp16_target.units);
    fp16_target.diagnostic.bypass_host_look = true;
    fp16_target.ui_correction = false;
    const auto fp16_context = reinterpret_cast<uintptr_t>(&fp16_owner);
    const uintptr_t fp16_buffers[]{reinterpret_cast<uintptr_t>(
        renodx::utils::directx::NativeIdentity(backbuffer.Get()))};
    rate::RegisterPresentOutput(fp16_context, reinterpret_cast<uintptr_t>(fp16_owner.device_identity),
        output_identity, fp16_buffers);
    addon::bridge::OwnedParameters fp16_parameters = parameters;
    fp16_parameters.Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, backbuffer.Get());
    fp16_parameters.Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, static_cast<ID3D12Resource*>(nullptr));
    fp16_parameters.Set(NVSDK_NGX_DLSSG_Parameter_UIAlpha, static_cast<ID3D12Resource*>(nullptr));
    fp16_parameters.Set(NVSDK_NGX_DLSSG_Parameter_UserInterfaceRecompositionEnabled, 0u);
    fp16_parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, 1u);
    const auto* fp16_handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xF045});
    native::RegisterCreatedFeatureLocked(fp16_handle, NVSDK_NGX_Feature_FrameGeneration, &fp16_parameters);
    static Snapshot* sdk_color = nullptr;
    funnel_accounting_tests::Restore fp16_substitutions;
    fp16_substitutions.Value(&native::ngx_slot_real[0].evaluate);
    native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(+[](ID3D12GraphicsCommandList* list,
        const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter* block, PFN_NVSDK_NGX_ProgressCallback) {
      Check(list == model_gpu->list.Get(), "the FG wrapper forwards the real WARP command list");
      *sdk_color = Capture(*model_gpu, native::PreFgResource(block, NVSDK_NGX_DLSSG_Parameter_Backbuffer),
          NATIVE_INPUT_STATE);
      return NVSDK_NGX_Result_Success;
    });
    bool fp16_initialized = false;
    const auto fp16_record = [&](bool reset) {
      std::vector<uint16_t> input(width * height * 4);
      for (size_t value = 0; value < input.size(); value += 4) {
        input[value] = DirectX::PackedVector::XMConvertFloatToHalf(0.5f);
        input[value + 1] = DirectX::PackedVector::XMConvertFloatToHalf(0.625f);
        input[value + 2] = DirectX::PackedVector::XMConvertFloatToHalf(0.75f);
        input[value + 3] = DirectX::PackedVector::XMConvertFloatToHalf((token % 4 + 1) * 0.25f);
      }
      upload(backbuffer.Get(), input, fp16_initialized ? NATIVE_INPUT_STATE : D3D12_RESOURCE_STATE_COMMON);
      fp16_initialized = true;
      fp16_parameters.Set(NVSDK_NGX_DLSSG_Parameter_Reset, reset ? 1u : 0u);
      const auto before_evaluates = model_calls;
      const auto before_reuses = native::edit_reuse_reuses.load();
      Snapshot consumed;
      sdk_color = &consumed;
      run_sdk(++token, [&] {
        Check(NVSDK_NGX_SUCCEED(native::HookedEvaluateFeatureSlot<0>(
                  gpu.list.Get(), fp16_handle, &fp16_parameters, nullptr)),
            "the real NGX wrapper preserves the software SDK result");
      });
      sdk_color = nullptr;
      const auto restored = Capture(gpu, backbuffer.Get(), NATIVE_INPUT_STATE);
      submit();
      const auto pixels = consumed.Pixels();
      Check(pixels == restored.Pixels() && pixels.size() == input.size() * sizeof(uint16_t),
          "accepted FP16 SDK input remains on its real Backbuffer after the wrapper");
      for (size_t alpha = 3; alpha < input.size(); alpha += 4) {
        uint16_t actual;
        std::memcpy(&actual, pixels.data() + alpha * sizeof(actual), sizeof(actual));
        Check(actual == input[alpha], "FP16 writeback keeps the current alpha");
      }
      return std::pair{model_calls - before_evaluates, native::edit_reuse_reuses.load() - before_reuses};
    };
    bool fp16_ready = false;
    for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 12; ++attempt) {
      const auto phase = fp16_record(true);
      if (phase.first == 1 && native::fg_service::streams.at({true, fp16_context}).base_frame_clock.complete) {
        fp16_ready = true;
        break;
      }
    }
    Check(fp16_ready && fp16_record(false) == std::pair{0u, uint64_t{1}}
            && fp16_record(false) == std::pair{1u, uint64_t{0}},
        "the registered SDK and actual NGX wrappers deliver FP16 refresh, reuse and refresh");
    native::present_pre_fg = 0;
    Check(fp16_record(false) == std::pair{0u, uint64_t{0}},
        "NRPresentPreFg=0 leaves the FP16 SDK path untouched");
    {
      const addon::RuntimeLock lock(native::runtime_mutex);
      native::RetirePresentStream(nullptr);
      native::ForgetSourceHandleLocked(fp16_handle);
    }
    submit();
    rate::DestroyPresentOutput(fp16_context);
    native_adapter::state.present = nullptr;
    native_adapter::decisions_ready = false;
    Check(SUCCEEDED(proof_list->Close()), "close the unsubmitted foreign proof-query list");
    nr_edit_reuse_xefg_cases::model_pixels.clear();
    model_gpu = nullptr;
    check_debug();
    std::cout << "NR_RELEASE_PARITY native-prefg-present-x2 synchronous source=successful-present-start+sdk-present"
                 " same-scope=1 ngx-detour=executed pq-evaluates=1/0/1/0 fp16-evaluates=1/0/1 fp16-alpha=preserved\n"
                 "NR_RELEASE_PARITY native-prefg-present-x2 synchronous-source-refusal"
                 " missing-present-start=refused failed-present-start=refused wrong-sdk-scope=refused\n"
                 "NR_RELEASE_PARITY native-prefg-present-x2 asynchronous source=set-tag-for-frame+copy+fence"
                 " ngx-detour=executed evaluates=1/0/1 color=complete stale-alpha/expired/role-retag=original\n"
                 "NR_RELEASE_PARITY native-prefg-present-x2 mfg-recovery"
                 " repeated=redelivered missing-role/changed-guide/foreign-list=original"
                 " sdk-reject=ngx-detour+rgba-rollback recovery=reset+refresh\n"
                 "NR_RELEASE_PARITY native-prefg-present-x2 rejected-redelivery"
                 " sdk-reject=ngx-detour sdk-input=edited-rgba backbuffer=exact-rgba bindings=restored"
                 " cache-history-base=unchanged retry=redelivered\n";
    std::cout << "PASS native Pre-FG x2 evidence=WARP+MOCK format=HDR10-PQ,FP16"
                 " roles=BB+HUDLess+UIAlpha,FP16_BB"
                 " frame_source=registered_PresentStart_and_SDK_scope numeric_NGX_FrameID=absent"
                 " deferred_frame_source=registered_tag_wrapper_MOCK_CopyResource_real cross_thread_outside_SDK_scope=checked"
                 " deferred_refresh_reuse_refresh=1/0/1 tag_unsubmitted/failed/overwritten=denied"
                 " direct_UntilPresent_no_clone=checked direct_refresh_reuse_refresh=1/0/1 direct_pending_ambiguity=denied"
                 " direct_warmup_MFG_claim=stable"
                 " direct_unclaimed_recorded_overwrite=invalidated_and_retained"
                 " direct_owner_unavailable=claimed_and_tracked_without_NR"
                 " baseline_repeat_and_late=1/0/1/0 common_frame_and_output=checked repeated_MFG=stable current_color_alpha=checked"
                 " late_F1_same_and_changed_roles=F2_preserved provider_same_generation_equal_and_lower_tokens=distinct"
                 " late_F1_SDK_failure=F2_preserved SDK_scope_wrap=max/zero/one late_max_after_wrap=denied"
                 " unsubmitted_F2_late_F1_failure=PREPARED_to_SUBMITTED_once"
                 " partial_BB_failure=no_output missing_role_and_foreign_unsubmitted=denied"
                 " host_replay=two_GPU_executions_one_CPU_claim replayed_cache=denied recovery=checked"
                 " adaptive_unacquired=0_evaluate/1_reuse adaptive_active_upper=refresh/reuse"
                 " adaptive_low_per_frame=edited_complete/ordered_MFG/reject_pending_missing_version_new_token_alpha_change_guide_overwrite"
                 " adaptive_tokenless_after_prune=two_delivered fixed_recovery=refresh/reuse/MFG"
                 " motion_rejection/reset/gap/off/missing_scope/FG_failure/retirement=checked"
                 " production_Execute_and_Fence=real NR=MOCK FG_consumer=readback"
                 " per_output_identity=Unknown no_game_or_visual_acceptance\n";
    return 0;
  } catch (const std::exception& error) {
    fail_call = 0;
    {
      const addon::RuntimeLock lock(native::runtime_mutex);
      tag_source::Reset();
    }
    native_adapter::state.present = nullptr;
    native_adapter::decisions_ready = false;
    std::cerr << "nr-edit-reuse-native-contracts: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace nr_edit_reuse_native_cases
