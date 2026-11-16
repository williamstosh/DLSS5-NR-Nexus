/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#if defined(NR_NEXUS_DEV_CONTROL)
#include <d3d12sdklayers.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <screenshot.hpp>

#include "d3d12_test_gpu.hpp"

namespace fg_consumer_capture_cases {
namespace shot = renodx::addons::dlss5::screenshot;
namespace submission = renodx::addons::dlss5::submission;
using d3d12_test::Check;
using d3d12_test::Gpu;
using Microsoft::WRL::ComPtr;

inline int Run() {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    Check(renodx::utils::directx::Initialize(), "consumer capture DirectX entry points");
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
    }
    Gpu gpu(false);
    ComPtr<ID3D12InfoQueue> info;
    gpu.device.As(&info);
    Check(info != nullptr, "consumer capture requires a real WARP Debug InfoQueue");
    const auto directory = std::filesystem::current_path() / "fg-consumer-evidence";
    constexpr uint32_t width = 256, height = 192;
    constexpr shot::CaptureRect roi{.x = 16, .y = 8, .width = 32, .height = 16};
    const shot::FgConsumerContext context{.feature_handle = 0x1234, .parameters_identity = 0x5678,
        .module_slot = 1, .mfg_index = 2, .mfg_index_available = true, .present_generation = 10};
    std::string reason;
    Check(!shot::WantsFgConsumerCapture() && shot::BeginFgConsumerCapture(nullptr, nullptr, nullptr, nullptr, context) == 0,
        "disabled consumer capture is inert");
    const auto texture = [&](DXGI_FORMAT format, bool output) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = width, .Height = height, .DepthOrArraySize = 1, .MipLevels = 1,
          .Format = format, .SampleDesc = {1, 0},
          .Flags = (output ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE)};
      ComPtr<ID3D12Resource> result;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result))), "consumer fixture texture");
      return result;
    };
    const auto upload = [&](ID3D12Resource* target, uint8_t seed, bool output) {
      auto transfer = gpu.CreateTextureTransfer(target);
      const auto bytes_per_pixel = shot::internal::DiagnosticPixelBytes(target->GetDesc().Format);
      std::vector<uint8_t> data(static_cast<size_t>(width) * height * bytes_per_pixel);
      for (size_t index = 0; index < data.size(); ++index) {
        data[index] = static_cast<uint8_t>(seed + index * 17 + index / 53);
      }
      gpu.Transition(target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.UploadTexture(target, transfer, data.data(), data.size(), width * bytes_per_pixel);
      gpu.Transition(target, D3D12_RESOURCE_STATE_COPY_DEST,
          output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      return data;
    };
    const auto submit = [&](uint32_t repeats = 1, bool delayed = false) {
      Check(SUCCEEDED(gpu.list->Close()), "consumer fixture close");
      ComPtr<ID3D12Fence> gate;
      if (delayed) {
        Check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))
                  && SUCCEEDED(gpu.queue->Wait(gate.Get(), 1)), "consumer fixture bounded GPU gate");
      }
      struct ReleaseGate {
        ID3D12Fence* fence;
        ~ReleaseGate() {
          if (fence != nullptr) {
            fence->Signal(1);
          }
        }
      } release_gate{gate.Get()};
      const auto wait_completed = [&] {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(event != nullptr && SUCCEEDED(gpu.fence->SetEventOnCompletion(gpu.serial, event)), "consumer fixture Fence event");
        const auto wait = WaitForSingleObject(event, 10000);
        CloseHandle(event);
        Check(wait == WAIT_OBJECT_0, "consumer fixture bounded GPU completion");
      };
      ID3D12CommandList* lists[]{gpu.list.Get()};
      for (uint32_t repeat = 0; repeat < repeats; ++repeat) {
        if (repeat != 0) {
          wait_completed();
        }
        const auto ticket = submission::BeginSubmission(1, lists);
        gpu.queue->ExecuteCommandLists(1, lists);
        gpu.submission_pending = true;
        Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "consumer fixture Signal");
        submission::CompleteSubmission(ticket, gpu.fence.Get(), gpu.serial);
      }
      if (delayed) {
        const bool completed_early = shot::internal::StageSubmissionCompleted(&*shot::internal::pending);
        Check(SUCCEEDED(gate->Signal(1)), "release the fixture GPU gate before checking its result");
        release_gate.fence = nullptr;
        Check(!completed_early, "enqueued but incomplete Fence cannot prove the consumer copies");
      }
      wait_completed();
      gpu.submission_pending = false;
      // This is the same pre-Reset receipt preservation used by the real hook.
      shot::PreserveStageSubmissionProofs();
      Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "consumer fixture Reset");
      submission::OnCommandListReset(gpu.list.Get());
      submission::PruneCompletedGenerations();
      gpu.uploads.clear();
      gpu.retained_resources.clear();
    };
    const auto finish = [&](const char* expected) {
      shot::OnPresent(20, true);
      const auto deadline = GetTickCount64() + 10000;
      for (;;) {
        const auto status = shot::CopyFgConsumerStatus();
        if (std::strcmp(status.status, expected) == 0 && !shot::internal::worker_busy.load()
            && shot::internal::outstanding_jobs.load() == 0 && shot::internal::aggregate_bytes.load() == 0) return;
        Check(GetTickCount64() < deadline, "consumer worker settles within the fixture deadline");
        Sleep(1);
      }
    };
    const auto read = [](const std::filesystem::path& path) {
      std::ifstream input(path, std::ios::binary);
      Check(input.good(), "consumer export file exists");
      return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
    };
    ComPtr<ID3D12Resource> backbuffer, hudless, output;
    const std::array<std::pair<DXGI_FORMAT, DXGI_FORMAT>, 3> formats{{
        {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT},
        {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM}}};
    for (const auto& [input_format, output_format] : formats) {
      backbuffer = texture(output_format, false);
      hudless = texture(input_format, false);
      output = texture(output_format, true);
      upload(backbuffer.Get(), 19, false);
      const auto input_pixels = upload(hudless.Get(), 71, false);
      const auto output_pixels = upload(output.Get(), 149, true);
      const auto id = (input_format != output_format ? "warp_mixed"
          : (input_format == DXGI_FORMAT_R16G16B16A16_FLOAT ? "warp_fp16" : "warp_pq10"));
      Check(shot::RequestFgConsumerCapture(directory, id, roi, &reason), "arm one consumer request");
      Check(!shot::RequestCapture() && !shot::RequestFgConsumerCapture(directory, "conflict", roi), "shared capture request exclusion");
      Check(!shot::PrepareCapture(gpu.device.Get(), hudless.Get(), 0, 0, width, height, input_format, 1, 10, 80.f, true),
          "ordinary model capture cannot claim a consumer request");
      const auto serial = shot::BeginFgConsumerCapture(gpu.device.Get(), gpu.list.Get(), backbuffer.Get(), hudless.Get(), context);
      Check(serial != 0 && !shot::WantsFgConsumerCapture(), "consumer claims exactly one actual call");
      Check(shot::BeginFgConsumerCapture(gpu.device.Get(), gpu.list.Get(), backbuffer.Get(), hudless.Get(), context) == 0,
          "repeated MFG call cannot consume the same request twice");
      Check(shot::EndFgConsumerCapture(serial, gpu.list.Get(), output.Get(), 1, true), "MOCK successful SDK call seals typed output");
      Check(!shot::internal::StageSubmissionCompleted(&*shot::internal::pending), "recording alone is not GPU completion");
      submit(1, output_format == DXGI_FORMAT_R16G16B16A16_FLOAT);
      finish("written");
      const auto stem = std::string(id) + "_" + std::to_string(serial);
      const auto input_raw = read(directory / (stem + "_input.raw"));
      const auto output_raw = read(directory / (stem + "_output_interpolated.raw"));
      const auto input_pixel_bytes = shot::internal::DiagnosticPixelBytes(input_format);
      const auto output_pixel_bytes = shot::internal::DiagnosticPixelBytes(output_format);
      Check(input_raw.size() == size_t(roi.width) * roi.height * input_pixel_bytes
                && output_raw.size() == size_t(roi.width) * roi.height * output_pixel_bytes,
          "both exports contain exactly one tight ROI without row padding");
      for (uint32_t y = 0; y < roi.height; ++y) {
        const size_t pixel = static_cast<size_t>(roi.y + y) * width + roi.x;
        Check(std::memcmp(input_raw.data() + size_t(y) * roi.width * input_pixel_bytes,
                  input_pixels.data() + pixel * input_pixel_bytes, roi.width * input_pixel_bytes) == 0,
            "exported input is the exact selected HUDLess ROI, including alpha");
        Check(std::memcmp(output_raw.data() + size_t(y) * roi.width * output_pixel_bytes,
                  output_pixels.data() + pixel * output_pixel_bytes, roi.width * output_pixel_bytes) == 0,
            "exported output is the exact passed OutputInterpolated ROI");
      }
      const auto metadata_bytes = read(directory / (stem + ".meta.txt"));
      const std::string metadata(metadata_bytes.begin(), metadata_bytes.end());
      Check(metadata.find("raw_input_output_copy_valid=1") != std::string::npos
                && metadata.find("source_frame_token_available=0") != std::string::npos
                && metadata.find("interpolation_validity_flag_verified=0") != std::string::npos
                && metadata.find("presentation_verified=0") != std::string::npos,
          "raw-copy proof stays separate from unknown interpolation and display validity");
    }
    auto unsupported = gpu.Texture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    Check(shot::RequestFgConsumerCapture(directory, "unsupported", roi), "arm unknown color format case");
    Check(shot::BeginFgConsumerCapture(gpu.device.Get(), gpu.list.Get(), nullptr, unsupported.Get(), context) == 0
              && std::strcmp(shot::CopyFgConsumerStatus().status, "blocked") == 0,
        "unsupported consumer format stays Blocked without recording a copy");
    Check(shot::RequestFgConsumerCapture(directory, "expired", roi), "arm expiry case");
    shot::internal::requested_fg_consumer->deadline_ms = 0;
    Check(shot::BeginFgConsumerCapture(gpu.device.Get(), gpu.list.Get(), nullptr, hudless.Get(), context) == 0
              && std::strcmp(shot::CopyFgConsumerStatus().status, "blocked") == 0,
        "expired request records no copy and does not wait for a later call");
    Check(shot::RequestFgConsumerCapture(directory, "missing_output", roi), "arm missing typed output case");
    auto serial = shot::BeginFgConsumerCapture(gpu.device.Get(), gpu.list.Get(), nullptr, hudless.Get(), context);
    Check(serial != 0 && !shot::EndFgConsumerCapture(serial, gpu.list.Get(), nullptr, 1, true), "missing OutputInterpolated stays Blocked");
    submit();
    finish("blocked");
    Check(!std::filesystem::exists(directory / ("missing_output_" + std::to_string(serial) + "_output_interpolated.raw")),
        "missing SDK output is never fabricated");
    Check(shot::RequestFgConsumerCapture(directory, "unsubmitted", roi), "arm unsubmitted Reset case");
    serial = shot::BeginFgConsumerCapture(gpu.device.Get(), gpu.list.Get(), nullptr, hudless.Get(), context);
    Check(serial != 0, "unsubmitted capture records its input copy");
    Check(SUCCEEDED(gpu.list->Close()) && SUCCEEDED(gpu.allocator->Reset())
              && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "discard unsubmitted recording");
    submission::OnCommandListReset(gpu.list.Get());
    submission::PruneCompletedGenerations();
    Check(!shot::EndFgConsumerCapture(serial, gpu.list.Get(), output.Get(), 1, true), "changed recording cannot seal an old consumer call");
    finish("blocked");
    Check(!std::filesystem::exists(directory / ("unsubmitted_" + std::to_string(serial) + "_input.raw")),
        "Reset without submission exports no stamped bytes as input data");
    Check(shot::RequestFgConsumerCapture(directory, "replayed", roi), "arm replay rejection case");
    serial = shot::BeginFgConsumerCapture(gpu.device.Get(), gpu.list.Get(), nullptr, hudless.Get(), context);
    Check(serial != 0 && shot::EndFgConsumerCapture(serial, gpu.list.Get(), output.Get(), 1, true), "record one replayable capture");
    submit(2);
    finish("blocked");
    Check(!std::filesystem::exists(directory / ("replayed_" + std::to_string(serial) + "_input.raw")),
        "two actual submissions never export an exact-once consumer copy as written");
    const auto message_count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
    uint64_t stamp_warnings = 0;
    uint64_t unexpected_messages = 0;
    for (UINT64 index = 0; index < message_count; ++index) {
      SIZE_T bytes = 0;
      Check(SUCCEEDED(info->GetMessage(index, nullptr, &bytes)), "consumer Debug Layer message size");
      std::vector<uint8_t> storage(bytes);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
      Check(SUCCEEDED(info->GetMessage(index, message, &bytes)), "consumer Debug Layer message read");
      {
        // The existing unwritten-stamp protocol intentionally writes 16 CPU
        // bytes to a readback heap before any GPU use. Keep its utility warning
        // visible and reject every other Debug Layer message.
        if (message->ID == D3D12_MESSAGE_ID_UNMAP_RANGE_NOT_EMPTY && message->Severity == D3D12_MESSAGE_SEVERITY_WARNING
            && std::strstr(message->pDescription, "D3D12_HEAP_TYPE_READBACK") != nullptr
            && std::strstr(message->pDescription, "[0, 16)") != nullptr) {
          ++stamp_warnings;
          continue;
        }
        ++unexpected_messages;
        std::cerr << "consumer Debug Layer id=" << message->ID << " severity=" << message->Severity
                  << " " << message->pDescription << '\n';
      }
    }
    std::cout << "consumer Debug Layer: intentional readback-stamp warnings=" << stamp_warnings
              << " unexpected=" << unexpected_messages << '\n';
    Check(unexpected_messages == 0, "consumer capture produces no unexpected WARP Debug Layer messages");
    std::cout << "PASS FG consumer ROI: WARP copies/Fence/worker, SDK=MOCK; interpolation validity/display=unverified\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FG consumer capture failure: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace fg_consumer_capture_cases
#endif
