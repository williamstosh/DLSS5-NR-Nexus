"""Observe one verified Windows process; capture before an exception can exit it.

This debugger reads memory only. It neither launches nor stops the target and does
not change WER. Attachment changes exception dispatch and timing; exceptions that
occur before attachment can be missed. A normal hang_evidence dump has no
ExceptionStream: exception.json and thread-context-x64.bin preserve the event.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys
import time

import hang_evidence


EXCEPTION_DEBUG_EVENT = 1
CREATE_THREAD_DEBUG_EVENT = 2
CREATE_PROCESS_DEBUG_EVENT = 3
EXIT_THREAD_DEBUG_EVENT = 4
EXIT_PROCESS_DEBUG_EVENT = 5
LOAD_DLL_DEBUG_EVENT = 6
UNLOAD_DLL_DEBUG_EVENT = 7
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001
BREAKPOINT = 0x80000003
STACK_OVERFLOW = 0xC00000FD
MEM_COMMIT = 0x1000
MEM_IMAGE = 0x1000000
PAGE_GUARD = 0x100
READABLE = {0x02, 0x04, 0x08, 0x20, 0x40, 0x80}
EXECUTABLE = {0x10, 0x20, 0x40, 0x80}
MAX_RAW_BYTES = 16 * 1024 * 1024
MAX_ALLOCATION_BYTES = 256 * 1024
CONTEXT_SIZE = 1232
CONTEXT_ALL_X64 = 0x10001F


class IdentityMismatch(RuntimeError):
    pass


class EXCEPTION_RECORD(C.Structure):
    _fields_ = [("code", W.DWORD), ("flags", W.DWORD), ("next", C.c_void_p),
                ("address", C.c_void_p), ("count", W.DWORD),
                ("parameters", C.c_size_t * 15)]


class EXCEPTION_DEBUG_INFO(C.Structure):
    _fields_ = [("record", EXCEPTION_RECORD), ("first_chance", W.DWORD)]


class CREATE_PROCESS_DEBUG_INFO(C.Structure):
    _fields_ = [("file", W.HANDLE), ("process", W.HANDLE), ("thread", W.HANDLE),
                ("base", C.c_void_p), ("debug_offset", W.DWORD), ("debug_size", W.DWORD),
                ("teb", C.c_void_p), ("start", C.c_void_p),
                ("image_name", C.c_void_p), ("unicode", W.WORD)]


class CREATE_THREAD_DEBUG_INFO(C.Structure):
    _fields_ = [("thread", W.HANDLE), ("teb", C.c_void_p), ("start", C.c_void_p)]


class LOAD_DLL_DEBUG_INFO(C.Structure):
    _fields_ = [("file", W.HANDLE), ("base", C.c_void_p), ("debug_offset", W.DWORD),
                ("debug_size", W.DWORD), ("image_name", C.c_void_p), ("unicode", W.WORD)]


class DEBUG_UNION(C.Union):
    _fields_ = [("exception", EXCEPTION_DEBUG_INFO),
                ("create_process", CREATE_PROCESS_DEBUG_INFO),
                ("create_thread", CREATE_THREAD_DEBUG_INFO),
                ("load_dll", LOAD_DLL_DEBUG_INFO), ("base", C.c_void_p),
                ("exit_code", W.DWORD), ("raw", C.c_ubyte * 160)]


class DEBUG_EVENT(C.Structure):
    _fields_ = [("kind", W.DWORD), ("pid", W.DWORD), ("tid", W.DWORD), ("info", DEBUG_UNION)]


class MEMORY_BASIC_INFORMATION(C.Structure):
    _fields_ = [("base", C.c_void_p), ("allocation_base", C.c_void_p),
                ("allocation_protect", W.DWORD), ("partition", W.WORD),
                ("size", C.c_size_t), ("state", W.DWORD),
                ("protect", W.DWORD), ("type", W.DWORD)]


class SYSTEM_INFO(C.Structure):
    _fields_ = [("processor", W.DWORD), ("page_size", W.DWORD),
                ("minimum", C.c_void_p), ("maximum", C.c_void_p),
                ("mask", C.c_size_t), ("processors", W.DWORD),
                ("processor_type", W.DWORD), ("allocation_granularity", W.DWORD),
                ("level", W.WORD), ("revision", W.WORD)]


def stamp():
    return datetime.now(timezone.utc).isoformat()


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".new")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def number(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def load_seeds(path):
    if path is None:
        return []
    seeds = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    if not isinstance(seeds, list) or len(seeds) > 8:
        raise ValueError("Module seeds must be an array of at most eight pages")
    result = []
    for seed in seeds:
        module = seed["module"]
        digest = seed["sha256"]
        if (not isinstance(module, str) or not module
                or (not Path(module).is_absolute() and Path(module).name != module)
                or not re.fullmatch(r"[0-9a-fA-F]{64}", digest)):
            raise ValueError("Each module seed needs an absolute path or basename and exact SHA256")
        item = dict(module=module, sha256=digest.upper(), timestamp=number(seed["timestamp"]),
                    size_of_image=number(seed["size_of_image"]), rva=number(seed["rva"]))
        if not 0 <= item["rva"] < item["size_of_image"]:
            raise ValueError("Module seed RVA is outside its expected image")
        result.append(item)
    return result


def pe_identity(read):
    dos = read(0, 64)
    if dos[:2] != b"MZ":
        raise ValueError("Missing DOS signature")
    offset = struct.unpack_from("<I", dos, 0x3C)[0]
    if offset > 1024 * 1024:
        raise ValueError("PE header offset is out of bounds")
    header = read(offset, 88)
    if header[:4] != b"PE\0\0" or struct.unpack_from("<H", header, 24)[0] != 0x20B:
        raise ValueError("Expected a PE32+ image")
    return dict(timestamp=struct.unpack_from("<I", header, 8)[0],
                size_of_image=struct.unpack_from("<I", header, 80)[0])


class Win32:
    def __init__(self):
        if os.name != "nt" or C.sizeof(C.c_void_p) != 8:
            raise OSError("A native x64 Windows Python is required")
        if C.sizeof(DEBUG_EVENT) != 176 or C.sizeof(MEMORY_BASIC_INFORMATION) != 48:
            raise OSError("Unexpected Windows x64 structure layout")
        self.k = k = C.WinDLL("kernel32", use_last_error=True)
        self.psapi = C.WinDLL("psapi", use_last_error=True)
        signatures = {
            "OpenProcess": ([W.DWORD, W.BOOL, W.DWORD], W.HANDLE),
            "CloseHandle": ([W.HANDLE], W.BOOL),
            "GetProcessTimes": ([W.HANDLE] + [C.POINTER(W.FILETIME)] * 4, W.BOOL),
            "GetProcessId": ([W.HANDLE], W.DWORD),
            "QueryFullProcessImageNameW": ([W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)], W.BOOL),
            "WaitForSingleObject": ([W.HANDLE, W.DWORD], W.DWORD),
            "IsWow64Process": ([W.HANDLE, C.POINTER(W.BOOL)], W.BOOL),
            "DebugActiveProcess": ([W.DWORD], W.BOOL),
            "DebugSetProcessKillOnExit": ([W.BOOL], W.BOOL),
            "DebugActiveProcessStop": ([W.DWORD], W.BOOL),
            "WaitForDebugEvent": ([C.POINTER(DEBUG_EVENT), W.DWORD], W.BOOL),
            "ContinueDebugEvent": ([W.DWORD, W.DWORD, W.DWORD], W.BOOL),
            "ReadProcessMemory": ([W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t,
                                   C.POINTER(C.c_size_t)], W.BOOL),
            "VirtualQueryEx": ([W.HANDLE, C.c_void_p, C.POINTER(MEMORY_BASIC_INFORMATION), C.c_size_t], C.c_size_t),
            "GetNativeSystemInfo": ([C.POINTER(SYSTEM_INFO)], None),
            "OpenThread": ([W.DWORD, W.BOOL, W.DWORD], W.HANDLE),
            "GetThreadContext": ([W.HANDLE, C.c_void_p], W.BOOL),
            "GetFinalPathNameByHandleW": ([W.HANDLE, W.LPWSTR, W.DWORD, W.DWORD], W.DWORD),
            "GetModuleHandleW": ([W.LPCWSTR], W.HMODULE),
            "GetProcAddress": ([W.HMODULE, C.c_char_p], C.c_void_p),
        }
        for name, (args, result) in signatures.items():
            getattr(k, name).argtypes, getattr(k, name).restype = args, result
        self.psapi.GetModuleFileNameExW.argtypes = [W.HANDLE, W.HMODULE, W.LPWSTR, W.DWORD]
        self.psapi.GetModuleFileNameExW.restype = W.DWORD
        system = SYSTEM_INFO()
        k.GetNativeSystemInfo(C.byref(system))
        if system.processor & 0xFFFF != 9:
            raise OSError("Only native AMD64 process contexts are supported")
        self.page_size = int(system.page_size)
        self.maximum_address = int(system.maximum)
        ntdll = self.checked(k.GetModuleHandleW("ntdll.dll"))
        self.attach_breakpoint_rva = self.checked(k.GetProcAddress(ntdll, b"DbgBreakPoint")) - ntdll

    @staticmethod
    def checked(result):
        if not result:
            raise C.WinError(C.get_last_error())
        return result

    def open_process(self, pid):
        return self.checked(self.k.OpenProcess(hang_evidence.PROCESS_ACCESS, False, pid))

    def close(self, handle):
        if handle:
            self.checked(self.k.CloseHandle(handle))

    def running(self, handle):
        return self.k.WaitForSingleObject(handle, 0) == 0x102

    def identity(self, handle):
        times = [W.FILETIME() for _ in range(4)]
        self.checked(self.k.GetProcessTimes(handle, *[C.byref(item) for item in times]))
        path, size = C.create_unicode_buffer(32768), W.DWORD(32768)
        self.checked(self.k.QueryFullProcessImageNameW(handle, 0, path, C.byref(size)))
        return dict(pid=int(self.k.GetProcessId(handle)),
                    created=(times[0].dwHighDateTime << 32) | times[0].dwLowDateTime,
                    path=path.value)

    def verify_architecture(self, handle):
        wow64 = W.BOOL()
        self.checked(self.k.IsWow64Process(handle, C.byref(wow64)))
        if wow64.value:
            raise OSError("A WOW64 target is unsupported")

    def attach(self, pid):
        self.checked(self.k.DebugActiveProcess(pid))

    def keep_target_on_exit(self):
        self.checked(self.k.DebugSetProcessKillOnExit(False))

    def detach(self, pid):
        self.checked(self.k.DebugActiveProcessStop(pid))

    def wait(self, milliseconds):
        event = DEBUG_EVENT()
        if not self.k.WaitForDebugEvent(C.byref(event), milliseconds):
            error = C.get_last_error()
            if error in (121, 258):
                return None
            raise C.WinError(error)
        result = dict(kind=int(event.kind), pid=int(event.pid), tid=int(event.tid))
        if event.kind == EXCEPTION_DEBUG_EVENT:
            record = event.info.exception.record
            result.update(code=int(record.code), flags=int(record.flags), address=int(record.address or 0),
                          next=int(record.next or 0), first_chance=bool(event.info.exception.first_chance),
                          parameters=[int(record.parameters[i]) for i in range(min(record.count, 15))],
                          record_bytes=C.string_at(C.byref(record), C.sizeof(record)))
        elif event.kind in (CREATE_PROCESS_DEBUG_EVENT, LOAD_DLL_DEBUG_EVENT):
            info = event.info.create_process if event.kind == CREATE_PROCESS_DEBUG_EVENT else event.info.load_dll
            result.update(file=info.file, base=int(info.base or 0))
        elif event.kind == UNLOAD_DLL_DEBUG_EVENT:
            result["base"] = int(event.info.base or 0)
        return result

    def resume(self, event, status):
        self.checked(self.k.ContinueDebugEvent(event["pid"], event["tid"], status))

    def is_attach_breakpoint(self, event, modules):
        return any(module.get("path") and Path(module["path"]).name.casefold() == "ntdll.dll"
                   and event["address"] == module["base"] + self.attach_breakpoint_rva
                   for module in modules.values())

    def module_path(self, handle, event):
        buffer = C.create_unicode_buffer(32768)
        length = 0
        if event.get("file"):
            length = self.k.GetFinalPathNameByHandleW(event["file"], buffer, len(buffer), 0)
        if not 0 < length < len(buffer):
            length = self.psapi.GetModuleFileNameExW(handle, event["base"], buffer, len(buffer))
        if not 0 < length < len(buffer):
            return None
        path = buffer.value
        if path.startswith("\\\\?\\UNC\\"):
            return "\\\\" + path[8:]
        return path[4:] if path.startswith("\\\\?\\") else path

    def read(self, handle, address, size):
        buffer, actual = C.create_string_buffer(size), C.c_size_t()
        self.checked(self.k.ReadProcessMemory(handle, address, buffer, size, C.byref(actual)))
        if actual.value != size:
            raise OSError(f"Short process read: {actual.value}/{size}")
        return buffer.raw

    def query(self, handle, address):
        region = MEMORY_BASIC_INFORMATION()
        self.checked(self.k.VirtualQueryEx(handle, address, C.byref(region), C.sizeof(region)))
        return {key: int(getattr(region, key) or 0) for key in
                ("base", "allocation_base", "allocation_protect", "size", "state", "protect", "type")}

    def context(self, tid):
        handle = self.checked(self.k.OpenThread(0x0008, False, tid))  # THREAD_GET_CONTEXT
        try:
            storage = C.create_string_buffer(CONTEXT_SIZE + 15)
            address = (C.addressof(storage) + 15) & ~15
            C.c_uint32.from_address(address + 48).value = CONTEXT_ALL_X64
            self.checked(self.k.GetThreadContext(handle, address))
            return C.string_at(address, CONTEXT_SIZE)
        finally:
            self.close(handle)


def same_identity(expected, actual):
    return (int(expected["pid"]) == actual["pid"] and int(expected["created"]) == actual["created"]
            and os.path.normcase(os.path.abspath(expected["path"])) == os.path.normcase(os.path.abspath(actual["path"])))


def enumerate_memory(api, handle, deadline):
    regions, address = [], 0
    while address <= api.maximum_address:
        if time.monotonic() >= deadline:
            return regions, False, "capture_deadline"
        if len(regions) >= 200000:
            return regions, False, "region_count_limit"
        region = api.query(handle, address)
        next_address = region["base"] + region["size"]
        if region["size"] <= 0 or next_address <= address:
            raise OSError("VirtualQueryEx made no forward progress")
        regions.append(region)
        address = next_address
    return regions, True, None


def capture_seeds(api, handle, modules, seeds, destination, deadline):
    records, contents = [], []
    for index, seed in enumerate(seeds):
        record = dict(seed=seed, status="failed")
        records.append(record)
        try:
            if time.monotonic() >= deadline:
                raise TimeoutError("capture_deadline")
            expected_path = Path(seed["module"])
            matches = [module for module in modules.values() if module.get("path") and (
                os.path.normcase(os.path.abspath(module["path"])) == os.path.normcase(os.path.abspath(expected_path))
                if expected_path.is_absolute() else Path(module["path"]).name.casefold() == seed["module"].casefold())]
            if len(matches) != 1:
                raise ValueError("Expected exactly one loaded seed module")
            module = matches[0]
            record["module"] = module
            path = Path(module["path"])
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                disk_pe = pe_identity(lambda offset, size: (stream.seek(offset), stream.read(size))[1])
                stream.seek(0)
                while chunk := stream.read(1024 * 1024):
                    if time.monotonic() >= deadline:
                        raise TimeoutError("capture_deadline")
                    digest.update(chunk)
            disk_sha = digest.hexdigest().upper()
            memory_pe = pe_identity(lambda offset, size: api.read(handle, module["base"] + offset, size))
            record.update(disk_sha256=disk_sha, disk_pe=disk_pe, memory_pe=memory_pe)
            expected_pe = {key: seed[key] for key in ("timestamp", "size_of_image")}
            if disk_sha != seed["sha256"] or disk_pe != expected_pe or memory_pe != expected_pe:
                raise ValueError("Seed disk SHA256 or disk/runtime PE identity mismatch")
            rva = seed["rva"] // api.page_size * api.page_size
            address = module["base"] + rva
            size = min(api.page_size, seed["size_of_image"] - rva)
            content = api.read(handle, address, size)
            name = f"module-page-{index:02d}-{address:016x}.bin"
            (destination / name).write_bytes(content)
            record.update(status="captured", address=address, size=size, file=name,
                          sha256=hashlib.sha256(content).hexdigest())
            contents.append(content)
        except Exception as error:
            record["error"] = f"{type(error).__name__}: {error}"
    return records, contents


def capture_allocations(api, handle, regions, seed_contents, destination, deadline,
                        max_bytes=MAX_RAW_BYTES, allocation_bytes=MAX_ALLOCATION_BYTES):
    executable_bases = {r["allocation_base"] for r in regions if r["state"] == MEM_COMMIT
                        and r["type"] != MEM_IMAGE and r["protect"] & 0xFF in EXECUTABLE}
    executable_bases.discard(0)
    groups = {base: [r for r in regions if r["allocation_base"] == base] for base in executable_bases}
    # A seed slot page prioritizes its actual targets over unrelated JIT regions.
    pointers = {value[0] for content in seed_contents for value in struct.iter_unpack("<Q", content[:len(content) // 8 * 8])}
    priority = {base for base, group in groups.items() if any(
        r["base"] <= pointer < r["base"] + r["size"] for r in group for pointer in pointers)}
    records, total = [], 0
    for base in sorted(groups, key=lambda value: (value not in priority, sum(r["size"] for r in groups[value]), value)):
        record = dict(allocation_base=base, prioritized_by_seed=base in priority,
                      regions=groups[base], chunks=[], bytes=0, truncated=False,
                      unreadable_executable_regions=sum(r["state"] == MEM_COMMIT and
                          r["protect"] & 0xFF in EXECUTABLE and
                          (bool(r["protect"] & PAGE_GUARD) or r["protect"] & 0xFF not in READABLE)
                          for r in groups[base]))
        records.append(record)
        for region in groups[base]:
            if region["state"] != MEM_COMMIT or region["protect"] & PAGE_GUARD or region["protect"] & 0xFF not in READABLE:
                continue
            offset = 0
            while offset < region["size"]:
                available = min(max_bytes - total, allocation_bytes - record["bytes"])
                if available <= 0 or time.monotonic() >= deadline:
                    record["truncated"] = True
                    record["reason"] = "capture_deadline" if time.monotonic() >= deadline else "byte_budget"
                    break
                size = min(65536, region["size"] - offset, available)
                address = region["base"] + offset
                chunk = dict(address=address, requested_bytes=size)
                record["chunks"].append(chunk)
                try:
                    content = api.read(handle, address, size)
                    name = f"private-page-{address:016x}-{size:x}.bin"
                    (destination / name).write_bytes(content)
                    chunk.update(status="captured", file=name, bytes=size,
                                 sha256=hashlib.sha256(content).hexdigest())
                    total += size
                    record["bytes"] += size
                except Exception as error:
                    chunk.update(status="failed", error=f"{type(error).__name__}: {error}")
                offset += size
            if record["truncated"]:
                break
    return dict(byte_budget=max_bytes, allocation_byte_budget=allocation_bytes,
                bytes=total, allocations=records,
                coverage="partial" if any(r["truncated"] or r["unreadable_executable_regions"]
                    or any(c["status"] != "captured" for c in r["chunks"]) for r in records) else "selected_readable_regions")


def capture_exception(api, handle, identity, event, modules, seeds, destination, timeout, dump_capture):
    deadline = time.monotonic() + timeout
    result = dict(started=stamp(), status="failed", target_paused=True,
                  dump_exception_stream=False, exception_sidecar="exception.json")
    exception = {key: event[key] for key in ("pid", "tid", "code", "flags", "address", "next", "first_chance", "parameters")}
    exception.update(captured_at=stamp(), source="EXCEPTION_DEBUG_EVENT", thread_context="thread-context-x64.bin")
    write_json(destination / "exception.json", exception)
    (destination / "exception-record-x64.bin").write_bytes(event["record_bytes"])
    try:
        context = api.context(event["tid"])
        (destination / "thread-context-x64.bin").write_bytes(context)
        result["context"] = dict(status="captured", bytes=len(context), architecture="AMD64",
                                 context_flags=struct.unpack_from("<I", context, 48)[0],
                                 rsp=struct.unpack_from("<Q", context, 152)[0],
                                 rip=struct.unpack_from("<Q", context, 248)[0])
    except Exception as error:
        result["context"] = dict(status="failed", error=str(error))
    seed_records, contents = capture_seeds(api, handle, modules, seeds, destination, deadline)
    write_json(destination / "module-pages.json", seed_records)
    regions, complete, reason = enumerate_memory(api, handle, deadline)
    write_json(destination / "virtual-memory.json", dict(complete=complete, reason=reason, regions=regions))
    result["virtual_memory"] = dict(complete=complete, regions=len(regions), reason=reason)
    allocations = capture_allocations(api, handle, regions, contents, destination, deadline)
    write_json(destination / "executable-allocations.json", allocations)
    result["executable_memory"] = {key: allocations[key] for key in ("bytes", "coverage", "byte_budget", "allocation_byte_budget")}
    remaining = deadline - time.monotonic()
    result["dump"] = (dump_capture(identity, destination / "stack-dump", timeout=remaining)
                      if remaining > 0.1 else dict(status="not_run", reason="capture_deadline"))
    result["module_pages"] = [dict(status=r["status"], seed=r["seed"], error=r.get("error")) for r in seed_records]
    result["status"] = ("captured" if result["context"]["status"] == "captured" and complete
                        and allocations["coverage"] != "partial"
                        and result["dump"].get("status") == "captured"
                        and all(r["status"] == "captured" for r in seed_records) else "captured_with_gaps")
    result.update(finished=stamp(), deadline_exceeded=time.monotonic() > deadline)
    return result


def observe(identity, destination, *, stop_file=None, timeout=180, capture_timeout=20,
            seeds=(), api=None, dump_capture=hang_evidence.capture):
    if not 0 < timeout <= 180 or not 0 < capture_timeout <= 20:
        raise ValueError("Observation must be <=180s; capture must be <=20s")
    identity = {key: identity[key] for key in ("pid", "created", "path")}
    identity["pid"], identity["created"] = int(identity["pid"]), int(identity["created"])
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=False)
    record = dict(started=stamp(), process=identity, status="failed", attached=False,
                  detached=False, kill_on_exit=None, initial_breakpoint_handled=False,
                  observation_seconds=timeout, capture_seconds=capture_timeout,
                  observer_context=dict(pid=os.getpid(), executable=sys.executable, cwd=str(Path.cwd()),
                      localappdata_environment=os.environ.get("LOCALAPPDATA"),
                      normal_context_marker=os.environ.get("NR_NEXUS_NORMAL_USER_CONTEXT")),
                  limitations=["Exceptions before attachment can be missed",
                               "Debugger attachment changes timing and exception dispatch",
                               "Private AllocationBase identifies an allocation, not its allocating module",
                               "Normal stack dump has no ExceptionStream; event/context sidecars are authoritative"])
    handle, attached, pending, process_exited = None, False, None, False
    modules, event_counts, exceptions = {}, {}, []
    api = Win32() if api is None else api
    deadline = time.monotonic() + timeout

    def state(value):
        write_json(destination / "state.json", dict(state=value, time=stamp(), process=identity,
                   attached=attached, detached=record["detached"], kill_on_exit=record["kill_on_exit"],
                   initial_breakpoint_handled=record["initial_breakpoint_handled"]))

    try:
        state("validating")
        handle = api.open_process(identity["pid"])
        actual = api.identity(handle)
        record["verified_process"] = actual
        if not api.running(handle) or not same_identity(identity, actual):
            raise IdentityMismatch("PID, creation time, path or running state mismatch before attachment")
        api.verify_architecture(handle)
        if stop_file and Path(stop_file).exists():
            record["status"] = "stopped_without_exception"
            record["stop_reason"] = "stop_file_before_attach"
        else:
            state("attaching")
            api.attach(identity["pid"])
            attached = True
            record.update(attached=True, attached_at=stamp())
            # This must follow a successful debugging connection, on this thread.
            api.keep_target_on_exit()
            record["kill_on_exit"] = False
            state("initializing_debug_events")
            while time.monotonic() < deadline:
                if stop_file and Path(stop_file).exists():
                    record.update(status="stopped_without_exception", stop_reason="stop_file")
                    break
                event = api.wait(100)
                if event is None:
                    continue
                pending = event
                continue_status = DBG_EXCEPTION_NOT_HANDLED if event["kind"] == EXCEPTION_DEBUG_EVENT else DBG_CONTINUE
                event_counts[event["kind"]] = event_counts.get(event["kind"], 0) + 1
                if event["pid"] != identity["pid"]:
                    raise RuntimeError("Unexpected process debug event")
                if event["kind"] in (CREATE_PROCESS_DEBUG_EVENT, LOAD_DLL_DEBUG_EVENT):
                    try:
                        modules[event["base"]] = dict(base=event["base"], path=api.module_path(handle, event))
                    finally:
                        api.close(event.get("file"))
                elif event["kind"] == UNLOAD_DLL_DEBUG_EVENT:
                    modules.pop(event["base"], None)
                elif event["kind"] == EXIT_PROCESS_DEBUG_EVENT:
                    process_exited = True
                    record.update(status="exited_without_capture", stop_reason="process_exit")
                elif event["kind"] == EXCEPTION_DEBUG_EVENT:
                    if len(exceptions) < 64:
                        exceptions.append({key: event[key] for key in ("tid", "code", "address", "first_chance")})
                    initial_breakpoint = (not record["initial_breakpoint_handled"]
                                          and event["code"] == BREAKPOINT and event["first_chance"]
                                          and api.is_attach_breakpoint(event, modules))
                    if initial_breakpoint:
                        continue_status = DBG_CONTINUE
                        record["initial_breakpoint_handled"] = True
                    elif event["code"] == STACK_OVERFLOW or not event["first_chance"]:
                        state("capturing_exception")
                        record["capture"] = capture_exception(api, handle, identity, event, modules,
                                                              seeds, destination, capture_timeout, dump_capture)
                        record["status"] = record["capture"]["status"]
                api.resume(event, continue_status)
                pending = None
                if process_exited or "capture" in record:
                    break
                if event["kind"] == EXCEPTION_DEBUG_EVENT and record["initial_breakpoint_handled"]:
                    state("observing")
            else:
                record.update(status="timed_out_without_exception", stop_reason="observation_deadline")
    except IdentityMismatch as error:
        record.update(status="identity_mismatch", error=str(error))
    except BaseException as error:
        record.update(status="failed", error=f"{type(error).__name__}: {error}")
    finally:
        if pending is not None:
            try:
                status = DBG_EXCEPTION_NOT_HANDLED if pending["kind"] == EXCEPTION_DEBUG_EVENT else DBG_CONTINUE
                api.resume(pending, status)
                pending = None
            except Exception as error:
                record["continue_error"] = str(error)
        if attached:
            try:
                if not process_exited:
                    api.detach(identity["pid"])
                attached = False
                record["detached"] = True
                record["detached_at"] = stamp()
            except Exception as error:
                if handle and not api.running(handle):
                    attached = False
                    record["detached"] = True
                    record["detach_reason"] = "process_already_exited"
                else:
                    record["detach_error"] = str(error)
                    record["status"] = "cleanup_failed"
        else:
            record["detached"] = True
        if handle:
            try:
                api.close(handle)
            except Exception as error:
                record["close_error"] = str(error)
                record["status"] = "cleanup_failed"
        record.update(finished=stamp(), event_counts=event_counts, exceptions=exceptions,
                      loaded_modules=list(modules.values()))
        write_json(destination / "manifest.json", record)
        state("detached" if record["detached"] else "detach_failed")
    return record


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--process", required=True, help="This run's owned-process.json")
    parser.add_argument("--output", required=True, help="A new evidence directory")
    parser.add_argument("--stop-file", help="Stop observing and detach when this file exists")
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--capture-timeout", type=float, default=20)
    parser.add_argument("--module-seeds", help="JSON array of module/sha256/timestamp/size_of_image/rva")
    args = parser.parse_args(argv)
    try:
        identity = json.loads(Path(args.process).read_text(encoding="utf-8-sig"))
        record = observe(identity, args.output, stop_file=args.stop_file, timeout=args.timeout,
                         capture_timeout=args.capture_timeout, seeds=load_seeds(args.module_seeds))
    except Exception as error:
        record = dict(status="failed", error=f"{type(error).__name__}: {error}")
    print(json.dumps(record, ensure_ascii=True))
    return 0 if record.get("detached") and record.get("status") in (
        "captured", "stopped_without_exception", "timed_out_without_exception") else 1


if __name__ == "__main__":
    raise SystemExit(main())
