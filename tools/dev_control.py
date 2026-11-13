"""Bounded, standard-library client for the opt-in NR Nexus development pipe.

Creation times are raw Windows FILETIME ticks, matching game-test owner.created.
The interface reports configuration, API and GPU evidence separately.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import math
import os
import struct
import sys
import time
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--process-created', required=True, help='Raw decimal Windows FILETIME from owned-process.json')
    parser.add_argument('--timeout', type=float, default=3)
    parser.add_argument('--query-id', help='status only: look up an uncertain original request receipt')
    parser.add_argument('--confirm', action='store_true', help='Explicitly approve a returned configuration proposal')
    parser.add_argument('op', choices=('status', 'set'))
    parser.add_argument('key', nargs='?')
    parser.add_argument('value', nargs='?', help='JSON typed value')
    args = parser.parse_args()
    client = Client(args.pid, args.process_created, timeout=args.timeout)
    if args.op == 'set':
        if not args.key or args.value is None or args.query_id:
            parser.error('set requires key and JSON value')
        response = client.set(args.key, json.loads(args.value), confirm=args.confirm)
    else:
        if args.key or args.value or args.confirm:
            parser.error('status accepts no setting or confirmation')
        response = client.status(query_id=args.query_id)
    print(json.dumps(response, ensure_ascii=False, indent=2))
    return 0 if response['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
