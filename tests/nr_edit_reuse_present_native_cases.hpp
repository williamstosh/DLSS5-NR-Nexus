/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>
#include <utils/hash.hpp>

#include "d3d12_test_gpu.hpp"

// Included by the ordinary Present fixture inside the NGX-enabled FG test binary.
namespace nr_edit_reuse_present_native_cases {
namespace native = renodx::addons::dlss5::internal;
using d3d12_test::Check;
using d3d12_test::Gpu;

struct Runtime {
  // The Core and SR DLLs can take low slots when OnPresent installs its detours.
  static constexpr int SR_SLOT = native::kMaxNgxSlots - 1;

  NVSDK_NGX_Parameter* sr_parameters = nullptr;
  uint32_t sr_calls = 0;
  uint32_t nr_calls = 0;
  uint32_t nr_creates = 0;
  uint32_t nr_requested_preset = 0;
  int32_t nr_reset = -1;
  NVSDK_NGX_Result sr_result{};
  NVSDK_NGX_Result nr_result{};
  NVSDK_NGX_Result nr_create_result{};
  ID3D12Resource* sr_color = nullptr;
  ID3D12Resource* nr_depth = nullptr;
  ID3D12Resource* nr_motion = nullptr;
  nlohmann::json report;

  Runtime(const Gpu& gpu, const char* project_id_path) : device(gpu.device.Get()) {
    native::addon_module = GetModuleHandleW(nullptr);
    native::PrimeNgxLoaderSymbols(device);
    Check(native::NgxLoaderView().nr_ready && native::NgxLoaderView().nr_requirements.dual_models,
          "ordinary Present x2 loads a signed NR Runtime with Model B support");
    std::ifstream project_id_file(project_id_path);
    std::string project_id;
    Check(std::getline(project_id_file, project_id) && project_id.size() == 36,
          "read the existing isolated NGX ProjectID");
    const std::wstring runtime_directory = native::AddonDirectory().wstring();
    const wchar_t* paths[]{runtime_directory.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo = {paths, 1};
    info.LoggingInfo = {control_mask_gpu_cases::NativeNgxSdkLog, NVSDK_NGX_LOGGING_LEVEL_ON, true};
    const auto core_init = NVSDK_NGX_D3D12_Init_with_ProjectID(
        project_id.c_str(), NVSDK_NGX_ENGINE_TYPE_CUSTOM, "NR-Nexus-ControlMask-P0-offline-1",
        runtime_directory.c_str(), device, &info);
    if (NVSDK_NGX_FAILED(core_init)) {
      std::cerr << "Present x2 SDK Init result=" << static_cast<uint32_t>(core_init) << '\n';
    }
    Check(NVSDK_NGX_SUCCEED(core_init), "ordinary Present x2 initializes the real public D3D12 NGX Core");

    try {
      Check(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&sr_parameters)) && sr_parameters != nullptr,
            "ordinary Present x2 uses real SR capability parameters");
      sr_parameters->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(NVSDK_NGX_PerfQuality_Value_MaxQuality));
      sr_parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
      sr_parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
      sr_parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
          static_cast<int>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_IsHDR
                           | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure));
      sr_parameters->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.f);
      sr_parameters->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.f);
      sr_parameters->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 1000.f / 60.f);
      sr_parameters->Set(NVSDK_NGX_Parameter_Reset, 1);
      sr_parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.f);
      sr_parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f);
      for (const char* offset : {NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
               NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
               NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
               NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,
               NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y}) {
        sr_parameters->Set(offset, 0u);
      }

      native::PrimeNgxLoaderSymbols(device);
      const auto& provider = native::NgxLoaderView();
      Check(provider.core_ready && provider.core_create != nullptr && provider.core_evaluate != nullptr,
            "production loader resolves the SDK-initialized public NGX Core exports");
      std::scoped_lock lock(native::runtime_mutex);
      // Use the already initialized public Core on this exact device, as in
      // the native Render fixture. Present's own create/evaluate path stays in use.
      native::direct_api = {.module = provider.nr_module, .allocate = provider.core_allocate,
          .destroy = provider.core_destroy, .create = provider.core_create, .evaluate = provider.core_evaluate,
          .release = provider.core_release};
      native::direct_load_state = native::DirectLoadState::Ready;
      native::direct_runtime_requirements = provider.nr_requirements;
      native::direct_runtime_sha256 = provider.nr_sha256;
      native::direct_device = device;
      device->AddRef();
      native::direct_device_native = device;
      Check(native::EnsureDirectRuntime(gpu.list.Get()) && native::parameter_runtime_via_core.load(),
            "ordinary Present x2 adopts the real public Core for Feature 18");
      nr_create = native::core_create_feature;
      nr_evaluate = native::core_evaluate_feature;
      native::native_fg_public_off = true;
      native::native_fg_declaration = native::NativeFgDeclaration::kOff;
      native::core_create_feature = CreateNr;
      native::core_evaluate_feature = EvaluateNr;
      native::ngx_slot_real[SR_SLOT].create = reinterpret_cast<void*>(CreateSr);
      native::ngx_slot_real[SR_SLOT].evaluate = reinterpret_cast<void*>(EvaluateSr);
      native::ngx_slot_real[SR_SLOT].release = reinterpret_cast<void*>(ReleaseSr);
      active = this;
    } catch (...) {
      native::ReleaseCom(native::direct_device);
      if (sr_parameters != nullptr) NVSDK_NGX_D3D12_DestroyParameters(sr_parameters);
      NVSDK_NGX_D3D12_Shutdown1(device);
      throw;
    }
  }

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  ~Runtime() {
    native::core_create_feature = nr_create;
    native::core_evaluate_feature = nr_evaluate;
    native::ngx_slot_real[SR_SLOT] = {};
    active = nullptr;
  }

  void Shutdown() {
    // The caller has retired the SR handle, Present stream and completed work.
    {
      std::scoped_lock lock(native::runtime_mutex);
      native::UnhookInstalledDetours();
      native::ReleaseCom(native::direct_device);
      native::direct_device_native = nullptr;
    }
    const auto destroy = NVSDK_NGX_D3D12_DestroyParameters(sr_parameters);
    sr_parameters = nullptr;
    const auto shutdown = NVSDK_NGX_D3D12_Shutdown1(device);
    report["sr_create_result"] = static_cast<uint32_t>(sr_create_result);
    report["sr_release_result"] = static_cast<uint32_t>(sr_release_result);
    report["sr_destroy_parameters_result"] = static_cast<uint32_t>(destroy);
    report["core_shutdown_result"] = static_cast<uint32_t>(shutdown);
    Check(NVSDK_NGX_SUCCEED(destroy) && NVSDK_NGX_SUCCEED(shutdown), "ordinary Present x2 closes the real public NGX Core");
    std::ofstream file("nr-edit-reuse-present-native.json");
    file << report.dump(2) << '\n';
    file.close();
    Check(!file.fail(), "ordinary Present x2 saves native evidence");
  }

 private:
  inline static Runtime* active = nullptr;
  ID3D12Device* device = nullptr;
  native::DirectCreateFn nr_create = nullptr;
  native::DirectEvaluateFn nr_evaluate = nullptr;
  NVSDK_NGX_Result sr_create_result{};
  NVSDK_NGX_Result sr_release_result{};

  static NVSDK_NGX_Result NVSDK_CONV CreateSr(
      ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature,
      NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
    active->sr_create_result = NVSDK_NGX_D3D12_CreateFeature(list, feature, parameters, handle);
    return active->sr_create_result;
  }

  static NVSDK_NGX_Result NVSDK_CONV EvaluateSr(
      ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
      const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback progress) {
    ++active->sr_calls;
    active->sr_color = native::GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Color);
    active->sr_result = NVSDK_NGX_D3D12_EvaluateFeature(list, handle, parameters, progress);
    return active->sr_result;
  }

  static NVSDK_NGX_Result NVSDK_CONV ReleaseSr(NVSDK_NGX_Handle* handle) {
    active->sr_release_result = NVSDK_NGX_D3D12_ReleaseFeature(handle);
    return active->sr_release_result;
  }

  static NVSDK_NGX_Result NVSDK_CONV CreateNr(
      ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature,
      const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
    if (feature != native::kFeatureDlssNr) return active->nr_create(list, feature, parameters, handle);
    ++active->nr_creates;
    active->nr_requested_preset = native::GetUInt(parameters, "DLSSNR.Hint.Render.Preset");
    active->nr_create_result = active->nr_create(list, feature, parameters, handle);
    return active->nr_create_result;
  }

  static NVSDK_NGX_Result NVSDK_CONV EvaluateNr(
      ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
      const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback progress) {
    ++active->nr_calls;
    active->nr_reset = native::GetInt(parameters, "DLSSNR.Reset", -1);
    active->nr_depth = native::GetD3D12Resource(parameters, "DLSSNR.Depth");
    active->nr_motion = native::GetD3D12Resource(parameters, "DLSSNR.MVec");
    active->nr_result = active->nr_evaluate(list, handle, parameters, progress);
    return active->nr_result;
  }
};

template <typename Stats, typename PresentFrame, typename ReadBack, typename ReadSr, typename Swap>
void Run(Runtime& runtime, const Gpu& gpu, const Stats& stats, const PresentFrame& present_frame,
         const ReadBack& read_back, const ReadSr& read_sr, const Swap& swap) {
  using nlohmann::json;
  auto* color = native::GetD3D12Resource(runtime.sr_parameters, NVSDK_NGX_Parameter_Color);
  auto* sr_output = native::GetD3D12Resource(runtime.sr_parameters, NVSDK_NGX_Parameter_Output);
  auto* depth = native::GetD3D12Resource(runtime.sr_parameters, NVSDK_NGX_Parameter_Depth);
  auto* motion = native::GetD3D12Resource(runtime.sr_parameters, NVSDK_NGX_Parameter_MotionVectors);
  Check(color != nullptr && sr_output != nullptr && depth != nullptr && motion != nullptr,
        "ordinary Present x2 has real SR input, output and guides");
  const auto source_desc = color->GetDesc();
  const auto output_desc = sr_output->GetDesc();
  Check(source_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && output_desc.Format == source_desc.Format,
        "ordinary Present x2 passes real FP16 surfaces through SR");
  constexpr uint32_t WARMUP_LIMIT = native::kContractDebounceFrames + 8;
  auto& report = runtime.report;
  report = {{"evidence", "NATIVE_GPU+NATIVE_NGX"}, {"stage", "ordinary D3D12 Present"},
      {"format", "RGBA16F scRGB"}, {"render_size", {source_desc.Width, source_desc.Height}},
      {"output_size", {output_desc.Width, output_desc.Height}}, {"jitter", {0.f, 0.f}},
      {"motion", "zero MVLowRes, scale 1,1"}, {"motion_format", static_cast<uint32_t>(motion->GetDesc().Format)},
      {"depth_format", static_cast<uint32_t>(depth->GetDesc().Format)}, {"internal_model", "unknown"},
      {"warmup_limit", WARMUP_LIMIT}, {"frames", json::array()}};

  const auto frame = [&](bool reset) {
    const auto sr_calls = runtime.sr_calls;
    const auto nr_calls = runtime.nr_calls;
    present_frame(reset);
    Check(runtime.sr_calls == sr_calls + 1 && NVSDK_NGX_SUCCEED(runtime.sr_result)
              && runtime.sr_color == color
              && native::GetD3D12Resource(runtime.sr_parameters, NVSDK_NGX_Parameter_Color) == color,
          "ordinary Present x2 calls real SR once with the original Color binding");
    if (runtime.nr_calls > nr_calls) {
      if (NVSDK_NGX_FAILED(runtime.nr_result)) {
        std::cerr << "Present x2 real NR Evaluate result=" << static_cast<uint32_t>(runtime.nr_result) << '\n';
      }
      Check(NVSDK_NGX_SUCCEED(runtime.nr_result) && runtime.nr_depth != nullptr && runtime.nr_depth != depth
                && runtime.nr_motion != nullptr && runtime.nr_motion != motion,
            "ordinary Present x2 real Model B Evaluate uses captured guides");
    }
    return stats();
  };
  const auto capture = [&](bool edited) {
    const auto sr_pixels = read_sr();
    const auto pixels = read_back();
    Check(pixels.size() == output_desc.Width * output_desc.Height && pixels.size() == sr_pixels.size(),
          "ordinary Present x2 reads the complete SR output and actual DXGI buffer");
    float max_rgb_delta = 0.f;
    bool finite = true;
    bool alpha_preserved = true;
    const auto half = DirectX::PackedVector::XMConvertHalfToFloat;
    for (size_t index = 0; index < pixels.size(); ++index) {
      alpha_preserved &= pixels[index][3] == sr_pixels[index][3];
      for (size_t channel = 0; channel < 3; ++channel) {
        const float sr = half(sr_pixels[index][channel]);
        const float back = half(pixels[index][channel]);
        finite &= std::isfinite(sr) && std::isfinite(back);
        max_rgb_delta = std::max(max_rgb_delta, std::abs(sr - back));
      }
    }
    Check(finite && alpha_preserved, "ordinary Present x2 has finite RGB and preserves SR alpha");
    if (edited) {
      Check(max_rgb_delta > 0.f, "ordinary Present x2 changes RGB in the actual backbuffer");
    } else {
      Check(pixels == sr_pixels, "ordinary Present x2 retains the exact SR output without an NR submission");
    }
    const auto crc = [](const auto& image) {
      return renodx::utils::hash::ComputeCRC32(
          reinterpret_cast<const uint8_t*>(image.data()), image.size() * sizeof(image.front()));
    };
    const auto completed = gpu.fence->GetCompletedValue();
    const auto device_status = gpu.device->GetDeviceRemovedReason();
    Check(completed != UINT64_MAX && completed >= gpu.serial && SUCCEEDED(device_status),
          "ordinary Present x2 completes SR, Present and readbacks on a healthy native queue");
    return json{{"backbuffer_crc32", crc(pixels)}, {"sr_output_crc32", crc(sr_pixels)},
        {"alpha_preserved", alpha_preserved}, {"max_rgb_delta", max_rgb_delta},
        {"queue_fence", gpu.serial}, {"completed_queue_fence", completed}, {"device_status", device_status}};
  };
  const auto ring_completion = [&] {
    Check(native::present_ring.queue == gpu.queue.Get() && native::present_ring.fence != nullptr,
          "ordinary Present x2 submits to the same native DXGI queue as SR");
    const uint64_t completed = native::present_ring.fence->GetCompletedValue();
    Check(completed != UINT64_MAX && completed >= native::present_ring.submitted,
          "ordinary Present x2 fence proves its private recording completed");
    return completed;
  };

  native::enabled = false;
  const auto baseline_before = stats();
  const auto baseline_after = frame(true);
  Check(runtime.nr_calls == 0 && runtime.nr_creates == 0 && baseline_after.outputs == baseline_before.outputs
            && baseline_after.consumed_serial == baseline_before.consumed_serial
            && baseline_after.ring_submissions == baseline_before.ring_submissions,
        "NR Off preserves the real SR base without creating, evaluating or submitting NR");
  report["nr_off"] = {{"sr_calls", 1}, {"sr_result", static_cast<uint32_t>(runtime.sr_result)},
      {"nr_calls", 0}, {"readback", capture(false)}};
  swap();

  bool warmed = false;
  uint32_t prepared = 0;
  native::enabled = true;
  for (uint32_t attempt = 1; attempt <= WARMUP_LIMIT; ++attempt) {
    const auto before = stats();
    const auto nr_calls = runtime.nr_calls;
    const auto after = frame(false);
    if (after.outputs != before.outputs) {
      Check(after.outputs == before.outputs + 1 && runtime.nr_calls == nr_calls + 1
                && after.refreshes == before.refreshes + 1 && after.reuses == before.reuses
                && after.consumed_serial > before.consumed_serial && after.ring_submissions == before.ring_submissions + 1,
            "ordinary Present x2 first accepted native frame refreshes once");
      report["warmup_attempts"] = attempt;
      report["warmup_prepared_only_submissions"] = prepared;
      report["warmup_completed_present_fence"] = ring_completion();
      report["warmup_refresh_readback"] = capture(true);
      swap();
      warmed = true;
      break;
    }
    Check(runtime.nr_calls == nr_calls && after.consumed_serial == before.consumed_serial
              && after.refreshes == before.refreshes && after.reuses == before.reuses,
          "native warmup leaves the base unconsumed until an output completes");
    const auto retained = capture(false);
    if (after.ring_submissions > before.ring_submissions) {
      prepared += 1;
      if (prepared == 1) report["prepared_only_readback"] = retained;
    }
    swap();
  }
  if (!warmed) {
    std::cerr << "Present x2 warmup exhausted: SR calls=" << runtime.sr_calls << " NR creates=" << runtime.nr_creates
              << " last NR Create result=" << static_cast<uint32_t>(runtime.nr_create_result) << '\n';
  }
  Check(warmed && runtime.nr_creates > 0 && NVSDK_NGX_SUCCEED(runtime.nr_create_result)
            && runtime.nr_requested_preset == 2, "bounded ordinary Present warmup creates real Feature 18 with a Model B request");
  report["nr_create_calls_through_warmup"] = runtime.nr_creates;
  report["nr_create_result"] = static_cast<uint32_t>(runtime.nr_create_result);
  report["requested_model"] = "B";

  constexpr std::array EXPECTED_REFRESH{true, false, true, false};
  for (size_t index = 0; index < EXPECTED_REFRESH.size(); ++index) {
    const bool refresh = EXPECTED_REFRESH[index];
    const auto before = stats();
    const auto nr_calls = runtime.nr_calls;
    const auto after = frame(index == 0);
    std::cout << "PRESENT_X2_NATIVE frame=" << index << " nr_calls=" << runtime.nr_calls - nr_calls
              << " refreshes=" << after.refreshes - before.refreshes << " reuses=" << after.reuses - before.reuses
              << " outputs=" << after.outputs - before.outputs << " consumed=" << after.consumed_serial << '\n';
    Check(after.outputs == before.outputs + 1 && after.consumed_serial > before.consumed_serial
              && runtime.nr_calls == nr_calls + refresh && after.refreshes == before.refreshes + refresh
              && after.reuses == before.reuses + !refresh && after.history_sequence > before.history_sequence
              && after.ring_submissions == before.ring_submissions + 1,
          "ordinary Present x2 delivers four native bases in real NR cadence 1/0/1/0");
    report["frames"].push_back({{"frame", index}, {"sr_calls", 1}, {"sr_result", static_cast<uint32_t>(runtime.sr_result)},
        {"sr_retains_color", true}, {"nr_calls", runtime.nr_calls - nr_calls},
        {"nr_result", (refresh ? json(static_cast<uint32_t>(runtime.nr_result)) : json(nullptr))},
        {"nr_reset", (refresh ? json(runtime.nr_reset) : json(nullptr))}, {"refresh", refresh},
        {"consumed_serial", after.consumed_serial}, {"history_sequence", after.history_sequence},
        {"present_fence", after.ring_submissions}, {"completed_present_fence", ring_completion()},
        {"readback", capture(true)}});
    swap();
  }

  const auto& provider = native::NgxLoaderView();
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  Check(SUCCEEDED(gpu.factory->EnumAdapterByLuid(gpu.device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))),
        "ordinary Present x2 identifies the actual native adapter");
  DXGI_ADAPTER_DESC1 adapter_desc{};
  Check(SUCCEEDED(adapter->GetDesc1(&adapter_desc)), "ordinary Present x2 records adapter and driver identity");
  report["adapter"] = {{"vendor_id", adapter_desc.VendorId}, {"device_id", adapter_desc.DeviceId},
      {"driver_x100", provider.nr_requirements.driver_x100}};
  report["loaded_ngx_modules"] = json::array();
  const auto runtime_directory = native::AddonDirectory();
  for (const HMODULE module : {provider.core, provider.nr_module, GetModuleHandleW(L"nvngx_dlss.dll")}) {
    std::array<wchar_t, 32768> buffer{};
    Check(module != nullptr, "ordinary Present x2 loaded all three real NGX modules");
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    Check(length != 0 && length < buffer.size(), "ordinary Present x2 records the actual NGX module identity");
    const std::filesystem::path module_path(buffer.data());
    if (module != provider.core) {
      Check(std::filesystem::equivalent(module_path, runtime_directory / module_path.filename()),
            "ordinary Present x2 uses the supplied Runtime in the isolated test directory");
    }
    const auto sha256 = native::ComputeRuntimeSha256(module_path);
    Check(!sha256.empty(), "ordinary Present x2 hashes the loaded NGX module");
    report["loaded_ngx_modules"].push_back({{"module", module_path.filename().string()}, {"sha256", sha256},
        {"version", native::ReadNrRuntimeRequirements(module_path, gpu.device.Get()).file_version}});
  }
}

}  // namespace nr_edit_reuse_present_native_cases
