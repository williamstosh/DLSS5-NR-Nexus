/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)

#include <d3d12sdklayers.h>
#include <DirectXPackedVector.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include <nr_fg_output_marker.hpp>

#include "d3d12_test_gpu.hpp"

// Include beside nr_edit_reuse_gpu_cases.hpp in the existing native fixture.
// This dedicated Run uses that fixture's Gpu/transfer/submission helpers only.
namespace nr_fg_output_marker_gpu_cases {
namespace marker = renodx::addons::dlss5::fg_output_marker;
namespace submission = renodx::addons::dlss5::submission;
using d3d12_test::Check;
using d3d12_test::Gpu;
using Microsoft::WRL::ComPtr;

inline int Run() {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    Check(renodx::utils::directx::Initialize(), "initialize marker DirectX entry points");
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu(false);
    ComPtr<ID3D12InfoQueue> info;
    gpu.device.As(&info);
    constexpr uint32_t width = 256, height = 192;
    const auto texture = [&](DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = width, .Height = height, .DepthOrArraySize = 1, .MipLevels = 1,
          .Format = format, .SampleDesc = {1, 0}, .Flags = flags};
      ComPtr<ID3D12Resource> result;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result))), "marker test texture");
      return result;
    };
    auto target = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto transfer = gpu.CreateTextureTransfer(target.Get());
    std::vector<uint16_t> original(static_cast<size_t>(width) * height * 4);
    for (uint32_t pixel = 0; pixel < width * height; ++pixel) {
      const std::array<float, 4> values{
          .25f + .03125f * (pixel % 8), 5.5f, -.5f, .375f + .03125f * (pixel % 4)};
      for (size_t channel = 0; channel < values.size(); ++channel) {
        original[pixel * 4 + channel] = DirectX::PackedVector::XMConvertFloatToHalf(values[channel]);
      }
    }
    gpu.Transition(target.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu.UploadTexture(target.Get(), transfer, reinterpret_cast<const uint8_t*>(original.data()),
        original.size() * sizeof(uint16_t), width * 8);
    gpu.Transition(target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    marker::State state;
    const auto region = marker::RegionFor(width, height);
    Check(region.x == 64 && region.y == 64 && region.width == 128 && region.height == 64,
        "marker ROI follows the documented full-target placement");
    std::array<IUnknown*, 4> objects{};
    std::vector<uint8_t> refresh;
    for (uint32_t phase = 0; phase < 2; ++phase) {
      Check(marker::Record(gpu.device.Get(), gpu.list.Get(), &state, target.Get(), width, height, 1, phase != 0)
              == marker::Result::RECORDED,
          "record a linear FP16 marker phase");
      if (phase == 0) objects = state.Objects();
      Check(state.Objects() == objects && state.target.Get() == target.Get(),
          "both phases reuse immutable PSO/root/heap and retain the target");
      Check(!submission::ResourceReleasable(&state), "first marker recording tracks its stable owner");
      for (auto* object : objects) {
        Check(object != nullptr && !submission::ResourceReleasable(object),
            "first marker recording tracks every newly prepared GPU object");
      }
      gpu.Transition(target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      gpu.RecordTextureReadback(target.Get(), transfer);
      gpu.Transition(target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      gpu.Submit();
      const auto bytes = gpu.ReadTexture(transfer);
      Check(bytes.size() == original.size() * sizeof(uint16_t), "marker readback retains the full FP16 target");
      // Golden cell rows, LSB at the left. Last two cells are phase complements.
      const std::array<uint8_t, 4> rows{0xB3, 0x4E, 0xD6, static_cast<uint8_t>(phase == 0 ? 0x99 : 0x59)};
      for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          const auto pixel = static_cast<size_t>(y) * width + x;
          const auto* actual = bytes.data() + pixel * 8;
          const auto* expected = original.data() + pixel * 4;
          const bool inside = x >= region.x && x < region.x + region.width
              && y >= region.y && y < region.y + region.height;
          if (!inside) {
            Check(std::memcmp(actual, expected, 8) == 0, "marker leaves ROI-exterior RGBA byte-identical");
            continue;
          }
          std::array<uint16_t, 4> value{};
          std::memcpy(value.data(), actual, 8);
          Check(value[3] == expected[3], "marker preserves each nonzero original FP16 alpha");
          const auto column = (x - region.x) / 16;
          const auto row = (y - region.y) / 16;
          const bool high = ((rows[row] >> column) & 1u) != 0;
          const auto expected_rgb = DirectX::PackedVector::XMConvertFloatToHalf(high ? 4.f : .125f);
          Check(value[0] == expected_rgb && value[1] == expected_rgb && value[2] == expected_rgb,
              "marker writes the fixed binary geometry and complementary phase cells");
        }
      }
      if (phase == 0) refresh = bytes;
      else Check(bytes != refresh, "refresh and reuse markers are distinguishable in actual GPU pixels");
      Check(submission::ResourceReleasable(&state), "completed and retired marker recording releases its owner");
      for (auto* object : objects) {
        Check(submission::ResourceReleasable(object), "completed and retired marker recording releases GPU objects");
      }
    }
    marker::State rejected;
    auto other_format = texture(DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto no_uav = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    Check(marker::Record(gpu.device.Get(), gpu.list.Get(), &rejected, other_format.Get(), width, height, 1, false)
            == marker::Result::NOT_SUPPORTED,
        "marker rejects an otherwise valid float32 UAV instead of expanding format support");
    Check(marker::Record(gpu.device.Get(), gpu.list.Get(), &rejected, no_uav.Get(), width, height, 1, false)
            == marker::Result::NOT_SUPPORTED,
        "marker rejects FP16 resources without UAV capability");
    Check(marker::Record(gpu.device.Get(), gpu.list.Get(), &rejected, target.Get(), width, height, 2, false)
            == marker::Result::NOT_SUPPORTED,
        "marker rejects PQ instead of changing the encoding proxy");
    Check(marker::Record(gpu.device.Get(), gpu.list.Get(), &rejected, target.Get(), width - 1, height, 1, false)
            == marker::Result::NOT_SUPPORTED && !rejected.attempted,
        "marker requires the complete target extent and rejects before creating GPU state");
    if (info != nullptr) {
      for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T size = 0;
        info->GetMessage(index, nullptr, &size);
        std::vector<uint8_t> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(index, message, &size)), "marker debug message");
        Check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
      }
    }
    std::cout << "PASS FG output marker evidence=WARP pixels; no FG runtime or game handoff exercised\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL FG output marker: " << error.what() << '\n';
    return 1;
  }
}

}  // namespace nr_fg_output_marker_gpu_cases

#endif
