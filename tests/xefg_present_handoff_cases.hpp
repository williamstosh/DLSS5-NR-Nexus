/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace xefg_present_handoff_cases {
namespace addon = renodx::addons::dlss5;
namespace rate = addon::output_framerate;

struct ExternalList {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  std::shared_ptr<submission::RecordingStatus> recording;
};

inline void Run(Gpu& gpu, const auto& submit, const auto& wait_fence, bool ordinary_signal_loss_only = false) {
  using stage_execution_cases::Capture;
  using Pixel = std::array<uint8_t, 4>;
  constexpr Pixel scene_a{62, 111, 190, 77};
  constexpr Pixel menu{200, 88, 41, 149};
  constexpr Pixel scene_b{37, 193, 87, 219};
  constexpr uint32_t size = 8;
  constexpr auto present_state = D3D12_RESOURCE_STATE_PRESENT;

  funnel_accounting_tests::Restore restore;
  restore.Value(&native::ngx_slot_real[0]);
  restore.Value(&adapter::proxies[0].real[0]);
  restore.Value(&model_parameter_observer);
  restore.Value(&nr_edit_reuse_xefg_cases::consume_sdk_present);
  restore.Value(&nr_edit_reuse_xefg_cases::sdk_present_result);
  restore.Atomic(&native::present_frames);
  restore.Atomic(&native::stack_passes);
  restore.Atomic(&native::present_guides);
  restore.Atomic(&native::present_pre_fg);
  native::stack_passes = 1;
  native::present_frames = 0;
  nr_edit_reuse_xefg_cases::sdk_present_result = S_OK;
  native::ngx_slot_real[0].create = reinterpret_cast<void*>(nr_edit_reuse_render_cases::CreateSr);
  native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(nr_edit_reuse_gpu_cases::CopySr);
  native::ngx_slot_real[0].release = reinterpret_cast<void*>(+[](NVSDK_NGX_Handle*) -> NVSDK_NGX_Result {
    return NVSDK_NGX_Result_Success;
  });
  adapter::proxies[0].real[0] = reinterpret_cast<void*>(&nr_edit_reuse_xefg_cases::ConsumeSdkPresent);
  xefg::Configure(&native::fg_service::Service, &Prepare, &native::fg_service::Retire,
                  &native::fg_service::Requested);

  Session session(&gpu);
  session.context->ui_composition = false;
  session.extent_left = session.extent_top = 0;
  session.extent_height = session.extent_width = size;
  ComPtr<ID3D12Resource> back, other_back;
  const auto other_swapchain = gpu.Swapchain();
  Check(SUCCEEDED(session.proxy->GetBuffer(session.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back)))
            && SUCCEEDED(other_swapchain->GetBuffer(other_swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&other_back)))
            && back.Get() != other_back.Get(), "handoff uses two distinct actual DXGI backbuffers");
  PresentRingFaceProbe route(&gpu, back.Get(), session.proxy.Get(), PresentRingFaceProbe::Mode::NATIVE_ROUTE);
  PresentRingFaceProbe other_route(&gpu, other_back.Get(), other_swapchain.Get(), PresentRingFaceProbe::Mode::NATIVE_ROUTE);
  native::present_guides = native::kPresentGuidesOptional;
  native::present_pre_fg = 1;
  for (auto* output : {&route, &other_route}) {
    const auto buffer = static_cast<uintptr_t>(output->get_current_back_buffer().handle);
    const auto canonical = reinterpret_cast<uintptr_t>(
        renodx::utils::directx::NativeIdentity(reinterpret_cast<IUnknown*>(buffer)));
    std::vector buffers{buffer};
    if (canonical != 0 && canonical != buffer) buffers.push_back(canonical);
    rate::RegisterPresentOutput(reinterpret_cast<uintptr_t>(output),
        reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(gpu.device.Get())),
        reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(output->swapchain)), buffers);
  }
  const auto upload = [&](ID3D12Resource* target, const auto& pixel, D3D12_RESOURCE_STATES resting) {
    const std::vector pixels(size * size, pixel);
    auto transfer = gpu.CreateTextureTransfer(target);
    gpu.Transition(target, resting, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu.UploadTexture(target, transfer, reinterpret_cast<const uint8_t*>(pixels.data()),
                      pixels.size() * sizeof(pixel), size * sizeof(pixel));
    gpu.Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, resting);
  };
  const auto expected_pixels = [&](Pixel pixel, bool edited) {
    if (edited) {
      pixel = {0, 0, 0, pixel[3]};
    }
    std::vector<uint8_t> bytes;
    for (uint32_t index = 0; index < size * size; ++index) {
      bytes.insert(bytes.end(), pixel.begin(), pixel.end());
    }
    return bytes;
  };
  const auto color = gpu.Texture(size, size, DXGI_FORMAT_R8G8B8A8_UNORM, NATIVE_INPUT_STATE);
  auto source_desc = color->GetDesc();
  source_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> source_output;
  Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &source_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&source_output))), "handoff game SR output");
  const std::array depths{gpu.Texture(size, size, DXGI_FORMAT_R32_FLOAT),
                          gpu.Texture(size, size, DXGI_FORMAT_R32_FLOAT)};
  const std::array motions{gpu.Texture(size, size, DXGI_FORMAT_R16G16_FLOAT),
                           gpu.Texture(size, size, DXGI_FORMAT_R16G16_FLOAT)};
  for (size_t scene = 0; scene < depths.size(); ++scene) {
    upload(depths[scene].Get(), (scene == 0 ? .25f : .75f), present_state);
    upload(motions[scene].Get(), std::array<uint16_t, 2>{static_cast<uint16_t>(scene == 0 ? 0 : 0x3c00), 0},
           present_state);
  }
  submit();

  addon::bridge::OwnedParameters parameters;
  parameters.Set(NVSDK_NGX_Parameter_Color, color.Get());
  parameters.Set(NVSDK_NGX_Parameter_Output, source_output.Get());
  parameters.Set(NVSDK_NGX_Parameter_Width, size);
  parameters.Set(NVSDK_NGX_Parameter_Height, size);
  parameters.Set(NVSDK_NGX_Parameter_OutWidth, size);
  parameters.Set(NVSDK_NGX_Parameter_OutHeight, size);
  parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, size);
  parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, size);
  parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
  parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
  uint64_t source_generation = 0;
  NVSDK_NGX_Handle* source = nullptr;
  const auto rebuild_source = [&] {
    if (source != nullptr) {
      Check(NVSDK_NGX_SUCCEED(native::HookedReleaseFeatureSlot<0>(source)), "game releases its previous SR source");
    }
    Check(NVSDK_NGX_SUCCEED(native::HookedCreateFeatureSlot<0>(gpu.list.Get(), native::kFeatureDlss, &parameters, &source))
              && source != nullptr, "game creates its SR source through the real hook");
    const auto previous = source_generation;
    source_generation = native::create_contracts.at(source).source_generation;
    Check(source_generation > previous, "source rebuild advances the real game generation");
  };
  rebuild_source();

  uint32_t produced_colors = 0;
  const auto produce = [&](ID3D12Resource* target, Pixel pixel, int scene, bool reset) {
    ++native::present_generation;
    if (scene < 0) {
      upload(target, pixel, present_state);
    } else {
      upload(color.Get(), pixel, NATIVE_INPUT_STATE);
      parameters.Set(NVSDK_NGX_Parameter_MotionVectors, motions[scene].Get());
      parameters.Set(NVSDK_NGX_Parameter_Depth, depths[scene].Get());
      parameters.Set(NVSDK_NGX_Parameter_Reset, static_cast<int>(reset));
      for (auto* resource : {depths[scene].Get(), motions[scene].Get()}) {
        gpu.Transition(resource, present_state, NATIVE_INPUT_STATE);
      }
      Check(NVSDK_NGX_SUCCEED(native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), source, &parameters, nullptr)),
            "wrapped game Evaluate provides current color and Reset");
      gpu.Transition(source_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      gpu.Transition(target, present_state, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.list->CopyResource(target, source_output.Get());
      gpu.Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, present_state);
      gpu.Transition(source_output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      for (auto* resource : {depths[scene].Get(), motions[scene].Get()}) {
        gpu.Transition(resource, NATIVE_INPUT_STATE, present_state);
      }
    }
    submit();
    ++produced_colors;
  };

  std::vector<uint8_t> consumed;
  uint32_t sdk_reads = 0;
  nr_edit_reuse_xefg_cases::consume_sdk_present = [&](IDXGISwapChain* sdk_output) {
    ComPtr<ID3D12Resource> current;
    Check(sdk_output == session.proxy.Get()
              && SUCCEEDED(session.proxy->GetBuffer(session.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&current)))
              && current.Get() == back.Get(), "the original SDK Present consumes the same scene/menu backbuffer address");
    const auto pixels = Capture(gpu, current.Get(), present_state);
    submit();
    consumed = pixels.Pixels();
    ++sdk_reads;
  };
  ID3D12Resource* model_depth = nullptr;
  ID3D12Resource* model_motion = nullptr;
  model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter* block) {
    model_depth = native::GetD3D12Resource(block, "DLSSNR.Depth");
    model_motion = native::GetD3D12Resource(block, "DLSSNR.MVec");
  };

  uint32_t present_id = 40000;
  uint32_t version = 0;
  bool accepted = true;
  const auto ordinary_submissions = [](PresentRingFaceProbe* output) {
    const auto it = native::present_streams.find(output);
    return (it == native::present_streams.end() ? uint64_t{0} : it->second.nr_presents);
  };
  const auto call_ordinary = [&](PresentRingFaceProbe* output) {
    const auto attempt = native::BeginOrdinaryPresentAttempt(&output->queue, output);
    Check(static_cast<bool>(attempt), "ordinary Present begins a real output attempt");
    native::present_sdr_white_polled_ns = native::SteadyNowNs();
    native::RunPresentPath(&output->queue, output);
    return attempt;
  };
  const auto find_attempt = [](PresentRingFaceProbe* output) {
    return native::FindOrdinaryPresentAttempt(reinterpret_cast<uintptr_t>(
        renodx::utils::directx::NativeIdentity(output->swapchain)), reinterpret_cast<uintptr_t>(
        renodx::utils::directx::NativeIdentity(output->back)));
  };
  const auto sdk_present = [&](Session* sdk_session, int scene, bool reset, HRESULT expected = S_OK) {
    if (scene >= 0) {
      ++present_id;
      sdk_session->Tag(present_id, DEPTH, depths[scene].Get());
      sdk_session->Tag(present_id, MOTION, motions[scene].Get());
      const xefg::abi::Constants constants{.motion_scale_x = 1.f, .motion_scale_y = 1.f,
          .reset = static_cast<uint32_t>(reset), .frame_time_ms = 16.5f};
      Check(Hooks::Constants(sdk_session->handle, present_id, &constants) == 0, "SDK receives current game Reset and constants");
      sdk_session->Present(present_id);
    } else {
      Check(!sdk_session->context->fresh_id, "unlabeled application Present starts without a fresh ID");
      Check(adapter::ProxyHooks<0>::Present(sdk_session->proxy.Get(), 0, 0) == expected,
            "Proxy Present returns the original untagged SDK result");
    }
  };
  const auto next_execute = [] {
    const std::lock_guard lock(submission::tracker_mutex);
    return submission::next_execute;
  };
  const auto observe_color_submissions = [](submission::RecordingOwner owner, const void* backbuffer,
                                            uint64_t first_execute, const auto& observe) {
    const std::lock_guard lock(submission::tracker_mutex);
    for (const auto& [list_id, list] : submission::tracked_lists) {
      for (const auto& [generation, recorded] : list.generations) {
        if (recorded.owner != owner || recorded.first_execute < first_execute
            || std::ranges::none_of(recorded.classified_uses, [&](const auto& use) {
                 return use.kind == submission::UseKind::kOutputColor && use.output.resource == backbuffer
                     && use.output.subresource == 0;
               })) continue;
        observe(list_id, generation, recorded);
      }
    }
  };
  const auto frame = [&](const char* stage, PresentRingFaceProbe* output, Pixel pixel, int scene, bool reset,
                         bool sdk, bool edited, int expected_reset, bool check = true,
                         xefg::OrdinaryPresentAttempt* open_attempt = nullptr) {
    produce(output->back, pixel, scene, reset);
    const auto calls = model_calls;
    const auto creations = next_handle;
    const auto initializations = control_mask_gpu_cases::runtime_initializations;
    const auto ordinary = ordinary_submissions(output);
    const auto sdk_submissions = xefg::SubmittedFrames();
    const auto application_frame = session.context->application_frame;
    const auto reads = sdk_reads;
    model_depth = model_motion = nullptr;
    const auto first_execute = next_execute();
    const auto ordinary_real_guides = native::present_guides_real.load();
    const auto attempt = call_ordinary(output);
    const auto ordinary_delta = ordinary_submissions(output) - ordinary;
    std::pair<uint64_t, uint64_t> submitted_recording;
    if (ordinary_delta != 0) {
      const auto& ring = native::present_ring;
      const auto slot = std::ranges::find_if(ring.slots, [&](const auto& candidate) {
        return candidate.value == ring.submitted && candidate.list != nullptr;
      });
      if (slot != std::ranges::end(ring.slots)) {
        submitted_recording = submission::RecordingIdentity(slot->list);
      }
    }
    size_t ordinary_color_writes = 0;
    bool ordinary_color_matches_slot = false;
    nlohmann::json color_recordings = nlohmann::json::array();
    const auto color_identity = renodx::utils::directx::NativeIdentity(output->back);
    observe_color_submissions(submission::RecordingOwner::kOrdinaryPresent, color_identity, first_execute,
        [&](uint64_t list_id, uint64_t generation, const auto& recorded) {
          ordinary_color_writes += recorded.submissions.size();
          ordinary_color_matches_slot |= submitted_recording == std::pair{list_id, generation};
          color_recordings.push_back({{"list", list_id}, {"generation", generation}, {"execute", recorded.first_execute},
                                     {"submissions", recorded.submissions.size()}});
        });
    if (sdk) {
      sdk_present(&session, scene, reset);
    } else {
      wait_fence(native::present_ring.fence, native::present_ring.submitted);
      const auto pixels = Capture(gpu, output->back, present_state);
      submit();
      consumed = pixels.Pixels();
    }
    if (open_attempt != nullptr) {
      Check(find_attempt(output) == attempt, "the current ordinary attempt is available before the consumer begins");
      *open_attempt = attempt;
    } else {
      if (!sdk) native::EndOrdinaryPresentAttempt(attempt, S_OK);
      Check(!find_attempt(output), "consumer completion closes this ordinary Present attempt");
    }
    const auto evaluations = model_calls - calls;
    const int observed_reset = (evaluations == 0 ? -1 : model_resets.back());
    const auto sdk_delta = xefg::SubmittedFrames() - sdk_submissions;
    const bool tagged_guides = scene >= 0 && model_depth == depths[scene].Get() && model_motion == motions[scene].Get();
    bool neutral_guides = false;
    if (sdk) {
      const auto stream = native::fg_service::streams.find({false, reinterpret_cast<uintptr_t>(session.handle)});
      if (stream != native::fg_service::streams.end() && model_depth != nullptr && model_motion != nullptr) {
        const auto& role = stream->second.roles[0];
        neutral_guides = model_depth == role.neutral_depth && model_motion == role.neutral_motion;
      }
    } else if (const auto stream = native::present_streams.find(output); stream != native::present_streams.end()
               && model_depth != nullptr && model_motion != nullptr) {
      neutral_guides = model_depth == stream->second.neutral_depth && model_motion == stream->second.neutral_motion;
    }
    const bool pixels_match = consumed == expected_pixels(pixel, edited);
    const bool delivered = evaluations == static_cast<unsigned>(edited)
        && ordinary_color_writes == static_cast<size_t>(edited && !sdk)
        && ordinary_color_matches_slot == (edited && !sdk)
        && sdk_delta == (edited && sdk) && sdk_reads - reads == sdk
        && session.context->application_frame - application_frame == sdk && pixels_match;
    const bool valid = delivered && (expected_reset < 0 || observed_reset == expected_reset)
        && (!edited || (sdk && scene >= 0 ? tagged_guides : neutral_guides));
    std::cout << "XEFG_HANDOFF_FRAME " << nlohmann::json{
        {"phase", stage}, {"version", ++version}, {"output", output == &route ? "primary" : "other"},
        {"acceptance_frame", check}, {"sdk_on", session.context->enabled}, {"sdk_epoch", session.context->epoch},
        {"application_frame", session.context->application_frame}, {"id", sdk && scene >= 0 ? present_id : 0},
        {"source_generation", source_generation}, {"game_reset", reset}, {"model_reset", observed_reset},
        {"tagged_guides", tagged_guides}, {"neutral_guides", neutral_guides}, {"evaluates", evaluations},
        {"creates", next_handle - creations}, {"inits", control_mask_gpu_cases::runtime_initializations - initializations},
        {"ordinary_present_submissions", ordinary_delta}, {"ordinary_color_writes", ordinary_color_writes},
        {"ordinary_color_matches_slot", ordinary_color_matches_slot},
        {"ordinary_color_recordings", std::move(color_recordings)},
        {"ordinary_real_guides_selected", native::present_guides_real.load() - ordinary_real_guides},
        {"sdk_submissions", sdk_delta}, {"sdk_reads", sdk_reads - reads},
        {"sdk_result", sdk ? static_cast<unsigned>(xefg::CurrentResult()) : 0}, {"expected_edited", edited},
        {"source_rgba", pixel}, {"rgba", consumed}, {"delivery_matches", delivered}, {"passed", (check ? valid : delivered)}}.dump()
              << std::endl;
    if (check) {
      accepted &= valid;
    }
    return (check ? valid : delivered);
  };

  const auto warm = [&](const char* stage, PresentRingFaceProbe* output, bool sdk) {
    for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8; ++attempt) {
      if (frame(stage, output, scene_a, (sdk ? 0 : -1), false, sdk, true, -1, false)) return;
    }
    Check(false, "establish the fixture's initial output before checking first-frame handoffs");
  };
  const auto fail_first_tracker_signal = [&](const auto& operation) {
    using namespace control_mask_gpu_cases;
    namespace vtable = renodx::utils::vtable;
    vtable::Slot<SignalFn> signal_probe{.object = gpu.queue.Get(), .index = 14,
        .original = &real_signal, .replacement = Signal};
    vtable::Install(&signal_probe);
    const auto restore_signal = [](vtable::Slot<SignalFn>* slot) {
      signal_fault_fence = nullptr;
      vtable::Uninstall(slot);
    };
    const std::unique_ptr<vtable::Slot<SignalFn>, decltype(restore_signal)> signal_owner(&signal_probe, restore_signal);
    {
      std::lock_guard lock(addon::queue_completion_mutex);
      signal_fault_fence = addon::queue_completions.at(gpu.queue.Get()).fence;
    }
    Check(signal_fault_fence != nullptr, "the real tracker has a scoped first Signal target");
    const auto failures = injected_failures;
    operation();
    Check(injected_failures == failures + 1, "fail exactly the first post-Execute tracker Signal");
  };
  const auto release_fixture = [&] {
    Check(NVSDK_NGX_SUCCEED(native::HookedReleaseFeatureSlot<0>(source)), "release the rebuilt SR source");
    const addon::ngx_serial::PresentTurn turn;
    Check(turn.owns(), "handoff cleanup owns the runtime turn");
    const addon::RuntimeLock lock(native::runtime_mutex);
    Check(native::ReleasePresentRing(), "handoff cleanup retires the completed ordinary ring");
    for (auto* output : {&route, &other_route}) {
      native::RetirePresentStream(output);
      rate::DestroyPresentOutput(reinterpret_cast<uintptr_t>(output));
    }
    native::DrainRetiredResources(true);
  };
  if (ordinary_signal_loss_only) {
    Check(Hooks::Enable(session.handle, 0) == 0 && !session.context->enabled, "Signal fixture starts with ordinary ownership");
    warm("ordinary-receipt-signal-prefix", &route, false);
    produce(back.Get(), menu, -1, false);
    const auto calls = model_calls;
    const auto deliveries = xefg::SubmittedFrames();
    const auto reads = sdk_reads;
    xefg::OrdinaryPresentAttempt attempt;
    fail_first_tracker_signal([&] { attempt = call_ordinary(&route); });
    wait_fence(native::present_ring.fence, native::present_ring.submitted);
    xefg::OrdinaryPresentReceipt receipt;
    {
      const addon::RuntimeLock lock(native::runtime_mutex);
      receipt = native::ordinary_present_attempts.at(&route).receipt;
    }
    Check(receipt.attempt == attempt && receipt.executed && receipt.output_ready && receipt.group_complete
              && receipt.submission.signaled && receipt.submission.fence_value == native::present_ring.submitted
              && receipt.first_state == submission::SubmissionProofState::kSignalFailed && !receipt.proofs_healthy
              && model_calls == calls + 1, "ordinary color executed once and its own fence completed without a first tracker proof");
    Check(Hooks::Enable(session.handle, 1) == 0 && find_attempt(&route) == attempt,
          "the SDK takes the same output and sees the actual unproven ordinary receipt");
    auto sdk = adapter::BeginPresent(session.proxy.Get(), 0);
    Check(sdk.ordinary_claim == xefg::OrdinaryPresentClaim::CONFLICT && sdk.ordinary_receipt.attempt == attempt
              && sdk.ordinary_receipt.first_state == submission::SubmissionProofState::kSignalFailed
              && !sdk.color_executed && sdk.service_result == fg::Result::NOT_SERVED,
          "missing first tracker proof forbids claiming the ordinary result or writing it again");
    const auto status = nr_edit_reuse_xefg_cases::ConsumeSdkPresent(session.proxy.Get(), 0, 0);
    adapter::EndPresent(&sdk, status);
    Check(status == S_OK && xefg::CurrentResult() == fg::Result::NOT_SERVED && !find_attempt(&route)
              && sdk_reads == reads + 1 && model_calls == calls + 1 && xefg::SubmittedFrames() == deliveries
              && consumed == expected_pixels(menu, true) && !submission::ResourceReleasable(back.Get()),
          "SDK consumes the actual RGBA without a second enhancement or a manufactured lifetime proof");
    release_fixture();
    std::cout << "PASS xefg-ordinary-receipt-first-signal evidence=WARP+MOCK first_proof=failed owner_fence=complete"
                 " claim=conflict single_write=1" << std::endl;
    return;
  }
  warm("initial-sdk", &route, true);
  frame("on-last-scene-a", &route, scene_a, 0, false, true, true, 0);
  frame("on-menu-no-id", &route, menu, -1, false, true, true, 1);
  frame("on-menu-continued-no-id", &route, menu, -1, false, true, true, 0);
  rebuild_source();
  frame("on-first-scene-b-rebuilt", &route, scene_b, 1, true, true, true, 1);
  frame("on-scene-b-steady", &route, scene_b, 1, false, true, true, 0);
  const auto last_b = consumed;
  const auto last_b_calls = model_calls;
  const auto last_b_submissions = xefg::SubmittedFrames();
  const auto last_b_frame = session.context->application_frame;
  const auto last_b_reads = sdk_reads;
  session.Present(present_id);
  const bool duplicate_ok = model_calls == last_b_calls && xefg::SubmittedFrames() == last_b_submissions
      && session.context->application_frame == last_b_frame && sdk_reads == last_b_reads + 1 && consumed == last_b;
  accepted &= duplicate_ok;
  std::cout << "XEFG_HANDOFF_DUPLICATE id=" << present_id << " app_frame=" << last_b_frame
            << " single_write=" << duplicate_ok << std::endl;

  frame("off-last-scene-a", &route, scene_a, 0, true, true, true, 1);
  {
    uint64_t first_sdk_execute = 0;
    const auto color_identity = renodx::utils::directx::NativeIdentity(back.Get());
    const auto consume_original = nr_edit_reuse_xefg_cases::consume_sdk_present;
    std::vector<submission::GenerationSubmission> sdk_writes;
    nlohmann::json sdk_recordings = nlohmann::json::array();
    funnel_accounting_tests::Restore consumer_restore;
    consumer_restore.Value(&nr_edit_reuse_xefg_cases::consume_sdk_present);
    nr_edit_reuse_xefg_cases::consume_sdk_present = [&](IDXGISwapChain* output) {
      // The consumer's GPU readback retires completed tracker generations.
      observe_color_submissions(submission::RecordingOwner::kXeFg, color_identity, first_sdk_execute,
          [&](uint64_t list_id, uint64_t generation, const auto& recorded) {
            sdk_writes.insert(sdk_writes.end(), recorded.submissions.begin(), recorded.submissions.end());
            sdk_recordings.push_back({{"list", list_id}, {"generation", generation}, {"execute", recorded.first_execute}});
          });
      consume_original(output);
    };
    struct ReceiptCase {
      const char* ordinary_phase;
      const char* next_phase;
      HRESULT sdk_status;
      int ordinary_reset;
      bool raw_sdk_reset;
    };
    constexpr ReceiptCase receipt_cases[]{
        {"off-first-menu-ordinary", "after-enable-next-produced-menu", S_OK, 1, false},
        {"off-menu-before-sdk-failure", "after-failed-end-next-produced-menu", E_FAIL, -1, false},
        {"off-menu-before-sdk-reset", "after-sdk-reset-next-produced-menu", S_OK, -1, true},
    };
    const auto& game_resets = native::history_reset_counts[static_cast<size_t>(native::HistoryResetSource::kGameReset)];
    for (const auto& scenario : receipt_cases) {
      const bool sdk_success = scenario.sdk_status == S_OK;
      Check(Hooks::Enable(session.handle, 0) == 0 && !session.context->enabled, "SDK actually relinquishes this output");
      const auto before_ordinary = model_calls;
      const auto resets_before_ordinary = game_resets.load();
      xefg::OrdinaryPresentAttempt ordinary_attempt;
      Check(frame(scenario.ordinary_phase, &route, menu, -1, false, false, true,
                  scenario.ordinary_reset, true, &ordinary_attempt)
                && model_calls == before_ordinary + 1,
            "ordinary Present writes the complete menu version once before SDK BeginPresent");
      const auto ordinary_pixels = consumed;
      const auto menu_production = produced_colors;
      const auto reads = sdk_reads;
      const auto submissions = xefg::SubmittedFrames();
      first_sdk_execute = next_execute();
      sdk_writes.clear();
      sdk_recordings.clear();
      Check(native::present_guides == native::kPresentGuidesOptional && !session.context->ui_composition,
            "the SDK accepts the untagged menu with Optional guides");
      Check(Hooks::Enable(session.handle, 1) == 0 && session.context->enabled
                && find_attempt(&route) == ordinary_attempt, "SDK Enable retains the in-progress ordinary attempt");
      const auto resets_at_sdk_begin = game_resets.load();
      nr_edit_reuse_xefg_cases::sdk_present_result = scenario.sdk_status;
      sdk_present(&session, scenario.raw_sdk_reset ? 0 : -1, scenario.raw_sdk_reset, scenario.sdk_status);
      nr_edit_reuse_xefg_cases::sdk_present_result = S_OK;
      const auto resets_after_sdk = game_resets.load();
      const auto result = xefg::CurrentResult();
      const auto model_evaluates = model_calls - before_ordinary;
      const auto sdk_deliveries = xefg::SubmittedFrames() - submissions;
      const bool same_version = produced_colors == menu_production && route.back == back.Get();
      const bool preserved_rgba = consumed == ordinary_pixels;
      const bool finished = !find_attempt(&route);
      const bool single_write = same_version && model_evaluates == 1 && sdk_deliveries == 0 && sdk_writes.empty()
          && sdk_reads == reads + 1 && preserved_rgba && finished
          && result == (sdk_success ? fg::Result::ORDINARY_SUBMITTED : fg::Result::FAILED)
          && (!scenario.raw_sdk_reset || (resets_at_sdk_begin == resets_before_ordinary
              && resets_after_sdk == resets_at_sdk_begin + 1));
      accepted &= single_write;
      std::cout << "XEFG_HANDOFF_SAME_VERSION " << nlohmann::json{{"version", version}, {"same_version", same_version},
          {"model_evaluates", model_evaluates}, {"ordinary_bb_writes", 1}, {"sdk_bb_writes", sdk_writes.size()},
          {"sdk_deliveries", sdk_deliveries}, {"sdk_recordings", sdk_recordings}, {"same_rgba", preserved_rgba},
          {"sdk_success", sdk_success}, {"result", static_cast<unsigned>(result)}, {"finished", finished},
          {"raw_sdk_reset", scenario.raw_sdk_reset}, {"game_reset_delta", resets_after_sdk - resets_at_sdk_begin},
          {"passed", single_write}}.dump() << std::endl;

      sdk_writes.clear();
      sdk_recordings.clear();
      first_sdk_execute = next_execute();
      const bool next_delivered = frame(scenario.next_phase, &route, scene_a, -1, false, true, true,
                                        scenario.raw_sdk_reset ? 1 : -1);
      const bool next_completed = sdk_writes.size() == 1
          && submission::ReadSubmission(sdk_writes.front()).state == submission::SubmissionProofState::kComplete;
      const auto& evaluate = native::stage_telemetry[static_cast<size_t>(addon::stage_plan::Stage::PRESENT)].last_evaluate;
      const bool game_reset_applied = evaluate.reset_sent && (evaluate.reset_reasons & native::kResetGame) != 0
          && game_resets.load() == resets_after_sdk;
      const bool next_passed = next_delivered && next_completed && produced_colors == menu_production + 1
          && xefg::CurrentResult() == fg::Result::SUBMITTED && (!scenario.raw_sdk_reset || game_reset_applied);
      accepted &= next_passed;
      std::cout << "XEFG_HANDOFF_NEXT_VERSION " << nlohmann::json{{"version", version},
          {"new_color_produced", produced_colors == menu_production + 1}, {"sdk_bb_writes", sdk_writes.size()},
          {"sdk_recordings", sdk_recordings}, {"completed", next_completed}, {"after_sdk_success", sdk_success},
          {"raw_sdk_reset", scenario.raw_sdk_reset}, {"game_reset_applied", game_reset_applied},
          {"passed", next_passed}}.dump() << std::endl;
      if (scenario.raw_sdk_reset && single_write && next_passed) {
        std::cout << "PASS xefg-ordinary-receipt-game-reset evidence=WARP+MOCK game_reset_delta=1"
                     " same_version_write=1 sdk_writes=0 next_version_reset=1" << std::endl;
      }
    }
  }
  warm("other-ordinary-prefix", &other_route, false);
  xefg::OrdinaryPresentAttempt other_attempt;
  frame("another-output-ordinary-while-sdk-on", &other_route, scene_b, -1, false, false, true, 0, true, &other_attempt);
  rebuild_source();
  frame("reenabled-first-scene-b-rebuilt", &route, scene_b, 1, true, true, true, 1);
  Check(find_attempt(&other_route) == other_attempt,
        "SDK consumes its own BB without adopting or closing the ordinary receipt of another output");
  frame("reenabled-scene-b-steady", &route, scene_b, 1, false, true, true, 0);
  xefg::OrdinaryPresentAttempt next_other_attempt;
  frame("ordinary-no-sdk-next-begin", &other_route, menu, -1, false, false, true, -1, true, &next_other_attempt);
  Check(other_attempt.owner == next_other_attempt.owner && other_attempt.serial < next_other_attempt.serial,
        "a new real Begin supersedes the previous attempt when no finish callback arrived");
  native::EndOrdinaryPresentAttempt(other_attempt, E_FAIL);
  Check(find_attempt(&other_route) == next_other_attempt, "late ordinary Finish leaves the newer output attempt active");
  native::EndOrdinaryPresentAttempt(next_other_attempt, S_OK);
  Check(!find_attempt(&other_route), "successful finish without SDK closes only the current attempt");

  session.context->ui_composition = true;
  frame("required-hudless-missing", &route, menu, -1, false, true, false, -1);
  session.context->ui_composition = false;

  {
    const auto hudless = gpu.Texture(size, size, DXGI_FORMAT_R8G8B8A8_UNORM, present_state);
    upload(hudless.Get(), scene_b, present_state);
    submit();
    Check(Hooks::Enable(session.handle, 0) == 0, "SDK relinquishes the BB before the ordinary-only color");
    xefg::OrdinaryPresentAttempt ordinary_attempt;
    Check(frame("ordinary-before-new-hudless", &route, menu, -1, false, false, true, -1, true, &ordinary_attempt),
          "ordinary attempt completes its BB color before the SDK receives a distinct HUDLess");
    const auto ordinary_pixels = consumed;
    const auto calls = model_calls;
    const auto deliveries = xefg::SubmittedFrames();
    Check(Hooks::Enable(session.handle, 1) == 0, "SDK takes the same output with a necessary HUDLess");
    session.context->ui_composition = true;
    session.Tag(++present_id, HUDLESS, hudless.Get());
    submit();
    Check(Hooks::PresentId(session.handle, present_id) == 0, "the SDK receives the actual new HUDLess tag");
    auto sdk = adapter::BeginPresent(session.proxy.Get(), 0);
    Check(sdk.input.hudless == hudless.Get() && sdk.input.hudless != sdk.input.backbuffer
              && sdk.ordinary_claim == xefg::OrdinaryPresentClaim::CONFLICT && sdk.ordinary_receipt.attempt == ordinary_attempt
              && std::string_view(sdk.handoff_reason) == "ordinary_hudless_uncovered"
              && !sdk.color_executed && sdk.service_result == fg::Result::NOT_SERVED && model_calls == calls,
          "a complete BB receipt cannot stand in for the newly required HUDLess or allow a second BB enhancement");
    const auto status = nr_edit_reuse_xefg_cases::ConsumeSdkPresent(session.proxy.Get(), 0, 0);
    Check(status == S_OK && consumed == ordinary_pixels, "the actual SDK consumer reads only the ordinary enhanced BB");
    const auto hudless_capture = Capture(gpu, hudless.Get(), present_state);
    submit();
    Check(hudless_capture.Pixels() == expected_pixels(scene_b, false), "the rejected group keeps every HUDLess RGBA channel");
    session.context->ui_composition = false;

    produce(back.Get(), scene_a, -1, false);
    const auto next_attempt = call_ordinary(&route);
    Check(next_attempt.owner == ordinary_attempt.owner && next_attempt.serial > ordinary_attempt.serial,
          "a new ordinary attempt exists before the old SDK End returns");
    adapter::EndPresent(&sdk, status);
    Check(find_attempt(&route) == next_attempt && xefg::SubmittedFrames() == deliveries && model_calls == calls,
          "the delayed SDK End settles only its old receipt and leaves the newer attempt active");
    sdk_present(&session, -1, false);
    Check(!find_attempt(&route) && xefg::CurrentResult() == fg::Result::SUBMITTED
              && xefg::SubmittedFrames() == deliveries + 1 && model_calls == calls + 1
              && consumed == expected_pixels(scene_a, true),
          "the next actual BB version delivers after the late SDK End");
  }

  const auto create_list = [&] {
    ExternalList external;
    Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
              IID_PPV_ARGS(&external.allocator)))
              && SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, external.allocator.Get(),
                  nullptr, IID_PPV_ARGS(&external.list)))
              && native::InstallCommandListStateHooks(external.list.Get()), "borrowed WARP list and actual list hooks");
    return external;
  };
  const auto close_list = [](ExternalList* external) {
    Check(SUCCEEDED(external->list->Close()), "game closes its actual list for execution");
    external->recording = submission::ObserveRecording(external->list.Get());
    Check(external->recording != nullptr && !external->recording->closed.load()
              && !external->recording->submitted.load(), "game Close keeps this tracked generation executable");
  };
  const auto reset_list = [](ExternalList* external) {
    Check(SUCCEEDED(external->allocator->Reset()) && SUCCEEDED(external->list->Reset(external->allocator.Get(), nullptr))
              && external->recording->closed.load(), "actual game Reset ends this generation's replay eligibility");
    Check(SUCCEEDED(external->list->Close()), "close the empty replacement generation");
  };
  const auto execute_and_wait = [&](ID3D12CommandQueue* queue, const ExternalList& external) {
    ID3D12CommandList* lists[]{external.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    Check(SUCCEEDED(queue->Signal(gpu.fence.Get(), ++gpu.serial)), "fixture queues a separate completion fence");
    wait_fence(gpu.fence.Get(), gpu.serial);
    Check(external.recording->submitted.load() && !external.recording->closed.load(),
          "real Execute is observed and completed without closing the borrowed generation");
  };
  const auto read = [&](ID3D12Resource* target) {
    const auto snapshot = Capture(gpu, target, present_state);
    submit();
    return snapshot.Pixels();
  };
  const auto trace_gpu_color = [&](const char* stage, Pixel expected) {
    const auto bytes = read(back.Get());
    std::cout << "XEFG_HANDOFF_EXTERNAL_PIXELS " << nlohmann::json{
        {"phase", stage}, {"rgba", bytes}, {"gpu_completed", true}, {"expected_rgba", expected}}.dump() << std::endl;
    Check(bytes == expected_pixels(expected, false), "real external list changes the entire primary BB to its old RGBA");
  };

  Check(Hooks::Enable(session.handle, 0) == 0, "release SDK ownership before the independent guide capture");
  warm("guide-ordinary-prefix", &route, false);
  Check(!native::direct_api.private_instance, "guide departure starts on the ordinary provider");
  auto guide = create_list();
  ++native::present_generation;
  const auto prior_capture = native::present_captures.load();
  for (auto* resource : {depths[1].Get(), motions[1].Get()}) {
    native::Transition(guide.list.Get(), resource, present_state, NATIVE_INPUT_STATE);
  }
  {
    const addon::ngx_serial::PresentTurn turn;
    Check(turn.owns(), "independent game capture owns the NGX turn");
    const addon::RuntimeLock lock(native::runtime_mutex);
    const addon::EvaluateChainScope chain;
    native::CapturePresentGuides(guide.list.Get(), source, &parameters, NATIVE_INPUT_STATE, NATIVE_INPUT_STATE);
  }
  for (auto* resource : {depths[1].Get(), motions[1].Get()}) {
    native::Transition(guide.list.Get(), resource, NATIVE_INPUT_STATE, present_state);
  }
  Check(native::present_captures.load() == prior_capture + 1, "borrowed list records only the real ordinary guide clones");
  const auto latest_guide = std::ranges::max_element(native::present_guide_sets, {}, &native::PresentGuideSet::serial);
  auto* const retained_depth = latest_guide->depth;
  auto* const retained_motion = latest_guide->motion;
  const void* const retained_marker = latest_guide->marker;
#if defined(NR_NEXUS_DEV_CONTROL)
  namespace benchmark = addon::control_mask_capture::benchmark;
  const std::array<const void*, 1> provider_objects{&native::direct_api};
  Check(submission::ProviderTeardownWorkState(provider_objects) == submission::RecordedWork::kComplete,
        "without benchmark the independent guide recording permits provider retirement");
  Check(benchmark::Arm(8, 30, nullptr, nullptr), "arm the real benchmark owner across the provider handoff");
  const auto observe_source = [&](ID3D12GraphicsCommandList* list) {
    {
      benchmark::Scope scope(reinterpret_cast<uintptr_t>(source), list);
      native::ObserveSrBenchmarkResult(&scope, list, source, &parameters, NVSDK_NGX_Result_Success,
                                       true, false, addon::gpu_timers::NowNs());
    }
    std::shared_lock lock(benchmark::state.mutex);
    Check(!benchmark::state.rows.empty() && benchmark::state.rows.back().wrapper_finalized,
          "the actual benchmark Scope finalizes the mock SR result");
    const auto& row = benchmark::state.rows.back();
    Check(row.frame.recording != nullptr && row.frame.recording->list != 0
              && row.frame.recording->generation != 0,
          "the benchmark row owns the exact source recording status");
    return row.frame.recording;
  };
  const auto benchmark_recording = observe_source(guide.list.Get());
  bool benchmark_object_mapped = false;
  bool benchmark_kind_unknown = false;
  bool benchmark_cpu_observation = false;
  {
    std::lock_guard lock(submission::tracker_mutex);
    const auto& generation = submission::tracked_lists.at(benchmark_recording->list)
                                 .generations.at(benchmark_recording->generation);
    Check(generation.status == benchmark_recording, "the benchmark row and tracker refer to the same status object");
    for (const auto& use : generation.classified_uses) {
      if (use.object != benchmark_recording.get()) continue;
      benchmark_object_mapped = true;
      benchmark_kind_unknown |= use.kind == submission::UseKind::kUnknown;
      benchmark_cpu_observation |= use.kind == submission::UseKind::kIndependentCpuObservation;
    }
  }
  std::vector<submission::RecordingBlocker> benchmark_blockers;
  const auto benchmark_gate = submission::ProviderTeardownWorkState(provider_objects, &benchmark_blockers);
  bool benchmark_object_blocks = false;
  for (const auto& blocker : benchmark_blockers) {
    if (blocker.list != benchmark_recording->list || blocker.generation != benchmark_recording->generation) continue;
    benchmark_object_blocks = std::ranges::any_of(blocker.uses, [&](const auto& use) {
      return use.object == benchmark_recording.get() && use.kind == submission::UseKind::kUnknown;
    });
  }
  std::cout << "SR_BENCHMARK_PROVIDER mapping=" << benchmark_object_mapped
            << " kind_unknown=" << benchmark_kind_unknown << " exact_object_blocker=" << benchmark_object_blocks
            << " recording=" << benchmark_recording->list << '/' << benchmark_recording->generation << std::endl;
  Check(benchmark_object_mapped && benchmark_gate == submission::RecordedWork::kComplete,
        "arming benchmark preserves the independent guide's provider retirement permission");
  Check(benchmark_cpu_observation && !benchmark_kind_unknown,
        "the pure CPU benchmark observation has its own provider-independent classification");
  for (const auto kind : {submission::UseKind::kUnknown, submission::UseKind::kProvider}) {
    auto blocking = create_list();
    const auto observation = observe_source(blocking.list.Get());
    submission::SetRecordingOwner(blocking.list.Get(), submission::RecordingOwner::kBorrowedGame);
    if (kind == submission::UseKind::kUnknown) {
      // The old benchmark call used this exact default classification.
      submission::TrackUse(blocking.list.Get(), observation.get());
    } else {
      submission::TrackUse(blocking.list.Get(), &native::direct_api, kind);
    }
    Check(submission::ProviderTeardownWorkState(provider_objects) == submission::RecordedWork::kPending,
          "an unsubmitted unknown or real provider use remains a provider retirement blocker");
    close_list(&blocking);
    execute_and_wait(gpu.queue.Get(), blocking);
    Check(submission::ProviderTeardownWorkState(provider_objects) == submission::RecordedWork::kReplayable,
          "healthy completion still retains unknown and provider uses while the recording can replay");
    {
      const addon::ngx_serial::PresentTurn turn;
      Check(turn.owns(), "provider rejection owns the existing NGX turn");
      const addon::RuntimeLock lock(native::runtime_mutex);
      auto decline = addon::NrDeclineReason::kNrRuntimeUnavailable;
      Check(!native::EnsureDirectRuntime(blocking.list.Get(), &decline, true)
                && decline == addon::NrDeclineReason::kRuntimeSwitchPending && !native::direct_api.private_instance,
            "the actual provider handoff waits for replayable unknown and provider uses");
    }
    reset_list(&blocking);
    submission::PruneCompletedGenerations();
    Check(submission::ProviderTeardownWorkState(provider_objects) == submission::RecordedWork::kComplete
              && submission::ResourceReleasable(observation.get()),
          "Reset and healthy completion retire the blocking generation while benchmark rows still own its status");
  }
#endif
  close_list(&guide);
  Check(Hooks::Enable(session.handle, 1) == 0, "real SDK handoff starts while the independent guide is unsubmitted");
  const bool guide_handoff = frame("independent-unsubmitted-guide-allows-first-sdk", &route, scene_b, 1, true, true, true, 1);
  const bool guide_retained = !guide.recording->submitted.load() && !guide.recording->closed.load()
      && !submission::ResourceReleasable(retained_depth) && !submission::ResourceReleasable(retained_motion)
      && !submission::ResourceReleasable(retained_marker);
  accepted &= guide_handoff && guide_retained && native::direct_api.private_instance;
#if defined(NR_NEXUS_DEV_CONTROL)
  Check(benchmark::active.load() && benchmark_recording == guide.recording && guide_handoff,
        "the real provider handoff accepts the same retained guides while benchmark remains active");
  Check(benchmark::Stop("provider_handoff_fixture", nullptr, true), "stop the benchmark through its actual owner");
  Check(!benchmark_recording->submitted.load() && !benchmark_recording->closed.load()
            && !submission::ResourceReleasable(benchmark_recording.get())
            && !submission::ResourceReleasable(retained_depth) && !submission::ResourceReleasable(retained_motion),
        "benchmark Stop cannot retire an unsubmitted source recording or its retained guides");
  benchmark::Tick(addon::gpu_timers::NowNs(), false);
  benchmark::write_pending.store(false);
#endif
  execute_and_wait(gpu.queue.Get(), guide);
  Check(!submission::ResourceReleasable(retained_depth) && !submission::ResourceReleasable(retained_motion)
            && !submission::ResourceReleasable(retained_marker), "completed external guide remains alive while replayable");
  execute_and_wait(gpu.queue.Get(), guide);
  const auto depth_copy = gpu.CreateTextureTransfer(retained_depth);
  const auto motion_copy = gpu.CreateTextureTransfer(retained_motion);
  for (const auto& [resource, transfer] : {std::pair{retained_depth, &depth_copy}, std::pair{retained_motion, &motion_copy}}) {
    gpu.Transition(resource, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.RecordTextureReadback(resource, *transfer);
    gpu.Transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
  }
  submit();
  const auto depth_bytes = gpu.ReadTexture(depth_copy);
  const auto motion_bytes = gpu.ReadTexture(motion_copy);
  const std::array<uint8_t, 4> expected_depth{0, 0, 0x40, 0x3f};
  const std::array<uint8_t, 4> expected_motion{0, 0x3c, 0, 0};
  const auto guide_matches = [](const auto& bytes, const auto& pixel) {
    if (bytes.size() != size * size * pixel.size()) return false;
    for (size_t offset = 0; offset < bytes.size(); offset += pixel.size()) {
      if (!std::equal(pixel.begin(), pixel.end(), bytes.begin() + offset)) return false;
    }
    return true;
  };
  Check(guide_matches(depth_bytes, expected_depth) && guide_matches(motion_bytes, expected_motion),
        "independent real guide clones retain every expected depth and motion pixel after replay");
  reset_list(&guide);
  submission::PruneCompletedGenerations();
  const bool guide_retired = submission::ResourceReleasable(retained_depth) && submission::ResourceReleasable(retained_motion)
      && submission::ResourceReleasable(retained_marker);
  accepted &= guide_retired;
#if defined(NR_NEXUS_DEV_CONTROL)
  {
    std::shared_lock rows_lock(benchmark::state.mutex);
    std::lock_guard tracker_lock(submission::tracker_mutex);
    Check(benchmark::state.rows.front().frame.recording == benchmark_recording
              && benchmark_recording->closed.load() && benchmark_recording->submitted.load()
              && benchmark_recording->completed_healthy && !submission::live_uses.contains(benchmark_recording.get()),
          "the benchmark row can keep its status after replay, Reset and healthy GPU completion pruned its tracker use");
  }
  std::cout << "SR_BENCHMARK_PROVIDER active_handoff=accepted stop=source-retained"
               " unknown-and-provider=replay-blocked row-retained-after-prune=1 evidence=WARP+MOCK\n";
  {
    std::unique_lock lock(benchmark::state.mutex);
    benchmark::state.rows.clear();
    benchmark::state.sources.clear();
    benchmark::state.status = "idle";
  }
#endif
  std::cout << "XEFG_HANDOFF_GUIDES " << nlohmann::json{{"unsubmitted_handoff", guide_handoff},
      {"retained", guide_retained}, {"replayed", true}, {"depth_bytes", depth_bytes}, {"motion_bytes", motion_bytes},
      {"releasable_after_reset", guide_retired}}.dump() << std::endl;

  constexpr Pixel old_a{236, 29, 83, 52};
  constexpr Pixel old_b{51, 244, 103, 96};
  auto stale_a = gpu.Texture();
  auto stale_b = gpu.Texture();
  upload(stale_a.Get(), old_a, present_state);
  upload(stale_b.Get(), old_b, present_state);
  submit();
  const auto canonical_back = renodx::utils::directx::NativeIdentity(back.Get());
  const submission::OutputUse primary_write{.resource = canonical_back, .subresource = 0};
  const auto record_writer = [&](ID3D12Resource* stale_color, uint64_t recorded_version) {
    auto external = create_list();
    native::Transition(external.list.Get(), stale_color, present_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    native::Transition(external.list.Get(), back.Get(), present_state, D3D12_RESOURCE_STATE_COPY_DEST);
    external.list->CopyResource(back.Get(), stale_color);
    native::Transition(external.list.Get(), back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, present_state);
    native::Transition(external.list.Get(), stale_color, D3D12_RESOURCE_STATE_COPY_SOURCE, present_state);
    // The fixture models a host-proven output use for this explicit copy.
    // Both recordings really target subresource zero of the DXGI backbuffer.
    submission::SetRecordingOwner(external.list.Get(), submission::RecordingOwner::kBorrowedGame);
    submission::TrackUse(external.list.Get(), back.Get(), submission::UseKind::kOutputColor,
        {.resource = canonical_back, .subresource = 0, .version = recorded_version});
    close_list(&external);
    return external;
  };
  const auto trace_blockers = [&](const char* stage, submission::RecordedWork expected, size_t external_count) {
    std::vector<submission::RecordingBlocker> blockers;
    const auto work = submission::OutputWorkState(primary_write, {}, &blockers);
    nlohmann::json observed = nlohmann::json::array();
    for (const auto& blocker : blockers) {
      if (blocker.owner != submission::RecordingOwner::kBorrowedGame) continue;
      nlohmann::json submissions = nlohmann::json::array();
      for (const auto& receipt : blocker.submissions) {
        submissions.push_back({{"state", static_cast<unsigned>(receipt.state)}, {"value", receipt.value},
                               {"completed_value", receipt.completed_value}});
      }
      observed.push_back({{"list", blocker.list}, {"generation", blocker.generation},
          {"close_reason", static_cast<unsigned>(blocker.close_reason)}, {"first_execute", blocker.first_execute},
          {"first_signal", static_cast<unsigned>(blocker.first_submission.state)}, {"submissions", std::move(submissions)}});
    }
    std::cout << "XEFG_HANDOFF_BLOCKERS " << nlohmann::json{{"phase", stage},
        {"work", static_cast<unsigned>(work)}, {"external", observed}}.dump() << std::endl;
    Check((external_count == 0 || work == expected) && observed.size() == external_count,
          "tracker enumerates every matching external BB writer generation");
    return blockers;
  };
  auto writer_a = record_writer(stale_a.Get(), 1);
  auto writer_b = record_writer(stale_b.Get(), 2);
  trace_blockers("two-unsubmitted", submission::RecordedWork::kPending, 2);
  frame("writers-unsubmitted-b", &route, scene_b, 1, true, true, false, -1);
  execute_and_wait(gpu.queue.Get(), writer_a);
  trace_gpu_color("first-execute-a", old_a);
  execute_and_wait(gpu.queue.Get(), writer_b);
  trace_gpu_color("first-execute-b", old_b);
  trace_blockers("two-completed-still-replayable", submission::RecordedWork::kReplayable, 2);
  frame("writers-completed-b", &route, scene_b, 1, true, true, false, -1);
  Check(Hooks::Enable(session.handle, 0) == 0, "SDK Off transfers permission but leaves both game writers replayable");
  frame("ordinary-during-two-completed-writers", &route, menu, -1, false, false, false, -1);
  Check(Hooks::Enable(session.handle, 1) == 0, "SDK resumes while both game writers remain live");
  reset_list(&writer_a);
  execute_and_wait(gpu.queue.Get(), writer_b);
  trace_gpu_color("actual-replay-b-after-reset-a", old_b);
  trace_blockers("one-replayed-after-one-closed", submission::RecordedWork::kReplayable, 1);
  frame("writer-b-still-replayable", &route, scene_b, 1, true, true, false, -1);
  reset_list(&writer_b);
  trace_blockers("both-closed", submission::RecordedWork::kComplete, 0);
  frame("first-legal-b-after-both-close", &route, scene_b, 1, true, true, true, 1);

  const auto pending_model = [](Session* sdk_session, uint32_t creates, uint32_t evaluates, uint64_t deliveries) {
    const auto& stream = native::fg_service::streams.at({false, reinterpret_cast<uintptr_t>(sdk_session->handle)});
    const auto& model = native::features.at(stream.roles[0].Handle()).slots[0];
    Check(model.handle != nullptr && model.preparation_recording != nullptr && !model.creation_completed
              && model.preparation_recording->closed.load() && model.preparation_recording->submitted.load(),
          "cold application Present retains its actual private creation generation");
    Check(xefg::CurrentResult() == fg::Result::PREPARED && next_handle == creates && model_calls == evaluates
              && xefg::SubmittedFrames() == deliveries && stream.base_frame_clock.claims == 0
              && stream.base_frame_clock.outputs == 0 && stream.pending_xefg_game_reset
              && sdk_session->context->last_submission_frame == 0 && !submission::ResourceReleasable(model.handle),
          "pending creation keeps the model and game Reset without claiming or delivering an application version");
    return std::tuple{model.handle, model.preparation_recording, ComPtr<ID3D12Fence>(stream.fence), stream.submitted};
  };
  {
    Check(Hooks::Enable(session.handle, 0) == 0 && Hooks::Enable(session.handle, 1) == 0,
          "public SDK cycle starts a cold model on the existing output");
    produce(back.Get(), scene_b, 1, false);
    ComPtr<ID3D12Fence> gate;
    Check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "creation GPU gate");
    const auto release_gate = [](ID3D12Fence* fence) { fence->Signal(1); };
    std::unique_ptr<ID3D12Fence, decltype(release_gate)> unblock(gate.Get(), release_gate);
    funnel_accounting_tests::Restore readback_restore;
    readback_restore.Value(&nr_edit_reuse_xefg_cases::consume_sdk_present);
    nr_edit_reuse_xefg_cases::consume_sdk_present = {};
    const auto creates = next_handle + 1;
    const auto evaluates = model_calls;
    const auto deliveries = xefg::SubmittedFrames();
    const auto started = std::chrono::steady_clock::now();
    Check(SUCCEEDED(gpu.queue->Wait(gate.Get(), 1)), "queue holds the actual private Create in flight");
    sdk_present(&session, 1, true);
    const auto creation = pending_model(&session, creates, evaluates, deliveries);
    const auto& [model, recording, fence, value] = creation;
    Check(value != 0 && fence->GetCompletedValue() < value, "the owner Prepare Fence is genuinely in flight");
    std::this_thread::sleep_until(started + std::chrono::milliseconds(120));
    sdk_present(&session, 1, false);
    Check(pending_model(&session, creates, evaluates, deliveries) == creation && fence->GetCompletedValue() < value,
          "the next SDK application frame keeps the same pending cold Create through the GPU stall");
    const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    Check(waited_ms >= 100, "the actual creation is blocked for more than 100 ms");
    unblock.reset();
    wait_fence(fence.Get(), value);
    Check(read(back.Get()) == expected_pixels(scene_b, false), "preparation alone never changes the actual output");
    std::cout << "XEFG_HANDOFF_CREATION_GPU_PENDING " << nlohmann::json{{"held_ms", waited_ms},
        {"creates", 1}, {"claims", 0}, {"gpu_completed_after_release", true}}.dump() << std::endl;
  }
  frame("first-legal-b-after-creation-gate", &route, scene_b, 1, false, true, true, 1);
  const auto& last_evaluate = native::stage_telemetry[static_cast<size_t>(addon::stage_plan::Stage::PRESENT)].last_evaluate;
  Check(last_evaluate.reset_sent && (last_evaluate.reset_reasons & native::kResetGame) != 0,
        "the first legal application frame applies the actual Game Reset retained across both pending Presents");

  auto unsignaled = record_writer(stale_a.Get(), 3);
  fail_first_tracker_signal([&] { execute_and_wait(gpu.queue.Get(), unsignaled); });
  // DXGI writes stay on the swapchain's queue. Reset only the test's queue
  // tracker so the same real queue can provide valid evidence for the replay.
  Check(addon::ForgetQueueCompletion(gpu.queue.Get()), "fixture recreates only the faulted queue's completion tracker");
  std::cout << "XEFG_HANDOFF_SIGNAL first_signal=failed tracker_recreated_for_healthy_replay=1" << std::endl;
  trace_gpu_color("first-execute-a-without-tracker-signal", old_a);
  execute_and_wait(gpu.queue.Get(), unsignaled);
  trace_gpu_color("healthy-replay-a-cannot-repair-first-signal", old_a);
  reset_list(&unsignaled);
  const auto unknown = trace_blockers("first-signal-unproven-after-healthy-replay-and-reset", submission::RecordedWork::kPending, 1);
  const auto failed = std::ranges::find_if(unknown, [&](const auto& blocker) {
    return blocker.list == unsignaled.recording->list && blocker.generation == unsignaled.recording->generation;
  });
  Check(failed != unknown.end() && failed->close_reason == submission::CloseReason::kReset
            && failed->first_submission.state == submission::SubmissionProofState::kSignalFailed
            && failed->submissions.size() == 2
            && failed->submissions.back().state == submission::SubmissionProofState::kComplete
            && !submission::ResourceReleasable(back.Get()), "later healthy replay cannot prove the first Execute or release its BB");
  frame("first-signal-missing-keeps-b-original", &route, scene_b, 1, true, true, false, -1);

  Check(Hooks::Enable(session.handle, 0) == 0, "close the primary SDK owner before the independent preparation fault");
  {
    Session cold(&gpu);
    cold.context->ui_composition = false;
    cold.extent_left = cold.extent_top = 0;
    cold.extent_height = cold.extent_width = size;
    ComPtr<ID3D12Resource> cold_back;
    Check(SUCCEEDED(cold.proxy->GetBuffer(cold.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&cold_back)))
              && cold_back.Get() != back.Get(), "preparation fault uses a different actual DXGI output from the old color fault");
    funnel_accounting_tests::Restore readback_restore;
    readback_restore.Value(&nr_edit_reuse_xefg_cases::consume_sdk_present);
    nr_edit_reuse_xefg_cases::consume_sdk_present = [&](IDXGISwapChain* output) {
      Check(output == cold.proxy.Get(), "the original SDK consumer reads the preparation-fault output");
      consumed = read(cold_back.Get());
    };
    upload(cold_back.Get(), scene_b, present_state);
    submit();
    const auto creates = next_handle + 1;
    const auto evaluates = model_calls;
    const auto deliveries = xefg::SubmittedFrames();
    fail_first_tracker_signal([&] { sdk_present(&cold, 1, true); });
    const auto creation = pending_model(&cold, creates, evaluates, deliveries);
    const auto& [model, recording, fence, value] = creation;
    Check(value != 0 && fence->GetCompletedValue() >= value && fence->GetCompletedValue() != UINT64_MAX
              && consumed == expected_pixels(scene_b, false), "owner creation Fence completes but unproven NR leaves original RGBA");
    std::vector<submission::RecordingBlocker> blockers;
    submission::RecordedWorkState(&blockers);
    const auto missing = std::ranges::find_if(blockers, [&](const auto& blocker) {
      return blocker.list == recording->list && blocker.generation == recording->generation;
    });
    Check(missing != blockers.end() && missing->owner == submission::RecordingOwner::kXeFg
              && missing->first_submission.state == submission::SubmissionProofState::kSignalFailed,
          "the first Signal fault belongs to the production Feature creation and its specific generation");
    const auto epoch = cold.context->epoch;
    const auto app_frame = cold.context->application_frame;
    sdk_present(&cold, 1, false);
    Check(pending_model(&cold, creates, evaluates, deliveries) == creation && cold.context->epoch == epoch
              && cold.context->application_frame == app_frame + 1 && consumed == expected_pixels(scene_b, false),
          "the next real SDK application Present preserves the same unproven model and never claims its version");
    std::cout << "XEFG_HANDOFF_CREATION_SIGNAL_LOST " << nlohmann::json{{"creates", 1}, {"claims", 0},
        {"list", recording->list}, {"generation", recording->generation}, {"owner_fence", value},
        {"owner_completed", fence->GetCompletedValue()}, {"first_signal", "failed"}}.dump() << std::endl;
    Check(Hooks::Enable(cold.handle, 0) == 0, "close the preparation-fault SDK owner");
  }
  release_fixture();
  Check(accepted, "same-backbuffer Present handoff must deliver the first legal version exactly once");
  std::cout << "PASS xefg-ordinary-present-receipt evidence=WARP+MOCK same_version=1 next_version=1"
               " sdk_failure=1 late_end=1 distinct_output=1 hudless_conflict=1 no_sdk_finish=1" << std::endl;
  std::cout << "PASS xefg-present-handoff evidence=WARP+MOCK passes=1 same_backbuffer=1 full_rgba=1" << std::endl;
}
}  // namespace xefg_present_handoff_cases
