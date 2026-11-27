"""Bounded, standard-library client for the opt-in NR Nexus development pipe.

Creation times are raw Windows FILETIME ticks, matching game-test owner.created.
The interface reports configuration, API and GPU evidence separately.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
from datetime import datetime, timezone
import json
import math
import os
import struct
import sys
import time
import traceback
import uuid


MAX_BYTES = 64 * 1024
_pending = []  # Keep cancelled OVERLAPPED storage alive until Windows signals completion.


class ProtocolError(RuntimeError):
    pass


class OutcomeUnknown(RuntimeError):
    def __init__(self, request_id, cause):
        super().__init__(f'Mutation outcome unknown ({request_id}); query status before another mutation: {cause}')
        self.request_id = request_id


def _remaining_ms(deadline):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError('Development control deadline expired')
    return max(1, min(0xFFFFFFFE, math.ceil(remaining * 1000)))


class _Overlapped(C.Structure):
    _fields_ = [('Internal', C.c_size_t), ('InternalHigh', C.c_size_t),
                ('Offset', W.DWORD), ('OffsetHigh', W.DWORD), ('hEvent', W.HANDLE)]


def _kernel():
    if os.name != 'nt':
        raise OSError('The native development pipe requires Windows')
    k = C.WinDLL('kernel32', use_last_error=True)
    declarations = {
        'CreateFileW': ([W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE], W.HANDLE),
        'WaitNamedPipeW': ([W.LPCWSTR, W.DWORD], W.BOOL),
        'CreateEventW': ([C.c_void_p, W.BOOL, W.BOOL, W.LPCWSTR], W.HANDLE),
        'CloseHandle': ([W.HANDLE], W.BOOL),
        'WaitForSingleObject': ([W.HANDLE, W.DWORD], W.DWORD),
        'ReadFile': ([W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.POINTER(_Overlapped)], W.BOOL),
        'WriteFile': ([W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.POINTER(_Overlapped)], W.BOOL),
        'GetOverlappedResult': ([W.HANDLE, C.POINTER(_Overlapped), C.POINTER(W.DWORD), W.BOOL], W.BOOL),
        'CancelIoEx': ([W.HANDLE, C.POINTER(_Overlapped)], W.BOOL),
        'GetNamedPipeServerProcessId': ([W.HANDLE, C.POINTER(W.ULONG)], W.BOOL),
        'OpenProcess': ([W.DWORD, W.BOOL, W.DWORD], W.HANDLE),
        'GetProcessTimes': ([W.HANDLE] + [C.POINTER(W.FILETIME)] * 4, W.BOOL),
    }
    for name, (arguments, result) in declarations.items():
        function = getattr(k, name)
        function.argtypes, function.restype = arguments, result
    return k


def process_created(pid):
    """Read raw FILETIME, without changing DPI awareness or importing desktop."""
    k = _kernel()
    handle = k.OpenProcess(0x1000 | 0x100000, False, pid)
    if not handle:
        raise C.WinError(C.get_last_error())
    try:
        values = [W.FILETIME() for _ in range(4)]
        if k.WaitForSingleObject(handle, 0) != 0x102 or not k.GetProcessTimes(
                handle, *(C.byref(value) for value in values)):
            raise ProtocolError('Target process is unavailable')
        return str((values[0].dwHighDateTime << 32) | values[0].dwLowDateTime)
    finally:
        k.CloseHandle(handle)


class NamedPipeTransport:
    def __init__(self, pid):
        self.pid = pid
        self.path = rf'\\.\pipe\nr-nexus-dev-{pid}'

    def exchange(self, payload, deadline):
        """One frame each way; connection, partial reads and writes share deadline."""
        k = _kernel()
        for pending in list(_pending):
            if k.WaitForSingleObject(pending[2].hEvent, 0) == 0:
                k.CloseHandle(pending[2].hEvent)
                k.CloseHandle(pending[1])
                _pending.remove(pending)
        handle = None
        retained = False
        try:
            while True:
                _remaining_ms(deadline)
                # Named-pipe OPEN_EXISTING returns immediately; waiting for busy instances is explicit.
                handle = k.CreateFileW(self.path, 0xC0000000, 0, None, 3, 0x40000000, None)
                if handle != C.c_void_p(-1).value:
                    break
                error = C.get_last_error()
                handle = None
                if error not in (2, 231):  # Missing instance during worker handover, or PIPE_BUSY.
                    raise C.WinError(error)
                if error == 231:
                    if k.WaitNamedPipeW(self.path, _remaining_ms(deadline)):
                        continue
                    error = C.get_last_error()
                    if error == 121:
                        raise TimeoutError('Development pipe connection deadline expired')
                    if error not in (2, 231):
                        raise C.WinError(error)
                # The previous worker may have closed its sole instance before the
                # next creates one. No request bytes have been sent, so retry only
                # this connection under the original deadline.
                _remaining_ms(deadline)
                time.sleep(min(.01, max(0, deadline - time.monotonic())))
            server = W.ULONG()
            if not k.GetNamedPipeServerProcessId(handle, C.byref(server)) or server.value != self.pid:
                raise ProtocolError('Named pipe server PID mismatch')

            def transfer(data, count, writing=False):
                nonlocal retained
                _remaining_ms(deadline)
                buffer = C.create_string_buffer(data, len(data)) if writing else C.create_string_buffer(count)
                event = k.CreateEventW(None, True, False, None)
                if not event:
                    raise C.WinError(C.get_last_error())
                operation = _Overlapped(hEvent=event)
                done = W.DWORD()
                pending_io = False
                try:
                    function = k.WriteFile if writing else k.ReadFile
                    if not function(handle, buffer, count, C.byref(done), C.byref(operation)):
                        error = C.get_last_error()
                        if error != 997:
                            raise C.WinError(error)
                        pending_io = True
                        wait = k.WaitForSingleObject(event, _remaining_ms(deadline))
                        if wait != 0:
                            if wait == 0x102:
                                raise TimeoutError('Development pipe I/O deadline expired')
                            raise C.WinError(C.get_last_error())
                        pending_io = False
                        if not k.GetOverlappedResult(handle, C.byref(operation), C.byref(done), False):
                            raise C.WinError(C.get_last_error())
                    if not done.value:
                        raise ProtocolError('Development pipe closed inside frame')
                    _remaining_ms(deadline)
                    return done.value if writing else buffer.raw[:done.value]
                finally:
                    if pending_io:
                        k.CancelIoEx(handle, C.byref(operation))
                        # Never extend a deadline waiting for cancellation. Retain native storage
                        # and its handle until a subsequent exchange observes event completion.
                        if k.WaitForSingleObject(event, 0) != 0:
                            _pending.append((k, handle, operation, buffer))
                            retained = True
                    if not retained:
                        k.CloseHandle(event)

            framed = struct.pack('<I', len(payload)) + payload
            offset = 0
            while offset < len(framed):
                offset += transfer(framed[offset:], len(framed) - offset, True)

            def receive(count):
                chunks = bytearray()
                while len(chunks) < count:
                    chunks.extend(transfer(None, count - len(chunks)))
                return bytes(chunks)

            size, = struct.unpack('<I', receive(4))
            if not 0 < size <= MAX_BYTES:
                raise ProtocolError('Invalid development response frame length')
            return receive(size)
        finally:
            if handle and not retained:
                k.CloseHandle(handle)


class Client:
    def __init__(self, pid, process_created_value, *, transport=None, identity_probe=None,
                 timeout=3.0, journal=None):
        if type(pid) is not int or pid <= 0:
            raise ValueError('pid must be a positive integer')
        self.pid, self.created = pid, str(process_created_value)
        if not self.created.isdecimal() or int(self.created) <= 0:
            raise ValueError('process_created must be a positive raw FILETIME')
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError('timeout must be positive and finite')
        self.transport = transport if transport is not None else NamedPipeTransport(pid)
        self.identity_probe = identity_probe or process_created
        self.timeout, self.journal = timeout, journal

    def _request(self, request, deadline):
        _remaining_ms(deadline)
        if str(self.identity_probe(self.pid)) != self.created:
            raise ProtocolError('Target process creation time mismatch')
        payload = json.dumps(request, separators=(',', ':'), allow_nan=False).encode('utf-8')
        if len(payload) > MAX_BYTES:
            raise ProtocolError('Development request exceeds 64 KiB')
        if self.journal:
            self.journal('dev_control_request', request=request)
        raw = self.transport.exchange(payload, deadline)
        _remaining_ms(deadline)
        if len(raw) > MAX_BYTES:
            raise ProtocolError('Development response exceeds 64 KiB')
        try:
            response = json.loads(raw.decode('utf-8'), parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
        except (UnicodeError, ValueError) as error:
            raise ProtocolError('Invalid development response JSON') from error
        if not isinstance(response, dict) or response.get('id') != request['id'] or type(response.get('ok')) is not bool:
            raise ProtocolError('Development response envelope mismatch')
        self._validate_state(response)
        if str(self.identity_probe(self.pid)) != self.created:
            raise ProtocolError('Target process creation time changed during request')
        if self.journal:
            self.journal('dev_control_receipt', response=response)
        _remaining_ms(deadline)
        return response

    def _validate_state(self, response):
        code = response.get('code')
        if ('state' not in response and response.get('status') in ('error', 'outcome_unknown')
                and isinstance(code, str) and code.startswith('transport_')):
            return  # Transport failures precede a core snapshot; native identity checks still apply.
        state = response.get('state')
        if (not isinstance(state, dict) or type(state.get('pid')) is not int
                or state['pid'] != self.pid or state.get('process_created') != self.created):
            raise ProtocolError('Development response process identity mismatch')

    def status(self, *, query_id=None, deadline=None):
        deadline = deadline if deadline is not None else time.monotonic() + self.timeout
        while True:
            request = dict(id=str(uuid.uuid4()), op='status')
            if query_id is not None:
                request['query_id'] = query_id
            response = self._request(request, deadline)
            state = response.get('state')
            if not isinstance(state, dict):
                return response
            if (state.get('available') is not False
                    or state.get('reason', response.get('reason')) not in ('runtime_busy', 'configuration_changed')):
                return response
            _remaining_ms(deadline)
            time.sleep(min(.02, max(0, deadline - time.monotonic())))

    def _mutation(self, request, deadline):
        try:
            response = self._request(request, deadline)
            if response.get('status') != 'outcome_unknown':
                return response
            error = ProtocolError(f'Server reported outcome_unknown: {response.get("code", "")}')
        except (OSError, TimeoutError, ProtocolError) as caught:
            error = caught
        # Query only, never retransmit a possibly accepted mutation. Recovery shares
        # the original deadline; an expired deadline immediately yields OutcomeUnknown.
        try:
            observed = self.status(query_id=request['id'], deadline=deadline)
            receipt = observed.get('receipt')
            if isinstance(receipt, dict) and receipt.get('id') == request['id']:
                self._validate_state(receipt)
                if type(receipt.get('ok')) is bool and receipt.get('status') != 'outcome_unknown':
                    return receipt
        except (OSError, TimeoutError, ProtocolError):
            pass
        raise OutcomeUnknown(request['id'], error)

    def set(self, key, value, *, confirm=False, deadline=None):
        if type(confirm) is not bool or not isinstance(key, str) or not key:
            raise ValueError('set requires a key and Boolean confirm')
        deadline = deadline if deadline is not None else time.monotonic() + self.timeout
        request = dict(id=str(uuid.uuid4()), op='set', key=key, value=value)
        response = self._mutation(request, deadline)
        if confirm and response.get('status') == 'confirmation_required':
            identity, changes = response.get('identity'), response.get('changes')
            if not isinstance(identity, dict) or not isinstance(changes, dict):
                raise ProtocolError('Confirmation proposal is missing identity or changes')
            request = dict(request, id=str(uuid.uuid4()), confirm=dict(identity=identity, changes=changes))
            response = self._mutation(request, deadline)
        return response

    def adaptive_feature_experiment(self, mode, *, deadline=None):
        if type(mode) is not int or mode not in (0, 1, 2, 3):
            raise ValueError('adaptive_feature_experiment requires integer mode 0, 1, 2 or 3')
        deadline = deadline if deadline is not None else time.monotonic() + self.timeout
        request = dict(id=str(uuid.uuid4()), op='adaptive_feature_experiment', mode=mode)
        return self._mutation(request, deadline)

    def online_update(self, action='status', *, enabled=None, directory=None, completed=None, deadline=None):
        """Use the production updater without changing NR or image presets."""
        if action not in ('status', 'check', 'download', 'auto_check', 'download_directory',
                          'choose_directory', 'open_directory', 'open_release'):
            raise ValueError('Unknown online update action')
        if (action == 'auto_check' and type(enabled) is not bool) or (action != 'auto_check' and enabled is not None):
            raise ValueError('auto_check requires Boolean enabled')
        if (action == 'download_directory' and (not isinstance(directory, str) or not directory or '\0' in directory)) or (action != 'download_directory' and directory is not None):
            raise ValueError('download_directory requires a filesystem path')
        if completed is not None and (action != 'open_directory' or type(completed) is not bool):
            raise ValueError('completed applies only to open_directory')
        deadline = deadline if deadline is not None else time.monotonic() + self.timeout
        request = dict(id=str(uuid.uuid4()), op='online_update', action=action)
        if enabled is not None:
            request['enabled'] = enabled
        if directory is not None:
            request['directory'] = directory
        if completed is not None:
            request['completed'] = completed
        if action == 'status':
            return self._request(request, deadline)
        return self._mutation(request, deadline)

    def reset_nr(self, *, deadline=None):
        """Request the UI-equivalent NR reset; model and GPU completion require later observations."""
        deadline = deadline if deadline is not None else time.monotonic() + self.timeout
        return self._mutation(dict(id=str(uuid.uuid4()), op='reset_nr'), deadline)

    def bridge_adaptive_experiment(self, enabled, *, deadline=None):
        """Request the DEV Bridge experiment; acceptance does not establish source activation."""
        if type(enabled) is not bool:
            raise ValueError('bridge_adaptive_experiment requires Boolean enabled')
        deadline = deadline if deadline is not None else time.monotonic() + self.timeout
        request = dict(id=str(uuid.uuid4()), op='bridge_adaptive_experiment', enabled=enabled)
        return self._mutation(request, deadline)

    def edit_reuse_adaptive_experiment(self, enabled, *, deadline=None):
        """Request the DEV two-tier x2 experiment; admission is observed separately."""
        if type(enabled) is not bool:
            raise ValueError('edit_reuse_adaptive_experiment requires Boolean enabled')
        deadline = deadline if deadline is not None else time.monotonic() + self.timeout
        request = dict(id=str(uuid.uuid4()), op='edit_reuse_adaptive_experiment', enabled=enabled)
        return self._mutation(request, deadline)

    def inline_restore_roi(self, output_directory, *, tail_frames=None, deadline=None):
        if not isinstance(output_directory, str) or not output_directory.strip():
            raise ValueError('inline_restore_roi requires an output directory')
        deadline = deadline if deadline is not None else time.monotonic() + self.timeout
        request = dict(id=str(uuid.uuid4()), op='inline_restore_roi', output_directory=output_directory)
        if tail_frames is not None:
            if type(tail_frames) is not int or not 2 <= tail_frames <= 48:
                raise ValueError('tail_frames requires an integer from 2 to 48')
            request['tail_frames'] = tail_frames
        return self._mutation(request, deadline)

    def present_handoff_capture(self, action, *, run_directory=None, capture_id=None,
                                start_on_real_guides=False, deadline=None):
        """Arm, observe, or stop a bounded BB pair capture for an owned protected run."""
        deadline = deadline if deadline is not None else time.monotonic() + self.timeout
        request = dict(id=str(uuid.uuid4()), op='present_handoff_capture', action=action)
        if type(start_on_real_guides) is not bool:
            raise ValueError('start_on_real_guides requires a Boolean')
        if action == 'status':
            if run_directory is not None or capture_id is not None or start_on_real_guides:
                raise ValueError('status takes no capture parameters')
            return self._request(request, deadline)
        if action not in ('arm', 'stop') or not isinstance(capture_id, str) or not capture_id:
            raise ValueError('arm and stop require capture_id')
        request['capture_id'] = capture_id
        if action == 'arm':
            if not isinstance(run_directory, str) or not run_directory:
                raise ValueError('arm requires the protected run directory')
            request.update(run_directory=run_directory, start_on_real_guides=start_on_real_guides)
        elif run_directory is not None or start_on_real_guides:
            raise ValueError('stop takes only capture_id')
        return self._mutation(request, deadline)

    def edit_reuse_fg_experiment(self, enabled, *, deadline=None):
        """Request fixed-size Upscaled x2 with FG On; actual execution is observed separately."""
        if type(enabled) is not bool:
            raise ValueError('edit_reuse_fg_experiment requires Boolean enabled')
        deadline = deadline if deadline is not None else time.monotonic() + self.timeout
        request = dict(id=str(uuid.uuid4()), op='edit_reuse_fg_experiment', enabled=enabled)
        return self._mutation(request, deadline)

    def fg_execution_observation(self, action, *, deadline=None):
        """Arm, stop, or read the bounded CPU trace without changing NR settings."""
        if action not in ('arm', 'stop', 'status'):
            raise ValueError('fg_execution_observation requires arm, stop, or status')
        deadline = deadline if deadline is not None else time.monotonic() + self.timeout
        request = dict(id=str(uuid.uuid4()), op='fg_execution_observation', action=action)
        if action == 'status':
            return self._request(request, deadline)
        return self._mutation(request, deadline)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--process-created', required=True, help='Raw decimal Windows FILETIME from owned-process.json')
    parser.add_argument('--timeout', type=float, default=3)
    parser.add_argument('--journal', help='Write request, receipt and error JSONL to a new local file')
    parser.add_argument('--query-id', help='status only: look up an uncertain original request receipt')
    parser.add_argument('--confirm', action='store_true', help='Explicitly approve a returned configuration proposal')
    parser.add_argument('--tail-frames', type=int, help='inline-restore-roi only: capture 2–48 consecutive evaluations at the character ROI')
    parser.add_argument('--capture-id', help='Present handoff capture batch ID, required for arm and stop')
    parser.add_argument('--start-on-real-guides', action='store_true',
                        help='Wait up to 60 seconds for a Present with confirmed real guides')
    parser.add_argument('op', choices=('status', 'set', 'online-update', 'reset-nr', 'inline-restore-roi', 'present-handoff-capture'))
    parser.add_argument('key', nargs='?')
    parser.add_argument('value', nargs='?', help='JSON typed value')
    args = parser.parse_args()
    if args.tail_frames is not None and (args.op != 'inline-restore-roi' or not 2 <= args.tail_frames <= 48):
        parser.error('--tail-frames requires inline-restore-roi and a value from 2 to 48')
    if args.op != 'present-handoff-capture' and (args.capture_id is not None or args.start_on_real_guides):
        parser.error('capture options require present-handoff-capture')
    journal_file = open(args.journal, 'x', encoding='utf-8') if args.journal else None

    def journal(kind, **values):
        journal_file.write(json.dumps(dict(time=datetime.now(timezone.utc).isoformat(), kind=kind, **values),
                                      ensure_ascii=False, allow_nan=False) + '\n')
        journal_file.flush()

    try:
        client = Client(args.pid, args.process_created, timeout=args.timeout,
                        journal=journal if journal_file else None)
        if args.op == 'set':
            if not args.key or args.value is None or args.query_id:
                parser.error('set requires key and JSON value')
            response = client.set(args.key, json.loads(args.value), confirm=args.confirm)
        elif args.op == 'online-update':
            if args.query_id or args.confirm:
                parser.error('online-update accepts an action and optional typed value')
            options = {}
            if args.value is not None:
                field = {'auto_check': 'enabled', 'download_directory': 'directory', 'open_directory': 'completed'}.get(args.key)
                if field is None:
                    parser.error('this online-update action accepts no value')
                options[field] = json.loads(args.value)
            response = client.online_update(args.key or 'status', **options)
        elif args.op == 'inline-restore-roi':
            if not args.key or args.value is not None or args.query_id or args.confirm:
                parser.error('inline-restore-roi requires one output directory and no setting or confirmation')
            response = client.inline_restore_roi(args.key, tail_frames=args.tail_frames)
        elif args.op == 'present-handoff-capture':
            if args.key not in ('arm', 'status', 'stop') or args.query_id or args.confirm:
                parser.error('present-handoff-capture requires arm, status, or stop')
            if args.key == 'arm' and (not args.value or not args.capture_id):
                parser.error('present-handoff-capture arm requires a protected run directory and --capture-id')
            if args.key == 'stop' and not args.capture_id:
                parser.error('present-handoff-capture stop requires --capture-id')
            if args.key != 'arm' and (args.value is not None or args.start_on_real_guides):
                parser.error('run directory and --start-on-real-guides apply to arm only')
            if args.key == 'status' and args.capture_id is not None:
                parser.error('present-handoff-capture status takes no capture options')
            response = client.present_handoff_capture(args.key, run_directory=args.value,
                                                      capture_id=args.capture_id,
                                                      start_on_real_guides=args.start_on_real_guides)
        elif args.op == 'reset-nr':
            if args.key is not None or args.value is not None or args.query_id is not None or args.confirm:
                parser.error('reset-nr accepts no setting, query or confirmation')
            response = client.reset_nr()
        else:
            if args.key or args.value or args.confirm:
                parser.error('status accepts no setting or confirmation')
            response = client.status(query_id=args.query_id)
    except Exception as error:
        if journal_file:
            journal('dev_control_error', pid=args.pid, process_created=args.process_created,
                    error_type=type(error).__name__, error=str(error), traceback=traceback.format_exc())
        raise
    finally:
        if journal_file:
            journal_file.close()
    print(json.dumps(response, ensure_ascii=False, indent=2))
    return 0 if response['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
