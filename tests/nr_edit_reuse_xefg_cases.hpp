/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <DirectXPackedVector.h>
#include <functional>
#include <iostream>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include <present_frame_policy.hpp>
#include <utils/hash.hpp>

// Included after the shared XeFG adapter host and the NR model substitutes.
namespace nr_edit_reuse_xefg_cases {
namespace addon = renodx::addons::dlss5;
namespace policy = native::present_frame_policy;

using PresentPixel = std::array<uint16_t, 4>;
constexpr int32_t UI_TEXTURE = 3;
constexpr std::array<PresentPixel, 4> PRESENT_SOURCE_PIXELS{{
    {0x3400, 0x3600, 0x3800, 0x3900}, {0x3400, 0x3600, 0x3800, 0x3800},
    {0x3400, 0x3600, 0x3800, 0x3a00}, {0x3400, 0x3600, 0x3800, 0x3b00}}};

enum class PresentTrace { BASELINE, GENERATED_OUTPUT, REPEATED_CONSUMPTION };

struct PresentTraceFrame {
  uint32_t evaluates = 0;
  int reset = -1;
  uint64_t sequence = 0;
  uint64_t history_sequence = 0;
  uint64_t anchor_sequence = 0;
  uint64_t anchor_generation = 0;
  uint64_t anchor_age = 0;
  uint64_t reuses = 0;
  bool complete = false;
  std::vector<uint8_t> rgba;

  bool operator==(const PresentTraceFrame&) const = default;
};

inline auto PresentClockState(const policy::Clock& clock) {
  const auto& frame = clock.frame;
  return std::tuple{clock.claims, clock.outputs, clock.complete, frame.source, frame.generation, frame.sequence,
                    frame.output_epoch, frame.pass_begin, frame.pass_count, frame.roles, frame.reuse_edits};
}

inline void CheckPresentTraceFrame(const PresentTraceFrame& frame, uint32_t index, const char* route) {
  constexpr uint32_t WIDTH = 16;
  const bool refresh = index % 2 == 0;
  Check(frame.complete && frame.evaluates == (refresh ? 1u : 0u)
            && frame.reset == (refresh ? (index == 0 ? 1 : 0) : -1)
            && frame.sequence == index + 1 && frame.history_sequence == index + 1
            && frame.anchor_sequence == (index / 2) * 2 + 1 && frame.anchor_generation == index / 2 + 1
            && frame.anchor_age == index % 2 && frame.reuses == (refresh ? 0u : 1u),
        "Present baseline delivers one complete BB per real base with exact model, Reset and history cadence");
  Check(frame.rgba.size() == WIDTH * WIDTH * sizeof(PresentPixel), "Present reads back the entire scRGB output");
  const auto& original = PRESENT_SOURCE_PIXELS[index];
  for (size_t offset = 0; offset < frame.rgba.size(); offset += sizeof(original)) {
    PresentPixel pixel;
    std::memcpy(pixel.data(), frame.rgba.data() + offset, sizeof(pixel));
    Check(pixel[3] == original[3], "Present output preserves every pixel's current alpha");
  }
  std::cout << "PRESENT_POLICY_BASE route=" << route << " base=" << index + 1 << " calls=" << frame.evaluates
            << " reset=" << frame.reset << " history=" << frame.history_sequence
            << " anchor_base=" << frame.anchor_sequence << " anchor_generation=" << frame.anchor_generation
            << " anchor_age=" << frame.anchor_age << " complete=" << frame.complete
            << " rgba_crc32=" << renodx::utils::hash::ComputeCRC32(frame.rgba.data(), frame.rgba.size()) << '\n';
}

inline std::vector<uint8_t> ConsumeGeneratedOutput(Gpu* gpu, ID3D12Resource* delivered) {
  // The downstream stand-in emits a separate surface from the first delivery.
  // Reading it on the presentation queue proves an extra GPU consumption.
  const auto desc = delivered->GetDesc();
  auto generated = gpu->Texture(static_cast<uint32_t>(desc.Width), desc.Height, desc.Format);
  auto readback = gpu->CreateTextureTransfer(generated.Get());
  gpu->Transition(delivered, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  gpu->Transition(generated.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
  gpu->list->CopyResource(generated.Get(), delivered);
  gpu->Transition(generated.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  gpu->RecordTextureReadback(generated.Get(), readback);
  gpu->Transition(delivered, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
  gpu->Submit();
  return gpu->ReadTexture(readback);
}

static std::function<void(IDXGISwapChain*)> consume_sdk_present;
static HRESULT sdk_present_result = S_OK;

HRESULT STDMETHODCALLTYPE ConsumeSdkPresent(IDXGISwapChain* swapchain, UINT interval, UINT flags) {
  if (consume_sdk_present) consume_sdk_present(swapchain);
  FakePresent(swapchain, interval, flags);
  return sdk_present_result;
}

NVSDK_NGX_Result NVSDK_CONV EvaluatePresentEdit(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
  const auto game_list = model_gpu->list;
  model_gpu->list = list;
  const auto restore = [&](int*) { model_gpu->list = game_list; };
  int key = 0;
  const std::unique_ptr<int, decltype(restore)> restore_list(&key, restore);
  return nr_edit_reuse_gpu_cases::ConstantModel(list, handle, parameters, callback);
}

static std::vector<uint16_t> model_pixels;

NVSDK_NGX_Result NVSDK_CONV EditedModel(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
  const auto result = EvaluateModel(list, handle, parameters, callback);
  if (NVSDK_NGX_FAILED(result)) return result;
  auto* output = native::GetD3D12Resource(parameters, "DLSSNR.Output");
  const auto desc = output->GetDesc();
  Check(desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && model_pixels.size() == desc.Width * desc.Height * 4,
        "PQ mock writes encoded FP16 model-domain pixels");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  uint64_t bytes = 0;
  model_gpu->device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto upload = model_gpu->Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
  uint8_t* mapped = nullptr;
  Check(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "PQ mock upload map");
  for (uint32_t y = 0; y < desc.Height; ++y) {
    std::memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch,
                model_pixels.data() + y * desc.Width * 4, static_cast<size_t>(desc.Width) * 8);
  }
  upload->Unmap(0, nullptr);
  native::Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION from{.pResource = upload.Get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
  D3D12_TEXTURE_COPY_LOCATION to{.pResource = output, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  native::Transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  model_gpu->uploads.push_back(upload);
  return result;
}

int Run() {
  ComPtr<ID3D12InfoQueue> info;
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    Check(renodx::utils::directx::Initialize(), "initialize edit reuse XeFG DirectX entry points");
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
    }
    constexpr uint32_t width = 16, height = 16;
    Gpu gpu(false, width, height);
    model_gpu = &gpu;
    gpu.device.As(&info);
    model_calls = 0;
    model_resets.clear();
    model_evaluated_handles.clear();
    model_released_handles.clear();
    fail_call = 0;
    native::enabled = true;
    native::monitor_requested = true;
    native::hooks_enabled = true;
    native::shutdown_quiesce_enabled = false;
    native::hook_point = native::kHookPresent;
    native::present_pre_fg = 1;
    native::present_guides = native::kPresentGuidesRequired;
    native::present_ui_correction = false;
    native::OnNativeFgOptions(false);
    native::stack_passes = 1;
    native::pass_stage[0] = 0;
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
    native::telemetry_seconds = 0;
    native::gpu_timers_enabled = false;
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
    // Supply the existing model substitute as the explicitly private pre-FG
    // provider. The real loader and NVIDIA runtime are outside this fixture.
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
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "edit reuse XeFG queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "edit reuse XeFG list hooks");
    const auto upload = [&](ID3D12Resource* resource, const auto& values,
                            D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, UINT row_bytes) {
      auto transfer = gpu.CreateTextureTransfer(resource);
      if (resource->GetDesc().Format == DXGI_FORMAT_R32G8X24_TYPELESS) {
        // The shared transfer copies subresource 0: one float per depth pixel,
        // without the separate stencil plane or the resource's packed size.
        Check(transfer.footprint.Footprint.Depth == 1
                  && transfer.row_bytes == resource->GetDesc().Width * sizeof(float),
              "XeFG planar depth upload uses the four-byte plane-zero footprint");
      }
      gpu.Transition(resource, before, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.UploadTexture(resource, transfer, reinterpret_cast<const uint8_t*>(values.data()),
                        values.size() * sizeof(values[0]), row_bytes);
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, after);
    };
    adapter::modules[0].complete = true;
    adapter::modules[0].real[0] = reinterpret_cast<void*>(&FakeInit);
    adapter::modules[0].real[3] = reinterpret_cast<void*>(&FakeResource);
    adapter::modules[0].real[4] = reinterpret_cast<void*>(&FakeConstants);
    adapter::modules[0].real[5] = reinterpret_cast<void*>(&FakeId);
    adapter::modules[0].real[7] = reinterpret_cast<void*>(+[](void*, int32_t) { return sdk_status; });
    adapter::proxies[0].real[0] = reinterpret_cast<void*>(&ConsumeSdkPresent);
    adapter::proxies[0].real[1] = reinterpret_cast<void*>(&FakePresent1);
    xefg::Configure(&native::fg_service::Service, &native::fg_service::PrepareCapture,
                    &native::fg_service::Retire, &native::fg_service::Requested);
    bool sdk_failure_recovered = false;
    bool composed_group_published = false;
    for (const bool pq : {false, true}) {
      native::direct_api.evaluate = (pq ? EditedModel : nr_edit_reuse_gpu_cases::NeutralModel);
      native::codec_mode = (pq ? 3u : 0u);
      native::present_white_nits = (pq ? 0.f : 200.f);
      native::transfer_mode = 0;
      native::style_highlight_guard = 0.f;
      native::chroma_clamp_stops = 2.f;
      native::diffuse_white_nits = 200.f;
      const uint32_t guide_width = (pq ? width / 2 : width);
      const uint32_t guide_height = (pq ? height / 2 : height);
      auto depth = gpu.Texture(guide_width, guide_height,
          pq ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT);
      auto motion = gpu.Texture(guide_width, guide_height, DXGI_FORMAT_R16G16_FLOAT);
      upload(depth.Get(), std::vector<float>(guide_width * guide_height, .5f), D3D12_RESOURCE_STATE_COMMON,
             NATIVE_INPUT_STATE, guide_width * sizeof(float));
      upload(motion.Get(), std::vector<uint16_t>(guide_width * guide_height * 2, 0), D3D12_RESOURCE_STATE_COMMON,
             NATIVE_INPUT_STATE, guide_width * 4);
      gpu.Submit();
      Session session(&gpu, pq ? 1 : 3, 0, pq ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT);
      Check(Hooks::UiState(session.handle, 0) == 0, "start XeFG without a required HUDLess role");
      session.context->color_space = (pq ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
      session.extent_left = session.extent_top = 0;
      session.extent_width = width;
      session.extent_height = height;
      const auto context = reinterpret_cast<uintptr_t>(session.handle);
      const auto wait_service = [&] {
        const auto found = native::fg_service::streams.find({false, context});
        if (found == native::fg_service::streams.end() || found->second.submitted == 0) return;
        const auto& stream = found->second;
        if (stream.fence->GetCompletedValue() < stream.submitted) {
          HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
          Check(event != nullptr, "edit reuse XeFG completion event");
          const auto armed = stream.fence->SetEventOnCompletion(stream.submitted, event);
          const auto waited = (SUCCEEDED(armed) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
          CloseHandle(event);
          Check(waited == WAIT_OBJECT_0, "bounded edit reuse XeFG service completion");
        }
        Check(stream.fence->GetCompletedValue() >= stream.submitted
                  && SUCCEEDED(gpu.device->GetDeviceRemovedReason()), "healthy XeFG service Fence");
      };
      const auto read_buffer = [&](ID3D12Resource* resource) {
        const auto bytes = static_cast<size_t>(resource->GetDesc().Width);
        auto readback = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
        gpu.Transition(resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.list->CopyBufferRegion(readback.Get(), 0, resource, 0, bytes);
        gpu.Transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu.Submit();
        const D3D12_RANGE range{0, bytes};
        uint8_t* mapped = nullptr;
        Check(SUCCEEDED(readback->Map(0, &range, reinterpret_cast<void**>(&mapped))), "XeFG anchor readback map");
        std::vector<uint8_t> pixels(mapped, mapped + bytes);
        const D3D12_RANGE no_writes{0, 0};
        readback->Unmap(0, &no_writes);
        return pixels;
      };
      const auto read_output = [&](ID3D12Resource* backbuffer) {
        auto readback = gpu.CreateTextureTransfer(backbuffer);
        gpu.Transition(backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.RecordTextureReadback(backbuffer, readback);
        gpu.Transition(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        gpu.Submit();
        return gpu.ReadTexture(readback);
      };
      std::vector<uint16_t> source(width * height * 4);
      std::vector<uint32_t> source_pq(width * height);
      // Independent double-precision ST.2084 and D65 primary transforms. These
      // reference absolute display light, without calling the production codec.
      const auto pq_decode = [](double code) {
        const double p = std::pow(code, 32.0 / 2523.0);
        return std::pow(std::max(p - 3424.0 / 4096.0, 0.0) / (2413.0 / 128.0 - (2392.0 / 128.0) * p), 16384.0 / 2610.0);
      };
      const auto pq_encode = [](double light) {
        const double p = std::pow(std::clamp(light, 0.0, 1.0), 2610.0 / 16384.0);
        return std::pow((3424.0 / 4096.0 + (2413.0 / 128.0) * p) / (1.0 + (2392.0 / 128.0) * p), 2523.0 / 32.0);
      };
      const auto linear_709 = [&](uint32_t packed) {
        const double r = pq_decode((packed & 1023u) / 1023.0);
        const double g = pq_decode(((packed >> 10) & 1023u) / 1023.0);
        const double b = pq_decode(((packed >> 20) & 1023u) / 1023.0);
        return std::array<double, 3>{1.6604910 * r - .5876411 * g - .0728499 * b,
            -.1245505 * r + 1.1328999 * g - .0083494 * b, -.0181508 * r - .1005789 * g + 1.1187297 * b};
      };
      const auto srgb_encode = [](double light) {
        light = std::clamp(light, 0.0, 1.0);
        return (light <= .0031308 ? 12.92 * light : 1.055 * std::pow(light, 1.0 / 2.4) - .055);
      };
      constexpr std::array<double, 3> gains{1.125, 1.25, 1.375};
      const auto source_matches = [&](const std::vector<uint8_t>& pixels) {
        const size_t bytes = (pq ? source_pq.size() * sizeof(uint32_t) : source.size() * sizeof(uint16_t));
        const void* current = (pq ? static_cast<const void*>(source_pq.data()) : static_cast<const void*>(source.data()));
        return pixels.size() == bytes && std::memcmp(pixels.data(), current, bytes) == 0;
      };
      uint32_t source_frame = 0;
      uint32_t present_id = 0;
      const PresentPixel* trace_color = nullptr;
      struct Observation {
        fg::Result result = fg::Result::NOT_SERVED;
        uint32_t evaluates = 0;
        uint64_t reuses = 0;
        int reset = -1;
        std::vector<uint8_t> pixels;
      };
      enum class Change { NONE, MOTION, DEPTH, COLOR };
      const auto frame = [&](bool reset, bool guides = true, Change change = Change::NONE,
                             ID3D12Resource* hudless = nullptr, bool unsubmitted_guides = false,
                             ID3D12Resource* ui = nullptr) {
        ++source_frame;
        ++present_id;
        ComPtr<ID3D12Resource> backbuffer;
        Check(SUCCEEDED(session.proxy->GetBuffer(session.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer))),
              "edit reuse XeFG actual proxy Backbuffer");
        for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
          source[pixel * 4] = DirectX::PackedVector::XMConvertFloatToHalf(.25f + source_frame / 4096.f);
          source[pixel * 4 + 1] = DirectX::PackedVector::XMConvertFloatToHalf(.375f + (pixel % width) / 4096.f);
          source[pixel * 4 + 2] = DirectX::PackedVector::XMConvertFloatToHalf(.5f);
          source[pixel * 4 + 3] = DirectX::PackedVector::XMConvertFloatToHalf(.25f + .125f * ((pixel + source_frame) % 4));
          if (trace_color != nullptr) std::ranges::copy(*trace_color, source.begin() + pixel * 4);
          const uint32_t rgb = (pixel % width < 2 ? (1u << 10) | (2u << 20)
              : (change == Change::COLOR ? 610u | (650u << 10) | (680u << 20) : 460u | (500u << 10) | (530u << 20)));
          source_pq[pixel] = rgb | (((pixel + source_frame) % 4) << 30);
        }
        if (pq) {
          model_pixels.resize(width * height * 4);
          for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
            auto proxy = linear_709(source_pq[pixel]);
            for (auto& channel : proxy) {
              channel = std::max(channel, 0.0) * 10000.0 / native::diffuse_white_nits.load();
            }
            const double peak = std::max({proxy[0], proxy[1], proxy[2]});
            const double toe = peak + .014 * peak / (peak + .001);
            const double shoulder = (toe > .75 ? .75 + .25 * (1.0 - std::exp(-5.770780 * (toe - .75))) : toe);
            for (uint32_t channel = 0; channel < 3; ++channel) {
              model_pixels[pixel * 4 + channel] = DirectX::PackedVector::XMConvertFloatToHalf(
                  static_cast<float>(srgb_encode(proxy[channel] * shoulder / std::max(peak, 1e-6) * gains[channel])));
            }
            model_pixels[pixel * 4 + 3] = DirectX::PackedVector::XMConvertFloatToHalf(1.f);
          }
          upload(backbuffer.Get(), source_pq, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, width * 4);
        } else {
          upload(backbuffer.Get(), source, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, width * 8);
        }
        upload(depth.Get(), std::vector<float>(guide_width * guide_height, change == Change::DEPTH ? .9f : .5f),
               NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, guide_width * 4);
        std::vector<uint16_t> motion_values(guide_width * guide_height * 2, 0);
        if (change == Change::MOTION) {
          for (size_t pixel = 0; pixel < motion_values.size() / 2; ++pixel) {
            motion_values[pixel * 2] = DirectX::PackedVector::XMConvertFloatToHalf(32.f);
          }
        }
        upload(motion.Get(), motion_values, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, guide_width * 4);
        gpu.Submit();
        if (guides) {
          for (auto [type, resource] : {std::pair{DEPTH, depth.Get()}, std::pair{MOTION, motion.Get()}}) {
            const xefg::abi::Resource tag{.type = type, .validity = (unsubmitted_guides ? ONLY_NOW : UNTIL_NEXT_PRESENT),
                .base = {0, 0}, .size = {guide_width, guide_height}, .resource = resource, .state = NATIVE_INPUT_STATE};
            Check(Hooks::Resource(session.handle, gpu.list.Get(), present_id, &tag) == 0, "current XeFG guide tag");
            if (unsubmitted_guides) {
              const auto& copy = session.context->frames.at(present_id)->resources[type].copy;
              std::vector<std::pair<addon::FenceRef, uint64_t>> receipts;
              Check(copy != nullptr && addon::submission::UseSubmissionTokens(copy->texture.Get(), &receipts)
                        == addon::submission::UseSubmission::kUnsubmitted,
                    "ONLY_NOW guide has an actual copy on the still-unsubmitted producer list");
            }
          }
        }
        if (hudless != nullptr) session.Tag(present_id, HUDLESS, hudless);
        if (ui != nullptr) session.Tag(present_id, UI_TEXTURE, ui);
        const xefg::abi::Constants constants{.jitter_x = (trace_color != nullptr ? .25f : 0.f),
            .jitter_y = (trace_color != nullptr ? -.25f : 0.f), .motion_scale_x = 1.f, .motion_scale_y = 1.f,
            .reset = (reset ? 1u : 0u), .frame_time_ms = 1000.f / 60.f};
        Check(Hooks::Constants(session.handle, present_id, &constants) == 0, "current XeFG frame constants");
        const auto before_calls = model_calls;
        const auto before_reuses = native::edit_reuse_reuses.load();
        Check(Hooks::PresentId(session.handle, present_id) == 0, "current XeFG Present ID");
        Check(adapter::ProxyHooks<0>::Present(session.proxy.Get(), 0, 0) == sdk_present_result,
              "XeFG proxy preserves the original SDK Present result");
        if (unsubmitted_guides) {
          // Discard the unordered guide copy before the readback submits this list.
          Check(SUCCEEDED(gpu.list->Close()) && SUCCEEDED(gpu.allocator->Reset())
                    && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
                "discard the unsubmitted XeFG guide capture without executing it");
        }
        wait_service();
        Observation observed{.result = xefg::CurrentResult(), .evaluates = model_calls - before_calls,
            .reuses = native::edit_reuse_reuses.load() - before_reuses,
            .reset = (model_calls != before_calls ? model_resets.back() : -1)};
        observed.pixels = read_output(backbuffer.Get());
        native::DrainRetiredResources(true);
        return observed;
      };
      const auto check_pq_output = [&](const Observation& observed, bool exact_dark) {
        Check(observed.pixels.size() == source_pq.size() * sizeof(uint32_t), "PQ service returns packed HDR10 pixels");
        bool changed = false;
        for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
          uint32_t actual = 0;
          std::memcpy(&actual, observed.pixels.data() + pixel * sizeof(uint32_t), sizeof(actual));
          Check((actual >> 30) == (source_pq[pixel] >> 30), "PQ refresh and reuse preserve every current two-bit alpha code");
          if (pixel % width < 2) {
            if (exact_dark) {
              Check(actual == source_pq[pixel], "near-black PQ reuse selects current original RGB and alpha exactly");
            }
            continue;
          }
          auto light = linear_709(source_pq[pixel]);
          for (uint32_t channel = 0; channel < 3; ++channel) {
            light[channel] *= gains[channel];
          }
          const std::array<double, 3> target{.6274040 * light[0] + .3292820 * light[1] + .0433136 * light[2],
              .0690970 * light[0] + .9195400 * light[1] + .0113612 * light[2],
              .0163916 * light[0] + .0880132 * light[1] + .8955950 * light[2]};
          for (uint32_t channel = 0; channel < 3; ++channel) {
            const int expected = static_cast<int>(std::lround(pq_encode(target[channel]) * 1023.0));
            const int code = static_cast<int>((actual >> (channel * 10)) & 1023u);
            Check(std::abs(code - expected) <= 2, "non-neutral PQ edit matches independent linear-light and primary reference within two codes");
          }
          changed |= (actual & 0x3fffffffu) != (source_pq[pixel] & 0x3fffffffu);
        }
        Check(changed, "PQ service exercises a valid non-neutral edit rather than only selecting original pixels");
      };
      bool mature = false;
      for (uint32_t prefix = 0; prefix < native::kContractDebounceFrames + 12 && !mature; ++prefix) {
        const auto observed = frame(true);
        mature = observed.result == fg::Result::SUBMITTED && observed.evaluates == 1;
        if (!pq && observed.result == fg::Result::PREPARED) {
          const auto& clock = native::fg_service::streams.at({false, context}).base_frame_clock;
          Check(clock.outputs == 0 && !clock.complete && source_matches(observed.pixels),
                "prepared-only XeFG work cannot publish a complete BB group or alter the source image");
        }
      }
      Check(mature, "single Backbuffer XeFG cache submits a mock-model refresh");
      auto& stream = native::fg_service::streams.at({false, context});
      const auto owner = stream.roles[0].Handle();
      if (!pq) {
        native::direct_api.evaluate = EvaluatePresentEdit;
        std::array<PresentTraceFrame, 4> baseline;
        for (const auto variant : {PresentTrace::BASELINE, PresentTrace::GENERATED_OUTPUT,
                                   PresentTrace::REPEATED_CONSUMPTION}) {
          const uint64_t sequence_prefix = session.context->application_frame;
          const uint64_t claims_prefix = stream.base_frame_clock.claims;
          const uint64_t outputs_prefix = stream.base_frame_clock.outputs;
          const uint64_t presents_prefix = fake_present_calls;
          const uint64_t history_prefix = native::game_window.frames;
          uint64_t first_anchor_generation = 0;
          uint32_t downstream_outputs = 0;
          for (uint32_t index = 0; index < baseline.size(); ++index) {
            const bool refresh = index % 2 == 0;
            const auto submitted_before = stream.submitted;
            const bool generated = variant == PresentTrace::GENERATED_OUTPUT && index == 0;
            std::vector<uint8_t> sdk_rgba;
            consume_sdk_present = [&](IDXGISwapChain* swapchain) {
              const auto& clock = stream.base_frame_clock;
              Check(stream.submitted == submitted_before + 1 && clock.outputs == outputs_prefix + index + 1
                        && clock.complete,
                    "XeFG publishes an ordered whole group and submits its private list before the original SDK Present");
              ComPtr<IDXGISwapChain3> sdk_swapchain;
              ComPtr<ID3D12Resource> input;
              Check(SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&sdk_swapchain)))
                        && SUCCEEDED(sdk_swapchain->GetBuffer(sdk_swapchain->GetCurrentBackBufferIndex(),
                                                              IID_PPV_ARGS(&input))),
                    "original XeFG SDK Present receives the current proxy Backbuffer");
              sdk_rgba = read_output(input.Get());
              ++downstream_outputs;
              if (generated) {
                const auto sdk_base = PresentClockState(clock);
                const auto model_count = model_calls;
                const auto game_frames = native::game_window.frames;
                Check(ConsumeGeneratedOutput(&gpu, input.Get()) == sdk_rgba,
                      "XeFG downstream consumes one additional generated stand-in surface in GPU order");
                ++downstream_outputs;
                Check(PresentClockState(clock) == sdk_base && model_calls == model_count
                          && native::game_window.frames == game_frames,
                      "extra SDK output leaves XeFG claims, products, evaluations and game history unchanged");
              }
            };
            trace_color = &PRESENT_SOURCE_PIXELS[index];
            const auto observed = frame(index == 0);
            const auto& clock = stream.base_frame_clock;
            const auto& slot = native::features.at(owner).slots[0];
            const auto& anchor = slot.edit_reuse;
            const auto& delivered = stream.edit_reuse_monitor[0];
            Check(sdk_rgba == observed.pixels && observed.result == fg::Result::SUBMITTED && anchor != nullptr
                      && anchor->valid && slot.nr_preset == 2 && !slot.look_history.valid
                      && !native::features.contains(stream.roles[1].Handle()),
                  "XeFG SDK sees the actual single edited BB on the PRESENT stage with Look disabled");
            Check(clock.claims == claims_prefix + index + 1 && clock.outputs == outputs_prefix + index + 1
                      && clock.frame.source == context && clock.frame.generation == session.context->epoch
                      && clock.frame.pass_begin == 0 && clock.frame.pass_count == 1 && clock.frame.roles == 1
                      && clock.frame.reuse_edits && stream.last_output_frame[0] == clock.frame.sequence
                      && native::game_window.frames == history_prefix + index + 1,
                  "XeFG recognizes one real application input and commits one complete BB product each frame");
            if (index == 0) first_anchor_generation = anchor->anchor_generation;
            PresentTraceFrame product{.evaluates = observed.evaluates, .reset = observed.reset,
                .sequence = clock.frame.sequence - sequence_prefix,
                .history_sequence = anchor->source_sequence - sequence_prefix,
                .anchor_sequence = stream.last_model_frame[0] - sequence_prefix,
                .anchor_generation = anchor->anchor_generation - first_anchor_generation + 1,
                .anchor_age = anchor->source_sequence - stream.last_model_frame[0], .reuses = observed.reuses,
                .complete = clock.complete && delivered.handed_off && delivered.sequence == clock.frame.sequence
                    && delivered.stage == addon::stage_plan::Stage::PRESENT
                    && delivered.delivery == (refresh ? native::EditReuseDelivery::REFRESH : native::EditReuseDelivery::REUSED),
                .rgba = observed.pixels};
            if (variant == PresentTrace::REPEATED_CONSUMPTION && index == 1) {
              const auto before = PresentClockState(clock);
              const auto model_frame = stream.last_model_frame[0];
              const auto output_frame = stream.last_output_frame[0];
              const auto anchor_state = std::tuple{anchor->source_sequence, anchor->anchor_generation, anchor->reuse_count};
              const auto calls = model_calls;
              const auto history = native::game_window.frames;
              const auto presents = fake_present_calls;
              const auto submits = stream.submitted;
              session.Present(present_id);
              Check(fake_present_calls == presents + 1 && sdk_rgba == product.rgba && PresentClockState(clock) == before
                        && session.context->application_frame == clock.frame.sequence && stream.submitted == submits
                        && model_calls == calls && native::game_window.frames == history
                        && stream.last_model_frame[0] == model_frame && stream.last_output_frame[0] == output_frame
                        && std::tuple{anchor->source_sequence, anchor->anchor_generation, anchor->reuse_count} == anchor_state,
                    "XeFG repeats the same Present ID at the SDK and delivers the same BB without changing NR history");
            }
            consume_sdk_present = {};
            if (variant == PresentTrace::BASELINE) {
              CheckPresentTraceFrame(product, index, "xefg");
              Check(!source_matches(product.rgba), "the ordered XeFG output contains a nonzero scRGB edit");
              baseline[index] = std::move(product);
            } else {
              Check(product == baseline[index], "XeFG G/R base trace must equal A including complete ordered RGBA");
            }
          }
          const bool generated = variant == PresentTrace::GENERATED_OUTPUT;
          const bool repeated = variant == PresentTrace::REPEATED_CONSUMPTION;
          Check(downstream_outputs == 4 + generated + repeated && fake_present_calls == presents_prefix + 4 + repeated
                    && session.context->application_frame == sequence_prefix + 4,
                "XeFG G adds a downstream output without another outer application Present and R reaches the SDK once more");
          std::cout << "PRESENT_POLICY_CONSUMERS route=xefg generated=" << generated << " repeated=" << repeated
                    << " application_callbacks=" << fake_present_calls - presents_prefix
                    << " downstream_outputs=" << downstream_outputs << '\n';
        }
        trace_color = nullptr;
        native::direct_api.evaluate = nr_edit_reuse_gpu_cases::NeutralModel;
      }
      const auto reference_source_frame = source_frame;
      std::array<Observation, 5> reference;
      for (uint32_t index = 0; index < reference.size(); ++index) {
        reference[index] = frame(index == 0);
        Check(reference[index].result == fg::Result::SUBMITTED
                  && reference[index].evaluates == (index % 2 == 0 ? 1u : 0u)
                  && reference[index].reuses == (index % 2 == 0 ? 0u : 1u)
                  && reference[index].reset == (index == 0 ? 1 : (index % 2 == 0 ? 0 : -1)),
              "stable native Off establishes reference XeFG cadence 1/0/1/0/1");
      }
      // Replay only the deterministic input colors after the same Reset prefix;
      // real present IDs and application frames remain strictly increasing.
      source_frame = reference_source_frame;
      const auto baseline = frame(true);
      Check(baseline.evaluates == 1 && baseline.reuses == 0 && baseline.reset == 1,
            "explicit XeFG Reset refreshes the mock NR model");
      Check(baseline.pixels == reference[0].pixels, "matching Reset prefixes deliver identical XeFG reference RGBA");
      auto anchor = native::features.at(owner).slots[0].edit_reuse;
      Check(anchor != nullptr && anchor->valid, "XeFG single Backbuffer establishes an edit anchor");
      const auto contract = anchor->contract;
      const auto source_generation = native::features.at(owner).source_generation;
      const auto model = native::features.at(owner).slots[0].handle;
      const auto models = native::nr_features_created;
      const auto worksets = native::worksets_created;
      const auto reset_epoch = native::history_reset_epoch.load();
      const auto configuration = native::configuration_generation.load();
      const auto context_epoch = session.context->epoch;
      auto generation = anchor->anchor_generation;
      auto anchor_pixels = read_buffer(anchor->anchor.Get());
      if (pq) {
        check_pq_output(baseline, false);
      }
      const auto check_duplicate = [&](const Observation& observed) {
        const auto application_frame = session.context->application_frame;
        const auto calls = model_calls;
        const auto reuses = native::edit_reuse_reuses.load();
        const auto submissions = stream.submitted;
        const auto sequence = anchor->source_sequence;
        const auto reuse_count = anchor->reuse_count;
        const auto model_frame = stream.last_model_frame[0];
        const auto output_frame = stream.last_output_frame[0];
        session.Present(present_id);
        Check(session.context->application_frame == application_frame && stream.application_frame == application_frame
                  && model_calls == calls && native::edit_reuse_reuses.load() == reuses && stream.submitted == submissions
                  && anchor->anchor_generation == generation && anchor->source_sequence == sequence
                  && anchor->reuse_count == reuse_count && stream.last_model_frame[0] == model_frame
                  && stream.last_output_frame[0] == output_frame && read_buffer(anchor->anchor.Get()) == anchor_pixels,
              "same public presentID advances neither application cadence, model, reuse, anchor nor service submission");
        ComPtr<ID3D12Resource> current;
        Check(SUCCEEDED(session.proxy->GetBuffer(session.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&current))),
              "duplicate XeFG Present retains its actual proxy Backbuffer");
        Check(read_output(current.Get()) == observed.pixels, "duplicate presentID preserves the already delivered RGBA output");
      };
      native::OnNativeFgOptions(true);
      check_duplicate(baseline);
      // XeFG owns the color source throughout; these actual native callbacks
      // change only unrelated declarations and must not migrate its NR history.
      for (uint32_t index = 1; index < 5; ++index) {
        const bool native_on = index < 3;
        native::OnNativeFgOptions(native_on);
        const bool refresh = index % 2 == 0;
        const auto old_model_frame = stream.last_model_frame[0];
        const auto reuse_count = anchor->reuse_count;
        const auto observed = frame(false);
        std::cout << "OBS XeFG native-declaration format=" << (pq ? "PQ" : "scRGB")
                  << " frame=" << index + 1 << " native=" << (native_on ? "On" : "Off")
                  << " model_calls=" << observed.evaluates << " reuses=" << observed.reuses << '\n';
        Check(observed.result == fg::Result::SUBMITTED && observed.evaluates == (refresh ? 1u : 0u)
                  && observed.reuses == (refresh ? 0u : 1u) && observed.reset == (refresh ? 0 : -1),
              "unrelated native Off/On/Off preserves XeFG model cadence 1/0/1/0/1 without extra Reset");
        Check(!native::native_fg_owns.load() && xefg::Enabled() && xefg::OwnsStream()
                  && native::native_fg_declaration.load() == (native_on ? native::NativeFgDeclaration::kOn
                                                                        : native::NativeFgDeclaration::kOff)
                  && session.context->epoch == context_epoch && stream.epoch == context_epoch
                  && native::features.at(owner).source_generation == source_generation
                  && native::features.at(owner).slots[0].handle == model
                  && native::features.at(owner).slots[0].edit_reuse == anchor && anchor->contract == contract
                  && native::history_reset_epoch.load() == reset_epoch && native::configuration_generation.load() == configuration
                  && native::nr_features_created == models && native::worksets_created == worksets
                  && !native::features.contains(stream.roles[1].Handle()),
              "native declarations preserve XeFG source, role, contract, model, workset and actual ownership");
        if (refresh) {
          ++generation;
          anchor_pixels = read_buffer(anchor->anchor.Get());
          Check(stream.last_model_frame[0] == session.context->application_frame,
                "XeFG model frame advances only on refresh");
        } else {
          Check(anchor->reuse_count == reuse_count + 1 && read_buffer(anchor->anchor.Get()) == anchor_pixels
                    && stream.last_model_frame[0] == old_model_frame,
                "native declaration changes preserve exact XeFG anchor bytes on reuse");
        }
        Check(anchor->valid && anchor->anchor_generation == generation
                  && stream.last_output_frame[0] == session.context->application_frame,
              "every new XeFG source delivers output while only refresh advances its anchor generation");
        Check(observed.pixels == reference[index].pixels,
              "native declaration changes preserve exact XeFG RGBA against the same stable-declaration input sequence");
        if (pq) {
          check_pq_output(observed, !refresh);
        } else if (!refresh) {
          Check(source_matches(observed.pixels), "native declaration changes preserve exact current scRGB color and alpha");
        }
        check_duplicate(observed);
      }
      // Another source owns adaptive residency, so this Present stays on its
      // original fixed chain even though the menu requests adaptive sizing.
      Check(native::EnsureLookPipeline(gpu.device.Get()), "warm Look pipelines before requesting adaptive residency");
      const auto resident = std::make_shared<addon::adaptive_resolution::Session>();
      native::adaptive_resident_session = resident;
      native::nr_adaptive_enabled = true;
      const auto fixed = frame(false);
      const auto& feature = native::features.at(owner);
      Check(fixed.result == fg::Result::SUBMITTED && fixed.evaluates == 0 && fixed.reuses == 1
                && feature.adaptive_cache == nullptr && feature.slots[0].edit_reuse == anchor
                && anchor->anchor_generation == generation && read_buffer(anchor->anchor.Get()) == anchor_pixels,
            "an unacquired adaptive request preserves the fixed XeFG workset and reuse anchor");
      native::nr_adaptive_enabled = false;
      native::adaptive_resident_session.reset();
      ComPtr<ID3D12Resource> backbuffer;
      Check(SUCCEEDED(session.proxy->GetBuffer(session.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer))),
            "inspect XeFG Backbuffer ownership");
      for (const auto& binding : anchor->bindings) {
        for (const auto& resource : binding.resources) {
          Check(resource.Get() != backbuffer.Get(), "cache bindings retain private current-original surfaces rather than DXGI Backbuffer");
        }
      }
      const auto next = frame(false);
      Check(next.evaluates == 1 && next.reuses == 0 && next.reset == 0,
            "XeFG refreshes after exactly one reuse even when the dormant adaptive request is turned off again");
      const auto reset = frame(true);
      Check(reset.evaluates == 1 && reset.reuses == 0 && reset.reset == 1, "explicit Reset interrupts XeFG reuse cadence");
      anchor.reset();
      if (pq) {
        for (const auto change : {Change::MOTION, Change::DEPTH, Change::COLOR}) {
          Check(frame(true).evaluates == 1, "refresh PQ anchor before rejection case");
          const auto rejected = frame(false, true, change);
          Check(rejected.result == fg::Result::SUBMITTED && rejected.evaluates == 0 && rejected.reuses == 1
                    && source_matches(rejected.pixels),
                "motion bounds, depth and color rejection each preserve exact current packed PQ source");
        }
        native::direct_api.evaluate = nr_edit_reuse_gpu_cases::NeutralModel;
        Check(frame(true).evaluates == 1, "refresh neutral PQ anchor");
        const auto neutral = frame(false);
        Check(neutral.evaluates == 0 && neutral.reuses == 1 && source_matches(neutral.pixels),
              "neutral PQ reuse preserves current packed RGB and all alpha codes exactly");
        native::direct_api.evaluate = EditedModel;
        Check(frame(true).evaluates == 1, "refresh edited PQ anchor before white change");
        const auto old_contract = native::features.at(owner).slots[0].edit_reuse->contract;
        native::diffuse_white_nits = 240.f;
        const auto white_changed = frame(false);
        Check(white_changed.evaluates == 1 && white_changed.reuses == 0
                  && native::features.at(owner).slots[0].edit_reuse->contract != old_contract,
              "actual PQ white divisor change invalidates the anchor without relying on a configuration-generation bump");
        native::diffuse_white_nits = 200.f;
        Check(frame(true).evaluates == 1, "refresh PQ anchor before actual color-contract change");
        session.context->color_space = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        Check(frame(false).reuses == 0, "same packed output with a linear color-space contract cannot consume the PQ anchor");
        session.context->color_space = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        bool restored = false;
        for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 12 && !restored; ++attempt) {
          const auto observed = frame(true);
          Check(observed.reuses == 0, "restoring PQ requires an actual fresh model output");
          restored = observed.result == fg::Result::SUBMITTED && observed.evaluates == 1;
        }
        Check(restored, "PQ color contract recovers after a bounded model refresh");
      }
      const auto recover = [&](bool next_base = false) {
        const auto limit = (next_base ? 1u : native::kContractDebounceFrames + 12);
        for (uint32_t attempt = 0; attempt < limit; ++attempt) {
          const auto before = native::fg_service::streams.find({false, context});
          const auto before_products = (before != native::fg_service::streams.end() ? before->second.base_frame_clock.outputs : 0);
          const auto before_epoch = (before != native::fg_service::streams.end() ? before->second.epoch : 0);
          const auto observed = frame(false);
          Check(observed.reuses == 0, "interrupted XeFG source cannot reuse before a fresh model output");
          if (observed.result == fg::Result::SUBMITTED && observed.evaluates == 1) {
            const auto& recovered = native::fg_service::streams.at({false, context});
            const auto expected_outputs = (recovered.epoch == before_epoch ? before_products + 1 : 1);
            Check(observed.reset == 1 && recovered.epoch == session.context->epoch
                      && (pq || (recovered.base_frame_clock.complete && recovered.base_frame_clock.outputs == expected_outputs)),
                  "XeFG recovery sends Reset on its first actual model refresh and publishes a complete product");
            return;
          }
        }
        Check(false, "bounded XeFG source recovery reaches a fresh model output");
      };
      const auto missing = frame(false, false);
      Check(missing.result != fg::Result::SUBMITTED && missing.evaluates == 0 && missing.reuses == 0
                && source_matches(missing.pixels),
            "missing Required guides keep the current source and do not consume cached edits");
      recover();
      std::array<bool, 3> quarantine{};
      for (size_t index = 0; index < quarantine.size(); ++index) {
        quarantine[index] = stream.slots[index].quarantined;
        stream.slots[index].quarantined = true;
      }
      const auto skipped = frame(false);
      for (size_t index = 0; index < quarantine.size(); ++index) {
        stream.slots[index].quarantined = quarantine[index];
      }
      Check(skipped.result != fg::Result::SUBMITTED && skipped.evaluates == 0 && skipped.reuses == 0,
            "a busy XeFG recording ring skips one real base frame without consuming the anchor");
      recover();
      Check(frame(true).evaluates == 1, "establish XeFG refresh before injected model failure");
      fail_call = model_calls + 1;
      const auto failed = frame(true);
      fail_call = 0;
      Check(failed.result != fg::Result::SUBMITTED && failed.evaluates == 1 && failed.reuses == 0
                && source_matches(failed.pixels),
            "actual mock-model failure restores current XeFG source instead of cached output");
      recover();
      if (!pq) {
        const auto before_unordered = stream.base_frame_clock.outputs;
        const auto unordered = frame(false, true, Change::NONE, nullptr, true);
        Check(unordered.result != fg::Result::SUBMITTED && unordered.evaluates == 0 && unordered.reuses == 0
                  && stream.base_frame_clock.outputs == before_unordered && source_matches(unordered.pixels),
              "unordered XeFG capture does not publish a group and leaves the current source untouched");
        recover(true);

        native::direct_api.evaluate = EvaluatePresentEdit;
        const auto before_sdk_failure = stream.submitted;
        const auto before_sdk_submissions = xefg::SubmittedFrames();
        const auto before_sdk_history = native::game_window.frames;
        const auto before_sdk_products = stream.base_frame_clock.outputs;
        sdk_present_result = E_FAIL;
        const auto sdk_failed = frame(true);
        sdk_present_result = S_OK;
        const bool failed_delivery_cleared = sdk_failed.result == fg::Result::FAILED
            && stream.result == fg::Result::FAILED && !stream.base_frame_clock.complete
            && !stream.edit_reuse_monitor[0].handed_off && session.context->last_submission_frame == 0;
        Check(sdk_failed.evaluates == 1 && sdk_failed.reuses == 0 && !source_matches(sdk_failed.pixels)
                  && stream.submitted == before_sdk_failure + 1 && xefg::SubmittedFrames() == before_sdk_submissions + 1
                  && stream.fence->GetCompletedValue() >= stream.submitted
                  && stream.base_frame_clock.outputs == before_sdk_products + 1
                  && native::game_window.frames == before_sdk_history + 1,
              "SDK Present failure retains the completed private WARP work, pixels and execution history");
        const auto after_sdk_failure = frame(false);
        sdk_failure_recovered = failed_delivery_cleared && after_sdk_failure.result == fg::Result::SUBMITTED
            && after_sdk_failure.evaluates == 1 && after_sdk_failure.reuses == 0 && after_sdk_failure.reset == 1
            && stream.base_frame_clock.complete && stream.base_frame_clock.outputs == before_sdk_products + 2
            && native::game_window.frames == before_sdk_history + 2;
        std::cout << "XEFG_SDK_FAILURE cleared=" << failed_delivery_cleared << " recovered=" << sdk_failure_recovered
                  << " calls=" << sdk_failed.evaluates << '/' << after_sdk_failure.evaluates
                  << " reset=" << after_sdk_failure.reset << " completed_fence=" << stream.fence->GetCompletedValue()
                  << " failed_rgba_crc32=" << renodx::utils::hash::ComputeCRC32(sdk_failed.pixels.data(), sdk_failed.pixels.size())
                  << '\n';

        native::edit_reuse_enabled = false;
        native::nr_adaptive_enabled = true;
        native::nr_adaptive_selected_levels = 2;
        native::nr_adaptive_limited_residency = true;
        auto* const previous_status = session.context->get_status;
        session.context->get_status = +[](void*, xefg::abi::PresentStatus* status) -> int32_t {
          *status = {.frames_presented = 2, .frame_gen_result = 0, .enabled = 1};
          return 0;
        };
        for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 24; ++attempt) {
          frame(false);
          const auto& adaptive = stream.adaptive_session;
          if (adaptive != nullptr && adaptive->TargetReady() && adaptive->applied_generation == adaptive->status.generation) break;
        }
        const auto adaptive = stream.adaptive_session;
        Check(adaptive != nullptr && adaptive->TargetReady() && adaptive->ladder.count == 2
                  && adaptive->status.applied == adaptive->status.upper
                  && adaptive->applied_generation == adaptive->status.generation,
              "limited XeFG confirms the upper after the actual proxy accepts the ordered private group");

        const auto warm_output = [&] {
          const auto output = session.context->output_identity;
          Check(static_cast<bool>(output), "adaptive XeFG has a registered live proxy output");
          addon::output_framerate::Invalidate(output);
          const auto end = addon::output_framerate::NowNs() - 10'000'000;
          for (int bin = 4; bin >= 0; --bin) {
            const auto serial = ++session.context->present_serial;
            const auto sample_ns = end - bin * addon::output_framerate::Stream::BIN_NS;
            addon::output_framerate::ObserveXeFG(output, serial, sample_ns, true, true, 60, true, false);
          }
          Check(addon::output_framerate::Read(output, end).available, "complete current-output window before XeFG trial");
        };
        warm_output();
        frame(false);
        const auto upper_generation = adaptive->applied_generation;
        const auto upper_applied_ns = adaptive->applied_ns;
        adaptive->previous_level = 0;
        adaptive->trial = true;
        adaptive->Request(1, native::SteadyNowNs());
        const auto failed_generation = adaptive->status.generation;
        fg::FrameInput old_receipt;
        bool fault_reached = false;
        bool original_started_unconfirmed = false;
        consume_sdk_present = [&](IDXGISwapChain*) {
          const auto& pending = stream.pending_xefg_delivery;
          if (!pending.complete || pending.frame.generation != failed_generation
              || pending.actual != adaptive->ladder.sizes[1]) return;
          original_started_unconfirmed = adaptive->TargetReady() && adaptive->applied_generation == upper_generation
              && adaptive->applied_ns == upper_applied_ns;
          old_receipt.context = context;
          old_receipt.epoch = stream.epoch;
          old_receipt.application_frame = stream.application_frame;
          old_receipt.output = session.context->output_identity;
          fault_reached = true;
          sdk_present_result = E_FAIL;
        };
        for (uint32_t attempt = 0; attempt < 24 && !fault_reached; ++attempt) frame(false);
        consume_sdk_present = {};
        sdk_present_result = S_OK;
        Check(fault_reached && original_started_unconfirmed,
            "the complete ready lower tier remains pending as the original proxy starts consuming it");
        Check(fault_reached && stream.result == fg::Result::FAILED && !stream.base_frame_clock.complete
                  && adaptive->TargetReady() && adaptive->applied_generation == upper_generation
                  && adaptive->applied_ns == upper_applied_ns && adaptive->gpu_count == 0
                  && stream.fence->GetCompletedValue() >= stream.submitted
                  && !addon::output_framerate::Read(session.context->output_identity, addon::output_framerate::NowNs()).available,
              "outer E_FAIL rejects the low tier and its cost, preserves executed WARP resources, and withdraws its window");
        const auto fallback = frame(false);
        Check(fallback.result == fg::Result::SUBMITTED && fallback.reset == 1 && !adaptive->trial
                  && adaptive->status.generation != failed_generation
                  && adaptive->applied_generation == adaptive->status.generation
                  && adaptive->status.applied == adaptive->status.upper,
              "the next legal XeFG frame resets locally and the rejected output window sends the trial back to the upper anchor");
        const auto fallback_applied = adaptive->applied_ns;
        native::fg_service::OnXeFGPresentDelivery(old_receipt, true);
        Check(adaptive->applied_ns == fallback_applied && adaptive->status.applied == adaptive->status.upper,
              "late success for the withdrawn XeFG application frame cannot confirm the old trial");
        warm_output();
        frame(false);
        adaptive->Request(1, native::SteadyNowNs());
        const auto next_request = adaptive->status.generation;
        for (uint32_t attempt = 0; attempt < 24 && adaptive->applied_generation != next_request; ++attempt) frame(false);
        Check(adaptive->applied_generation == next_request && adaptive->status.applied == adaptive->ladder.sizes[1]
                  && adaptive->applied_ns > fallback_applied,
              "the next legal lower request confirms only after successful original proxy delivery");
        const auto confirmed_at = adaptive->applied_ns;
        session.Present(present_id);
        Check(adaptive->applied_ns == confirmed_at, "repeat XeFG consumption does not start a second warmup");

        const auto observe_steady_delivery = [&](HRESULT original_result) {
          namespace timers = addon::gpu_timers;
          warm_output();
          frame(false);
          adaptive->InvalidateSampling();
          adaptive->stable_after_ns = native::SteadyNowNs() - 1;
          adaptive->next_measure_ns = 0;
          timers::Segment* completed = nullptr;
          uint64_t sequence = 0;
          consume_sdk_present = [&](IDXGISwapChain*) {
            Check(adaptive->applied_generation == next_request && adaptive->applied_ns == confirmed_at,
                  "the original proxy is testing a steady already-confirmed tier");
            const auto& delivery = stream.pending_xefg_delivery;
            Check(delivery.complete && delivery.frame.delivery != nullptr,
                  "the steady adaptive GPU measurement belongs to the current whole-group delivery");
            wait_service();
            const addon::RuntimeLock lock(native::runtime_mutex);
            const auto slot = std::ranges::find_if(stream.slots, [&](const auto& entry) {
              return entry.fence_value == stream.submitted;
            });
            Check(slot != stream.slots.end() && SUCCEEDED(slot->allocator->Reset())
                      && SUCCEEDED(slot->list->Reset(slot->allocator, nullptr)),
                  "the exact completed private list is discarded against replay before polling inside the original SDK call");
            addon::submission::OnCommandListReset(slot->list);
            Check(SUCCEEDED(slot->list->Close()), "close the discarded XeFG fixture list");
            native::PruneCompletedSubmissionUses();
            const auto segment = std::ranges::find_if(timers::internal::segments, [&](const auto& entry) {
              return entry.recorded && entry.measure && entry.adaptive.delivery == delivery.frame.delivery;
            });
            Check(segment != std::end(timers::internal::segments) && segment->adaptive_stable
                      && segment->proof == timers::SampleState::VALID,
                  "steady XeFG NR has exact completed WARP timing before SDK return");
            completed = &*segment;
            sequence = segment->sequence;
            timers::Poll(true, false);
            Check(completed->recorded && completed->completed_ns != 0 && adaptive->gpu_count == 0
                      && timers::completed_stats[2].sequence != sequence,
                  "a GPU-completed steady frame awaiting SDK delivery cannot add cost or replace current dimensions");
            sdk_present_result = original_result;
          };
          const auto observed = frame(false);
          consume_sdk_present = {};
          sdk_present_result = S_OK;
          {
            const addon::RuntimeLock lock(native::runtime_mutex);
            timers::Poll(true, false);
          }
          Check(completed != nullptr && !completed->recorded && adaptive->applied_ns == confirmed_at,
                "the exact completed segment drains once after the matching final receipt");
          if (original_result == S_OK) {
            Check(observed.result == fg::Result::SUBMITTED && adaptive->gpu_count == 1
                      && timers::completed_stats[2].sequence == sequence,
                  "successful delivery admits the buffered cost and dimensions from the same completed GPU frame");
          } else {
            Check(observed.result == fg::Result::FAILED && adaptive->gpu_count == 0
                      && timers::completed_stats[2].sequence != sequence,
                  "rejected steady delivery contributes no cost or current dimensions despite completed GPU work");
          }
        };
        observe_steady_delivery(E_FAIL);
        observe_steady_delivery(S_OK);

        native::nr_adaptive_enabled = false;
        native::nr_adaptive_selected_levels = 0;
        native::nr_adaptive_limited_residency = false;
        native::edit_reuse_enabled = true;
        session.context->get_status = previous_status;
        recover();
        std::cout << "ADAPTIVE_DELIVERY_XEFG evidence=WARP+MOCK rejected_proxy=1 restored_anchor=1 late_receipt=ignored\n";

        native::direct_api.evaluate = nr_edit_reuse_gpu_cases::NeutralModel;

        const auto single_role = native::fg_service::streams.at({false, context}).base_frame_clock.frame;
        Check(Hooks::UiState(session.handle, 1) == 0 && session.context->epoch != single_role.generation,
              "real XeFG UI composition state changes the required color-role contract and its source epoch");
        const auto missing_role = frame(false);
        const auto missing_stream = native::fg_service::streams.find({false, context});
        const bool no_new_product = missing_stream == native::fg_service::streams.end()
            || (missing_stream->second.epoch == session.context->epoch && !missing_stream->second.base_frame_clock.complete
                && missing_stream->second.base_frame_clock.outputs == 0);
        Check(missing_role.result != fg::Result::SUBMITTED && missing_role.evaluates == 0 && missing_role.reuses == 0
                  && no_new_product && source_matches(missing_role.pixels),
              "a submitted BB from the previous single-color contract cannot serve the missing required HUDLess role");

        auto hudless = gpu.Texture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        upload(hudless.Get(), source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, width * 8);
        gpu.Submit();
        bool dual_ready = false;
        for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 12; ++attempt) {
          const auto dual = frame(false, true, Change::NONE, hudless.Get());
          const auto& current_stream = native::fg_service::streams.at({false, context});
          Check(current_stream.epoch == session.context->epoch, "dual-role observations belong to the current SDK epoch");
          if (dual.result == fg::Result::SUBMITTED) {
            const auto& clock = current_stream.base_frame_clock;
            Check(dual.evaluates == 2 && model_resets[model_resets.size() - 2] == 1 && dual.reset == 1
                      && dual.reuses == 0 && clock.complete && clock.frame.roles == 3
                      && clock.frame.generation == session.context->epoch && clock.frame.generation != single_role.generation
                      && current_stream.last_output_frame[0] == clock.frame.sequence && current_stream.last_output_frame[1] == clock.frame.sequence,
                  "supplying the required HUDLess causes real execution and complete ordered delivery of both roles");
            dual_ready = true;
            break;
          }
          Check(!current_stream.base_frame_clock.complete && source_matches(dual.pixels),
                "preparing the new color plan cannot publish a partial group or change the original BB");
        }
        Check(dual_ready, "dual-role XeFG finishes preparing its real model slots");
        // UiState retired the old node. This epoch's reference is used only
        // until the next UiState below; recovery queries the map again.
        auto& dual_stream = native::fg_service::streams.at({false, context});
        const auto dual_products = dual_stream.base_frame_clock.outputs;
        const auto dual_gap = frame(false);
        Check(dual_gap.result != fg::Result::SUBMITTED && dual_gap.evaluates == 0
                  && dual_stream.base_frame_clock.outputs == dual_products && source_matches(dual_gap.pixels),
              "missing a required role on an already mature dual-color chain cannot produce a partial group");
        const auto repaired = frame(false, true, Change::NONE, hudless.Get());
        Check(repaired.result == fg::Result::SUBMITTED && repaired.evaluates == 2 && repaired.reset == 1
                  && dual_stream.base_frame_clock.complete && dual_stream.base_frame_clock.outputs == dual_products + 1,
              "restoring the required HUDLess on the next legal frame refreshes and submits both roles");

        auto ui = gpu.Texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
        upload(ui.Get(), std::vector<uint32_t>(width * height, 0), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_COMMON, width * 4);
        gpu.Submit();
        const auto hudless_before = read_output(hudless.Get());
        const auto before_composition = dual_stream.base_frame_clock.outputs;
        const auto before_compose_submit = dual_stream.submitted;
        const auto before_compose_history = native::game_window.frames;
        const auto before_composed_frames = session.context->interval_nr_composed_frames.load();
        native::direct_api.evaluate = EvaluatePresentEdit;
        std::vector<uint8_t> sdk_bb, sdk_hudless;
        consume_sdk_present = [&](IDXGISwapChain*) {
          ComPtr<ID3D12Resource> backbuffer;
          Check(dual_stream.submitted == before_compose_submit + 1
                    && SUCCEEDED(session.proxy->GetBuffer(session.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer))),
                "original SDK Present receives the composed group after private submission");
          sdk_bb = read_output(backbuffer.Get());
          sdk_hudless = read_output(hudless.Get());
        };
        const auto composed = frame(false, true, Change::NONE, hudless.Get(), false, ui.Get());
        Check(composed.result == fg::Result::SUBMITTED && composed.evaluates == 1 && composed.reuses == 0
                  && session.context->interval_nr_composed_frames.load() == before_composed_frames + 1
                  && native::game_window.frames == before_compose_history + 1
                  && sdk_bb == composed.pixels && !source_matches(sdk_bb) && sdk_hudless != hudless_before,
              "mode 3 uses one HUDLess model and hands the real SDK both ordered edited colors");
        const auto composed_clock = PresentClockState(dual_stream.base_frame_clock);
        const auto composed_calls = model_calls;
        const auto composed_hudless = sdk_hudless;
        session.Present(present_id);
        consume_sdk_present = {};
        composed_group_published = dual_stream.base_frame_clock.complete && dual_stream.base_frame_clock.frame.roles == 3
            && dual_stream.base_frame_clock.outputs == before_composition + 1 && PresentClockState(dual_stream.base_frame_clock) == composed_clock
            && model_calls == composed_calls && native::game_window.frames == before_compose_history + 1
            && sdk_bb == composed.pixels && sdk_hudless == composed_hudless;
        std::cout << "XEFG_COMPOSED_GROUP complete=" << composed_group_published << " calls=" << composed.evaluates
                  << " bb_crc32=" << renodx::utils::hash::ComputeCRC32(sdk_bb.data(), sdk_bb.size())
                  << " hudless_crc32=" << renodx::utils::hash::ComputeCRC32(sdk_hudless.data(), sdk_hudless.size()) << '\n';
        native::direct_api.evaluate = nr_edit_reuse_gpu_cases::NeutralModel;
        Check(Hooks::UiState(session.handle, 0) == 0, "return to the original XeFG single BB role");
        recover();
      }
      const auto final_owner = native::fg_service::streams.at({false, context}).roles[0].Handle();
      auto final_anchor = native::features.at(final_owner).slots[0].edit_reuse;
      Check(final_anchor != nullptr, "recovered XeFG source owns a new edit cache");
      std::weak_ptr<addon::edit_reuse::State> retired_anchor = final_anchor;
      final_anchor.reset();
      Check(Hooks::Enable(session.handle, 0) == 0, "SDK Disable ends the XeFG source before owned retirement");
      native::fg_service::Retire();
      gpu.Submit();
      native::PruneCompletedSubmissionUses();
      native::DrainRetiredResources(true);
      Check(!native::fg_service::streams.contains({false, context}) && !native::features.contains(final_owner)
                && retired_anchor.expired(), "XeFG source retirement releases the cache after owned recordings and Fence completion");
    }
    Check(sdk_failure_recovered && composed_group_published,
          "XeFG revokes delivery after SDK failure and publishes both roles after one-model UI composition");
    model_pixels.clear();
    xefg::Configure(&Service, &Prepare, nullptr, &Requested);
    model_gpu = nullptr;
    if (info != nullptr) {
      for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(index, message, &bytes)), "edit reuse XeFG debug message");
        Check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
      }
    }
    std::cout << "PASS XeFG x2 single Backbuffer evidence=WARP+MOCK SR=MOCK NR=MOCK XeFG=MOCK"
                 " formats=scRGB/HDR10-PQ nonneutral_PQ=CPU_reference packed_RGBA=exact_on_rejection"
                 " depth=scRGB:R32_FLOAT/PQ:R32G8X24_TYPELESS_plane0"
                 " source_updates=per_base_frame native_declarations=Off/On/Off cadence=1/0/1/0/1"
                 " duplicate_present_id=stable_RGBA/anchor/model/reuse/submission anchor_bytes=immutable_on_reuse"
                 " scRGB_A/G/R=ordered_RGBA missing_HUDLess/unknown_order/recovery=checked"
                 " reset/guides/gap/failure/retirement=checked interpolation=not_executed\n";
    return 0;
  } catch (const std::exception& error) {
    consume_sdk_present = {};
    fail_call = 0;
    std::cerr << "nr-edit-reuse-xefg-contracts: " << error.what() << '\n';
    return 1;
  }
}

}  // namespace nr_edit_reuse_xefg_cases
