/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace adaptive_resource_strategy_cases {
namespace addon = renodx::addons::dlss5;
namespace adaptive = addon::adaptive_resolution;
namespace presets = addon::presets;

int Run() {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    present_fixture_ini = std::filesystem::absolute("adaptive-resource-strategy.ini");
    Gpu gpu(false, 16, 16);
    model_gpu = &gpu;
    native::enabled = true;
    native::hooks_enabled = true;
    native::hook_point = native::kHookUpscaled;
    native::stack_passes = 1;
    native::preset = 2;
    native::look_mode = false;
    native::control_mask_enabled = false;
    native::edit_reuse_enabled = false;
    native::nr_follow_input_res = false;
    native::nr_resolution_scale = 1.f;
    native::cost_meter_enabled = false;
    native::gpu_timers_enabled = false;
    native::max_worksets = 4;
    native::d3d12_swapchain_seen = true;
    native::adaptive_configuration_settings = native::comparison_ui::CaptureAdaptiveSettings;
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
    native::kSetAdaptiveResourceStrategy.Commit(0.f);
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()) && native::InstallCommandListStateHooks(gpu.list.Get()),
        "strategy fixture observes real submit and list lifetime gates");
    const auto submit = [&]() {
      Check(SUCCEEDED(gpu.list->Close()), "close strategy recording");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      gpu.queue->ExecuteCommandLists(1, lists);
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "signal strategy recording");
      presets::internal::Handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
      Check(event.value != nullptr && event.value != INVALID_HANDLE_VALUE
                && SUCCEEDED(gpu.fence->SetEventOnCompletion(gpu.serial, event.value))
                && WaitForSingleObject(event.value, 10000) == WAIT_OBJECT_0,
            "strategy recording completes on WARP");
      native::ResolveAdaptiveBorrowedDeliveries();
      Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
          "strategy recording becomes non-replayable");
      native::DrainRetiredResources(true);
      gpu.uploads.clear();
      gpu.retained_resources.clear();
    };

    const auto texture = [&](DXGI_FORMAT format, bool output) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = 16, .Height = 16, .DepthOrArraySize = 1, .MipLevels = 1, .Format = format, .SampleDesc = {1, 0},
          .Flags = (output ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE)};
      ComPtr<ID3D12Resource> resource;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
          &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))), "strategy texture");
      gpu.Write(resource.Get(), format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 0x38 : 0);
      gpu.Transition(resource.Get(), D3D12_RESOURCE_STATE_COMMON,
          output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : NATIVE_INPUT_STATE);
      return resource;
    };
    auto color = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, false);
    auto output = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    auto depth = texture(DXGI_FORMAT_R32_FLOAT, false);
    auto motion = texture(DXGI_FORMAT_R16G16_FLOAT, false);
    submit();
    addon::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_Parameter_Color, color.Get());
    parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
    parameters.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
    parameters.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
    parameters.Set(NVSDK_NGX_Parameter_Width, 16u);
    parameters.Set(NVSDK_NGX_Parameter_Height, 16u);
    parameters.Set(NVSDK_NGX_Parameter_OutWidth, 16u);
    parameters.Set(NVSDK_NGX_Parameter_OutHeight, 16u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 16u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 16u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<uint32_t>(
        NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_Reset, 0u);
    uint32_t source = 0;
    for (const auto [strategy, limited, levels, compatible] : {
             std::tuple{0u, false, 2u, true}, std::tuple{0u, true, 2u, true},
             std::tuple{1u, true, 2u, true}, std::tuple{2u, true, 2u, true}, std::tuple{3u, true, 2u, true},
             std::tuple{2u, true, 4u, true}, std::tuple{3u, true, 0u, true}, std::tuple{1u, true, 2u, false}}) {
      const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADAF00} + ++source);
      native::direct_runtime_requirements.file_version = (compatible ? "310.9.16.0" : "310.9.16.1");
      const presets::Document request{.scope = presets::Scope::QUALITY,
          .settings = {{presets::ADAPTIVE_RESOURCE_STRATEGY_KEY, strategy}, {"NRAdaptiveEnabled", true},
              {"NRAdaptiveLimitedResidency", limited}, {"NRAdaptiveLevelCount", levels}, {"NRAdaptiveMinScale", .5f}}};
      std::string error;
      const bool committed = native::preset_ui::Apply(request, false, false, &error);
      Check(committed, error.c_str());
      Check(native::ReadPresentRestoreRaw(presets::ADAPTIVE_RESOURCE_STRATEGY_KEY) == std::to_string(strategy),
          "production preset committed the selected resource strategy to the fixture INI");
      native::RegisterCreatedFeatureLocked(handle, native::kFeatureDlss, &parameters);
      const auto creates_before = native::nr_features_created;
      const auto record = [&]() {
        ++native::present_generation;
        native::NrRecording recording;
        {
          const addon::EvaluateChainScope chain;
          const addon::SourceStageScope source_stage(handle, true);
          const native::PipelinePreparationScope opportunity;
          native::ProcessInline(gpu.list.Get(), handle, &parameters, nullptr, nullptr, &recording, false,
#if defined(NR_NEXUS_DEV_CONTROL)
              false,
#endif
              nullptr, true);
        }
        submit();
        if (auto& feature = native::features.at(handle); feature.adaptive_cache != nullptr) {
          native::PollAdaptiveCache(feature.adaptive_cache.get());
        }
        return recording.group_complete && recording.output_ready;
      };
      bool delivered = false;
      for (int frame = 0; frame < 36; ++frame) delivered |= record();
      auto& feature = native::features.at(handle);
      Check(delivered && feature.adaptive_session != nullptr && feature.adaptive_cache != nullptr,
          "a legal resource policy still processes and delivers the upper tier");
      auto& session = *feature.adaptive_session;
      auto& cache = *feature.adaptive_cache;
      const uint32_t effective = (levels == 2 && compatible ? strategy : 0u);
      Check(session.status.requested_resource_strategy == strategy && session.settings.resource_strategy == effective
              && session.settings.limited_residency == (effective == 0 && limited)
              && session.ladder.count == (levels == 0 ? 4 : levels),
          "the executing group honors or explains the strategy without dropping legal levels");
      const auto fallback = (effective == strategy ? adaptive::ResourceFallback::NONE
          : compatible ? adaptive::ResourceFallback::LEVELS : adaptive::ResourceFallback::BINDING);
      Check(session.status.resource_fallback == fallback, "group status publishes the actual fallback reason");
      const bool demand = effective == 3 || (effective == 0 && limited);
      Check(native::nr_features_created - creates_before == (demand ? 1 : 2),
          "startup Create count distinguishes eager and cold-target preparation");
      if (demand) {
        Check(!cache.tiers[1].ready && cache.tiers[1].state.slots[0].handle == nullptr,
            "cold strategy does not prepare the next logical tier before it is requested");
      }
      session.Request(1, native::SteadyNowNs());
      for (int frame = 0; frame < 36 && session.status.applied != session.ladder.sizes[1]; ++frame) record();
      const auto target = session.ladder.sizes[1][0];
      std::cout << "RESOURCE_STRATEGY_TARGET strategy=" << strategy << " ready=" << cache.tiers[1].ready
                << " requested_tier=" << session.level << " applied_tier=" << session.active_level
                << " active=" << cache.active << " applied_width=" << session.status.applied[0].width
                << " prepared=" << session.prepared << " state=" << static_cast<int>(session.status.reason) << '\n';
      Check(cache.active == 1 && session.status.applied[0] == target && feature.input_width == target.width,
          "prepared target takes effect after real output delivery and retains its logical work grid");
      const auto& model = feature.slots[0];
      const native::WorksetKey key{handle, output.Get(), false, 1};
      Check(model.handle != nullptr && model.nr_preset == 2 && model.create_input_width == (effective >= 2 ? 16 : target.width)
              && native::worksets.at(key).proxy->GetDesc().Width == target.width
              && !model_resets.empty() && model_resets.back() == 1,
          "actual Feature Create capacity and physical work grid remain independently observable");
      std::cout << "RESOURCE_STRATEGY requested=" << strategy << " effective=" << effective << " levels=" << session.ladder.count
                << " startup=" << (demand ? "cold" : "eager") << " create_width=" << model.create_input_width
                << " evaluate_width=" << target.width << " delivered=1\n";
      if (effective == 2) {
        const auto readback = gpu.CreateTextureTransfer(output.Get());
        gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.RecordTextureReadback(output.Get(), readback);
        gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit();
        const auto rgba = gpu.ReadTexture(readback);
        bool changed_color = false;
        Check(rgba.size() == 16 * 16 * 8, "the target writes back at the original FP16 output extent");
        for (size_t offset = 0; offset < rgba.size(); offset += 8) {
          const auto rgb = std::span{rgba}.subspan(offset, 6);
          changed_color |= std::ranges::any_of(rgb, [](uint8_t value) { return value != 0x38; });
          Check(rgba[offset + 6] == 0x38 && rgba[offset + 7] == 0x38, "capacity output preserves the source alpha bits");
        }
        Check(changed_color, "real GPU readback sees the model edit on the original output");
        std::cout << "RESOURCE_STRATEGY_OUTPUT strategy=2 readback=fp16_16x16 alpha_preserved=1 color_written=1\n";
      }
      if (effective == 1 || effective == 2) {
        const auto* standby_model = model.handle;
        const auto create_width = model.create_input_width;
        session.Request(0, native::SteadyNowNs());
        for (int frame = 0; frame < 36 && cache.active != 0; ++frame) record();
        Check(cache.active == 0, "return to the upper tier before retargeting the spare model");
        const auto old_completion = cache.tiers[1].completion;
        const auto old_creates = native::nr_features_created;
        ComPtr<ID3D12CommandAllocator> guarded_allocator;
        ComPtr<ID3D12GraphicsCommandList> guarded_list;
        Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&guarded_allocator)))
                  && SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, guarded_allocator.Get(),
                      nullptr, IID_PPV_ARGS(&guarded_list))),
              "fixture owns an independent pending recording");
        addon::submission::TrackUse(guarded_list.Get(), standby_model);
        Check(!addon::submission::ResourceReleasable(standby_model), "pending list holds the spare model");
        native::kSetAdaptiveMinScale.Commit(.33f);
        record();
        Check(session.RequestMinimum(native::SteadyNowNs()), "cold endpoint fits the existing spare creation dimensions");
        for (int frame = 0; frame < 4; ++frame) record();
        Check(cache.active == 0 && session.status.applied == session.ladder.sizes[0]
                  && cache.tiers[1].completion == old_completion && native::nr_features_created == old_creates,
              "an old completion cannot endorse retargeting a model referenced by a pending recording");
        Check(SUCCEEDED(guarded_list->Close()) && SUCCEEDED(guarded_list->Reset(guarded_allocator.Get(), nullptr)),
              "close and discard the pending recording before rebinding the spare");
        for (int frame = 0; frame < 36 && cache.active != 1; ++frame) record();
        Check(cache.active == 1 && session.status.applied[0].width == 5 && feature.input_width == 5
                  && feature.slots[0].handle == standby_model && feature.slots[0].create_input_width == create_width
                  && cache.tiers[1].completion != old_completion && native::nr_features_created == old_creates
                  && model_resets.back() == 1,
              "the completed compatible spare keeps its Feature but binds a new 5x5 work grid with Reset and delivery");
        std::cout << "RESOURCE_STRATEGY_REBIND strategy=" << strategy << " in_flight=blocked stale_receipt=rejected"
                  << " create_delta=0 retained=1 delivered_width=5\n";
      }
      if (effective == 3) {
        session.Request(0, native::SteadyNowNs());
        for (int frame = 0; frame < 36 && cache.active != 0; ++frame) record();
        const auto* upper_model = feature.slots[0].handle;
        native::kSetAdaptiveMinScale.Commit(.33f);
        record();
        model_parameter_observer = [](const auto*, const auto*) {
          if (native::preparing_adaptive_frame != nullptr) fail_call = model_calls + 1;
        };
        Check(session.RequestMinimum(native::SteadyNowNs()), "request the new cold endpoint before injecting one SDK failure");
        for (int frame = 0; frame < 36 && !cache.failed; ++frame) record();
        model_parameter_observer = {};
        fail_call = 0;
        Check(cache.failed && cache.active == 0 && session.status.applied[0].width == 16
                  && feature.slots[0].handle == upper_model && !cache.tiers[1].ready,
              "a failed cold Evaluate preserves the last confirmed tier and retires the unsuccessful spare");
        std::cout << "RESOURCE_STRATEGY_FAILURE strategy=3 cold_evaluate=rejected confirmed_width=16 retained_upper=1\n";
      }
      native::ForgetSourceHandleLocked(handle);
      submit();
    }
    model_gpu = nullptr;
    present_fixture_ini.clear();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL adaptive-resource-strategy: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace adaptive_resource_strategy_cases
