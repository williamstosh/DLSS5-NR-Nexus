/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>

#include <d3d12sdklayers.h>
#include <dxgi1_4.h>

namespace inline_restore_roi_cases {
namespace addon = renodx::addons::dlss5;
namespace shot = addon::screenshot;

inline int Run() {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    auto project = std::filesystem::current_path();
    while (project.filename() != L"RenoDX-DLSSNR-Nexus" && project.has_parent_path()
           && project.parent_path() != project) project = project.parent_path();
    Check(project.filename() == L"RenoDX-DLSSNR-Nexus", "ROI fixture runs inside the authorized project");
    const auto output_root = project / L"tmp" / L"mafia-same-frame-roi";
    const auto stem = "warp-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    std::vector<std::filesystem::path> owned_outputs;
    const auto directory = [&](const char* suffix) {
      auto path = output_root / (stem + "-" + suffix);
      owned_outputs.push_back(path);
      return path;
    };
    std::string reason;
    Check(!shot::internal::inline_restore_active && !shot::HasPending(), "ROI capture is inert by default");
    Check(!shot::RequestInlineRestoreRoi(L"C:/tmp/mafia-same-frame-roi/x", &reason)
        && reason == "invalid_output_directory", "ROI refuses C drive without fallback");
    Check(!shot::RequestInlineRestoreRoi(L"tmp/mafia-same-frame-roi/x", &reason), "ROI refuses relative paths");
    Check(!shot::RequestInlineRestoreRoi(output_root / L".." / L"escape", &reason), "ROI refuses parent traversal");
    // Own the singleton synchronously so this case can inspect each hand-off before executing ProcessJob.
    shot::internal::worker_started = true;
    shot::InlineRestoreContext contract{
        .d3d11_bridge = true, .upscaled = true, .api_returned = true, .ngx_succeeded = true,
        .stack = 1, .work_width = 3840, .work_height = 2160, .output_width = 3840, .output_height = 2160,
        .output_rect = {.width = 3840, .height = 2160}, .codec = 2,
        .format = DXGI_FORMAT_R16G16B16A16_UNORM, .evaluation_sequence = 7, .source_generation = 11};
    Check(shot::InlineRestoreEligible(contract), "target contract accepted");
    for (const auto& mutate : std::vector<std::function<void(shot::InlineRestoreContext*)>>{
             [](auto* c) { c->d3d11_bridge = false; }, [](auto* c) { c->mixed = true; },
             [](auto* c) { c->present = true; }, [](auto* c) { c->preparing = true; },
             [](auto* c) { c->warmup = true; }, [](auto* c) { c->reset_sent = true; },
             [](auto* c) { c->api_returned = false; }, [](auto* c) { c->ngx_succeeded = false; },
             [](auto* c) { c->upscaled = false; }, [](auto* c) { c->stack = 2; },
             [](auto* c) { c->group_begin = 1; }, [](auto* c) { c->pass = 1; },
             [](auto* c) { c->work_width = 1920; }, [](auto* c) { c->output_height = 1080; },
             [](auto* c) { c->output_rect.x = 1; }, [](auto* c) { c->hdr_mode = 1; },
             [](auto* c) { c->codec = 1; }, [](auto* c) { c->format = DXGI_FORMAT_R16G16B16A16_FLOAT; }}) {
      auto rejected = contract;
      mutate(&rejected);
      Check(!shot::InlineRestoreEligible(rejected), "non-target contract rejected");
    }
    shot::internal::DiagnosticPlane offset_plane{
        .width = 152, .height = 48,
        .footprint = {.Offset = 512, .Footprint = {.Format = DXGI_FORMAT_R16G16B16A16_UNORM,
            .Width = 152, .Height = 48, .Depth = 1, .RowPitch = 1280}},
        .total_bytes = 512 + 1280 * 48, .format = DXGI_FORMAT_R16G16B16A16_UNORM};
    Check(shot::internal::DiagnosticPixelBytes(offset_plane.format) == 8
        && shot::internal::DiagnosticFootprintValid(offset_plane), "RGBA16 UNORM eight-byte offset/padding footprint");
    offset_plane.total_bytes = 512 + 1280 * 47 + 1215;
    Check(!shot::internal::DiagnosticFootprintValid(offset_plane), "short final row rejected");

    ComPtr<ID3D12Debug> debug;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      std::cout << "NotRun inline-restore-roi reason=debug_layer_unavailable\n";
      return 2;
    }
    debug->EnableDebugLayer();
    Gpu gpu(false);
    ComPtr<ID3D12InfoQueue> info_queue;
    if (FAILED(gpu.device.As(&info_queue))) {
      std::cout << "NotRun inline-restore-roi reason=info_queue_unavailable\n";
      return 2;
    }
    ComPtr<IDXGIAdapter1> software_adapter;
    DXGI_ADAPTER_DESC1 adapter_desc{};
    Check(SUCCEEDED(gpu.factory->EnumAdapterByLuid(gpu.device->GetAdapterLuid(), IID_PPV_ARGS(&software_adapter)))
        && SUCCEEDED(software_adapter->GetDesc1(&adapter_desc))
        && (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0,
        "ROI validates the actual D3D12 device belongs to a software adapter");
    model_gpu = &gpu;
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "ROI WARP real completion hook");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "ROI WARP real Reset hook");
    const auto wait_and_reset = [&] {
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "ROI fixture GPU completion signal");
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr, "ROI fixture completion event");
      const auto signalled = gpu.fence->SetEventOnCompletion(gpu.serial, event);
      const auto waited = (SUCCEEDED(signalled) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
      CloseHandle(event);
      Check(waited == WAIT_OBJECT_0, "ROI bounded WARP completion");
      shot::PreserveStageSubmissionProofs();
      Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
          "ROI completed recording Reset");
      native::DrainRetiredResources(true);
      gpu.uploads.clear();
      gpu.retained_resources.clear();
    };
    const auto submit = [&] {
      Check(SUCCEEDED(gpu.list->Close()), "ROI fixture recording close");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      gpu.queue->ExecuteCommandLists(1, lists);
      shot::PreserveStageSubmissionProofs();
      wait_and_reset();
    };
    {
      const shot::EvaluationScope began_before_request;
      Check(shot::RequestInlineRestoreRoi(directory("late-arm"), &reason), "arm after outer evaluation began");
      Check(shot::ClaimInlineRestoreRoi(gpu.list.Get(), contract) == 0, "late arm cannot claim an in-progress evaluation");
    }
    {
      const shot::EvaluationScope evaluation;
      auto reset = contract;
      reset.reset_sent = true;
      Check(shot::ClaimInlineRestoreRoi(gpu.list.Get(), reset) == 0 && shot::internal::requested_inline_restore,
          "reset skips without consuming request");
      reset = contract; reset.work_width = 1920;
      Check(shot::ClaimInlineRestoreRoi(gpu.list.Get(), reset) == 0
          && shot::GetInlineRestoreStatus().reason == "unsupported_inline_restore_contract",
          "stable wrong dimensions explicitly fail the request");
    }
    Check(shot::RequestInlineRestoreRoi(directory("expired"), &reason), "arm expiry fixture");
    shot::internal::requested_inline_restore->deadline_ms = 0;
    shot::OnPresent(1, true);
    Check(std::strcmp(shot::GetInlineRestoreStatus().status, "failed") == 0
        && !shot::internal::inline_restore_active, "unclaimed expiry explicitly fails and frees request slot");

    native::enabled = true;
    native::hooks_enabled = true;
    native::hook_point = native::kHookUpscaled;
    native::cost_meter_enabled = false;
    native::gpu_timers_enabled = false;
    native::stack_passes = 1;
    native::nr_follow_input_res = false;
    native::nr_resolution_scale = 1.f;
    native::nr_adaptive_enabled = false;
    native::codec_mode = 2;
    native::feed_mode = 0;
    native::look_mode = false;
    native::detail_stability = 0;
    native::chained_temporal_history = true;
    native::transfer_strength = 1.f;
    native::color_strength = 1.f;
    native::sdr_black_restore = 0;
    native::direct_api.module = GetModuleHandleW(nullptr);
    native::direct_api.create = CreateModel;
    native::direct_api.evaluate = EvaluateModel;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::successful_sr_device = gpu.device.Get();
    native::direct_runtime_requirements.file_version = "MOCK";
    native::parameter_runtime_ready = true;
    native::core_allocate_parameters = addon::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = addon::bridge::DestroyOwnedParameters;
    const auto texture = [&](DXGI_FORMAT format, bool writable, uint32_t width = 3840, uint32_t height = 2160) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = width, .Height = height, .DepthOrArraySize = 1, .MipLevels = 1, .Format = format,
          .SampleDesc = {1, 0}, .Flags = (writable ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE)};
      ComPtr<ID3D12Resource> resource;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))), "ROI production input texture");
      gpu.Write(resource.Get(), 0);
      gpu.Transition(resource.Get(), D3D12_RESOURCE_STATE_COMMON,
          writable ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : NATIVE_INPUT_STATE);
      return resource;
    };
    auto output = texture(DXGI_FORMAT_R16G16B16A16_UNORM, true);
    auto color = texture(DXGI_FORMAT_R16G16B16A16_UNORM, false);
    auto depth = texture(DXGI_FORMAT_R32_FLOAT, false);
    auto motion = texture(DXGI_FORMAT_R16G16_FLOAT, false);
    submit();
    addon::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_Parameter_Color, color.Get());
    parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
    parameters.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
    parameters.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
    parameters.Set(NVSDK_NGX_Parameter_Width, 3840u);
    parameters.Set(NVSDK_NGX_Parameter_Height, 2160u);
    parameters.Set(NVSDK_NGX_Parameter_OutWidth, 3840u);
    parameters.Set(NVSDK_NGX_Parameter_OutHeight, 2160u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 3840u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 2160u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
        static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_Reset, 0u);
    const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0x401A});
    native::RegisterCreatedFeatureLocked(handle, native::kFeatureDlss, &parameters);
    const auto evaluate = [&] {
      ++native::present_generation;
      const shot::EvaluationScope evaluation;
      const addon::EvaluateChainScope chain;
      const addon::SourceStageScope source(handle, true);
      native::NrRecording recording;
      bool touched = false;
      Check(native::ProcessInline(gpu.list.Get(), handle, &parameters, &touched, nullptr, &recording, false, true),
          "ROI production RecordInline accepts MOCK model on WARP");
      return recording;
    };
    const auto success_directory = directory("success");
    Check(shot::RequestInlineRestoreRoi(success_directory, &reason), "arm production ROI");
    Check(!shot::RequestCapture() && !shot::RequestInlineRestoreRoi(directory("busy"), &reason)
        && reason == "capture_busy", "ordinary and ROI requests share one busy gate");
    auto recorded = evaluate();
    Check(recorded.model_evaluate_mask == 0 && !shot::HasPending(), "fresh model warmup cannot claim ROI");
    submit();
    recorded = evaluate();
    Check(recorded.model_reset_mask == 1 && !shot::HasPending(), "real reset-bearing model call cannot claim ROI");
    submit();
    const auto original_code = [](uint32_t x, uint32_t y, uint32_t channel) {
      return static_cast<uint16_t>((x * 17 + y * 31 + channel * 8191) & 0xffffu);
    };
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT original_footprint{};
    uint64_t original_bytes = 0;
    const auto original_desc = output->GetDesc();
    gpu.device->GetCopyableFootprints(&original_desc, 0, 1, 0, &original_footprint, nullptr, nullptr, &original_bytes);
    auto original_upload = gpu.Buffer(original_bytes, D3D12_HEAP_TYPE_UPLOAD);
    void* original_mapped = nullptr;
    Check(SUCCEEDED(original_upload->Map(0, nullptr, &original_mapped)), "coordinate pattern upload map");
    for (uint32_t y = 0; y < 2160; ++y) {
      auto* row = static_cast<uint8_t*>(original_mapped) + original_footprint.Offset
          + static_cast<size_t>(y) * original_footprint.Footprint.RowPitch;
      for (uint32_t x = 0; x < 3840; ++x) {
        const uint16_t rgba[]{original_code(x, y, 0), original_code(x, y, 1), original_code(x, y, 2), original_code(x, y, 3)};
        std::memcpy(row + static_cast<size_t>(x) * 8, rgba, sizeof(rgba));
      }
    }
    original_upload->Unmap(0, nullptr);
    gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    const D3D12_TEXTURE_COPY_LOCATION original_source{.pResource = original_upload.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = original_footprint};
    const D3D12_TEXTURE_COPY_LOCATION original_destination{.pResource = output.Get(),
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
    gpu.list->CopyTextureRegion(&original_destination, 0, 0, 0, &original_source, nullptr);
    gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gpu.uploads.push_back(original_upload);
    recorded = evaluate();
    Check(recorded.inline_restore_serial != 0 && recorded.model_pass_mask == 1 && recorded.output_ready
        && recorded.group_complete && shot::internal::pending, "unique successful pass claims the four-plane capture");
    auto& pending = *shot::internal::pending;
    const uint64_t readback_heap_reserved_bytes = (pending.reservation ? pending.reservation->bytes : 0);
    Check(pending.planes.size() == 4 && pending.pre_nr_readback == nullptr && pending.nr_output_readback == nullptr
        && pending.allocated_bytes < 200'000, "four ROI-only readbacks allocate no full-image pair");
    shot::SealInlineRestoreBridge(recorded.inline_restore_serial, true, 123, true, true);
    Check(!shot::internal::StageSubmissionCompleted(&pending), "unsubmitted recording has no success evidence");
    {
      auto wrong_recording = pending;
      wrong_recording.planes.back().recording.second++;
      Check(!shot::internal::StageSubmissionCompleted(&wrong_recording), "different recording cannot complete a missing plane");
    }
    ComPtr<ID3D12Fence> blocked;
    Check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blocked))), "ROI pending-GPU gate");
    struct GateRelease {
      ID3D12Fence* fence;
      ~GateRelease() {
        if (fence != nullptr) {
          fence->Signal(1);
        }
      }
    } gate_release{.fence = blocked.Get()};
    Check(SUCCEEDED(gpu.queue->Wait(blocked.Get(), 1)), "block only the fixture WARP queue");
    Check(SUCCEEDED(gpu.list->Close()), "ROI captured recording close");
    ID3D12CommandList* lists[]{gpu.list.Get()};
    gpu.queue->ExecuteCommandLists(1, lists);
    shot::PreserveStageSubmissionProofs();
    Check(!pending.planes.front().submission_proof.empty()
        && !shot::internal::StageSubmissionCompleted(&pending), "receipt saved immediately but blocked GPU is not completed");
    shot::OnPresent(++native::present_generation, true);
    Check(shot::HasPending() && !shot::internal::job, "present count cannot deliver incomplete or replayable work");
    Check(SUCCEEDED(blocked->Signal(1)), "release synthetic fixture GPU gate");
    gate_release.fence = nullptr;
    wait_and_reset();
    Check(shot::internal::StageSubmissionCompleted(&pending) && shot::internal::ReadbacksReleasable(pending),
        "healthy exact receipts survive Reset and pruning");
    std::array<std::vector<uint8_t>, 4> expected;
    uint64_t readback_footprint_bytes = 0;
    for (size_t index = 0; index < pending.planes.size(); ++index) {
      const auto& plane = pending.planes[index];
      readback_footprint_bytes += plane.total_bytes;
      const size_t row_bytes = static_cast<size_t>(plane.width) * shot::internal::DiagnosticPixelBytes(plane.format);
      expected[index].resize(row_bytes * plane.height);
      if (index == 0) {
        for (uint32_t y = 0; y < 48; ++y) {
          for (uint32_t x = 0; x < 152; ++x) {
            const uint16_t rgba[]{original_code(1936 + x, 1106 + y, 0), original_code(1936 + x, 1106 + y, 1),
                original_code(1936 + x, 1106 + y, 2), original_code(1936 + x, 1106 + y, 3)};
            std::memcpy(expected[index].data() + (static_cast<size_t>(y) * 152 + x) * 8, rgba, sizeof(rgba));
          }
        }
        continue;
      }
      void* mapped = nullptr;
      Check(SUCCEEDED(plane.readback->Map(0, nullptr, &mapped)), "fixture reads completed ROI reference");
      for (uint32_t row = 0; row < plane.height; ++row) {
        std::memcpy(expected[index].data() + row * row_bytes,
            static_cast<uint8_t*>(mapped) + plane.footprint.Offset + row * plane.footprint.Footprint.RowPitch, row_bytes);
      }
      plane.readback->Unmap(0, nullptr);
    }
    auto completed = pending;
    completed.reservation.reset();
    for (auto& plane : completed.planes) plane.readback = nullptr;
    shot::OnPresent(++native::present_generation, true);
    Check(!shot::HasPending() && shot::internal::job, "only retired completed recording is handed to worker");
    auto work = std::move(shot::internal::job);
    shot::internal::job.reset();
    shot::internal::ProcessJob(std::move(work));
    Check(std::strcmp(shot::GetInlineRestoreStatus().status, "written") == 0, "four successful native raw files publish written");
    constexpr std::array names{"original", "post_resolve", "restore_blocks", "post_restore"};
    size_t files = 0, bytes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(success_directory)) {
      ++files;
      Check(entry.path().extension() != L".png" && entry.path().extension() != L".f32", "raw request emits no preview");
    }
    for (size_t index = 0; index < names.size(); ++index) {
      std::ifstream input(success_directory / (std::string(names[index]) + ".raw"), std::ios::binary);
      const std::vector<uint8_t> actual{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
      Check(actual == expected[index], "exported raw bytes preserve native codes and strip GPU row padding");
      if (index == 0) Check(actual == expected[0], "original ROI matches independent full-image x/y code oracle, not pre-DLAA Color");
      bytes += actual.size();
    }
    Check(files == 5 && bytes == 175392, "exactly four raw files and one sidecar with expected aggregate bytes");
    std::ifstream meta_file(success_directory / L"capture.meta.txt", std::ios::binary);
    const std::string meta{std::istreambuf_iterator<char>(meta_file), std::istreambuf_iterator<char>()};
    meta_file.close();
    Check(meta.find("capture_valid=1") != std::string::npos && meta.find(".receipt.0.value=") != std::string::npos
        && meta.find("nexus_build=") != std::string::npos && meta.find("pair_allocated=0") != std::string::npos,
        "sidecar binds build, exact receipts and no pair");
    for (const char* failure : {"missing", "discarded", "uncompleted", "copyback"}) {
      auto failed = completed;
      failed.inline_restore->output_directory = directory(failure);
      failed.inline_restore->capture_id = failed.inline_restore->output_directory.filename().string();
      failed.metadata_only = true;
      failed.completion_proven = false;
      failed.reservation.reset();
      for (auto& plane : failed.planes) plane.readback = nullptr;
      if (std::string_view(failure) == "missing") failed.planes.pop_back();
      if (std::string_view(failure) == "discarded") {
        failed.inline_restore->recording_status = std::make_shared<addon::submission::RecordingStatus>();
        failed.inline_restore->recording_status->closed = true;
      }
      if (std::string_view(failure) == "uncompleted") {
        failed.planes.front().submission_proof.waits = {{addon::FenceRef(blocked.Get()), 2}};
      }
      if (std::string_view(failure) == "copyback") failed.inline_restore->copyback_queued = false;
      Check(!shot::internal::StageSubmissionCompleted(&failed), "incomplete fixture cannot prove successful capture");
      const auto failed_directory = failed.inline_restore->output_directory;
      shot::internal::ProcessJob(std::move(failed));
      Check(std::strcmp(shot::GetInlineRestoreStatus().status, "failed") == 0, "incomplete capture never publishes written");
      for (const auto& entry : std::filesystem::directory_iterator(failed_directory)) {
        Check(entry.path().filename() == L"capture.meta.txt", "failed export contains only redacted failure metadata");
      }
    }
    // Five evaluations cross the old 24-plane limit through independently submitted and retired recordings.
    constexpr size_t TAIL_FRAME_COUNT = 5;
    const auto tail_directory = directory("tail-five");
    Check(shot::RequestInlineRestoreRoi(tail_directory, &reason, TAIL_FRAME_COUNT), "arm five-evaluation tail ROI");
    constexpr std::array tail_names{"original", "nr_model", "post_resolve", "post_restore", "depth"};
    constexpr std::array tail_formats{DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R32_FLOAT};
    constexpr size_t TAIL_PLANES_PER_FRAME = tail_names.size();
    constexpr size_t TAIL_PLANE_COUNT = TAIL_FRAME_COUNT * TAIL_PLANES_PER_FRAME;
    std::array<shot::internal::InlineRestoreFrame, TAIL_FRAME_COUNT> tail_frames{};
    std::array<std::vector<uint8_t>, TAIL_PLANE_COUNT> tail_expected;
    std::array<addon::GpuLease, TAIL_PLANE_COUNT> tail_receipts;
    const auto tail_name = [&](size_t frame, size_t role) {
      return "f" + std::to_string(tail_frames[frame].context.evaluation_sequence) + "_" + tail_names[role];
    };
    const auto tail_file = [&](size_t frame, size_t role) {
      return tail_directory / (tail_name(frame, role) + ".raw");
    };
    uint64_t tail_serial = 0;
    for (size_t frame = 0; frame < TAIL_FRAME_COUNT; ++frame) {
      // Change the ROI's first pixel so each frame has an independent coordinate oracle.
      Check(SUCCEEDED(original_upload->Map(0, nullptr, &original_mapped)), "tail source pattern map");
      const uint16_t marker[]{static_cast<uint16_t>(original_code(960, 768, 0) + frame * 997),
          original_code(960, 768, 1), original_code(960, 768, 2), original_code(960, 768, 3)};
      std::memcpy(static_cast<uint8_t*>(original_mapped) + original_footprint.Offset
          + 768 * original_footprint.Footprint.RowPitch + 960 * 8, marker, sizeof(marker));
      original_upload->Unmap(0, nullptr);
      gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      const D3D12_BOX tail_box{960, 768, 0, 1280, 896, 1};
      gpu.list->CopyTextureRegion(&original_destination, 960, 768, 0, &original_source, &tail_box);
      gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      gpu.uploads.push_back(original_upload);
      recorded = evaluate();
      Check(recorded.inline_restore_serial != 0 && recorded.model_pass_mask == 1 && recorded.output_ready
          && recorded.group_complete && shot::internal::pending, "qualified production evaluation joins tail");
      if (frame == 0) {
        tail_serial = recorded.inline_restore_serial;
      }
      shot::SealInlineRestoreBridge(recorded.inline_restore_serial, true, 124 + frame, true, true);
      auto& tail = *shot::internal::pending;
      Check(recorded.inline_restore_serial == tail_serial && tail.inline_restore
          && tail.inline_restore->tail_frame_count == TAIL_FRAME_COUNT
          && tail.inline_restore->context.tail_inputs_aligned && tail.inline_restore->frames.size() == frame + 1
          && tail.planes.size() == (frame + 1) * TAIL_PLANES_PER_FRAME
          && tail.pre_nr_readback == nullptr && tail.nr_output_readback == nullptr,
          "one pending serial accumulates five native ROI planes per aligned evaluation without full-image pair");
      const auto& captured = tail.inline_restore->frames.back();
      const auto& context = captured.context;
      tail_frames[frame] = captured;
      Check(context.present_generation == native::present_generation, "tail keeps exact qualified present ID");
      Check(captured.look_shaped && captured.look_stability_recorded, "tail records the executed filter for this evaluation");
      if (frame > 0) {
        const auto& previous = tail_frames[frame - 1];
        // The shared observation sequence also counts plan events; evaluates counts only this stage.
        Check(context.evaluation_sequence > previous.context.evaluation_sequence
            && context.model_evaluation_index == previous.context.model_evaluation_index + 1
            && context.present_generation > previous.context.present_generation
            && context.source_feature == previous.context.source_feature
            && context.source_generation == previous.context.source_generation
            && captured.recording != previous.recording,
            "tail preserves consecutive actual evaluations on one source with independent recordings");
      }
      Check(!shot::internal::StageSubmissionCompleted(&tail), "unsubmitted tail evaluation cannot borrow prior completion");
      submit();
      Check(captured.recording_status != nullptr && captured.recording_status->submitted.load()
          && captured.recording_status->closed.load() && captured.bridge_submitted && captured.copyback_queued,
          "each tail frame retains its submitted and retired recording with queued bridge copyback");
      for (size_t role = 0; role < TAIL_PLANES_PER_FRAME; ++role) {
        const auto index = frame * TAIL_PLANES_PER_FRAME + role;
        const auto name = tail_name(frame, role);
        const auto found = std::find_if(tail.planes.begin(), tail.planes.end(),
            [&](const auto& candidate) { return candidate.name == name; });
        Check(found != tail.planes.end(), "tail includes each sequence-named plane");
        const auto& plane = *found;
        Check(plane.width == 320 && plane.height == 128 && plane.format == tail_formats[role]
            && plane.recording == captured.recording && plane.readback != nullptr && !plane.submission_proof.empty(),
            "tail planes retain native formats, fixed ROI dimensions and exact receipts");
        tail_receipts[index] = plane.submission_proof;
        const size_t row_bytes = 320 * shot::internal::DiagnosticPixelBytes(plane.format);
        tail_expected[index].resize(row_bytes * 128);
        void* mapped = nullptr;
        Check(SUCCEEDED(plane.readback->Map(0, nullptr, &mapped)), "read retired tail plane");
        for (uint32_t y = 0; y < 128; ++y) {
          std::memcpy(tail_expected[index].data() + y * row_bytes,
              static_cast<uint8_t*>(mapped) + plane.footprint.Offset + y * plane.footprint.Footprint.RowPitch, row_bytes);
        }
        plane.readback->Unmap(0, nullptr);
        if (role == 0) {
          Check(std::memcmp(tail_expected[index].data(), marker, sizeof(marker)) == 0,
              "tail original starts at the fixed crop and belongs to this evaluation");
        }
      }
      if (frame + 1 < TAIL_FRAME_COUNT) {
        shot::OnPresent(++native::present_generation, true);
        Check(shot::HasPending() && !shot::internal::job, "each of the first four retired frames cannot hand off the tail");
      }
    }
    auto& tail = *shot::internal::pending;
    Check(shot::internal::StageSubmissionCompleted(&tail) && shot::internal::ReadbacksReleasable(tail),
        "all five frames retain independent exact completion after Reset and retirement");
    shot::OnPresent(++native::present_generation, true);
    Check(!shot::HasPending() && shot::internal::job, "all five sealed retired frames hand off one tail job");
    auto tail_work = std::move(shot::internal::job);
    shot::internal::job.reset();
    shot::internal::ProcessJob(std::move(tail_work));
    Check(std::strcmp(shot::GetInlineRestoreStatus().status, "written") == 0, "five-frame tail publishes written");
    size_t tail_files = 0, tail_bytes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(tail_directory)) {
      ++tail_files;
    }
    for (size_t frame = 0; frame < TAIL_FRAME_COUNT; ++frame) {
      for (size_t role = 0; role < TAIL_PLANES_PER_FRAME; ++role) {
        std::ifstream input(tail_file(frame, role), std::ios::binary);
        const std::vector<uint8_t> actual{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        Check(actual == tail_expected[frame * TAIL_PLANES_PER_FRAME + role],
            "sequence-named tail file has native bytes with no row gaps");
        tail_bytes += actual.size();
      }
    }
    Check(tail_files == TAIL_PLANE_COUNT + 1 && tail_bytes == 7'372'800,
        "exactly 25 native ROI files and one tail sidecar cross the old 24-plane limit");
    std::ifstream tail_meta_file(tail_directory / L"capture.meta.txt", std::ios::binary);
    const std::string tail_meta{std::istreambuf_iterator<char>(tail_meta_file), std::istreambuf_iterator<char>()};
    tail_meta_file.close();
    const auto tail_field = [&](const std::string& field) {
      return tail_meta.starts_with(field + "\n") || tail_meta.starts_with(field + "\r\n")
          || tail_meta.find("\n" + field + "\n") != std::string::npos
          || tail_meta.find("\n" + field + "\r\n") != std::string::npos;
    };
    Check(tail_field("capture_schema=inline_tail_roi_v1")
        && tail_field("frame_count=" + std::to_string(TAIL_FRAME_COUNT)) && tail_field("capture_valid=1"),
        "tail sidecar declares the five-frame schema");
    for (size_t frame = 0; frame < TAIL_FRAME_COUNT; ++frame) {
      const auto prefix = "frame." + std::to_string(frame) + ".";
      const auto& captured = tail_frames[frame];
      const auto& context = captured.context;
      Check(tail_field(prefix + "evaluation_sequence=" + std::to_string(context.evaluation_sequence))
          && tail_field(prefix + "model_evaluation_index=" + std::to_string(context.model_evaluation_index))
          && tail_field(prefix + "source_generation=" + std::to_string(context.source_generation))
          && tail_field(prefix + "present_generation=" + std::to_string(context.present_generation))
          && tail_field(prefix + "recording_list=" + std::to_string(captured.recording.first))
          && tail_field(prefix + "recording_generation=" + std::to_string(captured.recording.second))
          && tail_field(prefix + "bridge_submission=" + std::to_string(captured.bridge_submission))
          && tail_field(prefix + "look_shaped=1") && tail_field(prefix + "look_stability_recorded=1")
          && tail_field(prefix + "valid=1"),
          "sidecar preserves each qualified frame's exact IDs, recording and executed filter");
      for (size_t role = 0; role < TAIL_PLANES_PER_FRAME; ++role) {
        const auto plane_prefix = "plane." + tail_name(frame, role) + ".";
        const auto& proof = tail_receipts[frame * TAIL_PLANES_PER_FRAME + role];
        Check(tail_field(plane_prefix + "recording_list=" + std::to_string(captured.recording.first))
            && tail_field(plane_prefix + "recording_generation=" + std::to_string(captured.recording.second))
            && tail_field(plane_prefix + "submission_receipts=" + std::to_string(proof.waits.size()))
            && tail_field(plane_prefix + "valid=1"), "each exported tail plane binds its frame's recording and receipts");
        for (size_t receipt = 0; receipt < proof.waits.size(); ++receipt) {
          const auto& [fence, value] = proof.waits[receipt];
          const auto receipt_prefix = plane_prefix + "receipt." + std::to_string(receipt) + ".";
          Check(tail_field(receipt_prefix + "fence=" + std::to_string(reinterpret_cast<uintptr_t>(fence.fence)))
              && tail_field(receipt_prefix + "value=" + std::to_string(value))
              && tail_field(receipt_prefix + "healthy=1"), "each tail receipt preserves the exact completed submission");
        }
      }
    }
    // One reduced-grid production recording, not an adapter or resolution matrix.
    info_queue->ClearStoredMessages();
    auto small_output = texture(DXGI_FORMAT_R16G16B16A16_UNORM, true, 97, 65);
    auto small_color = texture(DXGI_FORMAT_R16G16B16A16_UNORM, false, 97, 65);
    auto small_depth = texture(DXGI_FORMAT_R32_FLOAT, false, 97, 65);
    auto small_motion = texture(DXGI_FORMAT_R16G16_FLOAT, false, 97, 65);
    addon::bridge::OwnedParameters small_parameters;
    small_parameters.Set(NVSDK_NGX_Parameter_Color, small_color.Get());
    small_parameters.Set(NVSDK_NGX_Parameter_Output, small_output.Get());
    small_parameters.Set(NVSDK_NGX_Parameter_Depth, small_depth.Get());
    small_parameters.Set(NVSDK_NGX_Parameter_MotionVectors, small_motion.Get());
    small_parameters.Set(NVSDK_NGX_Parameter_Width, 97u);
    small_parameters.Set(NVSDK_NGX_Parameter_Height, 65u);
    small_parameters.Set(NVSDK_NGX_Parameter_OutWidth, 97u);
    small_parameters.Set(NVSDK_NGX_Parameter_OutHeight, 65u);
    small_parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 97u);
    small_parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 65u);
    small_parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
        static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
    small_parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    small_parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    small_parameters.Set(NVSDK_NGX_Parameter_Reset, 0u);
    const auto* small_handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0x401B});
    native::RegisterCreatedFeatureLocked(small_handle, native::kFeatureDlss, &small_parameters);
    const native::BridgeAdaptiveSource mock_bridge{
        .api = addon::capability::Api::DX11, .backend = addon::capability::Backend::DX11_BRIDGE,
        .handle = small_handle, .device = gpu.device.Get(), .list = gpu.list.Get(),
        .device_generation = 1, .source_generation = 1, .shared_layout = 1};
    native::nr_resolution_scale = 0.67f;
    native::look_mode = true;
    native::look_strength = 0.f;
    native::detail_stability = 1;
    native::direct_api.evaluate = +[](ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* model,
        const NVSDK_NGX_Parameter* values, PFN_NVSDK_NGX_ProgressCallback callback) -> NVSDK_NGX_Result {
      const auto result = EvaluateModel(list, model, values, callback);
      if (NVSDK_NGX_FAILED(result)) return result;
      auto* destination = native::GetD3D12Resource(values, "DLSSNR.Output");
      const auto desc = destination->GetDesc();
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
      uint64_t bytes = 0;
      model_gpu->device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
      auto upload = model_gpu->Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
      void* mapped = nullptr;
      Check(SUCCEEDED(upload->Map(0, nullptr, &mapped)), "stage raw-model pattern upload");
      constexpr uint16_t raw_rgba[]{0x3800, 0x3400, 0x3000, 0x3c00};  // 0.5, 0.25, 0.125, 1 in FP16.
      for (uint32_t y = 0; y < desc.Height; ++y) {
        for (uint32_t x = 0; x < desc.Width; ++x) {
          std::memcpy(static_cast<uint8_t*>(mapped) + footprint.Offset
              + static_cast<size_t>(y) * footprint.Footprint.RowPitch + x * sizeof(raw_rgba), raw_rgba, sizeof(raw_rgba));
        }
      }
      upload->Unmap(0, nullptr);
      native::Transition(list, destination, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION from{.pResource = upload.Get(),
          .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
      D3D12_TEXTURE_COPY_LOCATION to{.pResource = destination, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      native::Transition(list, destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      model_gpu->uploads.push_back(upload);
      return result;
    };
    const auto evaluate_small = [&] {
      gpu.Transition(small_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION to{.pResource = small_output.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      const D3D12_BOX box{0, 0, 0, 97, 65, 1};
      gpu.list->CopyTextureRegion(&to, 0, 0, 0, &original_source, &box);
      gpu.Transition(small_output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      gpu.uploads.push_back(original_upload);
      ++native::present_generation;
      const shot::EvaluationScope evaluation;
      const addon::EvaluateChainScope chain;
      const addon::SourceStageScope source(small_handle, true);
      native::NrRecording result;
      Check(native::ProcessInline(gpu.list.Get(), small_handle, &small_parameters, nullptr, nullptr, &result,
          false, false, &mock_bridge), "reduced production RecordInline records MOCK Bridge source on WARP");
      return result;
    };
    evaluate_small();
    submit();
    evaluate_small();
    submit();
    shot::source_mode = static_cast<uint32_t>(shot::SourceMode::kEvaluatedBuffer);
    shot::stage_planes_enabled = true;
    Check(shot::RequestCapture(), "existing evaluated-buffer screenshot request arms bridge stages");
    const auto stage_recording = evaluate_small();
    Check(stage_recording.inline_restore_serial != 0 && shot::internal::pending
        && shot::internal::pending->bridge_model_stages, "one real model evaluation claims seven-stage capture");
    auto& stages = *shot::internal::pending;
    const auto stage_frame = *stages.bridge_model_stages;
    Check(stages.planes.size() == 7 && stages.pre_nr_readback != nullptr && stages.nr_output_readback != nullptr
        && stage_frame.context.work_width == 65 && stage_frame.context.work_height == 44
        && stage_frame.context.output_width == 97 && stage_frame.context.output_height == 65,
        "noninteger ratio keeps model grid, output grid and primary preview pair distinct");
    shot::SealInlineRestoreBridge(stage_recording.inline_restore_serial, true, 211, true, true);
    Check(!shot::internal::StageSubmissionCompleted(&stages), "recorded stages cannot publish before real software submission");
    submit();
    Check(shot::internal::BridgeModelStagesComplete(stages) && shot::internal::StageSubmissionCompleted(&stages),
        "all seven stages require submitted, completed and retired recording plus queued bridge copyback");
    for (const char* failure : {"missing", "recording", "uncompleted", "copyback"}) {
      auto rejected = stages;
      if (std::string_view(failure) == "missing") rejected.planes.pop_back();
      if (std::string_view(failure) == "recording") rejected.planes.front().recording.second++;
      if (std::string_view(failure) == "uncompleted") rejected.planes.front().submission_proof.waits = {{addon::FenceRef(blocked.Get()), 2}};
      if (std::string_view(failure) == "copyback") rejected.bridge_model_stages->copyback_queued = false;
      Check(!shot::internal::StageSubmissionCompleted(&rejected), "incomplete stages cannot borrow other-frame or elapsed-time evidence");
    }
    std::array<std::vector<float>, 7> stage_expected;
    for (size_t index = 0; index < shot::kBridgeModelStageNames.size(); ++index) {
      const auto name = shot::kBridgeModelStageNames[index];
      const auto plane = std::find_if(stages.planes.begin(), stages.planes.end(),
          [name](const auto& candidate) { return candidate.name == name; });
      Check(plane != stages.planes.end() && plane->required && plane->recording == stage_frame.recording,
          "every stage belongs to exactly this evaluation recording");
      const bool low_grid = index == 1 || index == 2 || index == 3;
      Check(plane->width == (low_grid ? 65u : 97u) && plane->height == (low_grid ? 44u : 65u)
          && plane->full_width == 97 && plane->full_height == 65 && !plane->region_copy,
          "full numerical planes retain their own grid without reusing fixed floor or tail coordinates");
      auto& expected = stage_expected[index];
      expected.resize(static_cast<size_t>(plane->width) * plane->height * 4);
      void* mapped = nullptr;
      Check(SUCCEEDED(plane->readback->Map(0, nullptr, &mapped)), "map completed stage reference");
      for (uint32_t y = 0; y < plane->height; ++y) {
        const auto* row = static_cast<const uint8_t*>(mapped) + plane->footprint.Offset
            + static_cast<size_t>(y) * plane->footprint.Footprint.RowPitch;
        Check(shot::internal::DecodeDiagnosticRow(row, plane->width, plane->format,
            expected.data() + static_cast<size_t>(y) * plane->width * 4),
            ("decode stage codes without display conversion plane=" + std::string(name)
                + " format=" + std::to_string(static_cast<uint32_t>(plane->format))).c_str());
        for (uint32_t x = 0; x < plane->width; ++x) {
          for (uint32_t channel = 0; channel < 4; ++channel) {
            const float actual = expected[(static_cast<size_t>(y) * plane->width + x) * 4 + channel];
            if (index == 0) Check(std::abs(actual - original_code(x, y, channel) / 65535.f) < 1e-7f,
                "output original preserves independent full-grid x/y native-code oracle");
            if (index == 2) {
              constexpr float raw_rgba[]{0.5f, 0.25f, 0.125f, 1.f};
              Check(actual == raw_rgba[channel], "raw numerical neural plane precedes host gains and history");
            }
          }
        }
      }
      plane->readback->Unmap(0, nullptr);
    }
    Check(std::abs(stage_expected[3][0] - 0.5f) > 0.1f
        && stage_expected[4].size() != stage_expected[3].size() && stage_expected[4][0] < 0.1f,
        "post-compose contains executed gains on the low grid before output-grid edge-aware transport");
    shot::OnPresent(++native::present_generation, true);
    Check(!shot::HasPending() && shot::internal::job, "completed stage set reaches existing screenshot worker");
    auto stage_work = std::move(shot::internal::job);
    shot::internal::job.reset();
    const auto stage_base = renodx::utils::path::GetReShadeBasePath() / L"DLSS5 Screenshots"
        / (L"NR_DIAGNOSTIC_" + std::to_wstring(stage_work->timestamp_ms) + L"_" + std::to_wstring(stage_work->serial));
    shot::internal::ProcessJob(std::move(stage_work));
    Check(std::strcmp(shot::internal::finished.outcome, "written") == 0, "complete stage set exports numerical files and labeled previews");
    std::vector<std::filesystem::path> stage_files{shot::internal::finished.pre_image,
        shot::internal::finished.post_image, shot::internal::finished.meta};
    for (size_t index = 0; index < shot::kBridgeModelStageNames.size(); ++index) {
      const std::string name(shot::kBridgeModelStageNames[index]);
      auto path = stage_base;
      path += L"_" + std::wstring(name.begin(), name.end()) + L".f32";
      stage_files.push_back(path);
      std::ifstream input(path, std::ios::binary);
      std::vector<float> values(stage_expected[index].size());
      input.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(float)));
      Check(input.gcount() == static_cast<std::streamsize>(values.size() * sizeof(float)) && values == stage_expected[index],
          "each full numerical stage exports exact converted native codes with row padding stripped");
      auto raw_path = stage_base;
      raw_path += L"_" + std::wstring(name.begin(), name.end()) + L".raw";
      stage_files.push_back(raw_path);
    }
    std::ifstream stage_meta_file(shot::internal::finished.meta, std::ios::binary);
    const std::string stage_meta{std::istreambuf_iterator<char>(stage_meta_file), std::istreambuf_iterator<char>()};
    stage_meta_file.close();
    Check(stage_meta.find("bridge_model_stage_capture_valid=1") != std::string::npos
        && stage_meta.find("evaluation_sequence=" + std::to_string(stage_frame.context.evaluation_sequence)) != std::string::npos
        && stage_meta.find("plane.post_compose.space=model_work_grid") != std::string::npos
        && stage_meta.find("plane.post_look.space=output_grid") != std::string::npos,
        "sidecar binds same evaluation, exact completion and each stage coordinate space");
    native::direct_api.evaluate = EvaluateModel;
    native::nr_resolution_scale = 1.f;
    native::look_mode = false;
    native::look_strength = 1.f;
    native::detail_stability = 0;
    shot::stage_planes_enabled = false;
    shot::source_mode = 0;
    native::ForgetSourceHandleLocked(small_handle);
    submit();
    for (uint64_t index = 0; index < info_queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index) {
      SIZE_T bytes = 0;
      Check(SUCCEEDED(info_queue->GetMessage(index, nullptr, &bytes)), "stage Debug Layer message size");
      std::vector<uint8_t> storage(bytes);
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
      Check(SUCCEEDED(info_queue->GetMessage(index, message, &bytes)), "stage Debug Layer message");
      Check(message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION && message->Severity != D3D12_MESSAGE_SEVERITY_ERROR
          && message->Severity != D3D12_MESSAGE_SEVERITY_WARNING, message->pDescription);
    }
    native::ForgetSourceHandleLocked(handle);
    submit();
    native::successful_sr_device = nullptr;
    model_gpu = nullptr;
    const char* keep = std::getenv("NR_NEXUS_TEST_KEEP_RAW");
    if (keep == nullptr || std::strcmp(keep, "1") != 0) {
      for (const auto& path : stage_files) if (!path.empty()) std::filesystem::remove(path);
      for (size_t frame = 0; frame < TAIL_FRAME_COUNT; ++frame) {
        for (size_t role = 0; role < TAIL_PLANES_PER_FRAME; ++role) {
          std::filesystem::remove(tail_file(frame, role));
        }
      }
      for (const auto& path : owned_outputs) {
        for (const char* name : names) std::filesystem::remove(path / (std::string(name) + ".raw"));
        std::filesystem::remove(path / L"capture.meta.txt");
        std::filesystem::remove(path);
      }
    }
    std::cout << "PASS inline-restore-roi evidence=CPU+WARP+MOCK raw_bytes=175392 planes=4 pair=none"
              << " model_stages=7 model_grid=65x44 output_grid=97x65 debug_layer=required software_adapter=verified"
              << " tail_frames=" << TAIL_FRAME_COUNT << " tail_planes=" << TAIL_PLANE_COUNT << " tail_raw_bytes=" << tail_bytes
              << " readback_footprint_bytes=" << readback_footprint_bytes
              << " readback_heap_reserved_bytes=" << readback_heap_reserved_bytes
              << " native_ngx=NotRun d3d11_delivery=MOCK cleanup=Standard\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "inline-restore-roi: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace inline_restore_roi_cases
