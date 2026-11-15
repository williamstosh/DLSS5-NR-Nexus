"""Protocol mocks and an optional owned-process native pipe interoperability test."""
import ctypes as C
from ctypes import wintypes as W
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import time
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location('dev_control', Path(__file__).resolve().parents[1] / 'tools/dev_control.py')
dev = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dev)


def response(request, **changes):
    return dict(id=request['id'], ok=True, status='observed' if request['op'] == 'status' else 'applied',
                reason='ok', identity=dict(configuration_generation=1, config_epoch=2,
                                          settings_hash='settings', resolution_hash='resolution'),
                changes={}, state=dict(pid=42, process_created='123', settings={'NeuralUplift': False}), **changes)


class Transport:
    def __init__(self, action=None):
        self.requests, self.deadlines = [], []
        self.action = action or (lambda request: response(request))

    def exchange(self, payload, deadline):
        request = json.loads(payload)
        self.requests.append(request)
        self.deadlines.append(deadline)
        result = self.action(request)
        return result if isinstance(result, bytes) else json.dumps(result).encode()


class ClientTests(unittest.TestCase):
    def client(self, action=None, **kw):
        self.transport = Transport(action)
        return dev.Client(42, '123', transport=self.transport, identity_probe=lambda pid: '123', **kw)

    def test_status_and_typed_single_setting_have_distinct_ids_and_bound_deadlines(self):
        client = self.client()
        client.status()
        client.set('NeuralUplift', False)
        self.assertEqual([r['op'] for r in self.transport.requests], ['status', 'set'])
        self.assertIs(self.transport.requests[1]['value'], False)
        self.assertNotEqual(*[r['id'] for r in self.transport.requests])
        self.assertEqual(dev.NamedPipeTransport(42).path, r'\\.\pipe\nr-nexus-dev-42')

    def test_confirmation_is_explicit_and_uses_exact_proposal_with_new_id(self):
        def proposal(request):
            result = response(request)
            if 'confirm' not in request:
                result.update(ok=False, status='confirmation_required', changes={'NRPasses': 2})
            return result
        client = self.client(proposal)
        self.assertEqual(client.set('NRHookPoint', 2)['status'], 'confirmation_required')
        self.assertEqual(len(self.transport.requests), 1)
        client.set('NRHookPoint', 2, confirm=True)
        initial, confirmed = self.transport.requests[-2:]
        self.assertNotEqual(initial['id'], confirmed['id'])
        self.assertEqual(confirmed['confirm'], dict(identity=response(initial)['identity'], changes={'NRPasses': 2}))
        self.assertEqual(self.transport.deadlines[-2], self.transport.deadlines[-1])

    def test_adaptive_feature_experiment_has_a_dedicated_integer_request_and_shared_deadline(self):
        client = self.client()
        deadline = time.monotonic() + 1
        for mode in (0, 1, 2, 3):
            actual = client.adaptive_feature_experiment(mode, deadline=deadline)
            request = self.transport.requests[-1]
            self.assertEqual(request['op'], 'adaptive_feature_experiment')
            self.assertEqual(request['mode'], mode)
            self.assertEqual(actual['id'], request['id'])
            self.assertNotIn('key', request)
            self.assertNotIn('confirm', request)
        self.assertEqual(self.transport.deadlines, [deadline] * 4)
        self.assertEqual(len({request['id'] for request in self.transport.requests}), 4)

    def test_boolean_experiments_have_independent_requests_and_shared_deadlines(self):
        for operation in ('bridge_adaptive_experiment', 'edit_reuse_adaptive_experiment'):
            with self.subTest(operation=operation):
                client = self.client()
                deadline = time.monotonic() + 1
                for enabled in (True, False, True):
                    actual = getattr(client, operation)(enabled, deadline=deadline)
                    request = self.transport.requests[-1]
                    self.assertEqual(request['op'], operation)
                    self.assertIs(request['enabled'], enabled)
                    self.assertEqual(actual['id'], request['id'])
                    self.assertTrue({'mode', 'key', 'value', 'confirm'}.isdisjoint(request))
                self.assertEqual(self.transport.deadlines, [deadline] * 3)
                self.assertEqual(len({request['id'] for request in self.transport.requests}), 3)

    def test_boolean_experiments_reject_non_boolean_before_transport(self):
        for operation in ('bridge_adaptive_experiment', 'edit_reuse_adaptive_experiment'):
            client = self.client()
            for enabled in (0, 1, -1, 1.0, 'true', None):
                with self.subTest(operation=operation, enabled=enabled), self.assertRaises(ValueError):
                    getattr(client, operation)(enabled)
            self.assertEqual(self.transport.requests, [])

    def test_lost_experiment_receipt_queries_without_replaying_or_claiming_activation(self):
        for operation in ('bridge_adaptive_experiment', 'edit_reuse_adaptive_experiment'):
            with self.subTest(operation=operation):
                original = {}
                evidence_key = operation.removesuffix('_experiment') + '_evidence'
                def action(request):
                    if request['op'] == operation:
                        original.update(request)
                        raise OSError('synthetic experiment response disconnect')
                    result = response(request)
                    receipt = response(original)
                    receipt['state']['experiments'] = {operation: True, evidence_key: 'requested_enabled_not_activation'}
                    receipt['identity']['experiments'] = {operation: True}
                    result['receipt'] = receipt
                    return result
                client = self.client(action)
                actual = getattr(client, operation)(True)
                self.assertEqual(actual['id'], original['id'])
                self.assertTrue(actual['state']['experiments'][operation])
                self.assertEqual(actual['state']['experiments'][evidence_key], 'requested_enabled_not_activation')
                self.assertNotIn('active', actual['state']['experiments'])
                self.assertEqual([request['op'] for request in self.transport.requests], [operation, 'status'])
                self.assertEqual(self.transport.requests[1]['query_id'], original['id'])
                self.assertEqual(self.transport.deadlines[0], self.transport.deadlines[1])

    def test_experiment_admission_rejection_is_returned_without_retry(self):
        for operation in ('bridge_adaptive_experiment', 'edit_reuse_adaptive_experiment'):
            for reason in ('configuration_busy', 'runtime_busy', 'nr_running', 'resources_not_released'):
                with self.subTest(operation=operation, reason=reason):
                    client = self.client(lambda request: dict(response(request), ok=False, status='rejected', reason=reason))
                    actual = getattr(client, operation)(False)
                    self.assertFalse(actual['ok'])
                    self.assertEqual(actual['reason'], reason)
                    self.assertEqual(len(self.transport.requests), 1)

    def test_inline_restore_roi_freezes_directory_without_setting_or_confirmation(self):
        client = self.client()
        directory = r'D:\workspace\RenoDX-DLSSNR-Nexus\tmp\mafia-same-frame-roi\capture-42'
        deadline = time.monotonic() + 1
        actual = client.inline_restore_roi(directory, deadline=deadline)
        request = self.transport.requests[0]
        self.assertEqual(request['op'], 'inline_restore_roi')
        self.assertEqual(request['output_directory'], directory)
        self.assertEqual(actual['id'], request['id'])
        self.assertNotIn('tail_frames', request)
        self.assertNotIn('key', request)
        self.assertNotIn('confirm', request)
        self.assertEqual(self.transport.deadlines, [deadline])

    def test_inline_restore_roi_sends_only_a_bounded_tail_count(self):
        client = self.client()
        for invalid_count in (True, 0, 1, 49, 2.5):
            with self.assertRaises(ValueError):
                client.inline_restore_roi(r'D:\capture', tail_frames=invalid_count)
        self.assertEqual(self.transport.requests, [])
        client.inline_restore_roi(r'D:\capture', tail_frames=48)
        self.assertEqual(self.transport.requests[0]['tail_frames'], 48)

    def test_inline_restore_roi_rejects_missing_directory_before_transport(self):
        client = self.client()
        for directory in (None, '', '   ', 42, True):
            with self.subTest(directory=directory), self.assertRaises(ValueError):
                client.inline_restore_roi(directory)
        self.assertEqual(self.transport.requests, [])

    def test_lost_inline_capture_receipt_is_queried_without_arming_twice(self):
        original = {}
        def action(request):
            if request['op'] == 'inline_restore_roi':
                original.update(request)
                raise OSError('synthetic capture response disconnect')
            result = response(request)
            result['receipt'] = dict(response(original), status='armed')
            return result
        client = self.client(action)
        actual = client.inline_restore_roi(r'D:\capture')
        self.assertEqual(actual['status'], 'armed')
        self.assertEqual(actual['id'], original['id'])
        self.assertEqual([r['op'] for r in self.transport.requests], ['inline_restore_roi', 'status'])
        self.assertEqual(self.transport.requests[1]['query_id'], original['id'])
        self.assertEqual(self.transport.deadlines[0], self.transport.deadlines[1])

    def test_adaptive_feature_experiment_rejects_invalid_modes_without_transport(self):
        client = self.client()
        for mode in (True, False, -1, 4, 1.0, '2', None):
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                client.adaptive_feature_experiment(mode)
        self.assertEqual(self.transport.requests, [])

    def test_lost_experiment_receipt_is_queried_without_replaying_the_operation(self):
        original = {}
        def action(request):
            if request['op'] == 'adaptive_feature_experiment':
                original.update(request)
                raise OSError('synthetic experiment response disconnect')
            result = response(request)
            result['state']['experiments'] = dict(adaptive_feature_mode=2, evidence='requested_mode_not_activation')
            result['receipt'] = response(original)
            result['receipt']['state']['experiments'] = result['state']['experiments']
            return result
        client = self.client(action)
        actual = client.adaptive_feature_experiment(2)
        self.assertEqual(actual['id'], original['id'])
        self.assertEqual(actual['state']['experiments']['adaptive_feature_mode'], 2)
        self.assertEqual([request['op'] for request in self.transport.requests], ['adaptive_feature_experiment', 'status'])
        self.assertEqual(self.transport.requests[1]['query_id'], original['id'])
        self.assertEqual(self.transport.deadlines[0], self.transport.deadlines[1])

    def test_transport_failure_queries_receipt_without_replaying_mutation(self):
        original = {}
        def action(request):
            if request['op'] == 'set':
                original.update(request)
                raise OSError('synthetic disconnected after write')
            result = response(request)
            result['receipt'] = response(original)
            return result
        client = self.client(action)
        self.assertEqual(client.set('NeuralUplift', False)['id'], original['id'])
        self.assertEqual([r['op'] for r in self.transport.requests], ['set', 'status'])
        self.assertEqual(self.transport.requests[1]['query_id'], original['id'])

    def test_unresolved_outcome_is_explicit_and_never_retried(self):
        def action(request):
            if request['op'] == 'set':
                raise OSError('synthetic disconnect')
            result = response(request)
            result['receipt'] = None
            return result
        client = self.client(action)
        with self.assertRaises(dev.OutcomeUnknown) as error:
            client.set('NeuralUplift', True)
        self.assertEqual(error.exception.request_id, self.transport.requests[0]['id'])
        self.assertEqual([r['op'] for r in self.transport.requests], ['set', 'status'])

    def test_server_outcome_unknown_queries_without_resending(self):
        def action(request):
            result = response(request)
            result['status'] = 'outcome_unknown' if request['op'] == 'set' else 'observed'
            return result
        client = self.client(action)
        with self.assertRaises(dev.OutcomeUnknown):
            client.set('NRPasses', 2, confirm=True)
        self.assertEqual([r['op'] for r in self.transport.requests], ['set', 'status'])

    def test_stateless_known_transport_error_is_returned_without_identity_misclassification(self):
        def action(request):
            return dict(id=request['id'], ok=False, status='error', code='transport_invalid_frame')
        client = self.client(action)
        client.identity_probe = mock.Mock(return_value='123')
        for actual in (client.status(), client.set('NeuralUplift', False)):
            self.assertEqual(actual['code'], 'transport_invalid_frame')
            self.assertEqual(actual['status'], 'error')
            self.assertNotIn('state', actual)
        self.assertEqual([r['op'] for r in self.transport.requests], ['status', 'set'])
        self.assertEqual(client.identity_probe.call_count, 4)

    def test_stateless_transport_unknown_only_queries_original_receipt(self):
        def action(request):
            if request['op'] == 'set':
                return dict(id=request['id'], ok=False, status='outcome_unknown', code='transport_timeout')
            return dict(response(request), receipt=None)
        client = self.client(action)
        with self.assertRaisesRegex(dev.OutcomeUnknown, 'transport_timeout'):
            client.set('NeuralUplift', True)
        self.assertEqual([r['op'] for r in self.transport.requests], ['set', 'status'])
        self.assertEqual(self.transport.requests[1]['query_id'], self.transport.requests[0]['id'])

    def test_process_pid_creation_and_request_id_are_bound(self):
        for key, value in (('pid', 43), ('pid', True), ('process_created', '124'), ('process_created', 123)):
            def wrong(request):
                result = response(request)
                result['state'][key] = value
                return result
            with self.subTest(key=key, value=value), self.assertRaises(dev.ProtocolError):
                self.client(wrong).status()
        client = self.client(lambda request: dict(response(request), id='wrong'))
        with self.assertRaises(dev.ProtocolError):
            client.status()
        client.identity_probe = lambda pid: '124'
        with self.assertRaisesRegex(dev.ProtocolError, 'creation'):
            client.status()
        self.assertEqual(len(self.transport.requests), 1)

    def test_wire_limits_invalid_json_and_expired_deadline_are_rejected(self):
        for value in (b'{' , b'\xff', b'NaN', b'x' * (dev.MAX_BYTES + 1)):
            with self.subTest(value=value[:8]), self.assertRaises(dev.ProtocolError):
                self.client(lambda request: value).status()
        client = self.client()
        with self.assertRaises(dev.ProtocolError):
            client._request(dict(id='x', op='set', value='a' * dev.MAX_BYTES), time.monotonic() + 1)
        self.assertEqual(self.transport.requests, [])
        with self.assertRaises(TimeoutError):
            client.status(deadline=time.monotonic() - 1)
        self.assertEqual(self.transport.requests, [])

    def test_settings_assignment_is_only_configuration_evidence(self):
        state = self.client().set('NeuralUplift', False)['state']
        self.assertNotIn('gpu_completed', state)
        self.assertNotIn('actual_stage', state)

    def test_busy_status_retries_only_reads_under_one_deadline(self):
        calls = []
        def action(request):
            calls.append(request)
            result = response(request)
            if len(calls) <= 2:
                result['state'].update(available=False,
                    reason='runtime_busy' if len(calls) == 1 else 'configuration_changed')
                del result['state']['settings']
            return result
        client = self.client(action)
        deadline = time.monotonic() + 1
        with mock.patch.object(dev.time, 'sleep'):
            actual = client.status(query_id='original', deadline=deadline)
        self.assertIn('settings', actual['state'])
        self.assertEqual(len(calls), 3)
        self.assertTrue(all(r['op'] == 'status' and r['query_id'] == 'original' for r in calls))
        self.assertEqual(self.transport.deadlines, [deadline] * 3)

    def test_busy_status_remains_bounded(self):
        clock = [1.0]
        def action(request):
            result = response(request)
            result['state'].update(available=False, reason='runtime_busy')
            return result
        client = self.client(action)
        with mock.patch.object(dev.time, 'monotonic', side_effect=lambda: clock[0]), \
                mock.patch.object(dev.time, 'sleep', side_effect=lambda seconds: clock.__setitem__(0, clock[0] + seconds)):
            with self.assertRaises(TimeoutError):
                client.status(deadline=1.05)
        self.assertLessEqual(len(self.transport.requests), 4)


class FakeKernel:
    """Overlapped API mock with deliberately partial stream reads and writes."""
    def __init__(self, reply=b'{}', pending=False, server=42, read_pending=False):
        self.input = bytearray(struct.pack('<I', len(reply)) + reply)
        self.output, self.waits, self.closed = bytearray(), [], []
        self.pending, self.server, self.cancelled = pending, server, False
        self.read_pending = read_pending

    def CreateFileW(self, *args):
        self.open_flags = args[5]
        return 7

    def CreateEventW(self, *args):
        return 8

    def GetNamedPipeServerProcessId(self, handle, pid):
        C.cast(pid, C.POINTER(W.ULONG))[0] = self.server
        return True

    def WriteFile(self, handle, buffer, count, done, operation):
        if self.pending:
            C.set_last_error(997)
            return False
        count = min(count, 3)
        self.output.extend(C.string_at(buffer, count))
        C.cast(done, C.POINTER(W.DWORD))[0] = count
        return True

    def ReadFile(self, handle, buffer, count, done, operation):
        if self.read_pending:
            C.set_last_error(997)
            return False
        count = min(count, 2, len(self.input))
        chunk = bytes(self.input[:count])
        del self.input[:count]
        C.memmove(buffer, chunk, count)
        C.cast(done, C.POINTER(W.DWORD))[0] = count
        return True

    def WaitForSingleObject(self, handle, timeout):
        self.waits.append(timeout)
        return 0x102 if self.pending or self.read_pending else 0

    def CancelIoEx(self, *args):
        self.cancelled = True
        return True

    def CloseHandle(self, handle):
        self.closed.append(handle)


class NativeTransportMockTests(unittest.TestCase):
    def tearDown(self):
        dev._pending.clear()

    def test_partial_stream_framing_and_overlapped_flag(self):
        native = FakeKernel(b'{"ok":true}')
        with mock.patch.object(dev, '_kernel', return_value=native):
            self.assertEqual(dev.NamedPipeTransport(42).exchange(b'{}', time.monotonic() + 1), b'{"ok":true}')
        self.assertEqual(native.output, struct.pack('<I', 2) + b'{}')
        self.assertEqual(native.open_flags, 0x40000000)
        self.assertIn(7, native.closed)

    def test_read_size_cap_and_wrong_server_pid_are_rejected(self):
        native = FakeKernel()
        native.input = bytearray(struct.pack('<I', dev.MAX_BYTES + 1))
        with mock.patch.object(dev, '_kernel', return_value=native), self.assertRaisesRegex(dev.ProtocolError, 'length'):
            dev.NamedPipeTransport(42).exchange(b'{}', time.monotonic() + 1)
        native = FakeKernel(server=43)
        with mock.patch.object(dev, '_kernel', return_value=native), self.assertRaisesRegex(dev.ProtocolError, 'PID'):
            dev.NamedPipeTransport(42).exchange(b'{}', time.monotonic() + 1)
        self.assertEqual(native.output, b'')

    def test_pending_timeout_cancels_and_keeps_native_storage_alive_without_waiting(self):
        for mode in ('read', 'write'):
            dev._pending.clear()
            native = FakeKernel(pending=mode == 'write', read_pending=mode == 'read')
            # Keep the 100ms boundary exact; uptime + .1 can round above .1
            # on Windows' coarse monotonic clock before ceil converts to ms.
            with self.subTest(mode=mode), mock.patch.object(dev, '_kernel', return_value=native), \
                    mock.patch.object(dev.time, 'monotonic', return_value=0.), self.assertRaises(TimeoutError):
                dev.NamedPipeTransport(42).exchange(b'{}', .1)
            self.assertTrue(native.cancelled)
            self.assertTrue(all(0 <= wait <= 100 for wait in native.waits))
            self.assertEqual(len(dev._pending), 1)
            self.assertNotIn(7, native.closed)

    def test_busy_connect_wait_is_bounded_and_never_uses_synchronous_read_write(self):
        native = FakeKernel()
        def busy(*args):
            C.set_last_error(231)
            return C.c_void_p(-1).value
        def wait(path, milliseconds):
            native.waits.append(milliseconds)
            C.set_last_error(121)
            return False
        native.CreateFileW = busy
        native.WaitNamedPipeW = wait
        with mock.patch.object(dev, '_kernel', return_value=native), \
                mock.patch.object(dev.time, 'monotonic', return_value=0.), self.assertRaises(OSError):
            dev.NamedPipeTransport(42).exchange(b'{}', .1)
        self.assertEqual(native.output, b'')
        self.assertTrue(all(0 < wait <= 100 for wait in native.waits))

    def test_missing_instance_handover_connects_and_writes_the_request_once(self):
        native = FakeKernel(b'{"ok":true}')
        original_open = native.CreateFileW
        attempts = []
        def open_after_handover(*args):
            attempts.append(args)
            if len(attempts) <= 2:
                C.set_last_error(2)
                return C.c_void_p(-1).value
            return original_open(*args)
        native.CreateFileW = open_after_handover
        with mock.patch.object(dev, '_kernel', return_value=native), mock.patch.object(dev.time, 'sleep') as pause:
            actual = dev.NamedPipeTransport(42).exchange(b'{"op":"set"}', time.monotonic() + 1)
        self.assertEqual(actual, b'{"ok":true}')
        self.assertEqual(len(attempts), 3)
        self.assertEqual(native.output, struct.pack('<I', 12) + b'{"op":"set"}')
        self.assertEqual(pause.call_count, 2)
        self.assertTrue(all(0 < call.args[0] <= .01 for call in pause.call_args_list))

    def test_missing_endpoint_stops_at_original_deadline_without_sending(self):
        native = FakeKernel()
        clock = [10.0]
        attempts = []
        def missing(*args):
            attempts.append(args)
            C.set_last_error(2)
            return C.c_void_p(-1).value
        native.CreateFileW = missing
        with mock.patch.object(dev, '_kernel', return_value=native), \
                mock.patch.object(dev.time, 'monotonic', side_effect=lambda: clock[0]), \
                mock.patch.object(dev.time, 'sleep', side_effect=lambda seconds: clock.__setitem__(0, clock[0] + seconds)):
            with self.assertRaisesRegex(TimeoutError, 'deadline expired'):
                dev.NamedPipeTransport(42).exchange(b'{"op":"set"}', 10.025)
        self.assertEqual(native.output, b'')
        self.assertEqual(native.closed, [])
        self.assertEqual(len(attempts), 3)
        self.assertEqual(clock[0], 10.025)

    def test_busy_instance_disappearing_during_wait_retries_connection_only(self):
        native = FakeKernel()
        original_open = native.CreateFileW
        attempts = []
        def open_after_wait(*args):
            attempts.append(args)
            if len(attempts) == 1:
                C.set_last_error(231)
                return C.c_void_p(-1).value
            return original_open(*args)
        def disappear(path, milliseconds):
            C.set_last_error(2)
            return False
        native.CreateFileW, native.WaitNamedPipeW = open_after_wait, disappear
        with mock.patch.object(dev, '_kernel', return_value=native), mock.patch.object(dev.time, 'sleep'):
            self.assertEqual(dev.NamedPipeTransport(42).exchange(b'{}', time.monotonic() + 1), b'{}')
        self.assertEqual(native.output, struct.pack('<I', 2) + b'{}')
        self.assertEqual(len(attempts), 2)


@unittest.skipUnless(os.environ.get('NR_NEXUS_PIPE_FIXTURE'), 'Set NR_NEXUS_PIPE_FIXTURE to the bounded C++ CPU fixture')
class NativePipeTest(unittest.TestCase):
    def test_python_client_matches_native_transport(self):
        fixture = Path(os.environ['NR_NEXUS_PIPE_FIXTURE']).resolve(strict=True)
        process = subprocess.Popen([str(fixture), '--client-fixture', '2000'], cwd=fixture.parent,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
        try:
            identity = json.loads(process.stdout.readline())
            self.assertEqual(identity['pid'], process.pid)
            client = dev.Client(process.pid, identity['process_created'])
            self.assertEqual(client.status()['state']['settings']['NeuralUplift'], False)
            enabled = client.set('NeuralUplift', True)
            self.assertTrue(enabled['ok'])
            self.assertTrue(client.status()['state']['settings']['NeuralUplift'])
            self.assertEqual(client.status(query_id=enabled['id'])['receipt'], enabled)
            changed = client.set('NRHookPoint', 1)
            self.assertTrue(changed['ok'])
            self.assertEqual(client.status()['state']['settings']['NRHookPoint'], 1)
            self.assertTrue(client.set('NeuralUplift', False)['ok'])
            self.assertFalse(client.status()['state']['settings']['NeuralUplift'])
            _, errors = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 0, errors)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=5)


if __name__ == '__main__':
    unittest.main()
