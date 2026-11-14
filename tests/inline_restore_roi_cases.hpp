/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>

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
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu(false);
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
    const auto texture = [&](DXGI_FORMAT format, bool writable) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = 3840, .Height = 2160, .DepthOrArraySize = 1, .MipLevels = 1, .Format = format,
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
    auto wrong_recording = pending;
    wrong_recording.planes.back().recording.second++;
    Check(!shot::internal::StageSubmissionCompleted(&wrong_recording), "different recording cannot complete a missing plane");
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
    native::ForgetSourceHandleLocked(handle);
    submit();
    native::successful_sr_device = nullptr;
    model_gpu = nullptr;
    const char* keep = std::getenv("NR_NEXUS_TEST_KEEP_RAW");
    if (keep == nullptr || std::strcmp(keep, "1") != 0) {
      for (const auto& path : owned_outputs) {
        for (const char* name : names) std::filesystem::remove(path / (std::string(name) + ".raw"));
        std::filesystem::remove(path / L"capture.meta.txt");
        std::filesystem::remove(path);
      }
    }
    std::cout << "PASS inline-restore-roi evidence=CPU+WARP+MOCK raw_bytes=175392 planes=4 pair=none"
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
