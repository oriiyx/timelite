#!/usr/bin/env python3
"""Independent controller journal and oracle. Standard library only; no power control."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import sys
import time


def durable(file):
    file.flush()
    if sys.platform == 'darwin':
        import fcntl
        fcntl.fcntl(file.fileno(), 51)  # F_FULLFSYNC; fail instead of falling back
    else:
        os.fsync(file.fileno())


def event(file, **data):
    file.write(json.dumps(dict(time_ns=time.time_ns(), **data)) + '\n')
    durable(file)


def new_journal(path):
    file = Path(path).open('x', encoding='utf-8')
    durable(file)
    directory = os.open(str(Path(path).resolve().parent), os.O_RDONLY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    return file


def command(line):
    if not re.fullmatch(r'[ACR] [1-9][0-9]{0,6}', line):
        raise ValueError('invalid operation: ' + line)
    op, value = line.split()
    value = int(value)
    if value > 1000000:
        raise ValueError('value exceeds workload bound')
    return op, value


def receive(process, timeout):
    # Read unbuffered bytes: no hidden readline buffer and no unbounded wait.
    result = bytearray()
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        deadline = time.monotonic() + timeout
        while len(result) < 128:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not selector.select(remaining):
                raise TimeoutError('response timeout; operation outcome uncertain')
            char = os.read(process.stdout.fileno(), 1)
            if not char:
                raise EOFError('transport ended; operation outcome uncertain')
            if char == b'\n':
                return result.decode('ascii')
            result += char
    raise ValueError('oversized response')


def collect(plan, journal, transport, timeout):
    lines = Path(plan).read_text().splitlines()
    if not 1 <= len(lines) <= 256:
        raise ValueError('plan must contain 1..256 operations')
    previous = 0
    for line in lines:
        op, value = command(line)
        if op == 'A':
            if value <= previous:
                raise ValueError('append IDs must strictly increase')
            previous = value
    with new_journal(journal) as file:
        event(file, kind='start', transport=transport, plan=lines,
              evidence='transport run; physical power action must be documented separately')
        with Path(str(journal) + '.stderr').open('xb') as stderr:
            process = subprocess.Popen(transport, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=stderr, bufsize=0)
            try:
                if receive(process, timeout) != 'ready':
                    raise ValueError('missing ready response')
                for line in lines:
                    event(file, kind='intent', operation=line)
                    process.stdin.write((line + '\n').encode('ascii'))
                    response = receive(process, timeout)
                    op, value = command(line)
                    match = re.fullmatch(r'ok ([ACR]) ([0-9]+) ([0-9]+)', response)
                    if not match or match[1] != op or int(match[2]) != value:
                        raise ValueError('invalid response: ' + response)
                    sequence = int(match[3])
                    if (op == 'A' and sequence == 0) or (op != 'A' and sequence != 0):
                        raise ValueError('invalid response sequence')
                    event(file, kind='ack', operation=line, sequence=sequence)
                    print(response, flush=True)  # visible only after controller persistence
                process.stdin.close()
                code = process.wait(timeout=timeout)
                event(file, kind='end', exit_code=code)
                return 0 if code == 0 else 1
            except (OSError, ValueError, EOFError, TimeoutError, subprocess.TimeoutExpired) as error:
                event(file, kind='uncertain', detail=str(error))
                return 1
            finally:
                if process.poll() is None:
                    process.terminate()  # transport cleanup is NOT a power cut
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                if not process.stdin.closed:
                    process.stdin.close()
                process.stdout.close()
                durable(stderr)


def apply(state, operation, sequence=None):
    state = copy.deepcopy(state)
    op, value = command(operation)
    if op == 'A':
        if value <= state['floor']:
            raise ValueError('reused or unordered append ID')
        state['high'] += 1
        if sequence is not None and sequence != state['high']:
            raise ValueError('acknowledged sequence mismatch')
        state['floor'] = value
        state['pending'].append((state['high'], value))
    elif op == 'C':
        if state['pending']:
            state['segments'].append(state['pending'])
            state['pending'] = []
    else:
        state['segments'] = [s for s in state['segments'] if max(v for _, v in s) >= value]
    return state


def candidates(journal):
    state = dict(high=0, floor=0, segments=[], pending=[])
    pending = None
    started = False
    for line in Path(journal).read_text().splitlines():
        item = json.loads(line)  # torn/invalid controller evidence fails closed
        kind = item['kind']
        if kind == 'start':
            if started:
                raise ValueError('multiple sessions require separate reconciled trials')
            started = True
        elif kind == 'intent':
            if not started or pending is not None:
                raise ValueError('overlapping or unpaired intents')
            command(item['operation'])
            pending = item['operation']
        elif kind == 'ack':
            if pending != item['operation'] or pending is None:
                raise ValueError('unpaired acknowledgement')
            if pending[0] != 'A' and item['sequence'] != 0:
                raise ValueError('invalid non-append sequence')
            state = apply(state, pending, item['sequence'] if pending[0] == 'A' else None)
            pending = None
        elif kind not in ('end', 'uncertain'):
            raise ValueError('unknown journal event')
    if not started:
        raise ValueError('missing session')
    return [state] if pending is None else [state, apply(state, pending)]


def records(state):
    return [record for segment in state['segments'] for record in segment] + state['pending']


def compare(journal, dump, continuation=None):
    text = Path(dump).read_text()
    if 'open_result=ok\n' not in text or 'read_error=' in text:
        raise ValueError('recovery/dump did not succeed')
    actual = []
    for line in text.splitlines():
        if line.startswith('batch='):
            match = re.fullmatch(r'batch=([0-9]+) series=14 us=([0-9]+) value=([0-9]+)', line)
            if not match or match[2] != match[3]:
                raise ValueError('unexpected record contents')
            actual.append((int(match[1]), int(match[2])))
    floor = re.findall(r'^last_timestamp_us=([0-9]+)$', text, re.M)
    count = re.findall(r'^committed_batches=([0-9]+)$', text, re.M)
    if len(floor) != 1 or len(count) != 1 or int(count[0]) != len(actual):
        raise ValueError('missing or inconsistent dump metadata')
    states = candidates(journal)
    matches = []
    for state in states:
        expected = state
        if continuation is not None:
            expected = apply(state, 'A ' + str(continuation))
        if records(expected) == actual and expected['floor'] == int(floor[0]):
            matches.append(state)
    if not matches:
        raise ValueError('recovered records/floor violate external acknowledgement oracle')
    return dict(status='PASS', acceptable_states=len(matches),
                interrupted_operation=len(states) > 1, records_checked=len(actual),
                sequence_high_water_checked=continuation is not None,
                evidence='record preservation on supplied recovered dump; not power-loss certification')


def preserve(database, wal, destination, inspect, logs=()):
    destination = Path(destination)
    destination.mkdir(exist_ok=False)
    original = destination / 'original'
    working = destination / 'working'
    original.mkdir()
    working.mkdir()
    for index, log in enumerate(logs):
        shutil.copyfile(log, original / ('log-%d' % index))
    hashes = {}
    # Caller must establish that no writer can run. Never open original via API.
    for name, source in [('database', database), ('wal', wal)]:
        path = Path(source)
        if path.is_symlink() or not path.is_file():
            raise ValueError('evidence member must be a regular non-symlink file')
        shutil.copyfile(path, original / name)
        hashes[name] = hashlib.sha256((original / name).read_bytes()).hexdigest()
    (destination / 'hashes.json').write_text(json.dumps(hashes, indent=2) + '\n')
    # Flush preserved bytes and namespace before any recovering open.
    for path in list(original.iterdir()) + [destination / 'hashes.json']:
        with path.open('rb') as file:
            durable(file)
    for path in (original, destination, destination.parent):
        fd = os.open(str(path), os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    outcomes = {}
    with (destination / 'verify.txt').open('x') as output:
        args = [inspect, 'verify', str(original / 'database'), str(original / 'wal')]
        outcomes['verify'] = dict(command=args, exit_code=subprocess.run(args, stdout=output, stderr=subprocess.STDOUT).returncode)
        durable(output)
    # Originals are preserved even when verification fails; never repair them.
    for name in hashes:
        shutil.copyfile(original / name, working / name)
    with (destination / 'status.txt').open('x') as output:
        args = [inspect, 'status', str(working / 'database'), str(working / 'wal'), '--dump']
        outcomes['status'] = dict(command=args, exit_code=subprocess.run(args, stdout=output, stderr=subprocess.STDOUT).returncode)
    (destination / 'inspection.json').write_text(json.dumps(outcomes, indent=2) + '\n')
    return outcomes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    run = sub.add_parser('collect')
    run.add_argument('--plan', required=True)
    run.add_argument('--journal', required=True)
    run.add_argument('--timeout', type=float, default=30)
    run.add_argument('transport', nargs=argparse.REMAINDER)
    check = sub.add_parser('compare')
    check.add_argument('journal')
    check.add_argument('dump')
    check.add_argument('--continuation', type=int)
    save = sub.add_parser('preserve')
    save.add_argument('database')
    save.add_argument('wal')
    save.add_argument('destination')
    save.add_argument('--inspect', required=True)
    save.add_argument('--log', action='append', default=[], help='log to preserve before recovering open')
    save.add_argument('--quiescent', action='store_true', required=True,
                      help='assert workload is stopped and automatic reopening disabled')
    args = parser.parse_args()
    try:
        if args.action == 'collect':
            transport = args.transport
            if transport and transport[0] == '--':
                transport = transport[1:]
            if not transport or not 0 < args.timeout <= 3600:
                raise ValueError('transport and bounded timeout required')
            return collect(args.plan, args.journal, transport, args.timeout)
        if args.action == 'compare':
            print(json.dumps(compare(args.journal, args.dump, args.continuation), indent=2))
            return 0
        outcomes = preserve(args.database, args.wal, args.destination, args.inspect, args.log)
        print(json.dumps(outcomes, indent=2))
        return 0 if all(x['exit_code'] == 0 for x in outcomes.values()) else 1
    except (OSError, ValueError, KeyError) as error:
        print('FAIL: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
