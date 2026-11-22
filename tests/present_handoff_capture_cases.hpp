/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#if defined(NR_NEXUS_DEV_CONTROL)

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>

#include <present_handoff_capture.hpp>

inline void RunPresentHandoffCaptureCases() {
  namespace capture = renodx::addons::dlss5::present_handoff_capture;
  namespace state = capture::internal;
  namespace submission = renodx::addons::dlss5::submission;
  using Microsoft::WRL::ComPtr;
  using namespace std::chrono_literals;
  const auto require = [](bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
  };
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter> warp;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  ComPtr<ID3D12Fence> gate;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
  require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))
              && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))
              && SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))
              && SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))
              && SUCCEEDED(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)))
              && SUCCEEDED(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))
              && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))
              && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))),
          "Present handoff WARP device and list");
  const auto release_gate = [](ID3D12Fence* value) { value->Signal(1); };
  const std::unique_ptr<ID3D12Fence, decltype(release_gate)> gate_release(gate.Get(), release_gate);
  constexpr uint32_t WIDTH = 192;
  constexpr uint32_t HEIGHT = 96;
  constexpr uint32_t PITCH = WIDTH * 4;
  constexpr uint32_t PLANE_BYTES = PITCH * HEIGHT;
  const D3D12_HEAP_PROPERTIES default_heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC texture_desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      .Width = WIDTH, .Height = HEIGHT, .DepthOrArraySize = 1, .MipLevels = 1,
      .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .SampleDesc = {1, 0}};
  const D3D12_HEAP_PROPERTIES upload_heap{.Type = D3D12_HEAP_TYPE_UPLOAD};
  const D3D12_RESOURCE_DESC upload_desc{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
      .Width = 2 * PLANE_BYTES, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1,
      .SampleDesc = {1, 0}, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  ComPtr<ID3D12Resource> backbuffer;
  ComPtr<ID3D12Resource> upload;
  require(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&backbuffer)))
              && SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))),
          "Present handoff native RGBA8 backbuffer and upload");
  const auto pixel = [](uint32_t x, uint32_t y, uint32_t channel, uint32_t stage) {
    return static_cast<unsigned char>((x * 3 + y * 5 + channel * 11 + (channel == 3 ? 0 : stage * 33)) & 0xff);
  };
  void* mapped = nullptr;
  const D3D12_RANGE no_read{};
  require(SUCCEEDED(upload->Map(0, &no_read, &mapped)), "Present handoff map seed pixels");
  for (uint32_t stage = 0; stage < 2; ++stage) {
    for (uint32_t y = 0; y < HEIGHT; ++y) {
      for (uint32_t x = 0; x < WIDTH; ++x) {
        for (uint32_t c = 0; c < 4; ++c) {
          static_cast<unsigned char*>(mapped)[stage * PLANE_BYTES + y * PITCH + x * 4 + c] = pixel(x, y, c, stage);
        }
      }
    }
  }
  upload->Unmap(0, nullptr);
  require(!state::batch.worker_active, "Present handoff offline case requires an idle capture worker");
  state::batch.attempted = 0;
  state::batch.written = 0;
  state::batch.waiting_skipped = 0;
  state::batch.first_waiting_skip.reset();
  state::batch.started = {};
  state::batch.deadline = state::Clock::now() + 10s;
  state::batch.wait_for_guides = true;
  state::batch.accepting.store(true);
  constexpr auto OWNER = submission::RecordingOwner::kOrdinaryPresent;
  submission::SetRecordingOwner(list.Get(), OWNER);
  const auto write = [&](uint32_t stage) {
    const D3D12_TEXTURE_COPY_LOCATION source{.pResource = upload.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = {stage * PLANE_BYTES, {texture_desc.Format, WIDTH, HEIGHT, 1, PITCH}}};
    const D3D12_TEXTURE_COPY_LOCATION destination{.pResource = backbuffer.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  };
  const auto wait = [&](uint64_t value) {
    const std::unique_ptr<void, decltype(&CloseHandle)> event(CreateEventW(nullptr, FALSE, FALSE, nullptr), CloseHandle);
    require(event && SUCCEEDED(fence->SetEventOnCompletion(value, event.get()))
                && WaitForSingleObject(event.get(), 5000) == WAIT_OBJECT_0
                && fence->GetCompletedValue() != UINT64_MAX && fence->GetCompletedValue() >= value,
            "Present handoff bounded WARP fence completion");
  };
  const auto reset = [&] {
    require(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr)),
            "Present handoff reset private recording");
    submission::OnCommandListReset(list.Get());
    submission::SetRecordingOwner(list.Get(), OWNER);
  };
  ID3D12CommandList* lists[] = {list.Get()};
  capture::Capture warm;
  warm.Begin(list.Get(), backbuffer.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, {.app_frame = 1});
  require(state::batch.attempted == 0 && state::batch.waiting_skipped == 1
              && state::batch.first_waiting_skip->app_frame == 1,
          "Neutral frames do not consume the real-guide trigger window");
  write(0);
  capture::Capture healthy;
  healthy.Begin(list.Get(), backbuffer.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST,
                {.output = 11, .app_frame = 2, .real_guides = true, .source_confirmed = true});
  write(1);
  healthy.RecordFinal(D3D12_RESOURCE_STATE_COPY_DEST, {.source = 24, .reset = true, .model_calls = 1});
  require(SUCCEEDED(list->Close()) && SUCCEEDED(queue->Wait(gate.Get(), 1)), "Gate real capture GPU work");
  queue->ExecuteCommandLists(1, lists);
  require(SUCCEEDED(queue->Signal(fence.Get(), 1)), "First production capture Signal");
  healthy.Submitted(queue.Get(), fence.Get(), 1, true);
  require(submission::DiscardOwnedRecording(list.Get(), OWNER), "Close replay of the private recording");
  auto& pair = state::batch.samples[0];
  state::ResolveSample(&pair, true);
  require(pair.original.empty() && pair.final.empty() && pair.readback.Get() != nullptr,
          "Healthy Signal alone neither maps nor retires the capture");
  require(SUCCEEDED(gate->Signal(1)), "Release real capture GPU work");
  wait(1);
  state::ResolveSample(&pair, true);
  require(pair.state == state::SampleState::COMPLETE && pair.readback.Get() == nullptr && pair.completed_value == 1
              && pair.original.size() == 128 * 64 * 4 && pair.final.size() == pair.original.size()
              && pair.region.left == 32 && pair.region.top == 16 && pair.color.source == 24 && pair.color.reset
              && pair.recording->list != 0 && pair.recording->generation != 0,
          "Completed same-generation centered BB pair and metadata");
  for (uint32_t y = 0; y < 64; ++y) {
    for (uint32_t x = 0; x < 128; ++x) {
      for (uint32_t c = 0; c < 4; ++c) {
        const auto offset = (y * 128 + x) * 4 + c;
        require(static_cast<unsigned char>(pair.original[offset]) == pixel(x + 32, y + 16, c, 0)
                    && static_cast<unsigned char>(pair.final[offset]) == pixel(x + 32, y + 16, c, 1),
                "Native RGBA contents must match the actual before/after BB copies, including alpha");
      }
    }
  }

  reset();
  {
    capture::Capture cancelled;
    cancelled.Begin(list.Get(), backbuffer.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, {.app_frame = 3});
    require(SUCCEEDED(list->Close()) && submission::DiscardOwnedRecording(list.Get(), OWNER),
            "Discard the unsubmitted private recording");
  }
  auto& cancelled = state::batch.samples[1];
  state::ResolveSample(&cancelled, true);
  require(cancelled.state == state::SampleState::CANCELLED && cancelled.readback.Get() == nullptr
              && !cancelled.executed && cancelled.original.empty() && !cancelled.frame.real_guides,
          "Unsubmitted neutral sample after trigger is counted and cancelled without mapping");

  reset();
  capture::Capture unproven;
  unproven.Begin(list.Get(), backbuffer.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, {.app_frame = 4});
  unproven.RecordFinal(D3D12_RESOURCE_STATE_COPY_DEST, {});
  require(SUCCEEDED(list->Close()), "Close frame with no healthy Signal receipt");
  queue->ExecuteCommandLists(1, lists);
  unproven.Submitted(queue.Get(), fence.Get(), 2, false);
  require(submission::DiscardOwnedRecording(list.Get(), OWNER), "Close replay of unproven recording");
  require(SUCCEEDED(queue->Signal(fence.Get(), 3)), "Test-only later Signal for WARP teardown");
  wait(3);
  auto& lost = state::batch.samples[2];
  state::ResolveSample(&lost, true);
  require(lost.state == state::SampleState::SIGNAL_LOST && lost.executed && !lost.signaled
              && lost.readback.Get() != nullptr && lost.original.empty() && lost.final.empty()
              && lost.completed_value == 0 && pair.recording->generation != lost.recording->generation,
          "Later Signal cannot authorize export or production retirement of a failed first Signal");
  // The fixture can release the quarantined sample after its independent test-only wait.
  state::batch.accepting.store(false);
  state::batch.samples = {};
  state::batch.attempted = 0;
  state::batch.first_waiting_skip.reset();
  submission::OnCommandListDestroyed(list.Get());
  submission::PruneCompletedGenerations();
  std::cout << "PASS WARP actual BB RGBA pair, trigger, cancellation, healthy Fence and failed first Signal\n";
}

#endif
