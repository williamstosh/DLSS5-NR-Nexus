/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <vector>

#include <DirectXPackedVector.h>
#include <nlohmann/json.hpp>

// Included after the native Present route fixture and the GPU readback helper.
namespace xefg_inline_handoff_cases {
namespace addon = renodx::addons::dlss5;

inline void Run(Gpu& gpu, Session& session, ID3D12Resource* back,
                ID3D12Resource* sdk_depth, ID3D12Resource* sdk_motion,
                const auto& submit, const auto& wait_fence, nlohmann::json* native_report = nullptr) {
  using Stage = addon::stage_plan::Stage;
  using Work = submission::RecordedWork;
  using stage_execution_cases::Capture;
  using stage_execution_cases::Snapshot;
  const bool native_ngx = native_report != nullptr;
  ComPtr<IDXGIAdapter3> memory_adapter;
  uint64_t sampled_peak_local_bytes = 0;
  nlohmann::json memory_samples = nlohmann::json::array();
  const auto sample_memory = [&](const char* phase) {
    if (!native_ngx) return;
    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    Check(SUCCEEDED(memory_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)),
          "handoff samples actual process local GPU memory");
    sampled_peak_local_bytes = std::max(sampled_peak_local_bytes, memory.CurrentUsage);
    memory_samples.push_back({{"phase", phase}, {"current_usage", memory.CurrentUsage}, {"budget", memory.Budget}});
  };
  if (native_ngx) {
    Check(SUCCEEDED(gpu.factory->EnumAdapterByLuid(gpu.device->GetAdapterLuid(), IID_PPV_ARGS(&memory_adapter))),
          "handoff identifies the actual GPU memory adapter");
    sample_memory("private_ready");
  }
  const auto key = std::pair{false, reinterpret_cast<uintptr_t>(session.handle)};
  const auto epoch = session.context->epoch;
  const auto fg_held = [&] {
    return xefg::Enabled() && session.context->enabled && session.context->epoch == epoch;
  };
  const auto starting_module = native::direct_api.module;
  const bool legacy_private = native::direct_api.private_instance;
  const auto same_service_runtime = [&] {
    return native::direct_api.module == starting_module && native::direct_api.private_instance == legacy_private;
  };
  Check(fg_held() && xefg::CurrentResult() == fg::Result::SUBMITTED
            && starting_module != nullptr && native::TeardownProof() == Work::kComplete,
        "inline handoff starts with completed, non-replayable XeFG work with SDK FG on");

  PresentRingFaceProbe route(&gpu, back, session.proxy.Get(), PresentRingFaceProbe::Mode::NATIVE_ROUTE);
  native::present_pre_fg = 1;
  funnel_accounting_tests::Restore restore;
  restore.Value(&native::ngx_slot_real[0]);
  restore.Atomic(&native::stack_passes);
  restore.Atomic(&native::dedupe_mode);
  restore.Atomic(&native::look_mode);
  restore.Atomic(&native::detail_stability);
  native::stack_passes = 1;
  native::dedupe_mode = addon::kDedupePerEvaluate;
  native::look_mode = false;
  native::detail_stability = 1;

  bool real_sr = false;
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
  real_sr = native_ngx && std::filesystem::is_regular_file(native::AddonDirectory() / L"nvngx_dlss.dll");
#endif
  const auto nr_calls = [&] {
    return (native_ngx ? native_runtime_evaluation_results.size() : static_cast<size_t>(model_calls));
  };
  const auto previous_nr_succeeded = [&] {
    return !native_ngx || (!native_runtime_evaluation_results.empty()
        && NVSDK_NGX_SUCCEED(native_runtime_evaluation_results.back()));
  };
  const auto back_desc = back->GetDesc();
  const auto width = static_cast<uint32_t>(back_desc.Width);
  const auto height = back_desc.Height;
  const auto render_width = (real_sr ? width * 2 / 3 : width);
  const auto render_height = (real_sr ? height * 2 / 3 : height);
  const auto sr_format = (native_ngx ? DXGI_FORMAT_R16G16B16A16_FLOAT : back_desc.Format);
  auto color = gpu.Texture(render_width, render_height, sr_format);
  auto motion = gpu.Texture(render_width, render_height,
      real_sr ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R32G32_FLOAT);
  auto depth = gpu.Texture(render_width, render_height, DXGI_FORMAT_R32_FLOAT);
  auto output_desc = color->GetDesc();
  output_desc.Width = width;
  output_desc.Height = height;
  output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> output;
  Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &output_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output))), "handoff SR output");
  d3d12_test::TextureTransfer color_transfer;
  d3d12_test::TextureTransfer back_transfer;
  std::vector<uint16_t> source_rgba;
  std::vector<uint8_t> back_rgba;
  if (native_ngx) {
    color_transfer = gpu.CreateTextureTransfer(color.Get());
    back_transfer = gpu.CreateTextureTransfer(back);
    source_rgba.resize(static_cast<size_t>(render_width) * render_height * 4);
    back_rgba.resize(static_cast<size_t>(width) * height * 4);
  }
  for (auto* guide : {motion.Get(), depth.Get()}) {
    if (native_ngx && guide == depth.Get()) {
      const std::vector<float> depth_pixels(static_cast<size_t>(render_width) * render_height, 0.5f);
      auto transfer = gpu.CreateTextureTransfer(guide);
      gpu.Transition(guide, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.UploadTexture(guide, transfer, reinterpret_cast<const uint8_t*>(depth_pixels.data()),
                        depth_pixels.size() * sizeof(float), render_width * sizeof(float));
      gpu.Transition(guide, D3D12_RESOURCE_STATE_COPY_DEST, NATIVE_INPUT_STATE);
    } else {
      gpu.Write(guide, 0);
      gpu.Transition(guide, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    }
  }
  submit();

  addon::bridge::OwnedParameters mock_parameters;
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
  const auto release_parameters = [](NVSDK_NGX_Parameter* value) { NVSDK_NGX_D3D12_DestroyParameters(value); };
  std::unique_ptr<NVSDK_NGX_Parameter, decltype(release_parameters)> sr_parameters(nullptr, release_parameters);
  if (real_sr) {
    NVSDK_NGX_Parameter* block = nullptr;
    Check(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&block)) && block != nullptr,
          "public SDK supplies the real handoff SR parameter block");
    sr_parameters.reset(block);
  }
  auto& parameters = (real_sr ? *sr_parameters : static_cast<NVSDK_NGX_Parameter&>(mock_parameters));
#else
  auto& parameters = mock_parameters;
#endif
  parameters.Set(NVSDK_NGX_Parameter_Color, color.Get());
  parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
  parameters.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
  parameters.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
  parameters.Set(NVSDK_NGX_Parameter_Width, render_width);
  parameters.Set(NVSDK_NGX_Parameter_Height, render_height);
  parameters.Set(NVSDK_NGX_Parameter_OutWidth, width);
  parameters.Set(NVSDK_NGX_Parameter_OutHeight, height);
  parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, render_width);
  parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, render_height);
  const auto flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
      | (native_ngx ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0)
      | (real_sr ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0);
  if (real_sr) {
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<int>(flags));
    parameters.Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(NVSDK_NGX_PerfQuality_Value_MaxQuality));
    parameters.Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    parameters.Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 1000.f / 60.f);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0u);
  } else {
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<uint32_t>(flags));
  }
  parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
  parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
  parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.f);
  parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f);

  struct SrObservation {
    Gpu* gpu;
    ID3D12Resource* color;
    bool native_ngx;
    bool real_sr;
    uint32_t calls = 0;
    uint32_t source_wrappers = 0;
    size_t nr_calls_at_entry = 0;
    bool fail_sr = false;
    bool consumed_replacement = false;
    Snapshot consumed_color;
    Snapshot sr_output;
    Snapshot outer_output;
  };
  SrObservation observation{.gpu = &gpu, .color = color.Get(), .native_ngx = native_ngx, .real_sr = real_sr};
  static SrObservation* active_sr = nullptr;
  restore.Value(&active_sr);
  active_sr = &observation;
  native::ngx_slot_real[0].create = reinterpret_cast<void*>(nr_edit_reuse_render_cases::CreateSr);
  native::ngx_slot_real[0].release = reinterpret_cast<void*>(+[](NVSDK_NGX_Handle*) -> NVSDK_NGX_Result {
    return NVSDK_NGX_Result_Success;
  });
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
  if (real_sr) {
    native::ngx_slot_real[0].create = reinterpret_cast<void*>(NVSDK_NGX_D3D12_CreateFeature);
    native::ngx_slot_real[0].release = reinterpret_cast<void*>(NVSDK_NGX_D3D12_ReleaseFeature);
  }
#endif
  native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(+[](ID3D12GraphicsCommandList* list,
      const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* block,
      PFN_NVSDK_NGX_ProgressCallback progress) -> NVSDK_NGX_Result {
    auto& observed = *active_sr;
    ++observed.calls;
    observed.source_wrappers = addon::evaluate_chain.game_wrappers;
    observed.nr_calls_at_entry = (observed.native_ngx ? native_runtime_evaluation_results.size() : model_calls);
    if (observed.fail_sr) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    auto* input = native::GetD3D12Resource(block, NVSDK_NGX_Parameter_Color);
    auto* output = native::GetD3D12Resource(block, NVSDK_NGX_Parameter_Output);
    observed.consumed_replacement = input != observed.color;
    const auto input_state = (observed.consumed_replacement ? native::kPreSrReadable : NATIVE_INPUT_STATE);
    if (observed.native_ngx && observed.consumed_replacement) {
      observed.consumed_color = Capture(*observed.gpu, input, input_state);
    }
    auto result = NVSDK_NGX_Result_Success;
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
    if (observed.real_sr) {
      result = NVSDK_NGX_D3D12_EvaluateFeature(list, handle, block, progress);
    } else
#endif
    {
      native::Transition(list, input, input_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      native::Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyResource(output, input);
      native::Transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      native::Transition(list, input, D3D12_RESOURCE_STATE_COPY_SOURCE, input_state);
    }
    if (observed.native_ngx && native::hook_point.load() != native::kHookPresent && NVSDK_NGX_SUCCEED(result)) {
      observed.sr_output = Capture(*observed.gpu, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    return result;
  });
  NVSDK_NGX_Handle* sr = nullptr;
  Check(NVSDK_NGX_SUCCEED(native::HookedCreateFeatureSlot<0>(gpu.list.Get(), native::kFeatureDlss, &parameters, &sr))
            && sr != nullptr, "production Create wrapper registers the continuing SR source");
  const auto source_generation = native::create_contracts.at(sr).source_generation;
  if (real_sr) {
    submit();
  }

  uint32_t present_id = session.context->last_id;
  uint64_t fg_on_samples = 0;
  uint64_t fg_off_samples = 0;
  nlohmann::json phases = nlohmann::json::array();
  nlohmann::json external_blocks = nlohmann::json::array();
  nlohmann::json nested_deliveries = nlohmann::json::array();
  nlohmann::json nested_sr_failure;
  const auto present = [&] {
    const auto app_frame = session.context->application_frame;
    native::RunPresentPath(&route.queue, &route);
    session.Guides(++present_id, sdk_depth, sdk_motion);
    session.Present(present_id);
    if (session.context->enabled) {
      ++fg_on_samples;
    } else {
      ++fg_off_samples;
    }
    Check(fg_held() && session.context->application_frame == app_frame + 1,
          "outer Present and SDK Proxy advance the same enabled XeFG context");
  };
  const auto source_eval = [&](uint8_t value, NVSDK_NGX_Result expected = NVSDK_NGX_Result_Success) {
    if (native_ngx) {
      for (uint32_t y = 0; y < render_height; ++y) {
        for (uint32_t x = 0; x < render_width; ++x) {
          const auto offset = (static_cast<size_t>(y) * render_width + x) * 4;
          const auto u = static_cast<float>((x + value) % render_width) / render_width;
          const auto v = static_cast<float>(y) / render_height;
          source_rgba[offset] = DirectX::PackedVector::XMConvertFloatToHalf(0.12f + 0.65f * u);
          source_rgba[offset + 1] = DirectX::PackedVector::XMConvertFloatToHalf(0.1f + 0.65f * v);
          source_rgba[offset + 2] = DirectX::PackedVector::XMConvertFloatToHalf(0.15f + 0.5f * (1.f - u * v));
          source_rgba[offset + 3] = static_cast<uint16_t>(0x3800 + ((x + value) & 3u) * 0x100u);
        }
      }
      gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.UploadTexture(color.Get(), color_transfer, reinterpret_cast<const uint8_t*>(source_rgba.data()),
                        source_rgba.size() * sizeof(uint16_t), render_width * 8);
      gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, NATIVE_INPUT_STATE);
      parameters.Set(NVSDK_NGX_Parameter_Reset, observation.calls == 0 ? 1 : 0);
    } else {
      gpu.Write(color.Get(), value);
      gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    }
    observation.consumed_color = {};
    observation.sr_output = {};
    observation.outer_output = {};
    gpu.list->ClearState(nullptr);
    const auto calls_before = observation.calls;
    Check(native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), sr, &parameters, nullptr) == expected
              && observation.calls == calls_before + 1 && native::create_contracts.at(sr).source_generation == source_generation
              && native::GetD3D12Resource(&parameters, NVSDK_NGX_Parameter_Color) == color.Get(),
          "SR Evaluate forwards its result, keeps its source generation and restores the original Color binding");
    gpu.Transition(color.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
  };
  const auto check_mock_pixels = [&](const Snapshot& snapshot, uint8_t alpha) {
    const auto rgba = snapshot.Pixels();
    Check(snapshot.pixel_bytes == 4 && rgba.size() == static_cast<size_t>(width) * height * 4,
          "handoff readback covers the complete output");
    for (size_t x = 0; x < rgba.size(); x += 4) {
      Check(rgba[x] == 0 && rgba[x + 1] == 0 && rgba[x + 2] == 0 && rgba[x + 3] == alpha,
            "all delivered model RGB and current source alpha reach the output");
    }
  };
  const auto check_fp16 = [](std::span<const uint8_t> rgba, std::span<const uint8_t> reference) {
    Check(!rgba.empty() && rgba.size() == reference.size() && rgba.size() % 8 == 0,
          "native inline full RGBA16F readback matches reference geometry");
    bool rgb_changed = false;
    bool rgba_valid = true;
    for (size_t pixel = 0; pixel < rgba.size(); pixel += 8) {
      std::array<uint16_t, 4> channels;
      std::memcpy(channels.data(), rgba.data() + pixel, 8);
      rgba_valid &= std::ranges::none_of(channels, [](uint16_t channel) { return (channel & 0x7c00u) == 0x7c00u; })
          && std::memcmp(rgba.data() + pixel + 6, reference.data() + pixel + 6, 2) == 0;
      rgb_changed |= std::memcmp(rgba.data() + pixel, reference.data() + pixel, 6) != 0;
    }
    Check(rgba_valid, "native inline output has finite RGBA and exact current reference alpha");
    return rgb_changed;
  };
  const auto pending_count = [] {
    return addon::nr_decline_counts[static_cast<size_t>(addon::NrDeclineReason::kRuntimeSwitchPending)].load();
  };

  const auto run_xefg = [&] {
    if (native::hook_point.load() != native::kHookPresent) {
      native::kSetHookPoint.Commit(static_cast<float>(native::kHookPresent));
    }
    uint32_t resumed = 0;
    nlohmann::json frames = nlohmann::json::array();
    const auto& telemetry = native::stage_telemetry[static_cast<size_t>(Stage::PRESENT)];
    for (uint32_t frame = 0; frame < native::kContractDebounceFrames + 8 && resumed < 2; ++frame) {
      const auto value = static_cast<uint8_t>(128 + frame);
      source_eval(value);
      if (native_ngx) {
        for (uint32_t y = 0; y < height; ++y) {
          for (uint32_t x = 0; x < width; ++x) {
            const auto pixel = (static_cast<size_t>(y) * width + x) * 4;
            back_rgba[pixel] = static_cast<uint8_t>(32 + ((x + value) & 127u));
            back_rgba[pixel + 1] = static_cast<uint8_t>(32 + (y & 127u));
            back_rgba[pixel + 2] = static_cast<uint8_t>(32 + ((x ^ y) & 127u));
            back_rgba[pixel + 3] = static_cast<uint8_t>(value + (x & 3u));
          }
        }
        gpu.Transition(back, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.UploadTexture(back, back_transfer, back_rgba.data(), back_rgba.size(), width * 4);
        gpu.Transition(back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
      } else {
        gpu.Write(back, value);
      }
      submit();
      const auto calls_before = nr_calls();
      const auto service_before = xefg::SubmittedFrames();
      const auto accepted_before = telemetry.accepted;
      const auto started_ns = native::SteadyNowNs();
      present();
      const auto& service_stream = native::fg_service::streams.at(key);
      wait_fence(service_stream.fence, service_stream.submitted);
      if (native_ngx) {
        sample_memory("Present_attempt");
        std::cout << "NATIVE_XEFG_HANDOFF_ATTEMPT " << nlohmann::json{
            {"attempt", frame + 1}, {"application_frame", session.context->application_frame},
            {"result", static_cast<unsigned>(xefg::CurrentResult())}, {"nr_evaluations", nr_calls() - calls_before},
            {"submissions", xefg::SubmittedFrames() - service_before},
            {"cpu_and_completion_ms", (native::SteadyNowNs() - started_ns) / 1e6},
            {"source_evaluations", observation.calls}, {"old_work_complete", native::TeardownProof() == Work::kComplete}
        }.dump() << std::endl;
        Check(frame != 0 || xefg::CurrentResult() == fg::Result::SUBMITTED,
              "first legal native Present frame delivers after independent model preparation");
      }
      if (xefg::CurrentResult() != fg::Result::SUBMITTED) {
        Check(xefg::DisplayResult() != fg::Result::SUBMITTED && xefg::SubmittedFrames() == service_before,
              "XeFG restoration warmup does not report a submitted frame");
        resumed = 0;
        frames.clear();
        gpu.uploads.clear();
        continue;
      }
      Check(xefg::DisplayResult() == fg::Result::SUBMITTED && xefg::SubmittedFrames() == service_before + 1
                && nr_calls() == calls_before + 1 && previous_nr_succeeded() && same_service_runtime(),
            "XeFG Present retains the validated runtime and submits one actual NR pass again");
      const auto readback = Capture(gpu, back, D3D12_RESOURCE_STATE_COMMON);
      Snapshot private_color;
      if (native_ngx) {
        const auto slot = std::ranges::find_if(service_stream.slots, [&](const auto& slot) {
          return slot.fence_value == service_stream.submitted && slot.color[0] != nullptr;
        });
        Check(slot != service_stream.slots.end(), "native XeFG has a completed private color for the current submission");
        private_color = Capture(gpu, slot->color[0], NATIVE_INPUT_STATE);
      }
      submit();
      if (native_ngx) {
        const auto sdk_rgba = readback.Pixels();
        Check(sdk_rgba == private_color.Pixels() && sdk_rgba.size() == back_rgba.size() && sdk_rgba != back_rgba,
              "native XeFG delivers the edited private color exactly to the SDK backbuffer");
        bool alpha_matches = true;
        for (size_t i = 3; i < sdk_rgba.size(); i += 4) {
          alpha_matches &= sdk_rgba[i] == back_rgba[i];
        }
        const auto readback_completed = gpu.fence->GetCompletedValue();
        Check(alpha_matches && telemetry.accepted == accepted_before + 1
                  && telemetry.last_evaluate.api_returned && NVSDK_NGX_SUCCEED(telemetry.last_evaluate.result)
                  && readback_completed >= gpu.serial && readback_completed != UINT64_MAX
                  && SUCCEEDED(gpu.device->GetDeviceRemovedReason()),
              "native XeFG preserves alpha and has an accepted Feature 18 call on a healthy device");
        frames.push_back({{"application_frame", session.context->application_frame},
            {"sr_evaluate_sequence", observation.calls}, {"accepted_evaluate_sequence", telemetry.last_evaluate.sequence},
            {"delivery_sequence", xefg::SubmittedFrames()}, {"readback_pixels", sdk_rgba.size() / 4},
            {"delivery_complete", true}, {"readback_complete", true}, {"delivery_fence_value", service_stream.submitted},
            {"readback_fence_value", gpu.serial}, {"readback_fence_completed", readback_completed},
            {"gpu_fence_healthy", true}, {"sdk_fg_enabled", session.context->enabled}});
      } else {
        check_mock_pixels(readback, value);
      }
      ++resumed;
    }
    Check(resumed == 2 && fg_held(), "the same enabled XeFG context sustains Present NR service");
    if (native_ngx) {
      phases.push_back({{"stage", "Present"}, {"provider", "isolated"}, {"frames", std::move(frames)}});
    }
  };
  if (native_ngx) {
    run_xefg();
  }

  {
    funnel_accounting_tests::Restore owner_restore;
    owner_restore.Atomic(&native::nr_adaptive_enabled);
    owner_restore.Atomic(&native::nr_adaptive_min_scale);
    owner_restore.Atomic(&native::nr_adaptive_selected_levels);
    owner_restore.Atomic(&native::gpu_timers_enabled);
    owner_restore.Value(&adapter::proxies[0].real[0]);
    native::nr_adaptive_enabled = true;
    native::nr_adaptive_min_scale = 1.f;
    native::nr_adaptive_selected_levels = 0;
    native::gpu_timers_enabled = true;
    Session exiting(&gpu);
    exiting.context->ui_composition = false;
    exiting.extent_left = exiting.extent_top = 0;
    exiting.extent_width = width;
    exiting.extent_height = height;
    const auto exiting_key = std::pair{false, reinterpret_cast<uintptr_t>(exiting.handle)};
    ComPtr<ID3D12Resource> exiting_back;
    Check(SUCCEEDED(exiting.proxy->GetBuffer(exiting.proxy->GetCurrentBackBufferIndex(),
                                             IID_PPV_ARGS(&exiting_back))), "second owner actual DXGI backbuffer");
    gpu.Write(exiting_back.Get(), 96);
    gpu.Write(back, 160);
    submit();
    uint32_t exiting_id = 30000;
    bool warm = false;
    for (uint32_t frame = 0; frame < native::kContractDebounceFrames + 8 && !warm; ++frame) {
      exiting.Guides(++exiting_id, sdk_depth, sdk_motion);
      exiting.Present(exiting_id);
      const auto& stream = native::fg_service::streams.at(exiting_key);
      wait_fence(stream.fence, stream.submitted);
      warm = xefg::CurrentResult() == fg::Result::SUBMITTED;
    }
    Check(warm, "the second SDK owner matures on its own production private pool");
    warm = false;
    for (uint32_t frame = 0; frame < native::kContractDebounceFrames + 8 && !warm; ++frame) {
      session.Guides(++present_id, sdk_depth, sdk_motion);
      session.Present(present_id);
      const auto& stream = native::fg_service::streams.at(key);
      wait_fence(stream.fence, stream.submitted);
      warm = xefg::CurrentResult() == fg::Result::SUBMITTED;
    }
    Check(warm, "both SDK owners are active on the same warmed adaptive configuration");
    gpu.Write(back, 160);
    submit();
    const auto* const surviving_role = native::fg_service::streams.at(key).roles[0].Handle();
    const auto* const surviving_model = native::features.at(surviving_role).slots[0].handle;
    const auto* const surviving_parameters = native::features.at(surviving_role).slots[0].parameters;
    std::function<void(IDXGISwapChain*)> before_sdk_return;
    static std::function<void(IDXGISwapChain*)>* active_owner_probe = nullptr;
    owner_restore.Value(&active_owner_probe);
    active_owner_probe = &before_sdk_return;
    adapter::proxies[0].real[0] = reinterpret_cast<void*>(+[](IDXGISwapChain* proxy, UINT interval, UINT flags) -> HRESULT {
      const auto result = FakePresent(proxy, interval, flags);
      if (active_owner_probe != nullptr && *active_owner_probe) (*active_owner_probe)(proxy);
      return result;
    });
    ComPtr<ID3D12Fence> gate;
    Check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "owner-exit GPU gate");
    const auto unblock = [](ID3D12Fence* fence) { fence->Signal(1); };
    std::unique_ptr<ID3D12Fence, decltype(unblock)> release_gate(gate.Get(), unblock);
    ComPtr<ID3D12Fence> old_fence;
    uint64_t old_fence_value = 0;
    std::shared_ptr<submission::RecordingStatus> old_recording;
    std::shared_ptr<addon::adaptive_resolution::DeliveryReceipt> old_delivery;
    const NVSDK_NGX_Handle* old_model = nullptr;
    const auto old_epoch = exiting.context->epoch;
    before_sdk_return = [&](IDXGISwapChain* proxy) {
      if (proxy != exiting.proxy.Get()) return;
      const auto& stream = native::fg_service::streams.at(exiting_key);
      const auto slot = std::ranges::find_if(stream.slots, [&](const auto& candidate) {
        return candidate.list != nullptr && candidate.fence_value == stream.submitted;
      });
      Check(slot != stream.slots.end() && stream.awaiting_xefg_delivery,
            "the real proxy wrapper reaches SDK return with a submitted NR group awaiting EndPresent");
      old_recording = submission::ObserveRecording(slot->list);
      old_delivery = stream.pending_xefg_delivery.frame.delivery;
      old_fence = stream.fence;
      old_fence_value = stream.submitted;
      old_model = native::features.at(stream.roles[0].Handle()).slots[0].handle;
      const auto releases = std::count(model_released_handles.begin(), model_released_handles.end(), old_model);
      Check(old_delivery != nullptr && *old_delivery == addon::adaptive_resolution::DeliveryReceipt::PENDING
                && old_fence->GetCompletedValue() < old_fence_value && old_recording != nullptr
                && old_recording->closed.load() && old_recording->submitted.load(),
            "private replay closes at Execute while GPU submission and SDK delivery are pending");
      gpu.submission_pending = true;
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "owner-exit fixture completion marker");
      Check(Hooks::Enable(exiting.handle, 0) == 0, "public SDK Disable exits only the second owner");
      Check(old_recording->closed.load() && *old_delivery == addon::adaptive_resolution::DeliveryReceipt::PENDING
                && !submission::ResourceReleasable(old_model)
                && std::count(model_released_handles.begin(), model_released_handles.end(), old_model) == releases
                && old_fence->GetCompletedValue() < old_fence_value && fg_held()
                && native::fg_service::streams.contains(key) && native::features.contains(surviving_role)
                && native::features.at(surviving_role).slots[0].handle == surviving_model
                && native::features.at(surviving_role).slots[0].parameters == surviving_parameters,
            "owner exit closes replay, retains pending delivery and GPU operands, and preserves the other active owner");
    };
    Check(SUCCEEDED(gpu.queue->Wait(gate.Get(), 1)), "hold the actual private submission behind the GPU gate");
    exiting.Guides(++exiting_id, sdk_depth, sdk_motion);
    exiting.Present(exiting_id);
    Check(old_delivery != nullptr && *old_delivery != addon::adaptive_resolution::DeliveryReceipt::PENDING
              && old_recording->closed.load() && exiting.context->presents_in_flight == 0
              && exiting.context->epoch != old_epoch && exiting.context->last_submission_frame == 0
              && !submission::ResourceReleasable(old_model),
          "late SDK return settles only its old delivery and keeps GPU operands alive beyond EndPresent");
    before_sdk_return = {};
    const auto surviving_calls = nr_calls();
    session.Guides(++present_id, sdk_depth, sdk_motion);
    session.Present(present_id);
    Check(xefg::CurrentResult() == fg::Result::SUBMITTED && nr_calls() == surviving_calls + 1 && fg_held(),
          "the other SDK owner continues actual NR while the exited owner's GPU submission remains in flight");
    Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "cover both owners' queued GPU work");
    Check(SUCCEEDED(gate->Signal(1)), "release the owner-exit GPU gate");
    release_gate.release();
    wait_fence(old_fence.Get(), old_fence_value);
    wait_fence(gpu.fence.Get(), gpu.serial);
    gpu.submission_pending = false;
    submission::PruneCompletedGenerations();
    Check(submission::ResourceReleasable(old_model), "the old private operands become releasable at actual GPU completion");
    const auto private_output = [&](const auto& stream_key) {
      const auto& stream = native::fg_service::streams.at(stream_key);
      const auto slot = std::ranges::find_if(stream.slots, [&](const auto& candidate) {
        return candidate.fence_value == stream.submitted && candidate.color[0] != nullptr;
      });
      Check(slot != stream.slots.end(), "owner delivery retains the private product matching its actual submission");
      return Capture(gpu, slot->color[0], NATIVE_INPUT_STATE);
    };
    const auto surviving_output = Capture(gpu, back, D3D12_RESOURCE_STATE_COMMON);
    const auto surviving_private_output = private_output(key);
    submit();
    Check(surviving_output.Pixels() == surviving_private_output.Pixels(),
          "the surviving owner delivers its current private product to the whole SDK backbuffer");
    if (native_ngx) {
      const auto pixels = surviving_output.Pixels();
      Check(pixels.size() == static_cast<size_t>(width) * height * 4,
            "the surviving native owner delivers a full backbuffer");
      for (size_t pixel = 3; pixel < pixels.size(); pixel += 4) {
        Check(pixels[pixel] == 160, "the surviving native owner retains current full-frame alpha");
      }
    } else {
      check_mock_pixels(surviving_output, 160);
    }
    std::shared_ptr<addon::adaptive_resolution::DeliveryReceipt> returned_delivery;
    uint64_t returned_epoch = 0;
    uint64_t returned_frame = 0;
    addon::output_framerate::OutputIdentity returned_output_identity;
    bool returned_awaiting = false;
    bool returned_group_complete = false;
    bool returned_delivery_complete = false;
    before_sdk_return = [&](IDXGISwapChain* proxy) {
      if (proxy != exiting.proxy.Get()) return;
      const auto& stream = native::fg_service::streams.at(exiting_key);
      returned_awaiting = stream.awaiting_xefg_delivery;
      if (!returned_awaiting) return;
      returned_epoch = stream.epoch;
      returned_frame = stream.application_frame;
      returned_output_identity = stream.xefg_delivery_output;
      returned_delivery = stream.pending_xefg_delivery.frame.delivery;
      returned_delivery_complete = stream.pending_xefg_delivery.complete;
      const auto slot = std::ranges::find_if(stream.slots, [&](const auto& candidate) {
        return candidate.list != nullptr && candidate.fence_value == stream.submitted;
      });
      returned_group_complete = slot != stream.slots.end() && slot->base_frame_group.complete;
    };
    Check(Hooks::Enable(exiting.handle, 1) == 0 && exiting.context->epoch != old_epoch,
          "public SDK re-enable starts a fresh owner epoch");
    warm = false;
    size_t returned_nr_evals = 0;
    for (uint32_t frame = 0; frame < native::kContractDebounceFrames + 8 && !warm; ++frame) {
      returned_delivery.reset();
      returned_awaiting = returned_group_complete = returned_delivery_complete = false;
      const auto calls_before = nr_calls();
      exiting.Guides(++exiting_id, sdk_depth, sdk_motion);
      exiting.Present(exiting_id);
      const auto& stream = native::fg_service::streams.at(exiting_key);
      wait_fence(stream.fence, stream.submitted);
      returned_nr_evals = nr_calls() - calls_before;
      // A cold compatibility prefix can be SUBMITTED before its model group
      // is complete. Stop on that independent group proof, never on receipt success.
      warm = xefg::CurrentResult() == fg::Result::SUBMITTED && returned_group_complete;
      if (warm) {
        Check(returned_nr_evals == 1 && previous_nr_succeeded(),
              "the returned owner's first complete group contains one actual successful NR Evaluate");
      }
    }
    const auto& returned_stream = native::fg_service::streams.at(exiting_key);
    const bool matched_delivery = warm && returned_awaiting && returned_delivery_complete
        && !returned_stream.awaiting_xefg_delivery && returned_stream.epoch == returned_epoch
        && returned_stream.application_frame == returned_frame && returned_stream.xefg_delivery_output == returned_output_identity
        && exiting.context->epoch == returned_epoch && exiting.context->application_frame == returned_frame
        && exiting.context->output_identity == returned_output_identity && exiting.context->last_submission_frame == returned_frame;
    // Adaptive timing receipts are optional; SDK consumption is proved above
    // by the matching pending transition and the real EndPresent outcome.
    const bool adaptive_delivered = returned_delivery == nullptr || (returned_delivery != old_delivery
        && *returned_delivery == addon::adaptive_resolution::DeliveryReceipt::DELIVERED);
    if (!matched_delivery || !adaptive_delivered) {
      using Delivery = addon::adaptive_resolution::DeliveryReceipt;
      std::cerr << "XEFG_OWNER_REENTRY epoch=" << exiting.context->epoch
                << " application_frame=" << exiting.context->application_frame
                << " result=" << static_cast<unsigned>(xefg::CurrentResult())
                << " group_complete=" << returned_group_complete << " nr_evals=" << returned_nr_evals
                << " delivery_complete=" << returned_delivery_complete
                << " awaiting_before=" << returned_awaiting << " awaiting_after=" << returned_stream.awaiting_xefg_delivery
                << " sdk_last_submission=" << exiting.context->last_submission_frame
                << " fresh_receipt=" << (returned_delivery != nullptr && returned_delivery != old_delivery)
                << " receipt=" << (returned_delivery == nullptr ? "NONE" : *returned_delivery == Delivery::DELIVERED
                    ? "DELIVERED" : *returned_delivery == Delivery::REJECTED ? "REJECTED" : "PENDING") << '\n';
    }
    Check(matched_delivery,
          "the returned owner receives its own matched EndPresent delivery instead of reusing the old pending result");
    Check(adaptive_delivered, "an optional adaptive receipt must belong to the returned owner and be DELIVERED");
    const auto returned_output = Capture(gpu, exiting_back.Get(), D3D12_RESOURCE_STATE_COMMON);
    const auto returned_private_output = private_output(exiting_key);
    submit();
    Check(returned_output.Pixels() == returned_private_output.Pixels(),
          "the returned owner delivers its fresh private product to the whole SDK backbuffer");
    if (native_ngx) {
      const auto pixels = returned_output.Pixels();
      Check(pixels.size() == static_cast<size_t>(width) * height * 4,
            "the returned native owner delivers a full backbuffer");
      for (size_t pixel = 3; pixel < pixels.size(); pixel += 4) {
        Check(pixels[pixel] == 96, "the returned native owner delivers current full-frame alpha");
      }
    } else {
      check_mock_pixels(returned_output, 96);
    }
    before_sdk_return = {};
    Check(Hooks::Enable(exiting.handle, 0) == 0 && fg_held(), "final SDK Disable leaves the surviving owner active");
    exiting.CloseSdk();
    Check(fg_held(), "retiring the closed SDK context leaves the surviving owner active");
    std::cout << "XEFG_OWNER_ISOLATION exited=A active=B active_delivery=readback returned_delivery=readback\n";
    std::cout << "XEFG_OWNER_PENDING replayable=closed gpu_in_flight=retained old_EndPresent=settled reentry=fresh\n";
  }

  for (const auto stage : {Stage::RENDER, Stage::UPSCALED}) {
    const bool render = stage == Stage::RENDER;
    const char* stage_name = (render ? "Render" : "Upscaled");
    {
      funnel_accounting_tests::Restore off_restore;
      off_restore.Atomic(&native::disable_release_mode);
      auto& stream = native::fg_service::streams.at(key);
      wait_fence(stream.fence, stream.submitted);
      const auto slot = std::ranges::find_if(stream.slots, [&](const auto& slot) {
        return slot.list != nullptr && slot.fence_value == stream.submitted;
      });
      Check(slot != stream.slots.end(), "Off-policy fixture has a completed owned XeFG recording");
      const ComPtr<ID3D12GraphicsCommandList> retained_list = slot->list;
      const ComPtr<ID3D12CommandAllocator> retained_allocator = slot->allocator;
      const auto* const role = stream.roles[0].Handle();
      const auto* const model = native::features.at(role).slots[0].handle;
      native::disable_release_mode = 0;
      native::kSetEnabled.Commit(0.f);
      Check(!native::enabled.load() && fg_held() && submission::ResourceReleasable(retained_list.Get())
                && same_service_runtime() && native::features.contains(role)
                && native::features.at(role).slots[0].handle == model
                && native::fg_service::streams.contains(key)
                && std::ranges::any_of(native::fg_service::streams.at(key).slots,
                     [&](const auto& retained) {
                       return retained.list == retained_list.Get() && retained.allocator == retained_allocator.Get();
                     }),
            "NR Off policy zero retains completed, closed owned objects without another callback");
      std::cout << "XEFG_INLINE_OFF_HOLD stage=" << stage_name
                << " policy=0 objects=retained replayable=closed\n";
      native::kSetEnabled.Commit(1.f);
      bool resumed = false;
      for (uint32_t frame = 0; frame < native::kContractDebounceFrames + 8 && !resumed; ++frame) {
        session.Guides(++present_id, sdk_depth, sdk_motion);
        session.Present(present_id);
        const auto& resumed_stream = native::fg_service::streams.at(key);
        wait_fence(resumed_stream.fence, resumed_stream.submitted);
        resumed = xefg::CurrentResult() == fg::Result::SUBMITTED;
      }
      Check(resumed && fg_held(), "public NR re-enable resumes the same SDK context before the hook-exit test");
      Check(std::ranges::any_of(native::fg_service::streams.at(key).slots, [&](const auto& retained) {
        return retained.list == retained_list.Get() && retained.allocator == retained_allocator.Get();
      }), "NR re-enable rearms the retained private pool without discarding its list and allocator");
    }
    const auto& old_stream = native::fg_service::streams.at(key);
    const auto old_slot = std::ranges::find_if(old_stream.slots, [&](const auto& slot) {
      return slot.list != nullptr && slot.fence_value == old_stream.submitted;
    });
    Check(old_slot != old_stream.slots.end() && native::TeardownProof() == Work::kComplete,
          "last submitted private XeFG recording already completed and closed before the hook change");
    const std::array<ComPtr<IUnknown>, 4> old_owned_objects{
        old_slot->color[0], old_slot->color[1], old_slot->list, old_slot->allocator};
    const auto old_submissions = xefg::SubmittedFrames();
    native::kSetHookPoint.Commit(static_cast<float>(render ? native::kHookRender : native::kHookUpscaled));
    Check(fg_held() && !xefg::OwnsStream() && submission::ResourceReleasable(old_owned_objects[2].Get())
              && xefg::SubmittedFrames() == old_submissions,
          "the committed inline hook retires the old owner before any ordinary Present or old Service callback");
    std::cout << "XEFG_OWNER_EXIT stage=" << stage_name
              << " trigger=config_commit old_callbacks=0 replayable=closed fg=held_on\n";
    const auto& telemetry = native::stage_telemetry[static_cast<size_t>(stage)];
    const auto model_before_switch = nr_calls();
    const auto pending_before_switch = pending_count();
    const auto service_before_switch = xefg::SubmittedFrames();
    uint32_t delivered = 0;
    uint32_t source_frames = 0;
    nlohmann::json frames = nlohmann::json::array();
    for (; source_frames < native::kContractDebounceFrames + 8 && delivered < 2; ++source_frames) {
      Check(!xefg::OwnsStream() && xefg::CurrentResult() == fg::Result::NOT_SERVED
                && xefg::DisplayResult() == fg::Result::NOT_SERVED && xefg::SubmittedFrames() == service_before_switch,
            "XeFG stays enabled without claiming a live NR Present service in inline mode");
      const auto calls_before = nr_calls();
      const auto accepted_before = telemetry.accepted;
      const auto skipped_before = telemetry.skipped;
      const auto value = static_cast<uint8_t>(64 + source_frames);
      source_eval(value);
      const bool complete = nr_calls() == calls_before + 1 && previous_nr_succeeded()
          && telemetry.accepted == accepted_before + 1 && telemetry.skipped == skipped_before;
      if (!complete) {
        submit();
        delivered = 0;
        frames.clear();
        continue;
      }
      Check(!native::direct_api.private_instance && submission::ResourceReleasable(old_owned_objects[2].Get())
                && (!native_ngx || native::parameter_runtime_via_core.load())
                && telemetry.last_evaluate.api_returned && NVSDK_NGX_SUCCEED(telemetry.last_evaluate.result)
                && telemetry.last_evaluate.planned == stage && telemetry.last_evaluate.source_generation == source_generation
                && observation.consumed_replacement == render && observation.nr_calls_at_entry == calls_before + (render ? 1 : 0),
            "inline NR is evaluated on the ordinary provider and delivered at the selected side of SR");
      if (delivered == 0) {
        funnel_accounting_tests::Restore serial_restore;
        serial_restore.Atomic(&addon::ngx_serial::armed);
        serial_restore.Atomic(&addon::ngx_serial::retirement_armed);
        addon::ngx_serial::armed = false;
        addon::ngx_serial::retirement_armed = false;
        Check(addon::ngx_serial::depth == 0 && addon::ngx_serial::forced_turns.load() == 0,
              "empty-pool callback starts outside another NGX turn");
        const auto waits = addon::ngx_serial::present_busy.load();
        {
          const addon::ngx_serial::Call game_call;
          Check(addon::ngx_serial::unlocked_in_flight.load() != 0,
                "an ordinary game-call reservation blocks unnecessary retirement turns");
          xefg::ReconcileNrService();
        }
        Check(addon::ngx_serial::present_busy.load() == waits,
              "inline callback with no owned XeFG pool must not wait for an NGX retirement turn");
        for (const auto& object : old_owned_objects) {
          if (object == nullptr) continue;
          Check(std::ranges::none_of(native::present_retired, [&](const auto& retired) { return retired.key == object.Get(); }),
                "completed old private pool objects drain during inline callbacks without ordinary Present");
        }
        std::cout << "XEFG_INLINE_RECLAIM stage=" << stage_name << " completed_owned=drained\n";
        std::cout << "XEFG_INLINE_EMPTY_POOL stage=" << stage_name << " serial_wait_delta=0\n";
      }
      const auto readback = Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      const auto recording = submission::ObserveRecording(gpu.list.Get());
      const auto application_frame = session.context->application_frame;
      const bool park_game_recording = !render && delivered == 1;
      if (park_game_recording) {
        Check(SUCCEEDED(gpu.list->Close()), "park the actual inline game recording before its first submission");
        const auto provider = native::direct_api.module;
        const auto& slot = native::features.at(sr).slots[0];
        const auto* model = slot.handle;
        const auto* block = slot.parameters;
        const auto model_releases = (native_ngx ? 0 : std::count(model_released_handles.begin(), model_released_handles.end(), model));
        const auto game_recording = submission::RecordingIdentity(gpu.list.Get());
        native::kSetHookPoint.Commit(static_cast<float>(native::kHookPresent));
        const auto expect_blocked = [&](Work proof) {
          const auto declines = pending_count();
          Check(native::TeardownProof() == proof, "game recording supplies the expected lifetime obstruction");
          present();
          // Commit recreates active slots; the old game's model/parameters
          // must remain together in retirement until its real recording closes.
          const bool retired_pair = model != nullptr && block != nullptr
              && std::ranges::any_of(native::retired_nr_features, [&](const auto& retired) {
                return retired.handle == model && retired.parameters == block && retired.tracker_proof;
              });
          const bool model_retained = !submission::ResourceReleasable(model)
              && (native_ngx || std::count(model_released_handles.begin(), model_released_handles.end(), model) == model_releases);
          const bool recording_retained = !recording->closed.load()
              && submission::RecordingIdentity(gpu.list.Get()) == game_recording;
          const bool provider_retained = !native::direct_api.private_instance && native::direct_api.module == provider;
          if (!retired_pair || !model_retained || !recording_retained || !provider_retained
              || pending_count() != declines + 1 || nr_calls() != calls_before + 1
              || xefg::SubmittedFrames() != service_before_switch || xefg::CurrentResult() != fg::Result::NOT_SERVED
              || xefg::DisplayResult() != fg::Result::NOT_SERVED) {
            std::cerr << "XEFG_EXTERNAL_BLOCK proof=" << (proof == Work::kPending ? "Pending" : "Replayable")
                      << " retired_pair=" << retired_pair << " model_retained=" << model_retained
                      << " recording_retained=" << recording_retained << " provider_retained=" << provider_retained
                      << " pending_delta=" << pending_count() - declines << " nr_delta=" << nr_calls() - calls_before
                      << " submitted_delta=" << xefg::SubmittedFrames() - service_before_switch
                      << " result=" << static_cast<unsigned>(xefg::CurrentResult())
                      << " display=" << static_cast<unsigned>(xefg::DisplayResult()) << '\n';
          }
          Check(recording_retained && provider_retained && retired_pair && model_retained,
                "the exact pending or replayable game recording keeps its original model, parameters and runtime");
          if (legacy_private) {
            Check(xefg::CurrentResult() == fg::Result::NOT_SERVED && xefg::DisplayResult() == fg::Result::NOT_SERVED
                      && xefg::SubmittedFrames() == service_before_switch && pending_count() == declines + 1
                      && nr_calls() == calls_before + 1,
                  "the legacy fixture still distinguishes a blocked provider change");
          } else {
            Check(pending_count() == declines && xefg::CurrentResult() != fg::Result::FAILED,
                  "a same-runtime SDK call does not manufacture a provider-switch wait");
          }
          if (native_ngx) {
            external_blocks.push_back({{"proof", proof == Work::kPending ? "Pending" : "Replayable"},
                {"application_frame", session.context->application_frame}, {"sdk_fg_enabled", session.context->enabled},
                {"service_result", "NOT_SERVED"}, {"display_result", "NOT_SERVED"},
                {"game_recording_generation", recording->generation}, {"game_recording_closed", recording->closed.load()},
                {"ordinary_provider_retained", true}, {"runtime_switch_pending_delta", pending_count() - declines}});
            if (proof == Work::kReplayable) {
              external_blocks.back()["gpu_fence_value"] = gpu.fence->GetCompletedValue();
            }
          }
        };
        expect_blocked(Work::kPending);
        ID3D12CommandList* lists[]{gpu.list.Get()};
        gpu.queue->ExecuteCommandLists(1, lists);
        gpu.submission_pending = true;
        Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "signal the held game recording");
        wait_fence(gpu.fence.Get(), gpu.serial);
        gpu.submission_pending = false;
        expect_blocked(Work::kReplayable);
        Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
              "the game resets its own completed recording");
        native::PruneCompletedSubmissionUses();
      } else {
        submit();
      }
      Check(recording->submitted.load() && recording->closed.load() && recording->completed_healthy,
            "the inline delivery recording has healthy GPU completion");
      if (native_ngx) {
        const auto rgba = readback.Pixels();
        const auto sr_output = observation.sr_output.Pixels();
        Check(rgba.size() == static_cast<size_t>(width) * height * 8,
              "native inline reads the entire SR output");
        const bool output_edited = check_fp16(rgba, sr_output);
        Check(output_edited != render, "native Upscaled rewrites SR RGB and Render preserves the SR consumer output");
        if (render) {
          const auto stand_in = observation.consumed_color.Pixels();
          Check(check_fp16(stand_in, {reinterpret_cast<const uint8_t*>(source_rgba.data()),
                                   source_rgba.size() * sizeof(uint16_t)}),
                "native Render delivers edited full-size Color to SR with current game alpha");
        }
        const auto readback_completed = gpu.fence->GetCompletedValue();
        Check(readback_completed >= gpu.serial && readback_completed != UINT64_MAX
                  && SUCCEEDED(gpu.device->GetDeviceRemovedReason()),
              "native inline delivery completed on a healthy device");
        frames.push_back({{"application_frame", application_frame}, {"sr_evaluate_sequence", observation.calls},
            {"accepted_evaluate_sequence", telemetry.last_evaluate.sequence}, {"delivery_sequence", gpu.serial},
            {"game_recording_generation", recording->generation}, {"readback_pixels", rgba.size() / 8},
            {"delivery_complete", true}, {"readback_complete", true}, {"delivery_fence_value", gpu.serial},
            {"readback_fence_value", gpu.serial}, {"readback_fence_completed", readback_completed},
            {"gpu_fence_healthy", recording->completed_healthy}, {"sdk_fg_enabled", session.context->enabled}});
        if (render) {
          frames.back()["sr_input_readback_pixels"] = source_rgba.size() / 4;
        }
      } else {
        check_mock_pixels(readback, value);
      }
      ++delivered;
    }
    std::cout << "XEFG_INLINE_OBS stage=" << stage_name << " sr_frames=" << source_frames
              << " nr_evals=" << nr_calls() - model_before_switch << " delivered=" << delivered
              << " runtime_switch_pending=" << pending_count() - pending_before_switch << std::endl;
    Check(delivered == 2, "XeFG-held hook change must sustain two complete actual inline NR deliveries");

    if (native_ngx) {
      phases.push_back({{"stage", stage_name}, {"provider", "Core"}, {"frames", std::move(frames)}});
    }
    if (!render) {
      namespace timers = addon::gpu_timers;
      using Delivery = addon::adaptive_resolution::DeliveryReceipt;
      funnel_accounting_tests::Restore nested_restore;
      nested_restore.Value(&native::ngx_slot_real[0]);
      nested_restore.Value(&native::ngx_slot_real[1]);
      nested_restore.Value(&native::adaptive_configuration_settings);
      nested_restore.Value(&native::direct_runtime_requirements);
      nested_restore.Value(&native::ngx_loader_staging.nr_requirements);
      nested_restore.Atomic(&native::ngx_loader_published);
      nested_restore.Value(&timers::comparison_settings);
      nested_restore.Atomic(&native::nr_adaptive_enabled);
      nested_restore.Atomic(&native::nr_adaptive_min_scale);
      nested_restore.Atomic(&native::nr_adaptive_selected_levels);
      nested_restore.Atomic(&native::nr_adaptive_editing);
      nested_restore.Atomic(&native::nr_adaptive_limited_residency);
      nested_restore.Atomic(&native::adaptive_feature_mode);
      nested_restore.Atomic(&native::gpu_timers_enabled);
      nested_restore.Atomic(&native::cost_meter_enabled);
      nested_restore.Atomic(&native::preset);
      native::hook_point = native::kHookUpscaled;
      native::nr_adaptive_enabled = true;
      native::nr_adaptive_min_scale = 1.f;
      native::nr_adaptive_selected_levels = 0;
      native::nr_adaptive_editing = false;
      native::nr_adaptive_limited_residency = false;
      native::adaptive_feature_mode = native::AdaptiveFeatureMode::OFF;
      native::gpu_timers_enabled = true;
      native::cost_meter_enabled = false;
      native::preset = 2;
      if (!native_ngx) {
        native::direct_runtime_requirements.dual_models = true;
        native::ngx_loader_staging.nr_requirements.dual_models = true;
        native::PublishNgxLoaderSymbols();
      }
      // Earlier handoff frames used runtime-default slots. Apply the new
      // model contract before warming this independently selected B case.
      native::RecreateFeatures();
      native::adaptive_configuration_settings = native::comparison_ui::CaptureAdaptiveSettings;
      timers::comparison_settings = native::comparison_ui::CaptureSettings;
      namespace rate = addon::output_framerate;
      const auto release_output = [](reshade::api::swapchain* output) {
        const addon::RuntimeLock lock(native::runtime_mutex);
        native::RetirePresentStream(output);
        ObserveOutputDestroy(output, false);
      };
      std::unique_ptr<reshade::api::swapchain, decltype(release_output)> registered_route(nullptr, release_output);
      rate::OutputIdentity output_owner;
      d3d12_test::TextureTransfer delivery_readback;
      if (!native_ngx) {
        ObserveOutputInit(&route, false);
        registered_route.reset(&route);
        output_owner = rate::MatchPresentOutput(reinterpret_cast<uintptr_t>(&route), reinterpret_cast<uintptr_t>(back));
        Check(static_cast<bool>(output_owner), "production swapchain registration identifies the actual nested SR delivery backbuffer");
        delivery_readback = gpu.CreateTextureTransfer(output.Get());
      }
      const auto source_output = [&] {
        return rate::FindSourceOutput({.handle = reinterpret_cast<uintptr_t>(sr), .generation = source_generation,
            .device = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(gpu.device.Get())),
            .resource = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(output.Get())),
            .resource_generation = native::AdaptiveResourceGeneration(output.Get())});
      };
      if (!native_ngx) {
        Check(!source_output(), "source ownership starts unbound before the first real backbuffer delivery");
      }
      const auto finish_delivery = [&] {
        submit();
        const addon::RuntimeLock lock(native::runtime_mutex);
        native::ResolveAdaptiveBorrowedDeliveries();
        native::DrainRetiredResources(true);
        timers::Poll(true, false);
      };
      // Mature the single full-size adaptive level through the ordinary owner
      // before introducing the nested wrapper whose delivery is under test.
      bool warm = false;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8 && !warm; ++attempt) {
        present();
        const auto calls_before = nr_calls();
        const auto accepted_before = telemetry.accepted;
        const auto skipped_before = telemetry.skipped;
        source_eval(static_cast<uint8_t>(176 + attempt));
        Check(nr_calls() <= calls_before + 1, "one adaptive level records no standby model evaluations");
        warm = nr_calls() == calls_before + 1 && previous_nr_succeeded()
            && telemetry.accepted == accepted_before + 1 && telemetry.skipped == skipped_before;
        finish_delivery();
      }
      const auto& feature = native::features.at(sr);
      Check(warm && feature.adaptive_session != nullptr && feature.adaptive_session->settings.enabled
                && feature.adaptive_session->ladder.count == 1 && feature.adaptive_cache != nullptr
                && feature.adaptive_cache->count == 1 && !native::direct_api.private_instance,
            "nested SR starts with one warmed adaptive level on the ordinary provider");
      native::ngx_slot_real[1].evaluate = native::ngx_slot_real[0].evaluate;
      native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(+[](ID3D12GraphicsCommandList* list,
          const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* block,
          PFN_NVSDK_NGX_ProgressCallback progress) -> NVSDK_NGX_Result {
        const auto result = native::HookedEvaluateFeatureSlot<1>(list, handle, block, progress);
        if (NVSDK_NGX_SUCCEED(result)) {
          active_sr->outer_output = Capture(*active_sr->gpu,
              native::GetD3D12Resource(block, NVSDK_NGX_Parameter_Output), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        return result;
      });
      uint32_t completed_groups = 0;
      for (uint32_t frame = 0; frame < 2; ++frame) {
        if (frame == 1) {
          present();
          const auto calls_before = nr_calls();
          const auto accepted_before = telemetry.accepted;
          const auto source_sequence = native::features.at(sr).sr_sequence;
          const auto timer_before = timers::internal::next_sequence;
          const auto completed_before = timers::completed_stats[static_cast<size_t>(Stage::UPSCALED)].sequence;
          const auto before = Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          observation.fail_sr = true;
          source_eval(239, NVSDK_NGX_Result_FAIL_InvalidParameter);
          observation.fail_sr = false;
          Check(observation.source_wrappers == 2 && native::features.at(sr).sr_sequence == source_sequence + 1
                    && nr_calls() == calls_before && telemetry.accepted == accepted_before
                    && timers::internal::next_sequence == timer_before && observation.outer_output.readback == nullptr,
                "an actual inner SR failure propagates through both wrappers with no NR or new completion recording");
          const auto after = Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          const auto recording = submission::ObserveRecording(gpu.list.Get());
          finish_delivery();
          Check(before.Pixels() == after.Pixels()
                    && timers::completed_stats[static_cast<size_t>(Stage::UPSCALED)].sequence == completed_before,
                "the failed inner SR frame preserves full output pixels and cannot publish a new completed group");
          std::cout << "XEFG_INLINE_NESTED_SR_FAILURE stage=Upscaled sr_calls=1 nr_evals=0 source_owners=1"
                       " new_completed=0 full_readback=unchanged evidence="
                    << (native_ngx ? "NATIVE_GPU+MOCK_SR_FAILURE" : "WARP+MOCK") << '\n';
          if (native_ngx) {
            nested_sr_failure = {{"evidence", "NATIVE_GPU+MOCK_SR_FAILURE"},
                {"source_sequence", native::features.at(sr).sr_sequence}, {"sr_evaluate_sequence", observation.calls},
                {"nr_evaluate_count", nr_calls() - calls_before}, {"new_completed_groups", 0},
                {"retained_completed_sequence", completed_before}, {"output_unchanged", true},
                {"recording_closed", recording->closed.load()}};
          }
        }
        present();
        const auto source_sequence = native::features.at(sr).sr_sequence;
        const auto calls_before = nr_calls();
        const auto accepted_before = telemetry.accepted;
        const auto timer_before = timers::internal::next_sequence;
        const auto value = static_cast<uint8_t>(224 + frame);
        source_eval(value);
        Check(observation.source_wrappers == 2 && native::features.at(sr).sr_sequence == source_sequence + 1
                  && nr_calls() == calls_before + 1 && previous_nr_succeeded()
                  && telemetry.accepted == accepted_before + 1,
              "nested Cpp0 to Cpp1 wrappers count one source owner, one successful SR and one actual NR per base frame");
        const auto segment = std::ranges::find_if(timers::internal::segments, [&](const auto& candidate) {
          return candidate.recorded && candidate.sequence > timer_before && std::strcmp(candidate.path, "after") == 0;
        });
        const bool segment_found = segment != std::end(timers::internal::segments);
        std::cout << "XEFG_INLINE_NESTED_PREDICATES found=" << segment_found
                  << " sequence_delta=" << timers::internal::next_sequence - timer_before
                  << " measure=" << (segment_found && segment->measure)
                  << " pass_begin=" << (segment_found ? static_cast<int>(segment->pass_begin) : -1)
                  << " pass_count=" << (segment_found ? static_cast<int>(segment->pass_count) : -1)
                  << " model=" << (segment_found ? static_cast<int>(segment->passes[0].model) : -1)
                  << " dual_models=" << native::direct_runtime_requirements.dual_models
                  << " adaptive_enabled=" << (segment_found && segment->adaptive.enabled)
                  << " adaptive_preparing=" << (segment_found && segment->adaptive.preparing)
                  << " delivery_present=" << (segment_found && segment->adaptive.delivery != nullptr)
                  << " delivery_state=" << (segment_found && segment->adaptive.delivery != nullptr
                          ? static_cast<int>(*segment->adaptive.delivery) : -1) << std::endl;
        Check(segment != std::end(timers::internal::segments)
                  && timers::internal::next_sequence == timer_before + 1 && segment->measure
                  && segment->pass_begin == 0 && segment->pass_count == 1 && segment->passes[0].model == 2
                  && segment->adaptive.enabled && !segment->adaptive.preparing && segment->adaptive.delivery != nullptr,
              "the actual nested NR call records one timed model B group and its production delivery receipt");
        const auto receipt = segment->adaptive.delivery;
        const auto timer_sequence = segment->sequence;
        const auto group = native::EvaluationStagePlan(sr).At(Stage::UPSCALED);
        Snapshot readback;
        if (native_ngx) {
          readback = Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        } else {
          // Keep the full-copy observer's sealed producer intact until the
          // real outer Present consumes its exact submission receipt.
          gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
          gpu.Transition(back, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
          gpu.list->CopyResource(back, output.Get());
          gpu.RecordTextureReadback(output.Get(), delivery_readback);
          gpu.Transition(back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
          gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          readback = {.readback = delivery_readback.readback, .footprint = delivery_readback.footprint,
              .width = width, .height = height, .pixel_bytes = 4, .format = back_desc.Format};
        }
        const auto recording = submission::ObserveRecording(gpu.list.Get());
        finish_delivery();
        const auto completed_fence = gpu.fence->GetCompletedValue();
        Check(recording->submitted.load() && recording->closed.load() && recording->completed_healthy
                  && completed_fence != UINT64_MAX && completed_fence >= gpu.serial,
              "the nested NR list completes and naturally resets with an exact healthy submission proof");
        if (native_ngx) {
          const auto rgba = readback.Pixels();
          Check(rgba.size() == static_cast<size_t>(width) * height * 8
                    && check_fp16(rgba, observation.sr_output.Pixels()),
                "nested native NR rewrites the entire SR output with finite RGBA and exact SR alpha");
        } else {
          check_mock_pixels(readback, value);
        }
        Check(observation.outer_output.readback != nullptr && observation.outer_output.Pixels() == readback.Pixels(),
              "the outer source owner receives the exact enhanced output produced by the inner NR wrapper");
        const auto& completed = timers::completed_stats[static_cast<size_t>(Stage::UPSCALED)];
        const bool published = completed.sequence == timer_sequence
            && native::CurrentCompletedGroup(completed, group, timers::NowNs());
        std::cout << "XEFG_INLINE_NESTED_OBS frame=" << frame + 1 << " sr_calls=1 nr_evals=1 source_owners=1"
                  << " pixel_readback=passed gpu_fence=healthy delivery="
                  << (*receipt == Delivery::DELIVERED ? "DELIVERED" : *receipt == Delivery::REJECTED ? "REJECTED" : "PENDING")
                  << " completed_group=" << published << std::endl;
        Check(*receipt == Delivery::DELIVERED,
              "successful nested SR must deliver inner non-owner NR after its single real GPU submission");
        Check(published && completed.passes[0].model == 2 && fg_held(),
              "nested SR publishes the current completed model B group while XeFG stays enabled");
        if (!native_ngx) {
          native::RunPresentPath(&route.queue, &route);
          Check(source_output() == output_owner,
                "successful nested SR and its actual same-list backbuffer copy establish production SourceOutput ownership");
          std::cout << "XEFG_INLINE_NESTED_SOURCE_OUTPUT frame=" << frame + 1
                    << " outer_enhanced=matched same_list_copy=1 output_registered=1 source_bound=1\n";
        }
        ++completed_groups;
        if (native_ngx) {
          nested_deliveries.push_back({{"sr_evaluate_sequence", observation.calls},
              {"source_sequence", native::features.at(sr).sr_sequence}, {"source_owner_count", 1},
              {"accepted_evaluate_sequence", telemetry.last_evaluate.sequence}, {"completed_group_sequence", completed.sequence},
              {"delivery", "DELIVERED"}, {"readback_pixels", static_cast<size_t>(width) * height},
              {"readback_fence_value", gpu.serial}, {"readback_fence_completed", completed_fence},
              {"gpu_fence_healthy", recording->completed_healthy}, {"sdk_fg_enabled", session.context->enabled},
              {"outer_enhanced_output_matches", true}});
        }
      }
      std::cout << "XEFG_INLINE_NESTED stage=Upscaled wrappers=Cpp0>Cpp1 frames=" << completed_groups
                << " sr_calls=2 nr_evals=2 source_owners=2 adaptive_levels=1 gpu_timers=1"
                   " delivery=DELIVERED completed_groups=2 full_readbacks=2 fg=held_on evidence="
                << (native_ngx ? "NATIVE_GPU+NATIVE_NGX+MOCK_XeFG" : "WARP+MOCK") << '\n';
      std::cout << "XEFG_INLINE_NESTED_RECOVERY stage=Upscaled failed_inner_sr=1 recovered_own_sequence=1\n";
    }
    run_xefg();
    if (native_ngx) {
      std::cout << "CASE path=XeFG-inline-handoff stage=" << stage_name
                << " fg=held_on sr=" << (real_sr ? "NATIVE_NGX" : "MOCK")
                << " sr_wrapper=actual inline_provider=Core xefg_provider=isolated"
                   " inline_nr_evals=2 inline_delivery_readback=2 xefg_nr_evals=2 xefg_delivery_readback=2"
                << " game_pending_and_replayable=" << (render ? 0 : 1) << " evidence=NATIVE_GPU+NATIVE_NGX+MOCK_XeFG\n";
    } else {
      std::cout << "XEFG_INLINE_HANDOFF stage=" << stage_name
                << " fg=held_on sr=MOCK sr_wrapper=actual inline_nr_evals=2 inline_delivery_readback=2"
                   " xefg_nr_evals=2 xefg_delivery_readback=2"
                << " game_pending_and_replayable=" << (render ? 0 : 1) << " evidence=WARP+MOCK\n";
    }
  }
  Check(NVSDK_NGX_SUCCEED(native::HookedReleaseFeatureSlot<0>(sr)), "production Release wrapper releases the fixture SR source");
  if (native_ngx) {
    sample_memory("last_delivery");
    Check(phases.size() == 5 && external_blocks.size() == 2 && fg_on_samples > 0 && fg_off_samples == 0,
          "native receipt contains all five phases and both game-list obstructions with FG held on");
    *native_report = {{"schema", "nr-nexus-xefg-inline-handoff/1"}, {"status", "Passed"},
        {"evidence", "NATIVE_GPU+NATIVE_NGX+MOCK_XeFG"}, {"case", "xefg-held-inline-handoff"},
        {"sr_evidence", real_sr ? "NATIVE_NGX" : "MOCK"}, {"runtime", native::direct_runtime_requirements.file_version},
        {"driver_x100", native::direct_runtime_requirements.driver_x100}, {"runtime_sha256", native::direct_runtime_sha256},
        {"gpu_memory", {{"scope", "sampled process LOCAL CurrentUsage including inputs, SR, NR and retirement"},
            {"sampled_peak_local_bytes", sampled_peak_local_bytes}, {"samples", std::move(memory_samples)}}},
        {"sdk_fg", {{"evidence", "MOCK_XeFG"}, {"on_samples", fg_on_samples}, {"off_samples", fg_off_samples}}},
        {"phases", std::move(phases)}, {"external_game_recording_blocks", std::move(external_blocks)},
        {"nested_sr_deliveries", std::move(nested_deliveries)}, {"nested_sr_failure", std::move(nested_sr_failure)},
        {"nested_source_output", {{"status", "NotRun"}, {"reason", "SR RGBA16F and SDK RGBA8 backbuffer formats differ"}}}};
  }
}
}  // namespace xefg_inline_handoff_cases
