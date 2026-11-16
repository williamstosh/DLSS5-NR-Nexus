/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <DirectXPackedVector.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "d3d12_test_gpu.hpp"
#include "look_stage.hpp"

#if !defined(NR_NEXUS_TEST_WARP_ONLY)
#error This regression must be compiled with NR_NEXUS_TEST_WARP_ONLY.
#endif

namespace {
using d3d12_test::Check;
using d3d12_test::ComPtr;
using DirectX::PackedVector::XMConvertFloatToHalf;
using DirectX::PackedVector::XMConvertHalfToFloat;
using nlohmann::json;
using Pixel = std::array<float, 4>;
using Pair = std::array<Pixel, 2>;
namespace look = renodx::addons::dlss5::look;

constexpr float kRatioFloor = 1.f / 1024.f;
constexpr float kTolerance = 0.002f;
constexpr uint64_t kMaxPixels = 4096ull * 2160;

float Decode(float value) {
  value = std::clamp(value, 0.f, 1.f);
  return (value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f));
}

float Encode(float value) {
  value = std::clamp(value, 0.f, 1.f);
  return (value <= 0.0031308f ? 12.92f * value : 1.055f * std::pow(value, 1.f / 2.4f) - 0.055f);
}

float Luminance(const Pixel& value) {
  return value[0] * 0.212639f + value[1] * 0.715169f + value[2] * 0.072192f;
}

Pixel Quantize(Pixel value, DXGI_FORMAT format) {
  Check(format == DXGI_FORMAT_R16G16B16A16_UNORM || format == DXGI_FORMAT_R16G16B16A16_FLOAT,
        "supported RGBA16 input format");
  for (auto& channel : value) {
    Check(std::isfinite(channel), "finite fixture channel");
    if (format == DXGI_FORMAT_R16G16B16A16_UNORM) {
      channel = static_cast<float>(std::lround(std::clamp(channel, 0.f, 1.f) * 65535.f)) / 65535.f;
    } else {
      channel = XMConvertHalfToFloat(XMConvertFloatToHalf(channel));
      Check(std::isfinite(channel), "finite RGBA16_FLOAT input");
    }
  }
  return value;
}

struct Surface {
  UINT width = 0;
  UINT height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  std::vector<Pixel> pixels;

  const Pixel& At(int x, int y) const {
    return pixels[static_cast<size_t>(std::clamp(y, 0, static_cast<int>(height) - 1)) * width
                  + std::clamp(x, 0, static_cast<int>(width) - 1)];
  }
};

struct Region {
  UINT x = 0;
  UINT y = 0;
  UINT width = 0;
  UINT height = 0;
};

struct Scene {
  std::string id;
  bool reference_linear = false;
  Surface reference;
  Surface proxy;
  Surface neural;
  Region roi;
};

Surface Filled(UINT width, UINT height, DXGI_FORMAT format, Pixel value) {
  Check(width > 0 && height > 0 && width <= 4096 && height <= 2160
            && static_cast<uint64_t>(width) * height <= kMaxPixels,
        "bounded full texture dimensions");
  return {.width = width, .height = height, .format = format,
          .pixels = std::vector<Pixel>(static_cast<size_t>(width) * height, Quantize(value, format))};
}

// A scalar oracle uses global clamped taps, independent of the production groupshared tile.
Pair ReferencePixel(const Scene& scene, UINT x, UINT y, bool legacy) {
  const float px = (static_cast<float>(x) + 0.5f) * scene.proxy.width / scene.reference.width - 0.5f;
  const float py = (static_cast<float>(y) + 0.5f) * scene.proxy.height / scene.reference.height - 0.5f;
  const int bx = static_cast<int>(std::floor(px));
  const int by = static_cast<int>(std::floor(py));
  Pixel original = scene.reference.At(static_cast<int>(x), static_cast<int>(y));
  if (!scene.reference_linear) {
    for (size_t channel = 0; channel < 3; ++channel) original[channel] = Decode(original[channel]);
  }
  const float guide = std::log2(std::max(Luminance(original), 1e-8f));
  Pixel joint{};
  Pixel spatial{};
  float joint_weight = 0.f;
  float spatial_weight = 0.f;
  for (int dy = -1; dy <= 2; ++dy) {
    for (int dx = -1; dx <= 2; ++dx) {
      Pixel proxy = scene.proxy.At(bx + dx, by + dy);
      const auto& neural = scene.neural.At(bx + dx, by + dy);
      for (size_t channel = 0; channel < 3; ++channel) proxy[channel] = Decode(proxy[channel]);
      const float tx = static_cast<float>(bx + dx) - px;
      const float ty = static_cast<float>(by + dy) - py;
      const float ws = std::exp(-(tx * tx + ty * ty) / 0.5f);
      const float low_guide = (legacy || scene.reference_linear
                                  ? neural[3]
                                  : std::log2(std::max(Luminance(proxy), 1e-8f)));
      const float difference = guide - low_guide;
      const float wr = std::exp(-(difference * difference) / 0.5f);
      for (size_t channel = 0; channel < 3; ++channel) {
        const float edit = std::log2(std::max(Decode(neural[channel]), kRatioFloor))
                           - std::log2(std::max(proxy[channel], kRatioFloor));
        joint[channel] += ws * wr * edit;
        spatial[channel] += ws * edit;
      }
      joint_weight += ws * wr;
      spatial_weight += ws;
    }
  }
  const float cx = std::clamp(px, 0.f, static_cast<float>(scene.proxy.width - 1));
  const float cy = std::clamp(py, 0.f, static_cast<float>(scene.proxy.height - 1));
  const int x0 = static_cast<int>(std::floor(cx));
  const int y0 = static_cast<int>(std::floor(cy));
  const float fx = cx - x0;
  const float fy = cy - y0;
  Pair result{};
  for (size_t channel = 0; channel < 3; ++channel) {
    const float proxy = Decode(scene.proxy.At(x0, y0)[channel]) * (1.f - fx) * (1.f - fy)
                        + Decode(scene.proxy.At(x0 + 1, y0)[channel]) * fx * (1.f - fy)
                        + Decode(scene.proxy.At(x0, y0 + 1)[channel]) * (1.f - fx) * fy
                        + Decode(scene.proxy.At(x0 + 1, y0 + 1)[channel]) * fx * fy;
    float edit = 0.f;
    if (joint_weight > 1e-6f) {
      edit = joint[channel] / joint_weight;
    } else if (legacy || scene.reference_linear) {
      edit = spatial[channel] / spatial_weight;
    }
    const float gain = std::exp2(edit);
    float neural = std::max(proxy, kRatioFloor) * gain;
    if (!scene.reference_linear) {
      neural = (original[channel] >= kRatioFloor
                    ? original[channel] * gain
                    : original[channel] + kRatioFloor * (gain - 1.f));
    }
    result[0][channel] = Encode(proxy);
    result[1][channel] = Encode(neural);
  }
  result[0][3] = result[1][3] = 1.f;
  return {Quantize(result[0], DXGI_FORMAT_R16G16B16A16_FLOAT),
          Quantize(result[1], DXGI_FORMAT_R16G16B16A16_FLOAT)};
}

std::vector<Pixel> ReadFloatPixels(const std::filesystem::path& path, UINT width, UINT height) {
  Check(width > 0 && height > 0 && static_cast<uint64_t>(width) * height <= kMaxPixels,
        "bounded float crop dimensions");
  static_assert(sizeof(Pixel) == 4 * sizeof(float));
  std::vector<Pixel> values(static_cast<size_t>(width) * height);
  Check(std::filesystem::file_size(path) == values.size() * sizeof(Pixel), "exact float32 RGBA crop byte count");
  std::ifstream file(path, std::ios::binary);
  file.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(Pixel)));
  Check(file.good(), "float32 crop read");
  for (const auto& pixel : values) {
    for (float channel : pixel) Check(std::isfinite(channel), "finite float32 crop");
  }
  return values;
}

Region ReadRegion(const json& descriptor, UINT full_width, UINT full_height) {
  const auto origin = descriptor.at("origin").get<std::array<UINT, 2>>();
  const auto extent = descriptor.at("extent").get<std::array<UINT, 2>>();
  Check(extent[0] > 0 && extent[1] > 0 && origin[0] < full_width && origin[1] < full_height
            && extent[0] <= full_width - origin[0] && extent[1] <= full_height - origin[1],
        "crop is inside its original full dimensions");
  return {.x = origin[0], .y = origin[1], .width = extent[0], .height = extent[1]};
}

void RequireCleanInfoQueue(ID3D12InfoQueue* queue, json* report) {
  Check(queue != nullptr, "mandatory D3D12 InfoQueue");
  json errors = json::array();
  json warnings = json::array();
  const auto count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
  for (UINT64 index = 0; index < count; ++index) {
    SIZE_T bytes = 0;
    Check(SUCCEEDED(queue->GetMessage(index, nullptr, &bytes)) && bytes >= sizeof(D3D12_MESSAGE),
          "InfoQueue message size");
    std::vector<uint8_t> storage(bytes);
    auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
    Check(SUCCEEDED(queue->GetMessage(index, message, &bytes)), "InfoQueue message read");
    if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) {
      warnings.push_back({{"id", message->ID},
                          {"description", std::string(message->pDescription, message->DescriptionByteLength)}});
    }
    if (message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION || message->Severity == D3D12_MESSAGE_SEVERITY_ERROR) {
      const std::string text(message->pDescription, message->DescriptionByteLength);
      errors.push_back({{"id", message->ID}, {"description", text}});
      std::cerr << text << '\n';
    }
  }
  (*report)["debug_layer"] = true;
  (*report)["info_queue"] = {{"available", true}, {"messages", count},
                            {"warnings", warnings.size()}, {"warning_messages", warnings}, {"errors", errors}};
  Check(errors.empty(), "D3D12 Debug Layer reported an error or corruption");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  json report{{"backend", "WARP"}, {"ngx", "NotRun"}, {"status", "Failed"}};
  std::filesystem::path result_path;
  try {
    Check(argc == 7, "usage: look_transport_tests <root> <shader> <result.json> fixed|legacy <roi.json|-> <roi-output-dir>");
    result_path = argv[3];
    const bool legacy = std::wstring(argv[4]) == L"legacy";
    Check(legacy || std::wstring(argv[4]) == L"fixed", "explicit fixed or legacy expectation");
    report["expectation"] = (legacy ? "legacy_negative_control" : "fixed_regression");
    report["shader"] = std::filesystem::path(argv[2]).generic_string();
    ComPtr<ID3D12Debug> debug;
    Check(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))), "mandatory D3D12 Debug Layer before device creation");
    debug->EnableDebugLayer();
    d3d12_test::Gpu gpu(false);
    ComPtr<ID3D12InfoQueue> info_queue;
    Check(SUCCEEDED(gpu.device.As(&info_queue)), "mandatory D3D12 InfoQueue interface");
    ComPtr<IDXGIAdapter1> adapter;
    Check(SUCCEEDED(gpu.factory->EnumAdapterByLuid(gpu.device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))),
          "actual software adapter identity");
    DXGI_ADAPTER_DESC1 adapter_desc{};
    Check(SUCCEEDED(adapter->GetDesc1(&adapter_desc)) && (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0,
          "actual device remains software WARP");
    LARGE_INTEGER driver{};
    Check(SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver)), "driver identity");
    report["adapter"] = {{"vendor_id", adapter_desc.VendorId}, {"device_id", adapter_desc.DeviceId},
                         {"software", true}, {"luid", {adapter_desc.AdapterLuid.LowPart, adapter_desc.AdapterLuid.HighPart}},
                         {"driver_version", {HIWORD(driver.HighPart), LOWORD(driver.HighPart), HIWORD(driver.LowPart), LOWORD(driver.LowPart)}}};
    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> errors;
    const auto compile = D3DCompileFromFile(argv[2], nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                          "main", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
    if (errors) std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
    Check(SUCCEEDED(compile), "production look_upsample shader compilation");
    ComPtr<ID3DBlob> signature_blob;
    Check(SUCCEEDED(look::SerializeRootSignature(D3D12SerializeRootSignature, &signature_blob, &errors)),
          "production look root signature serialization");
    ComPtr<ID3D12RootSignature> signature;
    Check(SUCCEEDED(gpu.device->CreateRootSignature(0, signature_blob->GetBufferPointer(), signature_blob->GetBufferSize(),
                                                  IID_PPV_ARGS(&signature))), "look root signature");
    const D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{
        .pRootSignature = signature.Get(), .CS = {shader->GetBufferPointer(), shader->GetBufferSize()}};
    ComPtr<ID3D12PipelineState> program;
    Check(SUCCEEDED(gpu.device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&program))), "look upsample PSO");
    RequireCleanInfoQueue(info_queue.Get(), &report);

    const auto dispatch = [&](const Scene& scene) {
      Check(scene.proxy.width <= scene.reference.width && scene.proxy.height <= scene.reference.height
                && (scene.proxy.width != scene.reference.width || scene.proxy.height != scene.reference.height)
                && scene.neural.width == scene.proxy.width && scene.neural.height == scene.proxy.height,
            "production transport grid contract");
      const auto upload = [&](const Surface& surface) {
        std::vector<uint16_t> words;
        words.reserve(surface.pixels.size() * 4);
        for (const auto& pixel : surface.pixels) {
          for (float value : pixel) {
            words.push_back(surface.format == DXGI_FORMAT_R16G16B16A16_UNORM
                                ? static_cast<uint16_t>(std::lround(std::clamp(value, 0.f, 1.f) * 65535.f))
                                : XMConvertFloatToHalf(value));
          }
        }
        auto texture = gpu.Texture(surface.width, surface.height, surface.format, D3D12_RESOURCE_STATE_COPY_DEST);
        auto transfer = gpu.CreateTextureTransfer(texture.Get());
        transfer.readback.Reset();
        gpu.UploadTexture(texture.Get(), transfer, reinterpret_cast<const uint8_t*>(words.data()), words.size() * 2,
                          static_cast<UINT64>(surface.width) * 8);
        gpu.Transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return texture;
      };
      auto reference = upload(scene.reference);
      auto proxy = upload(scene.proxy);
      auto neural = upload(scene.neural);
      const D3D12_HEAP_PROPERTIES default_heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      auto output_desc = reference->GetDesc();
      output_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      std::array<ComPtr<ID3D12Resource>, 2> output;
      std::array<d3d12_test::TextureTransfer, 2> transfer;
      for (size_t index = 0; index < output.size(); ++index) {
        Check(SUCCEEDED(gpu.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &output_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output[index]))), "look transport output");
        transfer[index] = gpu.CreateTextureTransfer(output[index].Get());
        transfer[index].upload.Reset();
      }
      const D3D12_RESOURCE_DESC history_desc{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = 256,
          .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleDesc = {1, 0},
          .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR, .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
      ComPtr<ID3D12Resource> history;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &history_desc,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&history))), "valid unused history buffer");
      const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
          .NumDescriptors = 14, .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
      ComPtr<ID3D12DescriptorHeap> heap;
      Check(SUCCEEDED(gpu.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap))), "look descriptor heap");
      const auto stride = gpu.device->GetDescriptorHandleIncrementSize(heap_desc.Type);
      const auto cpu_start = heap->GetCPUDescriptorHandleForHeapStart();
      const auto gpu_start = heap->GetGPUDescriptorHandleForHeapStart();
      const auto cpu_at = [&](UINT slot) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu_start.ptr + slot * stride}; };
      const auto gpu_at = [&](UINT slot) { return D3D12_GPU_DESCRIPTOR_HANDLE{gpu_start.ptr + slot * stride}; };
      for (UINT slot = 0; slot < 4; ++slot) {
        auto* texture = (slot == 1 ? proxy.Get() : (slot == 2 ? neural.Get() : reference.Get()));
        const D3D12_SHADER_RESOURCE_VIEW_DESC srv{.Format = texture->GetDesc().Format,
            .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, .Texture2D = {.MipLevels = 1}};
        gpu.device->CreateShaderResourceView(texture, &srv, cpu_at(slot));
      }
      const D3D12_SHADER_RESOURCE_VIEW_DESC motion_srv{.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
          .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
          .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, .Texture2D = {.MipLevels = 1}};
      gpu.device->CreateShaderResourceView(nullptr, &motion_srv, cpu_at(13));
      for (UINT slot = 0; slot < look::kUavCount; ++slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
            .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D};
        if (slot == look::kUavBandA || slot == look::kUavBandB || slot == look::kUavTraceBlocks) {
          uav.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        } else if (slot == look::kUavBandMaxA || slot == look::kUavBandMaxB) {
          uav.Format = DXGI_FORMAT_R32G32_FLOAT;
        } else if (slot == look::kUavTraceHistogram) {
          uav.Format = DXGI_FORMAT_R32_TYPELESS;
          uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
          uav.Buffer = {.NumElements = look::kTraceWords, .Flags = D3D12_BUFFER_UAV_FLAG_RAW};
        }
        ID3D12Resource* texture = nullptr;
        if (slot == look::kUavUpProxy) texture = output[0].Get();
        if (slot == look::kUavUpNeural) texture = output[1].Get();
        gpu.device->CreateUnorderedAccessView(texture, nullptr, &uav, cpu_at(4 + slot));
      }
      for (auto* resource : std::array<IUnknown*, 9>{reference.Get(), proxy.Get(), neural.Get(), output[0].Get(),
                                                    output[1].Get(), history.Get(), heap.Get(), signature.Get(), program.Get()}) {
        gpu.Retain(resource);
      }
      const look::Pipeline pipeline{.root_signature = signature.Get(), .upsample = program.Get()};
      const look::Bindings bindings{.heap = heap.Get(), .codec_set = gpu_at(0), .look_set = gpu_at(0),
          .uav_table = gpu_at(4), .motion = gpu_at(13), .history_in = history->GetGPUVirtualAddress(),
          .history_out = history->GetGPUVirtualAddress()};
      const look::Frame frame{.nr_width = scene.proxy.width, .nr_height = scene.proxy.height,
          .full_width = scene.reference.width, .full_height = scene.reference.height,
          .reference_linear = scene.reference_linear, .transport = true};
      look::RecordUpsample(gpu.list.Get(), pipeline, bindings, look::MakeConstants({}, frame));
      for (size_t index = 0; index < output.size(); ++index) {
        gpu.Transition(output[index].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.RecordTextureReadback(output[index].Get(), transfer[index]);
      }
      gpu.Submit();
      Check(gpu.fence->GetCompletedValue() >= gpu.serial, "real submitted WARP completion before readback");
      RequireCleanInfoQueue(info_queue.Get(), &report);
      std::array<std::vector<Pixel>, 2> result;
      for (size_t index = 0; index < output.size(); ++index) {
        const auto bytes = gpu.ReadTexture(transfer[index]);
        auto& crop = result[index];
        crop.reserve(static_cast<size_t>(scene.roi.width) * scene.roi.height);
        for (UINT y = scene.roi.y; y < scene.roi.y + scene.roi.height; ++y) {
          for (UINT x = scene.roi.x; x < scene.roi.x + scene.roi.width; ++x) {
            Pixel pixel{};
            for (size_t channel = 0; channel < pixel.size(); ++channel) {
              uint16_t word = 0;
              std::memcpy(&word, bytes.data() + (static_cast<size_t>(y) * scene.reference.width + x) * 8 + channel * 2, 2);
              pixel[channel] = XMConvertHalfToFloat(word);
              Check(std::isfinite(pixel[channel]), "finite production transport output");
            }
            crop.push_back(pixel);
          }
        }
      }
      return result;
    };

    std::vector<Scene> scenes;
    const Pixel p{0.18f, 0.11f, 0.06f, 1.f};
    Pixel code{};
    Pixel edited{};
    for (size_t channel = 0; channel < 3; ++channel) {
      code[channel] = Encode(p[channel]);
      edited[channel] = Encode(p[channel] * std::array<float, 3>{1.8f, 0.8f, 1.2f}[channel]);
    }
    code[3] = 1.f;
    edited[3] = std::log2(Luminance(p));
    const auto constant_scene = [&](const std::string& id, bool linear, Pixel reference, Pixel proxy, Pixel neural) {
      return Scene{.id = id, .reference_linear = linear,
          .reference = Filled(35, 19, (linear ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16G16B16A16_UNORM), reference),
          .proxy = Filled(23, 13, DXGI_FORMAT_R16G16B16A16_FLOAT, proxy),
          .neural = Filled(23, 13, DXGI_FORMAT_R16G16B16A16_FLOAT, neural), .roi = {0, 0, 35, 19}};
    };
    scenes.push_back(constant_scene("supported_chromatic_edit", false, code, code, edited));
    auto identity = code;
    identity[3] = edited[3];
    scenes.push_back(constant_scene("identity", false, code, code, identity));
    Scene mismatch{.id = "area_guide_mismatch", .reference_linear = false,
        .reference = Filled(5, 5, DXGI_FORMAT_R16G16B16A16_UNORM, code),
        .proxy = Filled(3, 3, DXGI_FORMAT_R16G16B16A16_FLOAT, {0.01f, 0.01f, 0.01f, 1.f}),
        .neural = Filled(3, 3, DXGI_FORMAT_R16G16B16A16_FLOAT, {0.3f, 0.02f, 0.02f, edited[3]}),
        .roi = {2, 2, 1, 1}};
    mismatch.proxy.pixels[4] = Quantize(code, mismatch.proxy.format);
    edited[3] = -24.f;
    mismatch.neural.pixels[4] = Quantize(edited, mismatch.neural.format);
    scenes.push_back(std::move(mismatch));
    scenes.push_back(constant_scene("no_supported_tap", false, {0.6f, 0.6f, 0.6f, 1.f},
                                    {0.01f, 0.01f, 0.01f, 1.f}, {0.3f, 0.3f, 0.3f, -24.f}));
    scenes.push_back(constant_scene("hdr_matched_guide", true, {0.6f, 0.6f, 0.6f, 1.f},
                                    {0.01f, 0.01f, 0.01f, 1.f}, {0.3f, 0.3f, 0.3f, std::log2(0.6f)}));
    scenes.push_back(constant_scene("hdr_spatial_fallback", true, {0.6f, 0.6f, 0.6f, 1.f},
                                    {0.01f, 0.01f, 0.01f, 1.f}, {0.3f, 0.3f, 0.3f, -24.f}));
    auto edge = constant_scene("tile_clamp_edges", false, code, code, identity);
    edge.proxy = Filled(33, 17, DXGI_FORMAT_R16G16B16A16_FLOAT, code);
    edge.neural = Filled(33, 17, DXGI_FORMAT_R16G16B16A16_FLOAT, identity);
    for (UINT y = 0; y < edge.proxy.height; ++y) {
      for (UINT x = 0; x < edge.proxy.width; ++x) {
        Pixel proxy{};
        Pixel neural{};
        Pixel linear{};
        for (size_t channel = 0; channel < 3; ++channel) {
          proxy[channel] = Encode(p[channel] * (0.7f + 0.01f * x + 0.004f * y));
        }
        proxy[3] = 1.f;
        proxy = Quantize(proxy, edge.proxy.format);
        for (size_t channel = 0; channel < 3; ++channel) {
          linear[channel] = Decode(proxy[channel]);
          neural[channel] = Encode(linear[channel] * std::exp2(0.003f * x - 0.006f * y + 0.1f * channel));
        }
        neural[3] = std::log2(Luminance(linear));
        edge.proxy.pixels[static_cast<size_t>(y) * edge.proxy.width + x] = proxy;
        edge.neural.pixels[static_cast<size_t>(y) * edge.proxy.width + x] = Quantize(neural, edge.neural.format);
      }
    }
    scenes.push_back(std::move(edge));
    report["synthetic"] = json::array();
    for (const auto& scene : scenes) {
      const auto output = dispatch(scene);
      std::array<float, 2> max_error{};
      float red_peak = 0.f;
      float fixed_red_peak = 0.f;
      for (UINT y = 0; y < scene.roi.height; ++y) {
        for (UINT x = 0; x < scene.roi.width; ++x) {
          const auto index = static_cast<size_t>(y) * scene.roi.width + x;
          const auto expected = ReferencePixel(scene, scene.roi.x + x, scene.roi.y + y, legacy);
          for (size_t target = 0; target < 2; ++target) {
            for (size_t channel = 0; channel < 4; ++channel) {
              max_error[target] = std::max(max_error[target], std::abs(output[target][index][channel] - expected[target][channel]));
            }
          }
          red_peak = std::max(red_peak, output[1][index][0]);
          fixed_red_peak = std::max(fixed_red_peak, ReferencePixel(scene, scene.roi.x + x, scene.roi.y + y, false)[1][0]);
        }
      }
      report["synthetic"].push_back({{"id", scene.id}, {"sourceFullSize", {scene.reference.width, scene.reference.height}},
          {"workingSize", {scene.proxy.width, scene.proxy.height}}, {"referenceLinear", scene.reference_linear},
          {"max_encoded_error", max_error}, {"red_peak", red_peak}, {"fixed_reference_red_peak", fixed_red_peak},
          {"completed_fence", gpu.serial}});
      Check(max_error[0] <= kTolerance && max_error[1] <= kTolerance, "synthetic production transport differs from reference");
      if (scene.id == "area_guide_mismatch") {
        Check(ReferencePixel(scene, 2, 2, true)[1][0] > fixed_red_peak + 0.2f,
              "synthetic fixture must reproduce the old mismatched-guide amplification");
        Check(legacy ? red_peak > fixed_red_peak + 0.2f : red_peak < 0.7f,
              "area-guide mismatch negative control or fixed bound");
      }
      if (scene.id == "no_supported_tap") {
        Check(legacy ? red_peak > 0.99f : std::abs(red_peak - 0.6f) < kTolerance,
              "unsupported SDR edit must preserve the original only in the fixed path");
      }
      if (scene.id == "supported_chromatic_edit") {
        const auto& sample = output[1][0];
        Check(sample[0] > code[0] + 0.1f && sample[1] < code[1] - 0.02f && sample[2] > code[2] + 0.01f,
              "supported nonneutral chromatic edits remain active");
      }
      if (scene.reference_linear) {
        const auto fixed = ReferencePixel(scene, 0, 0, false);
        const auto old = ReferencePixel(scene, 0, 0, true);
        Check(fixed == old && std::abs(output[1][0][0] - 0.3f) < kTolerance,
              "HDR retains its matched-guide and spatial-fallback behavior");
      }
    }

    if (std::wstring(argv[5]) != L"-") {
      const std::filesystem::path descriptor_path = argv[5];
      std::ifstream descriptor_file(descriptor_path);
      Check(descriptor_file.good(), "ROI fixture descriptor open");
      const auto descriptor = json::parse(descriptor_file);
      Check(descriptor.at("schema") == "nr-nexus-look-transport-fixture/1", "ROI fixture schema");
      const auto full = descriptor.at("sourceFullSize").get<std::array<UINT, 2>>();
      const auto work = descriptor.at("workingSize").get<std::array<UINT, 2>>();
      Scene scene{.id = "captured_roi", .reference_linear = descriptor.value("referenceLinear", false)};
      scene.roi = ReadRegion(descriptor.at("outputRoi"), full[0], full[1]);
      std::array<Region, 3> input_regions;
      const auto load = [&](const char* key, UINT width, UINT height, Region* region) {
        const auto& item = descriptor.at(key);
        const std::string format = item.at("format");
        Check(format == "R16G16B16A16_UNORM" || format == "R16G16B16A16_FLOAT", "ROI input format");
        auto surface = Filled(width, height,
            (format == "R16G16B16A16_UNORM" ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT), {});
        *region = ReadRegion(item, width, height);
        const auto values = ReadFloatPixels(descriptor_path.parent_path() / item.at("path").get<std::string>(), region->width, region->height);
        for (UINT y = 0; y < region->height; ++y) {
          for (UINT x = 0; x < region->width; ++x) {
            surface.pixels[static_cast<size_t>(region->y + y) * width + region->x + x]
                = Quantize(values[static_cast<size_t>(y) * region->width + x], surface.format);
          }
        }
        return surface;
      };
      scene.reference = load("reference", full[0], full[1], &input_regions[0]);
      scene.proxy = load("proxy", work[0], work[1], &input_regions[1]);
      scene.neural = load("neural", work[0], work[1], &input_regions[2]);
      const auto covered = [](const Region& region, int x, int y) {
        return x >= static_cast<int>(region.x) && y >= static_cast<int>(region.y)
               && static_cast<UINT>(x) - region.x < region.width && static_cast<UINT>(y) - region.y < region.height;
      };
      for (UINT y = scene.roi.y; y < scene.roi.y + scene.roi.height; ++y) {
        for (UINT x = scene.roi.x; x < scene.roi.x + scene.roi.width; ++x) {
          Check(covered(input_regions[0], static_cast<int>(x), static_cast<int>(y)), "ROI reference crop coverage");
          const int bx = static_cast<int>(std::floor((static_cast<float>(x) + 0.5f) * work[0] / full[0] - 0.5f));
          const int by = static_cast<int>(std::floor((static_cast<float>(y) + 0.5f) * work[1] / full[1] - 0.5f));
          for (int dy = -1; dy <= 2; ++dy) {
            for (int dx = -1; dx <= 2; ++dx) {
              const int tx = std::clamp(bx + dx, 0, static_cast<int>(work[0]) - 1);
              const int ty = std::clamp(by + dy, 0, static_cast<int>(work[1]) - 1);
              Check(covered(input_regions[1], tx, ty) && covered(input_regions[2], tx, ty), "ROI contains every production transport tap");
            }
          }
        }
      }
      const auto output = dispatch(scene);
      const std::filesystem::path output_dir = argv[6];
      json roi{{"status", "Recorded"}, {"sourceFullSize", full}, {"workingSize", work},
          {"outputRoi", descriptor.at("outputRoi")}, {"referenceLinear", scene.reference_linear},
          {"input_formats", {descriptor.at("reference").at("format"), descriptor.at("proxy").at("format"), descriptor.at("neural").at("format")}},
          {"dispatch", "full production dimensions; zero outside supplied input crops; only covered ROI evaluated"},
          {"readback_format", "little-endian float32 RGBA converted from RGBA16_FLOAT"}, {"completed_fence", gpu.serial}};
      for (size_t target = 0; target < output.size(); ++target) {
        const auto name = (target == 0 ? "proxy-roi.f32" : "neural-roi.f32");
        std::ofstream file(output_dir / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(output[target].data()), static_cast<std::streamsize>(output[target].size() * sizeof(Pixel)));
        Check(file.good(), "ROI float32 readback write");
      }
      if (descriptor.contains("expectedNeural")) {
        const auto& item = descriptor.at("expectedNeural");
        const auto region = ReadRegion(item, full[0], full[1]);
        Check(region.x == scene.roi.x && region.y == scene.roi.y && region.width == scene.roi.width && region.height == scene.roi.height,
              "expected neural crop exactly matches output ROI");
        const auto expected = ReadFloatPixels(descriptor_path.parent_path() / item.at("path").get<std::string>(), region.width, region.height);
        float maximum = 0.f;
        for (size_t index = 0; index < expected.size(); ++index) {
          for (size_t channel = 0; channel < 4; ++channel) {
            maximum = std::max(maximum, std::abs(output[1][index][channel] - expected[index][channel]));
          }
        }
        const float tolerance = descriptor.value("tolerance", kTolerance);
        Check(std::isfinite(tolerance) && tolerance > 0.f && tolerance <= 0.01f, "bounded encoded ROI tolerance");
        roi["fixed_expected_max_encoded_error"] = maximum;
        roi["tolerance"] = tolerance;
        roi["status"] = (legacy ? "LegacyRecordedAgainstFixedReference" : (maximum <= tolerance ? "Passed" : "Failed"));
        report["roi"] = roi;
        Check(legacy || maximum <= tolerance, "captured ROI differs from the independently supplied fixed reference");
      } else {
        roi["limit"] = "no expectedNeural supplied; readback is not a pixel-correctness assertion";
      }
      report["roi"] = std::move(roi);
    } else {
      report["roi"] = {{"status", "NotRun"}, {"reason", "no ROI fixture supplied"}};
    }
    RequireCleanInfoQueue(info_queue.Get(), &report);
    report["completed_fence"] = gpu.fence->GetCompletedValue();
    report["status"] = "Passed";
    std::ofstream result(result_path);
    result << report.dump(2) << '\n';
    Check(result.good(), "transport result write");
    std::cout << scenes.size() << " bounded production look_upsample cases; strict WARP completed; NGX not run\n";
    return 0;
  } catch (const std::exception& error) {
    report["error"] = error.what();
    if (!result_path.empty()) {
      std::ofstream result(result_path);
      result << report.dump(2) << '\n';
    }
    std::cerr << error.what() << '\n';
    return 1;
  }
}
