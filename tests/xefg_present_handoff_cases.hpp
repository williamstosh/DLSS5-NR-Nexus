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
  namespace ui = addon::ui;
  // This fixture registers real D3D12 swapchains directly, without ReShade's
  // callback that otherwise records D3D12 for the production funnel.
  funnel_accounting_tests::Restore api_restore;
  api_restore.Atomic(&native::d3d12_swapchain_seen);
  native::d3d12_swapchain_seen = true;
  const auto output_of = [](const Session& target) {
    const std::shared_lock lock(adapter::state.mutex);
    return target.context->output_identity;
  };
  const auto card_inputs = [](const rate::OutputIdentity& output) {
    Check(static_cast<bool>(output), "snapshot uses a registered output identity");
#if defined(NR_NEXUS_TEST_LEGACY_XEFG_STATUS)
    return native::CollectCardInputs(addon::ComputeNrVerdict(native::CollectFunnelInputs()), native::SteadyNowNs());
#else
    return native::CollectCardInputs(output);
#endif
  };
  const auto check_status = [&](const char* phase, const rate::OutputIdentity& output,
                                ui::CardTone tone, ui::CardId id, const char* reason, uint64_t injected) {
    const auto in = card_inputs(output);
    const auto card = ui::ComputeStatusCard(in);
    const auto verdict = addon::ComputeNrVerdict(native::CollectFunnelInputs(xefg::ReadStatus(output)));
    const std::string_view verdict_reason = (in.verdict.reason == nullptr ? "" : in.verdict.reason);
    const bool matching = card.id == id && card.tone == tone && verdict.injected == injected
        && verdict_reason == reason
        && (tone == ui::CardTone::kGood ? in.fg_submitted_frames.value_or(0) > 0
                                         : !in.fg_submitted_frames.has_value())
        && (id != ui::CardId::kFgPreparing
            || (in.verdict.IsFgPreparing() && !in.fg_continuing_submission));
    std::cout << "XEFG_HANDOFF_PRESENT_STATUS " << nlohmann::json{
        {"phase", phase}, {"generation", output.generation}, {"injected", verdict.injected},
        {"reason", verdict_reason}, {"card", ui::CardSlug(card.id)}, {"tone", ui::CardToneName(card.tone)},
        {"continuing", in.fg_continuing_submission}, {"passed", matching}}.dump() << std::endl;
    Check(matching, phase);
  };
  const auto check_gray = [&](const char* phase, const Session& target) {
    check_status(phase, output_of(target), ui::CardTone::kIdle, ui::CardId::kFgPreparing, "fg_preparing", 0);
  };
  const auto check_green = [&](const char* phase, const Session& target) {
    check_status(phase, output_of(target), ui::CardTone::kGood, ui::CardId::kActive, "", 1);
  };
  const auto check_unproven = [&](const char* phase) {
    check_status(phase, output_of(session), ui::CardTone::kIdle, ui::CardId::kStarting, "", 1);
  };
  const auto check_warn = [&](const char* phase, const char* reason) {
    check_status(phase, output_of(session), ui::CardTone::kWarn, ui::CardId::kNotRunning, reason, 0);
  };
  std::optional<xefg::StatusSnapshot> first_unserved_status;
  const auto calls_before_first = model_calls;
  const auto submissions_before_first = xefg::SubmittedFrames();
  const auto frames_before_first = session.context->application_frame;
  Check(Hooks::Enable(session.handle, 0) == 0 && Hooks::Enable(session.handle, 1) == 0
            && session.context->enabled && rate::MatchPresentOutput(reinterpret_cast<uintptr_t>(&route),
                reinterpret_cast<uintptr_t>(back.Get())) == output_of(session),
        "actual SDK enable is tied to the current Present output");
  check_gray("first-enable-before-real-present", session);
  {
    funnel_accounting_tests::Restore test_present_restore;
    test_present_restore.Value(&nr_edit_reuse_xefg_cases::consume_sdk_present);
    nr_edit_reuse_xefg_cases::consume_sdk_present = {};
    Check(adapter::ProxyHooks<0>::Present(session.proxy.Get(), 0, DXGI_PRESENT_TEST) == S_OK,
          "SDK test flag is forwarded without classifying an application frame");
  }
  check_gray("test-present-does-not-classify-first-frame", session);
  Check(model_calls == calls_before_first && xefg::SubmittedFrames() == submissions_before_first
            && session.context->application_frame == frames_before_first,
        "no real Present, model evaluation, or NR submission occurred before the first color");
  warm("initial-sdk", &route, true);
  check_green("first-delivered-color-is-green", session);
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
      if (!sdk_success) {
        check_warn("first-sdk-present-failure-is-yellow", "fg_handoff_failed");
      }
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
      if (!sdk_success) {
        check_unproven("successful-sdk-return-without-matched-delivery-stays-gray");
      }
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
  {
    using Claim = xefg::OrdinaryPresentClaim;
    const auto consume_original = nr_edit_reuse_xefg_cases::consume_sdk_present;
    funnel_accounting_tests::Restore off_restore;
    off_restore.Value(&nr_edit_reuse_xefg_cases::consume_sdk_present);
    off_restore.Value(&adapter::proxies[0].real[1]);
    off_restore.Value(&session.context->bridge_proxy_ready);
    // Session wires its real composition swapchain directly in place of the
    // SDK's GetSwapchain export. Complete that mock boundary for Off claims.
    session.context->bridge_proxy_ready = true;
    adapter::proxies[0].real[1] = reinterpret_cast<void*>(+[](IDXGISwapChain1* swapchain, UINT interval, UINT flags,
        const DXGI_PRESENT_PARAMETERS*) -> HRESULT {
      return nr_edit_reuse_xefg_cases::ConsumeSdkPresent(swapchain, interval, flags);
    });
    const auto enter_pre_effects = [&] {
      const auto attempt = call_ordinary(&route);
      native::OnFinishPresent(&route.queue, &route);
      return attempt;
    };
    const auto off_present = [&](const DXGI_PRESENT_PARAMETERS* parameters) {
      Check((parameters != nullptr ? adapter::ProxyHooks<0>::Present1(session.proxy.Get(), 0, 0, parameters)
                                  : adapter::ProxyHooks<0>::Present(session.proxy.Get(), 0, 0)) == S_OK,
            "the real outer detour preserves its Off SDK return");
    };

    produce(back.Get(), scene_a, -1, false);
    const auto old_epoch = session.context->epoch;
    const auto before_late_disable = model_calls;
    const auto denied_attempt = call_ordinary(&route);
    const auto& denied = native::ordinary_present_attempts.at(&route);
    Check(denied.event_access.epoch == old_epoch && !denied.event_access.allowed && !denied.receipt.executed,
          "a one-Pass model cannot take the mature two-Pass pre-FX route while XeFG owns this version");
    upload(back.Get(), menu, present_state);
    submit();
    Check(Hooks::Enable(session.handle, 0) == 0 && session.context->epoch != old_epoch,
          "the original SDK accepted Disable after ordinary pre-FX and composition");
    nr_edit_reuse_xefg_cases::consume_sdk_present = [&](IDXGISwapChain* swapchain) {
      const auto* frame = adapter::ordinary_off_present;
      Check(frame != nullptr && frame->ordinary_off.access.call != 0, "the first Off call has an actual SDK turn");
      Check(frame->ordinary_claim == Claim::NO_MATCH && !frame->ordinary_receipt.executed,
            "the first Off correlates the previous On-epoch denial without manufacturing a color receipt");
      consume_original(swapchain);
    };
    off_present(nullptr);
    Check(model_calls == before_late_disable && consumed == expected_pixels(menu, false),
          "late Disable preserves the composed first read");
    native::EndOrdinaryPresentAttempt(denied_attempt);

    produce(back.Get(), scene_a, -1, false);
    // Simulate the already composed case with a second real GPU color write.
    // The outer detour never obtains a pre-FX callback for this version.
    upload(back.Get(), menu, present_state);
    submit();
    const auto before_first_off = model_calls;
    const auto before_off_write = session.context->last_handoff_write.call;
    nr_edit_reuse_xefg_cases::consume_sdk_present = [&](IDXGISwapChain* swapchain) { consume_original(swapchain); };
    off_present(nullptr);
    Check(model_calls == before_first_off && consumed == expected_pixels(menu, false)
              && session.context->last_handoff_write.call == before_off_write,
          "no pre-FX callback leaves the original SDK's first read unchanged");

    produce(back.Get(), scene_a, -1, false);
    const auto prior_attempt = call_ordinary(&route);
    const auto prior_calls = model_calls;
    const auto ordinary_after_prior = ordinary_submissions(&route);
    off_present(nullptr);
    const auto& prior_receipt = session.context->last_handoff_write;
    Check(model_calls == prior_calls && ordinary_submissions(&route) == ordinary_after_prior
              && consumed == expected_pixels(scene_a, true) && !find_attempt(&route)
              && prior_receipt.ordinary_claim == Claim::COVERED && prior_receipt.ordinary_receipt.attempt == prior_attempt
              && prior_receipt.ordinary_receipt.call == 0 && prior_receipt.call != 0 && !session.context->bridge_present_fg_on,
          "Off accepts an earlier complete ordinary write once without restoring FG ownership");

    produce(back.Get(), scene_b, -1, false);
    RECT whole{0, 0, size, size};
    const DXGI_PRESENT_PARAMETERS full{.DirtyRectsCount = 1, .pDirtyRects = &whole};
    const auto before_nested = model_calls;
    const auto before_nested_writes = ordinary_submissions(&route);
    nr_edit_reuse_xefg_cases::consume_sdk_present = [&](IDXGISwapChain* swapchain) {
      enter_pre_effects();
      consume_original(swapchain);
    };
    off_present(&full);
    const auto& nested_receipt = session.context->last_handoff_write;
    Check(model_calls == before_nested + 1 && ordinary_submissions(&route) == before_nested_writes + 1
              && consumed == expected_pixels(scene_b, true) && !find_attempt(&route)
              && nested_receipt.ordinary_claim == Claim::COVERED && nested_receipt.ordinary_receipt.call == nested_receipt.call
              && nested_receipt.ordinary_receipt.first_execute != 0 && nested_receipt.ordinary_receipt.proofs_healthy,
          "a nested pre-FX callback handles the exact full Present1 with one actual Execute and first Signal");

    produce(back.Get(), menu, -1, false);
    RECT partial{0, 0, size / 2, size};
    const DXGI_PRESENT_PARAMETERS dirty{.DirtyRectsCount = 1, .pDirtyRects = &partial};
    nr_edit_reuse_xefg_cases::consume_sdk_present = [&](IDXGISwapChain* swapchain) {
      const auto attempt = native::BeginOrdinaryPresentAttempt(&route.queue, &route);
      Check(!attempt, "the outer dirty region refuses a full write even when the nested callback claims full color");
      native::RunPresentPath(&route.queue, &route);
      consume_original(swapchain);
    };
    off_present(&dirty);
    Check(model_calls == before_nested + 1 && consumed == expected_pixels(menu, false),
          "partial Present1 preserves the first consumer's original RGBA");
    POINT offset{1, 0};
    const DXGI_PRESENT_PARAMETERS scroll{.pScrollRect = &whole, .pScrollOffset = &offset};
    off_present(&scroll);
    Check(model_calls == before_nested + 1 && consumed == expected_pixels(menu, false),
          "a scroll Present1 does not acquire full BackBuffer write permission");

    Check(Hooks::Enable(session.handle, 1) == 0 && session.context->enabled, "return the fixture to XeFG ownership");
    std::cout << "PASS xefg-off-present-turn evidence=WARP+MOCK late_disable=1 missing_pre_fx=1 prior_write=1 nested_write=1"
                 " full_present1=1 partial_present1_rejected=1 scroll_rejected=1" << std::endl;

    native::NgxLoaderSymbols signed_mock{.nr_ready = true, .nr_module = native::direct_api.module,
        .nr_image_generation = 1, .original_nr_exports = true, .init = native::direct_api.init,
        .allocate = native::direct_api.allocate, .destroy = native::direct_api.destroy, .create = native::direct_api.create,
        .evaluate = native::direct_api.evaluate, .release = native::direct_api.release, .shutdown = native::direct_api.shutdown};
    funnel_accounting_tests::Restore warm_restore;
    warm_restore.Atomic(&native::ngx_loader_published);
    warm_restore.Atomic(&native::stack_passes);
    warm_restore.Atomic(&native::pass_stage[0]);
    warm_restore.Value(&native::direct_api.original_image_generation);
    warm_restore.Value(&native::original_nr_init_generation);
    warm_restore.Value(&native::original_nr_face_verified);
    warm_restore.Value(&session.context->init.flags);
    warm_restore.Value(&session.context->omit_optional_hudless);
    native::ngx_loader_published = &signed_mock;
    native::direct_api.original_image_generation = 1;
    native::original_nr_init_generation = 1;
    native::original_nr_face_verified = true;
    native::stack_passes = 2;
    native::pass_stage[0] = 0;
    session.context->init.flags = 1;  // Inverted depth, motion has no jitter.
    session.context->omit_optional_hudless = true;
    nr_edit_reuse_xefg_cases::consume_sdk_present = consume_original;
    bool mature = false;
    for (uint32_t i = 0; i < 12 && !mature; ++i) {
      produce(back.Get(), scene_a, -1, false);
      sdk_present(&session, 1, false);
      const auto& warm = native::present_streams.at(&route).warm_color;
      mature = warm.complete && warm.signed_group && warm.has_real_layout;
    }
    Check(mature, "the real guide SDK route warms the two model slots of the registered physical owner");
    produce(back.Get(), scene_b, -1, false);
    sdk_present(&session, -1, false);
    Check(session.context->last_handoff_write.service_result == fg::Result::SUBMITTED
              && !session.context->last_handoff_write.has_guides,
          "the actual earlier SDK consumer used the warm missing-guide contract");

    produce(back.Get(), scene_a, -1, false);
    const auto before_warm_off = model_calls;
    const auto warm_creates = next_handle;
    const auto warm_epoch = session.context->epoch;
    const auto warm_attempt = native::BeginOrdinaryPresentAttempt(&route.queue, &route);
    Check(static_cast<bool>(warm_attempt), "ordinary Present begins the warm pre-FX attempt");
    const auto& warm_event = native::ordinary_present_attempts.at(&route);
    const D3D12_COMMAND_QUEUE_DESC alternate_desc{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> alternate_queue;
    Check(SUCCEEDED(gpu.device->CreateCommandQueue(&alternate_desc, IID_PPV_ARGS(&alternate_queue))),
          "create a second real submission queue on the same WARP device");
    const auto alternate_identity = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(alternate_queue.Get()));
    Check(alternate_identity != 0 && alternate_identity != warm_event.queue,
          "the alternate queue has a distinct native identity");
    Check(xefg::QueryOrdinaryPreEffects(warm_event.event_access, warm_attempt, warm_event.queue, size, size).attempt == warm_attempt
              && !xefg::QueryOrdinaryPreEffects(warm_event.event_access, warm_attempt, alternate_identity, size, size).attempt,
          "only the same physical queue can take this otherwise eligible pre-FX write");
    native::present_sdr_white_polled_ns = native::SteadyNowNs();
    native::RunPresentPath(&route.queue, &route);
    const auto warm_receipt = native::ordinary_present_attempts.at(&route).receipt;
    Check(warm_receipt.executed && warm_receipt.pre_effects_xefg && warm_receipt.proofs_healthy
              && warm_receipt.epoch == warm_epoch && warm_receipt.attempt == warm_attempt
              && model_calls == before_warm_off + 2 && next_handle == warm_creates,
          "pre-FX executes the complete mature two-Pass neutral model once under live On ownership");
    auto pre_fx_pixels = Capture(gpu, back.Get(), present_state);
    // Copy an independent UI pixel after the pre-FX read. A late model would
    // erase that marker as well as increment the existing two-Pass write.
    const Pixel ui_pixel{91, 120, 151, 201};
    const auto ui = gpu.Texture();
    upload(ui.Get(), ui_pixel, present_state);
    gpu.Transition(ui.Get(), present_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.Transition(back.Get(), present_state, D3D12_RESOURCE_STATE_COPY_DEST);
    const D3D12_TEXTURE_COPY_LOCATION to{.pResource = back.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    const D3D12_TEXTURE_COPY_LOCATION from{.pResource = ui.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    constexpr D3D12_BOX top_left{0, 0, 0, 1, 1, 1};
    gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, &top_left);
    gpu.Transition(back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, present_state);
    submit();
    Check(pre_fx_pixels.Pixels() == expected_pixels(scene_a, true), "the pre-FX consumer reads current edited color");
    Check(Hooks::Enable(session.handle, 0) == 0, "the real Disable follows the pre-FX write and UI composition");
    Check(call_ordinary(&route) == warm_attempt && model_calls == before_warm_off + 2,
          "a second event before this SDK call cannot replace or replay the already-executed attempt after Disable");
    off_present(nullptr);
    auto expected_composited = expected_pixels(scene_a, true);
    std::ranges::copy(ui_pixel, expected_composited.begin());
    const auto& settled = session.context->last_handoff_write;
    Check(consumed == expected_composited && model_calls == before_warm_off + 2 && next_handle == warm_creates
              && settled.ordinary_claim == Claim::COVERED && settled.ordinary_receipt.attempt == warm_attempt
              && settled.ordinary_receipt.epoch == warm_epoch && settled.epoch == warm_epoch + 1
              && std::string_view(settled.reason) == "off_pre_fx_owner_receipt" && !find_attempt(&route),
          "Off consumes the composed version with one Execute, original UI and the exact adjacent Disable claim");

    Check(Hooks::Enable(session.handle, 1) == 0, "SDK enable restores its regular consumer");
    produce(back.Get(), scene_b, -1, false);
    sdk_present(&session, -1, false);
    Check(session.context->last_handoff_write.service_result == fg::Result::SUBMITTED
              && !session.context->last_handoff_write.has_guides,
          "a genuinely untagged startup delivery precedes the first real-guide case");
    produce(back.Get(), scene_a, -1, false);
    const auto before_first_guides = model_calls;
    const auto startup_attempt = call_ordinary(&route);
    Check(!native::ordinary_present_attempts.at(&route).receipt.executed && model_calls == before_first_guides,
          "the reopened declaration must deliver real guides before an untagged frame can authorize pre-FX");
    sdk_present(&session, 1, false);
    Check(consumed == expected_pixels(scene_a, true) && model_calls == before_first_guides + 2
              && model_motion == motions[1].Get() && model_depth == depths[1].Get(),
          "the reopened declaration's first real guides are delivered on the SDK path");
    native::EndOrdinaryPresentAttempt(startup_attempt);
    produce(back.Get(), scene_b, -1, false);
    sdk_present(&session, -1, false);
    Check(session.context->last_handoff_write.service_result == fg::Result::SUBMITTED
              && !session.context->last_handoff_write.has_guides,
          "the warmed On declaration has now consumed the qualified falling edge");
    {
      const addon::RuntimeLock lock(native::runtime_mutex);
      const auto& game_sr = native::features.at(source);
      const auto physical = rate::FindPresentOwner(0, warm_receipt.backbuffer);
      const rate::SourceIdentity sr_source{.handle = reinterpret_cast<uintptr_t>(source), .generation = game_sr.source_generation,
          .device = reinterpret_cast<uintptr_t>(game_sr.source_device12_identity.Get()),
          .resource = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(source_output.Get())),
          .resource_generation = native::AdaptiveResourceGeneration(source_output.Get())};
      rate::BindSourceOutput(sr_source, physical);
      Check(rate::FindSourceOutput(sr_source) == session.context->output_identity,
            "fixture binds its GPU SR source to exactly the physical output it copies into");
    }
    produce(back.Get(), scene_b, 1, false);
    const auto late_calls = model_calls;
    const auto late_attempt = call_ordinary(&route);
    const auto& late_owner = native::ordinary_present_attempts.at(&route);
    Check(!late_owner.receipt.executed && model_calls == late_calls
              && std::string_view(late_owner.pre_effects_rejection) == "off_pre_fx_current_sr_guides"
              && !session.context->fresh_id,
          "current SR guide producer keeps late SDK tags eligible before any tag reaches the SDK");
    sdk_present(&session, 1, false);
    Check(consumed == expected_pixels(scene_b, true) && model_calls == late_calls + 2 && next_handle == warm_creates
              && model_motion == motions[1].Get() && model_depth == depths[1].Get()
              && session.context->last_handoff_write.service_result == fg::Result::SUBMITTED,
          "the first late-tagged frame uses both real guides, the same models, and one original SDK delivery");
    native::EndOrdinaryPresentAttempt(late_attempt);

    produce(back.Get(), scene_a, -1, false);
    sdk_present(&session, -1, false);
    produce(back.Get(), scene_b, -1, false);
    const auto unconsumed_attempt = call_ordinary(&route);
    Check(native::ordinary_present_attempts.at(&route).receipt.pre_effects_xefg,
          "the next missing-guide version can acquire its own pre-FX write");
    const auto after_unconsumed_write = model_calls;
    Check(Hooks::Enable(session.handle, 0) == 0 && Hooks::Enable(session.handle, 1) == 0,
          "an additional Enable breaks the adjacent Disable transfer");
    sdk_present(&session, -1, false);
    Check(session.context->last_handoff_write.ordinary_claim == Claim::CONFLICT
              && session.context->last_handoff_write.ordinary_receipt.attempt == unconsumed_attempt
              && model_calls == after_unconsumed_write && !find_attempt(&route),
          "the unconsumed write cannot be adopted or repeated across multiple epoch changes");
    produce(back.Get(), scene_a, -1, false);
    sdk_present(&session, 1, false);
    Check(consumed == expected_pixels(scene_a, true) && model_calls == after_unconsumed_write + 2
              && session.context->last_handoff_write.service_result == fg::Result::SUBMITTED,
          "the same backbuffer can deliver a newly produced version through the regular SDK owner");
    std::cout << "PASS xefg-off-pre-fx-handoff evidence=WARP+MOCK two_pass=1 exact_disable=1 ui_retained=1"
                 " foreign_queue_rejected=1 late_real_guides=1 stale_epoch_rejected=1 next_version=1\n";
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
    check_warn("first-terminal-refusal-visible-inside-sdk", "fg_unserved");
    first_unserved_status = xefg::ReadStatus(output_of(session));
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
    check_warn("first-unserved-hudless-conflict-is-yellow", "fg_unserved");
    Check(find_attempt(&route) == next_attempt && xefg::SubmittedFrames() == deliveries && model_calls == calls,
          "the delayed SDK End settles only its old receipt and leaves the newer attempt active");
    sdk_present(&session, -1, false);
    Check(!find_attempt(&route) && xefg::CurrentResult() == fg::Result::SUBMITTED
              && xefg::SubmittedFrames() == deliveries + 1 && model_calls == calls + 1
              && consumed == expected_pixels(scene_a, true),
          "the next actual BB version delivers after the late SDK End");
    check_unproven("untagged-success-does-not-borrow-a-delivery-proof");
    frame("status-recovery-with-current-guides", &route, scene_a, 1, false, true, true, -1);
    check_green("success-with-selected-delivery-after-first-unserved-is-green", session);
  }

  {
    funnel_accounting_tests::Restore enable_restore;
    enable_restore.Value(&adapter::modules[0].real[6]);
    enable_restore.Value(&receipt_sdk_status);
    adapter::modules[0].real[6] = reinterpret_cast<void*>(&FakeReceiptEnable);
    receipt_sdk_status = -19;
    const auto failed_disable_output = output_of(session);
    const auto failed_disable_epoch = session.context->epoch;
    Check(Hooks::Enable(session.handle, 0) == -19 && session.context->enabled
              && output_of(session) == failed_disable_output && session.context->epoch == failed_disable_epoch,
          "a failed SDK Disable preserves the actual selected owner and epoch");
    check_green("failed-disable-keeps-prior-success", session);
  }

  {
    Session other_sdk(&gpu);
    other_sdk.context->ui_composition = false;
    Check(Hooks::Enable(other_sdk.handle, 0) == 0 && Hooks::Enable(other_sdk.handle, 1) == 0
              && output_of(other_sdk) != output_of(session),
          "the second SDK owner has a separately registered output");
    check_green("first-output-retains-its-own-green", session);
    check_gray("second-output-cannot-borrow-first-output-green", other_sdk);
    {
      // Keep the actual published ordinary owner on A while B performs a
      // different, genuine SDK attempt and records its own refusal.
      funnel_accounting_tests::Restore selected_restore;
      selected_restore.Value(&native::monitor_status_output);
      selected_restore.Value(&nr_edit_reuse_xefg_cases::consume_sdk_present);
      native::monitor_status_output = {.owner = rate::FindPresentOwner(0, reinterpret_cast<uintptr_t>(back.Get()))};
      nr_edit_reuse_xefg_cases::consume_sdk_present = {};
      other_sdk.context->ui_composition = true;
      sdk_present(&other_sdk, -1, false);
      const auto second = xefg::ReadStatus(output_of(other_sdk));
      Check(second && second->DisplayOutcome() == fg::Result::NOT_SERVED,
            "a real second-output refusal is independently visible");
      const auto observation = native::ReadSelectedXefgStatus();
      const auto selected = addon::ComputeNrVerdict(native::CollectFunnelInputs());
      const auto selected_card = ui::ComputeStatusCard(native::CollectCardInputs());
      Check(observation && observation->output == output_of(session)
                && selected.state == addon::NrState::kEngaged && selected_card.tone == ui::CardTone::kGood,
            "a second output cannot replace the selected verdict or card");
      other_sdk.context->ui_composition = false;
      Check(Hooks::Enable(other_sdk.handle, 0) == 0 && Hooks::Enable(other_sdk.handle, 1) == 0,
            "successful restart returns the second output to fresh status");
      std::cout << "PASS xefg-selected-status other_outcome_not_selected=1 selected_verdict=1 selected_card=1" << std::endl;
    }
    {
      const auto recovering = xefg::ReadStatus(output_of(session));
      const auto other = xefg::ReadStatus(output_of(other_sdk));
      Check(first_unserved_status && recovering && other && first_unserved_status->output == recovering->output,
            "HUD test follows an actual yellow and recovery on the same output");
      funnel_accounting_tests::Restore hud_restore;
      hud_restore.Atomic(&native::hud_enabled);
      hud_restore.Atomic(&native::hud_pill);
      hud_restore.Atomic(&native::hud_toast_until_ns);
      hud_restore.Value(&native::hud_recovery_pending);
      hud_restore.Value(&native::hud_xefg_scope);
      native::hud_enabled = true;
      native::hud_toast_until_ns = 0;
      const auto now = native::SteadyNowNs();
      native::MaybeEmitStatusCard(addon::ComputeNrVerdict(native::CollectFunnelInputs(first_unserved_status)),
          now, first_unserved_status);
      Check(native::hud_pill && native::hud_recovery_pending && native::hud_toast_until_ns == 0,
            "selected first real rejection claims its own HUD warning");
      native::MaybeEmitStatusCard(addon::ComputeNrVerdict(native::CollectFunnelInputs(xefg::StatusSnapshot{})),
          now + 1, xefg::StatusSnapshot{});
      Check(native::hud_pill && native::hud_recovery_pending && native::hud_toast_until_ns == 0,
            "an unresolved selection cannot clear the selected warning or consume its recovery");
      native::MaybeEmitStatusCard(addon::ComputeNrVerdict(native::CollectFunnelInputs(recovering)),
          now + 2, recovering);
      Check(!native::hud_pill && !native::hud_recovery_pending && native::hud_toast_until_ns == now + 2 + native::kHudToastNs,
            "only delivery on the selected generation consumes its warning and toasts");
      std::cout << "PASS xefg-selected-hud first_refusal=1 unrelated_present=1 same_generation_recovery=1" << std::endl;
    }

    produce(back.Get(), scene_a, -1, false);
    const auto attempt = call_ordinary(&route);
    const auto calls = model_calls;
    const auto deliveries = xefg::SubmittedFrames();
    auto late = adapter::BeginPresent(session.proxy.Get(), 0);
    Check(late.input.output == output_of(session) && late.color_executed
              && late.service_result == fg::Result::SUBMITTED && model_calls == calls + 1,
          "the old owner records real color before its successful SDK return is delayed");
    {
      const auto recording = xefg::ReadStatus(late.input.output);
      Check(recording && recording->recording && recording->continuing_submission,
            "next real recording has continuity from same-generation delivered color");
      funnel_accounting_tests::Restore monitor_restore;
      monitor_restore.Value(&native::monitor_snapshot);
      native::InvalidateMonitorDisplay(recording);
      Check(native::monitor_snapshot.verdict.IsFgPreparing()
                && native::monitor_snapshot.card.id == ui::CardId::kFgPreparing
                && native::monitor_snapshot.card.tone == ui::CardTone::kIdle,
            "a busy monitor must not manufacture selected delivery from an SDK status");
      const auto fresh = xefg::ReadStatus(output_of(other_sdk));
      native::InvalidateMonitorDisplay(fresh);
      Check(native::monitor_snapshot.card.id == ui::CardId::kFgPreparing
                && native::monitor_snapshot.card.tone == ui::CardTone::kIdle,
            "a new output with no result cannot use other generation's green");
      std::cout << "PASS xefg-monitor-busy no_unproven_green=1 new_scope_unknown=1" << std::endl;
    }
    const auto sdk_result = nr_edit_reuse_xefg_cases::ConsumeSdkPresent(session.proxy.Get(), 0, 0);
    Check(sdk_result == S_OK && consumed == expected_pixels(scene_a, true),
          "the delayed old SDK call genuinely consumed the enhanced pixels");

    const auto ordinary_key = rate::FindPresentOwner(0, reinterpret_cast<uintptr_t>(back.Get()));
    Check(ordinary_key && rate::ResolveRegisteredPresentOutput(ordinary_key) == late.input.output,
          "published ordinary identity initially selects the original XeFG generation");
    Check(Hooks::Enable(session.handle, 0) == 0 && !session.context->enabled,
          "successful Disable relinquishes the old selected owner");
    const auto off = card_inputs(output_of(session));
    Check(!off.fg_submitted_frames.has_value() && !off.fg_continuing_submission,
          "successful Disable exits the XeFG presentation status");
    Check(Hooks::Enable(session.handle, 1) == 0 && session.context->enabled
              && session.context->epoch != late.input.epoch && output_of(session) != late.input.output,
          "real re-enable creates a new selected generation on the primary output");
    check_gray("primary-new-epoch-is-unjudged", session);
    Check(ordinary_key && rate::ResolveRegisteredPresentOutput(ordinary_key) == output_of(session),
          "published ordinary identity resolves the newly accepted XeFG generation");
    adapter::EndPresent(&late, sdk_result);
    native::EndOrdinaryPresentAttempt(attempt, S_OK);
    Check(xefg::SubmittedFrames() == deliveries + 1,
          "reporting isolation does not change accounting for the genuinely submitted old color");
    check_gray("late-old-epoch-success-cannot-color-primary", session);
    check_gray("late-old-epoch-success-cannot-color-second-output", other_sdk);
    other_sdk.CloseSdk();
  }

  {
    Check(Hooks::Enable(session.handle, 0) == 0, "release the earlier SDK fixture before the load contract");
    funnel_accounting_tests::Restore load_restore;
    load_restore.Atomic(&native::stack_passes);
    load_restore.Atomic(&native::pass_stage[0]);
    load_restore.Atomic(&native::depth_mode);
    load_restore.Value(&model_parameter_observer);
    load_restore.Value(&nr_edit_reuse_xefg_cases::consume_sdk_present);
    native::stack_passes = 2;
    native::pass_stage[0] = 0;

    constexpr uint32_t inverted_flag = NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    constexpr uint32_t low_motion_flag = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    constexpr uint32_t guide_flags = inverted_flag | low_motion_flag | NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    const auto low_depth = gpu.Texture(size / 2, size / 2, DXGI_FORMAT_R32_FLOAT);
    const auto low_motion = gpu.Texture(size / 2, size / 2, DXGI_FORMAT_R16G16_FLOAT);
    struct GuideReceipt {
      ComPtr<ID3D12Resource> depth;
      ComPtr<ID3D12Resource> motion;
      uint32_t create_flags = 0;
      uint32_t model_creation = 0;
      int depth_inverted = -1;
      int reset = -1;
      std::vector<uint8_t> depth_bytes;
      std::vector<uint8_t> motion_bytes;
    };
    struct LoadReceipt {
      fg::Result result = fg::Result::NOT_SERVED;
      uint64_t deliveries = 0;
      uint32_t creates = 0;
      uint32_t evaluates = 0;
      uint64_t game_resets = 0;
      std::vector<uint8_t> pixels;
      std::vector<GuideReceipt> guides;
    };
    const auto uniform = [](const std::vector<uint8_t>& bytes, const auto& expected, size_t samples) {
      const auto* value = reinterpret_cast<const uint8_t*>(&expected);
      if (bytes.size() != sizeof(expected) * samples) return false;
      for (size_t offset = 0; offset < bytes.size(); offset += sizeof(expected)) {
        if (!std::equal(value, value + sizeof(expected), bytes.begin() + offset)) return false;
      }
      return true;
    };
    auto& game_resets = native::history_reset_counts[static_cast<size_t>(native::HistoryResetSource::kGameReset)];
    for (const bool sdk_inverted : {false, true}) {
      native::depth_mode = 0;
      Session load(&gpu, 0, sdk_inverted ? 1u : 0u);
      load.context->ui_composition = false;
      load.extent_top = load.extent_left = 0;
      load.extent_height = load.extent_width = size;
      ComPtr<ID3D12Resource> load_back;
      Check(SUCCEEDED(load.proxy->GetBuffer(load.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&load_back)))
                && load_back.Get() != back.Get(), "load contract owns a real, independent DXGI backbuffer");
      Check(Hooks::Enable(load.handle, 0) == 0 && Hooks::Enable(load.handle, 1) == 0,
            "new load contract is enabled through the actual SDK hook");
      const auto epoch = load.context->epoch;
      const auto output = load.context->output_identity;
      Check(static_cast<bool>(output), "load fixture has a registered XeFG output");
      const auto native_back = renodx::utils::directx::NativeIdentity(load_back.Get());
      Check(!addon::output_framerate::MatchPresentColorOwner(output, reinterpret_cast<uintptr_t>(native_back)),
            "load contract exercises the private XeFG guide path without an ordinary warm group");
      const auto sdk_flags = (sdk_inverted ? inverted_flag : 0u);
      std::vector<GuideReceipt> evaluated;
      std::vector<uint8_t> sdk_pixels;
      bool read_guides = false;
      model_parameter_observer = [&](const NVSDK_NGX_Handle* model, const NVSDK_NGX_Parameter* block) {
        GuideReceipt binding;
        binding.depth = native::GetD3D12Resource(block, "DLSSNR.Depth");
        binding.motion = native::GetD3D12Resource(block, "DLSSNR.MVec");
        binding.create_flags = model_create_flags.at(model);
        binding.model_creation = *reinterpret_cast<const uint32_t*>(model);
        binding.depth_inverted = native::GetInt(block, "DLSSNR.DepthInverted", -1);
        binding.reset = native::GetInt(block, "DLSSNR.Reset", -1);
        evaluated.push_back(std::move(binding));
      };
      nr_edit_reuse_xefg_cases::consume_sdk_present = [&](IDXGISwapChain* sdk_output) {
        ComPtr<ID3D12Resource> current;
        Check(sdk_output == load.proxy.Get()
                  && SUCCEEDED(load.proxy->GetBuffer(load.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&current)))
                  && current.Get() == load_back.Get(), "SDK consumes the same freshly produced load color");
        const auto color_snapshot = Capture(gpu, current.Get(), present_state);
        std::vector<std::pair<d3d12_test::TextureTransfer, d3d12_test::TextureTransfer>> guide_copies;
        if (read_guides) {
          for (const auto& binding : evaluated) {
            Check(binding.depth != nullptr && binding.motion != nullptr
                      && binding.depth->GetDesc().Format == DXGI_FORMAT_R32_FLOAT
                      && binding.motion->GetDesc().Format == DXGI_FORMAT_R16G16_FLOAT,
                  "the actual model bindings carry the expected scalar depth and two-lane motion formats");
            auto& [depth_copy, motion_copy] = guide_copies.emplace_back(
                gpu.CreateTextureTransfer(binding.depth.Get()), gpu.CreateTextureTransfer(binding.motion.Get()));
            const bool real = binding.depth.Get() == depths[1].Get();
            const auto resting = (real ? present_state : NATIVE_INPUT_STATE);
            for (const auto& [texture, copy] : {std::pair{binding.depth.Get(), &depth_copy},
                                               std::pair{binding.motion.Get(), &motion_copy}}) {
              gpu.Transition(texture, resting, D3D12_RESOURCE_STATE_COPY_SOURCE);
              gpu.RecordTextureReadback(texture, *copy);
              gpu.Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, resting);
            }
          }
        }
        submit();
        sdk_pixels = color_snapshot.Pixels();
        for (size_t pass = 0; pass < guide_copies.size(); ++pass) {
          evaluated[pass].depth_bytes = gpu.ReadTexture(guide_copies[pass].first);
          evaluated[pass].motion_bytes = gpu.ReadTexture(guide_copies[pass].second);
        }
      };
      const auto present_load = [&](const char* phase, Pixel pixel, int guides, bool game_reset, bool inspect) {
        produce(load_back.Get(), pixel, -1, false);
        evaluated.clear();
        sdk_pixels.clear();
        read_guides = inspect;
        const auto creates = next_handle;
        const auto calls = model_calls;
        const auto deliveries = xefg::SubmittedFrames();
        const auto resets = game_resets.load();
        const auto frame_before = load.context->application_frame;
        if (guides == 2) {
          ++present_id;
          load.Tag(present_id, DEPTH, low_depth.Get());
          load.Tag(present_id, MOTION, low_motion.Get());
          const xefg::abi::Constants constants{.motion_scale_x = 1.f, .motion_scale_y = 1.f, .frame_time_ms = 16.5f};
          Check(Hooks::Constants(load.handle, present_id, &constants) == 0, "fresh smaller-guide constants");
          load.Present(present_id);
        } else {
          sdk_present(&load, guides, game_reset);
        }
        LoadReceipt receipt{.result = xefg::CurrentResult(), .deliveries = xefg::SubmittedFrames() - deliveries,
            .creates = next_handle - creates, .evaluates = model_calls - calls, .game_resets = game_resets.load() - resets,
            .pixels = std::move(sdk_pixels), .guides = std::move(evaluated)};
        Check(load.context->enabled && load.context->epoch == epoch && load.context->output_identity == output
                  && load.context->application_frame == frame_before + 1 && !receipt.pixels.empty(),
              "one real SDK input and consumption on the same live output/context/epoch");
        std::cout << "XEFG_LOAD_NEUTRAL_GUIDES " << nlohmann::json{
            {"phase", phase}, {"sdk_inverted", sdk_inverted}, {"output_generation", output.generation},
            {"epoch", epoch}, {"application_frame", load.context->application_frame}, {"guides", guides},
            {"creates", receipt.creates}, {"evaluates", receipt.evaluates}, {"sdk_deliveries", receipt.deliveries},
            {"result", static_cast<unsigned>(receipt.result)}, {"game_resets", receipt.game_resets},
            {"first_consumed_rgba", std::array{receipt.pixels[0], receipt.pixels[1], receipt.pixels[2], receipt.pixels[3]}},
            {"full_rgba_nr", receipt.pixels == expected_pixels(pixel, true)}}.dump() << std::endl;
        return receipt;
      };
      const auto delivered = [&](const LoadReceipt& current, Pixel pixel) {
        return current.result == fg::Result::SUBMITTED && current.deliveries == 1 && current.evaluates == 2
            && current.guides.size() == 2 && current.pixels == expected_pixels(pixel, true);
      };
      const auto stable = [&](const LoadReceipt& current, Pixel pixel, int reset, uint64_t game_reset) {
        Check(delivered(current, pixel) && current.creates == 0 && current.game_resets == game_reset
                  && std::ranges::all_of(current.guides, [&](const auto& binding) { return binding.reset == reset; }),
              "mature two-pass XeFG keeps both models, resets only as expected, and delivers exact current RGBA");
      };
      const auto verify_guides = [&](const LoadReceipt& use, bool real, bool interpreted_inverted) {
        Check(use.guides.size() == 2, "both model evaluations expose their actual consumed guides");
        for (const auto& binding : use.guides) {
          const float expected_depth = (real ? .75f : (interpreted_inverted ? 0.f : 1.f));
          const std::array<uint16_t, 2> expected_motion{static_cast<uint16_t>(real ? 0x3c00u : 0u), 0};
          Check((binding.create_flags & guide_flags) == sdk_flags
                    && binding.depth_inverted == static_cast<int>(interpreted_inverted),
                "model creation keeps SDK depth and excludes inferred motion bits; Evaluate honors DepthMode");
          Check((binding.depth.Get() == depths[1].Get()) == real && (binding.motion.Get() == motions[1].Get()) == real
                    && uniform(binding.depth_bytes, expected_depth, size * size)
                    && uniform(binding.motion_bytes, expected_motion, size * size),
                "WARP readback at SDK consumption proves the bound far-plane depth/zero motion or unchanged real guides");
          std::cout << "XEFG_LOAD_BOUND_GUIDE " << nlohmann::json{
              {"real", real}, {"depth", expected_depth}, {"motion_bits", expected_motion},
              {"create_flags", binding.create_flags}, {"depth_inverted", binding.depth_inverted},
              {"reset", binding.reset}, {"samples_proven", size * size}}.dump() << std::endl;
        }
      };
      bool warmed = false;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8u && !warmed; ++attempt) {
        warmed = delivered(present_load("load-neutral-warm", scene_a, -1, false, false), scene_a);
      }
      Check(warmed, "two-pass load fixture completes cold preparation before the continuity measurement");

      const auto no_guides = present_load("no-guides", menu, -1, false, true);
      stable(no_guides, menu, 0, 0);
      const auto fresh_guides = present_load("fresh-guides", scene_b, 1, false, true);
      stable(fresh_guides, scene_b, 1, 0);
      const auto no_guides_again = present_load("no-guides-again", scene_a, -1, false, true);
      stable(no_guides_again, scene_a, 1, 0);
      const auto guides_return = present_load("fresh-guides-return", menu, 1, false, true);
      stable(guides_return, menu, 1, 0);
      verify_guides(no_guides, false, sdk_inverted);
      verify_guides(fresh_guides, true, sdk_inverted);
      verify_guides(no_guides_again, false, sdk_inverted);
      verify_guides(guides_return, true, sdk_inverted);
      const auto explicit_reset = present_load("explicit-game-reset", scene_b, 1, true, false);
      stable(explicit_reset, scene_b, 1, 1);

      native::depth_mode = (sdk_inverted ? 1u : 2u);
      const auto forced = present_load("opposite-depth-mode-neutral", scene_a, -1, false, true);
      stable(forced, scene_a, 1, 0);
      verify_guides(forced, false, !sdk_inverted);
      const auto forced_repeat = present_load("same-forced-depth-neutral-reuse", scene_b, -1, false, true);
      stable(forced_repeat, scene_b, 0, 0);
      verify_guides(forced_repeat, false, !sdk_inverted);
      const auto forced_real = present_load("opposite-depth-mode-real-unchanged", menu, 1, false, true);
      stable(forced_real, menu, 1, 0);
      verify_guides(forced_real, true, !sdk_inverted);
      native::depth_mode = 0;
      const auto automatic = present_load("restore-auto-depth-on-same-neutral-pair", scene_b, -1, false, true);
      stable(automatic, scene_b, 1, 0);
      verify_guides(automatic, false, sdk_inverted);

      if (!sdk_inverted) {
        const auto game_pixel = expected_pixels(scene_a, false);
        load.extent_height = load.extent_width = size / 2;
        auto seed_low = [&](ID3D12Resource* target, const auto& pixel) {
          const std::vector values((size / 2) * (size / 2), pixel);
          auto transfer = gpu.CreateTextureTransfer(target);
          gpu.Transition(target, present_state, D3D12_RESOURCE_STATE_COPY_DEST);
          gpu.UploadTexture(target, transfer, reinterpret_cast<const uint8_t*>(values.data()),
                            values.size() * sizeof(pixel), (size / 2) * sizeof(pixel));
          gpu.Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, present_state);
        };
        seed_low(low_depth.Get(), .25f);
        seed_low(low_motion.Get(), std::array<uint16_t, 2>{0, 0});
        submit();
        bool new_mature = false;
        const uint32_t previous_models = next_handle;
        std::array<bool, 2> first_eval_with_reset{};
        for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8u && !new_mature; ++attempt) {
          const uint32_t ready_models = next_handle;
          const auto changed = present_load("genuinely-smaller-guide-contract", scene_a, 2, false, false);
          for (const auto& binding : changed.guides) {
            Check(binding.model_creation > previous_models && binding.model_creation <= previous_models + 2
                      && binding.model_creation <= ready_models
                      && (binding.create_flags & guide_flags) == low_motion_flag,
                  "only a newly contracted model created on a previous recording can evaluate");
            const uint32_t idx = binding.model_creation - previous_models - 1;
            if (!first_eval_with_reset[idx]) {
              Check(binding.reset == 1, "each rebuilt model resets on its own first evaluation");
              first_eval_with_reset[idx] = true;
            }
          }
          if (changed.evaluates == 0) {
            Check(changed.deliveries == 0 && changed.pixels == game_pixel
                      && (changed.creates == 0 || changed.result == fg::Result::PREPARED),
                  "with no mature pass the SDK consumes the exact original frame");
          } else {
            // An already mature first pass may serve the current frame while
            // the second slot is created. It must never evaluate the new slot.
            Check(changed.deliveries == 1 && changed.result == fg::Result::SUBMITTED
                      && changed.pixels == expected_pixels(scene_a, true),
                  "the SDK consumes the exact current mature result, never a just-created slot");
          }
          if (delivered(changed, scene_a)) {
            Check(next_handle == previous_models + 2 && changed.creates == 0
                      && std::ranges::all_of(first_eval_with_reset, [](bool seen) { return seen; }),
                  "a true contract change rebuilds both models and recovers mature two-pass delivery");
            new_mature = true;
          }
        }
        Check(new_mature, "the changed creation contract achieves real two-pass delivery");
      }
      load.CloseSdk();
    }
    std::cout << "PASS xefg-load-neutral-guides evidence=WARP+MOCK continuity=1 two_pass=1 actual_pixels=1"
                 " depth_modes=1 game_reset=1 true_rebuild=1" << std::endl;
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
  const auto guide_provider = native::direct_api.module;
  const auto guide_provider_inits = control_mask_gpu_cases::runtime_initializations;
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
  Check(guide_handoff && guide_retained && !native::direct_api.private_instance
            && native::direct_api.module == guide_provider
            && control_mask_gpu_cases::runtime_initializations == guide_provider_inits,
        "the same original provider accepts the independent guides without replacing or reinitializing it");
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
    check_gray("before-first-cold-model-present", session);
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
    check_gray("actual-first-preparation-remains-gray", session);
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
  check_green("actual-delivery-after-first-preparation-is-green", session);
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
  std::cout << "PASS xefg-first-handoff-status evidence=WARP+MOCK fresh=1 prepared=1"
               " unserved=1 sdk_failure=1 selected_output=1 old_epoch=1 failed_disable=1" << std::endl;
  std::cout << "PASS xefg-present-handoff evidence=WARP+MOCK passes=1 same_backbuffer=1 full_rgba=1" << std::endl;
}
}  // namespace xefg_present_handoff_cases
