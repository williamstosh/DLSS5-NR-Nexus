"""Bounded static evidence collection inside the existing protected runner."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
STAGES = ('Render', 'Upscaled', 'Present')


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def integer(value, name, minimum, maximum):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f'{name} must be an integer from {minimum} to {maximum}')
    return value


def budget(target):
    hold = integer(target['capture_hold_seconds'], 'capture_hold_seconds', 10, 900)
    options = target.get('static_capture', {})
    if options.get('hold_seconds', hold) != hold:
        raise ValueError('CaptureHoldSeconds differs from the prepared static target')
    reserve = integer(options.get('exit_reserve_seconds', 60), 'exit_reserve_seconds', 1, hold - 1)
    stage = integer(options.get('stage_timeout_seconds', 60), 'stage_timeout_seconds', 1, hold - reserve)
    overhead = integer(options.get('startup_exit_seconds',
        target.get('startup_timeout', 120) + target.get('load_timeout', 120) + reserve),
        'startup_exit_seconds', reserve, 3600)
    overall = integer(target['run_timeout'], 'run_timeout', 1, 7200)
    if overall < hold + overhead:
        raise ValueError('run_timeout must cover capture_hold_seconds + startup_exit_seconds')
    return dict(hold_seconds=hold, exit_reserve_seconds=reserve,
                stage_timeout_seconds=stage, startup_exit_seconds=overhead, run_timeout=overall)


def prepare_target(source, destination, hold_seconds=600, exit_reserve_seconds=60,
                   stage_timeout_seconds=60, startup_exit_seconds=None):
    """Write a new local target copy; never modify the user's original target."""
    source = Path(source).resolve(strict=True)
    destination = Path(destination).resolve()
    if not destination.is_relative_to((ROOT / 'tmp').resolve()):
        raise ValueError('Prepared target must remain below repository tmp/')
    target = read(source)
    overhead = (target.get('startup_timeout', 120) + target.get('load_timeout', 120)
                + exit_reserve_seconds if startup_exit_seconds is None else startup_exit_seconds)
    target.update(capture_hold_seconds=hold_seconds, run_timeout=hold_seconds + overhead,
                  static_capture=dict(hold_seconds=hold_seconds, exit_reserve_seconds=exit_reserve_seconds,
                                      stage_timeout_seconds=stage_timeout_seconds, startup_exit_seconds=overhead))
    budget(target)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open('x', encoding='utf-8') as stream:
        json.dump(target, stream, ensure_ascii=False, indent=2)
    return target


def context(run, entry_script, now=None):
    run = Path(run).resolve(strict=True)
    if (not run.is_dir() or not any(run.is_relative_to((ROOT / name).resolve())
                                     for name in ('tmp', 'artifacts'))):
        raise ValueError('Run directory must be the runner output below repository tmp/ or artifacts/')
    target = read(run / 'target.json')
    manifest = read(run / 'source-manifest.json')
    if manifest.get('target_sha256') != sha(run / 'target.json'):
        raise RuntimeError('Runner target hash mismatch')
    script = Path(target['capture_script']).resolve(strict=True)
    if script != Path(entry_script).resolve(strict=True) or sha(script) != target.get('capture_script_sha256'):
        raise RuntimeError('Capture script identity mismatch')
    owner, ready = read(run / 'owned-process.json'), read(run / 'capture-ready.json')
    window = ready['identity']
    keys = ('pid', 'hwnd', 'created', 'path')
    if (ready.get('automatic_foreground') is not True
            or any(type(window.get(key)) is not int or window[key] <= 0 for key in keys[:3])
            or type(ready.get('pid')) is not int or ready['pid'] != window['pid']
            or any(type(owner.get(key)) is not type(window.get(key)) or owner[key] != window[key] for key in keys)
            or Path(window['path']) != Path(target['executable'])):
        raise RuntimeError('Automatic capture identity mismatch')
    settings = budget(target)
    started = datetime.fromisoformat(ready['time'])
    if started.tzinfo is None:
        raise ValueError('Capture-ready time must include its timezone')
    elapsed = ((now or datetime.now(timezone.utc)) - started).total_seconds()
    if elapsed < 0 or elapsed >= settings['hold_seconds']:
        raise RuntimeError('Capture-ready budget is stale or invalid')
    return run, window, settings, settings['hold_seconds'] - elapsed


def evaluate_fields(line):
    return [dict(re.findall(r'(\w+)=([^\s]+)', match[1]))
            for match in re.finditer(r'last_nr_evaluate\[([^\]]+)\]', line)]


def sequence(lines):
    return max((int(fields.get('seq', '0')) for line in lines for fields in evaluate_fields(line)), default=0)


def log_lines(data):
    return data.decode('utf-8-sig', errors='replace').splitlines()


def appended_lines(baseline, current):
    if not current.startswith(baseline):
        raise RuntimeError('ReShade log changed during static observation')
    appended = current[len(baseline):]
    if baseline and not baseline.endswith(b'\n'):
        appended = appended.partition(b'\n')[2]  # Do not count an older, partly written line as new.
    return log_lines(appended[:appended.rfind(b'\n') + 1])


def plan_matches(lines, expected, expected_pass, expected_plan=None):
    for line in reversed(lines):
        match = re.search(r'stage_plan\[([^\]]+)\]', line)
        if match:
            valid = re.search(r'(?:^|\s)valid=1(?:\s|$)', match[1])
            requested = re.search(r'(?:^|\s)requested=([^\s]+)', match[1])
            passes = dict((int(number), (stage, planned)) for number, stage, planned in
                          re.findall(r'(\d+):(Render|Upscaled|Present)->(Render|Upscaled|Present)',
                                     requested[1] if requested else ''))
            return (bool(valid) and passes.get(expected_pass) == (expected, expected)
                    and (expected_plan is None or passes == {int(k): (v, v) for k, v in expected_plan.items()}))
    return False


def stage_evidence(lines, expected, previous_sequence, elapsed_seconds, expected_pass=1,
                   current_plan=None, expected_plan=None):
    """A configuration plan or a fresh log containing an old Evaluate is insufficient."""
    if not plan_matches(lines if current_plan is None else current_plan, expected, expected_pass, expected_plan):
        return None
    candidates = [(int(fields.get('seq', '0')), line, fields) for line in lines for fields in evaluate_fields(line)
                  if fields.get('actual_pass') == str(expected_pass)]
    if not candidates:
        return None
    seq, line, fields = max(candidates, key=lambda value: value[0])
    if (fields.get('stage') == fields.get('requested') == fields.get('planned') == expected
            and fields.get('api_returned') == '1' and fields.get('ngx_result', '').lower() == '0x1'
            and seq > previous_sequence
            and 0 <= int(fields.get('age_ms', '-1')) <= elapsed_seconds * 1000):
        return line
    return None


def capture(run, entry_script=None):
    # Importing desktop changes DPI awareness. Keep pure file/contract imports inert.
    run = Path(run).resolve(strict=True)
    if not any(run.is_relative_to((ROOT / name).resolve()) for name in ('tmp', 'artifacts')):
        raise ValueError('Run directory must remain below repository tmp/ or artifacts/')
    import desktop
    from runner import dump, stamp

    events, labels = [], set()
    outcome, state, active, cleanup = 'NotTested', 'waiting', None, None
    try:
        run, window, settings, remaining = context(run, entry_script or Path(sys.argv[0]))
        outer_deadline = time.monotonic() + remaining
        collection_deadline = outer_deadline - settings['exit_reserve_seconds']
        waiting_deadline = collection_deadline
        log = Path(window['path']).parent / 'ReShade.log'

        def record(kind, **values):
            events.append(dict(time=stamp(), kind=kind, **values))
            dump(run / 'static-events.json', events)

        def begin_cleanup(reason, current):
            nonlocal state, cleanup, active
            state, active = 'cleanup', None
            cleanup = dict(deadline=min(outer_deadline, time.monotonic() + settings['exit_reserve_seconds']),
                           baseline=current, overlay_closed=False)
            record('cleanup_started', reason=reason, functionalOutcome=outcome)

        while True:
            if (run / 'capture-cancel.json').exists():
                raise RuntimeError('Capture cancelled by protected runner')
            if (run / 'capture-failed.json').exists():
                raise RuntimeError('Protected capture is already failing')
            if not desktop.same_process(window):
                raise RuntimeError('Owned process identity changed')
            now = time.monotonic()
            if now >= outer_deadline or (cleanup and now >= cleanup['deadline']):
                raise TimeoutError('Static capture cleanup was not confirmed before its deadline')
            control = run / 'static-control.json'
            request = read(control) if control.exists() else {}
            if request and (type(request.get('pid')) is not int or request['pid'] != window['pid']):
                raise RuntimeError('Static control PID mismatch')
            if request.get('handoff') is not True:
                desktop.require_foreground(window, 'static capture')
            current = log.read_bytes()
            current_lines = log_lines(current[:current.rfind(b'\n') + 1])

            if not cleanup and now >= collection_deadline:
                outcome = 'Blocked'
                record('blocked', reason='collection_budget_exhausted', functionalOutcome=outcome)
                begin_cleanup('collection_budget_exhausted', current)
            if not cleanup and not active and now >= waiting_deadline:
                outcome = 'Blocked'
                record('blocked', reason='waiting_for_stage_request', functionalOutcome=outcome)
                begin_cleanup('waiting_for_stage_request', current)
            if not cleanup and active:
                fresh = appended_lines(active['cursor'], current)
                if (request.get('label') != active['label']
                        or request.get('expected_stage') != active['expected_stage']
                        or request.get('expected_pass', 1) != active['expected_pass']
                        or request.get('expected_plan') != active['expected_plan']):
                    if request.get('completed') is not True:
                        raise ValueError('Await the active stage result before replacing its request')
                confirmed = (not active['confirmation_required']
                             or (request.get('label') == active['label']
                                 and request.get('confirmation_completed') is True))
                evidence = stage_evidence(fresh, active['expected_stage'], active['sequence'],
                    active['previous_read'] - active['started'], active['expected_pass'], current_lines,
                    active['expected_plan'])
                if evidence:
                    active['evidence'] = evidence
                elif any(fields.get('actual_pass') == str(active['expected_pass'])
                         for line in fresh for fields in evaluate_fields(line)):
                    active['evidence'] = None
                active.update(cursor=current, previous_read=now)
                if (confirmed and active['evidence']
                        and plan_matches(current_lines, active['expected_stage'], active['expected_pass'],
                                         active['expected_plan'])):
                    desktop.capture(window, run / ('addon-menu-' + active['label'] + '.png'), activate=False)
                    record('stage_observed', label=active['label'], expected_stage=active['expected_stage'],
                           expected_pass=active['expected_pass'], actual_evaluate=active['evidence'],
                           expected_plan=active['expected_plan'],
                           confirmation_completed=confirmed,
                           evidence_level='API_RETURNED_NOT_GPU_OR_VISUAL')
                    labels.add(active['label'])
                    outcome, state, active = 'Observed', 'waiting', None
                    waiting_deadline = min(collection_deadline, now + settings['stage_timeout_seconds'])
                elif now >= active['deadline']:
                    outcome = 'Blocked'
                    record('blocked', label=active['label'], expected_stage=active['expected_stage'],
                           reason='confirmation_pending' if not confirmed else 'actual_stage_not_observed',
                           functionalOutcome=outcome)
                    begin_cleanup('stage_timeout', current)

            if request.get('completed') is True:
                if not cleanup:
                    if active:
                        outcome = 'Blocked'
                        record('blocked', reason='stage_incomplete_at_completion', functionalOutcome=outcome)
                    begin_cleanup('completion_requested', current)
            if cleanup:
                # This is the main agent's observation of the actual frame, never a HOME-key assumption.
                cleanup['overlay_closed'] = (request.get('completed') is True
                                             and request.get('overlay_closed') is True
                                             and request.get('nr_off') is True)
                verdict = next((line for line in reversed(appended_lines(cleanup['baseline'], current))
                                if 'NR-VERDICT v3 state=' in line), None)
                if cleanup['overlay_closed'] and verdict and 'NR-VERDICT v3 state=OFF ' in verdict:
                    desktop.capture(window, run / 'addon-menu-cleanup.png', activate=False)
                    dump(run / 'static-complete.json', dict(time=stamp(), pid=window['pid'],
                        labels=sorted(labels), functionalOutcome=outcome, nr_verdict=verdict,
                        overlay_closed='main_agent_visual_confirmation', cleanup_capture='addon-menu-cleanup.png',
                        visual_quality='not_tested_by_collector', budget=settings))
                    dump(run / 'capture-complete.json', dict(pid=window['pid'], completed=True))
                    return
            elif not active and request.get('label') and request['label'] not in labels:
                label, expected = request['label'], request.get('expected_stage')
                if not isinstance(label, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]{0,63}', label):
                    raise ValueError('Invalid static evidence label')
                if expected not in STAGES or type(request.get('confirmation_required', False)) is not bool:
                    raise ValueError('Stage request requires expected_stage and a Boolean confirmation_required')
                expected_pass = integer(request.get('expected_pass', 1), 'expected_pass', 1, 4)
                expected_plan = request.get('expected_plan')
                if expected_plan is not None and (not isinstance(expected_plan, dict) or not expected_plan
                        or any(k not in ('1', '2', '3', '4') or v not in STAGES for k, v in expected_plan.items())
                        or expected_plan.get(str(expected_pass)) != expected):
                    raise ValueError('expected_plan must include the observed pass/stage and valid pass entries')
                active = dict(label=label, expected_stage=expected, expected_pass=expected_pass,
                              expected_plan=expected_plan,
                              cursor=current, sequence=sequence(current_lines), previous_read=now, evidence=None,
                              started=now, deadline=min(collection_deadline, now + settings['stage_timeout_seconds']),
                              confirmation_required=request.get('confirmation_required', False))
                state = 'awaiting_stage'
                record('stage_requested', label=label, expected_stage=expected,
                       expected_pass=expected_pass,
                       expected_plan=expected_plan,
                       confirmation_required=active['confirmation_required'])

            dump(run / 'static-progress.json', dict(time=stamp(), pid=window['pid'], state=state,
                functionalOutcome=outcome, labels=sorted(labels),
                pending_label=active['label'] if active else None,
                seconds_remaining=round((cleanup['deadline'] if cleanup else active['deadline'] if active
                                         else waiting_deadline) - time.monotonic(), 1),
                capture_seconds_remaining=round(outer_deadline - time.monotonic(), 1),
                budget=settings))
            time.sleep(.5)
    except Exception as error:
        dump(run / 'capture-failed.json', dict(time=stamp(), stage='static_capture', error=repr(error),
            functionalOutcome=outcome, no_game_input_sent_by_collector=True))
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', nargs='?', type=Path)
    parser.add_argument('--prepare-target', type=Path)
    parser.add_argument('--output-target', type=Path)
    parser.add_argument('--hold-seconds', type=int, default=600, help='Target preparation only')
    parser.add_argument('--exit-reserve-seconds', type=int, default=60, help='Target preparation only')
    parser.add_argument('--stage-timeout-seconds', type=int, default=60, help='Target preparation only')
    parser.add_argument('--startup-exit-seconds', type=int, help='Target preparation only')
    args = parser.parse_args()
    if args.prepare_target:
        if args.run or not args.output_target:
            parser.error('--prepare-target requires --output-target and no run directory')
        target = prepare_target(args.prepare_target, args.output_target, args.hold_seconds,
            args.exit_reserve_seconds, args.stage_timeout_seconds, args.startup_exit_seconds)
        print(json.dumps(budget(target)))
    elif args.run and not args.output_target:
        preparation_options = {'--hold-seconds', '--exit-reserve-seconds',
                               '--stage-timeout-seconds', '--startup-exit-seconds'}
        if preparation_options.intersection(arg.split('=')[0] for arg in sys.argv[1:]):
            parser.error('Prepare the target copy first; capture uses only its recorded budgets')
        capture(args.run)
    else:
        parser.error('Specify a runner directory or --prepare-target with --output-target')


if __name__ == '__main__':
    main()
