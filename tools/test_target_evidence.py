"""Host tooling checks; no physical power interruption is performed."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('evidence', HERE / 'target/evidence.py')
evidence = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(evidence)
SPEC = importlib.util.spec_from_file_location('runner014', HERE / 'test.py')
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class OracleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.journal = self.root / 'journal'

    def journal_for(self, operations, uncertain=None):
        rows = [dict(kind='start')]
        high = 0
        for operation in operations:
            rows.append(dict(kind='intent', operation=operation))
            if operation[0] == 'A':
                high += 1
            rows.append(dict(kind='ack', operation=operation,
                             sequence=high if operation[0] == 'A' else 0))
        if uncertain:
            rows.append(dict(kind='intent', operation=uncertain))
        self.journal.write_text(''.join(json.dumps(r) + '\n' for r in rows))

    def dump(self, records, floor):
        path = self.root / 'dump'
        path.write_text('open_result=ok\ncommitted_batches=%d\nlast_timestamp_us=%d\n' % (len(records), floor) +
                        ''.join('batch=%d series=14 us=%d value=%d\n' % (s, v, v) for s, v in records))
        return path

    def test_missing_acknowledged_record_rejected(self):
        self.journal_for(['A 100', 'A 200'])
        with self.assertRaises(ValueError):
            evidence.compare(self.journal, self.dump([(2, 200)], 200))

    def test_lost_append_response_has_two_outcomes(self):
        self.journal_for(['A 100'], 'A 200')
        for records, floor in [([(1, 100)], 100), ([(1, 100), (2, 200)], 200)]:
            self.assertTrue(evidence.compare(self.journal, self.dump(records, floor))['interrupted_operation'])

    def test_retention_old_or_new_but_never_partial_segment_or_wal(self):
        self.journal_for(['A 100', 'A 200', 'C 1', 'A 300'], 'R 250')
        for records in [[(1, 100), (2, 200), (3, 300)], [(3, 300)]]:
            evidence.compare(self.journal, self.dump(records, 300))
        for records in [[(2, 200), (3, 300)], []]:
            with self.assertRaises(ValueError):
                evidence.compare(self.journal, self.dump(records, 300))

    def test_complete_retention_preserves_floor_and_sequence(self):
        self.journal_for(['A 100', 'C 1', 'R 200'])
        evidence.compare(self.journal, self.dump([], 100))
        with self.assertRaises(ValueError):
            evidence.compare(self.journal, self.dump([], 0))
        evidence.compare(self.journal, self.dump([(2, 300)], 300), 300)
        with self.assertRaises(ValueError):
            evidence.compare(self.journal, self.dump([(1, 300)], 300), 300)

    def test_successful_retention_requires_new_state(self):
        self.journal_for(['A 100', 'C 1', 'R 200'])
        with self.assertRaises(ValueError):
            evidence.compare(self.journal, self.dump([(1, 100)], 100))

    def test_corrupt_controller_journal_fails_closed(self):
        self.journal.write_text('{"kind":')
        with self.assertRaises(ValueError):
            evidence.candidates(self.journal)

    def test_cross_configuration_is_explicit_and_not_in_preflight(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertIn('blocked', runner.configuration('arm32-cross'))
        self.assertNotIn('arm32-cross', runner.GROUPS['preflight'])

    def test_cross_missing_config_creates_blocked_report(self):
        with patch.dict(os.environ, {}, clear=True):
            rows, meta = runner.native('arm32-cross', HERE.parent, self.root / 'out', None,
                                       1, 30, None, runner.Commands())
        self.assertTrue(all(r['status'] == 'BLOCKED' for r in rows))
        self.assertIn('blocked', meta['configuration'])

    @unittest.skipIf(os.name == 'nt', 'controller requires POSIX pipes and directory sync')
    def test_transport_loss_never_creates_ack(self):
        plan = self.root / 'plan'
        plan.write_text('A 100\n')
        transport = [sys.executable, '-c',
                     'import sys; print("ready", flush=True); sys.stdin.readline()']
        self.assertEqual(evidence.collect(plan, self.journal, transport, 5), 1)
        rows = [json.loads(line) for line in self.journal.read_text().splitlines()]
        self.assertNotIn('ack', [r['kind'] for r in rows])
        self.assertEqual(len(evidence.candidates(self.journal)), 2)

    def test_host_compiler_cannot_pass_as_arm32(self):
        target = dict(device='fixture', isa='fixture', os='Linux', kernel='fixture',
                      abi='fixture', libc='fixture', sdk_identity='fixture',
                      triple='fixture-arm-linux', float_abi='hard', cc=['cc'], ar=['ar'],
                      flags=[], link=[], libs=[])
        config = self.root / 'target.json'
        config.write_text(json.dumps(target))
        with patch.dict(os.environ, {'TIMELITE_ARM32_CONFIG': str(config)}):
            rows, meta = runner.native('arm32-cross', HERE.parent, self.root / 'cross',
                                       'basic', 1, 30, None, runner.Commands())
        self.assertEqual(rows[0]['status'], 'BLOCKED')
        self.assertNotIn('prerequisites', meta)

    @unittest.skipIf(os.name == 'nt', 'controller requires POSIX durable directory sync')
    def test_real_workload_journal_preservation_recovery_and_continuation(self):
        config = runner.configuration('native')
        binaries = []
        for name, source in [('workload', 'tools/target/workload.c'), ('inspect', 'tools/inspect/inspect.c')]:
            binary = self.root / name
            subprocess.run(config['cc'] + config['flags'] + config['link'] +
                           [source, 'timelite.c', 'file_io.c'] + config['libs'] + ['-o', str(binary)],
                           cwd=HERE.parent, check=True, capture_output=True)
            binaries.append(str(binary))
        db, wal = str(self.root / 'db'), str(self.root / 'wal')
        plan = self.root / 'plan'
        plan.write_text('A 100\nA 200\nC 1\nA 300\nC 1\nA 400\nR 250\nC 1\nR 500\n')
        result = evidence.collect(plan, self.journal, [binaries[0], 'create', db, wal], 10)
        self.assertEqual(result, 0, Path(str(self.journal) + '.stderr').read_text())
        trial = self.root / 'trial'
        outcomes = evidence.preserve(db, wal, trial, binaries[1])
        self.assertEqual([x['exit_code'] for x in outcomes.values()], [0, 0])
        evidence.compare(self.journal, trial / 'status.txt')
        self.assertEqual((trial / 'original/database').read_bytes(), Path(db).read_bytes())
        with self.assertRaises(FileExistsError):
            evidence.preserve(db, wal, trial, binaries[1])
        continued = subprocess.run([binaries[0], 'open', str(trial / 'working/database'),
                                    str(trial / 'working/wal')], input='A 600\n',
                                   text=True, capture_output=True, check=True)
        self.assertIn('ok A 600 5', continued.stdout)
        dump = self.root / 'continued'
        with dump.open('w') as output:
            subprocess.run([binaries[1], 'status', str(trial / 'working/database'),
                            str(trial / 'working/wal'), '--dump'], stdout=output, check=True)
        evidence.compare(self.journal, dump, 600)


if __name__ == '__main__':
    unittest.main()
