#pragma once

namespace native_mfg_ordering_cases {

inline ID3D12Resource* consumed_hudless = nullptr;

inline NVSDK_NGX_Result NVSDK_CONV Consume(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                           const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback) {
  consumed_hudless = native::PreFgResource(parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess);
  return NVSDK_NGX_Result_Success;
}

inline int Run() {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    Gpu gpu;
    model_gpu = &gpu;
    native::enabled.store(true);
    native::hooks_enabled.store(true);
    native::hook_point.store(native::kHookPresent);
    native::present_pre_fg.store(1);
    native::present_guides.store(native::kPresentGuidesNever);
    native::stack_passes.store(1);
    native::codec_mode.store(0);
    native::transfer_strength.store(1.f);
    native::color_strength.store(1.f);
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = CreateModel;
    native::direct_api.evaluate = EvaluateModel;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::parameter_runtime_ready = true;
    native::core_allocate_parameters = renodx::addons::dlss5::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = renodx::addons::dlss5::bridge::DestroyOwnedParameters;
    native::ngx_slot_real[1].evaluate = reinterpret_cast<void*>(&Consume);
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "Legacy MFG queue completion hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "Legacy MFG producer state hooks");

    auto backbuffer = gpu.Texture();
    auto hudless = gpu.Texture();
    gpu.Write(backbuffer.Get(), 64);
    gpu.Write(hudless.Get(), 96);
    gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    gpu.Transition(hudless.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    auto& owner = native::present_streams[nullptr];
    owner.device_identity = renodx::utils::directx::NativeIdentity(gpu.device.Get());
    owner.backbuffer_width = owner.backbuffer_height = 8;
    owner.backbuffer_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    owner.prefg_target_valid = true;
    owner.prefg_target.diagnostic.bypass_host_look = true;
    owner.prefg_target.pass_transfer_strength[0] = 1.f;
    owner.prefg_target.pass_color_strength[0] = 1.f;

    renodx::addons::dlss5::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_Backbuffer, backbuffer.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_HUDLess, hudless.Get());
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, 3u);
    parameters.Set(NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, 0u);
    const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xF078});
    {
      const renodx::addons::dlss5::RuntimeLock lock(native::runtime_mutex);
      native::RegisterCreatedFeatureLocked(handle, NVSDK_NGX_Feature_FrameGeneration, &parameters);
    }
    const auto evaluate = [&](ID3D12GraphicsCommandList* list, unsigned long long frame, uint32_t index) {
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, frame);
      parameters.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, index);
      consumed_hudless = nullptr;
      Check(NVSDK_NGX_SUCCEED(native::HookedEvaluateFeatureSlot<1>(list, handle, &parameters, nullptr)),
            "Legacy MFG forwards the SDK result");
      Check(consumed_hudless != nullptr
                && native::PreFgResource(&parameters, NVSDK_NGX_DLSSG_Parameter_HUDLess) == hudless.Get(),
            "Each legacy MFG call reaches the SDK and restores the caller's binding");
      return consumed_hudless;
    };

    ComPtr<ID3D12CommandAllocator> consumer_allocator;
    ComPtr<ID3D12GraphicsCommandList> consumer;
    Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&consumer_allocator)))
              && SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                  consumer_allocator.Get(), nullptr, IID_PPV_ARGS(&consumer))), "Legacy MFG separate consumer list");
    Check(native::InstallCommandListStateHooks(consumer.Get()), "Legacy MFG consumer state hooks");
    const auto execute = [&](ID3D12GraphicsCommandList* list, bool lose_first_signal = false) {
      ID3D12CommandList* commands[]{list};
      if (lose_first_signal) {
        // Execute real GPU work while attaching the same missing proof as a failed tracker Signal.
        const auto pending = submission::BeginSubmission(1, commands);
        Check(pending.size() == 1, "Reserve the exact faulted first Execute");
        reinterpret_cast<native::QueueExecuteCommandListsFn>(native::real_queue_execute_command_lists)(gpu.queue.Get(), 1, commands);
        submission::CompleteSubmission(pending, nullptr, 0);
      } else {
        gpu.queue->ExecuteCommandLists(1, commands);
      }
      const auto event = std::unique_ptr<void, decltype(&CloseHandle)>(CreateEventW(nullptr, FALSE, FALSE, nullptr), CloseHandle);
      Check(event != nullptr && SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial))
                && SUCCEEDED(gpu.fence->SetEventOnCompletion(gpu.serial, event.get()))
                && WaitForSingleObject(event.get(), 10000) == WAIT_OBJECT_0
                && gpu.fence->GetCompletedValue() == gpu.serial, "Legacy MFG test fence completes actual GPU work");
    };
    const auto recycle = [&](ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator) {
      Check(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator, nullptr)), "Legacy MFG host list Reset");
      const renodx::addons::dlss5::RuntimeLock lock(native::runtime_mutex);
      native::PruneCompletedSubmissionUses();
    };
    const auto capture = [&](ID3D12GraphicsCommandList* list) {
      auto image = gpu.CreateTextureTransfer(backbuffer.Get());
      const D3D12_TEXTURE_COPY_LOCATION source{.pResource = backbuffer.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      const D3D12_TEXTURE_COPY_LOCATION target{.pResource = image.readback.Get(),
          .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = image.footprint};
      native::Transition(list, backbuffer.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
      native::Transition(list, backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
      return image;
    };
    Check(SUCCEEDED(gpu.list->Close()), "Legacy MFG input seed closes");
    execute(gpu.list.Get());
    recycle(gpu.list.Get(), gpu.allocator.Get());

    unsigned long long frame = 1;
    ID3D12Resource* enhanced = nullptr;
    for (; frame <= 8; ++frame) {
      enhanced = evaluate(gpu.list.Get(), frame, 1);
      if (enhanced != hudless.Get()) break;
      Check(SUCCEEDED(gpu.list->Close()), "Legacy MFG warmup closes");
      execute(gpu.list.Get());
      recycle(gpu.list.Get(), gpu.allocator.Get());
    }
    Check(enhanced != nullptr && enhanced != hudless.Get(), "Legacy first delivery records a mature private color");
    const auto calls = model_calls;
    Check(evaluate(consumer.Get(), frame, 2) == hudless.Get() && model_calls == calls,
          "List B cannot substitute an unsubmitted List A result");
    auto image = capture(consumer.Get());
    Check(SUCCEEDED(consumer->Close()), "Legacy MFG consumer closes before the producer");
    execute(consumer.Get());
    const auto raw = gpu.ReadTexture(image);
    Check(raw == std::vector<uint8_t>(256, 64), "List B submitted first observes and leaves raw RGBA");
    recycle(consumer.Get(), consumer_allocator.Get());

    Check(evaluate(gpu.list.Get(), frame, 2) == enhanced && model_calls == calls,
          "The open producing recording may re-deliver in command order with zero extra Evaluate");
    image = capture(gpu.list.Get());
    Check(SUCCEEDED(gpu.list->Close()), "Legacy MFG producer closes");
    execute(gpu.list.Get());
    const auto edited = gpu.ReadTexture(image);
    Check(edited != raw && edited.size() == raw.size(), "The ordered same-list delivery changes RGB");
    for (size_t alpha = 3; alpha < edited.size(); alpha += 4) {
      Check(edited[alpha] == raw[alpha], "Legacy MFG preserves alpha");
    }
    Check(evaluate(consumer.Get(), frame, 2) == hudless.Get() && model_calls == calls,
          "GPU completion alone cannot authorize re-delivery from a replayable producer");
    recycle(gpu.list.Get(), gpu.allocator.Get());
    Check(evaluate(consumer.Get(), frame, 2) == enhanced && model_calls == calls,
          "A successful first receipt survives Reset and prune and authorizes cross-list re-delivery");
    image = capture(consumer.Get());
    Check(SUCCEEDED(consumer->Close()), "Legacy MFG ordered consumer closes");
    execute(consumer.Get());
    Check(gpu.ReadTexture(image) == edited, "Retired producer gives the same edited RGBA to the separate consumer");
    recycle(consumer.Get(), consumer_allocator.Get());

    ++frame;
    gpu.Transition(backbuffer.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
    gpu.Write(backbuffer.Get(), 64);
    gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    Check(evaluate(gpu.list.Get(), frame, 1) != hudless.Get() && model_calls > calls,
          "A genuine subsequent legacy frame still evaluates and delivers NR");
    const auto fault_recording = submission::RecordingIdentity(gpu.list.Get());
    image = capture(gpu.list.Get());
    Check(SUCCEEDED(gpu.list->Close()), "Legacy MFG faulted producer closes");
    execute(gpu.list.Get(), true);
    Check(gpu.ReadTexture(image) == edited, "Losing Signal evidence leaves the first actual edited RGBA observable");
    execute(gpu.list.Get());
    std::vector<submission::RecordingBlocker> blockers;
    submission::InspectRecordedWork([&](uint64_t list, uint64_t generation, const submission::Generation&) {
      return std::pair{list, generation} == fault_recording;
    }, &blockers);
    Check(blockers.size() == 1 && blockers.front().first_submission.state == submission::SubmissionProofState::kSignalFailed
              && blockers.front().submissions.size() == 2
              && blockers.front().submissions.back().state == submission::SubmissionProofState::kComplete,
          "Actual replay completes but cannot repair the first Signal receipt");
    recycle(gpu.list.Get(), gpu.allocator.Get());

    auto producer = std::exchange(gpu.list, consumer);
    gpu.Transition(backbuffer.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
    gpu.Write(backbuffer.Get(), 112);
    gpu.Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    gpu.list = std::move(producer);
    const auto fault_calls = model_calls;
    Check(evaluate(consumer.Get(), frame, 2) == hudless.Get() && model_calls == fault_calls,
          "Reset and a healthy replay leave first-Signal loss unproven with zero extra Evaluate");
    image = capture(consumer.Get());
    Check(SUCCEEDED(consumer->Close()), "Legacy MFG first-Signal recovery probe closes");
    execute(consumer.Get());
    Check(gpu.ReadTexture(image) == std::vector<uint8_t>(256, 112), "A faulted producer cannot replace current raw RGBA");
    std::cout << "NR_RELEASE_PARITY native-prefg-present-x2 legacy-mfg-ordering"
                 " unsubmitted-consumer-first=raw-rgba,zero-evaluate open-replayable=original-binding"
                 " same-recording=edited-rgba,zero-evaluate retired-producer=edited-rgba,zero-evaluate"
                 " first-signal-loss-after-replay=raw-rgba,zero-evaluate next-base=edited\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL legacy Native MFG ordering: " << error.what() << '\n';
    return 1;
  }
}

}  // namespace native_mfg_ordering_cases
