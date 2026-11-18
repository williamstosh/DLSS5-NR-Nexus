/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <vector>

#include <embed/shaders.h>

#include "../../utils/directx.hpp"
#include "submission_tracker.hpp"

namespace renodx::addons::dlss5::edit_reuse {

enum class Phase : uint32_t {
  CAPTURE = 0,
  APPLY = 1,
  MOTION = 2,
  SNAPSHOT_MOTION = 3,
  SELECT_ORIGINAL = 4,
  PREPARE = 5,
  APPLY_AND_SNAPSHOT_MOTION = 6,
};

struct Inputs {
  // Proxy/neural/anchor/Validity working extent.
  uint32_t width = 0;
  uint32_t height = 0;
  // Original/decoded extent. Zero retains the working extent.
  uint32_t output_width = 0;
  uint32_t output_height = 0;
  uint32_t motion_width = 0;
  uint32_t motion_height = 0;
  float motion_scale_x = 1.f;
  float motion_scale_y = 1.f;
  // Guide content's raster displacement from the unjittered image grid,
  // in render pixels. Motion values themselves exclude jitter.
  float raster_offset_x = 0.f;
  float raster_offset_y = 0.f;
  float previous_motion_offset_x = 0.f;
  float previous_motion_offset_y = 0.f;
  // Pre-SR color shares the guides' jittered raster. In that case the
  // anchor lookup also needs the displacement between the two color grids.
  bool color_uses_guide_raster = false;
  float anchor_raster_delta_x = 0.f;
  float anchor_raster_delta_y = 0.f;
  ID3D12Resource* proxy = nullptr;
  ID3D12Resource* neural = nullptr;
  ID3D12Resource* motion = nullptr;
  ID3D12Resource* depth = nullptr;
  ID3D12Resource* original = nullptr;
  // An ordinary Present backbuffer is borrowed for ordered work on its
  // presentation queue. ResizeBuffers requires all addon COM refs released.
  uint64_t borrowed_original_epoch = 0;
  ID3D12Resource* decoded = nullptr;
  DXGI_FORMAT depth_view_format = DXGI_FORMAT_R32_FLOAT;
};

struct Binding {
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
  std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 6> resources;
  ID3D12Resource* borrowed_original = nullptr;  // Identity only.
  uint64_t borrowed_original_epoch = 0;
};

// One immutable geometry and one x2 anchor per source Feature/Stage. The
// submission tracker guards the State marker until every recording is retired.
struct State {
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
  Microsoft::WRL::ComPtr<ID3D12Resource> anchor;
  Microsoft::WRL::ComPtr<ID3D12Resource> previous_motion;
  Microsoft::WRL::ComPtr<ID3D12Resource> validity;
  Microsoft::WRL::ComPtr<ID3D12Resource> composed_motion;
  std::vector<Binding> bindings;
  std::array<uint64_t, 40> contract{};
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t output_width = 0;
  uint32_t output_height = 0;
  uint32_t motion_width = 0;
  uint32_t motion_height = 0;
  uint64_t anchor_generation = 0;
  uint64_t source_sequence = 0;
  uint64_t reuse_count = 0;
  uint64_t allocated_bytes = 0;
  float motion_offset_x = 0.f;
  float motion_offset_y = 0.f;
  float anchor_raster_offset_x = 0.f;
  float anchor_raster_offset_y = 0.f;
  bool valid = false;
  bool reused = false;
};

// Inputs' guide textures are shader-readable, anchor buffers stay UAV, and
// composed_motion rests in UAV between model calls. CAPTURE reads shader-
// readable neural; APPLY writes UAV neural; SELECT_ORIGINAL writes UAV decoded
// and reads the still-unmodified game output in original. Other targets are
// not accessed by that phase. validity contains one uint (0/1) per working pixel.
// All passes use the same descriptor contract and root-UAV buffers. Each
// texture tuple has five immutable phase slices. Inactive views are typed null
// so one PSO's static resource declarations never bind conflicting SRV/UAV
// states. A full in-flight heap ring declines reuse.
inline bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                   State* state, const Inputs& inputs, Phase phase) {
  if (device == nullptr || list == nullptr || state == nullptr
      || inputs.width == 0 || inputs.height == 0 || inputs.motion_width == 0 || inputs.motion_height == 0) return false;
  const uint32_t output_width = (inputs.output_width != 0 ? inputs.output_width : inputs.width);
  const uint32_t output_height = (inputs.output_height != 0 ? inputs.output_height : inputs.height);
  if (state->root == nullptr) {
    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    ranges[0] = {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV, .NumDescriptors = 5};
    ranges[1] = {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV, .NumDescriptors = 1};
    ranges[2] = {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV, .NumDescriptors = 1, .BaseShaderRegister = 4};
    D3D12_ROOT_PARAMETER parameters[7]{};
    for (uint32_t table = 0; table < 3; ++table) {
      parameters[table].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameters[table].DescriptorTable = {1, &ranges[table]};
    }
    for (uint32_t buffer = 0; buffer < 3; ++buffer) {
      parameters[3 + buffer].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
      parameters[3 + buffer].Descriptor.ShaderRegister = 1 + buffer;
    }
    parameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[6].Constants = {0, 0, 16};
    const D3D12_ROOT_SIGNATURE_DESC desc{.NumParameters = 7, .pParameters = parameters};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(renodx::utils::directx::pD3D12SerializeRootSignature(
            &desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors))
        || FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&state->root)))) return false;
    const D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{
        .pRootSignature = state->root.Get(), .CS = {__edit_reuse.data(), __edit_reuse.size()}};
    if (FAILED(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&state->pipeline)))) return false;
  }
  if (state->pipeline == nullptr) return false;
  if (state->anchor == nullptr) {
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    const auto buffer = [&](uint64_t bytes, Microsoft::WRL::ComPtr<ID3D12Resource>* resource) {
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = bytes,
          .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = {1, 0},
          .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR, .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
      const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc);
      if (allocation.SizeInBytes == UINT64_MAX
          || FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(resource->ReleaseAndGetAddressOf())))) return false;
      state->allocated_bytes += allocation.SizeInBytes;
      return true;
    };
    if (!buffer(static_cast<uint64_t>(inputs.width) * inputs.height * 32, &state->anchor)
        || !buffer(static_cast<uint64_t>(inputs.motion_width) * inputs.motion_height * 8, &state->previous_motion)
        || !buffer(static_cast<uint64_t>(inputs.width) * inputs.height * 4, &state->validity)) return false;
    const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = inputs.motion_width, .Height = inputs.motion_height, .DepthOrArraySize = 1, .MipLevels = 1,
        .Format = DXGI_FORMAT_R32G32_FLOAT, .SampleDesc = {1, 0},
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc);
    if (allocation.SizeInBytes == UINT64_MAX
        || FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&state->composed_motion)))) return false;
    state->allocated_bytes += allocation.SizeInBytes;
    state->width = inputs.width;
    state->height = inputs.height;
    state->output_width = output_width;
    state->output_height = output_height;
    state->motion_width = inputs.motion_width;
    state->motion_height = inputs.motion_height;
  }
  if (state->previous_motion == nullptr || state->validity == nullptr || state->composed_motion == nullptr
      || state->width != inputs.width || state->height != inputs.height
      || state->output_width != output_width || state->output_height != output_height
      || state->motion_width != inputs.motion_width || state->motion_height != inputs.motion_height) return false;
  const std::array<ID3D12Resource*, 6> resources{
      inputs.proxy, inputs.neural, inputs.motion, inputs.depth, inputs.original, inputs.decoded};
  constexpr size_t kOriginal = 4;
  const bool borrow_original = inputs.borrowed_original_epoch != 0;
  for (const auto* resource : resources) if (resource == nullptr) return false;
  Binding* binding = nullptr;
  for (auto& entry : state->bindings) {
    bool same = entry.borrowed_original_epoch == inputs.borrowed_original_epoch;
    for (size_t index = 0; index < resources.size(); ++index) {
      const auto* previous = (borrow_original && index == kOriginal ? entry.borrowed_original : entry.resources[index].Get());
      same &= previous == resources[index];
    }
    if (same) {
      binding = &entry;
      break;
    }
  }
  const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  if (binding == nullptr) {
    for (auto& entry : state->bindings) {
      if (submission::ResourceReleasable(entry.heap.Get())) {
        binding = &entry;
        break;
      }
    }
    if (binding == nullptr) {
      if (state->bindings.size() == 8) return false;
      state->bindings.emplace_back();
      binding = &state->bindings.back();
    }
    if (binding->heap == nullptr) {
      const D3D12_DESCRIPTOR_HEAP_DESC desc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
          .NumDescriptors = 5 * 7, .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
      if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&binding->heap)))) return false;
    }
    binding->borrowed_original = (borrow_original ? inputs.original : nullptr);
    binding->borrowed_original_epoch = inputs.borrowed_original_epoch;
    for (size_t index = 0; index < resources.size(); ++index) {
      binding->resources[index] = (borrow_original && index == kOriginal ? nullptr : resources[index]);
    }
    auto cpu = binding->heap->GetCPUDescriptorHandleForHeapStart();
    // t0 proxy, t1 neural, t2 motion, t3 depth, t4 original.
    constexpr std::array<uint32_t, 5> active_srvs{
        (1u << 0) | (1u << 1) | (1u << 3),  // CAPTURE
        (1u << 0) | (1u << 2) | (1u << 3),  // APPLY
        1u << 2,                           // MOTION
        1u << 2,                           // SNAPSHOT_MOTION
        1u << 4};                          // SELECT_ORIGINAL
    for (uint32_t phase_index = 0; phase_index < active_srvs.size(); ++phase_index) {
      for (uint32_t index = 0; index < 5; ++index) {
        const auto resource_desc = resources[index]->GetDesc();
        const D3D12_SHADER_RESOURCE_VIEW_DESC view{
            .Format = (index == 3 ? inputs.depth_view_format : resource_desc.Format),
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Texture2D = {.MipLevels = 1}};
        device->CreateShaderResourceView((active_srvs[phase_index] & (1u << index)) != 0 ? resources[index] : nullptr,
                                         &view, cpu);
        cpu.ptr += stride;
      }
      const auto binding_phase = static_cast<Phase>(phase_index);
      auto* target = (binding_phase == Phase::SELECT_ORIGINAL ? inputs.decoded : inputs.neural);
      const D3D12_UNORDERED_ACCESS_VIEW_DESC target_view{.Format = target->GetDesc().Format,
          .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D};
      device->CreateUnorderedAccessView(
          binding_phase == Phase::APPLY || binding_phase == Phase::SELECT_ORIGINAL ? target : nullptr,
          nullptr, &target_view, cpu);
      cpu.ptr += stride;
      const D3D12_UNORDERED_ACCESS_VIEW_DESC motion_view{.Format = state->composed_motion->GetDesc().Format,
          .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D};
      device->CreateUnorderedAccessView(binding_phase == Phase::MOTION ? state->composed_motion.Get() : nullptr,
                                       nullptr, &motion_view, cpu);
      cpu.ptr += stride;
    }
  }
  submission::TrackUse(list, state);
  submission::TrackUse(list, binding->heap.Get());
  if (phase == Phase::PREPARE) return true;
  const D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV};
  list->ResourceBarrier(1, &barrier);
  ID3D12DescriptorHeap* heaps[]{binding->heap.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(state->root.Get());
  list->SetPipelineState(state->pipeline.Get());
  auto gpu = binding->heap->GetGPUDescriptorHandleForHeapStart();
  const bool apply_and_snapshot = phase == Phase::APPLY_AND_SNAPSHOT_MOTION;
  const auto dispatch_phase = apply_and_snapshot ? Phase::APPLY : phase;
  gpu.ptr += static_cast<UINT64>(dispatch_phase) * 7ull * stride;
  list->SetComputeRootDescriptorTable(0, gpu);
  list->SetComputeRootDescriptorTable(1, {gpu.ptr + 5ull * stride});
  list->SetComputeRootDescriptorTable(2, {gpu.ptr + 6ull * stride});
  list->SetComputeRootUnorderedAccessView(3, state->anchor->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(4, state->previous_motion->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(5, state->validity->GetGPUVirtualAddress());
  const std::array<uint32_t, 16> constants{inputs.width, inputs.height, inputs.motion_width, inputs.motion_height,
      std::bit_cast<uint32_t>(inputs.motion_scale_x), std::bit_cast<uint32_t>(inputs.motion_scale_y),
      static_cast<uint32_t>(phase), inputs.color_uses_guide_raster ? 1u : 0u,
      std::bit_cast<uint32_t>(inputs.raster_offset_x), std::bit_cast<uint32_t>(inputs.raster_offset_y),
      std::bit_cast<uint32_t>(inputs.previous_motion_offset_x), std::bit_cast<uint32_t>(inputs.previous_motion_offset_y),
      output_width, output_height,
      std::bit_cast<uint32_t>(inputs.anchor_raster_delta_x), std::bit_cast<uint32_t>(inputs.anchor_raster_delta_y)};
  list->SetComputeRoot32BitConstants(6, 16, constants.data(), 0);
  const bool motion_phase = phase == Phase::MOTION || phase == Phase::SNAPSHOT_MOTION;
  const uint32_t dispatch_width = motion_phase ? inputs.motion_width
      : (phase == Phase::SELECT_ORIGINAL ? output_width : inputs.width);
  const uint32_t dispatch_height = motion_phase ? inputs.motion_height
      : (phase == Phase::SELECT_ORIGINAL ? output_height : inputs.height);
  const uint32_t covered_width = apply_and_snapshot ? std::max(dispatch_width, inputs.motion_width) : dispatch_width;
  const uint32_t covered_height = apply_and_snapshot ? std::max(dispatch_height, inputs.motion_height) : dispatch_height;
  list->Dispatch((covered_width + 7) / 8, (covered_height + 7) / 8, 1);
  list->ResourceBarrier(1, &barrier);
  return true;
}

}  // namespace renodx::addons::dlss5::edit_reuse
