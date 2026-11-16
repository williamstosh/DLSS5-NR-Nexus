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
#include <vector>

#include <DirectXPackedVector.h>

#include <edit_reuse.hpp>
#include <native_fg_frame.hpp>

// Included after the native host, shared readbacks and edit-model substitutes.
namespace nr_edit_reuse_native_cases {
namespace addon = renodx::addons::dlss5;
namespace frame_source = addon::fg::native_options::frame_source;
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
      native::PruneCompletedSubmissionUses();
      gpu.uploads.clear();
      gpu.retained_resources.clear();
      native::DrainRetiredResources(true);
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
    frame_source::Activate();
    std::vector<uint32_t> source_back(width * height), source_hudless(width * height);
    std::vector<uint8_t> source_ui(width * height * 4);
    bool initialized = false;
    struct Request {
      uint32_t token = 0;
      bool reset = false;
      uint32_t index = 1;
      bool source_scope = true;
      bool fg_success = true;
      bool update_source = true;
      bool reject_motion = false;
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
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_Reset, request.reset ? 1u : 0u);
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, request.index);
      Observation observed;
      const auto before_calls = model_calls;
      const auto before_reuses = native::edit_reuse_reuses.load();
      Snapshot consumed_back, consumed_hudless, restored_back, original_hudless, unchanged_ui;
      const auto consume = [&] {
        auto* original = native::RunNativePreFg(gpu.list.Get(), &parameters, handle, &observed.replaced);
        auto* bound_hudless = native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess);
        if (observed.replaced) {
          Check(original == hudless.Get() && bound_hudless != hudless.Get() && bound_hudless != backbuffer.Get(),
              "native FG consumes a private HUDLess result and returns the borrowed original binding");
        } else {
          Check(original == nullptr && bound_hudless == hudless.Get(),
              "unprepared native input keeps its original HUDLess binding");
        }
        Check(native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_UIAlpha) == ui.Get(),
            "native exact alpha stays bound at FG consumption");
        consumed_back = Capture(gpu, backbuffer.Get(), NATIVE_INPUT_STATE);
        consumed_hudless = Capture(gpu, bound_hudless, NATIVE_INPUT_STATE);
        native::RestoreNativePreFg(gpu.list.Get(), &parameters, original, request.fg_success, handle, observed.replaced);
        Check(native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess) == hudless.Get()
                && native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_UI) == nullptr
                && native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_UIAlpha) == ui.Get(),
            "native FG success/failure restores original HUDLess/UI/alpha bindings");
        restored_back = Capture(gpu, backbuffer.Get(), NATIVE_INPUT_STATE);
        original_hudless = Capture(gpu, hudless.Get(), NATIVE_INPUT_STATE);
        unchanged_ui = Capture(gpu, ui.Get(), NATIVE_INPUT_STATE);
      };
      if (request.source_scope) {
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
      Check(original_hudless_pixels.size() == source_hudless.size() * sizeof(uint32_t)
              && std::memcmp(original_hudless_pixels.data(), source_hudless.data(), source_hudless.size() * sizeof(uint32_t)) == 0,
          "FG never mutates the game's original HUDLess pixels");
      Check(unchanged_ui.Pixels() == source_ui, "FG never mutates original UI/alpha pixels");
      return observed;
    };
    const auto matches = [](const std::vector<uint8_t>& pixels, const std::vector<uint32_t>& source) {
      return pixels.size() == source.size() * sizeof(uint32_t)
          && std::memcmp(pixels.data(), source.data(), pixels.size()) == 0;
    };
    uint32_t token = 100;
    const auto recover = [&] {
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 12; ++attempt) {
        const auto observed = record({.token = ++token, .reset = true});
        Check(observed.reuses == 0, "native recovery cannot reuse an invalidated edit anchor");
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
    Check(stream.native_edit_reuse && stream.native_edit_frame == static_cast<uint64_t>(token) + 1
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
    Check(no_scope.reuses == 0 && !native::fg_service::streams.at({true, context}).native_edit_reuse,
        "missing native SDK scope returns to the existing full-rate path and cannot consume a previous edit anchor");
    recover();
    native::OnNativeFgOptions(false);
    const auto off = record({.token = ++token});
    Check(!off.replaced && off.evaluates == 0 && off.reuses == 0
            && matches(off.restored_back, source_back),
        "explicit native FG Off retains the original safe path without cached edits");
    native::OnNativeFgOptions(true);
    recover();
    auto final_anchor = native::features.at(role_handle).slots[0].edit_reuse;
    Check(final_anchor != nullptr, "recovered native output owns its final private cache");
    std::weak_ptr<addon::edit_reuse::State> retired_anchor = final_anchor;
    final_anchor.reset();
    frame_source::Stop();
    native::RetirePresentStream(nullptr);
    native::ForgetSourceHandleLocked(handle);
    submit();
    Check(!native::fg_service::streams.contains({true, context}) && !native::features.contains(role_handle)
            && retired_anchor.expired() && native::retired_nr_features.empty()
            && native::retired_final_resources.empty(),
        "native source retirement releases all edit caches, models and worksets after actual Fence completion");
    native::edit_reuse_fg_experiment = false;
    nr_edit_reuse_xefg_cases::model_pixels.clear();
    model_gpu = nullptr;
    check_debug();
    std::cout << "PASS native Pre-FG x2 evidence=WARP+MOCK format=HDR10-PQ roles=BB+HUDLess+UIAlpha"
                 " frame_source=Reflex_PresentStart_and_SDK_scope numeric_NGX_FrameID=absent"
                 " refresh_reuse_refresh=1/0/1 repeated_MFG=stable current_color_alpha=checked"
                 " motion_rejection/reset/gap/off/missing_scope/FG_failure/retirement=checked"
                 " production_Execute_and_Fence=real NR=MOCK FG_consumer=readback no_game_or_visual_acceptance\n";
    return 0;
  } catch (const std::exception& error) {
    fail_call = 0;
    frame_source::Stop();
    native::edit_reuse_fg_experiment = false;
    std::cerr << "nr-edit-reuse-native-contracts: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace nr_edit_reuse_native_cases
