#!/usr/bin/env python3
"""Unit tests for tools/test.py. Standard library only.

Each test builds a small fixture repository (in a directory whose name contains
a space) with its own tools/inventory.json and C programs, points the runner at
it, and checks the report. Run directly with
    python3 -m unittest discover -s tools -p test_runner.py
or through the suite with `python3 tools/test.py run runner`.
"""
import contextlib
import importlib.util
import io
import json
import os
import shutil
import sys
import tempfile
import threading
import time
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('timelite_test_runner', HERE / 'test.py')
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)

HOST_PROFILE = 'windows-native' if os.name == 'nt' else 'native'
PLATFORMS = ['posix', 'windows']

EXIT_PROGRAM = '#include <stdio.h>\nint main(void)\n{\n    puts("%s");\n    return %d;\n}\n'
HANG_PROGRAM = """#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif
int main(void)
{
    for (;;)
    {
#if defined(_WIN32)
        Sleep(1000);
#else
        sleep(1);
#endif
    }
}
"""
MUTATE_PROGRAM = """#include <stdio.h>
#include <stdlib.h>
int main(void)
{
    FILE *f = fopen(getenv("TIMELITE_FIXTURE_MUTATE"), "a");
    if (f == NULL)
    {
        return 1;
    }
    fputs("/* changed during the run */\\n", f);
    return fclose(f) == 0 ? 0 : 1;
}
"""
LIBRARY_HEADER = 'int fixture_value(void);\nint fixture_backend(void);\n'
LIBRARY_SOURCE = '#include "timelite.h"\nint fixture_value(void)\n{\n    return 42;\n}\n'
BACKEND_SOURCE = '#include "timelite.h"\nint fixture_backend(void)\n{\n    return 1;\n}\n'
LIBRARY_TEST = ('#include "timelite.h"\n#include <stdio.h>\nint main(void)\n{\n'
                '    printf("%d\\n", fixture_value() + fixture_backend());\n    return 0;\n}\n')


def entry(name, **extra):
    e = dict(name=name, source='tests/%s.c' % name, backend='none', platforms=PLATFORMS, coverage='fixture ' + name)
    e.update(extra)
    return e


class Fixture:
    """A throwaway repository the runner can snapshot, build and run."""

    def __init__(self, base):
        self.root = Path(tempfile.mkdtemp(prefix='fixture repo ', dir=base))
        (self.root / 'tests').mkdir()
        (self.root / 'tools').mkdir()
        self.inventory = []

    def program(self, name, source, **extra):
        (self.root / 'tests' / (name + '.c')).write_text(source, encoding='utf-8')
        self.inventory.append(entry(name, **extra))
        return self

    def exit_program(self, name, code, text=None, **extra):
        return self.program(name, EXIT_PROGRAM % (text or name, code), **extra)

    def library(self):
        (self.root / 'timelite.h').write_text(LIBRARY_HEADER, encoding='utf-8')
        (self.root / 'timelite.c').write_text(LIBRARY_SOURCE, encoding='utf-8')
        (self.root / 'file_io.c').write_text(BACKEND_SOURCE, encoding='utf-8')
        (self.root / 'file_io_windows.c').write_text(BACKEND_SOURCE, encoding='utf-8')
        return self.program('library_consumer', LIBRARY_TEST, backend='library', stdout='43\n')

    def write(self):
        (self.root / 'tools/inventory.json').write_text(json.dumps(self.inventory, indent=1), encoding='utf-8')
        return self.root


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.base = Path(tempfile.mkdtemp(prefix='timelite runner '))
        self.addCleanup(shutil.rmtree, self.base, True)
        self.saved_root = runner.ROOT
        self.addCleanup(setattr, runner, 'ROOT', self.saved_root)
        self.saved_env = dict(os.environ)
        self.addCleanup(self.restore_env)

    def restore_env(self):
        os.environ.clear()
        os.environ.update(self.saved_env)

    def run_suite(self, root, *args):
        """Run the runner in-process against root; return (exit code, report, stdout)."""
        runner.ROOT = Path(root)
        out = self.base / ('run %d' % int(time.monotonic() * 1000000))
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            code = runner.main(['run'] + list(args) + ['--output', str(out)])
        report = json.loads((out / 'report.json').read_text(encoding='utf-8'))
        return code, report, stdout.getvalue()

    @staticmethod
    def by_name(report):
        return {r['name']: r for r in report['results']}

    # --- Commands -----------------------------------------------------------

    def test_command_success_failure_and_missing_executable(self):
        commands = runner.Commands()
        log = self.base / 'logs' / 'a b.log'
        good = commands.run([sys.executable, '-c', 'print("hello")'], self.base, log, 10)
        self.assertEqual((good['status'], good['exit_code']), ('PASS', 0))
        self.assertEqual(runner.log_output(good), 'hello\n')
        bad = commands.run([sys.executable, '-c', 'raise SystemExit(3)'], self.base, log, 10)
        self.assertEqual((bad['status'], bad['exit_code'], bad['reason']), ('FAIL', 3, 'exit 3'))
        missing = commands.run(['timelite-no-such-tool-xyz', '--version'], self.base, log, 10)
        self.assertEqual(missing['status'], 'BLOCKED')
        self.assertIsNone(missing['exit_code'])
        self.assertIn('cannot start', missing['reason'])

    def test_timeout_kills_and_reaps_child(self):
        commands = runner.Commands()
        started = time.monotonic()
        result = commands.run([sys.executable, '-c', 'import time; time.sleep(30)'], self.base,
                              self.base / 'timeout.log', 0.5)
        self.assertEqual(result['status'], 'FAIL')
        self.assertIn('timeout', result['reason'])
        self.assertLess(time.monotonic() - started, 15)
        self.assertEqual(commands.children, set())

    def test_cancellation_stops_running_child(self):
        commands = runner.Commands()
        results = []
        thread = threading.Thread(target=lambda: results.append(commands.run(
            [sys.executable, '-c', 'import time; time.sleep(30)'], self.base, self.base / 'cancel.log', 60)))
        thread.start()
        time.sleep(0.5)
        commands.stop_all()
        thread.join(15)
        self.assertFalse(thread.is_alive())
        self.assertEqual((results[0]['status'], results[0]['reason']), ('FAIL', 'cancelled'))
        self.assertEqual(commands.children, set())
        after = commands.run([sys.executable, '-c', 'pass'], self.base, self.base / 'after.log', 10)
        self.assertEqual(after['status'], 'NOT RUN')

    # --- result semantics -------------------------------------------------

    def test_successful_requires_rows_and_allows_only_optional_skips(self):
        self.assertFalse(runner.successful([]))
        self.assertTrue(runner.successful([runner.row('a', 'PASS', 'ok')]))
        self.assertTrue(runner.successful([runner.row('a', 'PASS', 'ok'),
                                           runner.row('b', 'SKIP', 'optional', required=False)]))
        self.assertFalse(runner.successful([runner.row('b', 'SKIP', 'optional', required=False)]),
                         'optional skips alone must not pass')
        for status in ('FAIL', 'BLOCKED', 'NOT RUN', 'SKIP'):
            self.assertFalse(runner.successful([runner.row('a', 'PASS', 'ok'), runner.row('b', status, 'x')]),
                             status)

    def test_snapshot_excludes_git_and_build_output(self):
        root = Fixture(self.base).exit_program('pass', 0).write()
        (root / '.git').mkdir()
        (root / '.git/config').write_text('x', encoding='utf-8')
        (root / 'build').mkdir()
        (root / 'build/pass.o').write_bytes(b'\0')
        (root / 'secret.txt').write_text('private', encoding='utf-8')
        files = runner.source_files(root)
        self.assertEqual(sorted(files), ['tests/pass.c', 'tools/inventory.json'])
        changed = dict(files, **{'tests/pass.c': files['tests/pass.c'] + b'\n'})
        self.assertNotEqual(runner.digest(files), runner.digest(changed))

    # --- end-to-end on fixtures -------------------------------------------

    def test_passing_and_failing_fixtures_and_report_accuracy(self):
        root = (Fixture(self.base).exit_program('pass', 0).exit_program('fail', 5)
                .exit_program('wrong_output', 0, text='unexpected', stdout='expected\n').library().write())
        self.assertIn(' ', str(root))
        code, report, text = self.run_suite(root, HOST_PROFILE)
        rows = self.by_name(report)
        self.assertEqual(code, 1)
        self.assertEqual(report['status'], 'FAIL')
        self.assertEqual(rows['pass']['status'], 'PASS')
        self.assertEqual(rows['library_consumer']['status'], 'PASS')
        self.assertEqual((rows['fail']['status'], rows['fail']['reason']), ('FAIL', 'exit 5'))
        self.assertEqual(rows['fail']['commands'][-1]['exit_code'], 5)
        self.assertEqual(rows['wrong_output']['status'], 'FAIL')
        self.assertIn('differs', rows['wrong_output']['reason'])
        self.assertTrue(Path(rows['fail']['commands'][-1]['log']).is_file())
        self.assertIn('--test', rows['fail']['reproduce'])
        self.assertIn('rerun:', text)
        self.assertEqual(report['source']['digest'], runner.digest(runner.source_files(root)))
        self.assertFalse(report['stale'])
        suite = ET.parse(Path(report['run_directory']) / 'junit.xml').getroot()
        self.assertEqual((suite.get('tests'), suite.get('failures')), ('4', '2'))
        meta = report['profiles'][HOST_PROFILE]
        self.assertTrue(meta['compiler']['version'])
        self.assertTrue(runner.successful(meta['prerequisites']))

    def test_selecting_one_test_runs_only_it(self):
        root = Fixture(self.base).exit_program('pass', 0).exit_program('fail', 1).write()
        code, report, _ = self.run_suite(root, HOST_PROFILE, '--test', 'pass')
        self.assertEqual(code, 0)
        self.assertEqual([r['name'] for r in report['results']], ['pass'])

    def test_empty_selection_fails(self):
        root = Fixture(self.base).exit_program('pass', 0).write()
        code, report, _ = self.run_suite(root, HOST_PROFILE, '--test', 'missing')
        self.assertEqual(code, 1)
        self.assertEqual(report['results'][0]['status'], 'FAIL')
        self.assertIn('missing', report['results'][0]['reason'])
        code, report, _ = self.run_suite(root, 'runner', '--test', 'pass')
        self.assertEqual(code, 1)

    def test_required_skip_fails_and_optional_skip_passes(self):
        root = (Fixture(self.base).exit_program('pass', 0).exit_program('group_skipped', 78)
                .exit_program('other_platform', 0, platforms=['nowhere']).write())
        code, report, _ = self.run_suite(root, HOST_PROFILE)
        rows = self.by_name(report)
        self.assertEqual(code, 1)
        self.assertEqual((rows['group_skipped']['status'], rows['group_skipped']['required']), ('SKIP', True))
        self.assertEqual((rows['other_platform']['status'], rows['other_platform']['required']), ('SKIP', False))
        code, report, _ = self.run_suite(root, HOST_PROFILE, '--test', 'other_platform')
        self.assertEqual(code, 1, 'selecting an inapplicable test must not pass')
        root = Fixture(self.base).exit_program('pass', 0).exit_program('other_platform', 0, platforms=['nowhere']).write()
        code, report, _ = self.run_suite(root, HOST_PROFILE)
        self.assertEqual(code, 0)

    def test_missing_compiler_blocks_every_test(self):
        root = Fixture(self.base).exit_program('pass', 0).write()
        os.environ['CC'] = 'timelite-no-such-compiler-xyz'
        code, report, _ = self.run_suite(root, HOST_PROFILE)
        self.assertEqual(code, 1)
        rows = self.by_name(report)
        self.assertEqual(rows['pass']['status'], 'BLOCKED')
        self.assertEqual(rows['selection']['reason'], 'no test ran to completion')

    def test_timeout_is_a_failure_with_reason(self):
        root = Fixture(self.base).program('hang', HANG_PROGRAM).write()
        code, report, _ = self.run_suite(root, HOST_PROFILE, '--timeout', '2')
        self.assertEqual(code, 1)
        self.assertEqual(report['results'][0]['status'], 'FAIL')
        self.assertIn('timeout', report['results'][0]['reason'])

    def test_source_change_during_run_marks_results_stale(self):
        root = Fixture(self.base).program('mutate', MUTATE_PROGRAM).write()
        os.environ['TIMELITE_FIXTURE_MUTATE'] = str(root / 'tests/mutate.c')
        code, report, text = self.run_suite(root, HOST_PROFILE)
        rows = self.by_name(report)
        self.assertEqual(rows['mutate']['status'], 'PASS')
        self.assertTrue(report['stale'])
        self.assertEqual(rows['source']['status'], 'FAIL')
        self.assertEqual(code, 1)
        self.assertIn('STALE', text)

    @unittest.skipIf(os.name == 'nt', 'sanitizer profile is POSIX-only')
    def test_configurations_are_isolated(self):
        root = Fixture(self.base).library().write()
        code, report, _ = self.run_suite(root, 'native')
        self.assertEqual(code, 0, report)
        code, sanitized, _ = self.run_suite(root, 'sanitize')
        self.assertEqual(code, 0, sanitized)
        plain = report['profiles']['native']
        asan = sanitized['profiles']['sanitize']
        self.assertNotEqual(plain['prerequisites'][0]['command'], asan['prerequisites'][0]['command'])
        self.assertIn('-fsanitize=address,undefined', asan['prerequisites'][0]['command'])
        self.assertNotIn('-fsanitize=address,undefined', plain['prerequisites'][0]['command'])
        objects = {Path(p['command'][-1]).parent for p in plain['prerequisites'] + asan['prerequisites']}
        self.assertEqual(len(objects), 2, 'object files of the two configurations share a directory')

    def test_storage_capability_from_probe(self):
        root = (Fixture(self.base).exit_program('pass', 0)
                .exit_program('storage_probe', 77, execution='probe')
                .exit_program('batch', 77, execution='batch')
                .exit_program('batches', 0, execution='batch-example').write())
        code, report, _ = self.run_suite(root, HOST_PROFILE)
        rows = self.by_name(report)
        self.assertEqual(report['profiles'][HOST_PROFILE]['storage_capability'], 'unsupported')
        if os.name == 'nt':
            self.assertEqual(code, 0)
            self.assertEqual({r['status'] for r in report['results']}, {'PASS'})
            self.assertIn('contract', rows['batch']['reason'])
        else:
            self.assertEqual(code, 1)
            self.assertEqual(rows['storage_probe']['status'], 'BLOCKED')
            self.assertEqual(rows['batch']['status'], 'BLOCKED')
            self.assertEqual(rows['batches']['status'], 'BLOCKED')
            self.assertEqual(rows['batches']['commands'][-1]['log'].endswith('batches-build.log'), True)
            self.assertIn('--test-root', rows['batch']['reason'])
        if os.name != 'nt' and shutil.which('gcc'):
            code, report, _ = self.run_suite(root, 'linux', '--worker')
            rows = self.by_name(report)
            self.assertEqual(code, 0, report)
            self.assertEqual((rows['batch']['status'], rows['batch']['required']), ('SKIP', False))
            self.assertEqual((rows['batches']['status'], rows['batches']['required']), ('SKIP', False))

    def test_explicit_test_root_gets_private_child(self):
        root = Fixture(self.base).exit_program('pass', 0).write()
        storage = self.base / 'storage root'
        storage.mkdir()
        code, report, _ = self.run_suite(root, HOST_PROFILE, '--test-root', str(storage))
        self.assertEqual(code, 0)
        child = Path(report['profiles'][HOST_PROFILE]['storage_root'])
        self.assertEqual(child.parent, storage.resolve())
        self.assertTrue(Path(report['results'][0]['storage']).is_relative_to(child))
        self.assertIn('--test-root', report['results'][0]['reproduce'])


if __name__ == '__main__':
    unittest.main()
