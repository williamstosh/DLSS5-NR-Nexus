"""Synthetic checks for attribution boundaries used by the FG timing report."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import fg_timing as timing


def event(line, qpc, provider, event_id, **fields):
    return dict(line=line, qpc=qpc, provider=provider, id=event_id, pid=12, tid=34, **fields)


class TimingTests(unittest.TestCase):
    def fixture(self):
        rows = [event(1, 100, "d3d12", 139, queue=60, lists=[70]),
                event(2, 102, "dxgkrnl", 178, context=80, sequence=90, packet_type=0),
                event(3, 105, "d3d12", 140, queue=60, lists=[70]),
                event(4, 112, "dxgkrnl", 180, context=81, sequence=90, packet_type=0),
                event(5, 120, "dxgkrnl", 180, context=80, sequence=90, packet_type=0)]
        trace = dict(rows=rows, valid=True, header=dict(qpc_frequency=1_000_000_000))
        fact = dict(thread_id=34, queue=60, list=70, execute_entry_ns=99, execute_return_ns=106,
                    first_submission=True, submission_index=0, signal_succeeded=True)
        return trace, fact

    def test_exact_execute_and_packet_context(self):
        trace, fact = self.fixture()
        result = timing.relate_submission(trace, timing.execute_batches(trace), fact)
        self.assertEqual(result["status"], "batch_retired")
        self.assertEqual(result["batch_retired_qpc"], 120)
        self.assertEqual(result["packets"][0]["retire_line"], 5)

    def test_ambiguous_execute_and_loss_never_supply_retirement(self):
        trace, fact = self.fixture()
        batches = timing.execute_batches(trace)
        self.assertEqual(timing.relate_submission(trace, batches + batches, fact)["status"], "native_execute_not_unique")
        trace["valid"] = False
        result = timing.relate_submission(trace, batches, fact)
        self.assertEqual(result["status"], "trace_loss_or_incomplete")
        self.assertNotIn("batch_retired_qpc", result)

    def test_signal_failure_and_later_replay_do_not_repair_first_proof(self):
        trace, fact = self.fixture()
        fact["signal_succeeded"] = False
        result = timing.relate_submission(trace, timing.execute_batches(trace), fact)
        self.assertFalse(result["signal_succeeded"])
        self.assertEqual(result["batch_retired_qpc"], 120)
        fact.update(execute_entry_ns=130, execute_return_ns=150, first_submission=False, submission_index=1)
        replay = timing.relate_submission(trace, timing.execute_batches(trace), fact)
        self.assertEqual(replay["status"], "native_execute_not_unique")
        self.assertFalse(replay["first_submission"])

    def test_missing_packet_retirement_remains_unknown(self):
        trace, fact = self.fixture()
        trace["rows"].pop()
        result = timing.relate_submission(trace, timing.execute_batches(trace), fact)
        self.assertEqual(result["status"], "batch_retirement_unproven")
        self.assertIsNone(result["batch_retired_qpc"])

    def test_sr_gaps_and_new_sources_are_distinct(self):
        frames = [dict(source_handle=1, source_generation=1, source_sequence=sequence,
                       phase="phase", delivery="per_frame", sr_result=1, sr_success_ns=ns,
                       source_gpu="complete", nr_gpu=None)
                  for sequence, ns in ((1, 100_000_000), (2, 140_000_000), (4, 190_000_000))]
        result = timing.source_metrics(dict(frames=frames))["phase"]
        self.assertEqual(result["successful_sr_interval_ms"]["n"], 1)
        self.assertEqual(result["successful_sr_fps"], 25)
        frames[-1]["source_handle"] = 2
        with self.assertRaisesRegex(ValueError, "one successful-SR"):
            timing.source_metrics(dict(frames=frames))

    def test_sdk_window_discards_partial_prefix(self):
        phase = dict(sdk_intervals=[
            "context=ab seconds=5.0 application_presents=200 status_samples=200 displayed_frames=800 sdk_fg_enabled=1",
            "context=ab seconds=5.0 application_presents=125 status_samples=125 displayed_frames=500 sdk_fg_enabled=1"])
        result = timing.sdk_metrics(phase)
        self.assertEqual(result["complete_intervals"], 1)
        self.assertEqual(result["application_fps"], 25)
        self.assertEqual(result["sdk_queued_fps"], 100)

    def test_sdk_partial_status_is_not_a_full_output_rate_or_multiplier(self):
        line = "context=ab seconds=5.0 application_presents=125 status_samples=100 displayed_frames=400 sdk_fg_enabled=1"
        result = timing.sdk_metrics(dict(sdk_intervals=[line, line]))
        self.assertFalse(result["status_covers_all_applications"])
        self.assertIsNone(result["sdk_queued_fps"])
        self.assertIsNone(result["sdk_outputs_per_application"])
        self.assertEqual(result["sdk_outputs_per_status_sample"], 4)
        self.assertEqual(result["interval_end_enabled"], [1])
        self.assertNotIn("all_complete_intervals_enabled", result)

    def test_documented_status_receipt_runs_through_cli_and_checks_pid(self):
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory(prefix="fg-timing-", dir=root / "tmp") as name:
            directory = Path(name)
            trace = directory / "trace.jsonl"
            trace.write_text("\n".join(json.dumps(row) for row in (
                dict(type="header", pid=12, qpc_frequency=10_000_000),
                dict(type="end", stop_win32=0, process_win32=0, events_lost=0,
                     realtime_buffers_lost=0, log_buffers_lost=0, consumer_events_lost=0))) + "\n", encoding="utf-8")
            observation = dict(request_generation=1, state="stopped", reason="requested_stop",
                               source_frames=0, overflow=0, events=[])
            status = dict(ok=True, state=dict(pid=12, edit_reuse=dict(fg_observation=observation)))
            receipt = directory / "observation.json"
            receipt.write_text(json.dumps(status), encoding="utf-8")
            output = directory / "result.json"
            command = [sys.executable, "-B", timing.__file__, "--trace", str(trace),
                       "--observation", str(receipt), "--output", str(output)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(output.read_text())["observations"]["sources"], [])
            status["state"]["pid"] = 13
            receipt.write_text(json.dumps(status), encoding="utf-8")
            command[-1] = str(directory / "wrong-process.json")
            failed = subprocess.run(command, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn("different processes", failed.stderr)
            self.assertFalse((directory / "wrong-process.json").exists())

    def test_service_export_uses_captured_process_filename_and_hash(self):
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory(prefix="fg-timing-", dir=root / "tmp") as name:
            directory = Path(name)
            trace = directory / "trace.jsonl"
            trace.write_text("\n".join(json.dumps(row) for row in (
                dict(type="header", pid=12, qpc_frequency=10_000_000),
                dict(type="end", stop_win32=0, process_win32=0, events_lost=0,
                     realtime_buffers_lost=0, log_buffers_lost=0, consumer_events_lost=0))) + "\n", encoding="utf-8")
            service = directory / "present-service.json"
            service.write_text(json.dumps(dict(scope="xefg_single_backbuffer_service", status="terminal", events=[])), encoding="utf-8")
            phase = dict(pid=12, phases=[], service_export=dict(name="control-mask-12-123-1.json",
                         sha256=timing.hashlib.sha256(service.read_bytes()).hexdigest()))
            cadence = directory / "cadence.json"
            cadence.write_text(json.dumps(phase), encoding="utf-8")
            command = [sys.executable, "-B", timing.__file__, "--trace", str(trace), "--cadence", str(cadence),
                       "--service", str(service), "--output", str(directory / "valid.json")]
            result = subprocess.run(command, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            phase["service_export"]["name"] = "control-mask-13-123-1.json"
            cadence.write_text(json.dumps(phase), encoding="utf-8")
            command[-1] = str(directory / "wrong-service.json")
            failed = subprocess.run(command, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn("different process", failed.stderr)
            self.assertFalse((directory / "wrong-service.json").exists())


if __name__ == "__main__":
    unittest.main()
