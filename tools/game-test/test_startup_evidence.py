"""Deterministic routing tests and one owned, windowless CPU debuggee."""
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

import startup_evidence as evidence


ROOT = Path(__file__).resolve().parents[2]
OUTPUT = Path(os.environ.get("NR_NEXUS_STARTUP_TEST_OUTPUT", str(ROOT / "tmp" / "startup-evidence-tests")))
IDENTITY = dict(pid=123, created=7, path=str(Path(sys.executable).resolve()))


def exception(code=evidence.STACK_OVERFLOW, first=True):
    return dict(kind=evidence.EXCEPTION_DEBUG_EVENT, pid=123, tid=456, code=code,
                flags=0, address=0x123456, next=0, first_chance=first,
                parameters=[], record_bytes=bytes(152))


def region(base, allocation, size, protect, state=evidence.MEM_COMMIT):
    return dict(base=base, allocation_base=allocation, size=size, protect=protect,
                allocation_protect=4, state=state, type=0x20000)


class FakeAPI:
    page_size = 4096
    maximum_address = 0x3FFF

    def __init__(self, events, stop=None):
        self.events = list(events)
        self.stop = stop
        self.calls = []
        self.regions = [region(0, 0, 0x1000, 0, 0x10000), region(0x1000, 0x1000, 0x1000, 4),
                        region(0x2000, 0x1000, 0x1000, 0x20), region(0x3000, 0, 0x1000, 0, 0x10000)]

    def open_process(self, pid):
        self.calls.append(("open", pid))
        return 17

    def identity(self, handle):
        self.calls.append(("identity", handle))
        return dict(IDENTITY)

    def running(self, handle):
        return True

    def verify_architecture(self, handle):
        pass

    def attach(self, pid):
        self.calls.append(("attach", pid))

    def keep_target_on_exit(self):
        self.calls.append(("kill_on_exit", False))

    def detach(self, pid):
        self.calls.append(("detach", pid))

    def close(self, handle):
        self.calls.append(("close", handle))

    def wait(self, milliseconds):
        if self.events:
            return self.events.pop(0)
        self.stop.write_text("stop", encoding="ascii")
        return None

    def resume(self, event, status):
        self.calls.append(("resume", event["kind"], event.get("code"), status))

    def is_attach_breakpoint(self, event, modules):
        return event["address"] == 0x123456

    def module_path(self, handle, event):
        return "module.dll"

    def context(self, tid):
        context = bytearray(evidence.CONTEXT_SIZE)
        struct.pack_into("<I", context, 48, evidence.CONTEXT_ALL_X64)
        struct.pack_into("<Q", context, 152, 0xABC000)
        struct.pack_into("<Q", context, 248, 0x123456)
        return bytes(context)

    def query(self, handle, address):
        return next(r for r in self.regions if r["base"] <= address < r["base"] + r["size"])

    def read(self, handle, address, size):
        return b"H" * size if address < 0x2000 else b"X" * size


class StartupEvidenceTests(unittest.TestCase):
    def setUp(self):
        OUTPUT.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="startup-unit-", dir=OUTPUT)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def run_observer(self, events, **kwargs):
        api = FakeAPI(events, self.root / "stop")
        capture = mock.Mock(return_value=dict(status="captured"))
        result = evidence.observe(IDENTITY, self.root / "out", stop_file=api.stop,
                                  api=api, dump_capture=capture, **kwargs)
        return result, api, capture

    def test_identity_mismatch_never_attaches(self):
        api = FakeAPI([])
        expected = dict(IDENTITY, created=8)
        result = evidence.observe(expected, self.root / "out", api=api)
        self.assertEqual(result["status"], "identity_mismatch")
        self.assertFalse(any(call[0] == "attach" for call in api.calls))
        self.assertEqual(api.calls[-1], ("close", 17))

    def test_stop_detaches_without_capture(self):
        result, api, capture = self.run_observer([exception(evidence.BREAKPOINT)])
        self.assertEqual(result["status"], "stopped_without_exception")
        self.assertTrue(result["detached"])
        self.assertFalse(result["kill_on_exit"])
        self.assertTrue(result["initial_breakpoint_handled"])
        self.assertLess(api.calls.index(("attach", 123)), api.calls.index(("kill_on_exit", False)))
        capture.assert_not_called()

    def test_stack_exception_captures_context_allocation_header_and_passes_through(self):
        result, api, capture = self.run_observer([exception(evidence.BREAKPOINT), exception()])
        self.assertEqual(result["status"], "captured", result)
        self.assertTrue(result["detached"])
        self.assertIn(("resume", 1, evidence.STACK_OVERFLOW, evidence.DBG_EXCEPTION_NOT_HANDLED), api.calls)
        self.assertEqual((self.root / "out" / "thread-context-x64.bin").stat().st_size, 1232)
        allocation = json.loads((self.root / "out" / "executable-allocations.json").read_text())["allocations"][0]
        self.assertEqual(allocation["allocation_base"], 0x1000)
        self.assertEqual(allocation["bytes"], 8192)
        self.assertEqual((self.root / "out" / allocation["chunks"][0]["file"]).read_bytes(), b"H" * 4096)
        self.assertTrue(0 < capture.call_args.kwargs["timeout"] <= 20)

    def test_later_breakpoint_and_other_first_chance_are_not_swallowed(self):
        events = [exception(evidence.BREAKPOINT), exception(evidence.BREAKPOINT), exception(0xE0424242), exception()]
        result, api, _ = self.run_observer(events)
        self.assertEqual(result["status"], "captured")
        resumes = [call for call in api.calls if call[0] == "resume"]
        self.assertEqual([r[-1] for r in resumes], [evidence.DBG_CONTINUE] + [evidence.DBG_EXCEPTION_NOT_HANDLED] * 3)

    def test_first_breakpoint_outside_attach_site_is_not_swallowed(self):
        unexpected = dict(exception(evidence.BREAKPOINT), address=0x888000)
        result, api, _ = self.run_observer([unexpected, exception(evidence.BREAKPOINT), exception()])
        self.assertEqual(result["status"], "captured")
        resumes = [call for call in api.calls if call[0] == "resume"]
        self.assertEqual([r[-1] for r in resumes], [evidence.DBG_EXCEPTION_NOT_HANDLED,
                                                evidence.DBG_CONTINUE, evidence.DBG_EXCEPTION_NOT_HANDLED])

    def test_second_chance_other_exception_is_captured(self):
        result, _, _ = self.run_observer([exception(0xC0000005, False)])
        self.assertEqual(result["status"], "captured")
        saved = json.loads((self.root / "out" / "exception.json").read_text())
        self.assertEqual(saved["code"], 0xC0000005)
        self.assertFalse(saved["first_chance"])

    def test_dump_failure_still_resumes_and_detaches(self):
        api = FakeAPI([exception()])
        result = evidence.observe(IDENTITY, self.root / "out", api=api,
                                  dump_capture=lambda *args, **kwargs: dict(status="timeout"))
        self.assertEqual(result["status"], "captured_with_gaps")
        self.assertTrue(result["detached"])
        self.assertIn(("resume", 1, evidence.STACK_OVERFLOW, evidence.DBG_EXCEPTION_NOT_HANDLED), api.calls)

    def test_partial_executable_memory_cannot_report_full_capture(self):
        partial = dict(byte_budget=4096, allocation_byte_budget=4096,
                       bytes=4096, coverage="partial", allocations=[])
        with mock.patch.object(evidence, "capture_allocations", return_value=partial):
            result, _, _ = self.run_observer([exception()])
        self.assertEqual(result["status"], "captured_with_gaps")
        self.assertEqual(result["capture"]["executable_memory"]["coverage"], "partial")

    def test_file_handles_are_closed_even_when_module_path_fails(self):
        event = dict(kind=3, pid=123, tid=456, file=91, base=0x400000)
        api = FakeAPI([event])
        api.module_path = mock.Mock(side_effect=OSError("synthetic path failure"))
        result = evidence.observe(IDENTITY, self.root / "out", api=api)
        self.assertEqual(result["status"], "failed")
        self.assertIn(("close", 91), api.calls)
        self.assertIn(("resume", 3, None, evidence.DBG_CONTINUE), api.calls)
        self.assertTrue(result["detached"])

    def test_failed_kill_on_exit_setting_detaches_immediately(self):
        api = FakeAPI([])
        api.keep_target_on_exit = mock.Mock(side_effect=OSError("synthetic API failure"))
        result = evidence.observe(IDENTITY, self.root / "out", api=api)
        self.assertEqual(result["status"], "failed")
        self.assertIn(("detach", 123), api.calls)
        self.assertTrue(result["detached"])

    def test_allocation_byte_budget_reports_truncation(self):
        api = FakeAPI([])
        result = evidence.capture_allocations(api, 17, api.regions, [], self.root,
                    time.monotonic() + 1, max_bytes=4096, allocation_bytes=4096)
        self.assertEqual(result["bytes"], 4096)
        self.assertEqual(result["coverage"], "partial")
        self.assertTrue(result["allocations"][0]["truncated"])

    def test_module_seed_checks_disk_hash_and_runtime_pe_before_reading_page(self):
        path = self.root / "module.dll"
        image = bytearray(4096)
        image[:2] = b"MZ"
        struct.pack_into("<I", image, 0x3C, 0x80)
        image[0x80:0x84] = b"PE\0\0"
        struct.pack_into("<I", image, 0x88, 123)
        struct.pack_into("<H", image, 0x98, 0x20B)
        struct.pack_into("<I", image, 0xD0, 4096)
        path.write_bytes(image)
        seed = dict(module=str(path), sha256=hashlib.sha256(image).hexdigest().upper(),
                    timestamp=123, size_of_image=4096, rva=0)
        config = self.root / "seeds.json"
        config.write_text(json.dumps([seed]), encoding="utf-8")
        loaded = evidence.load_seeds(config)
        api = FakeAPI([])
        api.read = mock.Mock(side_effect=lambda handle, address, size: bytes(image[address - 0x400000:address - 0x400000 + size]))
        records, contents = evidence.capture_seeds(api, 17, {0x400000: dict(base=0x400000, path=str(path))},
                                                  loaded, self.root, time.monotonic() + 1)
        self.assertEqual(records[0]["status"], "captured", records)
        self.assertEqual(contents, [bytes(image)])
        loaded[0]["sha256"] = "0" * 64
        records, contents = evidence.capture_seeds(api, 17, {0x400000: dict(base=0x400000, path=str(path))},
                                                  loaded, self.root, time.monotonic() + 1)
        self.assertEqual(records[0]["status"], "failed")
        self.assertEqual(contents, [])

    @unittest.skipUnless(os.name == "nt" and C.sizeof(C.c_void_p) == 8, "native Windows x64 only")
    def test_owned_cpu_process_real_attach_exception_memory_and_detach(self):
        child_script = self.root / "fixture.py"
        child_script.write_text(FIXTURE, encoding="utf-8")
        # Windows venv python.exe can be a redirector that spawns the interpreter.
        # The explicitly owned debuggee must be the interpreter, not that stub.
        interpreter = str(Path(getattr(sys, "_base_executable", sys.executable)).resolve())
        child = subprocess.Popen([interpreter, "-B", str(child_script), str(self.root)],
                                 creationflags=0x08000000)
        watcher = None
        try:
            api = evidence.Win32()
            process = api.open_process(child.pid)
            try:
                identity = api.identity(process)
            finally:
                api.close(process)
            owned = self.root / "owned-process.json"
            owned.write_text(json.dumps(identity), encoding="utf-8")
            out = self.root / "real-evidence"
            watcher = subprocess.Popen([interpreter, "-B", evidence.__file__, "--process", str(owned),
                      "--output", str(out), "--stop-file", str(self.root / "stop-real"),
                      "--timeout", "15", "--capture-timeout", "15"],
                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", creationflags=0x08000000)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                state = out / "state.json"
                if state.exists() and json.loads(state.read_text())["state"] == "observing":
                    break
                if watcher.poll() is not None:
                    self.fail("Watcher exited before observing: " + repr(watcher.communicate()))
                time.sleep(0.02)
            else:
                self.fail("Watcher did not acknowledge attachment")
            (self.root / "go").write_text("go", encoding="ascii")
            stdout, stderr = watcher.communicate(timeout=25)
            self.assertEqual(watcher.returncode, 0, stderr + stdout)
            child.wait(timeout=5)
            self.assertEqual(child.returncode, 0)
            manifest = json.loads((out / "manifest.json").read_text())
            fixture = json.loads((self.root / "fixture-result.json").read_text())
            self.assertEqual(manifest["status"], "captured", manifest)
            self.assertTrue(manifest["detached"])
            self.assertFalse(manifest["kill_on_exit"])
            self.assertEqual(fixture["handled"], [0xE0424242, evidence.STACK_OVERFLOW])
            context = (out / "thread-context-x64.bin").read_bytes()
            self.assertEqual(len(context), 1232)
            self.assertNotEqual(struct.unpack_from("<Q", context, 248)[0], 0)
            event = json.loads((out / "exception.json").read_text())
            self.assertEqual(event["code"], evidence.STACK_OVERFLOW)
            self.assertTrue(event["first_chance"])
            allocations = json.loads((out / "executable-allocations.json").read_text())
            target = next(a for a in allocations["allocations"] if a["allocation_base"] == fixture["allocation_base"])
            self.assertTrue(any(b"NR_NEXUS_EXECUTABLE_PAGE_CPU_FIXTURE" in (out / c["file"]).read_bytes()
                                for c in target["chunks"] if c["status"] == "captured"))
            self.assertTrue(any(b"NR_NEXUS_ALLOCATION_HEADER_CPU_FIXTURE" in (out / c["file"]).read_bytes()
                                for c in target["chunks"] if c["status"] == "captured"))
            self.assertTrue(json.loads((out / "virtual-memory.json").read_text())["complete"])
            with (out / "stack-dump" / "game.dmp").open("rb") as stream:
                header = stream.read(32)
                self.assertEqual(header[:4], b"MDMP")
                count, offset = struct.unpack_from("<II", header, 8)
                stream.seek(offset)
                kinds = [struct.unpack("<III", stream.read(12))[0] for _ in range(count)]
            self.assertIn(3, kinds)
            self.assertIn(4, kinds)
            self.assertNotIn(6, kinds, "This normal dump must not masquerade as an exception dump")
            proof = dict(evidence="CPU synthetic RaiseException, not actual stack exhaustion or game evidence",
                         monitor_context=dict(pid=os.getpid(), executable=sys.executable, cwd=str(Path.cwd()),
                                              localappdata_environment=os.environ.get("LOCALAPPDATA"),
                                              normal_context_marker=os.environ.get("NR_NEXUS_NORMAL_USER_CONTEXT")),
                         child=fixture, observer_manifest=manifest, context_bytes=len(context),
                         event=event, captured_allocation=target, minidump_streams=kinds,
                         watcher_exit=watcher.returncode, child_exit=child.returncode)
            evidence.write_json(OUTPUT / "real-api-proof.json", proof)
        finally:
            (self.root / "stop-real").write_text("stop", encoding="ascii")
            (self.root / "go").write_text("go", encoding="ascii")
            if watcher is not None and watcher.poll() is None:
                watcher.communicate(timeout=35)
            # This is the explicitly created fixture only; the observer never kills a target.
            if child.poll() is None:
                child.kill()
            child.wait(timeout=5)


FIXTURE = r'''
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import sys
import time

root = Path(sys.argv[1])
deadline = time.monotonic() + 25
while not (root / "go").exists():
    if time.monotonic() >= deadline:
        raise SystemExit(2)
    time.sleep(0.01)
k = C.WinDLL("kernel32", use_last_error=True)
k.VirtualAlloc.argtypes = [C.c_void_p, C.c_size_t, W.DWORD, W.DWORD]
k.VirtualAlloc.restype = C.c_void_p
k.VirtualProtect.argtypes = [C.c_void_p, C.c_size_t, W.DWORD, C.POINTER(W.DWORD)]
k.VirtualProtect.restype = W.BOOL
k.VirtualFree.argtypes = [C.c_void_p, C.c_size_t, W.DWORD]
k.VirtualFree.restype = W.BOOL
k.RaiseException.argtypes = [W.DWORD, W.DWORD, W.DWORD, C.c_void_p]
CALLBACK = C.WINFUNCTYPE(C.c_long, C.c_void_p)
k.AddVectoredExceptionHandler.argtypes = [W.ULONG, CALLBACK]
k.AddVectoredExceptionHandler.restype = C.c_void_p
k.RemoveVectoredExceptionHandler.argtypes = [C.c_void_p]
k.RemoveVectoredExceptionHandler.restype = W.ULONG
handled = []
@CALLBACK
def handler(pointers):
    record = C.c_void_p.from_address(pointers).value
    code = C.c_uint32.from_address(record).value
    if code in (0xE0424242, 0xC00000FD):
        handled.append(code)
        return -1
    return 0
registration = k.AddVectoredExceptionHandler(1, handler)
base = k.VirtualAlloc(None, 65536, 0x3000, 4)
if not base or not registration:
    raise C.WinError(C.get_last_error())
try:
    header = b"NR_NEXUS_ALLOCATION_HEADER_CPU_FIXTURE"
    executable = b"NR_NEXUS_EXECUTABLE_PAGE_CPU_FIXTURE"
    C.memmove(base, header, len(header))
    C.memmove(base + 4096, executable, len(executable))
    old = W.DWORD()
    if not k.VirtualProtect(base + 4096, 4096, 0x20, C.byref(old)):
        raise C.WinError(C.get_last_error())
    k.RaiseException(0xE0424242, 0, 0, None)
    k.RaiseException(0xC00000FD, 0, 0, None)
    result = dict(pid=os.getpid(), parent_pid=os.getppid(), executable=sys.executable,
                  localappdata_environment=os.environ.get("LOCALAPPDATA"),
                  normal_context_marker=os.environ.get("NR_NEXUS_NORMAL_USER_CONTEXT"),
                  allocation_base=base, executable_page=base + 4096, handled=handled)
    (root / "fixture-result.json").write_text(json.dumps(result), encoding="utf-8")
finally:
    k.RemoveVectoredExceptionHandler(registration)
    k.VirtualFree(base, 0, 0x8000)
'''


if __name__ == "__main__":
    unittest.main()
