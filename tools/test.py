#!/usr/bin/env python3
"""Timelite test runner: builds and runs the C test inventory in isolated
configurations and writes machine-readable reports.

Python 3 standard library only. Python is test tooling, not a dependency of
the Timelite library. Every command is a subprocess argument array; no shell.

Result statuses:
  PASS     the command ran and met its contract
  FAIL     the command ran and did not meet its contract (or timed out)
  SKIP     deliberately not run; optional skips are fine, required ones fail
  BLOCKED  could not run: missing tool, image, storage capability or host
  NOT RUN  never started, for example after cancellation

Test exit-code conventions (see docs/feature/005-testing-suite.md):
  0   pass
  77  durable provisioning unsupported on this storage (contract on Windows)
  78  a required test group did not run (for example >4 GiB sparse files)
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import platform
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import uuid
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STRICT = ['-std=c99', '-Wall', '-Wextra', '-Wpedantic', '-Werror']
SANITIZE = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
SNAPSHOT_LIMIT = 16 * 1024 * 1024
EXIT_UNSUPPORTED = 77
EXIT_GROUP_SKIPPED = 78
CONTAINER_PLATFORM = 'linux/amd64'

# storage: 'required' means unsupported durable provisioning blocks the
# profile; 'optional' records an explicit skip (container filesystems are not
# native durability coverage); 'contract' means ENOTSUP is the expected result.
PROFILES = {
    'native': dict(kind='native', platform='posix', flags=[], storage='required',
                   summary='host compiler, default flags'),
    'sanitize': dict(kind='native', platform='posix', flags=SANITIZE, storage='required',
                     summary='host compiler with AddressSanitizer and UndefinedBehaviorSanitizer'),
    'linux': dict(kind='container', platform='posix', flags=[], storage='optional',
                  cc=['gcc'], ar=['ar'], target='linux/x86_64',
                  summary='Debian gcc, x86-64, in the prepared Docker image'),
    'linux32': dict(kind='container', platform='posix', flags=['-m32'], storage='optional',
                    cc=['gcc'], ar=['ar'], target='linux/i386',
                    summary='Debian gcc -m32, x86 32-bit with 64-bit offsets, in the prepared image'),
    'windows-cross': dict(kind='container', platform='windows', flags=[], storage='contract',
                          cc=['x86_64-w64-mingw32-gcc'], ar=['x86_64-w64-mingw32-ar'],
                          target='windows/x86_64', execute=False,
                          summary='MinGW-w64 cross-compilation only; nothing is executed'),
    'windows-native': dict(kind='native', platform='windows', storage='contract',
                           flags=['--target=x86_64-pc-windows-msvc'],
                           cc=['clang'], ar=['llvm-ar'],
                           summary='Windows host, Clang with the Windows SDK, tests executed'),
    'runner': dict(kind='selftest', summary='unit tests of this runner (tools/test_runner.py)'),
}
GROUPS = {
    'local': ['native', 'sanitize', 'runner'],
    'preflight': ['native', 'sanitize', 'runner', 'linux', 'linux32', 'windows-cross'],
}


# --- source snapshot -------------------------------------------------------

def source_files(root):
    """Allowlisted source inputs as {relative posix path: bytes}."""
    root = Path(root)
    names = ['timelite.c', 'timelite.h', 'file_io.c', 'file_io_windows.c', 'file_io.h',
             'Makefile', 'README.md', 'AGENTS.md', 'CLAUDE.md', 'LICENSE', '.gitignore']
    directories = [('tests', {'.c', '.h'}), ('examples', {'.c'}),
                   ('tools', {'.py', '.json', '.md', '.txt', ''}),
                   ('docs/feature', {'.md'}), ('.github/workflows', {'.yml', '.yaml'})]
    paths = [root / n for n in names if (root / n).exists()]
    for directory, suffixes in directories:
        folder = root / directory
        if folder.is_dir():
            paths += [p for p in folder.rglob('*') if p.is_file() and p.suffix in suffixes]
    result = {}
    total = 0
    for path in sorted(set(paths)):
        relative = path.relative_to(root)
        if any((root / parent).is_symlink() for parent in [relative] + list(relative.parents)[:-1]):
            raise ValueError('symlinked source is not allowed: ' + str(path))
        data = path.read_bytes()
        total += len(data)
        if total > SNAPSHOT_LIMIT:
            raise ValueError('source snapshot exceeds 16 MiB')
        result[relative.as_posix()] = data
    return result


def digest(files):
    h = hashlib.sha256()
    for name, data in sorted(files.items()):
        h.update(name.encode() + b'\0' + str(len(data)).encode() + b'\0' + data)
    return h.hexdigest()


def snapshot(root, destination):
    """Copy the allowlisted sources into destination; return their digest."""
    files = source_files(root)
    for name, data in files.items():
        path = Path(destination) / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return digest(files), len(files)


def git_info(root):
    result = {}
    for key, args in [('commit', ['rev-parse', 'HEAD']), ('dirty', ['status', '--porcelain'])]:
        try:
            p = subprocess.run(['git'] + args, cwd=root, capture_output=True, text=True, timeout=10)
            result[key] = p.stdout.strip() if p.returncode == 0 else None
        except (OSError, subprocess.TimeoutExpired):
            result[key] = None
    return result


# --- results ---------------------------------------------------------------

def row(name, status, reason, **extra):
    r = dict(name=name, status=status, reason=reason, required=True, coverage='', commands=[])
    r.update(extra)
    return r


def ok(r):
    return r['status'] == 'PASS' or (r['status'] == 'SKIP' and not r.get('required', True))


def successful(rows):
    """At least one PASS and nothing failing; skips alone never pass."""
    return any(r['status'] == 'PASS' for r in rows) and all(ok(r) for r in rows)


def words(value):
    return shlex.split(value)


def load_inventory(source):
    return json.loads((Path(source) / 'tools/inventory.json').read_text(encoding='utf-8'))


# --- subprocess ownership --------------------------------------------------

class Commands:
    """Runs argument arrays with a timeout, logs, and cooperative cancellation.
    Every child is owned: cancellation or timeout kills its process group."""

    def __init__(self):
        self.cancel = threading.Event()
        self.lock = threading.Lock()
        self.children = set()

    def stop(self, process):
        try:
            if os.name == 'nt':
                subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
            else:
                os.killpg(process.pid, signal.SIGKILL)
        except (ProcessLookupError, OSError, subprocess.TimeoutExpired):
            pass
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass

    def run(self, command, cwd, log, timeout=120, env=None):
        command = [str(x) for x in command]
        log = Path(log)
        started = time.monotonic()
        result = dict(command=command, cwd=str(cwd), log=str(log), exit_code=None,
                      status='NOT RUN', reason='cancelled before launch', seconds=0.0)
        if self.cancel.is_set():
            return result
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open('w', encoding='utf-8', errors='replace') as output:
            output.write('$ ' + ' '.join(shlex.quote(x) for x in command) + '\n')
            output.flush()
            try:
                process = subprocess.Popen(command, cwd=str(cwd), env=env, stdout=output,
                                           stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                           start_new_session=(os.name != 'nt'))
            except OSError as error:
                result.update(status='BLOCKED', reason='cannot start: ' + str(error))
            else:
                with self.lock:
                    self.children.add(process)
                try:
                    while process.poll() is None:
                        if self.cancel.wait(0.05):
                            self.stop(process)
                            result.update(status='FAIL', reason='cancelled')
                            break
                        if time.monotonic() - started > timeout:
                            self.stop(process)
                            result.update(status='FAIL', reason='timeout after %gs' % timeout)
                            break
                    else:
                        code = process.returncode
                        result.update(status='PASS' if code == 0 else 'FAIL',
                                      reason='exit 0' if code == 0 else 'exit %d' % code)
                    result['exit_code'] = process.returncode
                finally:
                    with self.lock:
                        self.children.discard(process)
        result['seconds'] = round(time.monotonic() - started, 3)
        return result

    def stop_all(self):
        self.cancel.set()
        with self.lock:
            children = list(self.children)
        for process in children:
            self.stop(process)


def log_output(result):
    """Command output without the first line, which echoes the command."""
    try:
        text = Path(result['log']).read_text(encoding='utf-8', errors='replace')
    except OSError:
        return ''
    return text.split('\n', 1)[1] if '\n' in text else ''


# --- native build and execution -------------------------------------------

def configuration(name):
    """Compiler, archiver and flags for one profile. CC/AR/CPPFLAGS/CFLAGS/
    LDFLAGS/LDLIBS override host profiles; container toolchains are fixed."""
    profile = PROFILES[name]
    windows = profile['platform'] == 'windows'
    if profile['kind'] == 'container':
        cc, ar = list(profile['cc']), list(profile['ar'])
    else:
        cc = words(os.environ.get('CC', '')) or list(profile.get('cc', ['cc']))
        ar = words(os.environ.get('AR', '')) or list(profile.get('ar', ['ar']))
    flags = words(os.environ.get('CPPFLAGS', '')) + ['-I.'] + STRICT
    flags += words(os.environ.get('CFLAGS', '-O0 -g'))
    flags += ['-UNDEBUG'] + list(profile['flags'])
    return dict(profile=name, cc=cc, ar=ar, flags=flags,
                link=words(os.environ.get('LDFLAGS', '')), libs=words(os.environ.get('LDLIBS', '')),
                platform=profile['platform'], backend='file_io_windows.c' if windows else 'file_io.c',
                exe='.exe' if windows else '', execute=profile.get('execute', True),
                storage=profile['storage'])


def host_matches(config):
    return (config['platform'] == 'windows') == (os.name == 'nt')


def native(name, source, out, selected, jobs, timeout, test_root, commands):
    """Build and run the inventory for one configuration. Returns (rows, metadata)."""
    source, out = Path(source), Path(out)
    out.mkdir(parents=True, exist_ok=True)
    config = configuration(name)
    meta = dict(configuration=config, host=dict(os=platform.platform(), machine=platform.machine(),
                                                python=platform.python_version()))
    inventory = load_inventory(source)
    if selected:
        inventory = [t for t in inventory if t['name'] == selected]
        if not inventory:
            return [row('selection', 'FAIL', 'no inventory test named ' + repr(selected))], meta
    if config['execute'] and not host_matches(config):
        return [row(t['name'], 'BLOCKED', 'profile needs a %s host' % config['platform'])
                for t in inventory], meta
    applicable = [t for t in inventory if config['platform'] in t['platforms']]
    rows = [row(t['name'], 'SKIP', 'not applicable to ' + config['platform'], required=bool(selected),
                coverage=t['coverage']) for t in inventory if t not in applicable]
    if not applicable:
        return rows, meta

    def call(args, log_name, env=None, cwd=None):
        return commands.run(args, cwd or source, out / (log_name + '.log'), timeout, env)

    version = call(config['cc'] + ['--version'], 'compiler-version')
    machine = call(config['cc'] + ['-dumpmachine'], 'compiler-target')
    # -dumpmachine ignores -m32, so the real target ABI comes from predefined macros.
    macros = call(config['cc'] + config['flags'] + ['-dM', '-E', '-x', 'c', '-'], 'compiler-macros')
    meta['compiler'] = dict(version=log_output(version).strip(), dumpmachine=log_output(machine).strip(),
                            target=target_abi(log_output(macros)), commands=[version, machine, macros])
    if version['status'] != 'PASS':
        return rows + [row(t['name'], 'BLOCKED', 'compiler unavailable: ' + ' '.join(config['cc']),
                           coverage=t['coverage'], commands=[version]) for t in applicable], meta

    # Shared prerequisite: the public static library, built once per configuration.
    prerequisites = []
    library = out / 'libtimelite.a'
    if any(t['backend'] == 'library' for t in applicable):
        objects = []
        for src in ['timelite.c', config['backend']]:
            obj = out / (src + '.o')
            objects.append(obj)
            prerequisites.append(call(config['cc'] + config['flags'] + ['-c', src, '-o', obj], 'compile-' + src))
        if successful(prerequisites):
            prerequisites.append(call(config['ar'] + ['rcs', library] + objects, 'archive'))
    meta['prerequisites'] = prerequisites

    # Storage root: an explicit supported filesystem, or a private run directory.
    if test_root:
        root = Path(test_root).resolve()
        if not root.is_dir():
            return rows + [row(t['name'], 'BLOCKED', 'test root is not a directory: ' + str(root),
                               coverage=t['coverage']) for t in applicable], meta
        storage_root = Path(tempfile.mkdtemp(prefix='timelite-run-', dir=root))
    else:
        storage_root = out / 'storage'
        storage_root.mkdir(exist_ok=True)
    meta['storage_root'] = str(storage_root)

    def sources_for(t):
        if t['backend'] == 'library':
            return [t['source'], library]
        if t['backend'] == 'fake':
            return [t['source'], 'timelite.c']
        if t['backend'] == 'native':
            return [t['source'], config['backend']]
        return [t['source']]

    def build(t, r):
        if t['backend'] == 'library' and not successful(prerequisites):
            r.update(status='BLOCKED', reason='static library prerequisite failed; see compile/archive logs')
            return None
        binary = out / (t['name'] + config['exe'])
        result = call(config['cc'] + config['flags'] + ['-D' + d for d in t.get('defines', [])] +
                      config['link'] + sources_for(t) + config['libs'] + ['-o', binary], t['name'] + '-build')
        r['commands'].append(result)
        if result['status'] != 'PASS':
            r.update(status=result['status'], reason='build failed: ' + result['reason'])
            return None
        return binary

    def private_env(t):
        storage = Path(tempfile.mkdtemp(prefix=t['name'] + '-', dir=storage_root))
        env = os.environ.copy()
        env.update(TIMELITE_TEST_ROOT=str(storage), TMPDIR=str(storage), TMP=str(storage), TEMP=str(storage),
                   ASAN_OPTIONS='halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        return storage, env

    # Storage capability comes from the C probe, never from a Python guess.
    capability = 'unknown'
    probe_rows = []
    for t in [t for t in applicable if t.get('execution') == 'probe']:
        r = row(t['name'], 'NOT RUN', 'not started', coverage=t['coverage'])
        binary = build(t, r)
        if binary and config['execute']:
            storage, env = private_env(t)
            result = call([binary], t['name'] + '-run', env, storage)
            r['commands'].append(result)
            r['storage'] = str(storage)
            code = result['exit_code']
            if code == 0:
                capability = 'supported'
                r.update(status='PASS', reason='durable provisioning supported on storage root')
            elif code == EXIT_UNSUPPORTED:
                capability = 'unsupported'
                r.update(**unsupported_status(config, 'storage root does not support durable provisioning'))
            else:
                r.update(status=result['status'], reason='probe failed: ' + result['reason'])
        elif binary:
            r.update(status='PASS', reason='compiled only; not executed in this profile', coverage='cross-build')
        probe_rows.append(r)
    meta['storage_capability'] = capability

    def test(t):
        r = row(t['name'], 'NOT RUN', 'not started', coverage=t['coverage'])
        if commands.cancel.is_set():
            r['reason'] = 'cancelled'
            return r
        binary = build(t, r)
        if binary is None:
            return r
        execution = t.get('execution')
        if not config['execute']:
            r.update(status='PASS', reason='compiled only; not executed in this profile', coverage='cross-build')
            return r
        if execution == 'batch-example' and capability != 'supported':
            r.update(**unsupported_status(config, 'batch example needs supported durable provisioning'))
            r['coverage'] = 'public batch consumer compiled; runtime NOT RUN'
            return r
        storage, env = private_env(t)
        r['storage'] = str(storage)
        args = [binary]
        if execution == 'batch-example':
            args += [storage / 'example.db', storage / 'example.wal']
        result = call(args, t['name'] + '-run', env, storage)
        r['commands'].append(result)
        r.update(status=result['status'], reason=result['reason'])
        code = result['exit_code']
        if code == EXIT_UNSUPPORTED and execution == 'batch':
            r.update(**unsupported_status(config, 'unsupported provisioning rejected; native batch behavior NOT RUN'))
            r['coverage'] = ('unsupported provisioning contract verified' if config['storage'] == 'contract'
                             else 'native batch behavior NOT RUN: storage unsupported')
        elif code == EXIT_GROUP_SKIPPED:
            r.update(status='SKIP', reason='required group skipped by the test (exit 78); see log', required=True)
        elif code == 0 and execution == 'batch':
            r['coverage'] = 'native batch behavior exercised on supported storage'
        elif code == 0 and 'stdout' in t and log_output(result) != t['stdout']:
            r.update(status='FAIL', reason='output differs from inventory expectation')
        return r

    remaining = [t for t in applicable if t.get('execution') != 'probe']
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        rows += probe_rows + list(pool.map(test, remaining))
    return rows, meta


def target_abi(macros):
    """Short architecture/ABI description from a compiler's predefined macros."""
    defined = {line.split()[1] for line in macros.splitlines() if line.startswith('#define ') and len(line.split()) > 1}
    architecture = next((name for macro, name in [('__x86_64__', 'x86-64'), ('__i386__', 'x86 32-bit'),
                                                   ('__aarch64__', 'ARM64'), ('__arm__', 'ARM32')]
                         if macro in defined), 'unknown')
    system = next((name for macro, name in [('_WIN32', 'Windows'), ('__APPLE__', 'macOS'), ('__linux__', 'Linux')]
                   if macro in defined), 'unknown OS')
    pointer = next((line.split()[2] for line in macros.splitlines()
                    if line.startswith('#define __SIZEOF_POINTER__ ')), '?')
    return '%s %s, %s-byte pointers' % (system, architecture, pointer)


def unsupported_status(config, reason):
    """How an ENOTSUP result counts, by profile storage policy."""
    if config['storage'] == 'contract':
        return dict(status='PASS', reason=reason + ' (expected contract on this platform)')
    if config['storage'] == 'optional':
        return dict(status='SKIP', required=False,
                    reason=reason + ' (optional here; container storage is not native coverage)')
    return dict(status='BLOCKED', reason=reason + '; pass --test-root DIR on a supported filesystem')


# --- containers --------------------------------------------------------------

def dockerfile(source):
    return Path(source) / 'tools/docker/Dockerfile'


def image_tag(source):
    return 'timelite-tests:' + hashlib.sha256(dockerfile(source).read_bytes()).hexdigest()[:16]


def container(name, source, out, selected, jobs, timeout, test_root, commands):
    """Run one container profile as a worker inside the prepared image."""
    source, out = Path(source), Path(out)
    out.mkdir(parents=True, exist_ok=True)
    tag = image_tag(source)
    meta = dict(image_tag=tag, host=dict(os=platform.platform(), machine=platform.machine()),
                container_platform=CONTAINER_PLATFORM, target=PROFILES[name]['target'],
                emulated=platform.machine().lower() not in ('x86_64', 'amd64'))
    inspected = commands.run(['docker', 'image', 'inspect', tag], source, out / 'image-inspect.log', 60)
    meta['inspect'] = inspected
    if inspected['status'] != 'PASS':
        return [row(name, 'BLOCKED', 'prepared image %s unavailable; run: python3 tools/test.py prepare' % tag,
                    commands=[inspected])], meta
    try:
        info = json.loads(log_output(inspected))[0]
    except (ValueError, IndexError, KeyError):
        return [row(name, 'BLOCKED', 'cannot parse docker image inspect output', commands=[inspected])], meta
    meta['image_id'] = info.get('Id')
    if info.get('Architecture') != 'amd64' or info.get('Os') != 'linux':
        return [row(name, 'BLOCKED', 'prepared image is not linux/amd64', commands=[inspected])], meta
    toolchain = commands.run(['docker', 'run', '--rm', '--pull=never', '--network=none',
                              '--platform=' + CONTAINER_PLATFORM, info['Id'], 'cat', '/etc/timelite-toolchain.txt'],
                             source, out / 'toolchain.log', 120)
    meta['toolchain'] = log_output(toolchain)
    container_name = 'timelite-' + uuid.uuid4().hex
    args = ['docker', 'run', '--rm', '--pull=never', '--network=none', '--platform=' + CONTAINER_PLATFORM,
            '--name', container_name,
            '--mount', 'type=bind,src=%s,dst=/source,readonly' % source,
            '--mount', 'type=bind,src=%s,dst=/out' % out]
    if os.name != 'nt':
        args += ['--user', '%d:%d' % (os.getuid(), os.getgid())]
    inner_root = None
    if test_root:
        # Hand the container one private child, never the caller's directory.
        owned = Path(tempfile.mkdtemp(prefix='timelite-container-', dir=Path(test_root).resolve()))
        meta['storage_root'] = str(owned)
        args += ['--mount', 'type=bind,src=%s,dst=/storage' % owned]
        inner_root = '/storage'
    args += [info['Id'], 'python3', '/source/tools/test.py', 'run', name, '--worker',
             '--output', '/out/worker', '--jobs', str(jobs), '--timeout', str(timeout)]
    if selected:
        args += ['--test', selected]
    if inner_root:
        args += ['--test-root', inner_root]
    inventory_size = len(load_inventory(source))
    try:
        executed = commands.run(args, source, out / 'container.log', timeout * (inventory_size + 4))
    finally:
        # Remove exactly this invocation's container, also after Ctrl-C.
        meta['cleanup'] = Commands().run(['docker', 'rm', '-f', container_name], source, out / 'cleanup.log', 60)
    meta['execution'] = executed
    report = out / 'worker/report.json'
    if not report.exists():
        return [row(name, 'BLOCKED' if executed['status'] == 'BLOCKED' else 'FAIL',
                    'container produced no report (%s); see container.log' % executed['reason'],
                    commands=[executed])], meta
    data = json.loads(report.read_text(encoding='utf-8'))
    meta['worker'] = {k: v for k, v in data.items() if k != 'results'}
    rows = data['results']
    if executed['status'] != 'PASS' and successful(rows):
        rows.append(row(name + '-container', 'FAIL', 'container exited abnormally: ' + executed['reason'],
                        commands=[executed]))
    return rows, meta


# --- reports -----------------------------------------------------------------

def write_reports(out, data):
    out = Path(out)
    (out / 'report.json').write_text(json.dumps(data, indent=2, default=str) + '\n', encoding='utf-8')
    suite = ET.Element('testsuite', name='timelite', tests=str(len(data['results'])))
    failures = skipped = 0
    for r in data['results']:
        case = ET.SubElement(suite, 'testcase', name=r['name'], classname=r.get('profile', 'suite'),
                             time='%.3f' % sum(c.get('seconds', 0) for c in r.get('commands', [])))
        if r['status'] == 'SKIP' and not r.get('required', True):
            ET.SubElement(case, 'skipped', message=r['reason'])
            skipped += 1
        elif not ok(r):
            ET.SubElement(case, 'failure', message=r['status'] + ': ' + r['reason'])
            failures += 1
        ET.SubElement(case, 'system-out').text = '\n'.join(c['log'] for c in r.get('commands', []))
    suite.set('failures', str(failures))
    suite.set('skipped', str(skipped))
    ET.ElementTree(suite).write(out / 'junit.xml', encoding='utf-8', xml_declaration=True)


def print_summary(data, out):
    width = max([len(r.get('profile', '') + '/' + r['name']) for r in data['results']] + [10])
    for r in data['results']:
        label = (r.get('profile', '') + '/' + r['name']).ljust(width)
        seconds = sum(c.get('seconds', 0) for c in r.get('commands', []))
        status = r['status'] + ('' if ok(r) or r['status'] != 'SKIP' else ' (required)')
        print('%s  %-16s %5.1fs  %s' % (label, status, seconds, r.get('coverage') or r['reason']))
    counts = {}
    for r in data['results']:
        key = r['status'] if ok(r) or r['status'] != 'SKIP' else 'SKIP (required)'
        counts[key] = counts.get(key, 0) + 1
    print('\n' + ', '.join('%s %d' % (k, v) for k, v in sorted(counts.items())))
    for r in data['results']:
        if not ok(r):
            print('\n%s/%s %s: %s' % (r.get('profile', ''), r['name'], r['status'], r['reason']))
            for c in r.get('commands', []):
                if c['status'] != 'PASS':
                    print('  command: ' + ' '.join(shlex.quote(x) for x in c['command']))
                    print('  log:     ' + c['log'])
                    break
            if r.get('reproduce'):
                print('  rerun:   ' + ' '.join(shlex.quote(x) for x in r['reproduce']))
    if data.get('stale'):
        print('\nWARNING: source changed during the run; results are STALE for the working tree.')
    print('\n%s in %.1fs; reports: %s' % (data['status'], data.get('seconds', 0), out))


def artifact_root():
    path = ROOT / 'build/test-runs'
    path.mkdir(parents=True, exist_ok=True)
    return path


def new_run_directory(prefix, output):
    if output:
        out = Path(output).resolve()
        out.mkdir(parents=True, exist_ok=False)
        return out
    return Path(tempfile.mkdtemp(prefix=prefix + time.strftime('%Y%m%d-%H%M%S-'), dir=artifact_root()))


# --- actions -----------------------------------------------------------------

def action_list():
    print('Groups:')
    for group, members in GROUPS.items():
        print('  %-15s %s' % (group, ' + '.join(members)))
    print('Profiles:')
    for name, p in PROFILES.items():
        print('  %-15s %s' % (name, p['summary']))
    print('Tests (tools/inventory.json):')
    for t in load_inventory(ROOT):
        print('  %-22s %-8s %-15s %s' % (t['name'], t['backend'], ','.join(t['platforms']), t['coverage']))
    return 0


def action_doctor(commands):
    out = new_run_directory('doctor-', None)
    config = configuration('windows-native' if os.name == 'nt' else 'native')
    # BSD ar has no --version, so the archiver check only resolves the executable.
    archiver = shutil.which(config['ar'][0])
    rows = [row('archiver', 'PASS' if archiver else 'BLOCKED',
                'not found: ' + config['ar'][0] if archiver is None else 'found', detail=[archiver or ''])]
    checks = [('compiler', config['cc'] + ['--version'], True),
              ('docker', ['docker', 'version', '--format', '{{.Server.Os}}/{{.Server.Arch}}'], False),
              ('image', ['docker', 'image', 'inspect', '--format', '{{.Id}} {{.Os}}/{{.Architecture}}',
                         image_tag(ROOT)], False)]
    for name, cmd, required in checks:
        r = commands.run(cmd, ROOT, out / (name + '.log'), 60)
        reason = r['reason'] if r['status'] == 'PASS' else r['reason'] + '; see ' + r['log']
        rows.append(row(name, r['status'], reason, required=required, commands=[r],
                        detail=log_output(r).strip().splitlines()[:1]))
    data = dict(action='doctor', host=dict(os=platform.platform(), machine=platform.machine(),
                python=platform.python_version()), image_tag=image_tag(ROOT), results=rows)
    data['status'] = 'PASS' if all(ok(r) or not r['required'] for r in rows) else 'FAIL'
    write_reports(out, data)
    print('host: %s %s; python %s' % (platform.platform(), platform.machine(), platform.python_version()))
    for r in rows:
        detail = r['detail'][0] if r['detail'] else ''
        print('%-10s %-8s %s' % (r['name'], r['status'], detail if r['status'] == 'PASS' else r['reason']))
    if platform.machine().lower() not in ('x86_64', 'amd64'):
        print('note: container profiles run linux/amd64 under emulation on this host')
    print('%s; reports: %s' % (data['status'], out))
    return 0 if data['status'] == 'PASS' else 1


def action_prepare(commands):
    out = new_run_directory('prepare-', None)
    tag = image_tag(ROOT)
    build = commands.run(['docker', 'build', '--platform=' + CONTAINER_PLATFORM, '--tag', tag,
                          str(ROOT / 'tools/docker')], ROOT, out / 'build.log', 1800)
    rows = [row('build', build['status'], build['reason'], commands=[build])]
    if build['status'] == 'PASS':
        versions = commands.run(['docker', 'run', '--rm', '--pull=never', '--network=none',
                                 '--platform=' + CONTAINER_PLATFORM, tag, 'cat', '/etc/timelite-toolchain.txt'],
                                ROOT, out / 'toolchain.log', 120)
        rows.append(row('toolchain', versions['status'], versions['reason'], commands=[versions]))
        (out / 'toolchain.txt').write_text(log_output(versions), encoding='utf-8')
        print(log_output(versions), end='')
    data = dict(action='prepare', image_tag=tag, host=dict(os=platform.platform(), machine=platform.machine()),
                results=rows, status='PASS' if successful(rows) else 'FAIL')
    write_reports(out, data)
    print('%s: image %s; reports: %s' % (data['status'], tag, out))
    return 0 if data['status'] == 'PASS' else 1


def selftest(source, out, timeout, commands):
    result = commands.run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tools', '-p', 'test_runner.py'],
                          source, Path(out) / 'unittest.log', timeout)
    return [row('unittest', result['status'], result['reason'], coverage='runner behavior unit tests',
                commands=[result])], {}


def action_run(args, commands):
    out = new_run_directory('run-', args.output)
    if args.worker:
        source = ROOT
        files = source_files(ROOT)
        identity, count = digest(files), len(files)
    else:
        source = out / 'source'
        identity, count = snapshot(ROOT, source)
    data = dict(suite='timelite', run_directory=str(out), started=time.strftime('%Y-%m-%dT%H:%M:%S%z'),
                invocation=[str(x) for x in sys.argv], source=dict(digest=identity, files=count, **git_info(ROOT)),
                host=dict(os=platform.platform(), machine=platform.machine(), python=platform.python_version()),
                worker=bool(args.worker), profiles={}, results=[])
    clock = time.monotonic()
    names = list(GROUPS.get(args.profile, [args.profile]))
    inventory_names = {t['name'] for t in load_inventory(source)}
    for name in names:
        profile_out = out / name
        kind = PROFILES[name]['kind']
        if commands.cancel.is_set():
            rows, meta = [row(name, 'NOT RUN', 'cancelled')], {}
        elif kind == 'selftest':
            if args.test:
                rows, meta = [row('selection', 'FAIL', '--test selects C inventory tests; the runner profile has none')], {}
            else:
                rows, meta = selftest(source, profile_out, args.timeout, commands)
        elif kind == 'container' and not args.worker:
            rows, meta = container(name, source, profile_out, args.test, args.jobs, args.timeout,
                                   args.test_root, commands)
        else:
            rows, meta = native(name, source, profile_out, args.test, args.jobs, args.timeout,
                                args.test_root, commands)
        for r in rows:
            r['profile'] = name
            reproduce = [sys.executable, str(ROOT / 'tools/test.py'), 'run', name]
            if r['name'] in inventory_names:
                reproduce += ['--test', r['name']]
            if args.test_root:
                reproduce += ['--test-root', args.test_root]
            r['reproduce'] = reproduce
        worker_digest = meta.get('worker', {}).get('source', {}).get('digest')
        if worker_digest is not None and worker_digest != identity:
            rows.append(row(name + '-source', 'FAIL', 'container tested a different source digest', profile=name))
        data['profiles'][name] = meta
        data['results'] += rows
    after = digest(source_files(ROOT))
    data['stale'] = after != identity
    if data['stale']:
        data['results'].append(row('source', 'FAIL', 'source changed during the run; results are stale',
                                   profile='suite'))
    if commands.cancel.is_set():
        data['results'].append(row('cancellation', 'FAIL', 'suite cancelled by signal', profile='suite'))
    if not any(r['status'] == 'PASS' for r in data['results']):
        data['results'].append(row('selection', 'FAIL', 'no test ran to completion', profile='suite'))
    data['seconds'] = round(time.monotonic() - clock, 3)
    data['status'] = 'PASS' if successful(data['results']) else 'FAIL'
    write_reports(out, data)
    print_summary(data, out)
    return 0 if data['status'] == 'PASS' else 1


def parse(argv):
    parser = argparse.ArgumentParser(prog='tools/test.py', description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='action', required=True)
    sub.add_parser('list', help='show groups, profiles and the test inventory')
    sub.add_parser('doctor', help='check host tools and the prepared Docker image')
    sub.add_parser('prepare', help='build the pinned Docker toolchain image')
    run = sub.add_parser('run', help='run a profile or group')
    run.add_argument('profile', choices=list(PROFILES) + list(GROUPS))
    run.add_argument('--test', help='run exactly one inventory test by name')
    run.add_argument('--jobs', type=int, default=2, help='parallel tests per configuration (1..8, default 2)')
    run.add_argument('--timeout', type=float, default=120, help='seconds per command (default 120)')
    run.add_argument('--test-root', help='existing directory on a supported filesystem for storage tests')
    run.add_argument('--output', help='new run directory (default build/test-runs/run-<time>-<id>)')
    run.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.action == 'run':
        if not 1 <= args.jobs <= 8:
            parser.error('--jobs must be between 1 and 8')
        if not 0 < args.timeout <= 3600:
            parser.error('--timeout must be between 0 and 3600 seconds')
    return args


def main(argv=None):
    args = parse(argv)
    if args.action == 'list':
        return action_list()
    commands = Commands()
    previous = [(s, signal.signal(s, lambda *_: commands.cancel.set())) for s in (signal.SIGINT, signal.SIGTERM)]
    try:
        if args.action == 'doctor':
            return action_doctor(commands)
        if args.action == 'prepare':
            return action_prepare(commands)
        return action_run(args, commands)
    finally:
        if commands.cancel.is_set():
            commands.stop_all()
        for s, handler in previous:
            signal.signal(s, handler)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print('BLOCKED: ' + str(error), file=sys.stderr)
        sys.exit(2)
