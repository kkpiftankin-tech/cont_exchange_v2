# -*- coding: utf-8 -*-
"""Тесты epoch-guard-checker: `python3 tools/epoch-guard-checker/tests/test_check.py`. Репозиторий не нужен — строится временное дерево."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import check as C  # noqa: E402

REGISTRY = """
epochRegistry:
  version: 1
  classes: {A: a, B: b, C: c, E: e, N: n}
  tables:
    postgres:
      ce_state: {class: A}
      ce_journal: {class: B, time_column: ts}
      ce_market: {class: C}
      ce_reset_epoch: {class: E}
      users: {class: N}
    clickhouse:
      ext_log: {class: B, ddl: external}
"""


def make_tree(files):
    root = tempfile.mkdtemp(prefix='epoch_guard_')
    for rel, text in files.items():
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'w', encoding='utf-8').write(text)
    return root


class CheckerTests(unittest.TestCase):
    def tree(self, extra=None):
        files = {'specs/domain/epoch-classes.yaml': REGISTRY,
                 'infra/postgres/init.sql': 'CREATE TABLE IF NOT EXISTS ce_state (a int);\nCREATE TABLE ce_journal (ts timestamptz);\n'
                                            'CREATE TABLE IF NOT EXISTS ce_market (a int);\nCREATE TABLE ce_reset_epoch (a int);\nCREATE TABLE users (a int);\n'}
        files.update(extra or {})
        return make_tree(files)

    def test_discovers_ddl_and_loads_registry(self):
        root = self.tree()
        reg = C.load_registry(root)
        self.assertEqual(reg['ce_journal']['class'], 'B')
        self.assertEqual(set(C.discover_tables(root)), {'ce_state', 'ce_journal', 'ce_market', 'ce_reset_epoch', 'users'})

    def test_unclassified_table_is_found_and_service_copies_are_skipped(self):
        root = self.tree({'infra/postgres/more.sql': 'CREATE TABLE new_table (a int);\nCREATE TABLE ce_state_bak_20261029 (a int);\nCREATE TABLE tmp_x (a int);'})
        found = C.discover_tables(root)
        self.assertIn('new_table', found)
        self.assertNotIn('ce_state_bak_20261029', found)
        self.assertNotIn('tmp_x', found)
        self.assertEqual(C.main(['--root', root, '--strict']), 1)

    def test_read_of_journal_without_marker_is_flagged(self):
        root = self.tree({'tools/x/report.py': 'q = "SELECT * FROM ce_journal WHERE ts > now()"\n'})
        reads = C.unguarded_reads(root, C.load_registry(root))
        self.assertEqual([(r[0], r[2]) for r in reads], [('tools/x/report.py', 'ce_journal')])

    def test_marker_on_same_line_or_above_clears_the_finding(self):
        root = self.tree({
            'tools/x/a.py': 'q = "SELECT * FROM ce_journal"  # epoch-guard: window\n',
            'frontend/api/b.js': '// epoch-guard: ok(последние N строк, не накопление)\nconst q = `\n  SELECT *\n    FROM ce_journal\n`;\n',
            'tools/x/c.sql': '-- epoch-guard: window\nSELECT * FROM ce_journal JOIN ext_log ON true;\n',
        })
        self.assertEqual(C.unguarded_reads(root, C.load_registry(root)), [])

    def test_marker_too_far_above_does_not_count(self):
        far = '-- epoch-guard: window\n' + '\n' * 20 + 'SELECT * FROM ce_journal;\n'
        root = self.tree({'tools/x/far.sql': far})
        self.assertEqual(len(C.unguarded_reads(root, C.load_registry(root))), 1)

    def test_file_marker_exempts_whole_file(self):
        root = self.tree({'tools/x/repair.sql': '-- epoch-guard-file: ok(разовая правка данных)\nSELECT * FROM ce_journal;\n'})
        self.assertEqual(C.unguarded_reads(root, C.load_registry(root)), [])

    def test_state_market_and_unrelated_tables_need_no_marker(self):
        root = self.tree({'tools/x/ok.py': 'a = "FROM ce_state"; b = "FROM ce_market"; c = "FROM users"; d = "FROM ce_reset_epoch"\n'})
        self.assertEqual(C.unguarded_reads(root, C.load_registry(root)), [])

    def test_tests_and_libs_are_not_scanned(self):
        root = self.tree({'frontend/api/z.test.js': 'const q = "SELECT * FROM ce_journal"',
                          'tools/lib/helper.py': 'q = "SELECT * FROM ce_journal"',
                          'tools/epoch-guard-checker/self.py': 'q = "SELECT * FROM ce_journal"'})
        self.assertEqual(C.unguarded_reads(root, C.load_registry(root)), [])

    def test_report_mode_returns_zero_strict_returns_one(self):
        root = self.tree({'tools/x/report.py': 'q = "SELECT * FROM ce_journal"\n'})
        self.assertEqual(C.main(['--root', root]), 0)
        self.assertEqual(C.main(['--root', root, '--strict']), 1)

    def test_clean_tree_passes_strict(self):
        root = self.tree()
        self.assertEqual(C.main(['--root', root, '--strict']), 0)


if __name__ == '__main__':
    unittest.main(verbosity=1)
