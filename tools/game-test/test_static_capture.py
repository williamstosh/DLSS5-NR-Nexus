"""Pure file/clock and mocked protected-collector checks; no native desktop calls."""
from datetime import datetime, timezone
import ctypes
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock

import static_capture


HERE = Path(__file__).resolve().parent
NOW = datetime(2026, 11, 13, 12, tzinfo=timezone.utc)


class FrozenDateTime(datetime):
    @classmethod
    def now(cls, tz=None):
        return NOW


def write(path, value):
    path.write_text(json.dumps(value), encoding='utf-8')


def evaluate(stage='Upscaled', seq=1, age_ms=0, requested=None, planned=None, result='0x1', actual_pass=1):
    return (f'telemetry stage_plan[valid=1 requested=1:{stage}->{stage} '
            'evidence=current_configuration_not_execution] '
            f'last_nr_evaluate[stage={stage} seq={seq} age_ms={age_ms} actual_pass={actual_pass} '
            f'requested={requested or stage} planned={planned or stage} '
            f'api_returned=1 ngx_result={result} gpu_execution=Unverified]\n')


class StaticCaptureTests(unittest.TestCase):
    def setUp(self):
        native = mock.patch.object(ctypes, 'WinDLL')
        native.start()
        self.addCleanup(native.stop)
        temporary = tempfile.TemporaryDirectory(prefix='static-capture-', dir=static_capture.ROOT / 'tmp')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.run = self.root / 'custom-output' / 'runner-id'
        self.run.mkdir(parents=True)
        install = self.root / 'synthetic-install'
        install.mkdir()
        self.log = install / 'ReShade.log'
        self.log.write_text(evaluate(), encoding='utf-8')
        self.script = self.root / 'collector.py'
        self.script.write_text('# synthetic entry; never executed\n', encoding='utf-8')
        self.window = dict(pid=42, hwnd=7, created=123, path=str(install / 'synthetic-game.exe'))
        self.target = dict(executable=self.window['path'], capture_hold_seconds=10, run_timeout=14,
                           capture_script=str(self.script), capture_script_sha256=static_capture.sha(self.script),
                           static_capture=dict(hold_seconds=10, exit_reserve_seconds=3,
                                               stage_timeout_seconds=2, startup_exit_seconds=4))
        self.save_target()
        write(self.run / 'owned-process.json', self.window)
        self.ready = dict(pid=42, identity=self.window, automatic_foreground=True, time=NOW.isoformat())
        write(self.run / 'capture-ready.json', self.ready)
        self.desktop = types.SimpleNamespace(same_process=mock.Mock(return_value=True),
            require_foreground=mock.Mock(), capture=mock.Mock(side_effect=lambda window, path, **kw: path.write_bytes(b'MOCK')))
        # Use the real receipt helpers with an inert desktop module, without invoking runner.main.
        spec = importlib.util.spec_from_file_location('_static_capture_runner', HERE / 'runner.py')
        self.runner = importlib.util.module_from_spec(spec)
        with mock.patch.dict(sys.modules, desktop=self.desktop, normal_process=types.SimpleNamespace()):
            spec.loader.exec_module(self.runner)
        self.elapsed, self.actions = 0, {}
        self.clock = types.SimpleNamespace(monotonic=lambda: self.elapsed, sleep=self.sleep)

    def save_target(self):
        write(self.run / 'target.json', self.target)
        write(self.run / 'source-manifest.json', dict(target_sha256=static_capture.sha(self.run / 'target.json')))

    def sleep(self, seconds):
        self.elapsed += seconds
        action = self.actions.pop(self.elapsed, None)
        if action:
            action()

    def append(self, line):
        with self.log.open('a', encoding='utf-8') as stream:
            stream.write(line)

    def request(self, **values):
        write(self.run / 'static-control.json', dict(pid=42, **values))

    def complete_request(self):
        self.request(completed=True, overlay_closed=True, nr_off=True)

    def collect(self):
        with mock.patch.dict(sys.modules, desktop=self.desktop, runner=self.runner), \
                mock.patch.object(static_capture, 'time', self.clock), \
                mock.patch.object(static_capture, 'datetime', FrozenDateTime):
            static_capture.capture(self.run, self.script)

    def outcome(self):
        return static_capture.read(self.run / 'static-complete.json')

    def assert_failed(self, text):
        with self.assertRaisesRegex((RuntimeError, ValueError, TimeoutError), text):
            self.collect()
        self.assertFalse((self.run / 'capture-complete.json').exists())
        self.assertTrue((self.run / 'capture-failed.json').exists())

    def test_prepare_copy_derives_both_time_budgets_and_keeps_source(self):
        source = self.root / 'source.json'
        write(source, dict(startup_timeout=120, load_timeout=120, run_timeout=420, existing='unchanged'))
        original = source.read_bytes()
        target = static_capture.prepare_target(source, self.root / 'prepared.json')
        self.assertEqual(source.read_bytes(), original)
        self.assertEqual(target['run_timeout'], 900)
        self.assertEqual(target['capture_hold_seconds'], 600)
        self.assertEqual(target['static_capture'], dict(hold_seconds=600, exit_reserve_seconds=60,
                                                      stage_timeout_seconds=60, startup_exit_seconds=300))
        self.assertEqual(target['existing'], 'unchanged')
        with self.assertRaises(FileExistsError):
            static_capture.prepare_target(source, source)

    def test_hold_only_extension_and_mismatched_prepared_hold_are_rejected(self):
        self.target.update(capture_hold_seconds=600, run_timeout=420, static_capture={})
        with self.assertRaisesRegex(ValueError, 'run_timeout'):
            static_capture.budget(self.target)
        self.target.update(capture_hold_seconds=240, run_timeout=900,
                           static_capture=dict(hold_seconds=600))
        with self.assertRaisesRegex(ValueError, 'differs'):
            static_capture.budget(self.target)

    def test_capture_cli_rejects_budget_flags_that_only_prepare_a_target(self):
        with mock.patch.object(sys, 'argv', ['static_capture.py', str(self.run), '--hold-seconds', '600']), \
                mock.patch.object(sys, 'stderr', new=io.StringIO()), \
                mock.patch.object(static_capture, 'capture') as capture:
            with self.assertRaises(SystemExit) as error:
                static_capture.main()
        self.assertEqual(error.exception.code, 2)
        capture.assert_not_called()

    def test_actual_runner_directory_does_not_need_a_case_runs_prefix(self):
        run, owner, _, remaining = static_capture.context(self.run, self.script, NOW)
        self.assertEqual(run, self.run)
        self.assertEqual(owner, self.window)
        self.assertEqual(remaining, 10)
        renamed = self.root / 'another-output-root'
        self.run.rename(renamed)
        run, _, _, _ = static_capture.context(renamed, self.script, NOW)
        self.assertEqual(run, renamed)
        with self.assertRaisesRegex(ValueError, 'runner output'):
            static_capture.context(static_capture.ROOT, self.script, NOW)

    def test_mismatched_target_script_and_ready_identity_are_rejected(self):
        (self.run / 'target.json').write_text('{}', encoding='utf-8')
        with self.assertRaisesRegex(RuntimeError, 'target hash'):
            static_capture.context(self.run, self.script, NOW)
        self.save_target()
        self.script.write_text('# changed\n', encoding='utf-8')
        with self.assertRaisesRegex(RuntimeError, 'script identity'):
            static_capture.context(self.run, self.script, NOW)
        self.target['capture_script_sha256'] = static_capture.sha(self.script)
        self.save_target()
        for changed in (dict(self.ready, pid=99), dict(self.ready, pid=True),
                        dict(self.ready, automatic_foreground=False),
                        dict(self.ready, identity=dict(self.window, created=999))):
            write(self.run / 'capture-ready.json', changed)
            with self.subTest(changed=changed), self.assertRaisesRegex(RuntimeError, 'identity'):
                static_capture.context(self.run, self.script, NOW)

    def test_missing_owned_pid_cannot_enter_capture(self):
        owner = dict(self.window)
        del owner['pid']
        write(self.run / 'owned-process.json', owner)
        self.assert_failed('identity mismatch')
        self.desktop.same_process.assert_not_called()

    def test_ready_elapsed_time_is_deducted_and_stale_ready_is_rejected(self):
        later = NOW.replace(second=8)
        _, _, _, remaining = static_capture.context(self.run, self.script, later)
        self.assertEqual(remaining, 2)
        with self.assertRaisesRegex(RuntimeError, 'stale'):
            static_capture.context(self.run, self.script, NOW.replace(second=10))

    def test_first_request_after_sixty_seconds_keeps_its_stage_and_cleanup_budgets(self):
        self.target.update(capture_hold_seconds=600, run_timeout=900,
                           static_capture=dict(hold_seconds=600, exit_reserve_seconds=60,
                                               stage_timeout_seconds=60, startup_exit_seconds=300))
        self.save_target()
        self.actions = {61: lambda: self.request(label='render', expected_stage='Render'),
                        61.5: lambda: self.append(evaluate('Render', seq=2)),
                        62: self.complete_request,
                        62.5: lambda: self.append('NR-VERDICT v3 state=OFF stage=- reason=disabled\n')}
        self.collect()
        self.assertEqual(self.outcome()['functionalOutcome'], 'Observed')
        self.assertEqual(self.outcome()['labels'], ['render'])
        self.assertEqual(self.elapsed, 62.5)

    def test_configuration_requested_or_stale_evaluate_does_not_confirm_a_stage(self):
        planned = 'stage_plan[valid=1 requested=1:Render->Render evidence=current_configuration_not_execution]'
        cases = ([planned], [evaluate('Render', seq=1)], [evaluate('Render', seq=2, age_ms=6000)],
                 [evaluate('Render', seq=2, planned='Upscaled')],
                 [evaluate('Render', seq=2, result='0xBAD00002')],
                 [evaluate('Render', seq=2), evaluate('Upscaled', seq=3)])
        for lines in cases:
            with self.subTest(lines=lines):
                self.assertIsNone(static_capture.stage_evidence(lines, 'Render', 1, 1))
        self.assertIsNotNone(static_capture.stage_evidence([evaluate('Render', seq=2)], 'Render', 1, 1))

    def test_new_evaluate_requires_matching_current_plan_for_the_observed_pass(self):
        actual = [evaluate('Render', seq=2)]
        self.assertIsNone(static_capture.stage_evidence(actual, 'Render', 1, 1,
                                                       current_plan=[evaluate('Upscaled', seq=3)]))
        other_pass = [evaluate('Render', seq=2, actual_pass=2)]
        self.assertIsNone(static_capture.stage_evidence(other_pass, 'Render', 1, 1))

    def test_mixed_stage_plan_preserves_both_requested_passes_and_checks_actual_pass(self):
        current = ['stage_plan[valid=1 requested=1:Render->Render,2:Upscaled->Upscaled]']
        plan = {'1': 'Render', '2': 'Upscaled'}
        calls = [evaluate('Render', seq=2), evaluate('Upscaled', seq=3, actual_pass=2)]
        self.assertIsNotNone(static_capture.stage_evidence(calls, 'Render', 1, 1,
                                                          current_plan=current, expected_plan=plan))
        self.assertIsNotNone(static_capture.stage_evidence(calls, 'Upscaled', 1, 1, expected_pass=2,
                                                          current_plan=current, expected_plan=plan))
        self.assertIsNone(static_capture.stage_evidence(calls, 'Render', 1, 1, current_plan=current,
                                                       expected_plan={'1': 'Render', '2': 'Render'}))

    def test_same_telemetry_line_observes_later_upscaled_and_present_blocks(self):
        old = evaluate('Render', seq=7, age_ms=5000).split('last_nr_evaluate', 1)[1].strip()
        for stage in ('Upscaled', 'Present'):
            with self.subTest(stage=stage):
                new = evaluate(stage, seq=20, actual_pass=2).split('last_nr_evaluate', 1)[1].strip()
                line = (f'telemetry stage_plan[valid=1 requested=1:{stage}->{stage},2:{stage}->{stage}] '
                        f'nr_stage[Render evals=7] last_nr_evaluate{old} '
                        f'nr_stage[{stage} evals=20] last_nr_evaluate{new}')
                self.assertEqual(static_capture.sequence([line]), 20)
                self.assertIsNotNone(static_capture.stage_evidence([line], stage, 7, 1, expected_pass=2,
                    expected_plan={'1': stage, '2': stage}))
                self.assertIsNone(static_capture.stage_evidence([line], stage, 7, 1, expected_pass=1))

    def test_partial_old_log_line_is_excluded_from_new_evidence(self):
        baseline = b'old partly written'
        current = baseline + b' NR-VERDICT v3 state=OFF reason=old\nnew complete\npartial new'
        self.assertEqual(static_capture.appended_lines(baseline, current), ['new complete'])
        with self.assertRaisesRegex(RuntimeError, 'log changed'):
            static_capture.appended_lines(b'old', b'new')

    def test_old_sample_cannot_become_fresh_when_the_same_log_is_polled_again(self):
        self.request(label='render', expected_stage='Render')
        # The first observation's age places this call before the request. Later polls must not rehabilitate it.
        self.actions = {.5: lambda: self.append(evaluate('Render', seq=2, age_ms=100)),
                        2.5: self.complete_request,
                        3: lambda: self.append('NR-VERDICT v3 state=OFF stage=- reason=disabled\n')}
        self.collect()
        self.assertEqual(self.outcome()['functionalOutcome'], 'Blocked')
        self.assertEqual(self.outcome()['labels'], [])

    def test_observed_stage_and_fresh_off_finish_early_with_strict_runner_handshake(self):
        self.request(label='render', expected_stage='Render')
        self.actions = {1: lambda: self.append(evaluate('Render', seq=2, age_ms=100)),
                        1.5: self.complete_request,
                        2: lambda: self.append('NR-VERDICT v3 state=OFF stage=- reason=disabled\n')}
        self.collect()
        result = self.outcome()
        self.assertEqual((result['functionalOutcome'], result['labels']), ('Observed', ['render']))
        self.assertLess(self.elapsed, 10)
        self.assertTrue(self.runner.capture_complete(self.run, 42))
        self.assertFalse(self.runner.capture_complete(self.run, 99))
        self.assertFalse(self.runner.capture_script_complete(self.run, 42))
        write(self.run / 'capture-script-result.json', dict(pid=42, exit_code=0))
        self.assertTrue(self.runner.capture_script_complete(self.run, 42))
        self.assertEqual([call.args[1].name for call in self.desktop.capture.call_args_list],
                         ['addon-menu-render.png', 'addon-menu-cleanup.png'])
        self.assertTrue(all(call.kwargs == dict(activate=False) for call in self.desktop.capture.call_args_list))

    def test_no_progress_becomes_blocked_then_safe_cleanup_stays_blocked(self):
        self.request(label='render', expected_stage='Render')
        self.actions = {.5: lambda: self.append('stage_plan[valid=1 requested=1:Render->Render]\n'),
                        2.5: self.complete_request,
                        3: lambda: self.append('NR-VERDICT v3 state=OFF stage=- reason=disabled\n')}
        self.collect()
        self.assertEqual(self.outcome()['functionalOutcome'], 'Blocked')
        self.assertEqual(self.outcome()['labels'], [])
        self.assertEqual(self.elapsed, 3)
        events = static_capture.read(self.run / 'static-events.json')
        self.assertTrue(any(e['kind'] == 'blocked' and e['reason'] == 'actual_stage_not_observed' for e in events))

    def test_explicit_confirmation_still_needs_and_accepts_actual_stage_evidence(self):
        self.request(label='render', expected_stage='Render', confirmation_required=True)
        self.actions = {.5: lambda: self.append(evaluate('Render', seq=2)),
                        1: lambda: self.request(label='render', expected_stage='Render',
                                               confirmation_required=True, confirmation_completed=True),
                        1.5: self.complete_request,
                        2: lambda: self.append('NR-VERDICT v3 state=OFF stage=- reason=disabled\n')}
        self.collect()
        self.assertEqual(self.outcome()['functionalOutcome'], 'Observed')
        self.assertEqual(self.outcome()['labels'], ['render'])

    def test_actual_stage_while_confirmation_pending_is_blocked(self):
        self.request(label='render', expected_stage='Render', confirmation_required=True)
        self.actions = {.5: lambda: self.append(evaluate('Render', seq=2)),
                        2.5: self.complete_request,
                        3: lambda: self.append('NR-VERDICT v3 state=OFF stage=- reason=disabled\n')}
        self.collect()
        self.assertEqual(self.outcome()['functionalOutcome'], 'Blocked')
        events = static_capture.read(self.run / 'static-events.json')
        self.assertTrue(any(e.get('reason') == 'confirmation_pending' for e in events))

    def test_confirmation_click_without_actual_stage_never_confirms(self):
        self.target.update(capture_hold_seconds=600, run_timeout=900,
                           static_capture=dict(hold_seconds=600, exit_reserve_seconds=60,
                                               stage_timeout_seconds=60, startup_exit_seconds=300))
        self.save_target()
        self.request(label='render', expected_stage='Render', confirmation_required=True, confirmation_completed=True)
        self.actions = {.5: lambda: self.append(evaluate('Upscaled', seq=2))}
        self.assert_failed('cleanup was not confirmed')
        self.assertEqual(static_capture.read(self.run / 'capture-failed.json')['functionalOutcome'], 'Blocked')
        self.assertEqual(self.elapsed, 120)

    def test_preexisting_off_or_overlay_request_alone_cannot_release_hold(self):
        self.append('NR-VERDICT v3 state=OFF stage=- reason=disabled\n')
        self.complete_request()
        self.assert_failed('cleanup was not confirmed')

    def test_newer_engaged_after_off_cannot_release_hold(self):
        self.complete_request()
        self.actions = {.5: lambda: self.append('NR-VERDICT v3 state=OFF stage=- reason=disabled\n'
                                                'NR-VERDICT v3 state=ENGAGED stage=- reason=active\n')}
        self.assert_failed('cleanup was not confirmed')

    def test_collection_deadline_starts_bounded_cleanup_instead_of_waiting_forever(self):
        self.target['static_capture']['stage_timeout_seconds'] = 7
        self.save_target()
        self.actions = {7.5: self.complete_request,
                        8: lambda: self.append('NR-VERDICT v3 state=OFF stage=- reason=disabled\n')}
        self.collect()
        self.assertEqual(self.outcome()['functionalOutcome'], 'Blocked')
        self.assertEqual(self.elapsed, 8)

    def test_stage_gap_wait_and_cleanup_stay_sixty_seconds_after_late_handoff(self):
        self.target.update(capture_hold_seconds=600, run_timeout=900,
                           static_capture=dict(hold_seconds=600, exit_reserve_seconds=60,
                                               stage_timeout_seconds=60, startup_exit_seconds=300))
        self.save_target()
        repeat = lambda: self.request(label='render', expected_stage='Render')
        self.actions = {61: repeat,
                        61.5: lambda: self.append(evaluate('Render', seq=2)),
                        62: repeat, 80: repeat, 120: repeat}
        self.assert_failed('cleanup was not confirmed')
        self.assertEqual(static_capture.read(self.run / 'capture-failed.json')['functionalOutcome'], 'Blocked')
        events = static_capture.read(self.run / 'static-events.json')
        blocked = next(e for e in events if e['kind'] == 'blocked')
        self.assertEqual(blocked['reason'], 'waiting_for_stage_request')
        self.assertEqual(self.elapsed, 181.5)

    def test_no_initial_request_is_bound_by_total_collection_and_cleanup_deadlines(self):
        self.target.update(capture_hold_seconds=600, run_timeout=900,
                           static_capture=dict(hold_seconds=600, exit_reserve_seconds=60,
                                               stage_timeout_seconds=60, startup_exit_seconds=300))
        self.save_target()
        states = []
        observe = lambda: states.append(static_capture.read(self.run / 'static-progress.json')['state'])
        self.actions = {60.5: observe, 540.5: observe}
        self.assert_failed('cleanup was not confirmed')
        self.assertEqual(states, ['waiting', 'cleanup'])
        self.assertEqual(self.elapsed, 600)
        events = static_capture.read(self.run / 'static-events.json')
        blocked = next(e for e in events if e['kind'] == 'blocked')
        self.assertEqual(blocked['reason'], 'collection_budget_exhausted')

    def test_cancel_identity_loss_and_focus_loss_keep_completion_absent(self):
        cases = ('cancel', 'identity', 'focus')
        for case in cases:
            with self.subTest(case=case):
                if case == 'cancel':
                    write(self.run / 'capture-cancel.json', dict(reason='mock cancellation'))
                    text = 'cancelled'
                elif case == 'identity':
                    self.desktop.same_process.return_value = False
                    text = 'identity changed'
                else:
                    self.desktop.same_process.return_value = True
                    self.desktop.require_foreground.side_effect = RuntimeError('mock focus lost')
                    text = 'focus lost'
                self.assert_failed(text)
                (self.run / 'capture-failed.json').unlink()
                (self.run / 'capture-cancel.json').unlink(missing_ok=True)
        self.desktop.capture.assert_not_called()

    def test_wrong_control_pid_cannot_request_a_stage_or_complete(self):
        write(self.run / 'static-control.json', dict(pid=99, completed=True, overlay_closed=True, nr_off=True))
        self.assert_failed('control PID')

    def test_log_replacement_cannot_make_old_stage_look_fresh(self):
        self.request(label='render', expected_stage='Render')
        self.actions = {.5: lambda: self.log.write_text(evaluate('Render', seq=99), encoding='utf-8')}
        self.assert_failed('log changed')


if __name__ == '__main__':
    unittest.main()
