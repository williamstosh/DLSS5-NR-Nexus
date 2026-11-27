/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#define DEBUG_LEVEL_0
#include <windows.h>
#include <dlssnr.hpp>
#include "d3d12_test_gpu.hpp"

extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char* message) {
  std::cout << message << '\n';
}

int main() {
  namespace addon = renodx::addons::dlss5;
  namespace native = addon::internal;
  namespace submission = addon::submission;
  using d3d12_test::Check;
  using d3d12_test::ComPtr;
  using d3d12_test::Gpu;
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    renodx::utils::directx::pD3D12SerializeRootSignature = &D3D12SerializeRootSignature;
    Gpu gpu;
    constexpr uint32_t size = native::kMotionRingSize;
    constexpr auto srv_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto target = gpu.Texture();
    auto source = gpu.Texture(8, 8, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    std::array<ComPtr<ID3D12Resource>, size + 1> motion;
    ComPtr<ID3D12CommandAllocator> borrowed_allocator;
    ComPtr<ID3D12GraphicsCommandList> borrowed;
    Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
              IID_PPV_ARGS(&borrowed_allocator))), "borrowed allocator");
    Check(SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
              borrowed_allocator.Get(), nullptr, IID_PPV_ARGS(&borrowed))), "borrowed list");
    submission::SetRecordingOwner(borrowed.Get(), submission::RecordingOwner::kBorrowedGame);
    for (uint32_t i = 0; i <= size; ++i) {
      motion[i] = gpu.Texture(8, 8, DXGI_FORMAT_R16G16_FLOAT, srv_state);
      if (i == size) break;
      native::Transition(borrowed.Get(), motion[i].Get(), srv_state, D3D12_RESOURCE_STATE_COPY_DEST);
      borrowed->CopyResource(motion[i].Get(), source.Get());
      native::Transition(borrowed.Get(), motion[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, srv_state);
      submission::TrackUse(borrowed.Get(), motion[i].Get(), submission::UseKind::kIndependentGuide);
    }
    const auto submit = [&](ID3D12CommandList* list) {
      const auto pending = submission::BeginSubmission(1, &list);
      gpu.queue->ExecuteCommandLists(1, &list);
      const uint64_t signal = ++gpu.serial;
      const bool signaled = SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), signal));
      submission::CompleteSubmission(pending, (signaled ? gpu.fence.Get() : nullptr), signal);
      return signaled;
    };
    const auto wait = [&] {
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr, "fence event");
      const auto result = gpu.fence->SetEventOnCompletion(gpu.serial, event);
      const auto wait_result = (SUCCEEDED(result) ? WaitForSingleObject(event, 5000) : WAIT_FAILED);
      CloseHandle(event);
      Check(wait_result == WAIT_OBJECT_0 && gpu.fence->GetCompletedValue() >= gpu.serial, "WARP fence completed");
    };
    Check(SUCCEEDED(borrowed->Close()), "borrowed close");
    submission::ObserveCommandListClose(borrowed.Get());
    Check(submit(borrowed.Get()), "borrowed signal");
    wait();

    static std::atomic_bool displaced_destroyed{false};
    {
      addon::RuntimeLock lock(native::runtime_mutex);
      native::queue_tracking_active = true;
      native::look_mode = false;
      native::detail_stability = 0u;
      native::pass_look_result[0].follow = true;
      native::LookChain chain{.settings = native::LookSettingsSnapshots(true)};
      chain.wanted = native::LookSurfacesWanted(gpu.device.Get(), chain.settings[0], 8, 8, 8, 8);
      Check(chain.wanted != 0 && chain.settings[0].stabilize == 2 && chain.settings[1].stabilize == 2,
            "auto detail stability requests inherited Motion");
      auto* res = native::EnsureWorkset(gpu.device.Get(), nullptr, target.Get(), 8, 8, 8, 8,
                                       DXGI_FORMAT_R8G8B8A8_UNORM, 0, 2, chain.wanted);
      Check(res != nullptr, "Look workset");
      submission::SetRecordingOwner(gpu.list.Get(), submission::RecordingOwner::kXeFg);
      native::TrackWorksetRecording(gpu.list.Get(), *res);
      native::Transition(gpu.list.Get(), res->proxy, res->proxy_state, srv_state);
      native::Transition(gpu.list.Get(), res->nr_output, res->nr_output_state, srv_state);
      native::Transition(gpu.list.Get(), res->original, res->original_state, srv_state);
      native::Transition(gpu.list.Get(), res->decoded, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv_state);
      std::array<native::NrFeatureSlot, 2> passes;
      uint64_t sequence = 0;
      const auto run = [&](uint32_t pass, uint32_t index) {
        const native::EvaluationContract frame{
            .motion = motion[index].Get(), .motion_width = 8, .motion_height = 8};
        Check(native::RunLookStage(gpu.list.Get(), gpu.device.Get(), *res, passes[pass], pass, chain, frame,
                                   ++sequence).shaped, "Look compose recorded");
      };
      for (uint32_t i = 0; i < size; ++i) run(0, i);
      auto* keys = res->look_motion_slots.get();
      Check(keys != nullptr && !submission::ResourceReleasable(&(*keys)[2]), "Look tracks each actual SRV slot");
      run(1, 2);
      Check(native::look_motion_missing_passes.load() == 0, "same texture hits the original slot");
      run(1, size);
      Check(native::look_motion_ring_full.load() == 1 && native::look_motion_missing_passes.load() == 1,
            "true contention counts a missing Motion pass");

      ComPtr<ID3D12Fence> gate;
      Check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "gate fence");
      Check(SUCCEEDED(gpu.list->Close()), "Look close");
      submission::ObserveCommandListClose(gpu.list.Get());
      Check(SUCCEEDED(gpu.queue->Wait(gate.Get(), 1)), "hold Look execution");
      const bool signaled = submit(gpu.list.Get());
      const bool discarded = submission::DiscardOwnedRecording(gpu.list.Get(), submission::RecordingOwner::kXeFg);
      native::PruneCompletedSubmissionUses();
      const bool blocked = native::RingSlot(res->motion_views, motion[size].Get(), nullptr, nullptr, nullptr, keys) == size;
      Check(SUCCEEDED(gate->Signal(1)), "unblock Look execution");
      Check(signaled && discarded && blocked, "discarded Look still owns slots through the exact fence");
      wait();
      native::PruneCompletedSubmissionUses();
      auto* displaced = motion[0].Get();
      Check(submission::ResourceReleasable(&(*keys)[0]) && !submission::ResourceReleasable(displaced),
            "completed Look frees slots while the borrowed texture can replay");
      Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "Look reset");
      submission::OnCommandListReset(gpu.list.Get());

      native::look_mode = true;
      native::look_stabilize = 2u;
      chain.settings = native::LookSettingsSnapshots(false);
      native::TrackWorksetRecording(gpu.list.Get(), *res);
      run(0, size);
      Check(res->motion_views[0] == motion[size].Get() && native::look_motion_ring_recycled.load() == 1
                && native::look_motion_missing_passes.load() == 1,
            "explicit Motion overwrites the available slot and binds the new texture");
      Check(native::retired_look_motion_views.contains(displaced), "displaced texture retains its own reference");
      gpu.Submit();

      native::RetireAllWorksets();
      Check(native::retired_final_resources.size() == 1
                && native::retired_final_resources.back().resources.look_motion_slots.get() == keys,
            "slot keys keep their address as the workset retires");
      native::DrainRetiredResources(false);
      Check(native::retired_final_resources.empty() && native::retired_look_motion_views.size() == size,
            "workset release leaves each replayable texture independently retained");
      displaced->AddRef();
      auto* duplicate_view = displaced;
      native::ReleaseLookMotionView(&duplicate_view);
      Check(duplicate_view == nullptr && native::retired_look_motion_views.size() == size,
            "deferred ownership is deduplicated by texture");
      ComPtr<ID3DDestructionNotifier> notifier;
      UINT callback_id = 0;
      Check(SUCCEEDED(motion[0].As(&notifier))
                && SUCCEEDED(notifier->RegisterDestructionCallback([](void*) { displaced_destroyed = true; },
                    nullptr, &callback_id)), "observe final release of the displaced texture");
      notifier.Reset();
      motion[0].Reset();
      Check(!displaced_destroyed.load(), "the deferred reference keeps the displaced texture alive");
      Check(submit(borrowed.Get()), "replay with old texture retained only by the addon");
      wait();
      Check(SUCCEEDED(borrowed_allocator->Reset()) && SUCCEEDED(borrowed->Reset(borrowed_allocator.Get(), nullptr)),
            "borrowed reset");
      submission::OnCommandListReset(borrowed.Get());
      Check(SUCCEEDED(borrowed->Close()), "close empty borrowed list");
      for (auto& pass : passes) {
        for (auto*& buffer : pass.look_history.buffers) native::ReleaseCom(buffer);
      }
      native::ReleaseCodecPipeline();
      native::workset_free_policy = 1u;
      native::enabled = false;
      native::disable_release_mode = 1u;
    }
    native::RunLifecycleTick(native::SteadyNowNs(), false);
    Check(native::retired_look_motion_views.empty() && displaced_destroyed.load()
              && gpu.device->GetDeviceRemovedReason() == S_OK,
          "normal lifecycle releases the last texture reference while keeping worksets and bypassing NR");
    std::cout << "Look motion ring WARP contract passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
