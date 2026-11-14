/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <exception>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

namespace adaptive_inline_cases {
namespace addon = renodx::addons::dlss5;

int Run() {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu(false, 16, 16);
    model_gpu = &gpu;
    ComPtr<ID3D12InfoQueue> info;
    gpu.device.As(&info);
    native::enabled = true;
    native::hooks_enabled = true;
    Check(native::dedupe_mode.load() == native::defaults::kDedupeMode,
        "inline adaptive uses the production default deduplication policy");
    native::cost_meter_enabled = false;
    native::gpu_timers_enabled = false;
    native::stack_passes = 1;
    native::nr_follow_input_res = false;
    native::nr_resolution_scale = 1.f;
    native::codec_mode = 0;
    native::feed_mode = 0;
    native::look_mode = false;
    native::detail_stability = 0;
    native::chained_temporal_history = true;
    native::transfer_strength = 1.f;
    native::color_strength = 1.f;
    native::control_mask_enabled = true;
    native::nr_adaptive_selected_levels = 0;
    native::nr_adaptive_min_scale = .33f;
    native::nr_adaptive_target_fps = 116.f;
    Check(native::adaptive_feature_mode.load() == native::AdaptiveFeatureMode::OFF, "Feature capacity experiment defaults to disabled");
    native::max_worksets = 4;
    native::OnNativeFgOptions(false);
    native::adaptive_configuration_settings = native::comparison_ui::CaptureAdaptiveSettings;
    addon::gpu_timers::comparison_settings = native::comparison_ui::CaptureSettings;
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = CreateModel;
    native::direct_api.evaluate = EvaluateModel;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::successful_sr_device = gpu.device.Get();
    native::direct_runtime_requirements.dual_models = true;
    native::direct_runtime_requirements.file_version = "310.9.16.0";
    native::parameter_runtime_ready = true;
    native::core_allocate_parameters = addon::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = addon::bridge::DestroyOwnedParameters;
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "inline adaptive real queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "inline adaptive real list hooks");
    const auto texture = [&](DXGI_FORMAT format, bool writable, uint32_t size = 16) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = size, .Height = size, .DepthOrArraySize = 1, .MipLevels = 1,
          .Format = format, .SampleDesc = {1, 0},
          .Flags = (writable ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE)};
      ComPtr<ID3D12Resource> resource;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
          &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))), "inline adaptive texture");
      gpu.Write(resource.Get(), 0);
      gpu.Transition(resource.Get(), D3D12_RESOURCE_STATE_COMMON,
          writable ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : NATIVE_INPUT_STATE);
      return resource;
    };
    std::array colors{texture(DXGI_FORMAT_R8G8B8A8_UNORM, false), texture(DXGI_FORMAT_R8G8B8A8_UNORM, false)};
    std::array outputs{texture(DXGI_FORMAT_R8G8B8A8_UNORM, true), texture(DXGI_FORMAT_R8G8B8A8_UNORM, true)};
    auto motion = texture(DXGI_FORMAT_R16G16_FLOAT, false);
    auto depth = texture(DXGI_FORMAT_R32_FLOAT, false);
    gpu.Submit();
    const auto submit = [&](bool reset = true) {
      Check(SUCCEEDED(gpu.list->Close()), "inline adaptive recording close");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      gpu.queue->ExecuteCommandLists(1, lists);
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "inline adaptive Fence signal");
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr, "inline adaptive Fence event");
      const auto signal = gpu.fence->SetEventOnCompletion(gpu.serial, event);
      const auto wait = (SUCCEEDED(signal) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
      CloseHandle(event);
      Check(wait == WAIT_OBJECT_0, "inline adaptive bounded GPU completion");
      if (reset) {
        Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
            "inline adaptive completed recording reset");
      }
      native::DrainRetiredResources(true);
      addon::gpu_timers::Poll(true, false);
      gpu.uploads.clear();
    };
    addon::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
    parameters.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
    parameters.Set(NVSDK_NGX_Parameter_Width, 16u);
    parameters.Set(NVSDK_NGX_Parameter_Height, 16u);
    parameters.Set(NVSDK_NGX_Parameter_OutWidth, 16u);
    parameters.Set(NVSDK_NGX_Parameter_OutHeight, 16u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 16u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 16u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
        static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_Reset, 0u);
    uint32_t active_successes = 0;
    uint32_t preparation_calls = 0;
    uint32_t preparation_failures = 0;
    const NVSDK_NGX_Handle* expected_reset_model = nullptr;
    bool observed_expected_reset = false;
    model_parameter_observer = [&](const NVSDK_NGX_Handle* model, const NVSDK_NGX_Parameter* model_parameters) {
      if (model == expected_reset_model && native::preparing_adaptive_frame == nullptr) {
        observed_expected_reset |= native::GetInt(model_parameters, "DLSSNR.Reset") != 0;
      }
      const bool failing = model_calls + 1 == fail_call;
      if (native::preparing_adaptive_frame != nullptr) {
        ++preparation_calls;
        preparation_failures += failing;
      } else if (!failing) {
        ++active_successes;
      }
    };
    for (const auto hook : {native::kHookUpscaled, native::kHookRender}) {
      native::hook_point = hook;
      native::nr_adaptive_enabled = false;
      const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADA100} + hook);
      native::RegisterCreatedFeatureLocked(handle, native::kFeatureDlss, &parameters);
      uint32_t frame = 0;
      const auto evaluate = [&] {
        ++native::present_generation;
        const auto before_source = native::SourceSuccessfulEvaluations();
        const auto before_active = active_successes;
        const auto before_inline = native::successful_evaluations.load();
        const auto before_pre_sr = native::successful_pre_sr_evaluations.load();
        const uint32_t output = frame++ % 2;
        parameters.Set(NVSDK_NGX_Parameter_Color, colors[output].Get());
        parameters.Set(NVSDK_NGX_Parameter_Output, outputs[output].Get());
        bool accepted = false;
        {
          const addon::EvaluateChainScope chain;
          const addon::SourceStageScope source(handle, true);
          if (hook == native::kHookRender) {
            native::PreSrSwap swap;
            accepted = native::ProcessInlinePreSR(gpu.list.Get(), handle, &parameters, swap);
            native::RestorePreSrColor(&parameters, swap);
          } else {
            accepted = native::ProcessInline(gpu.list.Get(), handle, &parameters);
          }
        }
        const uint64_t completed_active = active_successes - before_active;
        Check(native::SourceSuccessfulEvaluations() == before_source + completed_active,
            "inline source successes exclude additional preparation Evaluate calls");
        Check(native::successful_evaluations.load() == before_inline
                + (hook == native::kHookUpscaled ? completed_active : 0)
            && native::successful_pre_sr_evaluations.load() == before_pre_sr
                + (hook == native::kHookRender ? completed_active : 0),
            "only the active insertion point contributes its successful source frame");
        return accepted;
      };
      const auto record = [&](bool reset = true) {
        const bool accepted = evaluate();
        submit(reset);
        return accepted;
      };
      const auto* occupied_owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADA200} + hook);
      Check(native::EnsureWorkset(gpu.device.Get(), occupied_owner, outputs[0].Get(), 16, 16, 16, 16,
          DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0) != nullptr, "diagnostic pressure uses a real WARP workset");
      native::features[occupied_owner].adaptive_cache = std::make_shared<native::AdaptiveFeatureCache>();
      native::features.at(occupied_owner).adaptive_cache->last_used_ns = native::SteadyNowNs();
      native::features.at(occupied_owner).adaptive_cache->resident = true;
      native::features.at(occupied_owner).adaptive_cache->count = 1;
      native::features.at(occupied_owner).adaptive_cache->output_identities.insert(reinterpret_cast<uintptr_t>(outputs[0].Get()));
      native::max_worksets = 1;
      const auto calls_before = model_calls;
      const auto setup_failures = addon::nr_decline_counts[static_cast<size_t>(addon::NrDeclineReason::kWorksetSetupFailed)].load();
      Check(!record(), "real capacity pressure rejects setup before model evaluation");
      auto& failed_slot = native::features.at(handle).slots[0];
      Check(failed_slot.failed && failed_slot.fail_count == 0 && model_calls == calls_before
          && addon::nr_decline_counts[static_cast<size_t>(addon::NrDeclineReason::kWorksetSetupFailed)].load() == setup_failures + 1,
          "capacity rejection reaches the unchanged setup-failure latch");
      Check(std::any_of(captured_logs.begin(), captured_logs.end(), [](const auto& entry) {
        return entry.second.find("reason=adaptive_owners_active workset_create_api=not_called") != std::string::npos;
      }), "setup diagnostics identify active cached owners and an uncalled workset creation API");
      Check(!record() && failed_slot.failed && failed_slot.fail_count == 1,
          "the original first retry remains immediate and records its next failure");
      const auto failure_ns = failed_slot.failed_ns;
      const auto backoffs = addon::nr_decline_counts[static_cast<size_t>(addon::NrDeclineReason::kFailureBackoff)].load();
      native::RetireWorkset({occupied_owner, outputs[0].Get()});
      native::features.erase(occupied_owner);
      native::DrainRetiredResources(true);
      native::max_worksets = 4;
      Check(!record() && failed_slot.failed && failed_slot.failed_ns == failure_ns && failed_slot.fail_count == 1
          && model_calls == calls_before
          && addon::nr_decline_counts[static_cast<size_t>(addon::NrDeclineReason::kFailureBackoff)].load() == backoffs + 1,
          "releasing pressure does not bypass the original one-second feature backoff");
      failed_slot.failed_ns = native::SteadyNowNs() - native::kFeatureRetryFirstNs - 1;
      record();
      Check(!failed_slot.failed,
          "the expired original backoff retries after pressure releases");
      std::cout << "DIAGNOSTIC_SETUP evidence=WARP+MOCK hook=" << hook
          << " reason=adaptive_owners_active workset_create_api=not_called backoff=unchanged recovery=passed\n";
      for (uint32_t warmup = 0; warmup < 8; ++warmup) record();
      Check(record(false), "manual inline NR records successfully before parking its actual GPU-complete list");
      auto parked_list = gpu.list;
      auto parked_allocator = gpu.allocator;
      const auto* parked_model = native::features.at(handle).slots[0].handle;
      const auto parked_releases = std::count(model_released_handles.begin(), model_released_handles.end(), parked_model);
      const auto parked_recording = addon::submission::ObserveRecording(parked_list.Get());
      Check(parked_recording->submitted.load() && !parked_recording->closed.load()
          && !addon::submission::ResourceReleasable(parked_model),
          "the completed active recording remains replayable and retains its model lifetime");
      ComPtr<ID3D12CommandAllocator> next_allocator;
      ComPtr<ID3D12GraphicsCommandList> next_list;
      Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
          IID_PPV_ARGS(&next_allocator))), "inline adaptive second allocator");
      Check(SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
          next_allocator.Get(), nullptr, IID_PPV_ARGS(&next_list))), "inline adaptive second list");
      gpu.allocator = next_allocator;
      gpu.list = next_list;
      Check(native::InstallCommandListStateHooks(gpu.list.Get()), "inline adaptive second-list hooks");
      native::nr_adaptive_enabled = true;
      const auto before_preparation = preparation_calls;
      for (uint32_t warmup = 0; warmup < 32; ++warmup) record();
      auto& feature = native::features.at(handle);
      auto cache = feature.adaptive_cache;
      std::cout << "ADAPTIVE_INLINE hook=" << hook << " outputs=" << (cache ? cache->output_count : 0)
          << " tiers=" << (cache ? cache->count : 0) << " failed=" << feature.adaptive_session->failed
          << " reason=" << addon::adaptive_resolution::ReasonText(feature.adaptive_session->status.reason) << '\n';
      Check(cache != nullptr && cache->count == 2 && cache->output_count == 2,
          "inline A/B rotation shares the original four-workset budget across two tiers");
      {
        const addon::adaptive_resolution::Frame adaptive_frame{.slot = 1, .enabled = true};
        const native::AdaptiveFrameScope scope(&adaptive_frame);
        const auto created = native::worksets_created;
        const char* rejection = "old_reason";
        Check(native::EnsureWorkset(gpu.device.Get(), occupied_owner, outputs[0].Get(), 16, 16, 16, 16,
            DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0, false, nullptr, nullptr, false, &rejection) == nullptr
            && rejection != nullptr && std::strcmp(rejection, "adaptive_capacity") == 0
            && native::worksets_created == created,
            "full adaptive capacity exposes its pre-allocation rejection without creating resources");
      }
      native::PollAdaptiveCache(cache.get());
      Check(!cache->failed && !feature.adaptive_session->failed && cache->tiers[0].ready && cache->tiers[1].ready,
          "Render/Upscaled standby tiers complete both output recordings without permanent pause");
      Check(preparation_calls > before_preparation && feature.adaptive_session->status.preparation_cpu_ms > 0.,
          "standby Evaluate calls execute and retain their preparation cost separately from source successes");
      Check(!parked_recording->closed.load() && !addon::submission::ResourceReleasable(parked_model),
          "standby preparation preserves the active tier's replayable recording protection");
      if (hook == native::kHookUpscaled) {
        const auto created_before = native::nr_features_created;
        for (const auto mode : {native::AdaptiveFeatureMode::UPPER_CAPACITY,
                               native::AdaptiveFeatureMode::UPPER_CAPACITY_ON_DEMAND,
                               native::AdaptiveFeatureMode::UPPER_CAPACITY_ADJACENT}) {
          native::adaptive_feature_mode = mode;
          Check(record() && record() && feature.adaptive_cache == cache && cache->count == 2
              && cache->tiers[1].state.slots[0].create_input_width == 5
              && native::nr_features_created == created_before,
              "upper-capacity modes leave the existing R8 rotating-output path and its original small Create capacity unchanged");
        }
        native::adaptive_feature_mode = native::AdaptiveFeatureMode::OFF;
      }
      {
        const auto set = [](const native::Setting& setting, float value) {
          setting.Store(value);
          native::ApplySettingEffect(setting.effect);
        };
        const auto saved_style = native::kSetStyle.Get();
        const auto upper_model = feature.slots[0].handle;
        const auto lower_model = cache->tiers[1].state.slots[0].handle;
        const auto allocations = native::worksets_created;
        const auto models = native::nr_features_created;
        addon::gpu_comparison::Identity before, resources_before, after, resources_after;
        native::comparison_ui::CaptureSettings(&before);
        native::comparison_ui::CaptureAdaptiveSettings(&resources_before);
        set(native::kSetStyle, saved_style == 0.f ? 1.f : 0.f);
        Check(record() && record(), "Evaluate-only steering keeps both active outputs running");
        native::comparison_ui::CaptureSettings(&after);
        native::comparison_ui::CaptureAdaptiveSettings(&resources_after);
        const auto reset_epoch = native::history_reset_epoch.load();
        Check(feature.adaptive_cache == cache && feature.slots[0].handle == upper_model
            && cache->tiers[1].state.slots[0].handle == lower_model
            && native::nr_features_created == models && native::worksets_created == allocations
            && before.settings_hash != after.settings_hash && resources_before.settings_hash == resources_after.settings_hash
            && feature.slots[0].acknowledged_reset_epoch == reset_epoch
            && cache->tiers[1].state.slots[0].acknowledged_reset_epoch != reset_epoch,
            "steering retains compatible allocations and measurement identity while each tier consumes its own History reset");
        feature.adaptive_session->Request(1, native::SteadyNowNs());
        Check(record() && record() && cache->active == 1 && feature.slots[0].handle == lower_model
            && feature.slots[0].acknowledged_reset_epoch == reset_epoch && native::nr_features_created == models,
            "the retained lower model applies the pending steering reset when it is activated");
        feature.adaptive_session->Request(0, native::SteadyNowNs());
        Check(record() && record(), "steering reuse returns to its original upper model");
        set(native::kSetTransferStrength, .5f);
        Check(record() && record() && feature.adaptive_cache == cache && feature.slots[0].handle == upper_model
            && native::history_reset_epoch.load() == reset_epoch && native::nr_features_created == models
            && native::worksets_created == allocations,
            "resolve-only strength changes keep all allocations and do not request a model History reset");

        const auto saved_target = native::kSetAdaptiveTargetFps.Get();
        set(native::kSetAdaptiveTargetFps, saved_target + 1.f);
        Check(record() && record() && feature.adaptive_cache == cache
            && native::nr_features_created == models && native::worksets_created == allocations,
            "target FPS edits retain the entire selected collection");
        set(native::kSetAdaptiveTargetFps, saved_target);
        for (const auto* setting : {&native::kSetMonitorKey, &native::kSetMonitorModeKey}) {
          const auto saved = setting->Get();
          set(*setting, saved == 0.f ? 120.f : 0.f);
          Check(record() && record() && native::nr_features_created == models && native::worksets_created == allocations,
              "monitor hotkeys preserve all selected Feature and workset allocations");
          set(*setting, saved);
        }
        for (const auto& [setting, value] : std::array{
                 std::pair{&native::kSetControlMaskRed, .8f},
                 std::pair{&native::kSetControlMaskGreen, .7f},
                 std::pair{&native::kSetControlMaskBlue, .6f},
                 std::pair{&native::kSetControlMaskDepthNear, .8f},
                 std::pair{&native::kSetControlMaskDepthFar, .99999f},
                 std::pair{&native::kSetControlMaskMotionStart, 0.f},
                 std::pair{&native::kSetControlMaskMotionEnd, 1.f}}) {
          const auto saved = setting->Get();
          set(*setting, value);
          Check(record() && record() && native::nr_features_created == models && native::worksets_created == allocations,
              "RGB and guide thresholds update the actual mask without replacing selected allocations");
          set(*setting, saved);
        }
        const auto now = native::SteadyNowNs();
        cache->last_used_ns = now - 6'000'000'000;
        for (auto& [key, resources] : native::worksets) {
          if (key.handle == handle) resources.last_used_ns = now - 6'000'000'000;
        }
        native::max_worksets = 5;
        Check(native::EnsureWorkset(gpu.device.Get(), occupied_owner, outputs[0].Get(), 16, 16, 16, 16,
            DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0) != nullptr, "a temporary stream owns one real recent workset");
        native::RetireSupersededWorksets(now);
        Check(feature.adaptive_cache == cache && cache->resident
            && std::count_if(native::worksets.begin(), native::worksets.end(),
                [&](const auto& workset) { return workset.first.handle == handle; }) == 4,
            "another active stream cannot retire a selected owner paused longer than five seconds");
        native::RetireWorkset({occupied_owner, outputs[0].Get()});
        native::DrainRetiredResources(true);
        native::max_worksets = 4;
        const auto after_temporary = native::worksets_created;
        Check(native::EnsureWorkset(gpu.device.Get(), occupied_owner, outputs[0].Get(), 16, 16, 16, 16,
            DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0) == nullptr && native::worksets_created == after_temporary,
            "the LRU applies the same resident protection after the five-second idle boundary");
        Check(record() && record() && feature.adaptive_cache == cache && native::nr_features_created == models,
            "the original owner resumes its retained collection without a new Feature");
        const auto before_three_models = native::nr_features_created;
        const auto before_three_worksets = native::worksets_created;
        set(native::kSetAdaptiveLevelCount, 3.f);
        for (uint32_t warmup = 0; warmup < 24; ++warmup) record();
        Check(feature.adaptive_session->settings.selected_levels == 3 && feature.adaptive_session->prepared
            && cache->count == 3 && native::nr_features_created == before_three_models + 1
            && native::worksets_created == before_three_worksets + 2
            && native::adaptive_resident_worksets == 6 && native::EffectiveAdaptiveWorksetLimit() == 10
            && native::max_worksets.load() == 4,
            "three explicit levels own all six real rotating-output worksets without changing the ordinary four-workset setting");
        const auto three_models = native::nr_features_created;
        set(native::kSetAdaptiveLevelCount, 0.f);
        native::max_worksets = 3;
        Check(record() && record() && feature.adaptive_session->status.reason == addon::adaptive_resolution::Reason::BUDGET
            && !feature.adaptive_session->prepared && native::nr_features_created == three_models,
            "Default still rejects a complete two-output collection outside its actual ordinary quota");
        set(native::kSetAdaptiveTargetFps, saved_target + 1.f);
        Check(record() && record() && feature.adaptive_session->status.reason == addon::adaptive_resolution::Reason::BUDGET
            && native::nr_features_created == three_models, "target FPS does not retry a denied collection");
        native::max_worksets = 4;
        set(native::kSetAdaptiveLevelCount, 2.f);
        set(native::kSetAdaptiveTargetFps, saved_target);
        Check(record() && record() && feature.adaptive_cache == cache && feature.adaptive_session->prepared
            && cache->count == 2 && native::nr_features_created == three_models,
            "an explicit selection rechecks budget once and reuses both retained complete-size tuples");

        set(native::kSetAdaptiveMinScale, .5f);
        record();
        Check(!feature.adaptive_session->prepared && !feature.adaptive_session->trial && cache->active == 0,
            "a committed missing endpoint enters preparation before any automatic request");
        for (uint32_t warmup = 0; warmup < 24; ++warmup) record();
        Check(feature.adaptive_cache == cache && feature.adaptive_session->prepared && cache->tiers[1].ready
            && cache->ladder.sizes[1][0].width == 8 && feature.slots[0].handle == upper_model
            && native::nr_features_created == three_models + 1,
            "editing the minimum prepares the missing endpoint while retaining the complete upper allocation");
        feature.adaptive_session->Request(1, native::SteadyNowNs());
        Check(record() && record() && cache->active == 1 && feature.input_width == 8,
            "the prepared endpoint is available immediately when it is requested");
        feature.adaptive_session->Request(0, native::SteadyNowNs());
        Check(record() && record() && cache->active == 0, "the legal upper remains available during selection edits");
        const auto reused_upper = cache->tiers[1].state.slots[0].handle;
        const auto before_reindex_models = native::nr_features_created;
        expected_reset_model = reused_upper;
        observed_expected_reset = false;
        {
          std::unique_lock lock(native::present_restore_configuration.mutex);
          native::nr_resolution_scale.store(.5f);
          ++native::nr_resolution_commit_epochs[0];
        }
        for (uint32_t warmup = 0; warmup < 24; ++warmup) record();
        Check(feature.adaptive_cache == cache && feature.slots[0].handle == reused_upper
            && feature.input_width == 8 && observed_expected_reset && feature.adaptive_session->prepared
            && native::nr_features_created == before_reindex_models + 1,
            "a previously lower complete-size tuple becomes the manual upper with its original Feature and a local History reset");
        expected_reset_model = nullptr;
        {
          std::unique_lock lock(native::present_restore_configuration.mutex);
          native::nr_resolution_scale.store(1.f);
          ++native::nr_resolution_commit_epochs[0];
        }
        set(native::kSetAdaptiveMinScale, .33f);
        for (uint32_t warmup = 0; warmup < 24; ++warmup) record();
        set(native::kSetAdaptiveLevelCount, 0.f);

        set(native::kSetStyle, saved_style);
        set(native::kSetTransferStrength, 1.f);
        Check(record() && record(), "reuse regression restores the original Evaluate and resolve controls");
        native::adaptive_feature_mode = native::AdaptiveFeatureMode::OFF;
        const auto saved_preset = native::kSetPreset.Get();
        native::comparison_ui::CaptureAdaptiveSettings(&resources_before);
        native::kSetPreset.Store(saved_preset == 1.f ? 2.f : 1.f);
        native::comparison_ui::CaptureAdaptiveSettings(&resources_after);
        Check(resources_before.settings_hash != resources_after.settings_hash,
            "model selection remains part of the allocation identity");
        native::kSetPreset.Store(saved_preset);
        const auto settled_models = native::nr_features_created;
        Check(record() && record() && feature.adaptive_cache == cache && cache->active == 0
            && cache->tiers[1].ready && cache->ladder.sizes[1][0].width == 5
            && native::nr_features_created == settled_models,
            "both outputs consume the restored measurement epoch with the original lower tier ready and no Create");
      }
      if (hook == native::kHookRender) {
        auto running_list = gpu.list;
        auto running_allocator = gpu.allocator;
        ComPtr<ID3D12CommandAllocator> pending_allocator;
        ComPtr<ID3D12GraphicsCommandList> pending_list;
        Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&pending_allocator))), "Render target-tier pending allocator");
        Check(SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            pending_allocator.Get(), nullptr, IID_PPV_ARGS(&pending_list))), "Render target-tier pending list");
        gpu.allocator = pending_allocator;
        gpu.list = pending_list;
        Check(native::InstallCommandListStateHooks(gpu.list.Get()), "Render target-tier pending-list hooks");
        native::ActivateAdaptiveTier(&feature, 1);
        feature.adaptive_session->Request(1, native::SteadyNowNs());
        const auto before_pending = model_calls;
        Check(evaluate() && model_calls == before_pending + 1,
            "the target tier owns an actual NR recording that is deliberately left unsubmitted");
        const auto target_recording = addon::submission::ObserveRecording(gpu.list.Get());
        Check(!target_recording->submitted.load() && !target_recording->closed.load(),
            "target-tier History is reserved by its own open unsubmitted recording");
        gpu.allocator = running_allocator;
        gpu.list = running_list;
        native::ActivateAdaptiveTier(&feature, 0);
        feature.adaptive_session->Request(1, native::SteadyNowNs());
        const auto before_blocked = model_calls;
        Check(!evaluate() && model_calls == before_blocked,
            "Render selects and checks the requested tier before recording: its own pending History prevents every NR call");
        Check(!feature.adaptive_session->failed && !cache->failed,
            "the requested tier's temporary History wait does not latch an execution failure");
        gpu.allocator = pending_allocator;
        gpu.list = pending_list;
        submit();
        gpu.allocator = running_allocator;
        gpu.list = running_list;
        Check(record() && record(), "Render resumes the requested tier after its own pending producer submits");
        feature.adaptive_session->Request(0, native::SteadyNowNs());
        Check(record() && record() && cache->active == 0,
            "Render returns to the original active tier after the target-tier admission check");
      }
      const auto creates = native::nr_features_created;
      feature.adaptive_session->Request(1, native::SteadyNowNs());
      Check(record() && record(), "both rotating outputs accept their prepared lower tier");
      Check(cache->active == 1 && feature.adaptive_session->status.applied[0].width == 5
          && native::nr_features_created == creates,
          "inline lower tier actually applies its requested size without cold Create");
      feature.adaptive_session->Request(0, native::SteadyNowNs());
      Check(record() && record() && cache->active == 0 && native::nr_features_created == creates,
          "inline prepared upper tier restores without cold Create");
      const auto deadline = native::SteadyNowNs() + 5'000'000'000;
      uint64_t serial = 0;
      while (feature.adaptive_session->level == 0 && native::SteadyNowNs() < deadline) {
        const auto now = native::SteadyNowNs();
        {
          std::unique_lock lock(addon::output_framerate::internal::state.mutex);
          auto& estimate = addon::output_framerate::internal::state.stream;
          estimate.previous_ns = now;
          estimate.snapshot = {.available = true, .source = addon::output_framerate::Source::PRESENT,
              .source_generation = 1, .configuration_generation = native::configuration_generation.load(),
              .window_serial = ++serial, .window_begin_ns = now - 500'000'000, .window_end_ns = now,
              .fps = 50., .fg_off = true};
        }
        record();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      std::cout << "ADAPTIVE_INLINE controller hook=" << hook << " level=" << feature.adaptive_session->level
          << " gpu_samples=" << feature.adaptive_session->gpu_count
          << " applied_width=" << feature.adaptive_session->status.applied[0].width
          << " reason=" << addon::adaptive_resolution::ReasonText(feature.adaptive_session->status.reason) << '\n';
      Check(feature.adaptive_session->level == 1 && cache->active == 1
          && feature.adaptive_session->status.applied[0].width == 5 && native::nr_features_created == creates,
          "real GPU samples and mocked low output FPS drive the production inline controller into its ready lower tier");
      {
        std::unique_lock lock(addon::output_framerate::internal::state.mutex);
        addon::output_framerate::internal::state.stream = {};
      }
      auto& failing_tier = cache->tiers[(cache->active + 1) % cache->count];
      failing_tier.output_completions.clear();
      failing_tier.ready = false;
      const auto before_failure = model_calls;
      const auto before_preparation_failure = preparation_failures;
      fail_call = model_calls + 2;  // Active output succeeds, then standby Evaluate fails.
      Check(record() && model_calls == before_failure + 2 && cache->failed && feature.adaptive_session->failed,
          "a true standby API failure preserves the successful active output and pauses adaptive preparation");
      Check(preparation_failures == before_preparation_failure + 1,
          "the standby API failure is observed even though it contributes no successful source frame");
      fail_call = 0;
      const auto before_recreate = native::configuration_generation.load();
      native::RecreateFeatures();
      Check(native::configuration_generation.load() == before_recreate + 1 && feature.adaptive_cache == nullptr,
          "the production clear-failure action retires the inline failed tier set");
      for (uint32_t warmup = 0; warmup < 32; ++warmup) record();
      auto recovered_cache = feature.adaptive_cache;
      native::PollAdaptiveCache(recovered_cache.get());
      Check(recovered_cache != cache && !recovered_cache->failed && !feature.adaptive_session->failed
          && recovered_cache->tiers[0].ready && recovered_cache->tiers[1].ready
          && feature.adaptive_session->status.applied[0].width == 16,
          "Render/Upscaled clear-failure rebuilds GPU-ready tiers and resumes the saved upper working size");
      feature.adaptive_session->Request(1, native::SteadyNowNs());
      Check(record() && record() && recovered_cache->active == 1
          && feature.adaptive_session->status.applied[0].width == 5,
          "the recovered inline source runs its real lower working size before adaptive Off");
      native::nr_adaptive_enabled = false;
      Check(record() && record() && feature.adaptive_cache == nullptr
          && feature.adaptive_session->status.applied[0].width == 16 && native::nr_resolution_scale.load() == 1.f,
          "adaptive Off restores the unchanged manual upper size after recovery");
      native::nr_adaptive_enabled = true;
      for (uint32_t warmup = 0; warmup < 32; ++warmup) record();
      Check(feature.adaptive_cache != nullptr && !feature.adaptive_session->failed
          && feature.adaptive_session->status.applied[0].width == 16,
          "adaptive On resumes the same inline source after Off without changing the saved upper limit");
      native::kSetAdaptiveMinScale.Store(1.f);
      native::RecreateFeatures();
      for (uint32_t warmup = 0; warmup < 12; ++warmup) record();
      const auto single_tier = feature.adaptive_cache;
      Check(single_tier != nullptr && single_tier->count == 1 && single_tier->tiers[0].ready,
          "a source first prepared at a 100-percent minimum owns only its upper physical tier");
      const auto single_model = feature.slots[0].handle;
      const auto single_models = native::nr_features_created;
      const auto single_worksets = native::worksets_created;
      native::kSetAdaptiveMinScale.Store(.5f);
      record();
      Check(feature.adaptive_cache == single_tier && single_tier->count == 2
          && feature.adaptive_session->ladder.count == 2 && !feature.adaptive_session->prepared
          && feature.slots[0].handle == single_model,
          "leaving a one-size range starts one explicit preparation and retains the original upper Feature");
      for (uint32_t warmup = 0; warmup < 24; ++warmup) record();
      Check(feature.adaptive_session->prepared && single_tier->active == 0 && single_tier->tiers[1].ready
          && native::nr_features_created == single_models + 1 && native::worksets_created == single_worksets + 2,
          "the full newly selected set completes both rotating outputs before automatic control resumes");
      feature.adaptive_session->Request(1, native::SteadyNowNs());
      Check(record() && record() && single_tier->active == 1
          && feature.adaptive_session->status.applied[0].width == 8
          && single_tier->tiers[0].state.slots[0].handle == single_model,
          "the newly prepared lower level switches without replacing the original upper allocation");

      native::kSetAdaptiveMinScale.Store(.33f);
      for (uint32_t warmup = 0; warmup < 24; ++warmup) record();
      auto binding_cache = feature.adaptive_cache;
      const auto binding_models = native::nr_features_created;
      const auto binding_worksets = native::worksets_created;
      auto* rebound_resource = (hook == native::kHookUpscaled ? outputs[0].Get() : colors[0].Get());
      uint64_t original_binding_identity = 0;
      UINT identity_size = sizeof(original_binding_identity);
      Check(SUCCEEDED(rebound_resource->GetPrivateData(native::kAdaptiveOutputIdentityGuid,
          &identity_size, &original_binding_identity)) && original_binding_identity != 0,
          "the current output carries a non-owning object-attached identity");
      const auto changed_binding_identity = native::next_adaptive_output_identity++;
      Check(SUCCEEDED(rebound_resource->SetPrivateData(native::kAdaptiveOutputIdentityGuid,
          sizeof(changed_binding_identity), &changed_binding_identity)), "MOCK same-address replacement identity");
      ++native::present_generation;
      native::AdaptiveOutputCount(&feature, handle, rebound_resource, hook == native::kHookRender,
          16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, rebound_resource);
      Check(!feature.adaptive_output_blocked
          && binding_cache->previous_output_bindings.contains(original_binding_identity),
          "a new cookie at the same address is admitted only after healthy exact completion");
      for (const auto& [key, resources] : native::worksets) {
        if (key.handle != handle || key.resource != rebound_resource) continue;
        Check(std::none_of(std::begin(resources.bound_sources), std::end(resources.bound_sources),
            [&](const auto* source) { return source == rebound_resource; }),
            "same-address replacement invalidates source-view pointer shortcuts before the next descriptor bind");
      }
      for (uint32_t step = 0; step < 4; ++step) record();
      Check(feature.adaptive_session->prepared && native::nr_features_created == binding_models
          && native::worksets_created == binding_worksets,
          "MOCK identity replacement reacquires real WARP readiness without allocating a new tier");
      const auto previous_bindings = binding_cache->previous_output_bindings.size();
      auto third = texture(DXGI_FORMAT_R8G8B8A8_UNORM, hook == native::kHookUpscaled);
      gpu.Submit();
      const auto output_references_before = third->AddRef();
      third->Release();
      auto& changed_output = (hook == native::kHookUpscaled ? outputs[0] : colors[0]);
      auto original_output = changed_output;
      changed_output = third;
      for (uint32_t step = 0; step < 4; ++step) record();
      const auto output_references_after = third->AddRef();
      third->Release();
      if (hook == native::kHookUpscaled) {
        Check(output_references_after == output_references_before + 1,
            "output identity adds no COM ownership beyond the fixture's own current-output reference");
      }
      Check(feature.adaptive_cache == binding_cache && binding_cache->previous_output_bindings.size() == previous_bindings + 1
          && native::nr_features_created == binding_models && native::worksets_created == binding_worksets,
          "a proven same-geometry binding replacement refreshes receipts and references without replacing Feature or scratch");
      changed_output = original_output;
      for (uint32_t step = 0; step < 4; ++step) record();
      Check(binding_cache->budget_blocked && feature.adaptive_session->status.reason == addon::adaptive_resolution::Reason::BUDGET
          && native::nr_features_created == binding_models && native::worksets_created == binding_worksets,
          "a previously rebound output returning in A/B/C/A locks budget instead of starting a rolling preparation cycle");
      const auto previous_target = native::nr_adaptive_target_fps.load();
      native::nr_adaptive_target_fps.store(previous_target + 1.f);
      for (uint32_t step = 0; step < 8; ++step) record();
      Check(binding_cache->budget_blocked && native::nr_features_created == binding_models
          && native::worksets_created == binding_worksets,
          "target FPS and repeated source observations cannot restart a rejected output-rotation contract");
      native::nr_adaptive_target_fps.store(previous_target);

      std::cout << "ADAPTIVE_INLINE accounting hook=" << hook << " frames=" << frame
          << " source_successes=" << native::SourceSuccessfulEvaluations()
          << " model_calls=" << model_calls << " preparation_calls=" << preparation_calls
          << " preparation_failures=" << preparation_failures << '\n';
      native::ForgetSourceHandleLocked(handle);
      submit();
      Check(!addon::submission::ResourceReleasable(parked_model),
          "source retirement cannot release a model referenced by the parked active recording");
      Check(SUCCEEDED(parked_allocator->Reset()) && SUCCEEDED(parked_list->Reset(parked_allocator.Get(), nullptr)),
          "fixture closes its parked recording only after preparation and source-retirement assertions");
      native::DrainRetiredResources(true);
      Check(addon::submission::ResourceReleasable(parked_model)
          && std::count(model_released_handles.begin(), model_released_handles.end(), parked_model) == parked_releases + 1,
          "closing the fixture's completed parked recording permits exactly one normal model release");
    }
    model_parameter_observer = {};
    {
      native::hook_point = native::kHookUpscaled;
      native::nr_adaptive_enabled = true;
      native::nr_adaptive_selected_levels = addon::adaptive_resolution::MAX_PREPARED_LEVELS;
      native::nr_adaptive_min_scale = .33f;
      native::nr_resolution_scale = 1.f;
      native::max_worksets = 4;
      std::array wide_colors{texture(DXGI_FORMAT_R8G8B8A8_UNORM, false, 64),
                            texture(DXGI_FORMAT_R8G8B8A8_UNORM, false, 64)};
      std::array wide_outputs{texture(DXGI_FORMAT_R8G8B8A8_UNORM, true, 64),
                             texture(DXGI_FORMAT_R8G8B8A8_UNORM, true, 64)};
      auto wide_motion = texture(DXGI_FORMAT_R16G16_FLOAT, false, 64);
      auto wide_depth = texture(DXGI_FORMAT_R32_FLOAT, false, 64);
      gpu.Submit();
      std::array output_readbacks{gpu.CreateTextureTransfer(wide_outputs[0].Get()),
                                 gpu.CreateTextureTransfer(wide_outputs[1].Get())};
      addon::bridge::OwnedParameters wide_parameters;
      wide_parameters.Set(NVSDK_NGX_Parameter_Depth, wide_depth.Get());
      wide_parameters.Set(NVSDK_NGX_Parameter_MotionVectors, wide_motion.Get());
      for (const auto* key : {NVSDK_NGX_Parameter_Width, NVSDK_NGX_Parameter_Height,
               NVSDK_NGX_Parameter_OutWidth, NVSDK_NGX_Parameter_OutHeight,
               NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
               NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height}) {
        wide_parameters.Set(key, 64u);
      }
      wide_parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
          static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
      wide_parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
      wide_parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
      wide_parameters.Set(NVSDK_NGX_Parameter_Reset, 0u);
      const auto* wide_handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADA800});
      native::RegisterCreatedFeatureLocked(wide_handle, native::kFeatureDlss, &wide_parameters);
      const auto before_wide_models = native::nr_features_created;
      uint32_t wide_frame = 0;
      const auto wide_record = [&](bool verify_writeback = false) {
        ++native::present_generation;
        const uint32_t output = wide_frame++ % 2;
        if (verify_writeback) {
          gpu.Transition(wide_outputs[output].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_COMMON);
          gpu.Write(wide_outputs[output].Get(), 0x7f);
          gpu.Transition(wide_outputs[output].Get(), D3D12_RESOURCE_STATE_COMMON,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        wide_parameters.Set(NVSDK_NGX_Parameter_Color, wide_colors[output].Get());
        wide_parameters.Set(NVSDK_NGX_Parameter_Output, wide_outputs[output].Get());
        bool accepted = false;
        {
          const addon::EvaluateChainScope chain;
          const addon::SourceStageScope source(wide_handle, true);
          if (native::hook_point.load() == native::kHookRender) {
            native::PreSrSwap swap;
            accepted = native::ProcessInlinePreSR(gpu.list.Get(), wide_handle, &wide_parameters, swap);
            native::RestorePreSrColor(&wide_parameters, swap);
          } else {
            accepted = native::ProcessInline(gpu.list.Get(), wide_handle, &wide_parameters);
          }
        }
        if (verify_writeback) {
          gpu.Transition(wide_outputs[output].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_COPY_SOURCE);
          gpu.RecordTextureReadback(wide_outputs[output].Get(), output_readbacks[output]);
          gpu.Transition(wide_outputs[output].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        submit();
        Check(SUCCEEDED(gpu.device->GetDeviceRemovedReason())
            && gpu.fence->GetCompletedValue() != UINT64_MAX && gpu.fence->GetCompletedValue() >= gpu.serial,
            "every selected-level step finishes on a healthy WARP Fence");
        if (verify_writeback) {
          const auto pixels = gpu.ReadTexture(output_readbacks[output]);
          bool changed = false;
          for (size_t pixel = 0; pixel < pixels.size(); pixel += 4) {
            changed |= pixels[pixel] != 0x7f || pixels[pixel + 1] != 0x7f || pixels[pixel + 2] != 0x7f;
          }
          Check(accepted && changed, "the selected level writes actual GPU model output back to the requested game surface");
        }
        return accepted;
      };
      bool ordinary_pressure = false;
      for (uint32_t preparation = 0; preparation < 128; ++preparation) {
        wide_record();
        auto& wide = native::features.at(wide_handle);
        if (!wide.adaptive_session->prepared) {
          Check(wide.adaptive_cache->active == 0 && !wide.adaptive_session->trial,
              "the full fifteen-size collection closes automatic decisions until every output is ready");
        }
        if (!ordinary_pressure && wide.adaptive_cache->resident) {
          const auto owned = native::AdaptiveResidentWorksetCount(wide.adaptive_session);
          for (uint32_t stream = 0; stream < 5; ++stream) {
            const auto* ordinary = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADA810} + stream);
            Check(native::EnsureWorkset(gpu.device.Get(), ordinary, wide_outputs[0].Get(), 16, 16, 16, 16,
                DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0) != nullptr,
                "ordinary traffic can use and recycle its original bounded pool during preparation");
          }
          Check(native::AdaptiveResidentWorksetCount(wide.adaptive_session) == owned
              && native::worksets.size() - owned == native::max_worksets.load()
              && native::EffectiveAdaptiveWorksetLimit() == 34,
              "unbuilt selected slots stay reserved while ordinary traffic remains at its original four-workset quota");
          ordinary_pressure = true;
        }
        if (wide.adaptive_session->prepared) break;
      }
      auto& wide = native::features.at(wide_handle);
      const auto collection = wide.adaptive_cache;
      Check(wide.adaptive_session->prepared && collection->count == 15 && collection->output_count == 2
          && native::AdaptiveResidentWorksetCount(wide.adaptive_session) == 30
          && native::adaptive_resident_worksets == 30 && native::max_worksets.load() == 4
          && native::nr_features_created == before_wide_models + 15,
          "all fifteen complete-size tuples own real Features and both rotating-output worksets within their explicit reservation");
      const auto steady_models = native::nr_features_created;
      const auto steady_worksets = native::worksets_created;
      for (size_t level = 0; level < collection->count; ++level) {
        wide.adaptive_session->Request(level, native::SteadyNowNs());
        wide_record(true);
        wide_record(true);
        Check(collection->active == level && wide.input_width == collection->ladder.sizes[level][0].width
            && wide.input_height == collection->ladder.sizes[level][0].height
            && wide.adaptive_session->status.applied == collection->ladder.sizes[level],
            "each selected level evaluates at its actual complete working dimensions on both outputs");
      }
      for (const size_t level : {14u, 0u, 7u, 14u, 0u}) {
        wide.adaptive_session->Request(level, native::SteadyNowNs());
        wide_record(true);
        wide_record(true);
      }
      Check(native::nr_features_created == steady_models && native::worksets_created == steady_worksets,
          "all-level and immediate fifteen-level round trips perform no new Feature or workset allocations");
      native::nr_adaptive_enabled = false;
      wide_record();
      wide_record();
      Check(wide.adaptive_cache == nullptr && native::adaptive_resident_session.expired()
          && native::adaptive_resident_worksets == 0
          && std::count_if(native::worksets.begin(), native::worksets.end(),
              [&](const auto& workset) { return workset.first.handle == wide_handle; }) == 2,
          "Off releases the explicit reservation and retires every standby while retaining the legal manual upper");
      for (uint32_t stream = 0; stream < 5; ++stream) {
        const auto* ordinary = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADA810} + stream);
        native::RetireWorkset({ordinary, wide_outputs[0].Get()});
      }
      submit();
      native::hook_point = native::kHookRender;
      native::RecreateFeatures();
      native::nr_adaptive_selected_levels = 3;
      native::nr_adaptive_enabled = true;
      for (uint32_t preparation = 0; preparation < 64; ++preparation) {
        wide_record();
        if (wide.adaptive_session->prepared) break;
      }
      Check(wide.adaptive_session->prepared && wide.adaptive_cache->pre_sr
          && native::AdaptiveResidentWorksetCount(wide.adaptive_session) == 6
          && std::count_if(native::worksets.begin(), native::worksets.end(), [&](const auto& workset) {
               return workset.first.handle == wide_handle && !workset.first.pre_sr;
             }) == 2,
          "ordinary PostSR scratch survives Off and a Render switch without consuming the six selected Render slots");
      for (uint32_t stream = 0; stream < 4; ++stream) {
        const auto* ordinary = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADA810} + stream);
        Check(native::EnsureWorkset(gpu.device.Get(), ordinary, wide_outputs[0].Get(), 16, 16, 16, 16,
            DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0) != nullptr, "ordinary Stage leftovers remain eligible for normal LRU");
      }
      Check(native::AdaptiveResidentWorksetCount(wide.adaptive_session) == 6
          && std::none_of(native::worksets.begin(), native::worksets.end(), [&](const auto& workset) {
               return workset.first.handle == wide_handle && !workset.first.pre_sr;
             }), "the pool can recycle old PostSR scratch while every selected Render workset stays resident");
      native::ForgetSourceHandleLocked(wide_handle);
      for (uint32_t stream = 0; stream < 4; ++stream) {
        const auto* ordinary = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADA810} + stream);
        native::RetireWorkset({ordinary, wide_outputs[0].Get()});
      }
      submit();
      native::nr_adaptive_selected_levels = 0;
      std::cout << "ADAPTIVE_SELECTED evidence=WARP+MOCK dimensions=64x64 levels=15 outputs=2 Features=15 selected_worksets=30 ordinary_limit=4 steady_allocations=0 native_NGX=not_run\n";
    }

    native::successful_sr_device = nullptr;
    {
      ID3D12RootSignature* root = nullptr;
      const auto before = captured_logs.size();
      Check(!native::CreateRootSignature(nullptr, E_INVALIDARG, nullptr, nullptr, &root, L"mock",
          "MOCK serialization failed", "mock creation failed"), "MOCK serialization failure remains a failure");
      Check(captured_logs.size() == before + 1
          && captured_logs.back().second.find("api=D3D12SerializeRootSignature hr=0x80070057") != std::string::npos,
          "MOCK HRESULT input is logged unchanged with its actual API label");
      std::cout << "DIAGNOSTIC_API evidence=MOCK api=D3D12SerializeRootSignature hr=0x80070057 illegal_D3D12_call=none\n";
      for (auto& sample : addon::gpu_timers::stage_stats) {
        sample = {};
      }
      for (auto& sample : addon::gpu_timers::execution_stats) {
        sample = {};
      }
      for (auto& sample : addon::gpu_timers::completed_stats) {
        sample = {};
      }
      native::card_observation.completions = {};
      addon::NrVerdict verdict{.state = addon::NrState::kOff, .api = "d3d12", .path = "inline"};
      auto tick_ns = native::SteadyNowNs();
      native::last_emitted_card = 0xff;
      native::card_line_tokens = 1;
      native::card_line_refill_ns = 0;
      const auto count_lines = [](const char* text) {
        return std::count_if(captured_logs.begin(), captured_logs.end(), [&](const auto& entry) {
          return entry.second.find(text) != std::string::npos;
        });
      };
      const auto cards_before = count_lines("NR-CARD v1 ");
      const auto evidence_before = count_lines("NR-CARD evidence ");
      native::MaybeEmitStatusCard(verdict, tick_ns);
      Check(count_lines("NR-CARD v1 ") == cards_before + 1 && count_lines("NR-CARD evidence ") == evidence_before + 1
          && captured_logs.back().second.find("gpu_candidate_completed_age_ms=unavailable") != std::string::npos,
          "production card change emits one companion evidence line with explicit missing GPU evidence");
      verdict.eligible = 10;
      tick_ns += 100'000'000LL;
      native::MaybeEmitStatusCard(verdict, tick_ns);
      Check(count_lines("NR-CARD evidence ") == evidence_before + 1,
          "changing counts and ages alone cannot bypass production card deduplication");
      native::card_line_tokens = 0;
      native::card_line_refill_ns = tick_ns;
      verdict.state = addon::NrState::kEngaged;
      native::MaybeEmitStatusCard(verdict, tick_ns);
      Check(count_lines("NR-CARD v1 ") == cards_before + 1 && count_lines("NR-CARD evidence ") == evidence_before + 1,
          "production token exhaustion suppresses both the card and evidence lines");
    }
    if (info != nullptr) {
      for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(index, message, &bytes)), "inline adaptive debug message");
        Check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
      }
    }
    std::cout << "PASS adaptive Render/Upscaled two-output preparation and ready-tier switching evidence=WARP+MOCK\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "adaptive-inline: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace adaptive_inline_cases
