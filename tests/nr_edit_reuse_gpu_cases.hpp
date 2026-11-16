/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <DirectXPackedVector.h>
#include <iostream>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#include <edit_reuse.hpp>

// Included after the native fixture's model substitutes and real GPU host.
namespace nr_edit_reuse_gpu_cases {
namespace addon = renodx::addons::dlss5;
namespace reuse = addon::edit_reuse;

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
  const std::array<uint16_t, 4> encoded{
      DirectX::PackedVector::XMConvertFloatToHalf(.85f), DirectX::PackedVector::XMConvertFloatToHalf(.75f),
      DirectX::PackedVector::XMConvertFloatToHalf(.9f), DirectX::PackedVector::XMConvertFloatToHalf(.875f)};
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
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = CreateModel;
    native::direct_api.evaluate = NeutralModel;
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
    std::vector<uint16_t> source_values(16 * 16 * 4);
    for (uint32_t pixel = 0; pixel < 16 * 16; ++pixel) {
      source_values[pixel * 4] = DirectX::PackedVector::XMConvertFloatToHalf(.25f + pixel / 2048.f);
      source_values[pixel * 4 + 1] = DirectX::PackedVector::XMConvertFloatToHalf(.375f);
      source_values[pixel * 4 + 2] = DirectX::PackedVector::XMConvertFloatToHalf(.5f);
      source_values[pixel * 4 + 3] = DirectX::PackedVector::XMConvertFloatToHalf(.25f + .125f * (pixel % 4));
    }
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
    parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f);
    const auto* owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xED170});
    native::RegisterCreatedFeatureLocked(owner, native::kFeatureDlss, &parameters);
    native::created_feature_ids[owner] = native::kFeatureDlss;
    const auto frame = [&](int reset, bool submit = true, bool mirror = false) {
      parameters.Set(NVSDK_NGX_Parameter_Reset, reset);
      gpu.list->ClearState(nullptr);
      const auto before = model_calls;
      const auto sequence = native::features.at(owner).sr_sequence;
      native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(CopySr);
      native::ngx_slot_real[0].evaluate_c = reinterpret_cast<void*>(MirrorSr);
      const auto result = (mirror ? native::HookedEvaluateFeatureCSlot<0>(gpu.list.Get(), owner, &parameters, nullptr)
                                  : native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr));
      native::ngx_slot_real[0].evaluate = nullptr;
      native::ngx_slot_real[0].evaluate_c = nullptr;
      Check(NVSDK_NGX_SUCCEED(result), "copy SR remains successful through cache wrapper");
      Check(native::features.at(owner).sr_sequence == sequence + 1, "nested C/C++ SR mirror advances one source sample");
      if (submit) {
        submit_commands("wrapper frame");
        native::DrainRetiredResources(true);
      }
      return model_calls - before;
    };
    for (uint32_t prefix = 0; prefix < native::kContractDebounceFrames + 8; ++prefix) frame(1);
    Check(native::features.at(owner).slots[0].edit_reuse != nullptr, "production wrapper establishes an edit anchor");
    Check(frame(1) == 1, "Reset forces a real NR refresh");
    auto anchor_state = native::features.at(owner).slots[0].edit_reuse;
    const auto generation = anchor_state->anchor_generation;
    const auto wrapper_anchor = read_buffer(anchor_state->anchor.Get());
    const auto before_reuses = native::edit_reuse_reuses.load();
    Check(frame(0, true, true) == 0 && native::edit_reuse_reuses.load() == before_reuses + 1,
          "nested mirrored source reuses once with zero actual NR calls");
    Check(anchor_state == native::features.at(owner).slots[0].edit_reuse && anchor_state->reuse_count == 1
              && anchor_state->anchor_generation == generation && read_buffer(anchor_state->anchor.Get()) == wrapper_anchor,
          "one reuse retains the exact refresh anchor and generation");
    auto neutral_final = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    submit_commands("wrapper neutral readback");
    const auto neutral_bytes = neutral_final.Pixels();
    Check(std::memcmp(neutral_bytes.data(), source_values.data(), neutral_bytes.size()) == 0,
          "production neutral reuse returns exact current half-float source and alpha");
    Check(frame(0) == 1, "the second following base frame refreshes rather than reusing a second time");
    Check(frame(1) == 1 && frame(1) == 1, "successive explicit Reset frames both run NR");
    native::intensity = .75f;
    native::configuration_generation.fetch_add(1);
    const auto before_settings_reuses = native::edit_reuse_reuses.load();
    Check(frame(0) == 0 && native::edit_reuse_reuses.load() == before_settings_reuses,
          "committed parameter change recreates NR without reusing the old edit");
    Check(frame(0) == 1 && model_resets.back() == 1 && native::edit_reuse_reuses.load() == before_settings_reuses,
          "the recreated NR refreshes with Reset after its existing maturation frame");
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
      native::nr_adaptive_enabled = true;
      Check(frame(0) == 1 && native::edit_reuse_reuses.load() == reuses_before_adaptive
                && native::edit_reuse_fallbacks.load() == fallbacks_before_adaptive + 1,
            "requested adaptive On retains per-frame NR even when its current route cannot be admitted");
      const auto session = native::features.at(owner).adaptive_session;
      Check(session != nullptr && native::features.at(owner).adaptive_cache == nullptr
                && session->status.reason == addon::adaptive_resolution::Reason::PREPARING
                && native::features.at(owner).slots[0].edit_reuse == nullptr && old_anchor.expired(),
            "unavailable adaptive admission retires the completed old anchor without treating the request as Off");
      reservation.reset();
      native::adaptive_resident_session.reset();
      Check(native::EnsureLookPipeline(gpu.device.Get()), "warm existing Look programs before admitted adaptive recording");
      Check(frame(0) >= 1 && native::edit_reuse_reuses.load() == reuses_before_adaptive
                && native::features.at(owner).adaptive_cache != nullptr
                && native::features.at(owner).slots[0].edit_reuse == nullptr,
            "an admitted adaptive On frame evaluates the model without edit reuse");
      native::PollAdaptiveCache(native::features.at(owner).adaptive_cache.get());
      native::nr_adaptive_enabled = false;
      Check(frame(0) == 1
                && native::features.at(owner).adaptive_session == session
                && native::features.at(owner).adaptive_cache == nullptr
                && native::features.at(owner).slots[0].edit_reuse != nullptr
                && native::features.at(owner).slots[0].edit_reuse->valid,
            "adaptive Off retains its Session, retires standby slots and refreshes a new edit anchor");
      const auto fresh_anchor = native::features.at(owner).slots[0].edit_reuse;
      const auto fresh_generation = fresh_anchor->anchor_generation;
      Check(frame(0) == 0 && native::edit_reuse_reuses.load() == reuses_before_adaptive + 1
                && native::features.at(owner).slots[0].edit_reuse == fresh_anchor
                && fresh_anchor->anchor_generation == fresh_generation,
            "the retained disabled Session permits one reuse with zero model calls and no recursive recapture");
    }
    {
      Check(!native::edit_reuse_adaptive_experiment.load(), "two-tier edit reuse experiment defaults to disabled");
      const auto saved_levels = native::nr_adaptive_selected_levels.load();
      const auto saved_minimum = native::nr_adaptive_min_scale.load();
      native::nr_adaptive_selected_levels = 2;
      native::nr_adaptive_min_scale = .75f;
      native::edit_reuse_adaptive_experiment = true;
      native::nr_adaptive_enabled = true;
      native::direct_api.evaluate = ConstantModel;
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
      const auto before_preparing_reuses = native::edit_reuse_reuses.load();
      auto& feature = native::features.at(owner);
      for (uint32_t attempt = 0; attempt < 32; ++attempt) {
        const auto successes = native::SourceSuccessfulEvaluations();
        const auto active_before = active_calls;
        frame(0);
        Check(native::SourceSuccessfulEvaluations() - successes == active_calls - active_before,
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
      Check(native::edit_reuse_reuses.load() == before_preparing_reuses,
            "the bounded preparation interval never reuses an edit before both tiers are ready");
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
      {
        namespace timers = addon::gpu_timers;
        const auto session = feature.adaptive_session;
        const auto saved_stability = session->stable_after_ns;
        const auto saved_comparison_stability = native::comparison_stable_after_ns[1];
        const auto saved_measure = session->next_measure_ns;
        const auto saved_output = [] {
          std::unique_lock lock(addon::output_framerate::internal::state.mutex);
          return addon::output_framerate::internal::state.stream;
        }();
        Check(!native::gpu_timers_enabled.load() && !native::cost_meter_enabled.load(),
              "paired WARP recording is driven by adaptive sampling without profiling");
        timers::Poll(true, false);
        session->InvalidateSampling();
        session->stable_after_ns = native::comparison_stable_after_ns[1] = 0;
        session->next_measure_ns = 0;
        const auto timed_source = [&](uint32_t calls) {
          // With no Present provider, Prepare intentionally clears its samples
          // every frame. Supply the existing synthetic output-FPS contract.
          const auto now = native::SteadyNowNs();
          {
            std::unique_lock lock(addon::output_framerate::internal::state.mutex);
            auto& estimate = addon::output_framerate::internal::state.stream;
            estimate.previous_ns = now;
            estimate.snapshot = {.available = true, .source = addon::output_framerate::Source::PRESENT,
                .source_generation = 1, .configuration_generation = native::configuration_generation.load(),
                .window_serial = 1, .window_begin_ns = now - 500'000'000, .window_end_ns = now,
                .fps = 60., .fg_off = true};
          }
          const auto sequence = timers::internal::next_sequence;
          const auto successes = native::SourceSuccessfulEvaluations();
          Check(frame(0, false) == calls && native::SourceSuccessfulEvaluations() == successes + 1,
                "paired timer source retains the actual refresh/reuse calls");
          const auto found = std::find_if(std::begin(timers::internal::segments), std::end(timers::internal::segments),
              [&](const auto& segment) { return segment.recorded && segment.sequence == sequence + 1; });
          Check(found != std::end(timers::internal::segments) && found->measure && found->adaptive.paired
                    && found->adaptive.pair_first_frame != 0
                    && found->adaptive.application_frame == feature.sr_sequence,
                "production timer captures the selected true source ID and paired tag");
          const auto tag = found->adaptive;
          const auto measured_sequence = found->sequence;
          // Preserve the real completion receipt before Poll; Gpu::Submit's
          // unconditional prune intentionally does not preserve timer receipts.
          Check(SUCCEEDED(gpu.list->Close()), "paired recording close");
          ID3D12CommandList* lists[]{gpu.list.Get()};
          gpu.queue->ExecuteCommandLists(1, lists);
          gpu.submission_pending = true;
          Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "paired completion signal");
          HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
          Check(event != nullptr, "paired completion event");
          const auto armed = gpu.fence->SetEventOnCompletion(gpu.serial, event);
          const auto wait = (SUCCEEDED(armed) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
          CloseHandle(event);
          Check(wait == WAIT_OBJECT_0 && SUCCEEDED(gpu.device->GetDeviceRemovedReason()), "paired healthy bounded completion");
          gpu.submission_pending = false;
          Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
                "paired completed recording reset");
          native::DrainRetiredResources(true);
          timers::Poll(true, false);
          gpu.uploads.clear();
          gpu.retained_resources.clear();
          Check(timers::stage_stats[1].valid && timers::stage_stats[1].sequence == measured_sequence,
                "both real WARP chains publish complete production timestamps");
          return std::pair{tag, timers::stage_stats[1].total_us};
        };
        const auto first = timed_source(1);
        Check(session->gpu_count == 0 && session->pending_pair.frame == first.first.application_frame,
              "one completed production chain does not publish a two-source cost");
        session->next_measure_ns = native::SteadyNowNs() + 10'000'000'000;
        const auto second = timed_source(0);
        Check(second.first.application_frame == first.first.application_frame + 1
                  && second.first.pair_first_frame == first.first.pair_first_frame
                  && session->gpu_count == 1
                  && std::abs(session->gpu_us[0] - (static_cast<double>(first.second) + second.second) / 2.) < .001,
              "the adjacent reuse is measured before the next 250ms opportunity and publishes the complete chain mean");
        session->next_measure_ns = 0;
        const auto unmatched = timed_source(1);
        Check(session->pending_pair.frame == unmatched.first.application_frame,
              "a new first source waits for its adjacent completion");
        session->stable_after_ns = saved_stability;
        native::comparison_stable_after_ns[1] = saved_comparison_stability;
        session->next_measure_ns = saved_measure;
        {
          std::unique_lock lock(addon::output_framerate::internal::state.mutex);
          addon::output_framerate::internal::state.stream = saved_output;
        }
      }
      std::weak_ptr<reuse::State> retired_lower = low_anchor;
      low_anchor.reset();
      feature.adaptive_session->Request(0, native::SteadyNowNs());
      Check(feature.adaptive_session->pending_pair.pair_first_frame == 0
                && feature.adaptive_session->measure_pair_first_frame == 0,
            "the real tier request clears the unfinished old measurement pair");
      checked_frame(0, 1);
      Check(tiers->active == 0 && feature.slots[0].handle == upper_model && model_resets.back() == 1
                && retired_lower.expired() && tiers->tiers[1].state.slots[0].edit_reuse == nullptr,
            "returning to the upper ready tier resets history and cannot resurrect the old low-grid edit");
      checked_frame(0, 0);
      Check(native::nr_features_created == models && native::worksets_created == allocations,
            "upper/lower/upper reuse creates no additional model or adaptive workset");
#if defined(NR_NEXUS_DEV_CONTROL)
      {
        namespace xefg = addon::fg::xefg;
        namespace xi = xefg::internal;
        native::native_fg_public_off = false;
        native::native_fg_declaration = native::NativeFgDeclaration::kUnknown;
        checked_frame(0, 1);
        Check(feature.slots[0].edit_reuse == nullptr && !feature.adaptive_session->paired_sampling,
              "Unknown without a public Off receipt never admits two-tier reuse");
        // Only the receipt is MOCK here. Public Enable/link acceptance is
        // exercised by fg_xefg_tests; recording, shaders and retirement use WARP.
        const bool stopped = xi::stopped.exchange(false);
        const bool sdk_enabled = xi::sdk_detours_enabled.exchange(true);
        Check(xi::state.contexts.empty() && xi::state.bridge_swapchains.empty(), "isolated SDK receipt fixture");
        auto context = std::make_shared<xi::Context>();
        context->device = gpu.device;
        context->device_identity = renodx::utils::directx::NativeIdentity(gpu.device.Get());
        context->proxy_identity = 0xE012;
        context->bridge_context_ready = context->bridge_proxy_ready = context->bridge_off_observed = true;
        context->bridge_off_epoch = context->epoch;
        context->bridge_off_uses_source_link = true;
        context->bridge_source_swapchain = 0xE011;
        context->bridge_source_generation = 1;
        context->bridge_link_generation = context->bridge_off_link_generation = 1;
        context->bridge_link_source = context->bridge_off_source = {
            .handle = reinterpret_cast<uintptr_t>(owner), .generation = feature.source_generation,
            .device12 = reinterpret_cast<uintptr_t>(context->device_identity.Get())};
        auto source_chain = std::make_shared<xi::BridgeSourceSwapchain>();
        source_chain->owner = context->bridge_source_swapchain;
        source_chain->identity = 0xE013;
        source_chain->device12 = context->device_identity;
        source_chain->generation = context->bridge_source_generation;
        source_chain->hook_ready = true;
        source_chain->context = context;
        source_chain->proxy = context->proxy_identity;
        xi::state.contexts.emplace(reinterpret_cast<void*>(uintptr_t{0xE014}), context);
        xi::state.bridge_swapchains.emplace(source_chain->owner, source_chain);
        Check(native::EditReuseXefgOffLocked(owner).available,
              "a source-bound MOCK SDK Disable is available without changing the native declaration");
        Check(!native::EditReuseXefgOffLocked(owner, feature.source_generation + 1).available,
              "a current Disable cannot admit a recording from a different source generation");
        checked_frame(0, 1);
        Check(feature.adaptive_session->paired_sampling && !native::native_fg_public_off.load()
                  && native::native_fg_declaration.load() == native::NativeFgDeclaration::kUnknown,
              "DEV accepts the independent SDK receipt and preserves native Unknown");
        checked_frame(0, 0);
        const auto sampling = feature.adaptive_session->sampling_generation;
        ++context->bridge_link_generation;
        context->bridge_off_link_generation = context->bridge_link_generation;
        checked_frame(0, 1);
        Check(feature.adaptive_session->sampling_generation > sampling && model_resets.back() == 1,
              "a newly accepted source link invalidates paired measurements and old NR edit history");
        checked_frame(0, 0);
        ++context->epoch;
        checked_frame(0, 1);
        Check(feature.slots[0].edit_reuse == nullptr && !feature.adaptive_session->paired_sampling,
              "a stale Disable epoch stops reuse and paired sampling immediately");
        xefg::ForgetBridgeSourceSwapchain(source_chain->owner);
        xi::state.contexts.clear();
        xi::sdk_detours_enabled = sdk_enabled;
        xi::stopped = stopped;
        native::native_fg_public_off = true;
        native::native_fg_declaration = native::NativeFgDeclaration::kOff;
      }
#endif
      native::edit_reuse_adaptive_experiment = false;
      const auto reuses = native::edit_reuse_reuses.load();
      checked_frame(0, 1);
      checked_frame(0, 1);
      Check(feature.slots[0].edit_reuse == nullptr && native::edit_reuse_reuses.load() == reuses,
            "disabling the private experiment restores the unchanged per-frame adaptive admission");
      native::nr_adaptive_enabled = false;
      native::nr_adaptive_selected_levels = saved_levels;
      native::nr_adaptive_min_scale = saved_minimum;
      native::direct_api.evaluate = NeutralModel;
      parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.f);
      parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f);
      upload(wrapper_depth.Get(), std::vector<float>(16 * 16, .5f), NATIVE_INPUT_STATE, NATIVE_INPUT_STATE, 16 * 4);
      submit_commands("restore normal guide input");
      Check(frame(1) == 1, "leaving the two-tier experiment returns to the existing fixed-size refresh");
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
#if defined(NR_NEXUS_DEV_CONTROL)
    // The original single-output adaptive/discard/failure source is already released above.
    // Give the rotating-output On scenario a new source lifetime and its own ordinary warmup.
    native::nr_adaptive_enabled = false;
    native::nr_follow_input_res = false;
    native::nr_resolution_scale = 1.f;
    native::edit_reuse_adaptive_experiment = false;
    native::edit_reuse_fg_experiment = false;
    native::edit_reuse_enabled = true;
    native::native_fg_owns = false;
    native::native_fg_public_off = true;
    native::native_fg_declaration = native::NativeFgDeclaration::kOff;
    native::mixed_present_fg_seen = false;
    native::direct_api.evaluate = NeutralModel;
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
                  && first_on_cache->contract[32] == 1
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
        native::direct_api.evaluate = ConstantModel;
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
        native::direct_api.evaluate = NeutralModel;
        Check(frame(1) == 1, "restore the neutral model with an explicit source Reset");
      }

      native::RegisterCreatedFeatureLocked(second_fg, NVSDK_NGX_Feature_FrameGeneration, &parameters);
      Check(!native::ReadEditReuseFgEnvironmentLocked().available && frame(0) == 1
                && native::features.at(owner).slots[0].edit_reuse == nullptr,
          "multiple native FG objects cannot lend one environment to an old edit anchor");
      native::ForgetSourceHandleLocked(second_fg);
      Check(frame(1) == 1 && frame(0) == 0, "a unique native environment restarts from a fresh anchor");
      const auto before_recreate = native::ReadEditReuseFgEnvironmentLocked().generation;
      native::RegisterCreatedFeatureLocked(fg_handle, NVSDK_NGX_Feature_FrameGeneration, &parameters);
      const auto reuses_before_recreate = native::edit_reuse_reuses.load();
      Check(native::ReadEditReuseFgEnvironmentLocked().generation != before_recreate
                && frame(0) == 1 && model_resets.back() == 1
                && native::edit_reuse_reuses.load() == reuses_before_recreate,
          "recreating the same FG handle address changes environment generation and resets x2 history");

      native::RegisterCreatedFeatureLocked(second_source, native::kFeatureDlss, &parameters);
      native::create_contracts.at(second_source).source_device12_identity =
          native::create_contracts.at(owner).source_device12_identity;
      Check(frame(0) == 1 && native::features.at(owner).slots[0].edit_reuse == nullptr,
          "DEV On rejects two observed SR sources on the same native device");
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
      Check(frame(0) == 1 && model_resets.back() == 1 && native::features.at(owner).slots[0].edit_reuse == nullptr,
          "leaving the DEV On experiment retires its cache without changing the public FG declaration");
      native::edit_reuse_fg_experiment = true;
      Check(frame(0) == 1 && native::features.at(owner).slots[0].edit_reuse != before_experiment_exit
                && native::features.at(owner).slots[0].edit_reuse->valid,
          "re-entering DEV On refreshes a new anchor after the intervening normal NR frame");
      const auto reset_before_off = native::history_reset_epoch.load();
      native::OnNativeFgOptions(false);
      Check(native::history_reset_epoch.load() == reset_before_off && frame(0) == 1 && model_resets.back() == 1
                && native::features.at(owner).slots[0].edit_reuse->contract[32] == 0,
          "On-to-Off changes cache identity and resets even when Upscaled owns no FG service or global reset");
      native::OnNativeFgOptions(true);
      native::mixed_present_fg_seen = true;
      Check(frame(1) == 1, "native On recovers its own anchor");

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
                && native::created_feature_ids.at(fg_handle) == NVSDK_NGX_Feature_FrameGeneration
                && !xefg::OwnsStream() && !xefg::QueryBridgeOff({}).available,
          "unique enabled XeFG takes environment precedence over OptiScaler-like DLSSG input without granting Off or ownership");
      Check(frame(0) == 1 && model_resets.back() == 1
                && native::features.at(owner).slots[0].edit_reuse->contract[32] == 2 && frame(0) == 0,
          "switching native-input to XeFG environment refreshes, then reuses only its own anchor");
      const auto before_resize = native::ReadEditReuseFgEnvironmentLocked().generation;
      {
        std::unique_lock lock(xi::state.mutex);
        xi::ReleaseFrames(context.get());
      }
      Check(native::ReadEditReuseFgEnvironmentLocked().generation != before_resize
                && frame(0) == 1 && model_resets.back() == 1,
          "XeFG frame-lifetime invalidation changes environment generation and forces a fresh x2 history");
      xi::state.contexts.emplace(reinterpret_cast<void*>(uintptr_t{0xED192}), make_context());
      xi::RefreshOwnership();
      Check(!native::ReadEditReuseFgEnvironmentLocked().available && frame(0) == 1
                && native::features.at(owner).slots[0].edit_reuse == nullptr,
          "multiple XeFG contexts decline instead of borrowing the native input environment");
      xi::state.contexts.erase(reinterpret_cast<void*>(uintptr_t{0xED192}));
      Check(frame(1) == 1, "unique XeFG context restarts with a refresh");
      const auto before_replace = native::ReadEditReuseFgEnvironmentLocked();
      context = make_context();
      xi::state.contexts[sdk_handle] = context;
      Check(native::ReadEditReuseFgEnvironmentLocked().generation != before_replace.generation
                && frame(0) == 1 && model_resets.back() == 1,
          "replacement at the same opaque SDK handle cannot inherit the prior environment or anchor");
      xi::state.contexts.clear();
      xi::owns_stream = saved_enabled;
      xi::sdk_detours_enabled = saved_detours;
      xi::stopped = saved_stopped;
      native::edit_reuse_fg_experiment = false;
      native::OnNativeFgOptions(false);
      native::ForgetSourceHandleLocked(fg_handle);
      for (uint32_t prefix = 0; prefix < 4; ++prefix) frame(1);
      std::cout << "PASS DEV On environment + WARP SR x2: unique source, native/XeFG metadata, precedence, ownership rejection, lifetime/switch Reset and exact neutral output; actual FG handoff=NotRun\n";
    }
    native::ForgetSourceHandleLocked(owner);
    native::DrainRetiredResources(true);
    Check(!native::features.contains(owner) && !native::created_feature_ids.contains(owner)
              && std::none_of(native::worksets.begin(), native::worksets.end(),
                  [&](const auto& entry) { return entry.first.handle == owner; }),
        "the separate On source lifetime releases its rotating-output worksets through normal retirement");
#endif
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
