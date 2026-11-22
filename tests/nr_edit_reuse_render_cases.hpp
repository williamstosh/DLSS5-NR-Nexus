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
#include <optional>
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
  std::optional<uint32_t> alpha_phase;
#if defined(NR_NEXUS_DEV_CONTROL)
  bool nested_cpp = false;
#endif
};

struct SrConsumer {
  const NVSDK_NGX_Handle* handle = nullptr;
  ID3D12Resource* game_color = nullptr;
  ID3D12Resource* game_motion = nullptr;
  FrameInput input;
  uint32_t calls = 0;
  uint64_t model_calls_at_entry = 0;
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
  consumer.model_calls_at_entry = model_calls;
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
  ComPtr<ID3D12DebugCommandList> debug_list;
  if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&debug_list)))) {
    Check(debug_list->AssertResourceState(consumer.game_color, 0, NATIVE_INPUT_STATE)
              && debug_list->AssertResourceState(color, 0, color_state),
          "original SR sees shader-readable game Color and replacement Color");
  }
  model_gpu->Transition(color, color_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  model_gpu->Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  const D3D12_TEXTURE_COPY_LOCATION from{.pResource = color, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  const D3D12_TEXTURE_COPY_LOCATION to{.pResource = output, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  // Tile the actual Color so the cross-stage case has a fully defined SR output.
  for (uint32_t y = 0; y < SR_SIZE; y += RENDER_SIZE) {
    for (uint32_t x = 0; x < SR_SIZE; x += RENDER_SIZE) {
      list->CopyTextureRegion(&to, x, y, 0, &from, nullptr);
    }
  }
  model_gpu->Transition(output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  model_gpu->Transition(color, D3D12_RESOURCE_STATE_COPY_SOURCE, color_state);
#if defined(NR_NEXUS_DEV_CONTROL)
  if (consumer.input.nested_cpp) return NVSDK_NGX_Result_Success;
#endif
  return consumer.input.sr_result;
}

#if defined(NR_NEXUS_DEV_CONTROL)
static int64_t nested_sr_return_ns = 0;

NVSDK_NGX_Result NVSDK_CONV MirrorSr(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                   const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
  Check(NVSDK_NGX_SUCCEED(native::HookedEvaluateFeatureSlot<1>(list, handle, parameters, callback)),
      "nested Render SR returns success");
  nested_sr_return_ns = timers::NowNs();
  return sr_consumer->input.sr_result;
}
#endif

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
    ComPtr<ID3D12Debug1> gpu_validation;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
      if (SUCCEEDED(debug.As(&gpu_validation))) {
        gpu_validation->SetEnableGPUBasedValidation(TRUE);
      }
    }
    Gpu gpu;
    model_gpu = &gpu;
    gpu.device.As(&info);
    std::cout << "OBS Render D3D12 debug=" << (info != nullptr)
              << " gpu_validation=" << (gpu_validation != nullptr) << '\n';
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
      uint64_t model_calls_before_sr = 0;
      bool swapped_color = false;
      bool adaptive_waiting_for_submit = false;
      std::vector<ColorPixel> sr_tile;
      std::vector<uint8_t> sr_rgba;
      uint64_t timer_sequence = 0;
      uint64_t timer_written = 0;
      std::shared_ptr<addon::adaptive_resolution::DeliveryReceipt> adaptive_receipt;
      bool timer_awaited_delivery = false;
      bool timer_published = false;
      timers::Stats completed_timer;
    };
    uint32_t application_frames = 0;
    const auto frame = [&](const FrameInput& input, bool submit = true) {
      std::vector<ColorPixel> source(RENDER_SIZE * RENDER_SIZE);
      std::vector<MotionPixel> vectors(source.size());
      constexpr std::array<uint16_t, 4> alphas{0x3400, 0x3600, 0x3800, 0x3900};
      const auto alpha_phase = input.alpha_phase.value_or(application_frames);
      for (size_t pixel = 0; pixel < source.size(); ++pixel) {
        source[pixel] = {0x3400, 0x3600, 0x3800, alphas[(pixel + alpha_phase) % alphas.size()]};
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
#if defined(NR_NEXUS_DEV_CONTROL)
      if (input.nested_cpp) {
        native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(MirrorSr);
        native::ngx_slot_real[1].evaluate = reinterpret_cast<void*>(EvaluateSr);
      }
#endif
      const auto status = native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), handle, &parameters, nullptr);
#if defined(NR_NEXUS_DEV_CONTROL)
      if (input.nested_cpp) {
        native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(EvaluateSr);
        native::ngx_slot_real[1].evaluate = nullptr;
      }
#endif
      sr_consumer = nullptr;
      Check(consumer.calls == 1 && status == input.sr_result, "wrapper calls game SR once and propagates its return");
      Check(native::GetD3D12Resource(&parameters, NVSDK_NGX_Parameter_Color) == color.Get(),
            "wrapper restores the original Color binding on return from SR");
      FrameResult result{.model_calls = model_calls - before,
          .model_calls_before_sr = consumer.model_calls_at_entry - before, .swapped_color = consumer.swapped_color};
      if (const auto& adaptive = native::features.at(handle).adaptive_session; adaptive != nullptr) {
        result.adaptive_waiting_for_submit = adaptive->applied_generation != adaptive->status.generation;
      }
      const timers::Segment* timer = nullptr;
      // A mixed frame can also record an Upscaled segment after original SR.
      const auto render_segment = std::ranges::find_if(timers::internal::segments, [timer_sequence_before](const auto& segment) {
        return segment.sequence > timer_sequence_before && segment.recorded && segment.measure
            && std::strcmp(segment.path, "preSR") == 0;
      });
      if (render_segment != std::ranges::end(timers::internal::segments)) {
        timer = &*render_segment;
        Check(timers::internal::mapped != nullptr
                  && timers::internal::mapped[timer->base + timers::kSlotFrameStart] == timers::kUnwritten,
              "PreSR wrapper recorded an unresolved production timestamp segment");
        result.timer_sequence = timer->sequence;
        result.timer_written = timer->written;
        result.adaptive_receipt = timer->adaptive.delivery;
        result.timer_awaited_delivery = result.adaptive_receipt != nullptr
            && *result.adaptive_receipt == addon::adaptive_resolution::DeliveryReceipt::PENDING;
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
      native::ResolveAdaptiveBorrowedDeliveries();
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
      auto output_bytes = output.Pixels();
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
      result.sr_rgba = std::move(output_bytes);
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

#if defined(NR_NEXUS_DEV_CONTROL)
    namespace benchmark = addon::control_mask_capture::benchmark;
    using benchmark::Delivery;
    timers::benchmark_observer = benchmark::ObserveTiming;
    native::control_mask_use_core_context = false;
    Check(benchmark::Arm(8, 30, nullptr, nullptr), "arm the Render source benchmark");
    uint32_t benchmark_frames = 0;
    const auto measure = [&](Delivery delivery, bool nr_enabled, bool reuse_enabled) {
      native::enabled = nr_enabled;
      native::edit_reuse_enabled = reuse_enabled;
      const auto recording = submission::RecordingIdentity(gpu.list.Get());
      const auto result = frame({.reset = delivery == Delivery::kRefresh ? 1 : 0, .nested_cpp = true});
      std::shared_lock lock(benchmark::state.mutex);
      ++benchmark_frames;
      Check(benchmark::state.rows.size() == benchmark_frames && benchmark::state.frame_count == benchmark_frames,
          "each successful outer Render SR adds one row despite the nested export");
      const auto& row = benchmark::state.rows.back();
      const auto& sample = row.frame;
      Check(row.wrapper_finalized && sample.sr_success_ns >= nested_sr_return_ns && sample.delivery == delivery
                && sample.source_sequence == benchmark_frames && sample.source_handle == reinterpret_cast<uintptr_t>(handle)
                && sample.source_generation == native::features.at(handle).source_generation
                && sample.input_width == RENDER_SIZE && sample.input_height == RENDER_SIZE
                && sample.output_width == SR_SIZE && sample.output_height == SR_SIZE,
            "the outer successful SR finalizes the actual source and input/output dimensions");
      Check(sample.recording != nullptr && sample.recording->list == recording.first
                && sample.recording->generation == recording.second && sample.recording->completed_healthy,
            "the source proves GPU completion on its exact recording");
      const bool reused = delivery == Delivery::kReuse;
      const auto calls = (nr_enabled && !reused ? 1u : 0u);
      Check(result.model_calls == calls && result.swapped_color == nr_enabled && sample.nr_enabled == nr_enabled,
          "the source receives the Render stand-in on enabled frames and the original Color when off");
      if (!nr_enabled) {
        Check(sample.stage == benchmark::Stage::kUnobserved && sample.passes_recorded == 0 && !row.timing.valid,
            "NR Off retains source proof without an NR timing or stage claim");
        return;
      }
      const auto& pass = sample.passes[0];
      Check(sample.stage == benchmark::Stage::kRender && sample.reuse_requested == reuse_enabled
                && sample.pre_sr_color_bound && sample.original_color_restored && sample.original_selection_recorded == reused
                && sample.passes_recorded == 1 && pass.width == RENDER_SIZE && pass.height == RENDER_SIZE
                && pass.calls == calls && pass.accepted == calls,
            "Render evidence identifies working size, actual calls, original selection and restored binding");
      Check(row.timing.valid && row.timing.sequence == result.timer_sequence
                && std::strcmp(row.timing.path, "preSR") == 0 && row.timing.recorded_ns < sample.sr_success_ns
                && (row.timing.passes[0].eval_us == 0) == reused,
            "the successful outer source owns the corresponding completed Pre-SR GPU timing");
    };
    measure(Delivery::kOff, false, false);
    measure(Delivery::kPerFrame, true, false);
    measure(Delivery::kRefresh, true, true);
    measure(Delivery::kReuse, true, true);
    measure(Delivery::kPerFrame, true, false);
    frame({.sr_result = NVSDK_NGX_Result_FAIL_InvalidParameter, .nested_cpp = true});
    {
      std::shared_lock lock(benchmark::state.mutex);
      Check(benchmark::state.frame_count == benchmark_frames && benchmark::state.sources.size() == 1
                && benchmark::state.sources[0].attempts == benchmark_frames + 1 && benchmark::state.sources[0].failures == 1
                && benchmark::state.rows.back().frame.sr_success_ns == 0,
            "an outer SR failure excludes the provisional Render row despite successful inner SR");
    }
    Check(benchmark::Stop("fixture_complete", nullptr, true), "stop the Render source benchmark");
    benchmark::Tick(timers::NowNs(), false);
    benchmark::write_pending = false;
    timers::benchmark_observer = nullptr;
    std::cout << "PASS Render sr_benchmark_v1 source, timing, Color and crossover evidence\n";
#endif

    {
      native::RecreateFeatures();
      model_parameter_observer = {};
      native::direct_api.evaluate = nr_edit_reuse_gpu_cases::ConstantModel;
      native::edit_reuse_enabled = false;
      native::nr_adaptive_enabled = true;
      native::nr_adaptive_selected_levels = 2;
      std::shared_ptr<addon::adaptive_resolution::Session> adaptive;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 24; ++attempt) {
        frame({});
        adaptive = native::features.at(handle).adaptive_session;
        if (adaptive != nullptr && adaptive->TargetReady() && adaptive->applied_generation == adaptive->status.generation) break;
      }
      Check(adaptive != nullptr && adaptive->TargetReady() && adaptive->ladder.count == 2
                && adaptive->status.applied == adaptive->status.upper
                && adaptive->applied_generation == adaptive->status.generation,
            "Render confirms the upper tier through a successful original SR and tracked WARP submission");
      const auto upper_generation = adaptive->applied_generation;
      const auto upper_applied_ns = adaptive->applied_ns;
      adaptive->Request(1, native::SteadyNowNs());
      const auto low_request = adaptive->status.generation;
      adaptive->next_measure_ns = 0;
      const auto rejected = frame({.sr_result = NVSDK_NGX_Result_FAIL_InvalidParameter});
      Check(rejected.swapped_color && rejected.adaptive_waiting_for_submit && adaptive->TargetReady()
                && rejected.adaptive_receipt != nullptr
                && *rejected.adaptive_receipt == addon::adaptive_resolution::DeliveryReceipt::REJECTED
                && adaptive->status.generation == low_request && adaptive->applied_generation == upper_generation
                && adaptive->applied_ns == upper_applied_ns && adaptive->gpu_count == 0,
            "rejected original SR consumes and restores the stand-in binding without confirming the requested lower tier");
      adaptive->next_measure_ns = 0;
      const auto accepted = frame({});
      Check(accepted.swapped_color && accepted.adaptive_waiting_for_submit && model_resets.back() == 1
                && accepted.timer_awaited_delivery && accepted.adaptive_receipt != nullptr
                && *accepted.adaptive_receipt == addon::adaptive_resolution::DeliveryReceipt::DELIVERED
                && adaptive->applied_generation == low_request && adaptive->status.applied == adaptive->ladder.sizes[1]
                && adaptive->applied_ns > upper_applied_ns,
            "the next successful SR resets locally and the exact borrowed Execute confirms the complete lower tier");
      const auto confirmed_at = adaptive->applied_ns;
      frame({});
      Check(adaptive->applied_ns == confirmed_at, "another successful Render frame does not restart the warmup");
      native::nr_adaptive_enabled = false;
      native::nr_adaptive_selected_levels = 0;
      std::cout << "ADAPTIVE_DELIVERY_RENDER evidence=WARP+MOCK rejected_sr_color_restored=1 recovered_reset=1\n";
    }

    // Keep the real SR consumer and Color restoration checks for the two-Pass chain.
    model_parameter_observer = {};
    native::RecreateFeatures();
    native::edit_reuse_enabled = true;
    native::stack_passes = 2;
    native::chained_temporal_history = true;
    native::pass_stage[0] = 0;
    native::preset = 1;
    native::intensity = .5f;
    native::pass_preset[0] = 2;
    native::pass_follow[0] = false;
    native::pass_intensity[0] = .75f;
    native::pass_transfer_strength[0] = 1.f;
    native::pass_color_strength[0] = 1.f;
    native::pass_resolution_mode[0] = 2;
    native::pass_resolution_scale[0] = .75f;
    native::detail_stability = 1;
    native::direct_api.evaluate = same_stage_resolution_cases::Evaluate;
    auto& render_calls = same_stage_resolution_cases::evaluations;
    same_stage_resolution_cases::stamps = {0x3900, 0x3b00};
    const auto render_frame = [&](const char* label, const FrameInput& input, uint64_t expected_calls, bool delivered) {
      render_calls.clear();
      auto result = frame(input);
      std::cout << "OBS Render multipass " << label << " calls=" << result.model_calls
                << " sr_consumed_edit=" << result.swapped_color << " fence=" << gpu.serial << '\n';
      Check(result.model_calls == expected_calls && render_calls.size() == expected_calls
                && result.swapped_color == delivered,
            "Render records all models or no models and delivers only the complete group to SR");
      return result;
    };
    const auto warm_pair = [&](const std::array<uint32_t, 2>* sizes = nullptr) {
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8; ++attempt) {
        render_calls.clear();
        const auto result = frame({.reset = 1});
        if (!result.swapped_color || result.model_calls != 2) continue;
        if (sizes == nullptr
            || (render_calls[0].input.width == (*sizes)[0] && render_calls[1].input.width == (*sizes)[1])) return;
      }
      Check(false, "Render two-Pass chain matures");
    };
    constexpr std::array<uint32_t, 2> pass_sizes{RENDER_SIZE, 12};
    warm_pair(&pass_sizes);
    const auto& render_slots = native::features.at(handle).slots;
    const auto group_anchor = render_slots[0].edit_reuse;
    Check(group_anchor != nullptr && group_anchor->valid, "Render warm-up establishes a terminal anchor");
    const auto generation_before_refresh = group_anchor->anchor_generation;
    std::vector<uint64_t> generation_at_evaluate;
    ComPtr<ID3D12DebugCommandList> debug_list;
    gpu.list.As(&debug_list);
    model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter* parameters) {
      generation_at_evaluate.push_back(group_anchor->anchor_generation);
      if (debug_list != nullptr) {
        auto* proxy = native::GetD3D12Resource(parameters, "DLSSNR.Color");
        auto* output = native::GetD3D12Resource(parameters, "DLSSNR.Output");
        Check(debug_list->AssertResourceState(proxy, 0, NATIVE_INPUT_STATE)
                  && debug_list->AssertResourceState(output, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
              "each Render model evaluates with SRV proxy and UAV output");
      }
    };
    const auto group_refresh = render_frame("refresh", {.reset = 1}, 2, true);
    model_parameter_observer = {};
    Check(group_refresh.model_calls_before_sr == 2 && generation_at_evaluate.size() == 2,
          "both Render models execute in order before original SR");
    for (size_t pass = 0; pass < 2; ++pass) {
      const auto& call = render_calls[pass];
      Check(call.handle == render_slots[pass].handle && call.preset == pass + 1
                && call.input.width == pass_sizes[pass] && call.input.height == pass_sizes[pass]
                && call.intensity == (pass == 0 ? .5f : .75f) && call.reset == 1,
            "Render refresh uses each logical Pass's Feature, model request, size, strength and Reset");
    }
    Check(render_slots[0].edit_reuse == group_anchor && group_anchor->valid && group_anchor->width == RENDER_SIZE
              && render_slots[1].edit_reuse == nullptr && !render_slots[0].look_history.valid
              && !render_slots[1].look_history.valid,
          "a spatial Render group has one terminal-color anchor at the full stage size");
    const auto group_generation = group_anchor->anchor_generation;
    std::cout << "OBS Render terminal generations before=" << generation_before_refresh
              << " at_models=" << generation_at_evaluate[0] << '/' << generation_at_evaluate[1]
              << " after=" << group_generation << '\n';
    Check(generation_at_evaluate == std::vector<uint64_t>(2, generation_before_refresh)
              && group_generation == generation_before_refresh + 1,
          "Render records exactly one terminal capture after both model evaluations");
    constexpr size_t center = 8 * RENDER_SIZE + 8;
    const auto check_terminal_pixel = [&](const ColorPixel& sr_pixel) {
      // Layout: log2 RGB edit and validity, followed by linear reference RGB and depth.
      std::array<float, 8> pixel{};
      auto* anchor = group_anchor->anchor.Get();
      const size_t offset = center * sizeof(pixel);
      auto readback = gpu.Buffer(sizeof(pixel), D3D12_HEAP_TYPE_READBACK);
      gpu.Transition(anchor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      gpu.list->CopyBufferRegion(readback.Get(), 0, anchor, offset, sizeof(pixel));
      gpu.Transition(anchor, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      gpu.Submit();
      const D3D12_RANGE range{0, sizeof(pixel)};
      void* mapped = nullptr;
      Check(SUCCEEDED(readback->Map(0, &range, &mapped)), "map terminal edit readback after the WARP fence");
      std::memcpy(pixel.data(), mapped, sizeof(pixel));
      const D3D12_RANGE no_writes{0, 0};
      readback->Unmap(0, &no_writes);
      Check(pixel[3] == 1.f && pixel[4] == .25f && pixel[5] == .375f && pixel[6] == .5f,
            "terminal edit references original linear Color");
      for (size_t channel = 0; channel < 3; ++channel) {
        const auto packed = DirectX::PackedVector::XMConvertHalfToFloat(sr_pixel[channel]);
        Check(std::abs(pixel[4 + channel] * std::exp2(pixel[channel]) - packed) < .0005f,
              "terminal edit reconstructs packed Color actually consumed by SR");
      }
      std::cout << "OBS Render terminal sentinel_R=" << pixel[4] * std::exp2(pixel[0])
                << " packed_SR_R=" << red(sr_pixel) << '\n';
    };
    check_terminal_pixel(group_refresh.sr_tile[center]);
    const auto total_reuses = native::edit_reuse_reuses.load();
    const auto group_reuse = render_frame("reuse", {}, 0, true);
    std::cout << "OBS Render multipass output_R=" << red(group_refresh.sr_tile[center]) << '/'
              << red(group_reuse.sr_tile[center]) << " anchor_generation=" << group_generation << '\n';
    Check(std::abs(red(group_refresh.sr_tile[center]) - .25f) > .05f
              && std::abs(red(group_reuse.sr_tile[center]) - red(group_refresh.sr_tile[center])) < .005f
              && group_anchor->anchor_generation == group_generation && native::edit_reuse_reuses.load() == total_reuses + 1,
          "zero-model Render reuse gives SR a nonneutral edit, with this frame's exact alpha");
    const auto settled = render_frame("refresh-after-reuse", {}, 2, true);
    Check(render_calls[0].reset == 0 && render_calls[1].reset == 0
              && render_calls[0].motion != motion.Get() && render_calls[1].motion != motion.Get(),
          "each Render Feature composes motion since its own actual Evaluate without an extra Reset");
    // The terminal anchor includes Transfer. Its live setting has no global Reset effect.
    native::kSetPassTransfer[0].Store(.625f);
    native::ApplySettingEffect(native::kSetPassTransfer[0].effect);
    const auto new_transfer = render_frame("tail-transfer-change", {}, 2, true);
    Check(std::abs(red(new_transfer.sr_tile[center]) - red(settled.sr_tile[center])) > .01f,
          "changing only Pass 2's transfer refreshes the terminal anchor with a visible GPU change");
    check_terminal_pixel(new_transfer.sr_tile[center]);
    const auto new_reuse = render_frame("reuse-after-transfer-change", {}, 0, true);
    Check(std::abs(red(new_reuse.sr_tile[center]) - red(new_transfer.sr_tile[center])) < .005f,
          "Render reuse delivers the new group edit after a transfer change");

    const auto generation_before_rejection = group_anchor->anchor_generation;
    const auto reuses_before_rejection = native::edit_reuse_reuses.load();
    render_frame("original-sr-failure", {.reset = 1, .sr_result = NVSDK_NGX_Result_FAIL_InvalidParameter}, 2, true);
    const auto& rejected_delivery = native::features.at(handle).edit_reuse_monitor[0];
    Check(group_anchor->anchor_generation == generation_before_rejection + 1 && !group_anchor->valid
              && rejected_delivery.delivery == native::EditReuseDelivery::NONE
              && rejected_delivery.reason == native::EditReuseBlock::SR_FAILED,
          "failed original SR invalidates the provisional terminal capture before publishing delivery");
    render_frame("refresh-after-sr-failure", {}, 2, true);
    Check(render_calls[0].reset == 1 && render_calls[1].reset == 1
              && native::edit_reuse_reuses.load() == reuses_before_rejection,
          "the next Render frame refreshes both models with recovery Reset after SR rejects the anchor");

    const auto generation_before_failure = group_anchor->anchor_generation;
    fail_call = model_calls + 2;
    const auto failed_tail = render_frame("second-model-failure", {.reset = 1}, 2, false);
    fail_call = 0;
    const auto& failed_render_delivery = native::features.at(handle).edit_reuse_monitor[0];
    const auto commit_slots = (uint64_t{1} << timers::kSlotCommitEnd) | (uint64_t{1} << timers::kSlotCopyBackEnd);
    Check(failed_tail.timer_sequence != 0 && (failed_tail.timer_written & commit_slots) == 0
              && group_anchor->anchor_generation == generation_before_failure
              && failed_render_delivery.delivery == native::EditReuseDelivery::ORIGINAL
              && failed_render_delivery.reason == native::EditReuseBlock::NR_FAILED
              && (render_slots[0].edit_reuse == nullptr || !render_slots[0].edit_reuse->valid),
          "a failed Render tail records no terminal capture, packed commit or stand-in copy");
    const auto reuses_after_failure = native::edit_reuse_reuses.load();
    render_calls.clear();
    const auto retry = frame({});
    std::cout << "OBS Render multipass failure-retry calls=" << retry.model_calls
              << " sr_consumed_edit=" << retry.swapped_color << " fence=" << gpu.serial << '\n';
    Check(native::edit_reuse_reuses.load() == reuses_after_failure,
          "ordinary Render retry cannot reuse the failed group anchor");

    native::pass_resolution_mode[0] = 0;
    native::detail_stability = 2;
    auto& tail_look = native::pass_look_result[0];
    tail_look.follow = false;
    tail_look.detail_stability = 1;
    constexpr std::array<uint32_t, 2> full_sizes{RENDER_SIZE, RENDER_SIZE};
    constexpr std::array<uint64_t, 4> cadence{2, 0, 2, 0};
    std::array<std::vector<uint8_t>, cadence.size()> full_reference;
    for (const bool change_dormant_pass : {false, true}) {
      native::RecreateFeatures();
      warm_pair(&full_sizes);
      std::array<uint64_t, 2> generations{};
      uint32_t look_written = render_slots[0].look_history.written;
      for (uint32_t sample = 0; sample < cadence.size(); ++sample) {
        if (change_dormant_pass && sample == 1) {
          native::pass_transfer_strength[1] = 0.f;
          native::pass_color_strength[1] = 0.f;
          native::pass_resolution_mode[1] = 2;
          native::pass_resolution_scale[1] = .75f;
          native::pass_look_result[1].follow = false;
          native::pass_look_result[1].detail_stability = 2;
        }
        const auto stabilized = native::look_stability_passes.load();
        auto result = render_frame("full-grid head Look", {.reset = (sample == 0 ? 1 : 0), .alpha_phase = sample},
                                   cadence[sample], true);
        Check(result.model_calls_before_sr == cadence[sample] && std::abs(red(result.sr_tile[center]) - .25f) > .05f,
              "original SR consumes the complete edited Render group on each real frame");
        const bool refreshed = cadence[sample] == 2;
        for (size_t pass = 0; pass < generations.size(); ++pass) {
          const auto& slot = render_slots[pass];
          const auto& cache = slot.edit_reuse;
          Check(cache != nullptr && cache->valid && cache->reused == !refreshed
                    && cache->width == RENDER_SIZE && cache->height == RENDER_SIZE
                    && cache->source_sequence == native::features.at(handle).sr_sequence,
                "full-grid Render keeps an anchor per layer at the current SR sequence");
          if (refreshed) {
            Check(render_calls[pass].handle == slot.handle && render_calls[pass].preset == pass + 1
                      && render_calls[pass].input.width == RENDER_SIZE && render_calls[pass].input.height == RENDER_SIZE
                      && render_calls[pass].reset == (sample == 0 ? 1 : 0),
                  "full-grid Render refresh evaluates A then B with the original size and Reset");
          }
          if (sample == 0) {
            generations[pass] = cache->anchor_generation;
          } else {
            generations[pass] += (refreshed ? 1u : 0u);
            Check(cache->anchor_generation == generations[pass], "only a model refresh advances a Render layer's anchor");
          }
        }
        const auto& history = render_slots[0].look_history;
        Check(render_slots[0].edit_reuse != render_slots[1].edit_reuse && history.valid && history.written != look_written
                  && !render_slots[1].look_history.valid && render_slots[1].look_history.buffers[0] == nullptr
                  && native::look_stability_passes.load() == stabilized + 1 && render_slots[2].handle == nullptr,
              "only the enabled Render head Look advances, including on a zero-model frame");
        look_written = history.written;
        if (change_dormant_pass) {
          Check(result.sr_rgba == full_reference[sample], "dormant Pass 3 settings preserve every SR output byte");
        } else {
          full_reference[sample] = std::move(result.sr_rgba);
        }
      }
    }
    render_frame("layered original SR failure", {.reset = 1, .sr_result = NVSDK_NGX_Result_FAIL_InvalidParameter}, 2, true);
    const auto& rejected_layers = native::features.at(handle).edit_reuse_monitor[0];
    Check(rejected_layers.delivery == native::EditReuseDelivery::NONE && rejected_layers.reason == native::EditReuseBlock::SR_FAILED
              && render_slots[0].edit_reuse != nullptr && !render_slots[0].edit_reuse->valid
              && render_slots[1].edit_reuse != nullptr && !render_slots[1].edit_reuse->valid,
          "rejected original SR invalidates both provisional layer anchors");
    render_frame("layered refresh after SR failure", {}, 2, true);
    Check(render_calls[0].reset == 1 && render_calls[1].reset == 1,
          "both Render layers reset on the first frame after SR rejects their anchors");
    std::cout << "PASS Render A-to-B full-grid head Look: model_calls=2/0/2/0, "
                 "per-layer anchors, exact dormant Pass 3 RGBA and original SR recovery; evidence=WARP+MOCK\n";

    {
      native::RecreateFeatures();
      tail_look.detail_stability = 2;
      warm_pair(&full_sizes);
      render_frame("both-temporal-refresh", {.reset = 1}, 2, true);
      const std::array anchors{render_slots[0].edit_reuse, render_slots[1].edit_reuse};
      std::array<uint64_t, 2> generations{};
      std::array<uint32_t, 2> look_written{};
      for (size_t pass = 0; pass < anchors.size(); ++pass) {
        const auto& slot = render_slots[pass];
        Check(render_calls[pass].handle == slot.handle && anchors[pass] != nullptr
                  && anchors[pass]->valid && slot.look_history.valid,
              "each temporal Render layer owns its edit anchor and Look history");
        generations[pass] = anchors[pass]->anchor_generation;
        look_written[pass] = slot.look_history.written;
      }
      const auto stabilized = native::look_stability_passes.load();
      render_frame("both-temporal-reuse", {}, 0, true);
      for (size_t pass = 0; pass < anchors.size(); ++pass) {
        const auto& slot = render_slots[pass];
        Check(slot.edit_reuse == anchors[pass] && anchors[pass]->anchor_generation == generations[pass]
                  && anchors[pass]->valid && anchors[pass]->reused
                  && slot.look_history.valid && slot.look_history.written != look_written[pass],
              "temporal Render reuse advances each Look history with its own retained edit anchor");
        std::cout << "OBS Render both-temporal pass=" << pass + 1 << " anchor_generation="
                  << generations[pass] << '/' << anchors[pass]->anchor_generation
                  << " history_written=" << look_written[pass] << '/' << slot.look_history.written << '\n';
      }
      Check(native::look_stability_passes.load() == stabilized + 2,
            "zero-model Render reuse executes both enabled temporal Look histories");
      std::cout << "PASS Render both-temporal Look reuse: model_calls=2/0, "
                   "both anchor generations unchanged, both Look histories advanced; evidence=WARP+MOCK\n";
      tail_look.detail_stability = 1;
    }

    native::look_upsample = addon::look::kUpsampleClassic;
    native::pass_resolution_mode[0] = 2;
    const std::array split_sizes{pass_sizes, std::array<uint32_t, 2>{12, RENDER_SIZE}};
    constexpr std::array<std::array<uint16_t, 2>, 3> split_stamps{{
        {0x3900, 0x3b00}, {0x3a00, 0x3b00}, {0x3a00, 0x38c0}}};
    struct SplitFrame {
      std::array<std::vector<uint8_t>, 2> inputs;
      std::vector<uint8_t> sr_rgba;
    };
    for (size_t temporal_pass = 0; temporal_pass < split_sizes.size(); ++temporal_pass) {
      const auto& sizes = split_sizes[temporal_pass];
      native::nr_resolution_scale = static_cast<float>(sizes[0]) / RENDER_SIZE;
      native::pass_resolution_scale[0] = static_cast<float>(sizes[1]) / RENDER_SIZE;
      std::cout << "OBS Render split grids=" << sizes[0] << '/' << sizes[1]
                << " temporal_pass=" << temporal_pass + 1 << '\n';
      native::detail_stability = (temporal_pass == 0 ? 2u : 1u);
      tail_look.detail_stability = (temporal_pass == 1 ? 2u : 1u);
      std::array<SplitFrame, split_stamps.size()> reference;
      for (const bool x2 : {false, true}) {
        native::RecreateFeatures();
        native::edit_reuse_enabled = x2;
        same_stage_resolution_cases::stamps = split_stamps[0];
        warm_pair(&sizes);
        const auto refreshes = native::edit_reuse_refreshes.load();
        const auto reuses = native::edit_reuse_reuses.load();
        uint32_t look_written = render_slots[temporal_pass].look_history.written;
        for (uint32_t sample = 0; sample < split_stamps.size(); ++sample) {
          same_stage_resolution_cases::stamps = split_stamps[sample];
          auto result = render_frame("split temporal", {.reset = (sample == 0 ? 1 : 0), .alpha_phase = sample}, 2, true);
          Check(result.model_calls_before_sr == 2, "split Render gives original SR both layers on every frame");
          for (size_t pass = 0; pass < sizes.size(); ++pass) {
            const auto& call = render_calls[pass];
            Check(call.handle == render_slots[pass].handle && call.preset == pass + 1
                      && call.input.width == sizes[pass] && call.input.height == sizes[pass]
                      && call.output.width == sizes[pass] && call.output.height == sizes[pass],
                  "split Render evaluates A then B on their resolved working grids");
          }
          const auto& history = render_slots[temporal_pass].look_history;
          Check(history.valid && history.width == sizes[temporal_pass] && history.height == sizes[temporal_pass]
                    && history.written != look_written && render_slots[1 - temporal_pass].look_history.buffers[0] == nullptr
                    && render_slots[0].edit_reuse == nullptr && render_slots[1].edit_reuse == nullptr
                    && native::edit_reuse_refreshes.load() == refreshes && native::edit_reuse_reuses.load() == reuses,
                "split Render advances only the selected Look history without publishing an anchor");
          look_written = history.written;
          std::array inputs{render_calls[0].input.Pixels(), render_calls[1].input.Pixels()};
          if (x2) {
            const auto& delivery = native::features.at(handle).edit_reuse_monitor[0];
            Check(delivery.delivery == native::EditReuseDelivery::PER_FRAME && delivery.reason == native::EditReuseBlock::GEOMETRY,
                  "split Render reports a geometry fallback with original per-frame delivery");
            Check(inputs == reference[sample].inputs && result.sr_rgba == reference[sample].sr_rgba,
                  "split Render x2 fallback matches both model inputs and full SR RGBA with x2 off");
          } else {
            reference[sample] = {.inputs = std::move(inputs), .sr_rgba = std::move(result.sr_rgba)};
          }
        }
      }
      std::cout << "PASS Render split temporal Look x2 fallback: grids=" << sizes[0] << '/' << sizes[1]
                << " temporal_pass=" << temporal_pass + 1
                << ", both model inputs and full SR RGBA match x2 off; evidence=WARP+MOCK\n";
    }

    native::RecreateFeatures();
    native::nr_resolution_scale = 1.f;
    native::pass_resolution_mode[0] = 0;
    native::look_upsample = addon::look::kUpsampleAuto;
    native::pass_stage[0] = 2;
    native::detail_stability = 1;
    tail_look.follow = true;
    same_stage_resolution_cases::stamps = split_stamps[0];
    native::gpu_timers_enabled = false;
    native::edit_reuse_enabled = false;
    warm_pair();
    const auto mixed_per_frame = render_frame("mixed-per-frame", {.reset = 1}, 2, true);
    const auto mixed_render_grid = render_calls[0].input;
    const auto mixed_upscaled_grid = render_calls[1].input;
    native::edit_reuse_enabled = true;
    const auto mixed_reuses = native::edit_reuse_reuses.load();
    const auto mixed_first = render_frame("mixed-refresh", {.reset = 1}, 2, true);
    const auto mixed_next = render_frame("mixed-next", {}, 2, true);
    std::cout << "OBS mixed cross-stage models before SR with x2 off/on=" << mixed_per_frame.model_calls_before_sr
              << '/' << mixed_first.model_calls_before_sr << '/' << mixed_next.model_calls_before_sr
              << " grids=" << render_calls[0].input.width << 'x' << render_calls[0].input.height << '/'
              << render_calls[1].input.width << 'x' << render_calls[1].input.height << '\n';
    Check(mixed_per_frame.model_calls_before_sr == 1 && mixed_first.model_calls_before_sr == 1
              && mixed_next.model_calls_before_sr == 1,
          "each mixed frame Evaluate sequence places one model on each side of original SR");
    Check(render_calls[0].input.width == mixed_render_grid.width && render_calls[0].input.height == mixed_render_grid.height
              && render_calls[1].input.width == mixed_upscaled_grid.width
              && render_calls[1].input.height == mixed_upscaled_grid.height,
          "x2 leaves the original mixed-stage working grids in effect");
    Check(native::edit_reuse_reuses.load() == mixed_reuses, "mixed-stage frames keep their per-frame cadence");
    std::cout << "PASS two-Pass Render x2 evidence=WARP+MOCK cadence=2/0/2/0, working grids=16x16/12x12, "
                 "terminal GPU pixels, temporal layer history, tail and original SR failure recovery\n";

    model_parameter_observer = {};
    native::ngx_slot_real[0].evaluate = nullptr;
    native::ForgetSourceHandleLocked(handle);
    native::FlushRetiredResources();
    native::direct_device->Release();
    native::direct_device = nullptr;
    model_gpu = nullptr;
    if (info != nullptr) {
      const auto messages = info->GetNumStoredMessages();
      for (UINT64 index = 0; index < messages; ++index) {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(index, message, &bytes)), "Render reuse debug message");
        Check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
      }
      std::cout << "OBS Render D3D12 messages=" << messages << " errors=0\n";
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
