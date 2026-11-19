/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#if defined(NR_NEXUS_DEV_CONTROL)

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>

#include <embed/shaders.h>

#include "../../utils/directx.hpp"
#include "submission_tracker.hpp"

namespace renodx::addons::dlss5::fg_output_marker {

enum class Result : uint8_t { RECORDED, NOT_SUPPORTED, FAILED };

inline constexpr uint32_t kMarkerWidth = 128;
inline constexpr uint32_t kMarkerHeight = 64;
inline constexpr uint32_t kCellSize = 16;
inline constexpr uint32_t kPatternBits = 0x19D64EB3u;

struct Region {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = kMarkerWidth;
  uint32_t height = kMarkerHeight;
};

inline constexpr Region RegionFor(uint32_t width, uint32_t height) {
  return {.x = width / 4, .y = height / 3};
}

// Diagnostic only, after Resolve/SELECT_ORIGINAL and before copyback. One
// workset owns this stable State and one immutable full-target UAV binding.
// It never accesses or changes the x2 anchor. The owner retires the State and
// Objects() together using their actual submission/recording proofs.
struct State {
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
  Microsoft::WRL::ComPtr<ID3D12Resource> target;
  uint32_t width = 0;
  uint32_t height = 0;
  bool attempted = false;
  Result preparation = Result::FAILED;

  std::array<IUnknown*, 4> Objects() const {
    return {root.Get(), pipeline.Get(), heap.Get(), target.Get()};
  }
};

// Caller supplies the same workset/device and a complete target already in
// UAV state; this function neither submits nor infers a resource state. Only
// scRGB RGBA16F and final PQ RGB10A2 are supported. Alpha retains the original
// UAV-loaded value; the caller has already established the output encoding.
inline Result Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, State* state,
                     ID3D12Resource* target, uint32_t width, uint32_t height,
                     uint8_t hdr_mode, bool reused) {
  if (device == nullptr || list == nullptr || state == nullptr || target == nullptr) return Result::FAILED;
  const auto region = RegionFor(width, height);
  const auto desc = target->GetDesc();
  const bool supported_format = (hdr_mode == 1 && desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
      || (hdr_mode == 2 && desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
  if (!supported_format || (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT
          && list->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE)
      || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
      || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1
      || desc.Width != width || desc.Height != height
      || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0
      || region.x > width || region.width > width - region.x
      || region.y > height || region.height > height - region.y) return Result::NOT_SUPPORTED;
  if (state->attempted && (state->target.Get() != target || state->width != width || state->height != height)) {
    return Result::NOT_SUPPORTED;
  }
  if (!state->attempted) {
    // Latch the attempt before capability queries or creation. Unsupported
    // capabilities and failed creation never cause per-frame PSO retries.
    state->attempted = true;
    state->target = target;
    state->width = width;
    state->height = height;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{.Format = desc.Format};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))) {
      return Result::FAILED;
    }
    constexpr auto required1 = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
    constexpr auto required2 = D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
    if ((support.Support1 & required1) != required1 || (support.Support2 & required2) != required2) {
      state->preparation = Result::NOT_SUPPORTED;
      return state->preparation;
    }
    if (renodx::utils::directx::pD3D12SerializeRootSignature == nullptr) return Result::FAILED;
    const D3D12_DESCRIPTOR_RANGE range{.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV, .NumDescriptors = 1};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &range};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {0, 0, 5};
    const D3D12_ROOT_SIGNATURE_DESC signature{.NumParameters = 2, .pParameters = parameters};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(renodx::utils::directx::pD3D12SerializeRootSignature(
            &signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors))
        || FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&state->root)))) return Result::FAILED;
    const D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{
        .pRootSignature = state->root.Get(), .CS = {__fg_output_marker.data(), __fg_output_marker.size()}};
    if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&state->pipeline)))) return Result::FAILED;
    const D3D12_DESCRIPTOR_HEAP_DESC heap{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = 1, .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&state->heap)))) return Result::FAILED;
    const D3D12_UNORDERED_ACCESS_VIEW_DESC view{.Format = desc.Format,
        .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D};
    device->CreateUnorderedAccessView(target, nullptr, &view, state->heap->GetCPUDescriptorHandleForHeapStart());
    state->preparation = Result::RECORDED;
  }
  if (state->preparation != Result::RECORDED) return state->preparation;
  try {
    // Register newly created objects before the first GPU reference, rather
    // than relying on an earlier workset collection that could not see them.
    submission::TrackUse(list, state);
    for (auto* object : state->Objects()) submission::TrackUse(list, object);
  } catch (...) {
    return Result::FAILED;
  }
  D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV};
  barrier.UAV.pResource = target;
  list->ResourceBarrier(1, &barrier);
  ID3D12DescriptorHeap* heaps[]{state->heap.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(state->root.Get());
  list->SetPipelineState(state->pipeline.Get());
  list->SetComputeRootDescriptorTable(0, state->heap->GetGPUDescriptorHandleForHeapStart());
  const std::array<uint32_t, 5> constants{region.x, region.y, reused ? 1u : 0u, kPatternBits, hdr_mode};
  list->SetComputeRoot32BitConstants(1, static_cast<UINT>(constants.size()), constants.data(), 0);
  list->Dispatch(kMarkerWidth / 8, kMarkerHeight / 8, 1);
  list->ResourceBarrier(1, &barrier);
  return Result::RECORDED;
}

}  // namespace renodx::addons::dlss5::fg_output_marker

#endif
