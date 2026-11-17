/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
#include <array>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <cstring>
#include <vector>
#include <fstream>
#include <iterator>
#include <d3d12sdklayers.h>
#include <screenshot.hpp>
#include "d3d12_test_gpu.hpp"

#include <nr_xefg_present_probe.hpp>

namespace nr_xefg_present_probe_cases {
namespace probe = renodx::addons::dlss5::xefg_present_probe;
inline int Run() {
  struct Restore {
    probe::Snapshot saved = probe::CopySnapshot();
    probe::Calibration saved_calibration = probe::CopyCalibration();
    probe::CaptureCallbacks saved_callbacks = probe::capture_callbacks;
    probe::ControlJob saved_control = probe::CopyControl();
    Restore() {
      std::unique_lock lock(probe::probe_state.mutex);
      probe::probe_state.snapshot = {};
      probe::calibration = {};
      probe::control_job = {};
    }
    ~Restore() {
      const auto deadline = GetTickCount64() + 5000;
      while (probe::ControlBusy(probe::CopyControl()) && GetTickCount64() < deadline) {
        Sleep(1);
      }
      probe::Snapshot retired;
      std::unique_lock lock(probe::probe_state.mutex);
      retired = std::move(probe::probe_state.snapshot);
      probe::probe_state.snapshot = saved;
      probe::calibration = saved_calibration;
      probe::capture_callbacks = saved_callbacks;
      probe::control_job = saved_control;
    }
  } restore;
  try {
    uint32_t checks = 0;
    const auto check = [&](bool value, const char* message) {
      ++checks;
      if (!value) {
        throw std::runtime_error(message);
      }
    };
    const probe::InitKey key{.slot = 1, .module = 0x1000, .handle = 0x2000, .factory = 0x3000, .window = 0x4000};
    const probe::Candidate candidate{.swapchain = 0x5000, .queue = 0x6000, .device = 0x7000,
        .method_module = 0x8000, .buffer_count = 2, .hooks_ready = true};
    const auto begin = [&](const probe::InitKey& input = probe::InitKey{}) {
      const auto serial = probe::BeginInit(input.handle != 0 ? input : key);
      std::unique_lock lock(probe::probe_state.mutex);
      if (serial != 0) {
        probe::probe_state.snapshot.pending.factory_hooks_ready = true;
      }
      return serial;
    };
    const auto context = [&](uint64_t serial) { probe::ContextInitialized(serial, 0x9000, 1, serial); };
    const auto created = [&](uint64_t serial, const probe::Candidate& value, int32_t result = S_OK) {
      const auto ticket = probe::BeginCreate(key.factory, key.window);
      check(ticket == serial, "the exact pending Init owns this factory callback");
      probe::FinishCreate(ticket, result, value, 2);
    };
    const auto binding = [&](uint64_t serial) {
      const auto snapshot = probe::CopySnapshot();
      for (const auto& value : snapshot.bindings) {
        if (value.init_serial == serial) return value;
      }
      return probe::Binding{};
    };

    auto serial = begin();
    check(probe::BeginCreate(key.factory + 1, key.window) == 0
            && probe::BeginCreate(key.factory, key.window + 1) == 0,
        "wrong factory and HWND cannot contribute a candidate");
    created(serial, candidate, E_FAIL);
    context(serial);
    probe::FinishInit(serial, 0);
    check(!binding(serial).bound && binding(serial).created.queue == 0,
        "a failed native creation cannot commit its requested queue");
    serial = begin();
    created(serial, candidate);
    context(serial);
    probe::FinishInit(serial, -31);
    check(!binding(serial).bound && !binding(serial).live, "an SDK Init failure cannot publish a successful native create");
    serial = begin();
    created(serial, candidate);
    probe::FinishInit(serial, 0);
    check(!binding(serial).bound, "a create without the actual final context generation remains blocked");
    serial = begin();
    created(serial, candidate);
    auto second = candidate;
    second.swapchain += 1;
    created(serial, second);
    context(serial);
    probe::FinishInit(serial, 0);
    check(!binding(serial).bound, "two distinct successful outputs cannot be selected by recency");
    serial = begin();
    check(probe::BeginInit({.slot = 0, .module = 0x1100, .handle = 0x2200, .factory = 0x3300, .window = 0x4400}) == 0,
        "overlapping independent Init marks the sole pending transaction ambiguous");
    created(serial, candidate);
    context(serial);
    probe::FinishInit(serial, 0);
    check(!binding(serial).bound, "an overlapping Init cannot inherit a later unique-looking candidate");

    serial = begin();
    created(serial, candidate);
    std::array<probe::QueueIdentity, probe::kMaxBuffers> init_queues{};
    init_queues[0] = {.queue = 0x6010, .device = candidate.device};
    init_queues[1] = {.queue = 0x6020, .device = candidate.device};
    auto init_resize = probe::BeginResize(candidate.swapchain);
    check(init_resize.pending, "a native Resize before Init returns belongs to its pending creation transaction");
    probe::FinishResize(init_resize, E_FAIL, 2, init_queues, true);
    check(probe::CopySnapshot().pending.queues[0] == candidate.queue,
        "failed in-Init Resize cannot replace the proven creation queue");
    init_resize = probe::BeginResize(candidate.swapchain);
    probe::FinishResize(init_resize, S_OK, 2, init_queues, true);
    context(serial);
    probe::FinishInit(serial, 0);
    check(binding(serial).bound && binding(serial).created.queue == candidate.queue
            && binding(serial).queues[0] == 0x6010 && binding(serial).queues[1] == 0x6020,
        "successful in-Init Resize publishes its actual queue map only with successful outer Init");

    serial = begin();
    // A factory may delegate CreateSwapChain to ForHwnd. Both successful
    // callbacks name the same native object/queue, rather than two candidates.
    std::thread worker([&] { probe::FinishCreate(probe::BeginCreate(key.factory, key.window), S_OK, candidate, 2); });
    worker.join();
    created(serial, candidate);
    check(probe::CopySnapshot().pending.candidates == 1, "same-object nested factory returns deduplicate across threads");
    context(serial);
    probe::FinishInit(serial, 0);
    check(binding(serial).bound && binding(serial).queues[1] == candidate.queue,
        "successful outer Init commits its exact context and native creation queue");
    check(probe::BeginPresent(candidate.swapchain + 1, 0, 1, 0).serial == 0,
        "an unrelated swapchain cannot enter the bound native observation");
    const auto test = probe::BeginPresent(candidate.swapchain, 0, 1, DXGI_PRESENT_TEST);
    probe::FinishPresent(test, S_OK);
    check(test.serial == 0 && binding(serial).test_calls == 1 && binding(serial).present_calls == 0,
        "TEST is counted separately and never produces an output-call serial");
    auto present = probe::BeginPresent(candidate.swapchain, 1, 2, 0);
    probe::FinishPresent(present, S_FALSE);
    check(present.queue == candidate.queue && binding(serial).last_present_result == S_FALSE
            && binding(serial).successful_present_calls == 1,
        "native call metadata preserves its original HRESULT without a completion claim");
    probe::FinishPresent(probe::BeginPresent(candidate.swapchain, 1, 1, 0), E_FAIL);
    check(binding(serial).successful_present_calls == 1 && binding(serial).last_present_result == E_FAIL,
        "a failed native Present does not add an accepted API call");
    check(probe::BeginPresent(candidate.swapchain, 99, 1, 0).queue == 0,
        "an unknown buffer index cannot manufacture a queue");
    const auto old_present = probe::BeginPresent(candidate.swapchain, 0, 1, 0);
    std::array<probe::QueueIdentity, probe::kMaxBuffers> queues{};
    queues[0] = {.queue = 0x6100, .device = candidate.device};
    queues[1] = {.queue = 0x6200, .device = candidate.device};
    auto resize = probe::BeginResize(candidate.swapchain);
    check(probe::BeginPresent(candidate.swapchain, 0, 1, 0).serial == 0, "Resize excludes simultaneous output admission");
    probe::FinishResize(resize, E_FAIL, 2, queues, true);
    check(binding(serial).bound && binding(serial).queues[0] == candidate.queue,
        "failed ResizeBuffers1 preserves the proven queue and rejects request values");
    resize = probe::BeginResize(candidate.swapchain);
    probe::FinishResize(resize, S_OK, 2, queues, true);
    check(binding(serial).bound && binding(serial).queues[0] == 0x6100 && binding(serial).queues[1] == 0x6200,
        "only successful ResizeBuffers1 commits each actual buffer queue");
    const auto stale_before = probe::CopySnapshot().stale_returns;
    probe::FinishPresent(old_present, S_OK);
    check(probe::CopySnapshot().stale_returns == stale_before + 1, "a pre-Resize call return cannot update a new buffer generation");
    resize = probe::BeginResize(candidate.swapchain);
    probe::FinishResize(resize, S_OK, 2, {}, false);
    check(binding(serial).bound && binding(serial).queues[1] == 0x6200,
        "plain ResizeBuffers retains a proven unchanged per-buffer mapping");
    queues[1].device += 1;
    resize = probe::BeginResize(candidate.swapchain);
    probe::FinishResize(resize, S_OK, 2, queues, true);
    check(!binding(serial).bound, "a queue from another device cannot establish a new output binding");

    serial = begin();
    created(serial, candidate);
    context(serial);
    probe::FinishInit(serial, 0);
    present = probe::BeginPresent(candidate.swapchain, 0, 1, 0);
    probe::UpdateContext(0x9000, 2, 123);
    check(binding(serial).context_generation == 123 && binding(serial).context_epoch == 2,
        "environment changes advance the same context metadata and invalidate old tickets");
    probe::Invalidate(key.slot, key.handle, "context_destroying");
    check(!binding(serial).live && probe::BeginPresent(candidate.swapchain, 0, 1, 0).serial == 0,
        "Destroy revokes observation before calling the SDK");
    const auto old_serial = serial;
    serial = begin();
    created(serial, candidate);
    context(serial);
    probe::FinishInit(serial, 0);
    probe::FinishPresent(present, S_OK);
    check(serial != old_serial && binding(serial).bound && binding(serial).successful_present_calls == 0,
        "reinitialized same handle/object addresses never accept an older context's return");
    for (uint32_t index = 0; index < 100; ++index) {
      probe::FinishPresent(probe::BeginPresent(candidate.swapchain, index % 2, 1, 0), S_OK);
    }
    check(binding(serial).present_calls == 100 && probe::CopySnapshot().events.size() == 24,
        "long runs retain bounded events and independent API-call counters");
    auto existing = key;
    existing.factory = 0;
    existing.application_swapchain = candidate.swapchain;
    serial = begin(existing);
    context(serial);
    probe::FinishInit(serial, 0);
    check(!binding(serial).bound && binding(serial).created.queue == 0,
        "InitFromSwapChain never borrows its application queue as native creation proof");
    serial = begin();
    const auto inflight = probe::BeginCreate(key.factory, key.window);
    context(serial);
    probe::FinishInit(serial, 0);
    probe::FinishCreate(inflight, S_OK, candidate, 1);
    check(!binding(serial).bound, "Init returning before a worker create cannot retroactively bind its late result");
    serial = begin();
    const auto stopping = probe::BeginCreate(key.factory, key.window);
    probe::StopObservation();
    probe::FinishCreate(stopping, S_OK, candidate, 2);
    probe::FinishInit(serial, 0);
    check(probe::CopySnapshot().stopped && probe::BeginInit(key) == 0
            && probe::BeginPresent(candidate.swapchain, 0, 1, 0).serial == 0,
        "Stop invalidates pending and live generations without calling SDK debug or GPU work");
    {
      std::unique_lock lock(probe::probe_state.mutex);
      probe::probe_state.snapshot = {};
      probe::calibration = {};
    }
    static int32_t debug_result = 7;
    static std::atomic_uint32_t debug_calls = 0, last_enabled = 99, debug_thread = 0;
    static std::atomic_bool debug_block = false;
    static bool tag_only = true;
    auto debug_key = key;
    debug_key.module_path = "SYNTHETIC_XEFG_MODULE";
    debug_key.debug = +[](void*, int32_t feature, uint32_t enabled, void*) {
      ++debug_calls;
      tag_only &= feature == 1;
      last_enabled = enabled;
      debug_thread = GetCurrentThreadId();
      const auto deadline = GetTickCount64() + 2000;
      while (debug_block.load() && GetTickCount64() < deadline) {
        Sleep(1);
      }
      return debug_result;
    };
    serial = begin(debug_key);
    created(serial, candidate);
    context(serial);
    probe::FinishInit(serial, 0);
    probe::Target target{.slot = key.slot, .module = key.module, .handle = key.handle,
        .init_serial = serial, .context = 0x9000, .context_generation = serial, .buffer_generation = 1};
    namespace shot = renodx::addons::dlss5::screenshot;
    uint64_t request_sequence = 0;
    const auto wait_control = [&](uint64_t job_id) {
      const auto deadline = GetTickCount64() + 5000;
      for (;;) {
        const auto value = probe::CopyControl();
        if (value.job_id == job_id && value.finished) return value;
        if (GetTickCount64() >= deadline) {
          check(false, "the bounded substitute control job must return");
        }
        Sleep(1);
      }
    };
    const auto control = [&](probe::ControlAction action, bool enabled = false, uint32_t samples = 8, uint32_t duration = 1000) {
      const auto job_id = probe::QueueControl("cpu-control-" + std::to_string(++request_sequence), action, target, enabled, samples, duration);
      check(job_id != 0 && shot::ScheduleXefgControl(), "control is queued on the existing screenshot worker");
      return wait_control(job_id).accepted;
    };
    check(!probe::EstablishBaseline(target), "Present-side direct SDK entry is rejected");
    debug_block = true;
    const auto blocked_id = probe::QueueControl("blocked-baseline", probe::ControlAction::BASELINE, target);
    check(blocked_id != 0 && shot::ScheduleXefgControl(), "a blocking SDK substitute does not block queue submission");
    const auto entry_deadline = GetTickCount64() + 1000;
    while (!probe::CopyControl().sdk_entered && GetTickCount64() < entry_deadline) {
      Sleep(1);
    }
    const auto entered = probe::CopyControl();
    const auto blocked_status = renodx::addons::dlss5::internal::dev_control::Execute({{"id", "blocked-status"},
        {"op", "xefg_present_probe"}, {"action", "status"}});
    check(entered.sdk_entered && !entered.sdk_returned && !entered.finished
            && entered.worker_thread != GetCurrentThreadId() && entered.sdk_entered_thread == entered.worker_thread
            && blocked_status["probe"]["control"]["job_id"] == blocked_id,
        "status returns during SDK entry and records the actual independent worker thread");
    check(probe::QueueControl("blocked-baseline", probe::ControlAction::BASELINE, target) == blocked_id,
        "duplicate request IDs return the original control job without reentering SDK");
    check(probe::QueueControl("cancel-entered", probe::ControlAction::STOP, target) == blocked_id
            && !probe::CopyControl().finished, "entered SDK calls remain in flight after cancellation");
    debug_block = false;
    const auto returned = wait_control(blocked_id);
    check(returned.sdk_returned && returned.api_result_available && returned.result == 7
            && returned.sdk_returned_thread == entered.worker_thread && debug_calls == 1 && debug_thread == entered.worker_thread,
        "exact SDK entry and return receipts preserve warning success and a single invocation");
    const auto rejected_calls = debug_calls.load();
    for (uint32_t rejection = 0; rejection < 3; ++rejection) {
      const auto job_id = probe::QueueControl("pre-entry-" + std::to_string(rejection), probe::ControlAction::BASELINE, target);
      {
        std::unique_lock lock(probe::probe_state.mutex);
        if (rejection == 0) {
          probe::control_job.cancel_requested = true;
        } else if (rejection == 1) {
          probe::control_job.deadline_ms = 0;
        } else {
          ++probe::control_job.target.context_generation;
        }
      }
      check(job_id != 0 && shot::ScheduleXefgControl(), "a rejected pre-entry job is still acknowledged asynchronously");
      const auto rejected = wait_control(job_id);
      const auto expected = rejection == 0 ? probe::ControlPhase::CANCELLED
          : rejection == 1 ? probe::ControlPhase::EXPIRED : probe::ControlPhase::REJECTED;
      check(!rejected.sdk_entered && !rejected.accepted && rejected.phase == expected && debug_calls == rejected_calls,
          "cancelled, expired and old-generation work never enters SDK");
    }
    check(control(probe::ControlAction::BASELINE) && probe::CopyCalibration().baseline_result == 7 && last_enabled == 0,
        "explicit false establishes a known test baseline and preserves positive SDK warning success");
    const auto calls_before_status = debug_calls.load();
    const auto status = renodx::addons::dlss5::internal::dev_control::Execute({{"id", "xefg-status-cpu"},
        {"op", "xefg_present_probe"}, {"action", "status"}});
    check(status["state"].contains("pid") && status["state"].contains("process_created")
            && status["probe"]["bindings"][0]["module_path"] == debug_key.module_path
            && status.dump().size() < 65536 && debug_calls == calls_before_status,
        "metadata status remains read-only, compact, process-bound and tied to the observed module path");
    static_assert(probe::kMaxBuffers == 8 && probe::kMaxSamplesPerWindow == 64);
    const auto calls_before_bounds = debug_calls.load();
    for (const auto [samples, duration] : std::array<std::array<uint32_t, 2>, 4>{{
             {0, 1000}, {65, 1000}, {64, 99}, {64, 3001}}}) {
      check(!control(probe::ControlAction::ARM, true, samples, duration)
              && debug_calls == calls_before_bounds && !probe::CopyCalibration().active,
          "zero/65 samples and out-of-range duration remain rejected before SDK entry");
    }
    check(control(probe::ControlAction::ARM, true, 64, 3000) && probe::CopyCalibration().requested == 64,
        "64 samples and the existing maximum duration are accepted in one continuous TAG window");
    check(control(probe::ControlAction::STOP) && !probe::CopyCalibration().restore_pending,
        "the larger window still uses the existing explicit baseline restoration");
    check(control(probe::ControlAction::ARM, true, 1, 100) && probe::CopyCalibration().requested == 1,
        "one sample and the existing minimum duration remain accepted");
    check(control(probe::ControlAction::STOP), "the minimum window restores its known baseline");
    check(control(probe::ControlAction::ARM, true) && last_enabled == 1, "TAG-only On arm preserves the controlled baseline");
    check(!control(probe::ControlAction::BASELINE), "an active TAG window cannot replace its baseline");
    {
      std::unique_lock lock(probe::probe_state.mutex);
      probe::calibration.active = false;
      probe::calibration.restore_pending = true;
    }
    check(!control(probe::ControlAction::BASELINE), "an inactive window with pending restoration cannot overwrite its baseline");
    check(control(probe::ControlAction::POLL) && !probe::CopyCalibration().restore_pending && !probe::CopyCalibration().tag_enabled
            && probe::CopyCalibration().restore_result == 7 && last_enabled == 0,
        "explicit poll restores known false while retaining the actual warning result");
    debug_result = -91;
    check(!control(probe::ControlAction::ARM, true, 1), "failed SDK TAG call cannot arm a capture");
    debug_result = 7;
    check(control(probe::ControlAction::POLL) && tag_only, "failed arm also restores only TAG, never SHOW");
    const std::array<shot::CaptureRect, 2> rois{{{.x = 256, .y = 256, .width = 128, .height = 64},
        {.x = 0, .y = 0, .width = 256, .height = 128}}};
    check(shot::ValidateXefgRois(rois), "two different bounded ROI dimensions are supported");
    auto overlap = rois;
    overlap[1] = overlap[0];
    check(!shot::ValidateXefgRois(overlap), "overlapping ROI requests are rejected");
    probe::CopyContext invalid{.target = target, .window_generation = probe::CopyCalibration().generation,
        .swapchain = candidate.swapchain, .queue = candidate.queue + 1, .index = 0};
    check(!probe::FrameCurrent(invalid), "wrong queue cannot validate an output sample");
    invalid.queue = candidate.queue;
    ++invalid.target.context_generation;
    check(!probe::FrameCurrent(invalid), "wrong context generation cannot validate an output sample");
    std::cout << "PASS xefg-present-probe CPU/MOCK checks=" << checks << " evidence=" << probe::kEvidence << '\n';

    // WARP exercises the production PRESENT copy boundary on an owned texture.
    // SDK, context routing and original Present results remain CPU substitutes.
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    Microsoft::WRL::ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
    }
    d3d12_test::Gpu gpu(false);
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> info;
    gpu.device.As(&info);
    check(info != nullptr, "WARP dual ROI requires Debug Layer messages");
    const auto source = gpu.Texture(1024, 768, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_PRESENT);
    auto transfer = gpu.CreateTextureTransfer(source.Get());
    std::vector<uint8_t> input(1024 * 768 * 8);
    for (size_t index = 0; index < input.size(); ++index) {
      input[index] = static_cast<uint8_t>(index * 17 + index / 67);
    }
    gpu.Transition(source.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu.UploadTexture(source.Get(), transfer, input.data(), input.size(), 1024 * 8);
    gpu.Transition(source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    gpu.Submit();
    auto owner = std::make_shared<probe::QueueOwner>();
    owner->queue = gpu.queue;
    owner->device = gpu.device;
    auto warp_candidate = candidate;
    warp_candidate.queue = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(gpu.queue.Get()));
    warp_candidate.device = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(gpu.device.Get()));
    warp_candidate.owner = owner;
    serial = begin(debug_key);
    created(serial, warp_candidate);
    context(serial);
    probe::FinishInit(serial, 0);
    target.init_serial = target.context_generation = serial;
    target.buffer_generation = 1;
    check(control(probe::ControlAction::BASELINE), "WARP fixture binds a synthetic SDK baseline");
    const auto directory = std::filesystem::current_path() / ("xefg-present-evidence-" + std::to_string(GetCurrentProcessId()));
    check(std::filesystem::create_directory(directory), "WARP writer owns a new empty output leaf");
    std::cout << "XEFG_PRESENT_WRITER_OUTPUT " << directory.string() << '\n';
    check(shot::RequestXefgCapture(directory, "warp_memory_only", rois, 1),
        "WARP fixture requests a local-only dual ROI capture");
    check(shot::ReserveXefgRecording() && !shot::ReserveXefgRecording(), "busy reservation prevents duplicate GPU object creation");
    auto packet = std::make_shared<probe::CopyPacket>();
    packet->owner = owner;
    check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&packet->allocator)))
            && SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, packet->allocator.Get(), nullptr, IID_PPV_ARGS(&packet->list)))
            && SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&packet->fence))), "WARP copy packet");
    probe::CopyContext copied{.target = target, .window_generation = probe::CopyCalibration().generation, .native_serial = 41,
        .swapchain = warp_candidate.swapchain, .queue = warp_candidate.queue, .buffer = reinterpret_cast<uintptr_t>(source.Get()),
        .index = 0, .width = 1024, .height = 768, .format = DXGI_FORMAT_R16G16B16A16_FLOAT};
    const auto capture_serial = shot::PrepareXefgPresent(copied, packet, source.Get());
    shot::ReleaseXefgRecording();
    check(capture_serial != 0, "production presenter records same-resource different-size ROI copies");
    check(SUCCEEDED(packet->list->Close()), "WARP copy close");
    Microsoft::WRL::ComPtr<ID3D12Fence> gate;
    check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))
            && SUCCEEDED(gpu.queue->Wait(gate.Get(), 1)), "WARP delayed completion gate");
    struct ReleaseGate {
      ID3D12Fence* fence;
      ~ReleaseGate() {
        if (fence != nullptr) {
          fence->Signal(1);
        }
      }
    } release_gate{gate.Get()};
    ID3D12CommandList* lists[]{packet->list.Get()};
    const auto reservations = renodx::addons::dlss5::submission::BeginSubmission(1, lists);
    gpu.queue->ExecuteCommandLists(1, lists);
    packet->signal_result = gpu.queue->Signal(packet->fence.Get(), 1);
    check(SUCCEEDED(packet->signal_result), "WARP exact copy Signal");
    packet->submitted = packet->signaled = true;
    renodx::addons::dlss5::submission::CompleteSubmission(reservations, packet->fence.Get(), 1);
    packet->Poll();
    check(!packet->IsCompleted() && !packet->IsRetired(), "an incomplete Fence cannot release or validate the capture");
    check(SUCCEEDED(gate->Signal(1)), "release WARP test gate");
    release_gate.fence = nullptr;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(event != nullptr && SUCCEEDED(packet->fence->SetEventOnCompletion(1, event)), "WARP fixture Fence event");
    const auto waited = WaitForSingleObject(event, 10000);
    CloseHandle(event);
    check(waited == WAIT_OBJECT_0, "bounded test-only wait, never used by the production Present callback");
    packet->Poll();
    check(packet->IsCompleted() && packet->IsRetired(), "completed packet discards replayable recording before retirement");
    shot::ReturnXefgPresent(capture_serial, S_FALSE);
    shot::PreserveStageSubmissionProofs();
    renodx::addons::dlss5::submission::PruneCompletedGenerations();
    std::optional<shot::internal::PendingCapture> export_capture;
    {
      std::scoped_lock lock(shot::internal::state_mutex);
      auto& capture = *shot::internal::pending;
      check(shot::internal::StageSubmissionCompleted(&capture) && shot::internal::ReadbacksReleasable(capture),
          "both readbacks have exact completed, non-replayable submission proof");
      const std::array<ID3D12Resource*, 2> readbacks{capture.pre_nr_readback, capture.nr_output_readback};
      for (size_t plane = 0; plane < 2; ++plane) {
        const auto roi = rois[plane];
        void* mapped = nullptr;
        check(SUCCEEDED(readbacks[plane]->Map(0, nullptr, &mapped)), "WARP ROI map");
        bool equal = true;
        for (uint32_t row = 0; row < roi.height; ++row) {
          equal &= std::memcmp(static_cast<const uint8_t*>(mapped) + capture.xefg_present->footprints[plane].Offset
                  + uint64_t(row) * capture.xefg_present->footprints[plane].Footprint.RowPitch,
              input.data() + (size_t(roi.y + row) * 1024 + roi.x) * 8, roi.width * 8) == 0;
        }
        readbacks[plane]->Unmap(0, nullptr);
        check(equal, "different-size ROI bytes equal their exact source rectangles");
      }
      capture.lease_taken = capture.exact = capture.completion_proven = true;
      capture.completion_generation = 2;
      export_capture = std::move(capture);
      shot::internal::pending.reset();
      shot::internal::has_pending.store(false);
    }
    shot::internal::outstanding_jobs.fetch_add(1);
    shot::internal::worker_busy.store(true);
    // Execute the same production worker job on this offline test thread.
    // The native Present callback always hands off and never calls ProcessJob.
    shot::internal::ProcessJob(std::move(export_capture));
    const auto stem = "warp_memory_only_" + std::to_string(capture_serial);
    const auto meta_path = directory / (stem + ".meta.txt");
    std::ifstream meta_file(meta_path, std::ios::binary);
    const std::string metadata{std::istreambuf_iterator<char>(meta_file), std::istreambuf_iterator<char>()};
    check(metadata.find("capture_schema=xefg_native_present_roi_v1\n") != std::string::npos
            && metadata.find("native_present_serial=41\n") != std::string::npos
            && metadata.find("buffer=" + std::to_string(copied.buffer) + "\n") != std::string::npos
            && metadata.find("capture_serial=" + std::to_string(capture_serial) + "\n") != std::string::npos
            && metadata.find("api_result=1\n") != std::string::npos
            && metadata.find("raw_copy_valid=1\n") != std::string::npos
            && metadata.find("classification=Unknown\n") != std::string::npos
            && metadata.find("marker_contribution=NotMeasured\n") != std::string::npos,
        "production writer binds both ROI to the same buffer/serial/HRESULT without classifying TAG or absent NR markers");
    std::array<std::filesystem::path, 3> outputs{directory / (stem + "_nr_roi.raw"), directory / (stem + "_sdk_roi.raw"), meta_path};
    for (size_t plane = 0; plane < 2; ++plane) {
      const auto roi = rois[plane];
      std::ifstream file(outputs[plane], std::ios::binary);
      const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
      bool equal = bytes.size() == size_t(roi.width) * roi.height * 8;
      for (uint32_t row = 0; row < roi.height && equal; ++row) {
        equal = std::memcmp(bytes.data() + size_t(row) * roi.width * 8,
            input.data() + (size_t(roi.y + row) * 1024 + roi.x) * 8, roi.width * 8) == 0;
      }
      check(equal, "production worker exports each different-size ROI as exact tight native-format bytes");
    }
    check(shot::CopyXefgCaptureStatus().written == 1 && shot::CopyXefgCaptureStatus().blocked == 0,
        "writer terminal counters identify one actual completed export");
    meta_file.close();
    for (const auto& file : outputs) {
      check(std::filesystem::remove(file), "remove only the fixture's three known output files");
    }
    check(std::filesystem::remove(directory), "remove the fixture's own empty output leaf");
    shot::CloseXefgRequest();
    check(shot::CopyXefgCaptureStatus().drained && !shot::internal::stage_busy.load(), "capture gate releases only after the owner drains");
    gpu.Transition(source.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.RecordTextureReadback(source.Get(), transfer);
    gpu.Transition(source.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    gpu.Submit();
    check(gpu.ReadTexture(transfer) == input, "capture restores PRESENT and never writes source pixels");
    for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
      SIZE_T bytes = 0;
      info->GetMessage(index, nullptr, &bytes);
      std::vector<uint8_t> storage(bytes);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
      info->GetMessage(index, message, &bytes);
      check(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR && message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION,
          message->pDescription);
    }
    std::cout << "PASS xefg-present-probe WARP dual ROI bytes, PRESENT restore, exact Fence retirement; SDK/Present=MOCK classification=Unknown\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL xefg-present-probe CPU/MOCK: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace nr_xefg_present_probe_cases
#endif
