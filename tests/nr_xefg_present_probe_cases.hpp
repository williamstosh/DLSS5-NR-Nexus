/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#if defined(NR_NEXUS_DEV_CONTROL) && defined(NR_NEXUS_TEST_XEFG_PRESENT_PROBE)
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <nr_xefg_present_probe.hpp>

namespace nr_xefg_present_probe_cases {
namespace probe = renodx::addons::dlss5::xefg_present_probe;
inline int Run() {
  struct Restore {
    probe::Snapshot saved = probe::CopySnapshot();
    Restore() {
      std::unique_lock lock(probe::probe_state.mutex);
      probe::probe_state.snapshot = {};
    }
    ~Restore() {
      std::unique_lock lock(probe::probe_state.mutex);
      probe::probe_state.snapshot = saved;
    }
  } restore;
  try {
    uint32_t checks = 0;
    const auto check = [&](bool value, const char* message) {
      ++checks;
      if (!value) throw std::runtime_error(message);
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
    std::cout << "PASS xefg-present-probe CPU/MOCK checks=" << checks << " evidence=" << probe::kEvidence << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL xefg-present-probe CPU/MOCK: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace nr_xefg_present_probe_cases
#endif
