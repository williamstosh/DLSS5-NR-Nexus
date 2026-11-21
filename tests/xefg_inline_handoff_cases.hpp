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
  const auto key = std::pair{false, reinterpret_cast<uintptr_t>(session.handle)};
  const auto epoch = session.context->epoch;
  const auto fg_held = [&] {
    return xefg::Enabled() && session.context->enabled && session.context->epoch == epoch;
  };
  Check(fg_held() && xefg::CurrentResult() == fg::Result::SUBMITTED
            && native::direct_api.private_instance && native::TeardownProof() == Work::kReplayable,
        "inline handoff starts at completed, replayable private XeFG work with SDK FG on");

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
    size_t nr_calls_at_entry = 0;
    bool consumed_replacement = false;
    Snapshot consumed_color;
    Snapshot sr_output;
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
    observed.nr_calls_at_entry = (observed.native_ngx ? native_runtime_evaluation_results.size() : model_calls);
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
  const auto source_eval = [&](uint8_t value) {
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
    gpu.list->ClearState(nullptr);
    const auto calls_before = observation.calls;
    Check(NVSDK_NGX_SUCCEED(native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), sr, &parameters, nullptr))
              && observation.calls == calls_before + 1 && native::create_contracts.at(sr).source_generation == source_generation
              && native::GetD3D12Resource(&parameters, NVSDK_NGX_Parameter_Color) == color.Get(),
          "successful SR Evaluate keeps its source generation and restores the original Color binding");
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
    native::hook_point = native::kHookPresent;
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
      present();
      const auto& service_stream = native::fg_service::streams.at(key);
      wait_fence(service_stream.fence, service_stream.submitted);
      if (xefg::CurrentResult() != fg::Result::SUBMITTED) {
        Check(xefg::DisplayResult() != fg::Result::SUBMITTED && xefg::SubmittedFrames() == service_before,
              "XeFG restoration warmup does not report a submitted frame");
        resumed = 0;
        frames.clear();
        gpu.uploads.clear();
        continue;
      }
      Check(xefg::DisplayResult() == fg::Result::SUBMITTED && xefg::SubmittedFrames() == service_before + 1
                && nr_calls() == calls_before + 1 && previous_nr_succeeded() && native::direct_api.private_instance,
            "XeFG Present uses its private provider and submits one actual NR pass again");
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

  for (const auto stage : {Stage::RENDER, Stage::UPSCALED}) {
    const bool render = stage == Stage::RENDER;
    const char* stage_name = (render ? "Render" : "Upscaled");
    const auto& old_stream = native::fg_service::streams.at(key);
    wait_fence(old_stream.fence, old_stream.submitted);
    const auto old_slot = std::ranges::find_if(old_stream.slots, [&](const auto& slot) {
      return slot.list != nullptr && slot.fence_value == old_stream.submitted;
    });
    Check(old_slot != old_stream.slots.end() && native::TeardownProof() == Work::kReplayable,
          "last submitted private XeFG recording completed without a Reset");
    const auto old_recording = submission::ObserveRecording(old_slot->list);
    Check(!old_recording->closed.load(), "old XeFG recording is still replayable before the hook change");
    const std::array<ComPtr<IUnknown>, 4> old_owned_objects{
        old_slot->color[0], old_slot->color[1], old_slot->list, old_slot->allocator};

    native::hook_point = (render ? native::kHookRender : native::kHookUpscaled);
    {
      funnel_accounting_tests::Restore off_restore;
      off_restore.Atomic(&native::enabled);
      off_restore.Atomic(&native::disable_release_mode);
      native::enabled = false;
      native::disable_release_mode = 0;
      native::RunPresentPath(&route.queue, &route);
      Check(native::fg_service::streams.contains(key) && !old_recording->closed.load()
                && native::direct_api.private_instance,
            "NR Off with release policy zero retains the old replayable private pool");
      std::cout << "XEFG_INLINE_OFF_HOLD stage=" << stage_name << " policy=0 replayable=retained\n";
    }
    const auto& telemetry = native::stage_telemetry[static_cast<size_t>(stage)];
    const auto model_before_switch = nr_calls();
    const auto pending_before_switch = pending_count();
    const auto service_before_switch = xefg::SubmittedFrames();
    uint32_t delivered = 0;
    uint32_t source_frames = 0;
    nlohmann::json frames = nlohmann::json::array();
    for (; source_frames < native::kContractDebounceFrames + 8 && delivered < 2; ++source_frames) {
      present();
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
      Check(!native::direct_api.private_instance && old_recording->closed.load()
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
          native::RunPresentPath(&route.queue, &route);
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
        native::hook_point = native::kHookPresent;
        const auto expect_blocked = [&](Work proof) {
          const auto declines = pending_count();
          Check(native::TeardownProof() == proof, "game recording supplies the expected lifetime obstruction");
          present();
          Check(xefg::CurrentResult() == fg::Result::NOT_SERVED && xefg::DisplayResult() == fg::Result::NOT_SERVED
                    && xefg::SubmittedFrames() == service_before_switch && pending_count() == declines + 1
                    && nr_calls() == calls_before + 1 && !recording->closed.load()
                    && !native::direct_api.private_instance && native::direct_api.module == provider
                    && native::features.at(sr).slots[0].handle == model && native::features.at(sr).slots[0].parameters == block,
                "pending or replayable game work blocks XeFG without fake activity or provider release");
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
    Check(phases.size() == 5 && external_blocks.size() == 2 && fg_on_samples > 0 && fg_off_samples == 0,
          "native receipt contains all five phases and both game-list obstructions with FG held on");
    *native_report = {{"schema", "nr-nexus-xefg-inline-handoff/1"}, {"status", "Passed"},
        {"evidence", "NATIVE_GPU+NATIVE_NGX+MOCK_XeFG"}, {"case", "xefg-held-inline-handoff"},
        {"sr_evidence", real_sr ? "NATIVE_NGX" : "MOCK"}, {"runtime", native::direct_runtime_requirements.file_version},
        {"driver_x100", native::direct_runtime_requirements.driver_x100}, {"runtime_sha256", native::direct_runtime_sha256},
        {"sdk_fg", {{"evidence", "MOCK_XeFG"}, {"on_samples", fg_on_samples}, {"off_samples", fg_off_samples}}},
        {"phases", std::move(phases)}, {"external_game_recording_blocks", std::move(external_blocks)}};
  }
}
}  // namespace xefg_inline_handoff_cases
