/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <DirectXPackedVector.h>

#include <cmath>
#include <fstream>
#include <map>
#include <span>
#include <vector>

#include <utils/hash.hpp>

namespace adaptive_resource_strategy_cases {
namespace addon = renodx::addons::dlss5;
namespace adaptive = addon::adaptive_resolution;
namespace presets = addon::presets;

#if defined(NR_NEXUS_TEST_NATIVE_NGX)
// fg_native_tests routes adaptive-resource-strategy-native to this real Runtime
// entry. adaptive-resource-strategy uses WARP and SDK substitutes.
struct NativeCalls {
  inline static NativeCalls* active = nullptr;
  native::DirectCreateFn create = nullptr;
  native::DirectEvaluateFn sr_evaluate = nullptr;
  Gpu* gpu = nullptr;
  d3d12_test::TextureTransfer* sr_readback = nullptr;
  presets::Json creates = presets::Json::array();
  presets::Json evaluations = presets::Json::array();
  bool measuring = false;

  static NVSDK_NGX_Result NVSDK_CONV Create(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature,
      const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
    const auto result = active->create(list, feature, parameters, handle);
    const auto* frame = native::active_adaptive_frame;
    Check(feature == native::kFeatureDlssNr && frame != nullptr && frame->session != nullptr,
        "native fallback Create belongs to the executing resource policy");
    const auto slot = frame->slot;
    const auto size = frame->session->ladder.sizes[slot][0];
    const std::array<uint32_t, 2> input{native::GetUInt(parameters, NVSDK_NGX_Parameter_Width),
        native::GetUInt(parameters, NVSDK_NGX_Parameter_Height)};
    const std::array<uint32_t, 2> output{native::GetUInt(parameters, NVSDK_NGX_Parameter_OutWidth),
        native::GetUInt(parameters, NVSDK_NGX_Parameter_OutHeight)};
    Check(NVSDK_NGX_SUCCEED(result) && handle != nullptr && *handle != nullptr
              && frame->session->settings.resource_strategy == adaptive::OFF && !frame->upper_capacity
              && input == output && input[0] == size.width && input[1] == size.height,
        "fallback creates an independent 1:1 Feature at the full selected tier's actual dimensions");
    active->creates.push_back({{"slot", slot}, {"input", input}, {"output", output},
        {"model", native::GetUInt(parameters, "DLSSNR.Hint.Render.Preset")},
        {"flags", native::GetInt(parameters, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags)},
        {"result", static_cast<uint32_t>(result)}});
    return result;
  }

  static NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
      const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback progress) {
    // Reuse the existing fixed-proxy producer and real Runtime forwarding.
    const auto result = control_mask_gpu_cases::ObserveNativeRuntimeEvaluate(list, handle, parameters, progress);
    if (active->measuring) {
      const auto* frame = native::active_adaptive_frame;
      Check(frame != nullptr && !frame->preparing && frame->session != nullptr,
          "a measured fallback Evaluate belongs to the selected active tier");
      const auto size = frame->sizes[0];
      auto* color = native::GetD3D12Resource(parameters, "DLSSNR.Color");
      auto* output = native::GetD3D12Resource(parameters, "DLSSNR.Output");
      const std::array<uint32_t, 2> actual{native::GetUInt(parameters, NVSDK_NGX_Parameter_Width),
          native::GetUInt(parameters, NVSDK_NGX_Parameter_Height)};
      Check(NVSDK_NGX_SUCCEED(result) && color != nullptr && output != nullptr
                && actual[0] == size.width && actual[1] == size.height
                && color->GetDesc().Width == size.width && color->GetDesc().Height == size.height
                && output->GetDesc().Width == size.width && output->GetDesc().Height == size.height,
          "real fallback Evaluate uses the selected physical color and neural output grid");
      presets::Json parameters_seen = presets::Json::object();
      for (const auto* key : {"DLSSNR.InputWidth", "DLSSNR.InputHeight", "DLSSNR.OutputWidth", "DLSSNR.OutputHeight",
               "DLSSNR.ColorSubrectWidth", "DLSSNR.ColorSubrectHeight", "DLSSNR.DepthSubrectWidth",
               "DLSSNR.DepthSubrectHeight", "DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight"}) {
        parameters_seen[key] = native::GetUInt(parameters, key);
      }
      active->evaluations.push_back({{"slot", frame->slot}, {"grid", actual},
          {"reset", native::GetInt(parameters, "DLSSNR.Reset", -1)},
          {"parameters", std::move(parameters_seen)}, {"result", static_cast<uint32_t>(result)}});
    }
    return result;
  }

  static NVSDK_NGX_Result NVSDK_CONV EvaluateSr(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
      const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback progress) {
    const auto result = active->sr_evaluate(list, handle, parameters, progress);
    if (NVSDK_NGX_SUCCEED(result) && active->sr_readback != nullptr) {
      auto* output = native::GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Output);
      active->gpu->Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      active->gpu->RecordTextureReadback(output, *active->sr_readback);
      active->gpu->Transition(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    return result;
  }
};

int RunNative(const char* project_id_path) {
  using Runtime = nr_edit_reuse_present_native_cases::Runtime;
  namespace fixed = control_mask_gpu_cases;
  presets::Json report{{"schema", "nr-nexus-resource-strategy-native/1"}, {"status", "Running"},
      {"evidence", "NATIVE_GPU+NATIVE_NGX"}, {"version", NR_NEXUS_VERSION},
      {"scope", "Upscaled FP16, one output, one B pass, four independent tiers"},
      {"requests", "MOCK direct Session requests after complete preparation"},
      {"history", "Each tier measured after Reset with four real NR Evaluates"},
      {"cases", presets::Json::array()}, {"comparisons", presets::Json::array()}};
  const auto checkpoint = [&] {
    std::ofstream file("adaptive-resource-strategy-native.json", std::ios::binary | std::ios::trunc);
    file << report.dump(2) << '\n';
    file.close();
    Check(!file.fail(), "native resource strategy evidence saved");
  };
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    present_fixture_ini = std::filesystem::absolute("adaptive-resource-strategy-native.ini");
    Gpu gpu(true, 960, 540);
    Runtime runtime(gpu, project_id_path);
    NativeCalls calls{.create = native::core_create_feature,
        .sr_evaluate = reinterpret_cast<native::DirectEvaluateFn>(native::ngx_slot_real[Runtime::SR_SLOT].evaluate), .gpu = &gpu};
    NativeCalls::active = &calls;
    const auto previous_evaluate = fixed::native_runtime_evaluate;
    fixed::native_runtime_evaluate = native::core_evaluate_feature;
    fixed::native_motion_gpu = &gpu;
    fixed::native_resize_fixed_input = true;
    native::core_create_feature = NativeCalls::Create;
    native::core_evaluate_feature = NativeCalls::Evaluate;
    native::ngx_slot_real[Runtime::SR_SLOT].evaluate = reinterpret_cast<void*>(NativeCalls::EvaluateSr);
    struct RestoreCallbacks {
      NativeCalls* calls;
      native::DirectEvaluateFn original_evaluate;
      ~RestoreCallbacks() {
        native::core_create_feature = calls->create;
        native::core_evaluate_feature = fixed::native_runtime_evaluate;
        native::ngx_slot_real[Runtime::SR_SLOT].evaluate = reinterpret_cast<void*>(calls->sr_evaluate);
        fixed::native_runtime_evaluate = original_evaluate;
        fixed::native_resize_fixed_input = false;
        fixed::native_motion_gpu = nullptr;
        NativeCalls::active = nullptr;
        present_fixture_ini.clear();
      }
    } restore{&calls, previous_evaluate};
    native::shutdown_quiesce_enabled = false;
    native::enabled = true;
    native::hooks_enabled = true;
    native::hook_point = native::kHookUpscaled;
    native::stack_passes = 1;
    native::preset = 2;
    native::look_mode = false;
    native::detail_stability = 1;
    native::use_auto_mask = false;
    native::control_mask_enabled = false;
    native::edit_reuse_enabled = false;
    native::nr_follow_input_res = false;
    native::nr_resolution_scale = 1.f;
    native::feed_mode = 1;
    native::norm_governor = 0;
    native::transfer_strength = 1.f;
    native::color_strength = 1.f;
    native::intensity = 1.f;
    native::cost_meter_enabled = false;
    native::gpu_timers_enabled = false;
    native::max_worksets = 4;
    native::d3d12_swapchain_seen = true;
    native::adaptive_configuration_settings = native::comparison_ui::CaptureAdaptiveSettings;
    Check(native::direct_runtime_requirements.file_version == "310.9.16.0"
              && native::InstallQueueCompletionHooks(gpu.queue.Get())
              && native::InstallCommandListStateHooks(gpu.list.Get()), "native fallback uses pinned Runtime and tracked commands");
    const auto submit = [&] {
      Check(SUCCEEDED(gpu.list->Close()), "close native strategy recording");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      gpu.queue->ExecuteCommandLists(1, lists);
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "signal native strategy recording");
      presets::internal::Handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
      Check(event.value != nullptr && event.value != INVALID_HANDLE_VALUE
                && SUCCEEDED(gpu.fence->SetEventOnCompletion(gpu.serial, event.value))
                && WaitForSingleObject(event.value, 10000) == WAIT_OBJECT_0, "bounded native strategy GPU completion");
      native::ResolveAdaptiveBorrowedDeliveries();
      Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
          "native strategy list is no longer replayable");
      native::DrainRetiredResources(true);
      gpu.uploads.clear();
      gpu.retained_resources.clear();
    };
    auto color = gpu.Texture(640, 360, DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto depth = gpu.Texture(640, 360, DXGI_FORMAT_R32_FLOAT);
    auto motion = gpu.Texture(640, 360, DXGI_FORMAT_R16G16_FLOAT);
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC output_desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = 960, .Height = 540,
        .DepthOrArraySize = 1, .MipLevels = 1, .Format = DXGI_FORMAT_R16G16B16A16_FLOAT, .SampleDesc = {1, 0},
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    ComPtr<ID3D12Resource> output;
    Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &output_desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output))), "native strategy original output");
    std::vector<uint16_t> color_values(640 * 360 * 4), motion_values(640 * 360 * 2, 0);
    std::vector<float> depth_values(640 * 360);
    for (uint32_t y = 0; y < 360; ++y) {
      for (uint32_t x = 0; x < 640; ++x) {
        const auto pixel = static_cast<size_t>(y) * 640 + x;
        const float u = static_cast<float>(x) / 639.f, v = static_cast<float>(y) / 359.f;
        color_values[pixel * 4] = DirectX::PackedVector::XMConvertFloatToHalf(.12f + .65f * u);
        color_values[pixel * 4 + 1] = DirectX::PackedVector::XMConvertFloatToHalf(.1f + .65f * v);
        color_values[pixel * 4 + 2] = DirectX::PackedVector::XMConvertFloatToHalf(.15f + .5f * (1.f - u * v));
        color_values[pixel * 4 + 3] = DirectX::PackedVector::XMConvertFloatToHalf(1.f);
        depth_values[pixel] = .2f + .6f * v;
      }
    }
    auto color_upload = gpu.CreateTextureTransfer(color.Get());
    auto motion_upload = gpu.CreateTextureTransfer(motion.Get());
    auto depth_upload = gpu.CreateTextureTransfer(depth.Get());
    for (const auto& [texture, transfer, data, bytes, row] : {
             std::tuple{color.Get(), &color_upload, reinterpret_cast<const uint8_t*>(color_values.data()), color_values.size() * 2, 640u * 8},
             std::tuple{motion.Get(), &motion_upload, reinterpret_cast<const uint8_t*>(motion_values.data()), motion_values.size() * 2, 640u * 4},
             std::tuple{depth.Get(), &depth_upload, reinterpret_cast<const uint8_t*>(depth_values.data()), depth_values.size() * 4, 640u * 4}}) {
      gpu.Transition(texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.UploadTexture(texture, *transfer, data, bytes, row);
      gpu.Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, NATIVE_INPUT_STATE);
    }
    submit();
    report["runtime"] = native::direct_runtime_requirements.file_version;
    report["nr_sha256"] = native::direct_runtime_sha256;
    report["input_color_crc32"] = renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(color_values.data()), color_values.size() * 2);
    report["input_depth_crc32"] = renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(depth_values.data()), depth_values.size() * 4);
    report["input_motion_crc32"] = renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(motion_values.data()), motion_values.size() * 2);
    report["internal_model"] = "unknown";
    using Planes = std::array<std::vector<uint8_t>, 3>;
    std::array<Planes, 4> baseline;
    presets::Json baseline_creates, baseline_histories;
    bool equal = true;
    NVSDK_NGX_Handle* sr_handle = nullptr;
    bool runtime_closed = false;
    const auto cleanup = [&] {
      calls.measuring = false;
      calls.sr_readback = nullptr;
      if (sr_handle != nullptr) {
        Check(NVSDK_NGX_SUCCEED(native::HookedReleaseFeatureSlot<Runtime::SR_SLOT>(sr_handle)), "release native strategy source");
        sr_handle = nullptr;
      }
      submit();
      native::ReleaseFeatureStates();
      native::RetireAllWorksets();
      native::DrainRetiredResources(true);
      Check(native::retired_nr_features.empty() && native::retired_final_resources.empty(),
          "native strategy retires every completed Feature and surface");
    };
    try {
      for (const uint32_t strategy : {0u, 2u, 3u}) {
        calls.creates = presets::Json::array();
        calls.evaluations = presets::Json::array();
        const presets::Document requested{.scope = presets::Scope::QUALITY,
            .settings = {{presets::ADAPTIVE_RESOURCE_STRATEGY_KEY, strategy}, {"NRAdaptiveEnabled", true},
                {"NRAdaptiveLimitedResidency", false}, {"NRAdaptiveLevelCount", 4u}, {"NRAdaptiveMinScale", .5f}}};
        std::string error;
        const bool committed = native::preset_ui::Apply(requested, false, false, &error);
        Check(committed, error.c_str());
        auto* parameters = runtime.sr_parameters;
        parameters->Set(NVSDK_NGX_Parameter_Color, color.Get());
        parameters->Set(NVSDK_NGX_Parameter_Output, output.Get());
        parameters->Set(NVSDK_NGX_Parameter_Depth, depth.Get());
        parameters->Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
        parameters->Set(NVSDK_NGX_Parameter_Width, 640u);
        parameters->Set(NVSDK_NGX_Parameter_Height, 360u);
        parameters->Set(NVSDK_NGX_Parameter_OutWidth, 960u);
        parameters->Set(NVSDK_NGX_Parameter_OutHeight, 540u);
        parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 640u);
        parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 360u);
        parameters->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
        parameters->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
        parameters->Set(NVSDK_NGX_Parameter_Reset, 1);
        Check(NVSDK_NGX_SUCCEED(native::HookedCreateFeatureSlot<Runtime::SR_SLOT>(
            gpu.list.Get(), native::kFeatureDlss, parameters, &sr_handle)), "create native strategy SR source");
        submit();
        const auto record = [&](bool reset) {
          ++native::present_generation;
          parameters->Set(NVSDK_NGX_Parameter_Reset, reset ? 1 : 0);
          const auto result = native::HookedEvaluateFeatureSlot<Runtime::SR_SLOT>(gpu.list.Get(), sr_handle, parameters, nullptr);
          Check(NVSDK_NGX_SUCCEED(result), "native strategy successful SR producer");
          submit();
        };
        uint32_t preparation_frames = 0;
        for (; preparation_frames < 64; ++preparation_frames) {
          record(true);
          auto& feature = native::features.at(sr_handle);
          if (feature.adaptive_cache != nullptr) native::PollAdaptiveCache(feature.adaptive_cache.get());
          if (feature.adaptive_session != nullptr && feature.adaptive_session->prepared) break;
        }
        auto& feature = native::features.at(sr_handle);
        Check(preparation_frames < 64 && feature.adaptive_session != nullptr && feature.adaptive_cache != nullptr,
            "native fallback completes its four-tier collection within 64 source frames");
        auto& session = *feature.adaptive_session;
        Check(session.ladder.count == 4 && session.settings.resource_strategy == adaptive::OFF
                  && session.status.requested_resource_strategy == strategy
                  && session.status.resource_fallback == (strategy == 0 ? adaptive::ResourceFallback::NONE : adaptive::ResourceFallback::LEVELS)
                  && calls.creates.size() == 4,
            "official setting preserves four logical tiers and reports the real fallback and independent Create count");
        report["cases"].push_back({{"requested_strategy", strategy}, {"effective_strategy", 0},
            {"fallback", strategy == 0 ? "none" : "levels"}, {"preparation_frames", preparation_frames + 1},
            {"create_calls", calls.creates}, {"tiers", presets::Json::array()}});
        if (strategy == 0) baseline_creates = calls.creates;
        else Check(calls.creates == baseline_creates, "fallback actual Create contracts match strategy zero");
        auto& condition = report["cases"].back();
        presets::Json histories = presets::Json::array();
        for (uint32_t tier = 0; tier < 4; ++tier) {
          session.Request(tier, native::SteadyNowNs());
          uint32_t waiting = 0;
          for (; waiting < 16 && (feature.adaptive_cache->active != tier || session.status.applied != session.ladder.sizes[tier]); ++waiting) record(true);
          Check(waiting < 16, "a prepared native tier reaches its real delivery boundary");
          auto original = gpu.CreateTextureTransfer(output.Get());
          calls.evaluations = presets::Json::array();
          calls.measuring = true;
          for (uint32_t frame = 0; frame < 4; ++frame) {
            calls.sr_readback = (frame == 3 ? &original : nullptr);
            record(frame == 0);
          }
          calls.measuring = false;
          calls.sr_readback = nullptr;
          Check(calls.evaluations.size() == 4 && calls.evaluations[0]["reset"] == 1
                    && calls.evaluations[1]["reset"] == 0 && calls.evaluations[2]["reset"] == 0 && calls.evaluations[3]["reset"] == 0,
              "all conditions use exactly four real active Evaluates after one Reset");
          histories.push_back(calls.evaluations);
          const auto& resources = native::worksets.at({sr_handle, output.Get(), false, tier});
          const std::array<ID3D12Resource*, 3> images{resources.proxy, resources.nr_output, output.Get()};
          const std::array<D3D12_RESOURCE_STATES, 3> states{resources.proxy_state, resources.nr_output_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
          std::array<d3d12_test::TextureTransfer, 3> transfers;
          for (size_t index = 0; index < images.size(); ++index) {
            transfers[index] = gpu.CreateTextureTransfer(images[index]);
            gpu.Transition(images[index], states[index], D3D12_RESOURCE_STATE_COPY_SOURCE);
            gpu.RecordTextureReadback(images[index], transfers[index]);
            gpu.Transition(images[index], D3D12_RESOURCE_STATE_COPY_SOURCE, states[index]);
          }
          submit();
          const auto original_rgba = gpu.ReadTexture(original);
          presets::Json sample{{"tier", tier}, {"grid", {session.ladder.sizes[tier][0].width, session.ladder.sizes[tier][0].height}},
              {"history", calls.evaluations}, {"fence", gpu.serial}, {"completed_fence", gpu.fence->GetCompletedValue()}};
          constexpr std::array<const char*, 3> names{"proxy", "neural", "output"};
          for (size_t plane = 0; plane < images.size(); ++plane) {
            auto rgba = gpu.ReadTexture(transfers[plane]);
            sample["crc32"][names[plane]] = renodx::utils::hash::ComputeCRC32(rgba.data(), rgba.size());
            if (plane == 2) {
              Check(rgba.size() == original_rgba.size(), "native resolve writes the original output extent");
              uint32_t alpha_changes = 0;
              for (size_t pixel = 0; pixel < rgba.size(); pixel += 8) {
                alpha_changes += rgba[pixel + 6] != original_rgba[pixel + 6] || rgba[pixel + 7] != original_rgba[pixel + 7];
              }
              sample["alpha_changed_pixels"] = alpha_changes;
              Check(alpha_changes == 0, "native fallback preserves the original SR output alpha bits");
            }
            if (strategy == 0) baseline[tier][plane] = rgba;
            else {
              const auto& reference = baseline[tier][plane];
              Check(reference.size() == rgba.size(), "native comparison planes share the actual grid");
              uint32_t rgb_changes = 0;
              std::array<float, 4> maximum{};
              std::array<double, 4> absolute_sum{};
              for (size_t pixel = 0; pixel < rgba.size(); pixel += 8) {
                bool rgb_changed = false;
                for (size_t channel = 0; channel < 4; ++channel) {
                  uint16_t expected, actual;
                  std::memcpy(&expected, reference.data() + pixel + channel * 2, 2);
                  std::memcpy(&actual, rgba.data() + pixel + channel * 2, 2);
                  const float value = DirectX::PackedVector::XMConvertHalfToFloat(actual);
                  const float before = DirectX::PackedVector::XMConvertHalfToFloat(expected);
                  Check(std::isfinite(value) && std::isfinite(before), "native readback has finite FP16 channels");
                  const auto difference = std::abs(value - before);
                  maximum[channel] = std::max(maximum[channel], difference);
                  absolute_sum[channel] += difference;
                  rgb_changed |= channel < 3 && expected != actual;
                }
                rgb_changes += rgb_changed;
              }
              for (auto& value : absolute_sum) value /= static_cast<double>(rgba.size() / 8);
              const bool bitwise_equal = rgba == reference;
              equal &= bitwise_equal;
              report["comparisons"].push_back({{"requested_strategy", strategy}, {"tier", tier}, {"plane", names[plane]},
                  {"bitwise_equal", bitwise_equal}, {"differing_rgb_pixels", rgb_changes},
                  {"maximum_absolute_rgba", maximum}, {"mean_absolute_rgba", absolute_sum}});
            }
          }
          condition["tiers"].push_back(std::move(sample));
          checkpoint();
        }
        if (strategy == 0) baseline_histories = histories;
        else Check(histories == baseline_histories, "actual Evaluate grids, Reset sequence and guide rectangles match strategy zero");
        cleanup();
      }
      report["equal_to_strategy_zero"] = equal;
      report["cleanup"] = "sources, Features and physical worksets retired after GPU completion";
      runtime.report = {{"scope", "resource strategy native fallback shared Runtime teardown"}};
      runtime_closed = true;
      runtime.Shutdown();
      fixed::native_resize_fixed_inputs.clear();
      report["status"] = (equal ? "Passed" : "Unexpected pixel difference");
      checkpoint();
      Check(equal, "same input and model history produce identical independent-path readbacks for strategies 0, 2 and 3");
      std::cout << "PASS resource-strategy-native evidence=NATIVE_GPU+NATIVE_NGX strategies=0,2,3 levels=4 history=reset+4 alpha=preserved\n";
      return 0;
    } catch (...) {
      if (!runtime_closed) {
        cleanup();
        runtime.report = {{"scope", "resource strategy native fallback failure teardown"}};
        runtime_closed = true;
        runtime.Shutdown();
      }
      fixed::native_resize_fixed_inputs.clear();
      throw;
    }
  } catch (const std::exception& error) {
    report["status"] = "Failed";
    report["failure"] = error.what();
    checkpoint();
    std::cerr << "FAIL resource-strategy-native: " << error.what() << '\n';
    return 1;
  }
}
#endif

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
