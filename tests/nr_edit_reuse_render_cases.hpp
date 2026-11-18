/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <utility>
#include <vector>

#include <DirectXPackedVector.h>

// Included after the native GPU fixture and its model substitutes.
namespace nr_edit_reuse_render_cases {
namespace addon = renodx::addons::dlss5;
namespace timers = addon::gpu_timers;
using stage_execution_cases::Capture;
using stage_execution_cases::Snapshot;

constexpr uint32_t RENDER_SIZE = 16;
constexpr uint32_t SR_SIZE = 32;
constexpr uint32_t EDIT_EDGE = RENDER_SIZE / 2;
constexpr auto COLOR_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr float MV_SCALE = 2.f;
using ColorPixel = std::array<uint16_t, 4>;
using MotionPixel = std::array<float, 2>;

struct FrameInput {
  int reset = 0;
  float jitter_x = -.5f;
  bool has_jitter_y = true;
  bool split_motion = false;
  NVSDK_NGX_Result sr_result = NVSDK_NGX_Result_Success;
};

struct SrConsumer {
  const NVSDK_NGX_Handle* handle = nullptr;
  ID3D12Resource* game_color = nullptr;
  ID3D12Resource* game_motion = nullptr;
  FrameInput input;
  uint32_t calls = 0;
  bool swapped_color = false;
  Snapshot motion;
};

static SrConsumer* sr_consumer = nullptr;

NVSDK_NGX_Result NVSDK_CONV CreateSr(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature feature,
                                   NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** handle) {
  Check(feature == native::kFeatureDlss, "Render fixture creates a game SR source");
  static uint32_t sr_handle = 1;
  *handle = reinterpret_cast<NVSDK_NGX_Handle*>(&sr_handle);
  return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV EvaluateSr(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                     const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback) {
  auto& consumer = *sr_consumer;
  ++consumer.calls;
  Check(handle == consumer.handle && list == model_gpu->list.Get(), "SR receives the original handle and list");
  auto* color = native::GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Color);
  auto* output = native::GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Output);
  auto* motion = native::GetD3D12Resource(parameters, NVSDK_NGX_Parameter_MotionVectors);
  Check(color != nullptr && color->GetDesc().Width == RENDER_SIZE && color->GetDesc().Height == RENDER_SIZE
            && color->GetDesc().Format == COLOR_FORMAT && output != nullptr && output->GetDesc().Width == SR_SIZE
            && output->GetDesc().Height == SR_SIZE && output->GetDesc().Format == COLOR_FORMAT,
        "game SR receives Render-sized FP16 Color and its larger SR output");
  Check(motion == consumer.game_motion && native::GetFloat(parameters, NVSDK_NGX_Parameter_MV_Scale_X) == MV_SCALE
            && native::GetFloat(parameters, NVSDK_NGX_Parameter_MV_Scale_Y) == MV_SCALE,
        "game SR receives current motion and the original scales");
  float jitter_y = 0.f;
  const bool has_jitter_y = native::GetFloatIfPresent(parameters, NVSDK_NGX_Parameter_Jitter_Offset_Y, jitter_y);
  Check(native::GetFloat(parameters, NVSDK_NGX_Parameter_Jitter_Offset_X) == consumer.input.jitter_x
            && has_jitter_y == consumer.input.has_jitter_y && jitter_y == 0.f
            && native::GetInt(parameters, NVSDK_NGX_Parameter_Reset, -1) == consumer.input.reset,
        "game SR receives its original jitter pair and Reset");
  consumer.motion = Capture(*model_gpu, motion, NATIVE_INPUT_STATE);
  consumer.swapped_color = color != consumer.game_color;
  const auto color_state = (consumer.swapped_color ? native::kPreSrReadable : NATIVE_INPUT_STATE);
  model_gpu->Transition(color, color_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  model_gpu->Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  const D3D12_TEXTURE_COPY_LOCATION from{.pResource = color, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  const D3D12_TEXTURE_COPY_LOCATION to{.pResource = output, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  // Use the actual Color binding to fill one Render-sized tile of the SR output.
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  model_gpu->Transition(output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  model_gpu->Transition(color, D3D12_RESOURCE_STATE_COPY_SOURCE, color_state);
  return consumer.input.sr_result;
}

NVSDK_NGX_Result NVSDK_CONV EvaluateStampedModel(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                               const NVSDK_NGX_Parameter* parameters,
                                               PFN_NVSDK_NGX_ProgressCallback callback) {
  const auto result = EvaluateModel(list, handle, parameters, callback);
  if (NVSDK_NGX_FAILED(result)) return result;
  auto* output = native::GetD3D12Resource(parameters, "DLSSNR.Output");
  const auto desc = output->GetDesc();
  Check(desc.Width == RENDER_SIZE && desc.Height == RENDER_SIZE && desc.Format == COLOR_FORMAT,
        "model output stays at fixed full Render resolution");
  // Uniform reference RGB makes both edits reusable. A sharp edge in the
  // model's encoded output makes a one-texel history-address error visible.
  std::vector<ColorPixel> pixels(RENDER_SIZE * RENDER_SIZE);
  for (size_t pixel = 0; pixel < pixels.size(); ++pixel) {
    const uint16_t stamp = (pixel % RENDER_SIZE < EDIT_EDGE ? 0x3900 : 0x3b00);  // FP16 .625 / .875.
    pixels[pixel] = {stamp, stamp, stamp, 0x3c00};
  }
  auto transfer = model_gpu->CreateTextureTransfer(output);
  model_gpu->Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  model_gpu->UploadTexture(output, transfer, reinterpret_cast<const uint8_t*>(pixels.data()),
                           pixels.size() * sizeof(ColorPixel), RENDER_SIZE * sizeof(ColorPixel));
  model_gpu->Transition(output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  return result;
}

int Run() {
  ComPtr<ID3D12InfoQueue> info;
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    Check(renodx::utils::directx::Initialize(), "initialize DirectX entry points");
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
    }
    Gpu gpu;
    model_gpu = &gpu;
    gpu.device.As(&info);
    native::enabled = true;
    native::hooks_enabled = true;
    native::d3d12_device_seen = true;
    native::shutdown_quiesce_enabled = false;
    native::dedupe_mode = addon::kDedupePerEvaluate;
    native::hook_point = native::kHookRender;
    native::stack_passes = 1;
    native::nr_follow_input_res = false;
    native::nr_resolution_scale = 1.f;
    native::nr_adaptive_enabled = false;
    native::codec_mode = 0;
    native::feed_mode = 1;
    native::norm_governor = 0;
    native::look_mode = false;
    native::detail_stability = 1;
    native::control_mask_enabled = false;
    native::use_auto_mask = false;
    native::cost_meter_enabled = false;
    native::gpu_timers_enabled = true;
    native::transfer_strength = 1.f;
    native::color_strength = 1.f;
    native::intensity = 1.f;
    native::preset = 2;
    native::edit_reuse_enabled = true;
    native::monitor_requested = true;
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = stage_execution_cases::Create;
    native::direct_api.evaluate = EvaluateStampedModel;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::direct_runtime_requirements.dual_models = true;
    native::direct_runtime_requirements.file_version = "310.9.16.0";
    native::parameter_runtime_ready = true;
    native::core_allocate_parameters = addon::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = addon::bridge::DestroyOwnedParameters;
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "Render reuse queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "Render reuse list hooks");

    auto color = gpu.Texture(RENDER_SIZE, RENDER_SIZE, COLOR_FORMAT, NATIVE_INPUT_STATE);
    auto motion = gpu.Texture(RENDER_SIZE, RENDER_SIZE, DXGI_FORMAT_R32G32_FLOAT, NATIVE_INPUT_STATE);
    auto depth = gpu.Texture(RENDER_SIZE, RENDER_SIZE, DXGI_FORMAT_R32_FLOAT, NATIVE_INPUT_STATE);
    ComPtr<ID3D12Resource> sr_output;
    auto output_desc = color->GetDesc();
    output_desc.Width = SR_SIZE;
    output_desc.Height = SR_SIZE;
    output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &output_desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&sr_output))), "Render fixture SR output");

    const auto upload = [&](ID3D12Resource* resource, const auto& pixels) {
      auto transfer = gpu.CreateTextureTransfer(resource);
      gpu.Transition(resource, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.UploadTexture(resource, transfer, reinterpret_cast<const uint8_t*>(pixels.data()),
                        pixels.size() * sizeof(pixels[0]), RENDER_SIZE * sizeof(pixels[0]));
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, NATIVE_INPUT_STATE);
    };
    upload(depth.Get(), std::vector<float>(RENDER_SIZE * RENDER_SIZE, .5f));
    gpu.Submit();

    addon::bridge::OwnedParameters base_parameters;
    base_parameters.Set(NVSDK_NGX_Parameter_Color, color.Get());
    base_parameters.Set(NVSDK_NGX_Parameter_Output, sr_output.Get());
    base_parameters.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
    base_parameters.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
    base_parameters.Set(NVSDK_NGX_Parameter_Width, RENDER_SIZE);
    base_parameters.Set(NVSDK_NGX_Parameter_Height, RENDER_SIZE);
    base_parameters.Set(NVSDK_NGX_Parameter_OutWidth, SR_SIZE);
    base_parameters.Set(NVSDK_NGX_Parameter_OutHeight, SR_SIZE);
    base_parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, RENDER_SIZE);
    base_parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, RENDER_SIZE);
    base_parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<uint32_t>(
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_IsHDR));
    base_parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, MV_SCALE);
    base_parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, MV_SCALE);
    NVSDK_NGX_Handle* handle = nullptr;
    native::ngx_slot_real[0].create = reinterpret_cast<void*>(CreateSr);
    Check(native::HookedCreateFeatureSlot<0>(gpu.list.Get(), native::kFeatureDlss, &base_parameters, &handle)
              == NVSDK_NGX_Result_Success && handle != nullptr,
          "production Create wrapper registers game SR");
    native::ngx_slot_real[0].create = nullptr;
    native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(EvaluateSr);

    Snapshot composed_motion;
    model_parameter_observer = [&](const NVSDK_NGX_Handle* model, const NVSDK_NGX_Parameter* parameters) {
      auto* proxy = native::GetD3D12Resource(parameters, "DLSSNR.Color");
      auto* nr_motion = native::GetD3D12Resource(parameters, "DLSSNR.MVec");
      Check(stage_execution_cases::presets.at(model) == 2 && proxy != nullptr
                && proxy->GetDesc().Width == RENDER_SIZE && proxy->GetDesc().Height == RENDER_SIZE,
            "actual NR call requests model B with full Render input");
      Check(nr_motion != nullptr && native::GetFloat(parameters, "DLSSNR.MVecScaleX") == MV_SCALE
                && native::GetFloat(parameters, "DLSSNR.MVecScaleY") == MV_SCALE,
            "NR observes motion with host scale preserved");
      if (nr_motion != motion.Get()) {
        composed_motion = Capture(gpu, nr_motion, NATIVE_INPUT_STATE);
      }
    };

    struct FrameResult {
      uint64_t model_calls = 0;
      bool swapped_color = false;
      std::vector<ColorPixel> sr_tile;
      uint64_t timer_sequence = 0;
      bool timer_published = false;
      timers::Stats completed_timer;
    };
    uint32_t application_frames = 0;
    const auto frame = [&](const FrameInput& input, bool submit = true) {
      std::vector<ColorPixel> source(RENDER_SIZE * RENDER_SIZE);
      std::vector<MotionPixel> vectors(source.size());
      constexpr std::array<uint16_t, 4> alphas{0x3400, 0x3600, 0x3800, 0x3900};
      for (size_t pixel = 0; pixel < source.size(); ++pixel) {
        source[pixel] = {0x3400, 0x3600, 0x3800, alphas[(pixel + application_frames) % alphas.size()]};
        vectors[pixel] = {.5f, -.25f};
        if (input.split_motion) {
          vectors[pixel] = {(pixel % RENDER_SIZE < EDIT_EDGE ? .5f : 1.f), .5f};
        }
      }
      ++application_frames;
      upload(color.Get(), source);
      upload(motion.Get(), vectors);
      auto parameters = base_parameters;
      parameters.Set(NVSDK_NGX_Parameter_Reset, input.reset);
      parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, input.jitter_x);
      if (input.has_jitter_y) {
        parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f);
      }
      SrConsumer consumer{.handle = handle, .game_color = color.Get(), .game_motion = motion.Get(), .input = input};
      sr_consumer = &consumer;
      composed_motion = {};
      const auto before = model_calls;
      const auto timer_sequence_before = timers::internal::next_sequence;
      gpu.list->ClearState(nullptr);
      const auto status = native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), handle, &parameters, nullptr);
      sr_consumer = nullptr;
      Check(consumer.calls == 1 && status == input.sr_result, "wrapper calls game SR once and propagates its return");
      Check(native::GetD3D12Resource(&parameters, NVSDK_NGX_Parameter_Color) == color.Get(),
            "wrapper restores the original Color binding on return from SR");
      FrameResult result{.model_calls = model_calls - before, .swapped_color = consumer.swapped_color};
      const timers::Segment* timer = nullptr;
      if (timers::internal::next_sequence != timer_sequence_before) {
        timer = &timers::internal::segments[timers::internal::current.idx];
        Check(timer->recorded && timer->measure && std::strcmp(timer->path, "preSR") == 0
                  && timer->sequence == timer_sequence_before + 1 && timers::internal::mapped != nullptr
                  && timers::internal::mapped[timer->base + timers::kSlotFrameStart] == timers::kUnwritten,
              "PreSR wrapper recorded one unresolved production timestamp segment");
        result.timer_sequence = timer->sequence;
      }
      if (!submit) {
        gpu.Retain(consumer.motion.readback.Get());
        return result;
      }
      const auto original = Capture(gpu, color.Get(), NATIVE_INPUT_STATE);
      const auto output = Capture(gpu, sr_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      // Use the production queue/list hooks and preserve their receipts before polling the timer.
      Check(SUCCEEDED(gpu.list->Close()), "close Render wrapper recording");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      gpu.queue->ExecuteCommandLists(1, lists);
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "signal Render wrapper completion");
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr, "Render wrapper completion event");
      const auto signal = gpu.fence->SetEventOnCompletion(gpu.serial, event);
      const auto wait = (SUCCEEDED(signal) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
      CloseHandle(event);
      const auto completed_fence = gpu.fence->GetCompletedValue();
      Check(wait == WAIT_OBJECT_0 && completed_fence != UINT64_MAX && completed_fence >= gpu.serial,
            "Render wrapper recording completes on the GPU fence");
      Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
            "close Render wrapper submission generation against replay");
      native::DrainRetiredResources(true);
      const auto published_before = timers::stage_published[0];
      if (timer != nullptr) {
        Check(timer->proof == timers::SampleState::VALID
                  && timers::internal::mapped[timer->base + timers::kSlotFrameStart] != timers::kUnwritten,
              "Render timer has a single-execution receipt and the GPU resolved its timestamp");
      }
      timers::Poll(true, false);
      result.timer_published = timers::stage_published[0] != published_before;
      if (timer != nullptr) {
        const auto& complete = timers::completed_stats[0];
        const auto& sample = (complete.sequence == result.timer_sequence ? complete : timers::stage_stats[0]);
        if (complete.sequence == result.timer_sequence) {
          result.completed_timer = complete;
        }
        std::cout << "OBS Render timer frame=" << application_frames << " seq=" << result.timer_sequence
                  << " calls=" << result.model_calls << " swapped=" << result.swapped_color
                  << " sr_result=" << status << " fence=" << completed_fence << " resolved=1 exact=1"
                  << " eval_start=" << ((timer->written >> timers::PassEvalStart(0)) & 1)
                  << " eval_end=" << ((timer->written >> timers::PassEvalEnd(0)) & 1)
                  << " published=" << result.timer_published << " complete=" << result.completed_timer.valid
                  << " total_us=" << sample.total_us << " eval_us=" << sample.eval_us
                  << " own_us=" << sample.own_us << " state=" << timers::StateText(sample.state) << '\n';
      }
      gpu.uploads.clear();
      gpu.retained_resources.clear();
      const auto original_bytes = original.Pixels();
      const auto sr_motion_bytes = consumer.motion.Pixels();
      Check(original_bytes.size() == source.size() * sizeof(ColorPixel)
                && std::memcmp(original_bytes.data(), source.data(), original_bytes.size()) == 0,
            "original Render Color bytes survive the production wrapper");
      Check(sr_motion_bytes.size() == vectors.size() * sizeof(MotionPixel)
                && std::memcmp(sr_motion_bytes.data(), vectors.data(), sr_motion_bytes.size()) == 0,
            "SR consumes original per-frame motion bytes while NR can consume a separate texture");
      const auto output_bytes = output.Pixels();
      std::vector<ColorPixel> tile(source.size());
      for (uint32_t row = 0; row < RENDER_SIZE; ++row) {
        std::memcpy(tile.data() + row * RENDER_SIZE, output_bytes.data() + row * SR_SIZE * sizeof(ColorPixel),
                    RENDER_SIZE * sizeof(ColorPixel));
      }
      for (size_t pixel = 0; pixel < tile.size(); ++pixel) {
        Check(tile[pixel][3] == source[pixel][3], "SR sees exact current-frame alpha");
      }
      if (!consumer.swapped_color) {
        Check(tile == source, "SR pass-through consumes exact current source pixels");
      }
      result.sr_tile = std::move(tile);
      return result;
    };

    const auto warm = [&] {
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8; ++attempt) {
        const auto result = frame({.reset = 1});
        if (result.swapped_color && result.model_calls == 1) return;
      }
      Check(false, "Render wrapper reaches a real model call after warm-up");
    };
    warm();
    const auto refresh = frame({.reset = 1});
    Check(refresh.model_calls == 1 && refresh.swapped_color && model_resets.back() == 1
              && composed_motion.readback == nullptr,
          "game Reset refreshes the edit with current-frame motion");

    const FrameInput shifted{.jitter_x = .5f, .split_motion = true};
    const auto render_anchor = native::features.at(handle).slots[0].edit_reuse;
    const auto look_stabilized = native::look_stability_passes.load();
    native::detail_stability = 0;
    native::nr_follow_input_res = true;
    native::nr_resolution_scale = .75f;
    native::native_fg_declaration = native::NativeFgDeclaration::kOn;
    const auto reuse = frame(shifted);
    Check(reuse.model_calls == 0 && reuse.swapped_color
              && native::features.at(handle).slots[0].edit_reuse == render_anchor
              && native::look_stability_passes.load() == look_stabilized,
          "Render Auto detail and following matching guides keep the Off/full-size x2 anchor across an FG declaration change");
    const auto& observed_reuse = native::features.at(handle).edit_reuse_monitor[0];
    Check(observed_reuse.delivery == native::EditReuseDelivery::REUSED && observed_reuse.recording != nullptr
              && observed_reuse.recording->submitted.load(),
          "the real SR wrapper publishes delivery for the measured Render reuse");
    const auto& refresh_timer = refresh.completed_timer;
    const auto& reuse_timer = reuse.completed_timer;
    Check(refresh.timer_published && refresh_timer.valid && refresh_timer.queue == gpu.queue.Get()
              && refresh_timer.total_us > 0 && refresh_timer.eval_us > 0
              && refresh_timer.eval_us == refresh_timer.passes[0].eval_us,
          "Render refresh publishes a complete GPU sample with the NR model time");
    Check(reuse.timer_published && reuse_timer.valid && reuse_timer.queue == gpu.queue.Get()
              && reuse_timer.total_us > 0 && reuse_timer.passes[0].total_us > 0
              && reuse_timer.eval_us == 0 && reuse_timer.passes[0].eval_us == 0
              && reuse_timer.own_us == reuse_timer.total_us,
          "Render reuse publishes the full GPU cost and zero NR Evaluate time");
    const size_t edge = 6 * RENDER_SIZE + EDIT_EDGE;
    const auto red = [](const ColorPixel& pixel) { return DirectX::PackedVector::XMConvertHalfToFloat(pixel[0]); };
    const float left = red(refresh.sr_tile[edge - 1]);
    const float right = red(refresh.sr_tile[edge]);
    Check(right - left > .05f && std::abs(left - .25f) > .05f, "refresh delivers two distinct nonzero edits to SR");
    // At x=7 current MV is +1 pixel and anchor jitter minus current jitter
    // is -1 pixel. Sampling the next guide texel or dropping jitter hits x=8.
    Check(std::abs(red(reuse.sr_tile[edge - 1]) - left) < .004f
              && std::abs(red(reuse.sr_tile[edge]) - right) < .004f,
          "SR pixels witness same-raster guide sampling and the jitter-corrected edit address");

    native::native_fg_declaration = native::NativeFgDeclaration::kUnknown;
    native::nr_follow_input_res = false;
    native::nr_resolution_scale = std::nextafter(1.f, 0.f);
    const auto composed_refresh = frame({});
    Check(composed_refresh.model_calls == 1 && composed_refresh.swapped_color && model_resets.back() == 0
              && composed_motion.format == DXGI_FORMAT_R32G32_FLOAT && composed_motion.width == RENDER_SIZE
              && composed_motion.height == RENDER_SIZE && native::features.at(handle).slots[0].edit_reuse == render_anchor,
          "a scale rounded to the same applied grid preserves History and the following refresh receives composed motion");
    const auto mv_bytes = composed_motion.Pixels();
    MotionPixel composed{};
    std::memcpy(composed.data(), mv_bytes.data() + (edge - 2) * sizeof(MotionPixel), sizeof(composed));
    // At x=6 the current +1 pixel and jitter delta +1 select the prior x=8
    // vector (+2 pixels), rather than the x=7 vector (+1 pixel).
    Check(std::abs(composed[0] - 1.5f) < 1e-5f && std::abs(composed[1] - .25f) < 1e-5f,
          "NR consumes the two-frame MV sum in original parameter units");
    const auto alternating = frame(shifted);
    Check(alternating.model_calls == 0 && alternating.swapped_color, "Render alternates refresh and reuse continuously");

    const auto reset = frame({.reset = 1});
    Check(reset.model_calls == 1 && reset.swapped_color && model_resets.back() == 1
              && composed_motion.readback == nullptr,
          "Reset after reuse discards composed motion and runs a fresh model evaluation");
    const auto missing = frame({.has_jitter_y = false});
    Check(missing.model_calls == 1 && missing.swapped_color && composed_motion.readback == nullptr,
          "missing jitter pair falls back to ordinary Render NR with game motion");
    Check(std::abs(red(missing.sr_tile[edge - 1]) - left) < .004f
              && std::abs(red(missing.sr_tile[edge]) - right) < .004f,
          "missing jitter delivers fresh Render edit pixels to SR");
    const auto recovered = frame({});
    Check(recovered.model_calls == 1 && recovered.swapped_color && composed_motion.readback == nullptr,
          "returning jitter contract refreshes before reusing again");
    const auto resumed = frame(shifted);
    Check(resumed.model_calls == 0 && resumed.swapped_color, "fresh Render anchor resumes reuse");

    fail_call = model_calls + 1;
    const auto failed = frame({});
    fail_call = 0;
    Check(failed.model_calls == 1 && !failed.swapped_color, "failed NR refresh sends original Color to SR");
    const auto backoff = frame(shifted);
    Check(backoff.model_calls == 0 && !backoff.swapped_color, "failed NR refresh leaves no edit to deliver during backoff");
    native::RecreateFeatures();
    warm();
    const auto sr_failure = frame({.reset = 1, .sr_result = NVSDK_NGX_Result_FAIL_InvalidParameter});
    Check(sr_failure.model_calls == 1 && sr_failure.swapped_color,
          "SR failure restoration is exercised with an actual replacement Color");
    const auto& observed_failure = native::features.at(handle).edit_reuse_monitor[0];
    Check(observed_failure.delivery == native::EditReuseDelivery::NONE
              && observed_failure.reason == native::EditReuseBlock::SR_FAILED,
          "a rejected real SR publishes the failure and never promotes its Render recording to delivered output");
    const auto after_sr_failure = frame({});
    Check(after_sr_failure.model_calls == 1 && after_sr_failure.swapped_color && model_resets.back() == 1
              && composed_motion.readback == nullptr,
          "SR rejection invalidates the produced anchor and the next refresh resets NR History");

    const auto abandoned = frame(shifted, false);
    Check(abandoned.model_calls == 0 && abandoned.swapped_color, "record a reuse without GPU submission");
    const auto abandoned_anchor = native::features.at(handle).slots[0].edit_reuse;
    const auto discarded = submission::ObserveRecording(gpu.list.Get());
    Check(!discarded->submitted.load() && SUCCEEDED(gpu.list->Close()) && SUCCEEDED(gpu.allocator->Reset())
              && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "discard the Render reuse recording");
    submission::OnCommandListReset(gpu.list.Get());
    Check(discarded->closed.load() && !discarded->submitted.load(), "discarded reuse has no GPU production proof");
    const auto rebuild = frame({});
    Check(rebuild.model_calls == 0 && !rebuild.swapped_color, "rebuild keeps original Color while a replacement model matures");
    const auto after_discard = frame({});
    Check(after_discard.model_calls == 1 && after_discard.swapped_color && model_resets.back() == 1
              && composed_motion.readback == nullptr && native::features.at(handle).slots[0].edit_reuse != abandoned_anchor,
          "discard recovery refreshes a new anchor without motion from the abandoned reuse");
    const auto reuse_after_discard = frame(shifted);
    Check(reuse_after_discard.model_calls == 0 && reuse_after_discard.swapped_color, "new anchor resumes reuse after discard");

    model_parameter_observer = {};
    native::ngx_slot_real[0].evaluate = nullptr;
    native::ForgetSourceHandleLocked(handle);
    native::FlushRetiredResources();
    native::direct_device->Release();
    native::direct_device = nullptr;
    model_gpu = nullptr;
    if (info != nullptr) {
      for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(index, message, &bytes)), "Render reuse debug message");
        Check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
      }
    }
    std::cout << "OBS Render reuse: frames=" << application_frames << " edit_edge_R=" << left << '/' << right
              << " NR_MV=" << composed[0] << '/' << composed[1] << " SR_MV=0.5/-0.25\n";
    std::cout << "PASS Render edit reuse evidence=WARP+MOCK, FP16 16x16 -> 32x32 SR, model B, cadence 1/0/1/0, "
                 "jittered edit address, composed NR motion, SR delivery/restoration, FG declaration, Reset, failures and discard recovery\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL Render edit reuse: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace nr_edit_reuse_render_cases
