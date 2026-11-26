"""Relate bounded FG observations to native Execute batches and GPU packet retirement."""
import argparse
from collections import defaultdict
import csv
import hashlib
import json
import math
from pathlib import Path
import re
from statistics import mean, median


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def describe(values):
    values = sorted(values)
    if not values:
        return {"n": 0}
    return dict(n=len(values), mean=mean(values), median=median(values),
                p95=values[math.ceil(len(values) * .95) - 1],
                p99=values[math.ceil(len(values) * .99) - 1], maximum=values[-1])


def source_metrics(document):
    phases = defaultdict(list)
    for frame in document["frames"]:
        phases[frame["phase"]].append(frame)
    result = {}
    for name, rows in phases.items():
        sources = {(frame["source_handle"], frame["source_generation"]) for frame in rows}
        if len(sources) != 1:
            raise ValueError("A phase must contain one successful-SR source and generation")
        rows.sort(key=lambda f: (f["source_handle"], f["source_generation"], f["source_sequence"]))
        intervals = []
        pairs = []
        counts = defaultdict(int)
        costs = defaultdict(list)
        for frame in rows:
            counts[str(frame["delivery"])] += 1
            timing = frame.get("nr_gpu")
            if timing and timing.get("valid") and timing.get("samples") == 1 and frame["source_gpu"] == "complete":
                costs[frame["delivery"]].append(timing["total_us"] / 1000)
        for first, second in zip(rows, rows[1:]):
            if (first["source_handle"], first["source_generation"], first["source_sequence"] + 1) != (
                    second["source_handle"], second["source_generation"], second["source_sequence"]):
                continue
            if first["sr_result"] != 1 or second["sr_result"] != 1:
                continue
            elapsed = (second["sr_success_ns"] - first["sr_success_ns"]) / 1e6
            if elapsed <= 0:
                raise ValueError("Non-increasing successful SR clock")
            intervals.append(elapsed)
            if first["delivery"] == "refresh" and second["delivery"] == "reuse":
                timings = [row.get("nr_gpu") for row in (first, second)]
                if (all(row["source_gpu"] == "complete" for row in (first, second))
                        and all(t and t.get("valid") and t.get("samples") == 1 for t in timings)):
                    pairs.append(sum(t["total_us"] for t in timings) / 1000)
        result[name] = dict(frames=len(rows), deliveries=dict(counts),
                            successful_sr_interval_ms=describe(intervals),
                            successful_sr_fps=(1000 / mean(intervals) if intervals else None),
                            nr_gpu_ms_by_delivery={kind: describe(values) for kind, values in costs.items()},
                            refresh_reuse_pair_gpu_ms=describe(pairs))
    return result


def read_trace(path):
    header = None
    terminal = None
    rows = []
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for line_number, raw in enumerate(stream, 1):
            digest.update(raw)
            row = json.loads(raw)
            row["line"] = line_number
            if row["type"] == "header":
                if header is not None:
                    raise ValueError("Multiple ETW headers")
                header = row
            elif row["type"] == "end":
                if terminal is not None:
                    raise ValueError("Multiple ETW terminal records")
                terminal = row
            elif row["type"] == "event":
                if (row["provider"] in ("d3d12", "dxgi")
                        or row["provider"] == "dxgkrnl" and row["id"] in (178, 180)):
                    rows.append(row)
    if not header or not terminal or header.get("qpc_frequency", 0) <= 0:
        raise ValueError("Missing ETW identity, frequency or terminal record")
    losses = {name: terminal.get(name) for name in (
        "events_lost", "realtime_buffers_lost", "log_buffers_lost", "consumer_events_lost")}
    valid = all(value == 0 for value in losses.values())
    valid &= all(terminal.get(name) == 0 for name in ("stop_win32", "process_win32"))
    rows.sort(key=lambda row: (row["qpc"], row["line"]))
    return dict(header=header, terminal=terminal, rows=rows, valid=valid,
                losses=losses, sha256=digest.hexdigest())


def execute_batches(trace):
    stacks = defaultdict(list)
    completed = []
    for row in trace["rows"]:
        if row["provider"] != "d3d12" or row["id"] not in (53, 54, 139, 140):
            continue
        lists = tuple(row.get("lists", [row.get("list")]))
        if not lists or any(value is None for value in lists):
            continue
        key = row["pid"], row["tid"], row["queue"], lists, (53 if row["id"] in (53, 54) else 139)
        if row["id"] in (53, 139):
            stacks[key].append(row)
        elif stacks[key]:
            entry = stacks[key].pop()
            if row["qpc"] <= entry["qpc"]:
                raise ValueError("Invalid native Execute interval")
            completed.append(dict(entry=entry, returned=row))
    return completed


def packet_retirements(trace, batch):
    start, end = batch["entry"], batch["returned"]
    submissions = [row for row in trace["rows"]
                   if row["provider"] == "dxgkrnl" and row["id"] == 178 and row.get("packet_type") == 0
                   and row["pid"] == start["pid"] and row["tid"] == start["tid"]
                   and start["qpc"] <= row["qpc"] <= end["qpc"]]
    packets = []
    for submit in submissions:
        matches = [row for row in trace["rows"] if row["provider"] == "dxgkrnl" and row["id"] == 180
                   and (row.get("context"), row.get("sequence"), row.get("packet_type")) == (
                       submit["context"], submit["sequence"], 0) and row["qpc"] >= submit["qpc"]]
        packets.append(dict(submit_line=submit["line"], context=submit["context"], sequence=submit["sequence"],
                            retire_line=(matches[0]["line"] if len(matches) == 1 else None),
                            retire_qpc=(matches[0]["qpc"] if len(matches) == 1 else None),
                            retire_matches=len(matches)))
    complete = bool(packets) and all(packet["retire_qpc"] is not None for packet in packets)
    return dict(packets=packets, batch_retired_qpc=(max(p["retire_qpc"] for p in packets) if complete else None))


def relate_submission(trace, batches, fact):
    frequency = trace["header"]["qpc_frequency"]
    # Steady-clock nanoseconds on MSVC and these ETW records share QPC. Keep
    # integer cross-products rather than rounding a boundary by a clock tick.
    candidates = [batch for batch in batches
                  if batch["entry"]["tid"] == fact["thread_id"]
                  and batch["entry"]["queue"] == fact["queue"]
                  and fact["list"] in batch["entry"].get("lists", [batch["entry"].get("list")])
                  and fact["execute_entry_ns"] * frequency <= batch["entry"]["qpc"] * 1_000_000_000
                  and batch["returned"]["qpc"] * 1_000_000_000 <= fact["execute_return_ns"] * frequency]
    result = dict(first_submission=fact["first_submission"], reservation_index=fact["submission_index"],
                  signal_succeeded=fact["signal_succeeded"], execute_matches=len(candidates),
                  native_list=fact["list"], queue=fact["queue"])
    if not trace["valid"]:
        return dict(result, status="trace_loss_or_incomplete")
    if len(candidates) != 1:
        return dict(result, status="native_execute_not_unique")
    batch = candidates[0]
    result.update(execute_entry_line=batch["entry"]["line"], execute_return_line=batch["returned"]["line"])
    result["retirement_scope"] = "whole native Execute batch"
    result.update(packet_retirements(trace, batch))
    result["status"] = ("batch_retired" if result["batch_retired_qpc"] is not None else "batch_retirement_unproven")
    return result


def observation_metrics(trace, batches, observed):
    result = dict(request_generation=observed["request_generation"], state=observed["state"],
                  reason=observed["reason"], source_frames=observed["source_frames"],
                  overflow=observed["overflow"], submissions=[], frame_times_ms=[], sources=[])
    source_events = [event for event in observed["events"] if event["kind"] == "source_success"]
    for event in observed["events"]:
        if event["kind"] == "sr_queue_submission":
            fact = dict(event["submission"], thread_id=event["thread_id"])
            matched = relate_submission(trace, batches, fact)
            matched.update(list_id=fact["list_id"], generation=fact["generation"],
                           observation_sequence=event["sequence"])
            result["submissions"].append(matched)
        elif event["kind"] == "xefg_constants":
            constants = event["constants"]
            result["frame_times_ms"].append(dict(sdk_context=constants["sdk_context"], epoch=constants["epoch"],
                present_id=constants["present_id"], api_result=event["api_result"], requested_value=constants["frame_time_ms"]))
    for event in observed["events"]:
        if event["kind"] != "sr_copyback_recorded":
            continue
        source = event["source"]
        recording = source["recording"]
        matches = [row for row in result["submissions"]
                   if (row["list_id"], row["generation"]) == (recording["list_id"], recording["generation"])]
        cycle_start = next((row for row in source_events if row["source"] == {
            "handle": source["handle"], "generation": source["generation"], "sequence": source["sequence"]}), None)
        cycle_end = next((row["observed_ns"] for row in source_events
                          if row["observed_ns"] > event["observed_ns"]), None)
        presents = [row for row in observed["events"] if row["kind"] == "xefg_present_entry"
                    and row["thread_id"] == event["thread_id"] and cycle_start is not None
                    and cycle_end is not None and cycle_start["observed_ns"] <= row["observed_ns"] < cycle_end]
        first = [row for row in matches if row["first_submission"]]
        row = dict(source=source, matched_submissions=len(matches), first_submission_matches=len(first),
                   same_thread_proxy_calls_in_source_cycle=len(presents))
        if len(first) == 1 and len(presents) == 1 and first[0].get("batch_retired_qpc") is not None:
            present = presents[0]
            retired_ns = first[0]["batch_retired_qpc"] * 1_000_000_000 / trace["header"]["qpc_frequency"]
            row.update(proxy_call_id=present["call_id"], present_id=present["present"]["present_id"],
                       batch_retired_ms_before_proxy_entry=(present["observed_ns"] - retired_ns) / 1e6,
                       association="same-thread CPU source cycle; not a source-to-FG pixel proof")
            returns = [item for item in observed["events"] if item["kind"] == "xefg_present_return"
                       and item["call_id"] == present["call_id"]]
            if len(returns) == 1:
                row["batch_retired_ms_before_proxy_return"] = (returns[0]["api_return_ns"] - retired_ns) / 1e6
        result["sources"].append(row)
    return result


def service_metrics(document, phases):
    result = {}
    for phase in phases:
        if not phase["name"].startswith("present_"):
            continue
        first_ns, last_ns = phase["start"]["ns"], phase["end"]["ns"]
        rows = [row for row in document["events"]
                if first_ns <= row.get("xefg", {}).get("service_entry_ns", 0) < last_ns]
        rows.sort(key=lambda row: row["xefg"]["application_frame"])
        valid = [row for row in rows if row["status"] == "valid" and row["samples"] == 1
                 and row["gpu_state"] == "GPU complete: single submission" and row["xefg"]["output_ready"]]
        intervals = []
        pairs = []
        available = {id(row) for row in valid}
        index = 0
        while index + 1 < len(rows):
            a, b = rows[index:index + 2]
            x, y = a["xefg"], b["xefg"]
            adjacent = x["stream_epoch"] == y["stream_epoch"] and x["application_frame"] + 1 == y["application_frame"]
            if adjacent:
                intervals.append((y["service_entry_ns"] - x["service_entry_ns"]) / 1e6)
            reuse_phase = phase["name"].endswith("_x2")
            paired = x["refresh"] and (y["reuse"] if reuse_phase else y["refresh"])
            if adjacent and paired and id(a) in available and id(b) in available:
                pairs.append({key: (a["timing_us"][key] + b["timing_us"][key]) / 1000
                              for key in ("service_total", "eval", "staging", "writeback")})
                # Cadence uses every adjacent pair, while cost pairs do not overlap.
                if index + 2 < len(rows):
                    c = rows[index + 2]["xefg"]
                    if y["stream_epoch"] == c["stream_epoch"] and y["application_frame"] + 1 == c["application_frame"]:
                        intervals.append((c["service_entry_ns"] - y["service_entry_ns"]) / 1e6)
                index += 2
            else:
                index += 1
        result[phase["name"]] = dict(recorded_rows=len(rows), completed_ready_rows=len(valid),
            refreshes=sum(row["xefg"]["refresh"] for row in valid), reuses=sum(row["xefg"]["reuse"] for row in valid),
            model_evaluates=sum(row["xefg"]["model_evaluates"] for row in valid),
            application_service_interval_ms=describe(intervals),
            application_service_fps=(1000 / mean(intervals) if intervals else None),
            nonoverlapping_pair_gpu_ms={key: describe([pair[key] for pair in pairs])
                                       for key in ("service_total", "eval", "staging", "writeback")})
    return result


def sdk_metrics(phase):
    # The first emitted interval may begin before the phase. Later intervals
    # are bounded by the first and last emissions captured within this phase.
    rows = []
    context = None
    for line in phase["sdk_intervals"]:
        fields = dict(re.findall(r"(context|seconds|application_presents|status_samples|displayed_frames|sdk_fg_enabled)=([0-9a-fA-F.]+)", line))
        if not all(key in fields for key in ("context", "seconds", "application_presents", "displayed_frames", "sdk_fg_enabled")):
            raise ValueError("Incomplete XeFG interval")
        if context is not None and context != fields["context"]:
            return dict(status="multiple_contexts")
        context = fields["context"]
        rows.append(fields)
    complete = rows[1:]
    duration = sum(float(row["seconds"]) for row in complete)
    applications = sum(int(row["application_presents"]) for row in complete)
    outputs = sum(int(row["displayed_frames"]) for row in complete)
    samples_known = bool(complete) and all("status_samples" in row for row in complete)
    samples = (sum(int(row["status_samples"]) for row in complete) if samples_known else None)
    covered = samples_known and all(int(row["status_samples"]) == int(row["application_presents"]) for row in complete)
    return dict(complete_intervals=len(complete), seconds=duration, application_presents=applications,
                sampled_sdk_outputs=outputs, status_samples=samples, status_covers_all_applications=covered,
                application_fps=(applications / duration if duration else None),
                sdk_queued_fps=(outputs / duration if duration and covered else None),
                sampled_sdk_outputs_per_second=(outputs / duration if duration else None),
                sdk_outputs_per_application=(outputs / applications if applications and covered else None),
                sdk_outputs_per_status_sample=(outputs / samples if samples else None),
                interval_end_enabled=[int(row["sdk_fg_enabled"]) for row in complete],
                enabled_scope="last queried status at each interval end; interior state unobserved")


def display_metrics(path, phase, frequency, pid):
    groups = defaultdict(list)
    with Path(path).open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            if int(row["ProcessID"]) != pid:
                continue
            qpc = int(row["QPCTime"])
            if not (phase["start"]["ns"] * frequency <= qpc * 1_000_000_000 < phase["end"]["ns"] * frequency):
                continue
            groups[row["SwapChainAddress"]].append(row)
    result = {}
    for swapchain, rows in groups.items():
        displayed = sorted({int(row["QPCTime"]) + round(float(row["msUntilDisplayed"]) * frequency / 1000)
                            for row in rows if row["Dropped"] == "0" and float(row["msUntilDisplayed"]) > 0})
        intervals = [(b - a) * 1000 / frequency for a, b in zip(displayed, displayed[1:])]
        result[swapchain] = dict(present_events=len(rows), displayed_event_timestamps=len(displayed),
            present_api_ms=describe([float(row["msInPresentAPI"]) for row in rows]),
            displayed_event_interval_ms=describe(intervals),
            displayed_event_fps=(1000 / mean(intervals) if intervals else None),
            modes=sorted({row["PresentMode"] for row in rows}), generated_frame_identity="unknown")
    return result


def analyze(trace_path, benchmark_path=None):
    trace = read_trace(trace_path)
    batches = execute_batches(trace)
    result = dict(schema="nr_fg_timing_1", trace=dict(sha256=trace["sha256"], pid=trace["header"]["pid"],
                  qpc_frequency=trace["header"]["qpc_frequency"], valid=trace["valid"], losses=trace["losses"],
                  dxgkrnl_requested=trace["header"].get("dxgkrnl_requested"),
                  dxgkrnl_enabled=trace["terminal"].get("dxgkrnl_enabled"),
                  missing_fields=trace["terminal"].get("missing_fields")), native_execute_batches=len(batches))
    if benchmark_path:
        benchmark = read_json(benchmark_path)
        if benchmark.get("pid") != trace["header"]["pid"]:
            raise ValueError("Trace and benchmark belong to different processes")
        result["successful_sr"] = source_metrics(benchmark)
    return result, trace, batches


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--benchmark", type=Path)
    parser.add_argument("--observation", type=Path)
    parser.add_argument("--cadence", type=Path)
    parser.add_argument("--service", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.service and not args.cadence:
        parser.error("--service requires --cadence to select the measured phase windows")
    result, trace, batches = analyze(args.trace, args.benchmark)
    if args.observation:
        state = read_json(args.observation)["state"]
        if state.get("pid") != trace["header"]["pid"]:
            raise ValueError("Trace and recording observations belong to different processes")
        observation = state["edit_reuse"]["fg_observation"]
        result["observations"] = observation_metrics(trace, batches, observation)
    if args.cadence:
        cadence = read_json(args.cadence)
        if cadence["pid"] != trace["header"]["pid"]:
            raise ValueError("Trace and phase windows belong to different processes")
        phases = cadence["phases"]
        result["sdk_windows"] = {phase["name"]: sdk_metrics(phase) for phase in phases}
        result["display_events"] = {}
        for phase in phases:
            path = args.cadence.parent / (phase["name"] + "-display.csv")
            if path.exists():
                result["display_events"][phase["name"]] = display_metrics(path, phase, trace["header"]["qpc_frequency"], trace["header"]["pid"])
        if args.service:
            receipt = cadence["service_export"]
            if not re.fullmatch(rf"control-mask-{cadence['pid']}-[0-9]+-[0-9]+\.json", receipt["name"]):
                raise ValueError("Service export filename belongs to a different process")
            if hashlib.sha256(args.service.read_bytes()).hexdigest().lower() != receipt["sha256"].lower():
                raise ValueError("Service export differs from the captured receipt")
            service = read_json(args.service)
            if service.get("scope") != "xefg_single_backbuffer_service" or service.get("status") != "terminal":
                raise ValueError("Expected a terminal XeFG single-Backbuffer service export")
            result["present_service"] = service_metrics(service, phases)
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(result, output, ensure_ascii=False, indent=2)
        output.write("\n")


if __name__ == "__main__":
    main()
