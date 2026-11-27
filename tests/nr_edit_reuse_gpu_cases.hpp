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
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include <edit_reuse.hpp>

#include "inline_execution_trace.hpp"
#include "nr_edit_reuse_adaptive_cases.hpp"

// Included after the native fixture's model substitutes and real GPU host.
namespace nr_edit_reuse_gpu_cases {
namespace addon = renodx::addons::dlss5;
namespace reuse = addon::edit_reuse;
namespace inline_trace = inline_execution_trace;

NVSDK_NGX_Result NVSDK_CONV CopySr(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback) {
  auto* color = native::GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Color);
  auto* output = native::GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Output);
  native::Transition(list, color, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  native::Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyResource(output, color);
  native::Transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  native::Transition(list, color, D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
  return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV MirrorSr(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback_C) {
  return native::HookedEvaluateFeatureSlot<0>(list, handle, parameters, nullptr);
}

#if defined(NR_NEXUS_DEV_CONTROL)
inline int64_t benchmark_inner_return_ns = 0;

NVSDK_NGX_Result NVSDK_CONV MirrorSrCpp(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
  const auto result = native::HookedEvaluateFeatureSlot<1>(list, handle, parameters, callback);
  benchmark_inner_return_ns = native::SteadyNowNs();
  return result;
}
#endif

NVSDK_NGX_Result NVSDK_CONV NeutralModel(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
  const auto result = EvaluateModel(list, handle, parameters, callback);
  if (NVSDK_NGX_FAILED(result)) return result;
  auto* proxy = native::GetD3D12Resource(parameters, "DLSSNR.Color");
  auto* output = native::GetD3D12Resource(parameters, "DLSSNR.Output");
  native::Transition(list, proxy, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  native::Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyResource(output, proxy);
  native::Transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  native::Transition(list, proxy, D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
  return result;
}

NVSDK_NGX_Result NVSDK_CONV ConstantModel(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
  const auto result = EvaluateModel(list, handle, parameters, callback);
  if (NVSDK_NGX_FAILED(result)) return result;
  auto* output = native::GetD3D12Resource(parameters, "DLSSNR.Output");
  const auto desc = output->GetDesc();
  Check(list == model_gpu->list.Get() && desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
        "constant model records encoded FP16 pixels into the current fixture list");
  std::array<uint16_t, 4> encoded;
  for (size_t channel = 0; channel < encoded.size(); ++channel) {
    encoded[channel] = DirectX::PackedVector::XMConvertFloatToHalf(inline_trace::MODEL_RGBA[channel]);
  }
  std::vector<uint16_t> pixels(static_cast<size_t>(desc.Width) * desc.Height * 4);
  for (size_t pixel = 0; pixel < pixels.size() / 4; ++pixel) {
    std::copy(encoded.begin(), encoded.end(), pixels.begin() + pixel * 4);
  }
  auto transfer = model_gpu->CreateTextureTransfer(output);
  native::Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  model_gpu->UploadTexture(output, transfer, reinterpret_cast<const uint8_t*>(pixels.data()),
      pixels.size() * sizeof(uint16_t), static_cast<UINT>(desc.Width) * 8);
  native::Transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  return result;
}

template <uint32_t NeutralPreset>
NVSDK_NGX_Result NVSDK_CONV OneEditedLayer(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
  const auto evaluate = (stage_execution_cases::presets.at(handle) == NeutralPreset ? NeutralModel : ConstantModel);
  return evaluate(list, handle, parameters, callback);
}

int Run() {
  ComPtr<ID3D12InfoQueue> info;
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    Check(renodx::utils::directx::Initialize(), "initialize shared DirectX entry points");
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu(false);
    model_gpu = &gpu;
    gpu.device.As(&info);
    std::cout << "Edit reuse D3D12 debug layer=" << (info != nullptr ? "available" : "unavailable") << std::endl;
    const auto submit_commands = [&](const char* phase) {
      try {
        gpu.Submit();
      } catch (...) {
        std::cerr << "Edit reuse submission failed: " << phase << " completed=" << gpu.serial << '\n';
        throw;
      }
    };
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "edit reuse queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "edit reuse list hooks");
    const auto uav_texture = [&](uint32_t width, uint32_t height, DXGI_FORMAT format) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = width, .Height = height, .DepthOrArraySize = 1, .MipLevels = 1, .Format = format,
          .SampleDesc = {1, 0}, .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
      ComPtr<ID3D12Resource> resource;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))), "edit reuse UAV texture");
      return resource;
    };
    const auto upload = [&](ID3D12Resource* resource, const auto& values, D3D12_RESOURCE_STATES before,
                            D3D12_RESOURCE_STATES after, uint32_t row_bytes) {
      auto transfer = gpu.CreateTextureTransfer(resource);
      gpu.Transition(resource, before, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.UploadTexture(resource, transfer, reinterpret_cast<const uint8_t*>(values.data()),
          values.size() * sizeof(values[0]), row_bytes);
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, after);
    };
    const auto read_buffer = [&](ID3D12Resource* resource) {
      const auto bytes = static_cast<size_t>(resource->GetDesc().Width);
      auto readback = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
      gpu.Transition(resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      gpu.list->CopyBufferRegion(readback.Get(), 0, resource, 0, bytes);
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      submit_commands("buffer readback");
      const D3D12_RANGE range{0, bytes};
      uint8_t* mapped = nullptr;
      Check(SUCCEEDED(readback->Map(0, &range, reinterpret_cast<void**>(&mapped))), "edit reuse buffer readback");
      std::vector<uint8_t> result(mapped, mapped + bytes);
      const D3D12_RANGE no_writes{0, 0};
      readback->Unmap(0, &no_writes);
      return result;
    };
    const auto fill_validity = [&](reuse::State* state) {
      const auto bytes = static_cast<size_t>(state->validity->GetDesc().Width);
      auto transfer = gpu.Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
      void* mapped = nullptr;
      const D3D12_RANGE no_reads{0, 0};
      Check(SUCCEEDED(transfer->Map(0, &no_reads, &mapped)), "validity sentinel upload map");
      std::memset(mapped, 0xA5, bytes);
      const D3D12_RANGE written{0, bytes};
      transfer->Unmap(0, &written);
      gpu.Transition(state->validity.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.list->CopyBufferRegion(state->validity.Get(), 0, transfer.Get(), 0, bytes);
      gpu.Transition(state->validity.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      gpu.uploads.push_back(transfer);
    };
    const auto encode = [](float linear) {
      return (linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.f / 2.4f) - 0.055f);
    };
    constexpr uint32_t width = 16, height = 8, guide_width = 8, guide_height = 4;
    auto proxy = gpu.Texture(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT);
    auto neural = uav_texture(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT);
    auto motion = gpu.Texture(guide_width, guide_height, DXGI_FORMAT_R32G32_FLOAT);
    auto depth = gpu.Texture(guide_width, guide_height, DXGI_FORMAT_R32_FLOAT);
    auto original = gpu.Texture(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT);
    auto decoded = uav_texture(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT);
    std::vector<float> proxy_values(width * height * 4);
    std::vector<float> neural_values(width * height * 4);
    std::vector<float> original_values(width * height * 4);
    std::vector<float> decoded_values(width * height * 4, -7.f);
    std::vector<float> motion_values(guide_width * guide_height * 2, 0.f);
    std::vector<float> depth_values(guide_width * guide_height, .5f);
    for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
      for (uint32_t channel = 0; channel < 3; ++channel) {
        const float reference = .125f + .0625f * channel;
        proxy_values[pixel * 4 + channel] = encode(reference);
        neural_values[pixel * 4 + channel] = encode(reference * (pixel % width < 8 ? 2.f : 1.5f));
        original_values[pixel * 4 + channel] = .03125f * (channel + 1) + static_cast<float>(pixel) / 1024.f;
      }
      proxy_values[pixel * 4 + 3] = .375f;
      neural_values[pixel * 4 + 3] = .875f;
      original_values[pixel * 4 + 3] = .25f + .03125f * (pixel % 4);
    }
    upload(proxy.Get(), proxy_values, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, width * 16);
    upload(neural.Get(), neural_values, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, width * 16);
    upload(motion.Get(), motion_values, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, guide_width * 8);
    upload(depth.Get(), depth_values, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, guide_width * 4);
    upload(original.Get(), original_values, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, width * 16);
    upload(decoded.Get(), decoded_values, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, width * 16);
    reuse::State state;
    const reuse::Inputs inputs{.width = width, .height = height, .motion_width = guide_width, .motion_height = guide_height,
        .proxy = proxy.Get(), .neural = neural.Get(), .motion = motion.Get(), .depth = depth.Get(),
        .original = original.Get(), .decoded = decoded.Get()};
    Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &state, inputs, reuse::Phase::PREPARE), "prepare sentinel cache");
    fill_validity(&state);
    Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &state, inputs, reuse::Phase::CAPTURE), "capture known spatial edit");
    submit_commands("initial anchor capture");
    const auto anchor_before = read_buffer(state.anchor.Get());
    const auto validity_before = read_buffer(state.validity.Get());
    Check(std::all_of(validity_before.begin(), validity_before.end(), [](uint8_t value) { return value == 0xA5; }),
          "refresh leaves validity untouched until APPLY overwrites every pixel");
    for (size_t pixel = 0; pixel < motion_values.size() / 2; ++pixel) motion_values[pixel * 2] = -.5f;
    const auto apply = [&] {
      gpu.Transition(neural.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &state, inputs, reuse::Phase::APPLY), "apply cached spatial edit");
      gpu.Transition(neural.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NATIVE_INPUT_STATE);
      Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &state, inputs, reuse::Phase::SELECT_ORIGINAL), "select current source for invalid reuse");
      auto model_plane = stage_execution_cases::Capture(gpu, neural.Get(), NATIVE_INPUT_STATE);
      auto final_plane = stage_execution_cases::Capture(gpu, decoded.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      submit_commands("reprojection and pixel selection");
      return std::pair{model_plane.Pixels(), final_plane.Pixels()};
    };
    upload(motion.Get(), motion_values, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, guide_width * 8);
    auto planes = apply();
    auto validity_bytes = read_buffer(state.validity.Get());
    std::vector<uint32_t> validity(width * height);
    std::memcpy(validity.data(), validity_bytes.data(), validity_bytes.size());
    for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
      const uint32_t x = pixel % width;
      Check(validity[pixel] == (x != 0 ? 1u : 0u), "scaled motion rejects the left boundary without clamping history");
      if (x == 0) {
        Check(std::memcmp(planes.second.data() + pixel * 16, original_values.data() + pixel * 4, 16) == 0,
              "out of bounds preserves the exact current source RGBA");
      } else {
        std::array<float, 4> actual;
        std::memcpy(actual.data(), planes.first.data() + pixel * 16, 16);
        for (uint32_t channel = 0; channel < 3; ++channel) {
          const float expected = encode((.125f + .0625f * channel) * (x - 1 < 8 ? 2.f : 1.5f));
          Check(std::fabs(actual[channel] - expected) < 3e-5f, "known edit follows current to previous motion on the output grid");
        }
        Check(actual[3] == .375f, "reuse preserves the current encoded proxy alpha");
      }
    }
    Check(read_buffer(state.anchor.Get()) == anchor_before, "reuse never writes the refresh anchor");
    depth_values[1] = .9f;
    upload(depth.Get(), depth_values, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, guide_width * 4);
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t channel = 0; channel < 3; ++channel) proxy_values[(y * width + 10) * 4 + channel] = encode(.75f);
    }
    upload(proxy.Get(), proxy_values, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, width * 16);
    planes = apply();
    validity_bytes = read_buffer(state.validity.Get());
    std::memcpy(validity.data(), validity_bytes.data(), validity_bytes.size());
    Check(validity[2] == 0 && validity[10] == 0, "depth and current color discontinuities reject cached edits");
    for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
      if (validity[pixel] == 0) {
        Check(std::memcmp(planes.second.data() + pixel * 16, original_values.data() + pixel * 4, 16) == 0,
              "every rejected pixel selects exact source color and alpha");
      }
    }
    // A neutral edit must take the exact-source branch, including alpha;
    // numerical codec neutrality alone is insufficient for this contract.
    upload(neural.Get(), proxy_values, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, width * 16);
    Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &state, inputs, reuse::Phase::CAPTURE), "capture neutral edit");
    planes = apply();
    Check(std::memcmp(planes.second.data(), original_values.data(), planes.second.size()) == 0,
          "neutral cache selects the exact original plane");
    for (size_t pixel = 0; pixel < motion_values.size() / 2; ++pixel) motion_values[pixel * 2] = 40000.f;
    upload(motion.Get(), motion_values, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, guide_width * 8);
    auto scaled_inputs = inputs;
    scaled_inputs.motion_scale_x = .00001f;
    Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &state, scaled_inputs, reuse::Phase::SNAPSHOT_MOTION)
              && reuse::Record(gpu.device.Get(), gpu.list.Get(), &state, scaled_inputs, reuse::Phase::MOTION),
          "compose normalized motion without half-float overflow");
    auto composed_plane = stage_execution_cases::Capture(gpu, state.composed_motion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    submit_commands("composed motion");
    const auto composed_bytes = composed_plane.Pixels();
    float composed_x = 0.f;
    std::memcpy(&composed_x, composed_bytes.data() + 2 * sizeof(float) * 2, sizeof(float));
    Check(std::isfinite(composed_x) && std::fabs(composed_x - 80000.f) < .1f,
          "RG32 composed motion retains finite 80000 raw units for an in-bounds 0.8-pixel displacement");
    std::cout << "PASS edit reuse WARP shader: untouched refresh validity, full sentinel overwrite, known edit, scaled reprojection, edge/depth/color rejection, neutral exact RGBA\n";

    // A fractional shift over texture detail can match the warped reference
    // exactly while differing from every individual tap.
    std::fill(depth_values.begin(), depth_values.end(), .5f);
    for (size_t pixel = 0; pixel < motion_values.size() / 2; ++pixel) {
      motion_values[pixel * 2] = -.25f;
    }
    for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
      const float source_luminance = (pixel % width % 2 == 0 ? .125f : .5f);
      for (uint32_t channel = 0; channel < 3; ++channel) {
        proxy_values[pixel * 4 + channel] = encode(source_luminance);
        neural_values[pixel * 4 + channel] = encode(1.5f * source_luminance);
      }
    }
    upload(depth.Get(), depth_values, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, guide_width * 4);
    upload(proxy.Get(), proxy_values, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, width * 16);
    upload(neural.Get(), neural_values, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, width * 16);
    Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &state, inputs, reuse::Phase::CAPTURE),
          "capture an edited texture before a half-pixel shift");
    const float warped_reference = .3125f;
    const float half_confidence_ratio = .5f * (std::exp2(-.125f) + std::exp2(-.375f));
    const float lighter = warped_reference / half_confidence_ratio;
    const float darker = warped_reference * half_confidence_ratio;
    for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
      for (uint32_t channel = 0; channel < 3; ++channel) {
        float current = warped_reference;
        if (pixel == 4) current = .75f;
        if (pixel == 5) current = lighter;
        if (pixel == 6) current = darker;
        if (pixel == 7 && channel == 0) current *= std::exp2(.38f);
        proxy_values[pixel * 4 + channel] = encode(current);
      }
    }
    depth_values[guide_width + 2] = .9f;
    upload(depth.Get(), depth_values, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, guide_width * 4);
    upload(proxy.Get(), proxy_values, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, width * 16);
    upload(motion.Get(), motion_values, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, guide_width * 8);
    planes = apply();
    std::array<float, 4> shifted_edit{};
    std::memcpy(shifted_edit.data(), planes.first.data() + 2 * 16, 16);
    Check(std::fabs(shifted_edit[0] - encode(1.5f * warped_reference)) < 3e-5f
              && shifted_edit[3] == .375f,
          "the matching bilinear color of translated detail keeps its edit and current alpha");
    for (const size_t rejected : {size_t{4}, size_t{7}, size_t{2 * width + 4}}) {
      Check(std::memcmp(planes.second.data() + rejected * 16, original_values.data() + rejected * 4, 16) == 0,
            "real color discontinuity and same-color depth reveal keep bit-exact original RGBA");
    }
    for (size_t pixel = 5; pixel <= 6; ++pixel) {
      const float current = (pixel == 5 ? lighter : darker);
      std::array<float, 4> attenuated{};
      std::memcpy(attenuated.data(), planes.first.data() + pixel * 16, 16);
      Check(std::fabs(attenuated[0] - encode(current * std::sqrt(1.5f))) < 3e-5f && attenuated[3] == .375f,
            "small moving color changes ease the edit symmetrically and preserve current alpha");
    }

    for (const bool low_grid : {false, true}) {
      constexpr uint32_t code_width = 1024, alpha_count = 4;
      const uint32_t work_width = (low_grid ? 768u : code_width);
      const uint32_t work_height = (low_grid ? 3u : alpha_count);
      auto code_original = gpu.Texture(code_width, alpha_count, DXGI_FORMAT_R10G10B10A2_UNORM);
      auto code_decoded = uav_texture(code_width, alpha_count, DXGI_FORMAT_R10G10B10A2_UNORM);
      auto code_proxy = gpu.Texture(work_width, work_height, DXGI_FORMAT_R32G32B32A32_FLOAT);
      auto code_neural = uav_texture(work_width, work_height, DXGI_FORMAT_R32G32B32A32_FLOAT);
      auto code_motion = gpu.Texture(1, 1, DXGI_FORMAT_R32G32_FLOAT);
      auto code_depth = gpu.Texture(1, 1, DXGI_FORMAT_R32_FLOAT);
      std::vector<uint32_t> codes(code_width * alpha_count);
      std::vector<float> neutral(work_width * work_height * 4, .375f);
      for (uint32_t alpha = 0; alpha < alpha_count; ++alpha) {
        for (uint32_t code = 0; code < code_width; ++code) {
          codes[alpha * code_width + code] = code | ((1023u - code) << 10)
              | (((code * 37u) & 1023u) << 20) | (alpha << 30);
        }
      }
      upload(code_original.Get(), codes, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, code_width * 4);
      upload(code_decoded.Get(), std::vector<uint32_t>(codes.size(), 0), D3D12_RESOURCE_STATE_COMMON,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, code_width * 4);
      upload(code_proxy.Get(), neutral, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, work_width * 16);
      upload(code_neural.Get(), neutral, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, work_width * 16);
      upload(code_motion.Get(), std::vector<float>(2, 0.f), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, 8);
      upload(code_depth.Get(), std::vector<float>(1, .5f), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, 4);
      reuse::State code_state;
      const reuse::Inputs code_inputs{.width = work_width, .height = work_height,
          .output_width = code_width, .output_height = alpha_count, .motion_width = 1, .motion_height = 1,
          .proxy = code_proxy.Get(), .neural = code_neural.Get(), .motion = code_motion.Get(), .depth = code_depth.Get(),
          .original = code_original.Get(), .decoded = code_decoded.Get()};
      Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &code_state, code_inputs, reuse::Phase::PREPARE),
            "prepare packed HDR10 sentinel cache");
      fill_validity(&code_state);
      Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &code_state, code_inputs, reuse::Phase::CAPTURE),
            "capture neutral anchor for packed HDR10 selection");
      gpu.Transition(code_neural.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &code_state, code_inputs, reuse::Phase::APPLY),
            "neutral HDR10 cache marks every pixel for original selection");
      gpu.Transition(code_neural.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NATIVE_INPUT_STATE);
      Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &code_state, code_inputs, reuse::Phase::SELECT_ORIGINAL),
            "select packed HDR10 source through production typed SRV and UAV");
      auto selected = stage_execution_cases::Capture(gpu, code_decoded.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      submit_commands("all HDR10 codes and alpha selection");
      const auto selected_bytes = selected.Pixels();
      Check(selected_bytes.size() == codes.size() * sizeof(uint32_t)
                && std::memcmp(selected_bytes.data(), codes.data(), selected_bytes.size()) == 0,
            "SELECT_ORIGINAL preserves all 1024 codes of each RGB channel and all four alpha codes bit exactly");
      const auto code_validity = read_buffer(code_state.validity.Get());
      Check(std::all_of(code_validity.begin(), code_validity.end(), [](uint8_t value) { return value == 0; }),
            "neutral HDR10 APPLY overwrites every nonzero validity sentinel with invalid");
    }

    {
      constexpr uint32_t work_width = 6, work_height = 4, output_width = 10, output_height = 7;
      constexpr uint32_t motion_width = 4, motion_height = 3;
      auto low_proxy = gpu.Texture(work_width, work_height, DXGI_FORMAT_R32G32B32A32_FLOAT);
      auto low_neural = uav_texture(work_width, work_height, DXGI_FORMAT_R32G32B32A32_FLOAT);
      auto low_motion = gpu.Texture(motion_width, motion_height, DXGI_FORMAT_R32G32_FLOAT);
      auto low_depth = gpu.Texture(motion_width, motion_height, DXGI_FORMAT_R32_FLOAT);
      auto low_original = gpu.Texture(output_width, output_height, DXGI_FORMAT_R32G32B32A32_FLOAT);
      auto low_decoded = uav_texture(output_width, output_height, DXGI_FORMAT_R32G32B32A32_FLOAT);
      std::vector<float> low_proxy_values(work_width * work_height * 4);
      std::vector<float> low_neural_values(work_width * work_height * 4);
      std::vector<float> low_original_values(output_width * output_height * 4);
      std::vector<float> low_decoded_values(output_width * output_height * 4);
      std::vector<float> low_motion_values(motion_width * motion_height * 2, 0.f);
      const std::array<float, 4> processed{.625f, .5f, .375f, .875f};
      for (uint32_t y = 0; y < work_height; ++y) {
        for (uint32_t x = 0; x < work_width; ++x) {
          const size_t pixel = y * work_width + x;
          const float edit = .25f + .125f * x + .0625f * y;
          for (uint32_t channel = 0; channel < 3; ++channel) {
            const float reference = .125f + .0625f * channel;
            low_proxy_values[pixel * 4 + channel] = encode(reference);
            low_neural_values[pixel * 4 + channel] = encode(reference * std::exp2(edit));
          }
          low_proxy_values[pixel * 4 + 3] = .375f;
          low_neural_values[pixel * 4 + 3] = .875f;
        }
      }
      for (uint32_t pixel = 0; pixel < output_width * output_height; ++pixel) {
        for (uint32_t channel = 0; channel < 4; ++channel) {
          low_original_values[pixel * 4 + channel] = .015625f * (channel + 1) + pixel / 1024.f;
          // Post-Resolve/Commit sentinel; this primitive case does not run Resolve.
          low_decoded_values[pixel * 4 + channel] = processed[channel];
        }
      }
      for (uint32_t y = 0; y < motion_height; ++y) {
        low_motion_values[(y * motion_width + 2) * 2] = 1.f;
      }
      upload(low_proxy.Get(), low_proxy_values, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, work_width * 16);
      upload(low_neural.Get(), low_neural_values, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, work_width * 16);
      upload(low_motion.Get(), low_motion_values, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, motion_width * 8);
      upload(low_depth.Get(), std::vector<float>(motion_width * motion_height, .5f),
          D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, motion_width * 4);
      upload(low_original.Get(), low_original_values, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, output_width * 16);
      upload(low_decoded.Get(), low_decoded_values, D3D12_RESOURCE_STATE_COMMON,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, output_width * 16);
      reuse::State low_state;
      reuse::Inputs low_inputs{.width = work_width, .height = work_height,
          .output_width = output_width, .output_height = output_height,
          .motion_width = motion_width, .motion_height = motion_height,
          .proxy = low_proxy.Get(), .neural = low_neural.Get(), .motion = low_motion.Get(),
          .depth = low_depth.Get(), .original = low_original.Get(), .decoded = low_decoded.Get()};
      Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &low_state, low_inputs, reuse::Phase::PREPARE),
            "prepare separate work/output/guide grids");
      fill_validity(&low_state);
      Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &low_state, low_inputs, reuse::Phase::CAPTURE),
            "capture non-neutral edit on the low working grid");
      const auto low_anchor = read_buffer(low_state.anchor.Get());
      const auto low_sentinel = read_buffer(low_state.validity.Get());
      Check(low_sentinel.size() == work_width * work_height * sizeof(uint32_t)
                && std::all_of(low_sentinel.begin(), low_sentinel.end(), [](uint8_t value) { return value == 0xA5; }),
            "low-grid CAPTURE leaves every working validity sentinel untouched");
      auto changed_extent = low_inputs;
      ++changed_extent.output_width;
      Check(!reuse::Record(gpu.device.Get(), gpu.list.Get(), &low_state, changed_extent, reuse::Phase::PREPARE),
            "an existing anchor cannot be rebound to a different output extent");

      // Jitter moves work x=2 into guide column 2, whose one-pixel motion
      // becomes 1.5 WORK pixels. Scaling by the output width would give 2.5.
      low_inputs.raster_offset_x = .4f;
      low_inputs.raster_offset_y = -.25f;
      for (uint32_t y = 0; y < work_height; ++y) {
        for (uint32_t channel = 0; channel < 3; ++channel) {
          low_proxy_values[(y * work_width + 1) * 4 + channel] = encode(.75f);
        }
      }
      upload(low_proxy.Get(), low_proxy_values, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, work_width * 16);
      gpu.Transition(low_neural.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &low_state, low_inputs, reuse::Phase::APPLY),
            "apply low-grid edit with nonzero raster jitter and physical guide motion");
      gpu.Transition(low_neural.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NATIVE_INPUT_STATE);
      Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &low_state, low_inputs, reuse::Phase::SELECT_ORIGINAL),
            "select original RGBA across the complete output grid");
      auto low_model = stage_execution_cases::Capture(gpu, low_neural.Get(), NATIVE_INPUT_STATE);
      auto low_final = stage_execution_cases::Capture(gpu, low_decoded.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      const auto low_validity_bytes = read_buffer(low_state.validity.Get());
      const auto low_model_bytes = low_model.Pixels();
      const auto low_final_bytes = low_final.Pixels();
      std::vector<uint32_t> low_validity(work_width * work_height);
      std::memcpy(low_validity.data(), low_validity_bytes.data(), low_validity_bytes.size());
      for (uint32_t y = 0; y < work_height; ++y) {
        for (uint32_t x = 0; x < work_width; ++x) {
          Check(low_validity[y * work_width + x] == (x == 1 || x == 5 ? 0u : 1u),
                "low-grid APPLY overwrites every sentinel with its known valid or rejected edit");
        }
      }
      std::array<float, 4> moved_edit;
      std::memcpy(moved_edit.data(), low_model_bytes.data() + (work_width + 2) * 16, 16);
      for (uint32_t channel = 0; channel < 3; ++channel) {
        Check(std::fabs(moved_edit[channel] - encode((.125f + .0625f * channel) * std::exp2(.75f))) < 3e-5f,
              "work (2,1) samples anchor x=3.5 with edit .75 stops, independent of output size and jitter delta");
      }
      Check(moved_edit[3] == .375f, "low-grid APPLY retains current encoded proxy alpha");
      for (const size_t pixel : {size_t{0}, size_t{7}, size_t{3 * output_width + 4}}) {
        Check(std::memcmp(low_final_bytes.data() + pixel * 16, processed.data(), 16) == 0,
              "clamped edge, exact work center and four valid nonzero taps preserve processed RGBA");
      }
      // Output (0,0) clamps to work (0,0); invalid work x=1 has zero
      // weight. Output (7,0) lands exactly on valid x=4, not invalid x=5.
      for (const size_t pixel : {size_t{1}, size_t{8}, size_t{output_width * output_height - 1}}) {
        Check(std::memcmp(low_final_bytes.data() + pixel * 16, low_original_values.data() + pixel * 4, 16) == 0,
              "any invalid nonzero tap, including the output tail beyond both work extents, selects exact original RGBA");
      }
      Check(read_buffer(low_state.anchor.Get()) == low_anchor,
            "low-grid APPLY and output selection never recursively rewrite the refresh anchor");
      std::cout << "PASS edit reuse low-grid footprint: work=6x4 output=10x7 guides=4x3, jitter/MV, full validity, zero/nonzero taps, exact RGBA and immutable anchor\n";
    }

    {
      // These are physical guide-raster offsets, independent of the SR API's
      // jitter sign. The 1.5 ratio places two output centers on opposite sides
      // of a guide texel boundary, distinguishing correct/omitted/reversed offsets.
      constexpr uint32_t witness_size = 12, witness_grid = 8;
      auto witness_proxy = gpu.Texture(witness_size, witness_size, DXGI_FORMAT_R32G32B32A32_FLOAT);
      auto witness_neural = uav_texture(witness_size, witness_size, DXGI_FORMAT_R32G32B32A32_FLOAT);
      auto witness_motion = gpu.Texture(witness_grid, witness_grid, DXGI_FORMAT_R32G32_FLOAT);
      auto witness_depth = gpu.Texture(witness_grid, witness_grid, DXGI_FORMAT_R32_FLOAT);
      auto witness_original = gpu.Texture(witness_size, witness_size, DXGI_FORMAT_R32G32B32A32_FLOAT);
      auto witness_decoded = uav_texture(witness_size, witness_size, DXGI_FORMAT_R32G32B32A32_FLOAT);
      std::vector<float> witness_proxy_values(witness_size * witness_size * 4);
      std::vector<float> witness_neural_values(witness_size * witness_size * 4);
      std::vector<float> witness_original_values(witness_size * witness_size * 4);
      std::vector<float> witness_motion_values(witness_grid * witness_grid * 2, 0.f);
      std::vector<float> witness_depth_values(witness_grid * witness_grid);
      for (uint32_t y = 0; y < witness_size; ++y) {
        for (uint32_t x = 0; x < witness_size; ++x) {
          const size_t pixel = static_cast<size_t>(y) * witness_size + x;
          const float edit = .2f + .02f * x + .03f * y;
          for (uint32_t channel = 0; channel < 3; ++channel) {
            witness_proxy_values[pixel * 4 + channel] = encode(.125f);
            witness_neural_values[pixel * 4 + channel] = encode(.125f * std::exp2(edit));
            witness_original_values[pixel * 4 + channel] = .03125f * (channel + 1) + static_cast<float>(pixel) / 1024.f;
          }
          witness_proxy_values[pixel * 4 + 3] = .375f;
          witness_neural_values[pixel * 4 + 3] = .875f;
          witness_original_values[pixel * 4 + 3] = .25f + .03125f * (pixel % 4);
        }
      }
      for (uint32_t y = 0; y < witness_grid; ++y) {
        for (uint32_t x = 0; x < witness_grid; ++x) {
          witness_depth_values[y * witness_grid + x] = .25f + (x >= 4 ? .25f : 0.f) + (y >= 4 ? .125f : 0.f);
        }
      }
      upload(witness_proxy.Get(), witness_proxy_values, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, witness_size * 16);
      upload(witness_neural.Get(), witness_neural_values, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, witness_size * 16);
      upload(witness_motion.Get(), witness_motion_values, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, witness_grid * 8);
      upload(witness_depth.Get(), witness_depth_values, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, witness_grid * 4);
      upload(witness_original.Get(), witness_original_values, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, witness_size * 16);
      upload(witness_decoded.Get(), std::vector<float>(witness_size * witness_size * 4, -7.f),
          D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, witness_size * 16);
      reuse::Inputs witness_inputs{.width = witness_size, .height = witness_size,
          .motion_width = witness_grid, .motion_height = witness_grid,
          .proxy = witness_proxy.Get(), .neural = witness_neural.Get(), .motion = witness_motion.Get(),
          .depth = witness_depth.Get(), .original = witness_original.Get(), .decoded = witness_decoded.Get()};
      const std::array<std::array<float, 2>, 3> raster_offsets{{{.4f, -.4f}, {0.f, 0.f}, {-.4f, .4f}}};
      const std::array<std::array<uint32_t, 2>, 4> witness_pixels{{{5, 1}, {6, 1}, {1, 5}, {1, 6}}};
      const std::array<std::array<float, 4>, 3> expected_depth{{
          {.5f, .5f, .25f, .25f}, {.25f, .5f, .25f, .375f}, {.25f, .25f, .375f, .375f}}};
      std::array<reuse::State, 3> raster_states;
      std::array<std::vector<uint8_t>, 3> captured_anchors;
      for (size_t variant = 0; variant < raster_offsets.size(); ++variant) {
        witness_inputs.raster_offset_x = raster_offsets[variant][0];
        witness_inputs.raster_offset_y = raster_offsets[variant][1];
        Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &raster_states[variant], witness_inputs, reuse::Phase::CAPTURE),
              "capture GPU raster-offset depth witnesses");
        captured_anchors[variant] = read_buffer(raster_states[variant].anchor.Get());
        for (size_t point = 0; point < witness_pixels.size(); ++point) {
          const size_t pixel = witness_pixels[point][1] * witness_size + witness_pixels[point][0];
          std::array<float, 8> anchor;
          std::memcpy(anchor.data(), captured_anchors[variant].data() + pixel * 32, 32);
          Check(anchor[3] == 1.f && anchor[7] == expected_depth[variant][point],
                "GPU guide lookup distinguishes both axes from missing or reversed raster offsets");
        }
      }
      for (const size_t pixel : {size_t{2}, size_t{2 * witness_size + witness_size - 1}}) {
        float anchor_valid = 1.f;
        std::memcpy(&anchor_valid, captured_anchors[0].data() + pixel * 32 + 3 * sizeof(float), sizeof(float));
        Check(anchor_valid == 0.f, "capture marks out-of-grid raster guides invalid rather than clamping an anchor");
      }

      // Current motion is zero. Only guide sampling changes: the edit itself
      // must remain at the same output pixel, without a jitter-difference shift.
      witness_inputs.raster_offset_x = 0.f;
      witness_inputs.raster_offset_y = 0.f;
      witness_inputs.previous_motion_offset_x = .4f;
      witness_inputs.previous_motion_offset_y = -.4f;
      gpu.Transition(witness_neural.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &raster_states[0], witness_inputs, reuse::Phase::APPLY),
            "reproject canonical edit with nonzero raster-offset difference");
      gpu.Transition(witness_neural.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NATIVE_INPUT_STATE);
      Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &raster_states[0], witness_inputs, reuse::Phase::SELECT_ORIGINAL),
            "select exact source for invalid offset anchor");
      auto witness_model = stage_execution_cases::Capture(gpu, witness_neural.Get(), NATIVE_INPUT_STATE);
      auto witness_final = stage_execution_cases::Capture(gpu, witness_decoded.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      const auto witness_validity_bytes = read_buffer(raster_states[0].validity.Get());
      const auto witness_model_bytes = witness_model.Pixels();
      const auto witness_final_bytes = witness_final.Pixels();
      uint32_t anchor_rejected = 1, interior_valid = 0;
      std::memcpy(&anchor_rejected, witness_validity_bytes.data() + 2 * sizeof(uint32_t), sizeof(uint32_t));
      constexpr size_t interior = 8 * witness_size + 8;
      std::memcpy(&interior_valid, witness_validity_bytes.data() + interior * sizeof(uint32_t), sizeof(uint32_t));
      Check(anchor_rejected == 0 && interior_valid == 1, "apply rejects invalid captured guides while retaining interior edit");
      Check(std::memcmp(witness_final_bytes.data() + 2 * 16, witness_original_values.data() + 2 * 4, 16) == 0,
            "invalid raster anchor returns exact current source color and alpha");
      std::array<float, 4> unmoved_edit;
      std::memcpy(unmoved_edit.data(), witness_model_bytes.data() + interior * 16, 16);
      for (size_t channel = 0; channel < 3; ++channel) {
        Check(std::fabs(unmoved_edit[channel] - encode(.125f * std::exp2(.6f))) < 3e-5f,
              "raster jitter difference is not added to the final edit displacement");
      }
      Check(unmoved_edit[3] == .375f && read_buffer(raster_states[0].anchor.Get()) == captured_anchors[0],
            "offset reuse preserves current proxy alpha and does not rewrite its anchor");

      for (uint32_t y = 0; y < witness_grid; ++y) {
        for (uint32_t x = 0; x < witness_grid; ++x) {
          witness_motion_values[(y * witness_grid + x) * 2] = (x < 4 ? 0.f : 2.f);
          witness_motion_values[(y * witness_grid + x) * 2 + 1] = (y < 4 ? 0.f : -2.f);
        }
      }
      upload(witness_motion.Get(), witness_motion_values, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, witness_grid * 8);
      Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &raster_states[0], witness_inputs, reuse::Phase::SNAPSHOT_MOTION),
            "snapshot previous raw-grid motion step");
      for (size_t pixel = 0; pixel < witness_motion_values.size() / 2; ++pixel) {
        witness_motion_values[pixel * 2] = .25f;
        witness_motion_values[pixel * 2 + 1] = -.25f;
      }
      upload(witness_motion.Get(), witness_motion_values, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, witness_grid * 8);
      const std::array<std::array<float, 2>, 3> expected_composed{{{.25f, -2.25f}, {.75f, -1.75f}, {2.25f, -.25f}}};
      for (size_t variant = 0; variant < raster_offsets.size(); ++variant) {
        witness_inputs.raster_offset_x = raster_offsets[variant][0];
        witness_inputs.raster_offset_y = raster_offsets[variant][1];
        witness_inputs.previous_motion_offset_x = -raster_offsets[variant][0];
        witness_inputs.previous_motion_offset_y = -raster_offsets[variant][1];
        Check(reuse::Record(gpu.device.Get(), gpu.list.Get(), &raster_states[0], witness_inputs, reuse::Phase::MOTION),
              "compose GPU motion with previous minus current raster offset");
        auto witness_composed = stage_execution_cases::Capture(gpu, raster_states[0].composed_motion.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit_commands("raster-offset composed motion witness");
        const auto bytes = witness_composed.Pixels();
        std::array<float, 2> actual;
        std::memcpy(actual.data(), bytes.data() + (4 * witness_grid + 3) * sizeof(float) * 2, sizeof(actual));
        Check(std::fabs(actual[0] - expected_composed[variant][0]) < 2e-5f
                  && std::fabs(actual[1] - expected_composed[variant][1]) < 2e-5f,
              "two-step GPU motion distinguishes correct, omitted and reversed raster-offset differences");
      }
      std::cout << "PASS edit reuse WARP raster offsets: ratio=1.5 both axes, capture validity, exact RGBA, canonical edit, two-step motion\n";
    }

    // Use the production SR wrapper with GPU-copy SR and NR substitutes. The
    // model call counter observes actual wrapper invocations, never cadence requests.
    native::enabled = true;
    native::hooks_enabled = true;
    native::d3d12_device_seen = true;
    native::shutdown_quiesce_enabled = false;
    native::hook_point = native::kHookUpscaled;
    native::stack_passes = 1;
    native::pass_stage[0] = 2u;
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
    native::gpu_timers_enabled = false;
    native::native_fg_public_off = true;
    native::native_fg_declaration = native::NativeFgDeclaration::kOff;
    native::transfer_strength = 1.f;
    native::color_strength = 1.f;
    native::intensity = 1.f;
    native::preset = 2;
    native::edit_reuse_enabled = true;
#if defined(NR_NEXUS_DEV_CONTROL)
    native::edit_reuse_fg_experiment = false;
#endif
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = CreateModel;
    edit_model_behavior = NeutralModel;
    native::direct_api.evaluate = EvaluateEditModelFixture;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::direct_runtime_requirements.dual_models = true;
    native::direct_runtime_requirements.file_version = "310.9.16.0";
    native::parameter_runtime_ready = true;
    native::parameter_runtime_via_core = false;
    native::core_allocate_parameters = addon::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = addon::bridge::DestroyOwnedParameters;
    auto source = gpu.Texture(16, 16, DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto output = uav_texture(16, 16, DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto wrapper_motion = gpu.Texture(16, 16, DXGI_FORMAT_R16G16_FLOAT);
    auto wrapper_depth = gpu.Texture(16, 16, DXGI_FORMAT_R32_FLOAT);
    std::vector<uint16_t> source_values(inline_trace::SOURCE_RGBA.begin(), inline_trace::SOURCE_RGBA.end());
    upload(source.Get(), source_values, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, 16 * 8);
    upload(wrapper_motion.Get(), std::vector<uint16_t>(16 * 16 * 2, 0), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, 16 * 4);
    upload(wrapper_depth.Get(), std::vector<float>(16 * 16, .5f), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE, 16 * 4);
    gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    submit_commands("wrapper input upload");
    addon::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_Parameter_Color, source.Get());
    parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
    parameters.Set(NVSDK_NGX_Parameter_Depth, wrapper_depth.Get());
    parameters.Set(NVSDK_NGX_Parameter_MotionVectors, wrapper_motion.Get());
    parameters.Set(NVSDK_NGX_Parameter_Width, 16u);
    parameters.Set(NVSDK_NGX_Parameter_Height, 16u);
    parameters.Set(NVSDK_NGX_Parameter_OutWidth, 16u);
    parameters.Set(NVSDK_NGX_Parameter_OutHeight, 16u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 16u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 16u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<uint32_t>(
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_IsHDR));
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.f);
    const auto parameters_without_jitter_y = parameters;
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f);

    {
      funnel_accounting_tests::Restore restore;
      restore.Value(&native::ngx_slot_real[0]);
      restore.Value(&edit_model_behavior);
      restore.Atomic(&native::d3d11_swapchain_seen);
      restore.Atomic(&native::d3d11_present_seen);
      restore.Atomic(&native::d3d12_swapchain_seen);
      restore.Atomic(&native::d3d12_present_seen);
      restore.Atomic(&native::dx11_route);
      restore.Atomic(&native::control_mask_use_core_context);
      restore.Atomic(&native::foreign_teardown_due_ns);
      edit_model_behavior = ConstantModel;
      native::ngx_slot_real[0].create = reinterpret_cast<void*>(+[](ID3D12GraphicsCommandList* list,
          NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** handle) {
        Check(list == model_gpu->list.Get() && feature == native::kFeatureDlss,
              "inline real Create receives the D3D12 SR producer");
        static uint32_t sr_handle = 1;
        *handle = reinterpret_cast<NVSDK_NGX_Handle*>(&sr_handle);
        return NVSDK_NGX_Result_Success;
      });
      native::ngx_slot_real[0].release = reinterpret_cast<void*>(+[](NVSDK_NGX_Handle*) {
        return NVSDK_NGX_Result_Success;
      });
      static uint32_t sr_calls = 0;
      native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(+[](ID3D12GraphicsCommandList* list,
          const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* block, PFN_NVSDK_NGX_ProgressCallback callback) {
        Check(list == model_gpu->list.Get(), "inline real Evaluate uses the same D3D12 SR producer");
        ++sr_calls;
        return CopySr(list, handle, block, callback);
      });

      constexpr auto STAGE = addon::stage_plan::Stage::UPSCALED;
      constexpr size_t FRAME_COUNT = 4;
      struct BaseResult {
        inline_trace::Base trace;
        std::vector<uint8_t> rgba;

        bool operator==(const BaseResult&) const = default;
      };
      std::array<BaseResult, FRAME_COUNT> dx12_bases;
      uint64_t previous_source_generation = 0;
      for (const bool foreign : {false, true}) {
        const char* route = (foreign ? "foreign" : "dx12");
        // Change the presentation observation on the same D3D12 producer and textures.
        native::d3d11_swapchain_seen = foreign;
        native::d3d11_present_seen = foreign;
        native::d3d12_swapchain_seen = !foreign;
        native::d3d12_present_seen = !foreign;
        native::dx11_route = (foreign ? native::kDx11RouteForeign : native::kDx11RouteUndecided);
        Check(native::D3D11OnlySession() == foreign && !native::ForeignD3D12SourceIgnored(),
              "inline policy contrast reaches the matching producer route");

        auto evaluation = parameters;
        NVSDK_NGX_Handle* sr = nullptr;
        Check(NVSDK_NGX_SUCCEED(native::HookedCreateFeatureSlot<0>(gpu.list.Get(), native::kFeatureDlss, &evaluation, &sr))
                  && sr != nullptr,
              "inline policy records the real SR Create through its wrapper");
        const auto& created = native::create_contracts.at(sr);
        const auto source_generation = created.source_generation;
        Check(created.source_type_confirmed && created.source_feature == native::kFeatureDlss
                  && created.source_device11_identity == nullptr && source_generation > previous_source_generation,
              "each real SR Create provides a new confirmed source generation");
        previous_source_generation = source_generation;

        const auto& stage = native::stage_telemetry[static_cast<size_t>(STAGE)];
        const auto evaluate = [&](int reset, bool capture = true) {
          evaluation.Set(NVSDK_NGX_Parameter_Reset, reset);
          gpu.list->ClearState(nullptr);
          const auto sr_before = sr_calls;
          const auto nr_before = model_calls;
          const auto resets_before = model_resets.size();
          const auto sequence_before = native::features.at(sr).sr_sequence;
          const auto groups_before = stage.groups;
          const auto skipped_before = stage.skipped;
          const auto plan_before = native::last_source_plan.sequence;
          Check(NVSDK_NGX_SUCCEED(native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), sr, &evaluation, nullptr))
                    && sr_calls == sr_before + 1,
                "inline wrapper preserves one successful real SR Evaluate per frame");
          const auto& feature = native::features.at(sr);
          const auto advance = (native::ForeignD3D12SourceIgnored() ? 0u : 1u);
          Check(feature.source_generation == source_generation && feature.sr_sequence == sequence_before + advance
                    && native::direct_device == gpu.device.Get(),
                "inline wrapper keeps its source generation and device with the admitted SR sequence");
          BaseResult result;
          result.trace.calls = model_calls - nr_before;
          Check(model_resets.size() - resets_before == result.trace.calls, "every actual NR call has a Reset observation");
          if (result.trace.calls != 0) {
            result.trace.reset = model_resets.back();
          }
          result.trace.complete = stage.groups == groups_before + 1 && stage.skipped == skipped_before;
          if (result.trace.complete) {
            const auto& observed = native::last_source_plan;
            const auto group = observed.plan.At(STAGE);
            Check(observed.sequence > plan_before && observed.source_handle == sr && observed.source_generation == source_generation
                      && observed.source_feature == native::kFeatureDlss && !observed.source_released
                      && observed.plan.valid && !observed.plan.mixed && observed.plan.pass_count == 1
                      && observed.plan.group_count == 1 && group.stage == STAGE && group.begin == 0 && group.count == 1
                      && stage.pass_begin == 0 && stage.pass_count == 1 && feature.slots[0].nr_preset == 2
                      && stage.width == inline_trace::WIDTH && stage.height == inline_trace::HEIGHT
                      && stage.work_width == inline_trace::WIDTH && stage.work_height == inline_trace::HEIGHT,
                  "the actual execution plan is one Upscaled model B Pass on the unchanged full-size D3D12 output");
          }
          stage_execution_cases::Snapshot final;
          std::shared_ptr<addon::submission::RecordingStatus> receipt;
          if (capture) {
            final = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (result.trace.complete) {
              receipt = addon::submission::ObserveRecording(gpu.list.Get());
            }
          }
          submit_commands(route);
          native::DrainRetiredResources(true);
          const auto reached = gpu.fence->GetCompletedValue();
          Check(reached != UINT64_MAX && reached >= gpu.serial, "inline SR and writeback complete on the D3D12 fence");
          Check(receipt == nullptr || (receipt->submitted.load() && receipt->closed.load() && receipt->completed_healthy),
                "inline NR delivery has a completed submission receipt");
          if (capture) {
            result.rgba = final.Pixels();
          }
          return result;
        };

        for (uint32_t warmup = 0; warmup < native::kContractDebounceFrames + 8; ++warmup) {
          evaluate(1, false);
        }
        const auto& warmed = native::features.at(sr);
        const auto anchor = warmed.slots[0].edit_reuse;
        Check(anchor != nullptr && anchor->valid && anchor->source_sequence == warmed.sr_sequence,
              "fixed Upscaled SR warmup leaves a completed current anchor");
        const auto sequence_origin = warmed.sr_sequence;
        const auto generation_origin = anchor->anchor_generation;
        const auto reuses_origin = anchor->reuse_count;
        auto anchor_generation = generation_origin;
        auto anchor_sequence = anchor->source_sequence;
        std::vector<uint8_t> refresh_rgba;
        for (size_t index = 0; index < FRAME_COUNT; ++index) {
          if (index == 1) {
            const auto configuration = native::configuration_generation.load();
            const auto history = native::history_reset_epoch.load();
            const auto before = model_calls;
            addon::presets::Document decoded;
            const addon::presets::Document saved{
                .scope = addon::presets::Scope::QUALITY, .settings = native::profile_ui::Snapshot()};
            std::string error;
            Check(addon::presets::Parse(addon::presets::Serialize(saved), &decoded, &error)
                      && native::preset_ui::Apply(decoded, true, false, &error)
                      && native::configuration_generation.load() == configuration
                      && native::history_reset_epoch.load() == history && model_calls == before,
                  "an equivalent preset roundtrip preserves the live source, configuration and history before reuse");
          }
          const bool refresh = index % 2 == 0;
          auto base = evaluate(index == 0 ? 1 : 0);
          const auto& feature = native::features.at(sr);
          Check(feature.slots[0].edit_reuse == anchor && anchor->valid && anchor->reused == !refresh
                    && anchor->reuse_count == reuses_origin + (index + 1) / 2,
                "reuse retains the valid refresh anchor for one adjacent base");
          base.trace.base = feature.sr_sequence - sequence_origin;
          base.trace.history = anchor->source_sequence - sequence_origin;
          // Reuse advances the history cursor even when the anchor retains its pixels.
          if (anchor->anchor_generation != anchor_generation) {
            anchor_generation = anchor->anchor_generation;
            anchor_sequence = anchor->source_sequence;
          }
          base.trace.anchor_base = anchor_sequence - sequence_origin;
          base.trace.anchor_generation = anchor->anchor_generation - generation_origin;
          base.trace.anchor_age = feature.sr_sequence - anchor_sequence;
          Check(base.trace.complete && base.trace.base == index + 1 && base.trace.history == index + 1
                    && base.trace.calls == (refresh ? 1u : 0u) && base.trace.reset == (refresh ? (index == 0 ? 1 : 0) : -1)
                    && base.trace.anchor_base == (index / 2) * 2 + 1 && base.trace.anchor_generation == index / 2 + 1
                    && base.trace.anchor_age == index % 2,
                "inline delivery preserves source clock, exact NR/Reset cadence and anchor identity");
          Check(base.rgba.size() == sizeof(inline_trace::SOURCE_RGBA), "inline readback contains the entire FP16 RGBA output");
          if (refresh) {
            refresh_rgba = base.rgba;
          }
          bool nonneutral = false;
          for (size_t channel = 0; channel < inline_trace::SOURCE_RGBA.size(); ++channel) {
            const size_t offset = channel * sizeof(uint16_t);
            uint16_t pixel, refreshed;
            std::memcpy(&pixel, base.rgba.data() + offset, sizeof(pixel));
            if (channel % 4 == 3) {
              Check(pixel == inline_trace::SOURCE_RGBA[channel], "inline output preserves exact current alpha");
              continue;
            }
            std::memcpy(&refreshed, refresh_rgba.data() + offset, sizeof(refreshed));
            const float actual = DirectX::PackedVector::XMConvertHalfToFloat(pixel);
            Check(std::abs(actual - DirectX::PackedVector::XMConvertHalfToFloat(refreshed)) < .003f,
                  "inline reuse reconstructs the nonzero RGB edit at every pixel");
            nonneutral |= std::abs(actual - DirectX::PackedVector::XMConvertHalfToFloat(inline_trace::SOURCE_RGBA[channel])) > .05f;
          }
          Check(nonneutral, "inline output contains a distinguishable nonzero NR edit");
          inline_trace::PrintBase(route, base.trace, base.rgba);
          if (foreign) {
            Check(base == dx12_bases[index], "foreign and DX12 deliver identical cadence and all FP16 RGBA bits");
          } else {
            dx12_bases[index] = std::move(base);
          }
        }

        if (foreign) {
          const auto expect_original = [&](addon::NrDeclineReason reason) {
            const auto declines = addon::nr_decline_counts[static_cast<size_t>(reason)].load();
            const auto reuses = native::edit_reuse_reuses.load();
            const auto rejected = evaluate(0);
            Check(!rejected.trace.complete && rejected.trace.calls == 0 && native::edit_reuse_reuses.load() == reuses
                      && addon::nr_decline_counts[static_cast<size_t>(reason)].load() == declines + 1
                      && rejected.rgba.size() == sizeof(inline_trace::SOURCE_RGBA)
                      && std::memcmp(rejected.rgba.data(), inline_trace::SOURCE_RGBA.data(), rejected.rgba.size()) == 0,
                  "inline decline retains the exact real SR output without evaluating or reusing NR");
          };
          Check(evaluate(1).trace.calls == 1, "prepare a reuse opportunity for the native route mismatch");
          native::dx11_route = native::kDx11RouteNative;
          expect_original(addon::NrDeclineReason::kSourceOther);
          native::dx11_route = native::kDx11RouteForeign;

          const auto foreign_refresh = evaluate(1);
          Check(foreign_refresh.trace.complete && foreign_refresh.trace.calls == 1,
                "restoring the foreign route refreshes before testing the Core switch");
          native::control_mask_use_core_context = true;
          expect_original(addon::NrDeclineReason::kNrRuntimeUnavailable);
          native::control_mask_use_core_context = false;
          const auto owned_refresh = evaluate(1);
          const auto owned_reuse = evaluate(0);
          Check(owned_refresh.trace.complete && owned_refresh.trace.calls == 1 && owned_refresh.trace.reset == 1
                    && owned_reuse.trace.complete && owned_reuse.trace.calls == 0,
                "owned runtime recovers refresh and reuse after the Core request is rejected");

          Check(evaluate(1).trace.calls == 1, "prepare a reuse opportunity for the missing jitter component");
          evaluation = parameters_without_jitter_y;
          const auto fallbacks = native::edit_reuse_fallbacks.load();
          const auto reuses = native::edit_reuse_reuses.load();
          const auto fallback = evaluate(0);
          Check(fallback.trace.complete && fallback.trace.calls == 1 && native::edit_reuse_fallbacks.load() > fallbacks
                    && native::edit_reuse_reuses.load() == reuses && native::features.at(sr).slots[0].edit_reuse == nullptr,
                "missing jitter retires the warm cache and runs the normal model path");
          evaluation = parameters;
          const auto recovered = evaluate(1);
          const auto reused = evaluate(0);
          const auto& restored_anchor = native::features.at(sr).slots[0].edit_reuse;
          Check(recovered.trace.complete && recovered.trace.calls == 1 && recovered.trace.reset == 1
                    && reused.trace.complete && reused.trace.calls == 0 && restored_anchor != nullptr
                    && restored_anchor->valid && restored_anchor->reused && native::edit_reuse_reuses.load() == reuses + 1,
                "restoring jitter re-establishes a new anchor and the next-frame reuse");
        }
        Check(NVSDK_NGX_SUCCEED(native::HookedReleaseFeatureSlot<0>(sr)), "inline SR release uses the production wrapper");
        native::DrainRetiredResources(true);
      }
      std::cout << "PASS fixed single-Pass Upscaled DX12/foreign equivalence and route/Core/jitter contrasts evidence=WARP+MOCK\n";

      funnel_accounting_tests::Restore source_restore;
      source_restore.Atomic(&native::hook_point);
      source_restore.Atomic(&native::stack_passes);
      source_restore.Atomic(&native::pass_stage[1]);
      source_restore.Atomic(&native::pass_resolution_mode[0]);
      source_restore.Atomic(&native::pass_resolution_scale[0]);
      source_restore.Atomic(&native::pass_resolution_mode[1]);
      source_restore.Atomic(&native::edit_reuse_enabled);
      source_restore.Atomic(&native::stage_source_api);
      source_restore.Atomic(&native::stage_source_rr);
      source_restore.Value(&native::last_source_plan);
      source_restore.Value(&native::stage_telemetry);
      source_restore.Value(&native::real_streamline_free[0]);
      native::d3d11_swapchain_seen = false;
      native::d3d11_present_seen = false;
      native::d3d12_swapchain_seen = true;
      native::d3d12_present_seen = true;
      native::dx11_route = native::kDx11RouteUndecided;
      native::edit_reuse_enabled = false;
      native::stack_passes = 3;
      native::pass_stage[1] = 0u;
      native::pass_resolution_mode[0] = 1u;
      native::pass_resolution_mode[1] = 0u;
      native::ngx_slot_real[0].evaluate_c = reinterpret_cast<void*>(MirrorSr);
      auto evaluation = parameters;
      NVSDK_NGX_Handle* sr = nullptr;
      Check(NVSDK_NGX_SUCCEED(native::HookedCreateFeatureSlot<0>(gpu.list.Get(), native::kFeatureDlss, &evaluation, &sr))
                && sr != nullptr,
            "shared-grid and fallback contrast creates a native SR source through the wrapper");
      const auto evaluate_native = [&](bool mirror = false) {
        gpu.list->ClearState(nullptr);
        const auto sr_before = sr_calls;
        const auto nr_before = model_calls;
        const auto sequence = native::features.at(sr).sr_sequence;
        const auto result = (mirror ? native::HookedEvaluateFeatureCSlot<0>(gpu.list.Get(), sr, &evaluation, nullptr)
                                    : native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), sr, &evaluation, nullptr));
        Check(NVSDK_NGX_SUCCEED(result) && sr_calls == sr_before + 1
                  && native::features.at(sr).sr_sequence == sequence + 1,
              "native SR records one original Evaluate and advances one source frame");
        return model_calls - nr_before;
      };
      const auto read_output = [&](const char* phase) {
        const auto result = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit_commands(phase);
        native::DrainRetiredResources(true);
        return result.Pixels();
      };
      const auto& upscaled = native::stage_telemetry[static_cast<size_t>(STAGE)];
      bool shared_ready = false;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8 && !shared_ready; ++attempt) {
        const auto groups = upscaled.groups;
        shared_ready = evaluate_native() == 3 && upscaled.groups == groups + 1;
        submit_commands("warm shared Upscaled grid");
        native::DrainRetiredResources(true);
      }
      Check(shared_ready, "three Upscaled passes warm on the same 16x16 workset");

      // Pass 2 follows the real Guide subrect while Passes 1 and 3 stay at 100% of Output.
      evaluation.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 12u);
      evaluation.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 12u);
      const auto rejected_calls = evaluate_native();
      const auto rejected_rgba = read_output("shared Upscaled grid rejection");
      const bool original_preserved = rejected_rgba.size() == sizeof(inline_trace::SOURCE_RGBA)
          && std::memcmp(rejected_rgba.data(), inline_trace::SOURCE_RGBA.data(), rejected_rgba.size()) == 0;
      std::cout << "SHARED_UPSCALED_GRID guide=12x12 calls=" << rejected_calls << " original_rgba=" << original_preserved << '\n';
      Check(rejected_calls == 0 && original_preserved,
            "unequal shared grids issue no NR Evaluate and preserve the original SR RGBA");
      Check(upscaled.resolution_blocked && upscaled.pass_resolutions[0].requested_width == 16
                && upscaled.pass_resolutions[1].requested_width == 12 && upscaled.pass_resolutions[1].requested_height == 12
                && upscaled.pass_resolutions[2].requested_width == 16,
            "the shared group detects 16x16, 12x12 Guide and 16x16 working grids");
      evaluation = parameters;
      const auto recovered_calls = evaluate_native();
      const auto recovered_rgba = read_output("shared Upscaled grid recovery");
      Check(recovered_calls == 3 && !upscaled.resolution_blocked && recovered_rgba.size() == rejected_rgba.size()
                && recovered_rgba != rejected_rgba,
            "restoring the 16x16 Guide recovers all three model calls and the edited output on the next SR");
      for (size_t offset = 6; offset < recovered_rgba.size(); offset += 8) {
        Check(std::memcmp(recovered_rgba.data() + offset, rejected_rgba.data() + offset, sizeof(uint16_t)) == 0,
              "the recovered shared workset preserves every original FP16 alpha");
      }
      std::cout << "PASS shared Upscaled grid Guide=12x12 rejected_calls=0 original_rgba=1 recovery_guide=16x16 calls=3 evidence=WARP+MOCK\n";

      native::stack_passes = 1;
      const addon::presets::Json render{{"NRHookPoint", native::kHookRender}};
      Check(native::last_source_plan.source_handle == sr && native::QueryConfigurationCapability(render).CanSubmit(),
            "the ordinary native NGX source allows a Render configuration");
      native::StreamlineCapture capture{.viewport = 1, .has_viewport = true, .has_constants = true,
          .motion_scale_x = 1.f / 16.f, .motion_scale_y = 1.f / 16.f};
      capture.color.resource = source.Get();
      capture.color.state = NATIVE_INPUT_STATE;
      capture.output.resource = output.Get();
      capture.output.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      capture.motion.resource = wrapper_motion.Get();
      capture.motion.state = NATIVE_INPUT_STATE;
      capture.depth.resource = wrapper_depth.Get();
      capture.depth.state = NATIVE_INPUT_STATE;
      const auto record_fallback = [&] {
        const addon::RuntimeLock lock(native::runtime_mutex);
        return native::ProcessStreamlineInline(gpu.list.Get(), sl::kFeatureDLSS, capture);
      };
      const auto plan_before_fallback = native::last_source_plan.sequence;
      native::hook_point = native::kHookRender;
      const auto calls_before_render = model_calls;
      const bool rendered = record_fallback();
      const auto& observed = native::last_source_plan;
      const auto* fallback = observed.source_handle;
      const auto first_render_query = native::QueryConfigurationCapability();
      Check(!rendered && model_calls == calls_before_render && observed.sequence > plan_before_fallback
                && observed.plan.At(addon::stage_plan::Stage::RENDER).count == 1
                && fallback != sr && native::synthetic_handles.contains(fallback)
                && native::stage_source_api.load() == addon::capability::Api::DX12
                && first_render_query.configuration.reason == addon::capability::Reason::BACKEND_UNSUPPORTED,
            "the first declined Streamline Render request observes the fallback source before querying capability");
      native::hook_point = native::kHookUpscaled;
      const auto plan_before_upscaled = observed.sequence;
      std::vector<addon::capability::Size> fallback_model_sizes;
      const auto warm_fallback = [&](uint32_t expected_calls, const char* phase) {
        for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8; ++attempt) {
          fallback_model_sizes.clear();
          gpu.list->ClearState(nullptr);
          // Streamline supplies a finished image via tags, with no game NGX callback or success scope.
          upload(output.Get(), source_values, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 16 * 8);
          const auto nr_before = model_calls;
          if (record_fallback() && model_calls == nr_before + expected_calls) return read_output(phase);
          submit_commands("warm Streamline Upscaled fallback");
          native::DrainRetiredResources(true);
        }
        return std::vector<uint8_t>{};
      };
      const auto fallback_rgba = warm_fallback(1, "Streamline Upscaled fallback delivery");
      Check(fallback_rgba.size() == rejected_rgba.size() && fallback_rgba != rejected_rgba,
            "the standalone Streamline fallback delivers a real Upscaled edit");
      Check(observed.sequence > plan_before_upscaled && observed.plan.valid && observed.plan.At(STAGE).count == 1
                && observed.source_handle == fallback && !native::create_contracts.contains(fallback),
            "Upscaled resumes actual delivery on the same synthetic Streamline source");
      const auto fallback_render = native::QueryConfigurationCapability(render);
      Check(native::QueryConfigurationCapability().CanSubmit() && !fallback_render.CanSubmit()
                && fallback_render.configuration.reason == addon::capability::Reason::BACKEND_UNSUPPORTED,
            "the observed Streamline fallback admits Upscaled and rejects DLSS Render");

      native::stack_passes = 2;
      native::pass_resolution_mode[0] = 2u;
      native::pass_resolution_scale[0] = .75f;
      const auto split = native::QueryConfigurationCapability();
      constexpr addon::capability::Size full_grid{.width = 16, .height = 16};
      constexpr addon::capability::Size tail_grid{.width = 12, .height = 12};
      Check(split.CanSubmit() && split.group_count == 1 && split.groups[0].independent_sizes_allowed
                && split.working_sizes[0] == full_grid && split.working_sizes[1] == tail_grid,
            "Streamline fallback admits the existing independent Upscaled working grids");
      {
        funnel_accounting_tests::Restore model_restore;
        model_restore.Value(&model_parameter_observer);
        model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter* block) {
          auto* input = native::GetD3D12Resource(block, "DLSSNR.Color");
          Check(input != nullptr, "the Streamline fallback model receives a real color texture");
          const auto desc = input->GetDesc();
          fallback_model_sizes.push_back({.width = static_cast<uint32_t>(desc.Width), .height = desc.Height});
        };
        const auto split_rgba = warm_fallback(2, "Streamline split Upscaled fallback delivery");
        Check(fallback_model_sizes.size() == 2 && fallback_model_sizes[0] == full_grid && fallback_model_sizes[1] == tail_grid
                  && split_rgba.size() == rejected_rgba.size() && split_rgba != rejected_rgba,
              "Streamline fallback delivers two model calls on the 16x16 and 12x12 worksets and edits the full SR output");
      }
      std::cout << "PASS Streamline split Upscaled calls=2 grids=16x16/12x12 evidence=WARP+MOCK\n";
      native::stack_passes = 1;
      {
        funnel_accounting_tests::Restore present_restore;
        present_restore.Atomic(&native::present_unpresented_evaluates);
        present_restore.Value(&native::present_fallback_episode);
        native::hook_point = native::kHookPresent;
        native::edit_reuse_enabled = true;
        const auto calls_before = model_calls;
        const auto captures_before = native::present_captures.load();
        const auto serial_before = native::present_capture_serial;
        const bool served_inline = record_fallback();
        submit_commands("Streamline ordinary Present guide capture");
        const auto present = native::QueryConfigurationCapability();
        const auto guides = std::ranges::find_if(native::present_guide_sets, [&](const auto& set) {
          return set.source_handle == fallback && set.serial > serial_before;
        });
        Check(!served_inline && model_calls == calls_before && native::present_captures.load() == captures_before + 1
                  && guides != std::end(native::present_guide_sets) && guides->motion != nullptr && guides->depth != nullptr
                  && observed.source_handle == fallback && observed.plan.At(addon::stage_plan::Stage::PRESENT).count == 1
                  && present.CanSubmit() && present.plan_reason == addon::stage_plan::Reason::NONE,
              "ordinary Streamline Present captures guides with zero inline model calls and remains configurable");
        const auto& feature = native::features.at(fallback);
        Check(observed.source_generation == 0 && feature.source_generation == 0 && feature.sr_sequence == 0
                  && !native::EditReuseSourceConfirmed(fallback, feature) && guides->base_frame.source == nullptr,
              "ordinary Streamline Present guides carry no successful SR identity for strict edit reuse");
        const addon::RuntimeLock lock(native::runtime_mutex);
        native::ClearPresentSource(guides);
        native::RetirePresentResource(&guides->motion);
        native::RetirePresentResource(&guides->depth);
        *guides = {};
        native::DrainPresentRetired();
      }
      std::cout << "PASS Streamline first Render calls=0; ordinary Present captures=1 inline_calls=0 strict_source=0 evidence=WARP+MOCK\n";
      native::hook_point = native::kHookUpscaled;
      native::edit_reuse_enabled = false;
      evaluate_native(true);
      submit_commands("nested native NGX source after Streamline fallback");
      Check(native::last_source_plan.source_handle == sr && native::QueryConfigurationCapability(render).CanSubmit(),
            "a nested native NGX Evaluate restores native Render eligibility while the fallback stream still exists");
      native::real_streamline_free[0] = +[](sl::Feature, const sl::ViewportHandle&) { return sl::Result::eOk; };
      Check(native::HookedStreamlineFreeResources<0>(sl::kFeatureDLSS, sl::ViewportHandle(capture.viewport)) == sl::Result::eOk
                && !native::synthetic_handles.contains(fallback) && !native::features.contains(fallback),
            "Streamline free releases the fallback source and its synthetic identity");
      Check(NVSDK_NGX_SUCCEED(native::HookedReleaseFeatureSlot<0>(sr)), "release the native source after the fallback contrast");
      native::DrainRetiredResources(true);
      std::cout << "PASS standalone Streamline Upscaled delivery and native/Streamline Render capability contrast evidence=WARP+MOCK\n";
    }

    const auto* owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xED170});
    native::RegisterCreatedFeatureLocked(owner, native::kFeatureDlss, &parameters);
    native::created_feature_ids[owner] = native::kFeatureDlss;
    const auto source_frame = [&](const NVSDK_NGX_Handle* source_handle, int reset, bool submit, bool mirror) {
      parameters.Set(NVSDK_NGX_Parameter_Reset, reset);
      gpu.list->ClearState(nullptr);
      const auto before = model_calls;
      const auto sequence = native::features.at(source_handle).sr_sequence;
      native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(CopySr);
      native::ngx_slot_real[0].evaluate_c = reinterpret_cast<void*>(MirrorSr);
      const auto result = (mirror ? native::HookedEvaluateFeatureCSlot<0>(gpu.list.Get(), source_handle, &parameters, nullptr)
                                  : native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), source_handle, &parameters, nullptr));
      native::ngx_slot_real[0].evaluate = nullptr;
      native::ngx_slot_real[0].evaluate_c = nullptr;
      Check(NVSDK_NGX_SUCCEED(result), "copy SR remains successful through cache wrapper");
      Check(native::features.at(source_handle).sr_sequence == sequence + 1, "nested C/C++ SR mirror advances one source sample");
      if (submit) {
        submit_commands("wrapper frame");
        native::ResolveAdaptiveBorrowedDeliveries();
        native::DrainRetiredResources(true);
      }
      return model_calls - before;
    };
    const auto frame = [&](int reset, bool submit = true, bool mirror = false) {
      return source_frame(owner, reset, submit, mirror);
    };
    for (uint32_t prefix = 0; prefix < native::kContractDebounceFrames + 8; ++prefix) frame(1);
    Check(native::features.at(owner).slots[0].edit_reuse != nullptr, "production wrapper establishes an edit anchor");
#if defined(NR_NEXUS_DEV_CONTROL)
    {
      namespace benchmark = addon::control_mask_capture::benchmark;
      namespace timers = addon::gpu_timers;
      funnel_accounting_tests::Restore restore;
      restore.Atomic(&native::gpu_timers_enabled);
      restore.Atomic(&native::control_mask_use_core_context);
      restore.Value(&timers::benchmark_observer);
      restore.Value(&native::ngx_slot_real[0]);
      restore.Value(&native::ngx_slot_real[1]);
      native::gpu_timers_enabled = true;
      native::control_mask_use_core_context = false;
      timers::benchmark_observer = benchmark::ObserveTiming;
      native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(MirrorSrCpp);
      native::ngx_slot_real[1].evaluate = reinterpret_cast<void*>(CopySr);
      Check(benchmark::Arm(2, 30, nullptr, nullptr), "arm a two-source measurement for the duplicate C++ export path");
      for (size_t index = 0; index < 2; ++index) {
        parameters.Set(NVSDK_NGX_Parameter_Reset, index == 0 ? 1 : 0);
        gpu.list->ClearState(nullptr);
        const auto recording = addon::submission::RecordingIdentity(gpu.list.Get());
        const auto calls_before = model_calls;
        Check(NVSDK_NGX_SUCCEED(native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr)),
            "outer game SR returns success through the inner C++ export");
        // The production queue hook owns the proof. The generic fixture
        // submit would register a second receipt and prune before the timer.
        Check(SUCCEEDED(gpu.list->Close()), "nested benchmark recording close");
        ID3D12CommandList* lists[]{gpu.list.Get()};
        gpu.queue->ExecuteCommandLists(1, lists);
        gpu.submission_pending = true;
        Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "nested benchmark completion signal");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(event != nullptr, "nested benchmark completion event");
        const auto armed = gpu.fence->SetEventOnCompletion(gpu.serial, event);
        const auto wait = (SUCCEEDED(armed) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
        CloseHandle(event);
        Check(wait == WAIT_OBJECT_0, "bounded nested benchmark completion");
        gpu.submission_pending = false;
        Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
            "nested benchmark recording reset");
        native::DrainRetiredResources(true);
        timers::Poll(true, false);
        gpu.uploads.clear();
        gpu.retained_resources.clear();
        std::shared_lock lock(benchmark::state.mutex);
        Check(benchmark::state.rows.size() == index + 1 && benchmark::state.frame_count == index + 1,
            "publish one row per successful outer SR call");
        const auto& row = benchmark::state.rows[index];
        const bool refresh = index == 0;
        Check(row.wrapper_finalized && row.frame.sr_success_ns >= benchmark_inner_return_ns
                  && row.frame.source_sequence == index + 1 && row.frame.passes_recorded == 1
                  && row.frame.delivery == (refresh ? benchmark::Delivery::kRefresh : benchmark::Delivery::kReuse)
                  && row.frame.passes[0].calls == (refresh ? 1u : 0u)
                  && model_calls - calls_before == (refresh ? 1u : 0u),
            "join the inner refresh/reuse to the authoritative outer source timestamp");
        Check(row.frame.recording != nullptr && row.frame.recording->list == recording.first
                  && row.frame.recording->generation == recording.second && row.timing.valid && row.timing.samples == 1
                  && row.timing.recorded_ns < benchmark_inner_return_ns,
            "the nested writeback's completed GPU timer uses the exact source recording");
      }
      Check(benchmark::Stop("fixture_complete", nullptr, true), "finish the duplicate C++ export measurement");
      benchmark::Tick(timers::NowNs(), false);
      benchmark::write_pending = false;
      std::unique_lock lock(benchmark::state.mutex);
      benchmark::state.rows.clear();
      benchmark::state.sources.clear();
      benchmark::state.status = "idle";
    }
#endif
    Check(frame(1) == 1, "Reset forces a real NR refresh");
    auto anchor_state = native::features.at(owner).slots[0].edit_reuse;
    const auto generation = anchor_state->anchor_generation;
    const auto wrapper_anchor = read_buffer(anchor_state->anchor.Get());
    const auto anchor_reuses = anchor_state->reuse_count;
    const auto before_reuses = native::edit_reuse_reuses.load();
    Check(frame(0, true, true) == 0 && native::edit_reuse_reuses.load() == before_reuses + 1,
          "nested mirrored source reuses once with zero actual NR calls");
    Check(anchor_state == native::features.at(owner).slots[0].edit_reuse && anchor_state->reuse_count == anchor_reuses + 1
              && anchor_state->anchor_generation == generation && read_buffer(anchor_state->anchor.Get()) == wrapper_anchor,
          "one reuse retains the exact refresh anchor and generation");
    auto neutral_final = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    submit_commands("wrapper neutral readback");
    const auto neutral_bytes = neutral_final.Pixels();
    Check(std::memcmp(neutral_bytes.data(), source_values.data(), neutral_bytes.size()) == 0,
          "production neutral reuse returns exact current half-float source and alpha");
    Check(frame(0) == 1, "the second following base frame refreshes rather than reusing a second time");
    Check(frame(1) == 1 && frame(1) == 1, "successive explicit Reset frames both run NR");
    const auto set_native_fg_mode = [](bool enabled) {
      namespace options = addon::fg::native_options;
      funnel_accounting_tests::Restore substitutions;
      substitutions.Value(&options::internal::state.real);
      substitutions.Atomic(&options::internal::options_changed);
      substitutions.Atomic(&options::internal::present_observed);
      substitutions.Atomic(&options::internal::stopped);
      options::internal::state.real = +[](const sl::ViewportHandle&, const sl::DLSSGOptions&) {
        return sl::Result::eOk;
      };
      options::internal::stopped = false;
      options::Configure(native::OnNativeFgOptions);

      // The SDK can call SetOptions while the SR wrapper holds this lock.
      addon::RuntimeLock lock(native::runtime_mutex);
      Check(!native::native_fg_owns.load(), "Upscaled Options contrast starts without a native owner");
      const bool changed = native::native_fg_public_off.load() == enabled;
      const auto generation = native::native_fg_options_generation.load();
      sl::DLSSGOptions request;
      request.mode = (enabled ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff);
      request.numFramesToGenerate = 1;
      Check(options::internal::HookedSetOptions(sl::ViewportHandle{0}, request) == sl::Result::eOk
                && native::native_fg_public_off.load() == !enabled
                && native::native_fg_declaration.load() == (enabled ? native::NativeFgDeclaration::kOn
                                                                   : native::NativeFgDeclaration::kOff)
                && native::native_fg_options_generation.load() == generation + changed
                && !native::native_fg_owns.load(),
            "successful SetOptions updates only the native declaration interval without acquiring ownership");
    };
    {
      edit_model_behavior = ConstantModel;
      auto& feature = native::features.at(owner);
      const auto source_generation = feature.source_generation;
      const auto configuration = native::configuration_generation.load();
      auto reset_epoch = native::history_reset_epoch.load();
      const auto models = native::nr_features_created;
      const auto worksets = native::worksets_created;
      const auto model = feature.slots[0].handle;
      const auto anchor = feature.slots[0].edit_reuse;
      auto anchor_generation = anchor->anchor_generation;
      const auto reuses = native::edit_reuse_reuses.load();
      const auto fallbacks = native::edit_reuse_fallbacks.load();
      std::vector<uint8_t> refresh_pixels, anchor_pixels;
      using Declaration = native::NativeFgDeclaration;
      const std::array declarations{Declaration::kOff, Declaration::kUnknown, Declaration::kUnknown,
          Declaration::kOff, Declaration::kOff, Declaration::kOn, Declaration::kOn, Declaration::kOff, Declaration::kOff};
      for (size_t index = 0; index < declarations.size(); ++index) {
        const auto declaration = declarations[index];
        if (index >= 4) {
          // Start with a refreshed Off frame and exercise the callback on both frame phases.
          set_native_fg_mode(declaration == Declaration::kOn);
        } else {
          // Unknown models the absence of a public Options declaration.
          native::native_fg_public_off = declaration == Declaration::kOff;
          native::native_fg_declaration = declaration;
        }
        const bool refresh = index % 2 == 0;
        const auto reuse_count = anchor->reuse_count;
        const auto calls = frame(index == 0 ? 1 : 0);
        if (index == 0) {
          reset_epoch = native::history_reset_epoch.load();
        }
        std::cout << "OBS fixed-size Upscaled frame=" << index + 1
                  << " declaration=" << (declaration == Declaration::kOff ? "Off" : (declaration == Declaration::kOn ? "On" : "Unknown"))
                  << " model_calls=" << calls << " expected=" << (refresh ? 1 : 0)
                  << " reuses=" << native::edit_reuse_reuses.load() - reuses << '\n';
        Check(calls == (refresh ? 1u : 0u), "owner-free declaration changes preserve single-Pass cadence");
        Check(feature.source_generation == source_generation && native::configuration_generation.load() == configuration
                  && native::history_reset_epoch.load() == reset_epoch && feature.slots[0].handle == model
                  && feature.slots[0].edit_reuse == anchor && native::nr_features_created == models
                  && native::worksets_created == worksets && native::edit_reuse_fallbacks.load() == fallbacks,
              "declaration-only changes preserve the live source, model, workset and edit cache");
        if (refresh) {
          Check(model_resets.back() == (index == 0 ? 1 : 0), "declaration-only changes add no NR History Reset");
          ++anchor_generation;
          anchor_pixels = read_buffer(anchor->anchor.Get());
        } else {
          Check(anchor->reuse_count == reuse_count + 1 && read_buffer(anchor->anchor.Get()) == anchor_pixels,
                "declaration changes preserve the exact nonzero refresh anchor on reuse");
        }
        Check(anchor->valid && anchor->anchor_generation == anchor_generation
                  && native::edit_reuse_reuses.load() == reuses + (index + 1) / 2,
              "only refresh advances the anchor generation and each adjacent frame reuses once");
        auto final = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit_commands("single-Pass declaration contrast readback");
        const auto pixels = final.Pixels();
        if (refresh) {
          refresh_pixels = pixels;
        }
        bool nonneutral = false;
        for (size_t pixel = 0; pixel < source_values.size() / 4; ++pixel) {
          for (size_t channel = 0; channel < 3; ++channel) {
            uint16_t refreshed, actual;
            std::memcpy(&refreshed, refresh_pixels.data() + (pixel * 4 + channel) * 2, 2);
            std::memcpy(&actual, pixels.data() + (pixel * 4 + channel) * 2, 2);
            Check(std::fabs(DirectX::PackedVector::XMConvertHalfToFloat(actual)
                      - DirectX::PackedVector::XMConvertHalfToFloat(refreshed)) < .003f,
                  "declaration contrast reads back the refreshed nonzero RGB edit");
            nonneutral |= std::fabs(DirectX::PackedVector::XMConvertHalfToFloat(actual)
                - DirectX::PackedVector::XMConvertHalfToFloat(source_values[pixel * 4 + channel])) > .05f;
          }
          Check(std::memcmp(pixels.data() + pixel * 8 + 6, source_values.data() + pixel * 4 + 3, 2) == 0,
                "declaration contrast preserves original alpha exactly");
        }
        Check(nonneutral, "declaration contrast output contains a distinguishable nonzero edit");
      }
      native::native_fg_public_off = false;
      native::native_fg_declaration = native::NativeFgDeclaration::kUnknown;
      native::create_contracts.at(owner).source_type_confirmed = false;
      const auto rejected_reuses = native::edit_reuse_reuses.load();
      Check(frame(0) == 1 && native::edit_reuse_reuses.load() == rejected_reuses && feature.slots[0].edit_reuse == nullptr,
            "Unknown FG cannot authorize an unconfirmed source");
      native::create_contracts.at(owner).source_type_confirmed = true;
      Check(frame(1) == 1, "the confirmed source recovers its own anchor under Unknown FG");
      ++native::create_contracts.at(owner).source_generation;
      Check(frame(0) == 1 && native::edit_reuse_reuses.load() == rejected_reuses && feature.slots[0].edit_reuse == nullptr,
            "Unknown FG cannot authorize a mismatched source generation");
      native::create_contracts.at(owner).source_generation = source_generation;
      Check(frame(1) == 1, "matching source generation restores refresh under Unknown FG");
      native::native_fg_owns = true;
      frame(0);
      Check(native::edit_reuse_reuses.load() == rejected_reuses && feature.slots[0].edit_reuse == nullptr,
            "Unknown FG cannot cross actual native Present-service ownership");
      native::native_fg_owns = false;
      Check(frame(1) == 1, "Upscaled ownership recovers with a refresh under Unknown FG");
      native::native_fg_declaration = native::NativeFgDeclaration::kOn;
      Check(frame(0) == 0
                && native::edit_reuse_reuses.load() == rejected_reuses + 1 && feature.slots[0].edit_reuse != nullptr,
            "native On declaration alone preserves fixed Upscaled reuse alongside the direct SR writer");
      native::native_fg_public_off = true;
      native::native_fg_declaration = native::NativeFgDeclaration::kOff;
      edit_model_behavior = NeutralModel;
      Check(frame(1) == 1, "restore the neutral model with an explicit source Reset after the declaration contrast");
      std::cout << "PASS single-Pass Upscaled Off/Unknown/Off and owner-free SetOptions Off/On/Off:"
                   " model_calls=1/0/1/0/1 per contrast, stable history, nonzero RGB, exact alpha; evidence=WARP+MOCK\n";
    }
    {
      auto& feature = native::features.at(owner);
      auto& slot = feature.slots[0];
      const auto b_anchor = slot.edit_reuse;
      const auto before_auto_reuses = native::edit_reuse_reuses.load();
      native::preset = 0;
      Check(frame(0) == 0 && slot.nr_preset == 2 && slot.edit_reuse == b_anchor
                && native::edit_reuse_reuses.load() == before_auto_reuses + 1,
            "a live B anchor also accepts Auto when its resolved request is B");

      const auto saved_style = native::style.load();
      const auto saved_skin = native::skin_structure_strength.load();
      const auto saved_skin_independent = native::skin_independent.load();
      const auto saved_runtime = native::direct_runtime_requirements;
      native::direct_runtime_requirements.file_version = "310.8.0.0";
      native::direct_runtime_requirements.dual_models = false;
      native::preset = 1;
      native::detail_stability = 0;
      native::use_auto_mask = true;
      native::control_mask_enabled = true;
      native::style = 2;
      native::skin_structure_strength = .8f;
      native::skin_independent = true;
      native::mixed_present_fg_seen = true;
      native::configuration_generation.fetch_add(1);
      edit_model_behavior = ConstantModel;
      const ID3D12Resource* model_motion = nullptr;
      model_parameter_observer = [&](const NVSDK_NGX_Handle* model, const NVSDK_NGX_Parameter* block) {
        Check(model == slot.handle && slot.nr_preset == 1 && native::GetInt(block, "DLSSNR.UseAutoMask") == 1
                  && native::GetD3D12Resource(block, "DLSSNR.ControlMask") == nullptr
                  && native::GetUInt(block, "DLSSNR.Style") == 2u
                  && native::GetFloat(block, "DLSSNR.Intensity") == 1.f
                  && native::GetFloat(block, "DLSSNR.SkinStructureStrength") == .8f,
              "legacy A refresh sends the configured steering and AutoMask while the Region Mask request is declined");
        model_motion = native::GetD3D12Resource(block, "DLSSNR.MVec");
      };
      Check(frame(1) == 0, "A keeps the existing feature maturation frame");
      const auto stabilized = native::look_stability_passes.load();
      Check(frame(1) == 1 && slot.model_reset && slot.look_history.valid && slot.look_history.whole_edit,
            "Upscaled Auto runs whole-edit Look on the real A model refresh and honors Reset");
      const auto restarts = native::look_history_restarts.load();
      const auto motion_missing = native::look_motion_missing_passes.load();
      const auto refresh_history = slot.look_history.written;
      const auto before_a_reuses = native::edit_reuse_reuses.load();
      const auto before_a_fallbacks = native::edit_reuse_fallbacks.load();
      std::vector<uint16_t> moving(16 * 16 * 2, 0);
      for (size_t pixel = 0; pixel < moving.size() / 2; ++pixel) {
        moving[pixel * 2] = DirectX::PackedVector::XMConvertFloatToHalf(.5f);
      }
      std::vector<float> revealed(16 * 16, .5f);
      revealed[0] = .9f;
      upload(wrapper_motion.Get(), moving, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, 16 * 4);
      upload(wrapper_depth.Get(), revealed, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, 16 * 4);
      submit_commands("A with active Look motion and reveal upload");
      Check(frame(0) == 0 && native::mixed_present_fg_seen.load() && native::edit_reuse_reuses.load() == before_a_reuses + 1
                && native::edit_reuse_fallbacks.load() == before_a_fallbacks,
            "legacy A reuses on fixed Upscaled despite a declined Region Mask request and an old FG observation");
      Check(slot.look_history.valid && slot.look_history.written != refresh_history
                && native::look_stability_passes.load() == stabilized + 2
                && native::look_history_restarts.load() == restarts
                && native::look_motion_missing_passes.load() == motion_missing,
            "A reuse advances Look with motion and without replaying the previous model Reset");
      auto reused_final = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      submit_commands("A with active Look output readback");
      const auto pixels = reused_final.Pixels();
      Check(std::memcmp(pixels.data(), source_values.data(), 4 * sizeof(uint16_t)) == 0,
            "the depth reveal selects exact current original RGBA after Look on the x2 reuse frame");
      constexpr size_t interior = (8 * 16 + 8) * 4;
      uint16_t edited_red = 0;
      std::memcpy(&edited_red, pixels.data() + interior * sizeof(uint16_t), sizeof(edited_red));
      Check(std::abs(DirectX::PackedVector::XMConvertHalfToFloat(edited_red)
                     - DirectX::PackedVector::XMConvertHalfToFloat(source_values[interior])) > .05f,
            "a valid interior pixel retains a visible edit through the active Look path");
      Check(frame(0) == 1 && model_resets.back() == 0 && native::look_stability_passes.load() == stabilized + 3
                && native::look_history_restarts.load() == restarts,
            "next A refresh keeps adjacent-motion Look history while the model advances across the reused frame");
      Check(slot.edit_reuse != nullptr && model_motion == slot.edit_reuse->composed_motion.Get(),
            "only the model consumes the combined two-frame motion on the next refresh");
      bool look_used_adjacent_motion = false;
      for (const auto& [key, resources] : native::worksets) {
        if (key.handle != owner) continue;
        for (const auto* motion : resources.motion_views) {
          if (motion == nullptr) continue;
          Check(motion == wrapper_motion.Get(), "Look binds only current source motion on each base frame");
          look_used_adjacent_motion = true;
        }
      }
      Check(look_used_adjacent_motion, "Look has a current source motion binding");
      Check(frame(1) == 1 && model_resets.back() == 1 && native::look_history_restarts.load() == restarts + 1,
            "a current source Reset restarts Look and forces a model refresh");

      model_parameter_observer = {};
      edit_model_behavior = NeutralModel;
      native::direct_runtime_requirements = saved_runtime;
      native::preset = 2;
      native::detail_stability = 1;
      native::use_auto_mask = false;
      native::control_mask_enabled = false;
      native::style = saved_style;
      native::skin_structure_strength = saved_skin;
      native::skin_independent = saved_skin_independent;
      native::mixed_present_fg_seen = false;
      upload(wrapper_motion.Get(), std::vector<uint16_t>(16 * 16 * 2, 0), NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, 16 * 4);
      upload(wrapper_depth.Get(), std::vector<float>(16 * 16, .5f), NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, 16 * 4);
      submit_commands("restore neutral wrapper guides");
    }
    native::intensity = .75f;
    native::configuration_generation.fetch_add(1);
    const auto before_settings_reuses = native::edit_reuse_reuses.load();
    Check(frame(0) == 0 && native::edit_reuse_reuses.load() == before_settings_reuses,
          "committed parameter change recreates NR without reusing the old edit");
    Check(frame(0) == 1 && model_resets.back() == 1 && native::edit_reuse_reuses.load() == before_settings_reuses,
          "the recreated NR refreshes with Reset after its existing maturation frame");
    {
      funnel_accounting_tests::Restore restore;
      restore.Value(&present_fixture_ini);
      restore.Value(&model_parameter_observer);
      restore.Value(&edit_model_behavior);
      restore.Atomic(&native::detail_stability);
      restore.Atomic(&native::control_mask_enabled);
      restore.Atomic(&native::control_mask_red);
      restore.Atomic(&native::monitor_requested);
      present_fixture_ini = std::filesystem::current_path() / "x2-request-history.ini";
      native::detail_stability = 2;
      native::control_mask_red = 0.5f;
      native::monitor_requested = true;
      edit_model_behavior = ConstantModel;
      native::kSetEditReuse.Commit(0.f);

      const auto* masked = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xED179});
      native::RegisterCreatedFeatureLocked(masked, native::kFeatureDlss, &parameters);
      auto& masked_feature = native::features.at(masked);
      auto& masked_slot = masked_feature.slots[0];
      auto& editing_feature = native::features.at(owner);
      auto& editing_slot = editing_feature.slots[0];
      const ID3D12Resource* model_motion = nullptr;
      bool model_used_mask = false;
      model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter* block) {
        model_used_mask = native::GetD3D12Resource(block, "DLSSNR.ControlMask") != nullptr;
        model_motion = native::GetD3D12Resource(block, "DLSSNR.MVec");
      };
      const auto evaluate = [&](const NVSDK_NGX_Handle* source_handle, bool use_mask) {
        native::control_mask_enabled = use_mask;
        const auto calls = source_frame(source_handle, 0, true, false);
        if (calls != 0) {
          Check(calls == 1 && model_used_mask == use_mask, "the SR wrapper sends only the current source's actual external Mask");
        }
        return calls;
      };
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8 && !masked_slot.look_history.valid; ++attempt) {
        evaluate(masked, true);
      }
      Check(masked_slot.look_history.valid && evaluate(owner, false) == 1 && editing_slot.look_history.valid
                && editing_slot.edit_reuse == nullptr,
            "both sources have continuous Look before requesting x2");
      const auto reset_epoch = native::history_reset_epoch.load();
      const auto restore_epoch = native::present_restore_configuration.epoch;
      const auto look_restarts = native::look_history_restarts.load();
      const auto check_masked_continuity = [&] {
        const auto written = masked_slot.look_history.written;
        Check(evaluate(masked, true) == 1 && model_resets.back() == 0 && masked_slot.edit_reuse == nullptr
                  && masked_slot.look_history.written != written && native::look_history_restarts.load() == look_restarts,
              "the actual Mask keeps the source per-frame and Look advances through x2 request changes");
      };
      native::kSetEditReuse.Commit(1.f);
      check_masked_continuity();
      Check(masked_feature.edit_reuse_monitor[0].sequence == masked_feature.sr_sequence
                && masked_feature.edit_reuse_monitor[0].delivery == native::EditReuseDelivery::PER_FRAME
                && masked_feature.edit_reuse_monitor[0].reason == native::EditReuseBlock::MASK,
            "the production monitor attributes per-frame delivery to the actual Mask");
      Check(evaluate(owner, false) == 1 && model_resets.back() == 0 && editing_slot.edit_reuse != nullptr
                && editing_slot.edit_reuse->valid && native::look_history_restarts.load() == look_restarts,
            "enabling x2 seeds an anchor from a real Evaluate with continuous model and Look history");
      const auto old_anchor = editing_slot.edit_reuse;
      native::kSetEditReuse.Commit(0.f);
      native::kSetEditReuse.Commit(1.f);
      const auto look_before_reuse = editing_slot.look_history.written;
      const auto reuse_count = native::edit_reuse_reuses.load();
      Check(evaluate(owner, false) == 0 && native::edit_reuse_reuses.load() == reuse_count + 1
                && editing_slot.edit_reuse == old_anchor && editing_slot.edit_reuse->reused
                && editing_slot.look_history.written != look_before_reuse
                && editing_feature.edit_reuse_monitor[0].sequence == editing_feature.sr_sequence
                && editing_feature.edit_reuse_monitor[0].delivery == native::EditReuseDelivery::REUSED,
            "request round-trip between source frames preserves the anchor and the adjacent reuse advances Look");
      std::string error;
      const addon::presets::Document disable{
          .scope = addon::presets::Scope::QUALITY, .settings = {{"NREditReuse", false}}};
      Check(native::preset_ui::Apply(disable, false, true, &error) && !native::edit_reuse_enabled.load()
                && native::history_reset_epoch.load() == reset_epoch
                && native::present_restore_configuration.epoch == restore_epoch,
            "the real preset transaction turns off the request without publishing either epoch");
      check_masked_continuity();
      const auto look_before_recovery = editing_slot.look_history.written;
      Check(evaluate(owner, false) == 1 && model_resets.back() == 1 && model_motion == wrapper_motion.Get()
                && editing_slot.model_reset_reasons == native::kResetEditReuseGap && editing_slot.edit_reuse == nullptr
                && editing_slot.look_history.written != look_before_recovery
                && native::look_history_restarts.load() == look_restarts,
            "without a composed guide the skipped source gives its next real model a local Reset and keeps adjacent Look");
      Check(evaluate(owner, false) == 1 && model_resets.back() == 0,
            "the actual local recovery is consumed exactly once");
      Check(native::preset_ui::Undo(&error) && native::edit_reuse_enabled.load()
                && native::history_reset_epoch.load() == reset_epoch
                && native::present_restore_configuration.epoch == restore_epoch,
            "real preset Undo restores the x2 request without publishing either epoch");
      check_masked_continuity();
      Check(evaluate(owner, false) == 1 && model_resets.back() == 0 && editing_slot.edit_reuse != nullptr
                && editing_slot.edit_reuse != old_anchor && editing_slot.edit_reuse->valid
                && native::look_history_restarts.load() == look_restarts,
            "Undo seeds a new x2 anchor without resetting either continuous history");

      native::RequestHistoryReset();
      const auto explicit_epoch = native::history_reset_epoch.load();
      Check(explicit_epoch == reset_epoch + 1 && masked_slot.acknowledged_reset_epoch == reset_epoch
                && editing_slot.acknowledged_reset_epoch == reset_epoch,
            "explicit global Reset remains pending per actual slot before Evaluate");
      Check(evaluate(masked, true) == 1 && model_resets.back() == 1 && masked_slot.acknowledged_reset_epoch == explicit_epoch
                && editing_slot.acknowledged_reset_epoch == reset_epoch,
            "the masked source acknowledges explicit Reset only on its own real Evaluate");
      Check(evaluate(owner, false) == 1 && model_resets.back() == 1 && editing_slot.acknowledged_reset_epoch == explicit_epoch
                && native::look_history_restarts.load() == look_restarts + 2 && evaluate(owner, false) == 0
                && editing_slot.acknowledged_reset_epoch == explicit_epoch,
            "explicit Reset forces a refresh and restarts Look while subsequent reuse acknowledges nothing new");
      native::ForgetSourceHandleLocked(masked);
      native::DrainRetiredResources(true);
      std::cout << "NR_RELEASE_PARITY dx12-upscaled-adaptive-x2 request-history mask_continuous=1 x2_local_reset=1"
                   " look_continuous=1 preset_undo=1 explicit_reset_slots=2 evidence=WARP+MOCK\n";
    }
    Check(frame(1) == 1, "establish fresh anchor before synthetic SR gap");
    ++native::features.at(owner).sr_sequence;
    const auto before_gap_reuses = native::edit_reuse_reuses.load();
    Check(frame(0) == 1 && native::edit_reuse_reuses.load() == before_gap_reuses,
          "source sample gap cannot reuse stale edit or compose across the missing motion");
    Check(model_resets.back() == 1, "missing source sample resets the real NR history");
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, .25f);
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, -.375f);
    const auto before_jitter_reuses = native::edit_reuse_reuses.load();
    const auto before_jitter_fallbacks = native::edit_reuse_fallbacks.load();
    Check(frame(0) == 0 && native::edit_reuse_reuses.load() == before_jitter_reuses + 1,
          "complete nonzero jitter retains x2 reuse cadence");
    const auto& jitter_cache = native::features.at(owner).slots[0].edit_reuse;
    Check(jitter_cache->motion_offset_x == .25f && jitter_cache->motion_offset_y == -.375f,
          "skipped physical-frame motion retains its own two-axis raster offset");
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, -.375f);
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, .25f);
    Check(frame(0) == 1 && native::edit_reuse_fallbacks.load() == before_jitter_fallbacks,
          "next nonzero jitter phase refreshes NR with composed physical motion");
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, .75f);
    Check(frame(0) == 1 && native::edit_reuse_fallbacks.load() > before_jitter_fallbacks,
          "jitter outside the validated render-pixel range retains per-frame NR");
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.f);
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f);
    Check(frame(1) == 1, "refresh recovers after unsupported jitter");
    const auto complete_parameters = parameters;
    parameters.Reset();
    parameters.Set(NVSDK_NGX_Parameter_Color, source.Get());
    parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
    parameters.Set(NVSDK_NGX_Parameter_Depth, wrapper_depth.Get());
    parameters.Set(NVSDK_NGX_Parameter_MotionVectors, wrapper_motion.Get());
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, .25f);
    const auto before_partial_jitter_reuses = native::edit_reuse_reuses.load();
    const auto before_partial_jitter_fallbacks = native::edit_reuse_fallbacks.load();
    Check(frame(0) == 1 && native::edit_reuse_reuses.load() == before_partial_jitter_reuses
              && native::edit_reuse_fallbacks.load() > before_partial_jitter_fallbacks,
          "a partial jitter pair cannot establish a guide sampling contract");
    parameters = complete_parameters;
    Check(frame(1) == 1, "complete zero jitter contract restores refresh");
    native::edit_reuse_enabled = false;
    const auto off_reuses = native::edit_reuse_reuses.load();
    Check(frame(1) == 1 && frame(0) == 1 && native::edit_reuse_reuses.load() == off_reuses,
          "feature Off preserves the normal per-frame model path");
    native::edit_reuse_enabled = true;
    for (uint32_t prefix = 0; prefix < 4; ++prefix) frame(1);

    {
      std::weak_ptr<reuse::State> old_anchor = native::features.at(owner).slots[0].edit_reuse;
      Check(!old_anchor.expired(), "adaptive transition begins with a real completed edit anchor");
      const auto reuses_before_adaptive = native::edit_reuse_reuses.load();
      const auto fallbacks_before_adaptive = native::edit_reuse_fallbacks.load();
      // Mock another source's live reservation. The production selector then
      // disables this frame's adaptive route while the user's request stays On.
      auto reservation = std::make_shared<addon::adaptive_resolution::Session>();
      native::adaptive_resident_session = reservation;
      native::kSetAdaptiveLimitedResidency.Store(1.f);
      native::kSetAdaptiveEnabled.Store(1.f);
      native::look_upsample = addon::look::kUpsampleClassic;
      Check(frame(0) == 0 && native::edit_reuse_reuses.load() == reuses_before_adaptive + 1
                && native::edit_reuse_fallbacks.load() == fallbacks_before_adaptive,
            "an adaptive request blocked by another source preserves reuse on the actual fixed workset");
      const auto session = native::features.at(owner).adaptive_session;
      Check(session != nullptr && native::features.at(owner).adaptive_cache == nullptr
                && session->status.reason == addon::adaptive_resolution::Reason::PREPARING
                && native::features.at(owner).slots[0].edit_reuse == old_anchor.lock(),
            "the dormant adaptive request retains its status and the current fixed anchor");
      Check(frame(0) == 1, "the following fixed frame refreshes before adaptive eligibility changes");
      const auto compatible_anchor = native::features.at(owner).slots[0].edit_reuse;
      const auto compatible_model = native::features.at(owner).slots[0].handle;
      const auto compatible_workset = native::features.at(owner).last_workset_key;
      const auto compatible_allocations = native::worksets_created;
      reservation.reset();
      native::adaptive_resident_session.reset();
      Check(native::EnsureLookPipeline(gpu.device.Get()), "warm existing Look programs before admitted adaptive recording");
      Check(frame(0) == 0 && native::edit_reuse_reuses.load() == reuses_before_adaptive + 2
                && native::features.at(owner).adaptive_cache != nullptr
                && native::features.at(owner).adaptive_cache->active == 0
                && native::features.at(owner).slots[0].edit_reuse == compatible_anchor
                && native::features.at(owner).slots[0].handle == compatible_model
                && native::features.at(owner).last_workset_key == compatible_workset
                && native::worksets_created == compatible_allocations,
            "adaptive eligibility alone keeps the same fixed workset, model and adjacent reuse");
      native::PollAdaptiveCache(native::features.at(owner).adaptive_cache.get());
      native::kSetAdaptiveEnabled.Store(0.f);
      auto& feature = native::features.at(owner);
      bool refreshed = false;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8; ++attempt) {
        if (frame(1) == 1 && feature.adaptive_cache == nullptr
            && feature.slots[0].edit_reuse != nullptr && feature.slots[0].edit_reuse->valid) {
          refreshed = true;
          break;
        }
      }
      Check(refreshed && feature.adaptive_session == session,
            "adaptive Off retains its Session, retires standby slots and refreshes a new edit anchor");
      const auto fresh_anchor = native::features.at(owner).slots[0].edit_reuse;
      const auto fresh_generation = fresh_anchor->anchor_generation;
      Check(frame(0) == 0 && native::edit_reuse_reuses.load() == reuses_before_adaptive + 3
                && native::features.at(owner).slots[0].edit_reuse == fresh_anchor
                && fresh_anchor->anchor_generation == fresh_generation,
            "the retained disabled Session permits one reuse with zero model calls and no recursive recapture");
      native::kSetAdaptiveLimitedResidency.Store(0.f);
    }
    {
      auto& original = native::features.at(owner);
      Check(frame(1) == 1, "prepare a fresh fixed anchor before changing source or model");
      const auto original_anchor = original.slots[0].edit_reuse;
      const auto original_model = original.slots[0].handle;
      const auto reuses_before = native::edit_reuse_reuses.load();
      const auto* second = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xED174});
      native::RegisterCreatedFeatureLocked(second, native::kFeatureDlss, &parameters);
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8; ++attempt) {
        const auto calls = source_frame(second, 0, true, false);
        Check(native::edit_reuse_reuses.load() == reuses_before,
              "the new handle on the same 16x16 output cannot consume the previous source's anchor");
        const auto& anchor = native::features.at(second).slots[0].edit_reuse;
        if (anchor != nullptr && anchor->valid) {
          Check(calls == 1, "the new source's first valid anchor records a model call");
          break;
        }
      }
      const auto& second_feature = native::features.at(second);
      Check(second_feature.slots[0].edit_reuse != nullptr && second_feature.slots[0].edit_reuse != original_anchor
                && second_feature.slots[0].handle != original_model && second_feature.input_width == 16
                && second_feature.source_generation != original.source_generation && model_resets.back() == 1,
            "the first delivered edit for a same-size new source refreshes its own model and history");
      Check(source_frame(second, 0, true, false) == 0, "the new source can reuse only its own refresh");
      native::ForgetSourceHandleLocked(second);
      native::DrainRetiredResources(true);

      native::kSetPreset.Commit(1.f);
      const auto reuses_before_model = native::edit_reuse_reuses.load();
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8; ++attempt) {
        const auto calls = frame(0);
        Check(native::edit_reuse_reuses.load() == reuses_before_model,
              "a changed model request cannot reuse the previous model's edit");
        const auto& anchor = original.slots[0].edit_reuse;
        if (anchor != nullptr && anchor->valid) {
          Check(calls == 1, "the changed model's first valid anchor records a model call");
          break;
        }
      }
      Check(original.slots[0].nr_preset == 1 && original.slots[0].edit_reuse != nullptr
                && original.slots[0].edit_reuse != original_anchor && original.input_width == 16
                && model_resets.back() == 1 && frame(0) == 0,
            "the real model replacement at unchanged geometry refreshes before resuming x2");
      native::kSetPreset.Commit(2.f);
      std::cout << "NR_RELEASE_PARITY dx12-upscaled-adaptive-x2 fixed-active grid=16x16 cadence=1/0"
                   " eligibility_keeps_anchor=1 same_grid_source_refresh=1 model_refresh=1 evidence=WARP+MOCK\n";
    }
    {
      auto& slot = native::features.at(owner).slots[0];
      const auto saved_upsample = native::look_upsample.load();
      const auto refresh_at_size = [&](uint32_t size) {
        for (uint32_t attempt = 0; attempt < 4; ++attempt) {
          if (frame(1) == 1 && slot.edit_reuse != nullptr && slot.edit_reuse->valid
              && slot.edit_reuse->width == size && slot.edit_reuse->height == size) return;
        }
        Check(false, "fixed Upscaled refresh reaches the committed edit grid");
      };
      edit_model_behavior = ConstantModel;
      native::look_upsample = addon::look::kUpsampleClassic;
      native::nr_resolution_scale = .75f;
      ++native::nr_resolution_commit_epochs[0];
      refresh_at_size(12);
      const auto low_anchor = slot.edit_reuse;
      const auto low_reuses = native::edit_reuse_reuses.load();
      std::vector<float> revealed(16 * 16, .5f);
      for (uint32_t y = 0; y < 16; ++y) {
        for (uint32_t x = 12; x < 16; ++x) {
          revealed[y * 16 + x] = .9f;
        }
      }
      upload(wrapper_depth.Get(), revealed, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, 16 * 4);
      submit_commands("fixed low-grid reveal upload");
      Check(frame(0) == 0 && slot.edit_reuse == low_anchor && native::edit_reuse_reuses.load() == low_reuses + 1,
            "fixed Upscaled with Classic Resolve reuses on the committed 12-to-16 grid");
      auto low_final = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      submit_commands("fixed low-grid output readback");
      const auto pixels = low_final.Pixels();
      for (uint32_t y = 0; y < 16; ++y) {
        for (uint32_t x = 12; x < 16; ++x) {
          const size_t pixel = (y * 16 + x) * 4;
          Check(std::memcmp(pixels.data() + pixel * sizeof(uint16_t), source_values.data() + pixel,
                            4 * sizeof(uint16_t)) == 0,
                "fixed low-grid Resolve and original selection restore exact RGBA in the reveal strip");
        }
      }
      constexpr size_t interior = (7 * 16 + 7) * 4;
      uint16_t edited_red = 0;
      std::memcpy(&edited_red, pixels.data() + interior * sizeof(uint16_t), sizeof(edited_red));
      Check(std::abs(DirectX::PackedVector::XMConvertHalfToFloat(edited_red)
                     - DirectX::PackedVector::XMConvertHalfToFloat(source_values[interior])) > .05f,
            "fixed low-grid output preserves a visible edit in the valid interior");
      native::look_upsample = addon::look::kUpsampleAuto;
      Check(frame(0) == 1 && slot.edit_reuse == nullptr && native::edit_reuse_reuses.load() == low_reuses + 1,
            "actual low-grid Look transport continues to evaluate every frame");
      native::look_upsample = addon::look::kUpsampleClassic;
      native::detail_stability = 0;
      Check(frame(0) == 1 && slot.edit_reuse == nullptr && native::edit_reuse_reuses.load() == low_reuses + 1,
            "actual low-grid temporal Look continues to evaluate every frame");

      native::detail_stability = 1;
      native::look_upsample = saved_upsample;
      native::nr_resolution_scale = 1.f;
      ++native::nr_resolution_commit_epochs[0];
      edit_model_behavior = NeutralModel;
      upload(wrapper_depth.Get(), std::vector<float>(16 * 16, .5f), NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, 16 * 4);
      submit_commands("restore fixed wrapper depth");
      refresh_at_size(16);
    }
    {
      const AdaptivePresentRegistration registration;
      AdaptivePresentOutput present(gpu, output.Get());
      SetAdaptiveOutputMode(&present, 70);
      const auto saved_levels = native::kSetAdaptiveLevelCount.Get();
      const auto saved_minimum = native::kSetAdaptiveMinScale.Get();
      native::kSetAdaptiveLevelCount.Store(2.f);
      native::kSetAdaptiveMinScale.Store(.75f);
      native::kSetAdaptiveEnabled.Store(1.f);
      edit_model_behavior = ConstantModel;
      native::intensity = 1.f;
      for (size_t pixel = 0; pixel < source_values.size() / 4; ++pixel) {
        source_values[pixel * 4] = DirectX::PackedVector::XMConvertFloatToHalf(.25f);
      }
      upload(source.Get(), source_values, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, 16 * 8);
      submit_commands("two-tier source upload");
      uint64_t active_calls = 0, preparation_calls = 0;
      model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*) {
        if (native::preparing_adaptive_frame != nullptr) {
          ++preparation_calls;
          Check(native::features.at(owner).slots[0].edit_reuse == nullptr,
                "standby model preparation never captures or consumes an edit anchor");
        } else {
          ++active_calls;
        }
      };
      auto& feature = native::features.at(owner);
      for (uint32_t attempt = 0; attempt < 32; ++attempt) {
        const auto successes = native::SourceSuccessfulEvaluations();
        const auto active_before = active_calls;
        const auto reuses_before = native::edit_reuse_reuses.load();
        frame(0);
        Check(native::SourceSuccessfulEvaluations() - successes
                  == active_calls - active_before + native::edit_reuse_reuses.load() - reuses_before,
              "standby preparation contributes no successful source frames");
        if (feature.adaptive_session != nullptr && feature.adaptive_session->prepared) break;
      }
      model_parameter_observer = {};
      auto tiers = feature.adaptive_cache;
      Check(tiers != nullptr && tiers->count == 2 && tiers->active == 0 && feature.adaptive_session->prepared
                && tiers->tiers[0].ready && tiers->tiers[1].ready && preparation_calls > 0,
            "the real adaptive path prepares exactly two independent model histories");
      Check(tiers->ladder.sizes[1][0].width == 12 && tiers->ladder.sizes[1][0].height == 12,
            "the prepared low tier uses a noninteger 12-to-16 Resolve ratio");
      const auto models = native::nr_features_created;
      const auto allocations = native::worksets_created;
      const auto upper_model = feature.slots[0].handle;
      const auto lower_model = tiers->tiers[1].state.slots[0].handle;
      Check(upper_model != lower_model, "ready tiers retain distinct model histories");
      parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, .25f);
      parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, -.375f);
      uint32_t checked_frames = 0;
      const auto checked_frame = [&](int reset, uint32_t calls) {
        const auto successes = native::SourceSuccessfulEvaluations();
        const auto actual_calls = frame(reset);
        ++checked_frames;
        if (actual_calls != calls || native::SourceSuccessfulEvaluations() != successes + 1) {
          std::cerr << "Two-tier frame=" << checked_frames << " slot=" << tiers->active
              << " reset=" << reset << " model_calls=" << actual_calls << " expected=" << calls
              << " source_successes=" << native::SourceSuccessfulEvaluations() - successes
              << " models=" << native::nr_features_created - models
              << " worksets=" << native::worksets_created - allocations << '\n';
        }
        Check(actual_calls == calls && native::SourceSuccessfulEvaluations() == successes + 1,
              "each prepared source frame succeeds once, including zero-model-call reuse");
      };
      const auto capture_output = [&] {
        auto capture = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit_commands("two-tier final output readback");
        return capture.Pixels();
      };
      checked_frame(1, 1);
      std::weak_ptr<reuse::State> upper_anchor = feature.slots[0].edit_reuse;
      checked_frame(0, 0);
      feature.adaptive_session->Request(1, native::SteadyNowNs());
      checked_frame(0, 1);
      Check(tiers->active == 1 && feature.slots[0].handle == lower_model && model_resets.back() == 1
                && upper_anchor.expired() && tiers->tiers[0].state.slots[0].edit_reuse == nullptr,
            "upper-to-lower activation resets the existing model and retires the departed anchor");
      auto low_anchor = feature.slots[0].edit_reuse;
      Check(low_anchor != nullptr && low_anchor->width == 12 && low_anchor->height == 12
                && low_anchor->output_width == 16 && low_anchor->output_height == 16,
            "production recording uses working N and output O for the low-grid edit cache");
      const auto lower_refresh = capture_output();
      const auto lower_anchor_bytes = read_buffer(low_anchor->anchor.Get());
      std::vector<float> occluded_depth(16 * 16, .5f);
      for (uint32_t y = 0; y < 16; ++y) {
        for (uint32_t x = 12; x < 16; ++x) occluded_depth[y * 16 + x] = .9f;
      }
      upload(wrapper_depth.Get(), occluded_depth, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, 16 * 4);
      submit_commands("two-tier occlusion upload");
      checked_frame(0, 0);
      const auto lower_reuse = capture_output();
      Check(read_buffer(low_anchor->anchor.Get()) == lower_anchor_bytes,
            "the low-grid full Resolve path never recaptures its reused anchor");
      constexpr size_t stable_pixel = 7 * 16 + 7;
      bool nonneutral = false;
      for (uint32_t channel = 0; channel < 4; ++channel) {
        uint16_t refreshed, reused;
        std::memcpy(&refreshed, lower_refresh.data() + (stable_pixel * 4 + channel) * 2, 2);
        std::memcpy(&reused, lower_reuse.data() + (stable_pixel * 4 + channel) * 2, 2);
        const float reference = DirectX::PackedVector::XMConvertHalfToFloat(refreshed);
        const float actual = DirectX::PackedVector::XMConvertHalfToFloat(reused);
        Check(std::fabs(reference - actual) < .003f,
              "actual low-grid Resolve/Commit/writeback preserves the refreshed edit in the valid interior");
        if (channel < 3) nonneutral |= std::fabs(reference
            - DirectX::PackedVector::XMConvertHalfToFloat(source_values[stable_pixel * 4 + channel])) > .05f;
      }
      Check(nonneutral, "the full-chain low-grid witness contains a visible non-neutral edit");
      for (uint32_t y = 0; y < 16; ++y) {
        for (uint32_t x = 12; x < 16; ++x) {
          const size_t pixel = y * 16 + x;
          Check(std::memcmp(lower_reuse.data() + pixel * 8, source_values.data() + pixel * 4, 8) == 0,
                "the full output occlusion strip restores original source RGBA exactly after Resolve and Commit");
        }
      }
      checked_frame(0, 1);
      checked_frame(0, 0);
      std::weak_ptr<reuse::State> retired_lower = low_anchor;
      low_anchor.reset();
      feature.adaptive_session->Request(0, native::SteadyNowNs());
      checked_frame(0, 1);
      Check(tiers->active == 0 && feature.slots[0].handle == upper_model && model_resets.back() == 1
                && retired_lower.expired() && tiers->tiers[1].state.slots[0].edit_reuse == nullptr,
            "returning to the upper ready tier resets history and cannot resurrect the old low-grid edit");
      checked_frame(0, 0);
      Check(native::nr_features_created == models && native::worksets_created == allocations,
            "upper/lower/upper reuse creates no additional model or adaptive workset");
      rate::ReleaseNativeViewport(70);
      native::kSetAdaptiveEnabled.Store(0.f);
      native::kSetAdaptiveLevelCount.Store(saved_levels);
      native::kSetAdaptiveMinScale.Store(saved_minimum);
      edit_model_behavior = NeutralModel;
      parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.f);
      parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f);
      upload(wrapper_depth.Get(), std::vector<float>(16 * 16, .5f), NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, 16 * 4);
      submit_commands("restore normal guide input");
      Check(frame(1) == 1, "disabling adaptive resolution returns to the fixed-size refresh");
      std::cout << "PASS edit reuse two ready tiers: actual low-grid Resolve/Commit/writeback, nonzero jitter, exact occlusion RGBA,"
                   " one reuse, source clock, separate preparation counts and retired anchors; evidence=WARP+MOCK\n";
    }
    Check(frame(1, false) == 1, "record a refresh without submitting its GPU commands");
    const auto discarded = submission::ObserveRecording(gpu.list.Get());
    Check(!discarded->submitted.load(), "discard probe has no submission proof");
    Check(SUCCEEDED(gpu.list->Close()) && SUCCEEDED(gpu.allocator->Reset())
              && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "discard unsubmitted refresh recording");
    submission::OnCommandListReset(gpu.list.Get());
    Check(discarded->closed.load() && !discarded->submitted.load(), "Reset distinguishes discard from completed refresh");
    const auto before_discard_reuses = native::edit_reuse_reuses.load();
    frame(0);
    Check(native::edit_reuse_reuses.load() == before_discard_reuses, "discarded refresh cannot become a cache anchor");
    for (uint32_t prefix = 0; prefix < 4; ++prefix) frame(1);
    fail_call = model_calls + 1;
    Check(frame(1) == 1, "inject failure in the next actual NR refresh");
    fail_call = 0;
    const auto failed_anchor = native::features.at(owner).slots[0].edit_reuse;
    Check(failed_anchor == nullptr || !failed_anchor->valid, "failed NR refresh leaves no reusable anchor");
    const auto before_failure_reuses = native::edit_reuse_reuses.load();
    frame(0);
    Check(native::edit_reuse_reuses.load() == before_failure_reuses, "failure backoff cannot expose the old cached edit");
    auto failed_final = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    submit_commands("wrapper failure readback");
    const auto failed_bytes = failed_final.Pixels();
    Check(std::memcmp(failed_bytes.data(), source_values.data(), failed_bytes.size()) == 0,
          "failed refresh and backoff retain current SR bits and alpha");
    native::ForgetSourceHandleLocked(owner);
    native::DrainRetiredResources(true);
    Check(!native::features.contains(owner) && submission::ResourceReleasable(anchor_state.get()),
          "source release retires submitted and discarded cache references");
    anchor_state.reset();

    {
      const AdaptivePresentRegistration registration;
      const auto* sr = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xED190});
      native::RegisterCreatedFeatureLocked(sr, native::kFeatureDlss, &parameters);
      edit_model_behavior = ConstantModel;
      native::look_upsample = addon::look::kUpsampleClassic;
      native::kSetAdaptiveEnabled.Store(1.f);
      native::kSetAdaptiveLevelCount.Store(2.f);
      native::kSetAdaptiveMinScale.Store(.75f);
      source_frame(sr, 0, true, false);  // Establish the SR device in the real wrapper.

      const auto proxy = gpu.Swapchain();
      ComPtr<ReceiptSwapchain> source;
      source.Attach(new ReceiptSwapchain(nullptr));
      source->reported_device12 = gpu.device;
      AdaptivePresentOutput present(gpu, output.Get(), source.Get());
      auto& sdk_slot = adapter::modules[0];
      auto& proxy_slot = adapter::proxies[0];
      auto& source_slot = adapter::source_present_slots[0];
      funnel_accounting_tests::Restore restore;
      restore.Value(&sdk_slot.real);
      restore.Value(&sdk_slot.complete);
      restore.Value(&proxy_slot);
      restore.Value(&source_slot);
      restore.Value(&receipt_proxy);
      restore.Value(&receipt_context_handle);
      restore.Value(&receipt_present_status);
      restore.Value(&rate::test_now_ns);
      restore.Atomic(&native::monitor_requested);
      native::monitor_requested = true;
      sdk_slot.complete = true;
      sdk_slot.real[0] = reinterpret_cast<void*>(&FakeInit);
      sdk_slot.real[2] = reinterpret_cast<void*>(&FakeReceiptGetSwapchain);
      sdk_slot.real[6] = reinterpret_cast<void*>(&FakeReceiptEnable);
      sdk_slot.real[8] = reinterpret_cast<void*>(&FakeReceiptDestroy);
      receipt_proxy = proxy.Get();
      proxy_slot.records[0].target = reinterpret_cast<uint8_t*>((*reinterpret_cast<void***>(proxy.Get()))[8]);
      proxy_slot.installed = proxy_slot.complete = true;
      proxy_slot.real[0] = reinterpret_cast<void*>(&FakePresent);
      source_slot.records[0].target = reinterpret_cast<uint8_t*>((*reinterpret_cast<void***>(source.Get()))[8]);
      source_slot.installed = source_slot.complete = true;
      source_slot.real[0] = reinterpret_cast<void*>(&FakeSourcePresent);
      source->present_callback = +[](ReceiptSwapchain*, UINT interval, UINT flags) {
        return adapter::ProxyHooks<0>::Present(static_cast<IDXGISwapChain*>(receipt_proxy), interval, flags);
      };
      xefg::Configure(nullptr, nullptr, nullptr, nullptr);
      xefg::ConfigureD3D12Source(native::ObserveD3D12SourceForXefg);
      const auto source_owner = reinterpret_cast<uintptr_t>(&present);
      Check(xefg::RegisterD3D12SourceSwapchain(source_owner, source.Get()), "register the linked WARP SR source");
      const auto context_handle = static_cast<void*>(source.Get());
      receipt_context_handle = context_handle;
      const xefg::abi::Init xefg_init{.application_swapchain = source.Get(), .max_interpolated = 1};
      Check(Hooks::Init(context_handle, gpu.queue.Get(), &xefg_init) == 0, "initialize the substitute through XeFG's Init hook");
      ComPtr<IDXGISwapChain> hooked_proxy;
      Check(Hooks::GetSwapchain(context_handle, IID_PPV_ARGS(&hooked_proxy)) == 0, "register the exact returned proxy");
      const auto context = adapter::state.contexts.at(context_handle);
      context->get_status = FakeReceiptPresentStatus;
      const auto sdk_present = [&] {
        Check(adapter::SourcePresentHooks<0>::Present(source.Get(), 0, 0) == S_OK, "nested source and proxy Present");
      };
      rate::test_now_ns = rate::NowNs();
      receipt_present_status = {.frames_presented = 1, .enabled = 0};
      sdk_present();
      Check(Hooks::Enable(context_handle, 0) == 0, "real SDK hook accepts explicit XeFG Disable");
      const auto output_id = present.Identity();
      const auto source_id = native::UniqueD3D12SourceLocked(reinterpret_cast<uintptr_t>(
          renodx::utils::directx::NativeIdentity(gpu.device.Get())));
      Check(xefg::QueryBridgeOff(source_id, output_id).available && output_id == context->output_identity,
            "Disable identifies the same successful SR and its XeFG output");
      for (uint32_t sample = 0; sample < 100; ++sample) {
        rate::test_now_ns += 25'000'000;
        sdk_present();
      }
      auto& feature = native::features.at(sr);
      const auto read_edit_delivery = [&](int64_t now) {
        return native::ReadDeliveredEditReuse(feature.edit_reuse_monitor, feature.source_generation,
            feature.sr_sequence, addon::stage_plan::Stage::UPSCALED, now).delivery;
      };
      const auto waiting = [](native::EditReuseDelivery delivery) {
        return delivery == native::EditReuseDelivery::NONE || delivery == native::EditReuseDelivery::REFRESH;
      };
      for (uint32_t i = 0; i < 64 && (!feature.adaptive_session->prepared
                                        || feature.adaptive_session->status.applied[0].width != 16); ++i) {
        source_frame(sr, 0, true, false);
      }
      Check(feature.adaptive_session->prepared && feature.adaptive_session->status.applied[0].width == 16
                && rate::Read(output_id, rate::NowNs()).available,
            "two ready tiers and current linked FPS evidence");
      feature.adaptive_session->Request(1, rate::NowNs());
      const auto next_finish = [&] {
        auto capture = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit_commands("XeFG low-grid actual SR output");
        native::ResolveAdaptiveBorrowedDeliveries();
        return capture.Pixels();
      };
      const auto source_pixel = source_values.data() + (6 * 16 + 6) * 4;
      const auto original = [&](const auto& bytes) {
        return std::memcmp(source_values.data(), bytes.data(), bytes.size()) == 0;
      };
      const auto edited = [&](const auto& bytes) {
        const auto* pixel = bytes.data() + (6 * 16 + 6) * 8;
        return std::memcmp(pixel, source_pixel, 6) != 0 && std::memcmp(pixel + 6, source_pixel + 3, 2) == 0;
      };
      Check(source_frame(sr, 1, false, false) == 1 && edited(next_finish())
                && feature.adaptive_session->status.applied[0].width == 12,
            "first complete reduced frame refreshes and delivers edited RGB with its original alpha");
      rate::test_now_ns += 3'000'000'000;
      Check(!rate::Read(output_id, rate::NowNs()).available && xefg::QueryBridgeOff(source_id, output_id).available,
            "age out only the output FPS window");
      auto before = native::edit_reuse_reuses.load();
      Check(source_frame(sr, 0, false, false) == 0 && edited(next_finish())
                && native::edit_reuse_reuses.load() == before + 1 && feature.adaptive_session->status.applied[0].width == 12
                && feature.adaptive_session->status.reason == adaptive::Reason::OUTPUT_UNAVAILABLE,
            "an expired FPS window keeps the reduced tier and delivers zero-model edit with current alpha");
      Check(read_edit_delivery(native::SteadyNowNs()) == native::EditReuseDelivery::REUSED,
            "the monitor reports the reduced reuse after its first valid submission settles");

      const auto on = [&] {
        receipt_present_status.enabled = 1;
        sdk_present();
        Check(!xefg::QueryBridgeOff(source_id, output_id).available, "one-output SDK ON withdraws the old Disable");
      };
      const auto disable = [&] {
        receipt_present_status.enabled = 0;
        Check(Hooks::Enable(context_handle, 0) == 0 && xefg::QueryBridgeOff(source_id, output_id).available,
              "restore authority only through a new explicit Disable");
      };
      Check(source_frame(sr, 0, true, false) == 1, "renew the reduced anchor before the redundant Disable");
      const auto before_repeat_disable = feature.slots[0].edit_reuse;
      receipt_present_status.enabled = 1;
      receipt_during_present_status = +[] {
        Check(Hooks::Enable(receipt_context_handle, 0) == 0, "explicit newer Disable while ON status returns");
      };
      sdk_present();
      Check(xefg::QueryBridgeOff(source_id, output_id).available,
            "ON from the prior declaration cannot withdraw the newer successful Disable");
      receipt_present_status.enabled = 0;
      Check(source_frame(sr, 0, false, false) == 0 && edited(next_finish())
                && feature.slots[0].edit_reuse == before_repeat_disable,
            "a redundant successful Disable keeps the same valid reduced anchor");
      const auto refreshes_before_revoke = native::edit_reuse_refreshes.load();
      model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*) { on(); };
      Check(source_frame(sr, 0, false, false) == 1, "withdraw authority inside the actual model call before copyback");
      model_parameter_observer = {};
      Check(original(next_finish()) && native::edit_reuse_refreshes.load() == refreshes_before_revoke
                && feature.slots[0].edit_reuse == nullptr,
            "revoked private refresh returns original RGBA without publishing an anchor or a second Evaluate");
      receipt_present_status.enabled = 0;
      sdk_present();
      Check(!xefg::QueryBridgeOff(source_id, output_id).available,
            "an SDK OFF status alone cannot renew revoked write permission");
      disable();
      Check(source_frame(sr, 0, true, false) == 1, "fresh authority seeds a new reduced anchor");
      before = native::edit_reuse_reuses.load();
      Check(source_frame(sr, 0, false, false) == 0, "record the next reduced reuse while authority holds");
      const auto unsubmitted = submission::ObserveRecording(gpu.list.Get());
      on();
      native::ResolveAdaptiveBorrowedDeliveries();
      Check(!unsubmitted->submitted.load() && waiting(read_edit_delivery(native::SteadyNowNs()))
                && native::edit_reuse_reuses.load() == before && !native::adaptive_borrowed_deliveries.empty(),
            "ON after copyback leaves the still unsubmitted candidate pending");
      Check(edited(next_finish()) && native::edit_reuse_reuses.load() == before + 1,
            "the ordered, submitted copyback survives the later ON receipt");
      native::ResolveAdaptiveBorrowedDeliveries();
      Check(native::edit_reuse_reuses.load() == before + 1 && source_frame(sr, 0, true, false) == 1,
            "settle once; the next frame cannot reuse the revoked authority");
      disable();
      Check(source_frame(sr, 0, true, false) == 1, "new authority refreshes after actual per-frame delivery");
      const auto effective_grid = feature.adaptive_session->status.applied;
      native::kSetFollowInputRes.Store(1.f);
      native::kSetLookMode.Store(1.f);
      const auto look_settings = native::LookSettingsSnapshot(true, false, 0, false);
      Check(addon::look::GainsNeutral(look_settings) && look_settings.stabilize == 0,
            "the alternate Look setting requests an identity operation");
      Check(source_frame(sr, 0, false, false) == 0 && edited(next_finish())
                && feature.adaptive_session->status.applied == effective_grid,
            "Follow Input and neutral Look keep low-grid reuse on the same effective size");
      native::kSetFollowInputRes.Store(0.f);
      native::kSetLookMode.Store(0.f);
      Check(source_frame(sr, 0, true, false) == 1, "refresh ahead of the discarded reuse");
      before = native::edit_reuse_reuses.load();
      Check(source_frame(sr, 0, false, false) == 0, "record a reuse that the host will discard");
      Check(SUCCEEDED(gpu.list->Close()) && SUCCEEDED(gpu.allocator->Reset())
                && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "discard exact list generation without Execute");
      submission::OnCommandListReset(gpu.list.Get());
      native::ResolveAdaptiveBorrowedDeliveries();
      Check(native::edit_reuse_reuses.load() == before && native::adaptive_borrowed_deliveries.empty(),
            "discard cannot report an edited delivery");

      const auto seed_refreshes = native::edit_reuse_refreshes.load();
      for (uint32_t attempt = 0; attempt < 4 && native::edit_reuse_refreshes.load() == seed_refreshes; ++attempt) {
        // Discarded work can require model maturation and standby preparation.
        source_frame(sr, 1, true, false);
      }
      Check(native::edit_reuse_refreshes.load() == seed_refreshes + 1,
            "seed a confirmed reduced anchor for the missing first submission proof");
      const auto& session = *feature.adaptive_session;
      const auto applied_grid = session.status.applied;
      const auto applied_generation = session.applied_generation;
      const auto applied_ns = session.applied_ns;
      const auto confirmed_sequence = session.delivered_edit_sequence;
      const auto& slot = feature.slots[0];
      const auto anchor = slot.edit_reuse;
      Check(applied_grid[0].width == 12 && session.delivered_edit_reuse && confirmed_sequence == feature.sr_sequence
                && anchor != nullptr && anchor->valid,
            "the replay probe starts from a delivered low-grid refresh and a valid anchor");
      const auto anchor_generation = anchor->anchor_generation;
      const auto anchor_reuses = anchor->reuse_count;
      const auto calls_before_replay = model_calls;
      const auto refreshes_before_replay = native::edit_reuse_refreshes.load();
      const auto reuses_before_replay = native::edit_reuse_reuses.load();
      const auto evaluations_before_replay = native::successful_evaluations.load();
      Check(source_frame(sr, 0, false, false) == 0, "record the low-grid reuse that will execute twice");
      const auto reuse_sequence = feature.sr_sequence;
      const auto anchor_sequence = anchor->source_sequence;
      const auto model = slot.handle;
      const auto look_sequence = slot.look_reuse_sequence;
      const auto monitor_now = native::SteadyNowNs();
      const auto recording = submission::ObserveRecording(gpu.list.Get());
      const auto& deliveries = native::adaptive_borrowed_deliveries;
      Check(deliveries.size() == 1 && deliveries.front().recording == recording
                && deliveries.front().delivery.source_sequence == reuse_sequence
                && deliveries.front().delivery.borrowed_edit_reused,
            "the actual SR reuse stages one deferred delivery for this recording");
      const auto delivery = deliveries.front().delivery.frame.delivery;
      const auto& observation = feature.edit_reuse_monitor[0];
      Check(observation.sequence == reuse_sequence && observation.source_generation == feature.source_generation
                && observation.delivery == native::EditReuseDelivery::REUSED && !recording->submitted.load()
                && waiting(read_edit_delivery(monitor_now)),
            "the recorded reuse is pending before its first Execute");

      const auto capture = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      gpu.Retain(capture.readback.Get());
      Check(SUCCEEDED(gpu.list->Close()), "close the exact XeFG low-grid reuse recording");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      const auto wait_for_gpu = [&] {
        gpu.submission_pending = true;
        Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "signal the independent XeFG fixture fence");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(event != nullptr, "XeFG fixture completion event");
        const auto armed = gpu.fence->SetEventOnCompletion(gpu.serial, event);
        const auto wait = (SUCCEEDED(armed) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
        CloseHandle(event);
        Check(wait == WAIT_OBJECT_0 && SUCCEEDED(gpu.device->GetDeviceRemovedReason()),
              "complete XeFG fixture GPU work before reusing the recording or its resources");
        gpu.submission_pending = false;
      };
      const auto proofs = [&] {
        std::lock_guard lock(submission::tracker_mutex);
        const auto& generation = submission::tracked_lists.at(recording->list).generations.at(recording->generation);
        Check(generation.unreserved_submissions == 0, "each Execute reserves an exact submission receipt");
        return generation.submissions;
      };
      const auto core_unchanged = [&] {
        return deliveries.size() == 1 && deliveries.front().recording == recording
            && (delivery == nullptr || *delivery == adaptive::DeliveryReceipt::PENDING) && feature.sr_sequence == reuse_sequence
            && session.status.applied == applied_grid && session.applied_generation == applied_generation
            && session.applied_ns == applied_ns && session.delivered_edit_sequence == confirmed_sequence
            && session.delivered_edit_reuse && model_calls == calls_before_replay && slot.handle == model
            && slot.look_reuse_sequence == look_sequence && native::successful_evaluations.load() == evaluations_before_replay
            && native::edit_reuse_refreshes.load() == refreshes_before_replay
            && native::edit_reuse_reuses.load() == reuses_before_replay && slot.edit_reuse == anchor
            && anchor->valid && anchor->reused && anchor->source_sequence == anchor_sequence
            && anchor->anchor_generation == anchor_generation && anchor->reuse_count == anchor_reuses;
      };

      ID3D12Fence* queue_fence = nullptr;
      {
        std::unique_lock lock(addon::queue_completion_mutex);
        auto& completion = addon::queue_completions.at(gpu.queue.Get());
        Check(completion.fence != nullptr && completion.fence != gpu.fence.Get() && !completion.faulted,
              "the fixture queue has a healthy tracker fence distinct from the GPU wait fence");
        queue_fence = std::exchange(completion.fence, nullptr);
      }
      // Exercise the real hook's missing-fence path for only the first Execute.
      gpu.queue->ExecuteCommandLists(1, lists);
      {
        std::unique_lock lock(addon::queue_completion_mutex);
        addon::queue_completions.at(gpu.queue.Get()).fence = queue_fence;
      }
      wait_for_gpu();
      const auto first_completion = gpu.serial;
      const auto first_pixels = capture.Pixels();
      const auto first_proofs = proofs();
      Check(recording->submitted.load() && first_proofs.size() == 1 && !first_proofs.front().pending
                && first_proofs.front().fence.empty() && first_proofs.front().value == 0 && edited(first_pixels),
            "first Execute writes edited RGB with original alpha but supplies no qualifying fence");
      native::ResolveAdaptiveBorrowedDeliveries();
      Check(core_unchanged() && waiting(read_edit_delivery(monitor_now)),
            "a completed first Execute without its qualifying fence keeps core and monitor pending");

      gpu.queue->ExecuteCommandLists(1, lists);
      wait_for_gpu();
      const auto replay_proofs = proofs();
      Check(replay_proofs.size() == 2 && !replay_proofs.front().pending && replay_proofs.front().fence.empty()
                && !replay_proofs.back().pending && replay_proofs.back().fence.fence == queue_fence
                && replay_proofs.back().value != 0 && queue_fence->GetCompletedValue() >= replay_proofs.back().value
                && std::ranges::all_of(replay_proofs, [&](const auto& proof) {
                  return proof.list_identity == recording->list && proof.recording_generation == recording->generation;
                }) && capture.Pixels() == first_pixels,
            "sequential replay of the same recording reproduces RGBA and earns its own completed fence");
      native::ResolveAdaptiveBorrowedDeliveries();
      const auto after_replay = read_edit_delivery(monitor_now);
      Check(core_unchanged(), "the replay fence cannot advance applied tier, confirmed cadence, counts or history");
      Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
            "reset the replayed list only after both GPU executions have finished");
      submission::PruneCompletedGenerations();
      native::ResolveAdaptiveBorrowedDeliveries();
      Check(recording->closed.load() && core_unchanged() && !submission::ResourceReleasable(anchor.get()),
            "reset after a missing first fence keeps the unresolved anchor resources pinned");

      Check(Hooks::Destroy(context_handle) == 0, "retire the original SDK context");
      xefg::ForgetBridgeSourceSwapchain(source_owner);
      xefg::ConfigureD3D12Source(nullptr);
      native::ForgetSourceHandleLocked(sr);
      native::ResolveAdaptiveBorrowedDeliveries();
      Check(deliveries.empty() && (delivery == nullptr || *delivery == adaptive::DeliveryReceipt::REJECTED),
            "source teardown removes the pending delivery without publishing it");
      {
        // Retire only this test's injected hole, using the first Execute's
        // independently observed completion after the source can no longer publish.
        std::lock_guard lock(submission::tracker_mutex);
        auto& generation = submission::tracked_lists.at(recording->list).generations.at(recording->generation);
        auto& first_proof = generation.submissions.front();
        first_proof.fence = addon::FenceRef(gpu.fence.Get());
        first_proof.value = first_completion;
      }
      submission::PruneCompletedGenerations();
      native::DrainRetiredResources(true);
      gpu.uploads.clear();
      gpu.retained_resources.clear();
      Check(submission::ResourceReleasable(anchor.get()), "release the isolated replay resources after source teardown");
      Check(waiting(after_replay), "the replay fence must not publish the first pending XeFG low-grid reuse in the monitor");
      std::cout << "NR_RELEASE_PARITY dx12-upscaled-adaptive-x2 xefg-write-permission fps-expired=1/0 rgba=edited-alpha-intact"
                   " revoked-before-copy=original revoked-after-copy=edited pending=unpublished discard=unpublished"
                   " old-on/late-off=ignored equivalent-settings=1 evidence=WARP+MOCK\n";
      std::cout << "NR_RELEASE_PARITY dx12-upscaled-adaptive-x2 xefg-replay-delivery first-fence=missing replay-fence=completed"
                   " rgba=identical monitor=waiting/waiting normal-monitor=reused core=unchanged retention=until-teardown evidence=WARP+MOCK\n";
    }

    enum class TimingCoverage { FULL, HALF, NONE };
    const auto automatic_descent = [&](const NVSDK_NGX_Handle* trial_source, bool native_off, int low_fps,
                                       TimingCoverage timing) {
      const AdaptivePresentRegistration registration;
      AdaptivePresentOutput present(gpu, output.Get());
      const auto output_identity = present.Identity();
      auto unrelated_buffer = uav_texture(16, 16, DXGI_FORMAT_R16G16B16A16_FLOAT);
      AdaptivePresentOutput unrelated(gpu, unrelated_buffer.Get());
      if (native_off) {
        SetAdaptiveOutputMode(&present, 71);
      } else {
        SetAdaptiveOutputMode(&unrelated, 71);
      }
      Check(rate::NativeMode(output_identity) == (native_off ? std::optional{0u} : std::nullopt),
            "the main PRESENT output has exactly the intended combination Off evidence");
      native::kSetEditReuse.Store(1.f);
      native::kSetAdaptiveEnabled.Store(1.f);
      native::kSetAdaptiveLevelCount.Store(2.f);
      native::kSetAdaptiveLimitedResidency.Store(0.f);
      native::kSetAdaptiveMinScale.Store(.75f);
      native::kSetAdaptiveTargetFps.Store(100.f);
      Check(!native::gpu_timers_enabled.load() && !native::cost_meter_enabled.load(),
            "the production adaptive route schedules its own measurements with profiling disabled");
      edit_model_behavior = ConstantModel;
      native::RegisterCreatedFeatureLocked(trial_source, native::kFeatureDlss, &parameters);
      auto& feature = native::features.at(trial_source);

      funnel_accounting_tests::Restore clock;
      clock.Value(&rate::test_now_ns);
      rate::test_now_ns = rate::NowNs();
      const auto cadence_begin_ns = rate::NowNs();
      int64_t next_present_ns = cadence_begin_ns;
      int64_t cadence_ns = 1'000'000'000 / 80;
      const auto advance = [&](int64_t until_ns) {
        Check(until_ns >= rate::NowNs(), "advance the shared fixture clock monotonically");
        while (next_present_ns <= until_ns) {
          rate::test_now_ns = next_present_ns;
          present.Present();
          unrelated.Present();
          next_present_ns += cadence_ns;
        }
        rate::test_now_ns = until_ns;
      };
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 32; ++attempt) {
        advance(rate::NowNs() + cadence_ns);
        source_frame(trial_source, 0, true, false);
        if (feature.adaptive_session != nullptr && feature.adaptive_session->prepared
            && feature.adaptive_session->status.applied[0].width == 16) break;
      }
      const auto session = feature.adaptive_session;
      const auto tiers = feature.adaptive_cache;
      Check(session != nullptr && session->prepared && session->status.applied[0].width == 16
                && tiers != nullptr && tiers->active == 0 && tiers->tiers[0].ready && tiers->tiers[1].ready,
            "actual SR deliveries confirm the upper working size and prepare both production tiers");
      const auto upper_model = feature.slots[0].handle;
      auto lower_model = tiers->tiers[1].state.slots[0].handle;
      Check(upper_model != nullptr && lower_model != nullptr && upper_model != lower_model,
            "the two prepared grids have distinct model histories");
      advance(rate::NowNs() + cadence_ns);
      Check(source_frame(trial_source, 1, true, false) == 1, "the delivered upper tier refreshes in fixed x2");
      advance(rate::NowNs() + cadence_ns);
      Check(source_frame(trial_source, 0, true, false) == 0 && session->paired_sampling,
            "even without combination Off the delivered upper tier runs fixed x2");

      const AdaptiveGpuSubmission queue{.gpu = &gpu};
      struct Recording {
        uint32_t calls = 0;
        timers::Segment* timer = nullptr;
      };
      const auto record = [&] {
        const auto sequence = timers::internal::next_sequence;
        const auto calls = source_frame(trial_source, 0, false, false);
        const auto segment = std::ranges::find_if(timers::internal::segments, [&](const auto& candidate) {
          return candidate.recorded && candidate.sequence > sequence && candidate.adaptive.session == session;
        });
        return Recording{.calls = calls, .timer = (segment != std::end(timers::internal::segments) ? &*segment : nullptr)};
      };
#if defined(NR_NEXUS_DEV_CONTROL)
      funnel_accounting_tests::Restore adaptive_diagnostic;
      adaptive_diagnostic.Atomic(&native::edit_reuse_adaptive_experiment);
#endif
      rate::Snapshot upper_output;
      advance(std::max({session->stable_after_ns, rate::NowNs(), cadence_begin_ns + 4 * rate::Stream::BIN_NS}));
      const auto upper_applied_ns = session->applied_ns;
      const auto upper_generation = session->applied_generation;
      adaptive::GpuSample old_half;
      Recording first_lower;
      for (uint32_t cycle = 0; cycle < 20; ++cycle) {
        advance(std::max(session->next_measure_ns, rate::NowNs() + cadence_ns));
        upper_output = rate::Read(output_identity, rate::NowNs());
        const auto refresh = record();
        if (tiers->active == 1) {
          first_lower = refresh;
          break;
        }
        Check(upper_output.available && upper_output.source == rate::Source::PRESENT
                  && upper_output.output == output_identity && session->status.output_fps == 80.,
              "the main output's measured cadence stays available during the upper timing probes");
        Check(refresh.calls == 1 && refresh.timer != nullptr && refresh.timer->measure && refresh.timer->adaptive.paired,
              "an actual upper refresh records the first member of an x2 cost cycle");
        const auto first_tag = refresh.timer->adaptive;
        const auto samples_before = session->gpu_count;
        queue.Complete(refresh.timer, timing == TimingCoverage::NONE ? 0 : 3000);
        Check(session->gpu_count == samples_before
                  && session->pending_pair.frame == (timing == TimingCoverage::NONE ? 0 : first_tag.application_frame),
              "a half pair or absent timing leaves complete costs unchanged");
        if (session->pending_pair.frame != 0) old_half = session->pending_pair;
        if (cycle == 0 && native_off) {
          SetAdaptiveOutputMode(&present, 71, sl::DLSSGMode::eOn);
        }
#if defined(NR_NEXUS_DEV_CONTROL)
        const bool toggle_diagnostic = cycle == 1 && native_off;
        if (toggle_diagnostic) {
          native::edit_reuse_adaptive_experiment = true;
        }
#endif
        // Both SR recordings observe the same completed output window.
        advance(rate::NowNs() + 1);
        const auto reused = record();
        Check(reused.calls == 0 && reused.timer != nullptr && reused.timer->measure
                  && reused.timer->adaptive.application_frame == first_tag.application_frame + 1
                  && reused.timer->adaptive.pair_first_frame == first_tag.pair_first_frame,
              "the adjacent, delivered zero-call upper reuse records the other half of the same cycle");
        queue.Complete(reused.timer, timing == TimingCoverage::FULL ? 1000 : 0);
        if (timing == TimingCoverage::FULL) {
          Check(session->gpu_count == std::min(samples_before + 1, session->gpu_us.size())
                    && session->gpu_us[session->gpu_count - 1] == 2000. && session->pending_pair.frame == 0,
                "delivered WARP refresh and reuse publish exactly one 2 ms base-frame cost");
        } else {
          Check(session->gpu_count == 0, "incomplete timing never publishes an upper cycle");
        }
        if (cycle == 0 && native_off) {
          Check(session->sampling_generation == first_tag.sampling_generation && session->status.output_fps == 80.,
                "withdrawing Off alone preserves a healthy fixed anchor and cost pair");
          SetAdaptiveOutputMode(&present, 71);
        }
#if defined(NR_NEXUS_DEV_CONTROL)
        if (toggle_diagnostic) {
          Check(session->sampling_generation == first_tag.sampling_generation && session->status.output_fps == 80.,
                "enabling diagnostic logging preserves the actual upper x2 pair and independent output window");
          native::edit_reuse_adaptive_experiment = false;
        }
#endif
      }
      const auto requested_ns = rate::NowNs();
      Check(upper_output.available && upper_output.source == rate::Source::PRESENT && upper_output.output == output_identity
                && upper_output.window_begin_ns >= upper_applied_ns + adaptive::WARMUP_NS
                && upper_output.window_end_ns - upper_output.window_begin_ns >= 4 * rate::Stream::BIN_NS
                && upper_output.fps == 80. && upper_output.split_available
                && upper_output.split_fps == std::array{80., 80.},
            "registered Present callbacks produce a complete 80 FPS window with two deficient halves after warmup");
      Check(tiers->active == 1 && session->level == 1 && session->status.requested[0].width == 12,
            "the completed output window requests the adjacent low tier independently of timestamp coverage");
      const auto low_generation = session->status.generation;
      Check(first_lower.calls == 1 && feature.slots[0].handle == lower_model && feature.input_width == 12
                && model_resets.back() == 1 && session->paired_sampling == native_off
                && session->pending_pair.frame == 0 && session->applied_generation == upper_generation
                && session->applied_ns == upper_applied_ns && session->status.applied[0].width == 16,
            "the selected low frame changes route and actual grid, keeps the trial, and only records pending delivery");
      if (native_off) {
        const auto discarded = submission::ObserveRecording(gpu.list.Get());
        const auto discarded_model = feature.slots[0].handle;
        const auto models_before_recovery = native::nr_features_created;
        const auto worksets_before_recovery = native::worksets_created;
        Check(SUCCEEDED(gpu.list->Close()), "close the failed first low-tier recording without executing");
        queue.ResetList();
        native::ResolveAdaptiveBorrowedDeliveries();
        timers::Poll(true, false);
        gpu.uploads.clear();
        gpu.retained_resources.clear();
        Check(discarded->closed.load() && !discarded->submitted.load() && session->level == 1
                  && session->applied_generation == upper_generation && session->applied_ns == upper_applied_ns
                  && session->gpu_count == 0 && session->pending_pair.frame == 0,
              "discarded first low refresh leaves the upper confirmation and warmup unmodified");
        advance(rate::NowNs() + cadence_ns);
        const auto maturation = record();
        lower_model = feature.slots[0].handle;
        Check(maturation.calls == 0 && tiers->active == 1 && feature.input_width == 12
                  && lower_model != nullptr && lower_model != discarded_model
                  && native::nr_features_created == models_before_recovery + 1
                  && native::worksets_created == worksets_before_recovery + 1,
              "discarded GPU state rebuilds the low workset and defers the new model's first Evaluate by one frame");
        const auto original = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        queue.Complete(nullptr, 0);
        const auto original_pixels = original.Pixels();
        Check(original_pixels.size() == source_values.size() * sizeof(uint16_t)
                  && std::memcmp(original_pixels.data(), source_values.data(), original_pixels.size()) == 0
                  && session->level == 1 && session->applied_generation == upper_generation && session->applied_ns == upper_applied_ns
                  && session->gpu_count == 0,
              "the maturation frame delivers original SR bits and leaves the old tier confirmation and costs intact");
        advance(rate::NowNs() + cadence_ns);
        first_lower = record();
        Check(first_lower.calls == 1 && feature.slots[0].handle == lower_model && model_resets.back() == 1
                  && session->status.generation == low_generation,
              "the first mature replacement refreshes within the original automatic trial");
      }

      const auto capture = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      queue.Complete(first_lower.timer, timing == TimingCoverage::FULL ? (native_off ? 2000 : 1500) : 0);
      const auto pixels = capture.Pixels();
      Check(pixels.size() == source_values.size() * sizeof(uint16_t)
                && std::memcmp(pixels.data(), source_values.data(), pixels.size()) != 0
                && std::memcmp(pixels.data() + 6, source_values.data() + 3, sizeof(uint16_t)) == 0,
            "first confirmed lower work really edits the game's WARP output and preserves its alpha");
      const auto low_applied_ns = session->applied_ns;
      Check(session->applied_generation == low_generation && session->status.applied[0].width == 12
                && low_applied_ns > upper_applied_ns && session->stable_after_ns == low_applied_ns + adaptive::WARMUP_NS
                && session->gpu_count == 0,
            "only complete low delivery starts its warmup with no inherited cost");
      if (!native_off) {
        Check(feature.slots[0].edit_reuse == nullptr, "low-tier execution cannot reuse without matching Off");
      }
      if (timing != TimingCoverage::NONE) {
        Check(old_half.frame != 0 && old_half.generation == upper_generation
                  && session->sampling_generation > old_half.sampling_generation,
              "the delayed input is a real completed upper half from the old request and sampling generation");
        session->ObserveGpu(old_half, rate::NowNs());
      }
      Check(session->pending_pair.frame == 0 && session->gpu_count == 0 && session->applied_ns == low_applied_ns,
            "the completed old-generation half cannot publish cost or restart the low warmup");
      cadence_ns = 1'000'000'000 / low_fps;
      next_present_ns = rate::NowNs() + cadence_ns;
      if (native_off) {
        advance(rate::NowNs() + cadence_ns);
        const auto next = record();
        Check(next.calls == 0 && feature.slots[0].edit_reuse != nullptr && feature.slots[0].edit_reuse->width == 12,
              "matching Native Off delivers a real zero-call low-grid reuse after recovery");
        queue.Complete(next.timer, 1000);
        Check(session->applied_ns == low_applied_ns && session->gpu_count == 0,
              "a low reuse during warmup cannot confirm or measure twice");
      }
      advance(std::max(session->stable_after_ns, rate::NowNs() + cadence_ns));
      rate::Snapshot lower_output;
      int64_t decision_ns = 0;
      bool observed_independent_window = false;
      const auto expected_reason = (low_fps == 100 ? adaptive::Reason::OBSERVING : adaptive::Reason::AT_MINIMUM);
      for (uint32_t cycle = 0; cycle < 30; ++cycle) {
        advance(std::max(session->next_measure_ns, rate::NowNs() + cadence_ns));
        const auto window = rate::Read(output_identity, rate::NowNs());
        const auto low = record();
        Check(tiers->active == 1 && feature.slots[0].handle == lower_model && feature.input_width == 12
                  && session->applied_ns == low_applied_ns && session->status.generation == low_generation
                  && session->paired_sampling == native_off && low.calls == 1
                  && low.timer != nullptr && low.timer->measure && low.timer->adaptive.paired == native_off,
              "each measured low refresh uses the new grid, model and sampling route");
        const auto before_low_sample = session->gpu_count;
        queue.Complete(low.timer, timing == TimingCoverage::FULL ? (native_off ? 2000 : 1500) : 0);
        if (native_off) {
          Check(session->gpu_count == before_low_sample && session->pending_pair.frame == feature.sr_sequence,
                "a completed low refresh on its own still cannot publish a cycle");
          advance(rate::NowNs() + cadence_ns);
          const auto reused = record();
          Check(reused.calls == 0 && reused.timer != nullptr && reused.timer->measure
                    && reused.timer->adaptive.pair_first_frame == session->pending_pair.frame,
                "the adjacent low base frame reuses the delivered edit and closes the cost cycle");
          queue.Complete(reused.timer, 1000);
          Check(session->gpu_count == std::min(before_low_sample + 1, session->gpu_us.size())
                    && session->gpu_us[session->gpu_count - 1] == 1500. && session->pending_pair.frame == 0,
                "the complete low x2 pair publishes the mean of its 2 ms refresh and 1 ms reuse");
        }
        if (decision_ns == 0 && window.available && window.window_begin_ns >= session->stable_after_ns
            && session->status.reason == expected_reason && session->status.output_fps == low_fps) {
          decision_ns = rate::NowNs();
          lower_output = window;
        }
        // Keep supplying output through a further independent full window.
        if (decision_ns != 0 && window.window_begin_ns >= lower_output.window_end_ns) {
          observed_independent_window = true;
          break;
        }
      }
      double gpu_us = 0., gpu_mad = 0.;
      Check(session->GpuStatistics(rate::NowNs(), &gpu_us, &gpu_mad) == (timing == TimingCoverage::FULL)
                && (timing != TimingCoverage::FULL || (gpu_us == 1500. && gpu_mad == 0.)),
            "only complete timestamp evidence supplies the fresh low-grid cost window");
      Check(observed_independent_window && decision_ns != 0
                && lower_output.source == rate::Source::PRESENT && lower_output.output == output_identity
                && lower_output.window_serial > upper_output.window_serial
                && lower_output.window_begin_ns >= low_applied_ns + adaptive::WARMUP_NS
                && lower_output.window_end_ns - lower_output.window_begin_ns >= 4 * rate::Stream::BIN_NS
                && lower_output.fps == low_fps && lower_output.split_available
                && lower_output.split_fps == std::array{double(low_fps), double(low_fps)}
                && session->status.output_fps == low_fps && session->status.reason == expected_reason
                && session->level == 1 && session->active_level == 1 && !session->trial,
            "the full post-warmup output holds an exactly on-target low grid or reports the deficient minimum");
      Dispatch<reshade::addon_event::destroy_swapchain>(&present, true);
      Dispatch<reshade::addon_event::init_swapchain>(&present, true);
      const auto replacement = present.Identity();
      Check(replacement && replacement != output_identity && !rate::IsCurrentOutput(output_identity)
                && !rate::Read(replacement, rate::NowNs()).available
                && rate::Read(unrelated.Identity(), rate::NowNs()).available,
            "registered output replacement revokes the old window even at the same resource address");
      source_frame(trial_source, 0, true, false);
      Check(session->status.display_output_generation == replacement.generation && session->status.output_fps == 0.,
            "after actual output replacement the source cannot use the previous window or the other live output's window");
      rate::ReleaseNativeViewport(71);
      native::ForgetSourceHandleLocked(trial_source);
      native::DrainRetiredResources(true);
      if (native_off) {
        std::cout << "NR_RELEASE_PARITY dx12-upscaled-adaptive-x2 paired-low output=PRESENT grid=12x12"
                     " calls=1/0 upper_us=2000 low_us=1500 discarded=1 recovered=1 stale_half_rejected=1 off_revocation_keeps_upper=1"
                     " held_100fps=1 output_replacement_rejected=1 evidence=WARP+MOCK\n";
      }
      return std::array{requested_ns - upper_applied_ns, low_applied_ns - upper_applied_ns,
          lower_output.window_begin_ns - low_applied_ns, lower_output.window_end_ns - low_applied_ns,
          decision_ns - low_applied_ns};
    };
    uintptr_t next_adaptive_source = 0xED171;
    for (const int low_fps : {100, 80}) {
      const auto full = automatic_descent(reinterpret_cast<const NVSDK_NGX_Handle*>(next_adaptive_source++),
          false, low_fps, TimingCoverage::FULL);
      const auto half = automatic_descent(reinterpret_cast<const NVSDK_NGX_Handle*>(next_adaptive_source++),
          false, low_fps, TimingCoverage::HALF);
      const auto none = automatic_descent(reinterpret_cast<const NVSDK_NGX_Handle*>(next_adaptive_source++),
          false, low_fps, TimingCoverage::NONE);
      Check(full == half && full == none, "identical output coverage gives identical request, delivery and FPS decision across timestamp inputs");
      std::cout << "NR_RELEASE_PARITY dx12-upscaled-adaptive-x2 " << (low_fps == 100 ? "unpaired-target" : "unpaired-minimum")
                << " output=PRESENT request=16x16/12x12 delivered=12x12 calls=1 warmup_ns=" << adaptive::WARMUP_NS
                << " low_fps=" << low_fps << " decision=" << (low_fps == 100 ? "OBSERVING" : "AT_MINIMUM")
                << " request_ns=" << full[0] << " delivery_ns=" << full[1]
                << " decision_window_ns=" << full[2] << '/' << full[3] << " decision_ns=" << full[4]
                << " timestamps=full/half/none costs=2000/1500,missing,missing output_replacement_rejected=1 evidence=WARP+MOCK\n";
    }
    automatic_descent(reinterpret_cast<const NVSDK_NGX_Handle*>(next_adaptive_source), true, 100, TimingCoverage::FULL);
    native::kSetAdaptiveEnabled.Store(0.f);
    native::kSetAdaptiveLevelCount.Store(0.f);
    native::kSetAdaptiveTargetFps.Store(native::kSetAdaptiveTargetFps.fallback);
    native::kSetAdaptiveMinScale.Store(native::kSetAdaptiveMinScale.fallback);
    std::cout << "PASS Upscaled adaptive x2 production parity evidence=WARP+MOCK\n";
#if defined(NR_NEXUS_DEV_CONTROL)
    // The original single-output adaptive/discard/failure source is already released above.
    // Give the rotating-output On scenario a new source lifetime and its own ordinary warmup.
    native::nr_adaptive_enabled = false;
    native::nr_follow_input_res = false;
    native::nr_resolution_scale = 1.f;
    native::edit_reuse_fg_experiment = false;
    native::edit_reuse_enabled = true;
    native::native_fg_owns = false;
    native::native_fg_public_off = true;
    native::native_fg_declaration = native::NativeFgDeclaration::kOff;
    native::mixed_present_fg_seen = false;
    edit_model_behavior = NeutralModel;
    fail_call = 0;
    native::RegisterCreatedFeatureLocked(owner, native::kFeatureDlss, &parameters);
    for (uint32_t prefix = 0; prefix < native::kContractDebounceFrames + 8; ++prefix) frame(1);
    {
      // FG environment is MOCK metadata. SR, cache shaders, ordering and copy-back use WARP;
      // this does not simulate a native/XeFG consumer or prove that FG reads the edited SR output.
      namespace xefg = addon::fg::xefg;
      namespace xi = xefg::internal;
      const auto* fg_handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xED180});
      const auto* second_fg = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xED181});
      const auto* second_source = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xED182});
      native::RegisterCreatedFeatureLocked(fg_handle, NVSDK_NGX_Feature_FrameGeneration, &parameters);
      native::OnNativeFgOptions(true);
      native::mixed_present_fg_seen = true;
      const auto native_environment = native::ReadEditReuseFgEnvironmentLocked();
      Check(native_environment.available && native_environment.provider == 1
                && native_environment.context == reinterpret_cast<uintptr_t>(fg_handle),
          "successful On declaration and a unique known FG handle expose environment metadata only");
      {
        namespace cap = addon::control_mask_capture;
        namespace timers = addon::gpu_timers;
        Check(!cap::Enabled() && !cap::CapturePending() && !cap::WorkPending(), "inline WARP fixture owns the idle capture singleton");
        struct RestoreInlineCapture {
          decltype(cap::wake_worker) wake = cap::wake_worker;
          decltype(timers::raw_observer) observer = timers::raw_observer;
          bool profiling = native::gpu_timers_enabled.exchange(true);
          int64_t stable = native::comparison_stable_after_ns[1];
          ~RestoreInlineCapture() {
            cap::Stop("fixture_complete", false);
            {
              std::unique_lock lock(cap::internal::state.mutex);
              auto& state = cap::internal::state;
              state.summary = {};
              state.rows.clear();
              state.armed = false;
              state.marker_needed = false;
              state.terminal_needed = false;
            }
            cap::internal::capture_pending = false;
            cap::internal::work_pending = false;
            cap::wake_worker = wake;
            timers::raw_observer = observer;
            native::gpu_timers_enabled = profiling;
            native::comparison_stable_after_ns[1] = stable;
          }
        } restore_inline_capture;
        timers::Poll(true, false);
        cap::wake_worker = +[] { return true; };  // No export worker or filesystem in this recording fixture.
        timers::raw_observer = cap::Observe;
        native::comparison_stable_after_ns[1] = 0;
        Check(cap::Configure(4, 30, 0, 4), "baseline and x2 share one finite existing capture request");
        const auto timed_frame = [&](int reset) {
          const auto sequence = timers::internal::next_sequence;
          const auto calls = frame(reset, false);
          const auto found = std::find_if(std::begin(timers::internal::segments), std::end(timers::internal::segments),
              [&](const auto& segment) { return segment.recorded && segment.sequence == sequence + 1; });
          Check(found != std::end(timers::internal::segments) && found->diagnostic_token != 0,
              "a successful production source write reserves exactly one complete-chain timer context");
          const auto timer_sequence = found->sequence;
          Check(SUCCEEDED(gpu.list->Close()), "inline capture recording close");
          ID3D12CommandList* lists[]{gpu.list.Get()};
          gpu.queue->ExecuteCommandLists(1, lists);
          gpu.submission_pending = true;
          Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "inline capture real completion signal");
          HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
          Check(event != nullptr, "inline capture completion event");
          const auto armed = gpu.fence->SetEventOnCompletion(gpu.serial, event);
          const auto wait = (SUCCEEDED(armed) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
          CloseHandle(event);
          Check(wait == WAIT_OBJECT_0 && SUCCEEDED(gpu.device->GetDeviceRemovedReason()), "inline capture bounded healthy WARP completion");
          gpu.submission_pending = false;
          Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
              "inline capture completed list reset");
          native::DrainRetiredResources(true);
          timers::Poll(true, false);
          submission::PruneCompletedGenerations();
          gpu.uploads.clear();
          gpu.retained_resources.clear();
          {
            std::shared_lock lock(cap::internal::state.mutex);
            const auto& rows = cap::internal::state.rows;
            const auto sample = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
              return row.timer_sequence == timer_sequence;
            });
            Check(sample != rows.end() && sample->state == cap::internal::RowState::VALID && sample->stats.valid
                      && sample->stats.sequence == timer_sequence && sample->stats.samples == 1,
                "the existing raw observer receives actual completed chain timestamps");
          }
          return calls;
        };
        native::edit_reuse_fg_experiment = true;
        native::edit_reuse_enabled = false;
        Check(timed_frame(1) == 1 && native::features.at(owner).slots[0].edit_reuse == nullptr,
            "the baseline runs ordinary NR while the MOCK FG environment is On");
        native::edit_reuse_enabled = true;
        const auto first_on_calls = timed_frame(0);
        const auto first_on_cache = native::features.at(owner).slots[0].edit_reuse;
        std::cout << "OBS DEV On first anchor: model_calls=" << first_on_calls << " reset=" << model_resets.back()
                  << " cache=" << (first_on_cache != nullptr) << '\n';
        Check(first_on_calls == 1 && first_on_cache != nullptr && first_on_cache->valid
                  && first_on_cache->contract[32] == 0 && first_on_cache->contract[33] == 0
                  && first_on_cache->contract[34] == 0
                  && first_on_cache->source_sequence == native::features.at(owner).sr_sequence,
            "DEV On establishes a fresh fixed-size SR anchor while preserving already-continuous NR History");
        const auto on_reuses = native::edit_reuse_reuses.load();
        Check(timed_frame(0) == 0 && native::edit_reuse_reuses.load() == on_reuses + 1,
            "DEV On reuses exactly once despite ordinary observed FG input");
        const auto on_final = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit_commands("DEV On neutral output readback");
        const auto on_bytes = on_final.Pixels();
        Check(std::memcmp(on_bytes.data(), source_values.data(), on_bytes.size()) == 0,
            "DEV On neutral edit preserves the current SR RGB and alpha exactly");
        Check(timed_frame(0) == 1, "DEV On refreshes on the next base frame after that reuse");
        const auto summary = cap::GetSummary();
        Check(summary.inline_edit_reuse && !summary.xefg_service && summary.recorded == 4
                  && summary.valid == 4 && summary.rejected == 0 && summary.dropped == 0,
            "one baseline and refresh/reuse/refresh each publish one real completed inline sample");
        std::shared_lock lock(cap::internal::state.mutex);
        const auto& rows = cap::internal::state.rows;
        Check(rows.size() == 4, "neither the NR branch nor a mirrored source creates an extra capture reservation");
        for (size_t index = 0; index < rows.size(); ++index) {
          const auto& context = rows[index].context;
          const bool reused = index == 2;
          Check(context.inline_edit_reuse && context.output_ready && context.source_handle == reinterpret_cast<uintptr_t>(owner)
                    && context.source_generation_available && context.source_generation == native::features.at(owner).source_generation
                    && context.source_sequence != 0 && context.model_evaluates == (reused ? 0u : 1u)
                    && context.reuse == reused && context.refresh != reused
                    && context.api_returned != reused && context.ngx_accepted != reused
                    && (!reused || context.reset_reasons == 0),
              "actual source identity, model calls, API returns and zero-reset reuse remain distinct");
          if (index != 0) {
            Check(context.source_sequence == rows[index - 1].context.source_sequence + 1,
                "capture cadence follows true source frames across baseline and x2");
          }
        }
      }
      {
        auto other_output = uav_texture(16, 16, DXGI_FORMAT_R16G16B16A16_FLOAT);
        gpu.Transition(other_output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit_commands("DEV On alternate output upload");
        edit_model_behavior = ConstantModel;
        Check(frame(1) == 1, "establish a nonzero edit before rotating the SR Output");
        const auto edited_first = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit_commands("DEV On first edited output readback");
        const auto edited_first_bytes = edited_first.Pixels();
        uint16_t first_red = 0;
        std::memcpy(&first_red, edited_first_bytes.data(), sizeof(first_red));
        Check(std::fabs(DirectX::PackedVector::XMConvertHalfToFloat(first_red)
                  - DirectX::PackedVector::XMConvertHalfToFloat(source_values[0])) > .02f,
            "the existing constant model produces a distinguishable nonzero edit");
        const auto anchor = native::features.at(owner).slots[0].edit_reuse;
        const auto anchor_generation = anchor->anchor_generation;
        const auto reuses_before_output = native::edit_reuse_reuses.load();
        parameters.Set(NVSDK_NGX_Parameter_Output, other_output.Get());
        Check(frame(0) == 0 && native::features.at(owner).slots[0].edit_reuse == anchor
                  && anchor->anchor_generation == anchor_generation
                  && native::edit_reuse_reuses.load() == reuses_before_output + 1,
            "the same logical SR source can reuse its anchor into a rotating Output without invoking NR again");
        const auto edited_second = stage_execution_cases::Capture(gpu, other_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const auto untouched_first = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit_commands("DEV On rotating target readbacks");
        const auto edited_second_bytes = edited_second.Pixels();
        const auto untouched_first_bytes = untouched_first.Pixels();
        uint16_t second_red = 0;
        std::memcpy(&second_red, edited_second_bytes.data(), sizeof(second_red));
        Check(std::fabs(DirectX::PackedVector::XMConvertHalfToFloat(second_red)
                  - DirectX::PackedVector::XMConvertHalfToFloat(source_values[0])) > .02f
                  && std::memcmp(untouched_first_bytes.data(), edited_first_bytes.data(), untouched_first_bytes.size()) == 0,
            "nonzero reuse reaches the current second Output while preserving the previous Output");
        parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
        Check(frame(0) == 1 && anchor->anchor_generation == anchor_generation + 1,
            "return to the first Output follows the same logical source's normal refresh cadence");
        std::cout << "OBS DEV On rotating Output: nonzero_reuse=1 return_refresh_reset=" << model_resets.back() << '\n';
        edit_model_behavior = NeutralModel;
        Check(frame(1) == 1, "restore the neutral model with an explicit source Reset");
      }

      const auto check_output = [&](const std::vector<uint8_t>* reference = nullptr) {
        auto capture = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit_commands("DEV stable-source edited output readback");
        const auto pixels = capture.Pixels();
        bool nonneutral = false;
        for (size_t pixel = 0; pixel < source_values.size() / 4; ++pixel) {
          for (size_t channel = 0; channel < 3; ++channel) {
            uint16_t actual;
            std::memcpy(&actual, pixels.data() + (pixel * 4 + channel) * 2, 2);
            if (reference != nullptr) {
              uint16_t expected;
              std::memcpy(&expected, reference->data() + (pixel * 4 + channel) * 2, 2);
              Check(std::fabs(DirectX::PackedVector::XMConvertHalfToFloat(actual)
                        - DirectX::PackedVector::XMConvertHalfToFloat(expected)) < .003f,
                  "DEV metadata changes preserve the actual refreshed RGB edit");
            }
            nonneutral |= std::fabs(DirectX::PackedVector::XMConvertHalfToFloat(actual)
                - DirectX::PackedVector::XMConvertHalfToFloat(source_values[pixel * 4 + channel])) > .05f;
          }
          Check(std::memcmp(pixels.data() + pixel * 8 + 6, source_values.data() + pixel * 4 + 3, 2) == 0,
              "DEV refresh and reuse preserve exact current source alpha");
        }
        Check(nonneutral, "the DEV stable-source witness contains a nonzero edit");
        return pixels;
      };
      {
        edit_model_behavior = ConstantModel;
        Check(frame(1) == 1, "establish a nonzero anchor before changing only diagnostic metadata");
        auto& feature = native::features.at(owner);
        const auto anchor = feature.slots[0].edit_reuse;
        const auto model = feature.slots[0].handle;
        const auto models = native::nr_features_created;
        const auto allocations = native::worksets_created;
        const auto source_generation = feature.source_generation;
        const auto epoch = native::history_reset_epoch.load();
        auto diagnostic_anchor_generation = anchor->anchor_generation;
        auto anchor_pixels = read_buffer(anchor->anchor.Get());
        const auto reference = check_output();
        uint32_t expected_calls = 0;
        const auto check_diagnostics = [&](const char* scenario, bool environment_available) {
          const auto reuses = native::edit_reuse_reuses.load();
          const auto calls = frame(0);
          std::cout << "OBS DEV diagnostic-only " << scenario << ": model_calls=" << calls
                    << " expected=" << expected_calls << '\n';
          Check(calls == expected_calls && native::ReadEditReuseFgEnvironmentLocked().available == environment_available,
              "diagnostic availability does not alter fixed-size SR refresh/reuse cadence");
          Check(feature.slots[0].edit_reuse == anchor && anchor->valid && feature.slots[0].handle == model
                    && native::nr_features_created == models && native::worksets_created == allocations
                    && feature.source_generation == source_generation && native::history_reset_epoch.load() == epoch
                    && anchor->contract[32] == 0 && anchor->contract[33] == 0 && anchor->contract[34] == 0,
              "FG diagnostic identity never replaces the live NR source, model, workset or contract");
          if (expected_calls == 0) {
            Check(native::edit_reuse_reuses.load() == reuses + 1 && read_buffer(anchor->anchor.Get()) == anchor_pixels,
                "diagnostic-only reuse leaves the exact nonzero refresh anchor unchanged");
          } else {
            Check(model_resets.back() == 0, "diagnostic-only refresh adds no NR Reset");
            ++diagnostic_anchor_generation;
            anchor_pixels = read_buffer(anchor->anchor.Get());
          }
          Check(anchor->anchor_generation == diagnostic_anchor_generation, "diagnostics never advance the anchor generation");
          check_output(&reference);
          expected_calls ^= 1u;
        };
        native::RegisterCreatedFeatureLocked(second_fg, NVSDK_NGX_Feature_FrameGeneration, &parameters);
        check_diagnostics("multiple FG handles", false);
        native::ForgetSourceHandleLocked(second_fg);
        check_diagnostics("unique FG handle restored", true);
        const auto before_recreate = native::ReadEditReuseFgEnvironmentLocked().generation;
        native::RegisterCreatedFeatureLocked(fg_handle, NVSDK_NGX_Feature_FrameGeneration, &parameters);
        Check(native::ReadEditReuseFgEnvironmentLocked().generation != before_recreate,
            "recreated FG handle still invalidates diagnostic attribution");
        check_diagnostics("FG handle recreated", true);
        native::ForgetSourceHandleLocked(fg_handle);
        check_diagnostics("FG handle unobserved", false);
        native::RegisterCreatedFeatureLocked(fg_handle, NVSDK_NGX_Feature_FrameGeneration, &parameters);
        check_diagnostics("FG handle observed again", true);

        const bool saved_stopped = xi::stopped.exchange(false);
        const bool saved_detours = xi::sdk_detours_enabled.exchange(true);
        const bool saved_enabled = xi::owns_stream.load();
        Check(xi::state.contexts.empty(), "isolated XeFG On metadata fixture");
        const auto make_context = [&] {
          auto context = std::make_shared<xi::Context>();
          context->queue = gpu.queue;
          context->device = gpu.device;
          context->proxy_identity = 0xED190;
          context->bridge_context_ready = context->bridge_proxy_ready = context->enabled = true;
          context->fg_environment_generation = ++xi::state.fg_environment_generation;
          return context;
        };
        auto context = make_context();
        const auto sdk_handle = reinterpret_cast<void*>(uintptr_t{0xED191});
        xi::state.contexts.emplace(sdk_handle, context);
        xi::RefreshOwnership();
        const auto xe_environment = native::ReadEditReuseFgEnvironmentLocked();
        Check(xe_environment.available && xe_environment.provider == 2
                  && native::native_fg_declaration.load() == native::NativeFgDeclaration::kOn
                  && !xefg::OwnsStream() && !xefg::QueryBridgeOff({}).available,
            "enabled XeFG changes diagnostic precedence without transferring SR resources or ownership");
        check_diagnostics("XeFG metadata precedence", true);
        const auto before_resize = native::ReadEditReuseFgEnvironmentLocked().generation;
        {
          std::unique_lock lock(xi::state.mutex);
          xi::ReleaseFrames(context.get());
        }
        Check(native::ReadEditReuseFgEnvironmentLocked().generation != before_resize,
            "XeFG metadata lifetime still changes its diagnostic generation");
        check_diagnostics("XeFG diagnostic generation", true);
        xi::state.contexts.emplace(reinterpret_cast<void*>(uintptr_t{0xED192}), make_context());
        xi::RefreshOwnership();
        check_diagnostics("multiple XeFG metadata contexts", false);
        xi::state.contexts.erase(reinterpret_cast<void*>(uintptr_t{0xED192}));
        check_diagnostics("unique XeFG metadata context restored", true);
        const auto before_replace = native::ReadEditReuseFgEnvironmentLocked().generation;
        context = make_context();
        xi::state.contexts[sdk_handle] = context;
        Check(native::ReadEditReuseFgEnvironmentLocked().generation != before_replace,
            "replacement at the same SDK handle retains a distinct diagnostic identity");
        check_diagnostics("XeFG metadata context replaced", true);
        xi::state.contexts.clear();
        xi::owns_stream = saved_enabled;
        xi::sdk_detours_enabled = saved_detours;
        xi::stopped = saved_stopped;
        check_diagnostics("native metadata restored", true);
        std::cout << "PASS DEV diagnostic-only changes: missing/ambiguous/recreated native and XeFG metadata,"
                     " stable SR history, nonzero RGB, exact alpha, immutable reuse anchor and no model/workset recreation; evidence=WARP+MOCK\n";
      }
      edit_model_behavior = NeutralModel;
      Check(frame(1) == 1, "restore the neutral model before source and ownership rejection checks");
      native::RegisterCreatedFeatureLocked(second_source, native::kFeatureDlss, &parameters);
      native::create_contracts.at(second_source).source_device12_identity =
          native::create_contracts.at(owner).source_device12_identity;
      const auto diagnostic_reuses = native::edit_reuse_reuses.load();
      Check(frame(0) == 0 && native::edit_reuse_reuses.load() == diagnostic_reuses + 1
                && native::features.at(owner).slots[0].edit_reuse->valid,
            "a second registered source does not revoke confirmed direct write-back for this SR handle");
      native::ForgetSourceHandleLocked(second_source);
      Check(frame(1) == 1, "the remaining source establishes its own fresh anchor");

      native::create_contracts.at(owner).source_type_confirmed = false;
      Check(frame(0) == 1 && native::features.at(owner).slots[0].edit_reuse == nullptr,
          "environment On cannot authorize an unconfirmed SR source");
      native::create_contracts.at(owner).source_type_confirmed = true;
      Check(frame(1) == 1, "confirmed source recovers with a refresh");
      native::native_fg_owns = true;
      const auto owned_reuses = native::edit_reuse_reuses.load();
      frame(0);
      Check(native::edit_reuse_reuses.load() == owned_reuses && native::features.at(owner).slots[0].edit_reuse == nullptr,
          "DEV On does not cross a native Present-service ownership boundary");
      native::native_fg_owns = false;
      Check(frame(1) == 1, "Upscaled ownership recovers with a refresh");

      const auto before_experiment_exit = native::features.at(owner).slots[0].edit_reuse;
      native::edit_reuse_fg_experiment = false;
      Check(frame(0) == 0 && native::features.at(owner).slots[0].edit_reuse == before_experiment_exit,
          "disabling DEV FG observation preserves the ordinary Upscaled reuse and anchor");
      native::edit_reuse_fg_experiment = true;
      Check(frame(0) == 1 && model_resets.back() == 0
                && native::features.at(owner).slots[0].edit_reuse == before_experiment_exit
                && before_experiment_exit->valid,
          "re-enabling DEV FG observation retains the model history and refresh cadence");
      const auto reset_before_off = native::history_reset_epoch.load();
      native::OnNativeFgOptions(false);
      Check(native::history_reset_epoch.load() == reset_before_off && frame(0) == 0
                && native::features.at(owner).slots[0].edit_reuse->contract[32] == 0,
          "On-to-Off without an owner retains the same fixed-size source history");
      native::OnNativeFgOptions(true);
      native::mixed_present_fg_seen = true;
      Check(frame(1) == 1, "native On retains its confirmed source");
      {
        namespace options = addon::fg::native_options;
        funnel_accounting_tests::Restore substitutions;
        substitutions.Value(&options::internal::state.real);
        substitutions.Atomic(&options::internal::options_changed);
        substitutions.Atomic(&options::internal::stopped);
        options::internal::state.real = +[](const sl::ViewportHandle&, const sl::DLSSGOptions&) {
          return sl::Result::eOk;
        };
        options::internal::stopped = false;
        options::Configure(native::OnNativeFgOptions);
        edit_model_behavior = ConstantModel;
        for (const bool hooked : {false, true}) {
          native::OnNativeFgOptions(false);
          Check(frame(1) == 1, "start an Options contrast from the same confirmed Off source");
          auto& feature = native::features.at(owner);
          const auto anchor = feature.slots[0].edit_reuse;
          const auto contract = anchor->contract;
          const auto model = feature.slots[0].handle;
          const auto models = native::nr_features_created;
          const auto allocations = native::worksets_created;
          const auto epoch = native::history_reset_epoch.load();
          auto options_anchor_generation = anchor->anchor_generation;
          auto anchor_pixels = read_buffer(anchor->anchor.Get());
          const auto reference = check_output();
          const std::array modes{false, true, true, false, false};
          for (size_t index = 1; index < modes.size(); ++index) {
            const auto options_generation = native::native_fg_options_generation.load();
            const auto diagnostic_generation = native::edit_reuse_native_fg_generation.load();
            const auto changed_ns = native::native_fg_options_changed_ns.load();
            {
              // The production wrapper can hold this lock when the SDK calls SetOptions.
              addon::RuntimeLock lock(native::runtime_mutex);
              if (hooked) {
                sl::DLSSGOptions request;
                request.mode = (modes[index] ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff);
                request.numFramesToGenerate = 1;
                Check(options::internal::HookedSetOptions(sl::ViewportHandle{0}, request) == sl::Result::eOk,
                    "real SetOptions wrapper forwards the MOCK SDK success to the production callback");
              } else {
                native::OnNativeFgOptions(modes[index]);
              }
            }
            const auto calls = frame(0);
            const uint32_t expected_calls = (index % 2 == 0 ? 1u : 0u);
            std::cout << "OBS DEV Options " << (hooked ? "HookedSetOptions" : "OnNativeFgOptions")
                      << " frame=" << index + 1 << " mode=" << (modes[index] ? "On" : "Off")
                      << " model_calls=" << calls << " expected=" << expected_calls << '\n';
            Check(calls == expected_calls, "actual owner-free Off/On/Off callbacks preserve cadence 1/0/1/0/1");
            const uint64_t changes = (modes[index] != modes[index - 1] ? 1u : 0u);
            Check(native::native_fg_options_generation.load() == options_generation + changes
                      && native::edit_reuse_native_fg_generation.load() == diagnostic_generation + changes
                      && (changes != 0 ? native::native_fg_options_changed_ns.load() >= changed_ns
                                       : native::native_fg_options_changed_ns.load() == changed_ns)
                      && native::native_fg_public_off.load() != modes[index]
                      && native::native_fg_declaration.load() == (modes[index]
                          ? native::NativeFgDeclaration::kOn : native::NativeFgDeclaration::kOff),
                "actual callbacks retain declaration intervals and diagnostic generation, including duplicate requests");
            Check(!native::native_fg_owns.load() && native::history_reset_epoch.load() == epoch
                      && feature.slots[0].edit_reuse == anchor && anchor->valid && anchor->contract == contract
                      && feature.slots[0].handle == model && native::nr_features_created == models
                      && native::worksets_created == allocations,
                "owner-free Options changes preserve the same NR contract, source, model and workset");
            if (expected_calls == 0) {
              Check(read_buffer(anchor->anchor.Get()) == anchor_pixels,
                  "Options callback reuse preserves the nonzero refresh anchor exactly");
            } else {
              Check(model_resets.back() == 0, "owner-free Options refresh adds no model Reset");
              ++options_anchor_generation;
              anchor_pixels = read_buffer(anchor->anchor.Get());
            }
            Check(anchor->anchor_generation == options_anchor_generation, "Options callbacks never advance anchor generation");
            check_output(&reference);
          }
          std::cout << "PASS DEV owner-free " << (hooked ? "HookedSetOptions" : "OnNativeFgOptions")
                    << " Off/On/Off: model_calls=1/0/1/0/1, stable history, nonzero RGB, exact alpha; evidence=WARP+MOCK\n";
        }
        edit_model_behavior = NeutralModel;
        Check(frame(1) == 1, "restore the neutral model after actual Options callback contrasts");
      }
      native::edit_reuse_fg_experiment = false;
      native::OnNativeFgOptions(false);
      native::ForgetSourceHandleLocked(fg_handle);
      for (uint32_t prefix = 0; prefix < 4; ++prefix) frame(1);
      std::cout << "PASS DEV On + WARP SR x2: shared fixed-size eligibility, unique source, diagnostic-independent history, ownership rejection and exact output; actual FG handoff=NotRun\n";
    }
    native::ForgetSourceHandleLocked(owner);
    native::DrainRetiredResources(true);
    Check(!native::features.contains(owner) && !native::created_feature_ids.contains(owner)
              && std::none_of(native::worksets.begin(), native::worksets.end(),
                  [&](const auto& entry) { return entry.first.handle == owner; }),
        "the separate On source lifetime releases its rotating-output worksets through normal retirement");
#endif
    // Exercise two independent temporal Look passes through the same successful SR wrapper.
    native::RegisterCreatedFeatureLocked(owner, native::kFeatureDlss, &parameters);
    native::created_feature_ids[owner] = native::kFeatureDlss;
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
    native::pass_look_result[0].follow = false;
    native::pass_look_result[0].mode = false;
    native::pass_look_result[0].detail_stability = 0;
    native::detail_stability = 0;
    native::direct_api.create = stage_execution_cases::Create;
    edit_model_behavior = same_stage_resolution_cases::Evaluate;
    auto& pair_calls = same_stage_resolution_cases::evaluations;
    same_stage_resolution_cases::stamps = {0x3900, 0x3b00};
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.f);
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f);

    const auto pair_frame = [&](const char* label, int reset, uint64_t expected_calls) {
      pair_calls.clear();
      const auto calls = frame(reset);
      const auto final = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      submit_commands("two-Pass Upscaled writeback readback");
      std::cout << "OBS Upscaled multipass " << label << " calls=" << calls << " fence=" << gpu.serial << '\n';
      Check(calls == expected_calls && pair_calls.size() == expected_calls, "Upscaled cadence applies to the complete group");
      return final.Pixels();
    };
    bool pair_ready = false;
    for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8; ++attempt) {
      pair_calls.clear();
      const auto calls = frame(1);
      std::cout << "OBS Upscaled multipass warmup=" << attempt << " calls=" << calls << '\n';
      if (calls == 2) {
        pair_ready = true;
        break;
      }
    }
    Check(pair_ready, "Upscaled two-Pass group matures");
    auto& pair_slots = native::features.at(owner).slots;
    const auto pair_refresh = pair_frame("refresh", 1, 2);
    const auto read_red = [](const std::vector<uint8_t>& bytes, size_t pixel) {
      uint16_t red = 0;
      std::memcpy(&red, bytes.data() + pixel * 8, sizeof(red));
      return DirectX::PackedVector::XMConvertHalfToFloat(red);
    };
    constexpr size_t center = 8 * 16 + 8;
    const auto first_layer_input = pair_calls[0].input.Pixels();
    const auto second_layer_input = pair_calls[1].input.Pixels();
    Check(std::abs(read_red(second_layer_input, center) - read_red(first_layer_input, center)) > .025f,
          "Pass 2 Evaluate reads edited current-frame color from Pass 1");
    std::array<uint32_t, 2> look_written{};
    std::array<uint64_t, 2> anchor_generations{};
    for (size_t pass = 0; pass < 2; ++pass) {
      const auto& call = pair_calls[pass];
      const auto& slot = pair_slots[pass];
      std::cout << "OBS Upscaled multipass pass=" << pass + 1 << " model=" << call.preset
                << " size=" << call.input.width << 'x' << call.input.height << " intensity=" << call.intensity
                << " reset=" << call.reset << " look=" << slot.look_history.valid
                << " anchor=" << (slot.edit_reuse != nullptr && slot.edit_reuse->valid) << '\n';
      Check(call.handle == slot.handle && call.preset == pass + 1 && call.intensity == (pass == 0 ? .5f : .75f)
                && call.input.width == 16 && call.input.height == 16 && call.reset == 1,
            "Upscaled refresh sends each Pass's real model, working size, strength and Reset");
      Check(slot.look_history.valid && slot.look_history.buffers[0] != nullptr && slot.edit_reuse != nullptr
                && slot.edit_reuse->valid,
            "each temporal layer owns a Look history and a pre-Look anchor");
      look_written[pass] = slot.look_history.written;
      anchor_generations[pass] = slot.edit_reuse->anchor_generation;
    }
    Check(pair_slots[0].look_history.buffers[0] != pair_slots[1].look_history.buffers[0]
              && pair_slots[0].edit_reuse != pair_slots[1].edit_reuse,
          "two temporal layers never share Look buffers or the edit anchor");
    const auto look_restarts = native::look_history_restarts.load();
    const auto group_reuses = native::edit_reuse_reuses.load();
    auto current_source = source_values;
    for (size_t channel = 0; channel < 3; ++channel) {
      current_source[center * 4 + channel] = DirectX::PackedVector::XMConvertFloatToHalf(
          DirectX::PackedVector::XMConvertHalfToFloat(current_source[center * 4 + channel]) * 1.04f);
    }
    current_source[center * 4 + 3] = 0x3a00;
    std::vector<float> reveal(16 * 16, .5f);
    reveal.front() = .9f;
    upload(source.Get(), current_source, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, 16 * 8);
    upload(wrapper_depth.Get(), reveal, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, 16 * 4);
    const auto pair_reuse = pair_frame("reuse", 0, 0);
    const float refresh_red = read_red(pair_refresh, center);
    const float reuse_red = read_red(pair_reuse, center);
    const float original_red = DirectX::PackedVector::XMConvertHalfToFloat(current_source[center * 4]);
    Check(std::abs(reuse_red - original_red) > .05f && std::abs(reuse_red - refresh_red) > .003f,
          "zero-model Look reuse writes a nonneutral edit of new RGB, distinct from the last output");
    Check(std::memcmp(pair_reuse.data(), current_source.data(), 8) == 0,
          "depth rejection in the group selects the current original RGBA");
    for (size_t pixel = 0; pixel < current_source.size() / 4; ++pixel) {
      Check(std::memcmp(pair_reuse.data() + pixel * 8 + 6, &current_source[pixel * 4 + 3], 2) == 0,
            "Upscaled two-Pass reuse preserves the exact current alpha");
    }
    for (size_t pass = 0; pass < 2; ++pass) {
      const auto& slot = pair_slots[pass];
      Check(slot.look_history.valid && slot.look_history.written != look_written[pass] && slot.edit_reuse != nullptr
                && slot.edit_reuse->anchor_generation == anchor_generations[pass],
            "reuse advances each Look history without moving either model anchor");
    }
    Check(native::look_history_restarts.load() == look_restarts && native::edit_reuse_reuses.load() == group_reuses + 1,
          "the whole group reuses once without consuming either model's old Reset");
    const auto& pair_work = native::worksets.at(native::WorksetKey{owner, output.Get()});
    const auto reused_layer_input = stage_execution_cases::Capture(gpu, pair_work.proxy, pair_work.proxy_state);
    submit_commands("two-Pass Upscaled reused tail input readback");
    const float reused_layer_red = read_red(reused_layer_input.Pixels(), center);
    const float refreshed_layer_red = read_red(second_layer_input, center);
    const float unedited_layer_red = read_red(first_layer_input, center);
    std::cout << "OBS Upscaled multipass tail_proxy_R=" << refreshed_layer_red << '/' << reused_layer_red
              << " original_proxy_R=" << unedited_layer_red << " final_R=" << refresh_red << '/' << reuse_red << '\n';
    Check(std::abs(reused_layer_red - refreshed_layer_red) > .001f
              && std::abs(reused_layer_red - refreshed_layer_red) < std::abs(reused_layer_red - unedited_layer_red),
          "Pass 2 reuse encodes this frame's edited Pass 1 color and retains the upstream edit");

    set_native_fg_mode(false);
    pair_frame("refresh-after-reuse", 0, 2);
    Check(pair_calls[0].reset == 0 && pair_calls[1].reset == 0
              && pair_calls[0].motion != wrapper_motion.Get() && pair_calls[1].motion != wrapper_motion.Get(),
          "both real models advance from their last refresh using composed motion");
    {
      const auto& feature = native::features.at(owner);
      const auto source_generation = feature.source_generation;
      const auto reset_epoch = native::history_reset_epoch.load();
      const auto models = native::nr_features_created;
      const auto worksets = native::worksets_created;
      const native::WorksetKey workset_key{owner, output.Get()};
      const auto* workset_proxy = native::worksets.at(workset_key).proxy;
      const auto reuses_before_options = native::edit_reuse_reuses.load();
      const auto fallbacks_before_options = native::edit_reuse_fallbacks.load();
      const std::array model_handles{pair_slots[0].handle, pair_slots[1].handle};
      const std::array anchors{pair_slots[0].edit_reuse, pair_slots[1].edit_reuse};
      std::array generations{anchors[0]->anchor_generation, anchors[1]->anchor_generation};
      std::array anchor_pixels{read_buffer(anchors[0]->anchor.Get()), read_buffer(anchors[1]->anchor.Get())};
      const std::array modes{true, true, false, false};
      for (size_t index = 0; index < modes.size(); ++index) {
        set_native_fg_mode(modes[index]);
        const bool refresh = (index % 2 != 0);
        const auto pixels = pair_frame(modes[index] ? "options-on" : "options-off", 0, refresh ? 2u : 0u);
        Check(feature.source_generation == source_generation && native::history_reset_epoch.load() == reset_epoch
                  && native::nr_features_created == models && native::worksets_created == worksets
                  && native::worksets.at(workset_key).proxy == workset_proxy
                  && native::look_history_restarts.load() == look_restarts
                  && native::edit_reuse_reuses.load() == reuses_before_options + (index + 2) / 2
                  && native::edit_reuse_fallbacks.load() == fallbacks_before_options,
              "owner-free Options preserve the two-Pass source, workset and history without fallback");
        for (size_t pass = 0; pass < anchors.size(); ++pass) {
          const auto& slot = pair_slots[pass];
          const auto anchor_bytes = read_buffer(anchors[pass]->anchor.Get());
          if (refresh) {
            Check(pair_calls[pass].handle == model_handles[pass] && pair_calls[pass].reset == 0,
                  "owner-free Options refresh the same two models without an Evaluate Reset");
            ++generations[pass];
            anchor_pixels[pass] = anchor_bytes;
          } else {
            Check(anchor_bytes == anchor_pixels[pass], "two-Pass reuse retains each refresh anchor byte-for-byte");
          }
          Check(slot.handle == model_handles[pass] && slot.edit_reuse == anchors[pass] && anchors[pass]->valid
                    && anchors[pass]->anchor_generation == generations[pass],
                "two-Pass Options preserve model and anchor identity with refresh-only generations");
        }
        Check(std::abs(read_red(pixels, center) - original_red) > .05f,
              "two-Pass Options contrast writes a nonneutral edit to the current SR output");
        for (size_t pixel = 0; pixel < current_source.size() / 4; ++pixel) {
          Check(std::memcmp(pixels.data() + pixel * 8 + 6, &current_source[pixel * 4 + 3], 2) == 0,
                "two-Pass Options contrast preserves exact current alpha");
        }
      }
      std::cout << "PASS two-Pass Upscaled owner-free SetOptions Off/On/Off: model_calls=2/0/2/0/2,"
                   " stable history, nonzero RGB, exact alpha; evidence=WARP+MOCK\n";
    }
    pair_slots[1].pending_reset = true;
    pair_slots[1].pending_reset_reasons = native::kResetRecovery;
    const auto restarts_before_tail_reset = native::look_history_restarts.load();
    pair_frame("tail-reset", 0, 2);
    Check(pair_calls[0].reset == 0 && pair_calls[1].reset == 1
              && native::look_history_restarts.load() == restarts_before_tail_reset + 1,
          "an invalid tail anchor refreshes the group and restarts only the tail's model and Look history");

    // Both directions matter: the neutral layer can run before or after
    // the only edit. Depth and color rejection still win at the final output.
    bool neutral_compositions_match = true;
    for (uint32_t neutral_model = 1; neutral_model <= 2; ++neutral_model) {
      edit_model_behavior = (neutral_model == 1 ? OneEditedLayer<1> : OneEditedLayer<2>);
      const std::vector<float> stable_depth(16 * 16, .5f);
      upload(source.Get(), source_values, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, 16 * 8);
      upload(wrapper_depth.Get(), stable_depth, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, 16 * 4);
      Check(frame(1) == 2, "one neutral layer still evaluates on a full group refresh");
      const auto refreshed_color = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      submit_commands("one neutral layer refresh readback");

      auto next_source = source_values;
      constexpr size_t color_disocclusion = center + 3;
      for (size_t channel = 0; channel < 3; ++channel) {
        next_source[color_disocclusion * 4 + channel] = DirectX::PackedVector::XMConvertFloatToHalf(2.f);
      }
      for (size_t pixel = 0; pixel < next_source.size() / 4; ++pixel) {
        next_source[pixel * 4 + 3] = 0x32f0;
      }
      upload(source.Get(), next_source, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, 16 * 8);
      upload(wrapper_depth.Get(), reveal, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, 16 * 4);
      Check(frame(0, false) == 0, "a neutral layer never forces Evaluate on an eligible group reuse");
      const auto reused_color = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      submit_commands("one neutral layer reuse and writeback readback");
      const auto pixels = reused_color.Pixels();
      const float game_red = DirectX::PackedVector::XMConvertHalfToFloat(next_source[center * 4]);
      const float edited_red = read_red(refreshed_color.Pixels(), center);
      const float reused_red = read_red(pixels, center);
      std::cout << "OBS Upscaled neutral model=" << neutral_model << " original/refresh/reuse R="
                << game_red << '/' << edited_red << '/' << reused_red << " fence=" << gpu.serial << '\n';
      Check(std::abs(edited_red - game_red) > .05f, "the nonneutral layer changes the full group refresh output");
      neutral_compositions_match &= std::abs(reused_red - game_red) > .05f;
      Check(std::memcmp(pixels.data(), next_source.data(), 8) == 0
                && std::memcmp(pixels.data() + color_disocclusion * 8, &next_source[color_disocclusion * 4], 8) == 0,
            "real depth and color rejection select exact current SR RGBA across the group");
      for (size_t pixel = 0; pixel < next_source.size() / 4; ++pixel) {
        Check(std::memcmp(pixels.data() + pixel * 8 + 6, &next_source[pixel * 4 + 3], 2) == 0,
              "neutral composition preserves current alpha on every pixel");
      }
    }
    Check(neutral_compositions_match, "a valid neutral edit in either layer preserves the other layer's edit");
    edit_model_behavior = same_stage_resolution_cases::Evaluate;
    upload(source.Get(), current_source, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, 16 * 8);
    upload(wrapper_depth.Get(), reveal, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, 16 * 4);

    fail_call = model_calls + 2;
    const auto tail_failure = pair_frame("second-model-failure", 1, 2);
    fail_call = 0;
    Check(std::memcmp(tail_failure.data(), current_source.data(), tail_failure.size()) == 0,
          "a model failure after an edited first Pass leaves the entire current SR output byte-exact");
    Check(std::ranges::none_of(std::span{pair_slots}.first(2), [](const auto& slot) {
            return slot.edit_reuse != nullptr && slot.edit_reuse->valid;
          }),
          "neither temporal layer publishes a valid cache when the tail fails");
    const auto reuses_after_failure = native::edit_reuse_reuses.load();
    pair_calls.clear();
    const auto retry_calls = frame(0);
    std::cout << "OBS Upscaled multipass failure-retry calls=" << retry_calls << " fence=" << gpu.serial << '\n';
    Check(native::edit_reuse_reuses.load() == reuses_after_failure,
          "ordinary retry cannot reuse the first Pass's partially advanced anchor");
    std::cout << "PASS two-Pass Upscaled x2 evidence=WARP+MOCK cadence=2/0/2, GPU tail input and writeback, "
                 "independent Look/Reset and failed-tail original RGBA\n";

    {
      auto& tail_look = native::pass_look_result[0];
      const auto warm_group = [&](bool x2) {
        native::ForgetSourceHandleLocked(owner);
        native::DrainRetiredResources(true);
        native::RegisterCreatedFeatureLocked(owner, native::kFeatureDlss, &parameters);
        native::created_feature_ids[owner] = native::kFeatureDlss;
        native::edit_reuse_enabled = x2;
        for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8; ++attempt) {
          pair_calls.clear();
          if (frame(1) == 2) return;
        }
        Check(false, "Upscaled A-to-B group matures");
      };

      native::detail_stability = 2;
      tail_look.detail_stability = 1;
      constexpr std::array<uint64_t, 4> cadence{2, 0, 2, 0};
      std::array<std::vector<uint8_t>, cadence.size()> full_reference;
      for (const bool change_dormant_pass : {false, true}) {
        warm_group(true);
        const auto& feature = native::features.at(owner);
        const auto& slots = feature.slots;
        std::array<std::shared_ptr<reuse::State>, 2> anchors;
        std::array<uint64_t, 2> generations{};
        uint32_t previous_history = slots[0].look_history.written;
        for (size_t sample = 0; sample < cadence.size(); ++sample) {
          if (change_dormant_pass && sample == 1) {
            native::pass_transfer_strength[1] = 0.f;
            native::pass_color_strength[1] = 0.f;
            native::pass_resolution_mode[1] = 2;
            native::pass_resolution_scale[1] = .75f;
            native::pass_look_result[1].follow = false;
            native::pass_look_result[1].detail_stability = 2;
          }
          const auto stabilized = native::look_stability_passes.load();
          auto pixels = pair_frame("full-grid head Look", sample == 0 ? 1 : 0, cadence[sample]);
          const bool refreshed = cadence[sample] == 2;
          for (size_t pass = 0; pass < anchors.size(); ++pass) {
            const auto& cache = slots[pass].edit_reuse;
            Check(cache != nullptr && cache->valid && cache->reused == !refreshed
                      && cache->width == 16 && cache->height == 16 && cache->source_sequence == feature.sr_sequence,
                  "full-grid Upscaled gives each layer an anchor for the current base frame");
            if (refreshed) {
              Check(pair_calls[pass].handle == slots[pass].handle && pair_calls[pass].preset == pass + 1
                        && pair_calls[pass].input.width == 16 && pair_calls[pass].input.height == 16
                        && pair_calls[pass].reset == (sample == 0 ? 1 : 0),
                    "full-grid Upscaled refresh evaluates A then B at the original size and Reset");
            }
            if (sample == 0) {
              anchors[pass] = cache;
              generations[pass] = cache->anchor_generation;
            } else {
              generations[pass] += (refreshed ? 1u : 0u);
              Check(cache == anchors[pass] && cache->anchor_generation == generations[pass],
                    "only a model refresh advances each Upscaled layer's anchor generation");
            }
          }
          const auto& history = slots[0].look_history;
          Check(anchors[0] != anchors[1] && history.valid && history.written != previous_history
                    && !slots[1].look_history.valid && slots[1].look_history.buffers[0] == nullptr
                    && native::look_stability_passes.load() == stabilized + 1 && slots[2].handle == nullptr,
                "only the enabled head Look advances, including on a zero-model frame");
          previous_history = history.written;
          if (change_dormant_pass) {
            Check(pixels == full_reference[sample], "dormant Pass 3 settings preserve every Upscaled output byte");
          } else {
            full_reference[sample] = std::move(pixels);
          }
        }
      }
      std::cout << "PASS Upscaled A-to-B full-grid head Look: model_calls=2/0/2/0, "
                   "per-layer anchors and dormant Pass 3 RGBA equality; evidence=WARP+MOCK\n";

      // Keep the same-tail control and put Look and the low grid on separate layers in both orders.
      struct SplitFrame {
        std::array<std::vector<uint8_t>, 2> inputs;
        std::vector<uint8_t> final_output;
      };
      struct SplitCase {
        std::array<uint32_t, 2> sizes;
        uint32_t temporal_pass;
      };
      constexpr std::array split_cases{
          SplitCase{.sizes = {12, 16}, .temporal_pass = 1},
          SplitCase{.sizes = {16, 12}, .temporal_pass = 0},
          SplitCase{.sizes = {16, 12}, .temporal_pass = 1},
      };
      constexpr size_t tail_center = 6 * 12 + 6;
      constexpr std::array<std::array<uint16_t, 2>, 3> split_stamps{{
          {0x3900, 0x3b00}, {0x3a00, 0x3b00}, {0x3a00, 0x38c0}}};
      native::look_upsample = addon::look::kUpsampleClassic;
      native::pass_resolution_mode[0] = 2;
      for (const auto& [sizes, temporal_pass] : split_cases) {
        native::nr_resolution_scale = static_cast<float>(sizes[0]) / 16.f;
        native::pass_resolution_scale[0] = static_cast<float>(sizes[1]) / 16.f;
        std::cout << "OBS Upscaled split grids=" << sizes[0] << '/' << sizes[1]
                  << " temporal_pass=" << temporal_pass + 1 << '\n';
        native::detail_stability = (temporal_pass == 0 ? 2u : 1u);
        tail_look.detail_stability = (temporal_pass == 1 ? 2u : 1u);
        std::array<SplitFrame, split_stamps.size()> reference;
        for (const bool x2 : {false, true}) {
          same_stage_resolution_cases::stamps = split_stamps[0];
          warm_group(x2);
          const auto& work = native::worksets.at(native::WorksetKey{owner, output.Get()});
          Check(work.resolution_tail != nullptr && work.proxy != work.resolution_tail->proxy
                    && work.nr_output != work.resolution_tail->nr_output,
                "independent Upscaled pair has distinct model resources");
          const auto& slots = native::features.at(owner).slots;
          const auto reuses = native::edit_reuse_reuses.load();
          const auto refreshes = native::edit_reuse_refreshes.load();
          uint32_t previous_history = slots[temporal_pass].look_history.written;
          for (size_t sample = 0; sample < split_stamps.size(); ++sample) {
            same_stage_resolution_cases::stamps = split_stamps[sample];
            auto final_output = pair_frame("split temporal", sample == 0 ? 1 : 0, 2);
            for (size_t pass = 0; pass < sizes.size(); ++pass) {
              const auto& call = pair_calls[pass];
              Check(call.handle == slots[pass].handle && call.preset == pass + 1
                        && call.input.width == sizes[pass] && call.input.height == sizes[pass]
                        && call.output.width == sizes[pass] && call.output.height == sizes[pass],
                    "split Upscaled Evaluate binds each model's actual input and output extent");
            }
            const auto& history = slots[temporal_pass].look_history;
            Check(slots[1 - temporal_pass].look_history.buffers[0] == nullptr && history.valid
                      && history.width == sizes[temporal_pass] && history.height == sizes[temporal_pass]
                      && history.written != previous_history,
                  "split Upscaled advances the selected Look history on its working grid");
            previous_history = history.written;
            std::array inputs{pair_calls[0].input.Pixels(), pair_calls[1].input.Pixels()};
            Check(final_output.size() == 16 * 16 * 8, "split Upscaled writes the full-resolution SR output");
            if (x2) {
              Check(inputs == reference[sample].inputs && final_output == reference[sample].final_output,
                    "split Upscaled x2 fallback matches both model inputs and final RGBA with x2 off");
            } else {
              reference[sample] = {.inputs = std::move(inputs), .final_output = std::move(final_output)};
            }
            Check(native::edit_reuse_reuses.load() == reuses && native::edit_reuse_refreshes.load() == refreshes
                      && slots[0].edit_reuse == nullptr && slots[1].edit_reuse == nullptr,
                  "split temporal Upscaled publishes neither reuse nor a refresh anchor");
          }
        }
        if (sizes[0] == 16 && temporal_pass == 1) {
          Check(std::abs(read_red(reference[1].inputs[1], tail_center)
                         - read_red(reference[0].inputs[1], tail_center)) > .025f,
                "changing only the first model changes the tail's actual input");
          Check(reference[2].inputs[1] == reference[1].inputs[1]
                    && std::abs(read_red(reference[2].final_output, center)
                                - read_red(reference[1].final_output, center)) > .025f,
                "changing only the tail model changes the actual final writeback");
        }
        std::cout << "PASS Upscaled split temporal Look x2 fallback: grids=" << sizes[0] << '/' << sizes[1]
                  << " temporal_pass=" << temporal_pass + 1
                  << ", both model inputs and final RGBA match x2 off; evidence=WARP+MOCK\n";
      }
      // A spatial-only tail can use the terminal cache. Its own Look must
      // invalidate that cache even when the parent has no Look surfaces.
      tail_look.detail_stability = 1;
      tail_look.mode = true;
      tail_look.stabilize = 0;
      tail_look.strength = .5f;
      same_stage_resolution_cases::stamps = split_stamps[0];
      const auto reuses = native::edit_reuse_reuses.load();
      pair_frame("split spatial refresh", 1, 2);
      const auto& slots = native::features.at(owner).slots;
      const auto terminal_anchor = slots[0].edit_reuse;
      Check(terminal_anchor != nullptr && terminal_anchor->valid && terminal_anchor->width == 16
                && terminal_anchor->height == 16 && slots[1].edit_reuse == nullptr
                && pair_calls[0].input.width == 16 && pair_calls[1].input.width == 12,
            "spatial Upscaled uses one full-stage terminal anchor with distinct model grids");
      const auto terminal_generation = terminal_anchor->anchor_generation;
      pair_frame("split spatial stable reuse", 0, 0);
      Check(native::edit_reuse_reuses.load() == reuses + 1 && slots[0].edit_reuse == terminal_anchor
                && terminal_anchor->reused && terminal_anchor->anchor_generation == terminal_generation,
            "a stable spatial-only split group reuses the same terminal edit generation");
      const auto before_look_change = pair_frame("split spatial next refresh", 0, 2);
      Check(slots[0].edit_reuse == terminal_anchor && terminal_anchor->anchor_generation == terminal_generation + 1,
            "the next full refresh advances the Upscaled terminal anchor once");
      tail_look.strength = 1.5f;
      pair_calls.clear();
      const auto changed_calls = frame(0, false);
      const auto changed_output = stage_execution_cases::Capture(
          gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      submit_commands("split Upscaled spatial Look writeback readback");
      const float before_red = read_red(before_look_change, center);
      const float after_red = read_red(changed_output.Pixels(), center);
      std::cout << "OBS Upscaled split spatial tail Look change calls=" << changed_calls
                << " final_R=" << before_red << '/' << after_red << " fence=" << gpu.serial << '\n';
      Check(changed_calls == 2 && pair_calls.size() == 2 && pair_calls[1].input.width == 12
                && native::edit_reuse_reuses.load() == reuses + 1,
            "changing only the split tail Look refreshes both models before reuse");
      Check(std::abs(after_red - before_red) > .05f,
            "the new spatial tail Look reaches the full-resolution SR output");
      std::cout << "PASS Upscaled split spatial Look: stable terminal reuse and tail Look refresh; evidence=WARP+MOCK\n";
    }

    native::ReleaseFeatureStates();
    native::RetireAllWorksets();
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
        Check(SUCCEEDED(info->GetMessage(index, message, &bytes)), "edit reuse debug message");
        Check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
      }
    }
    std::cout << "PASS edit reuse evidence=WARP+MOCK wrapper cadence, Reset/settings/gap/jitter/adaptive-On-Off/failure/discard/lifecycle; NVIDIA runtime not exercised\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL edit reuse contracts: " << error.what() << '\n';
    if (info != nullptr) {
      for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (SUCCEEDED(info->GetMessage(index, message, &bytes))
            && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
          std::cerr << message->pDescription << '\n';
        }
      }
    }
    return 1;
  }
}
}  // namespace nr_edit_reuse_gpu_cases
