# -*- coding: utf-8 -*-
"""Тесты tools/lib/epochs.py (ADR-090): `python3 tools/lib/test_epochs.py`. База не нужна — метки подаются текстом."""
import argparse
import io
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import epochs as E  # noqa: E402

H = 3600 * 1000
NOW = 1_800_000_000_000
TEXT = '\n'.join([
    '1\t%d\tlegacy\tlegacy\tmigration' % (NOW - 30 * H),
    '2\t%d\tbridge\tопыт B\towner' % (NOW - 10 * H),
    '3\t%d\tabsolute\tопыт C\towner' % (NOW - 2 * H),
])
EP = E.parse_epochs(TEXT)


class ParseTests(unittest.TestCase):
    def test_parse(self):
        self.assertEqual([e['epoch'] for e in EP], [1, 2, 3])
        self.assertEqual(EP[1]['note'], 'опыт B')
        self.assertEqual(E.current(EP)['epoch'], 3)

    def test_empty_means_no_marks_yet(self):
        self.assertEqual(E.parse_epochs(''), [])
        self.assertEqual(E.current([])['epoch'], 0)

    def test_corrupted_order_is_an_error_not_a_guess(self):
        with self.assertRaises(E.EpochsUnavailable):
            E.parse_epochs('1\t100\tx\tn\ta\n2\t50\tx\tn\ta')

    def test_load_reports_unavailable(self):
        import subprocess

        def boom(_):
            raise subprocess.CalledProcessError(1, 'psql', stderr='relation "ce_reset_epoch" does not exist')
        with self.assertRaises(E.EpochsUnavailable):
            E.load_epochs(boom)
        self.assertEqual(len(E.load_epochs(lambda _: TEXT)), 3)


class EpochAtTests(unittest.TestCase):
    def test_boundaries(self):
        self.assertEqual(E.epoch_at(EP, NOW - 31 * H), 0)
        self.assertEqual(E.epoch_at(EP, NOW - 10 * H), 2)          # ровно в момент метки — уже новая эпоха
        self.assertEqual(E.epoch_at(EP, NOW - 10 * H - 1), 1)
        self.assertEqual(E.epoch_at(EP, NOW), 3)

    def test_split_by_epoch(self):
        rows = [{'t': NOW - 20 * H}, {'t': NOW - 5 * H}, {'t': NOW - H}, {'t': NOW - 40 * H}]
        got = E.split_by_epoch(rows, lambda r: r['t'], EP)
        self.assertEqual(sorted(got), [0, 1, 2, 3])
        self.assertEqual({k: len(v) for k, v in got.items()}, {0: 1, 1: 1, 2: 1, 3: 1})
        self.assertEqual(got[2][0]['t'], NOW - 5 * H)


class ResolveTests(unittest.TestCase):
    def test_default_is_current_epoch(self):
        w = E.resolve(EP, now_ms=NOW)
        self.assertEqual((w.from_ms, w.to_ms, w.crosses_reset, w.requested), (NOW - 2 * H, NOW, False, 'current'))
        self.assertEqual([e['epoch'] for e in w.epochs], [3])

    def test_explicit_window_through_a_mark_is_refused(self):
        with self.assertRaises(E.CrossesResetError) as cm:
            E.resolve(EP, from_ms=NOW - 20 * H, to_ms=NOW - 5 * H, now_ms=NOW)
        self.assertEqual(cm.exception.markers, [NOW - 10 * H])
        self.assertIn('--allow-cross-reset', str(cm.exception))

    def test_explicit_window_inside_one_epoch_is_fine(self):
        w = E.resolve(EP, from_ms=NOW - 9 * H, to_ms=NOW - 5 * H, now_ms=NOW)
        self.assertFalse(w.crosses_reset)
        self.assertEqual([e['epoch'] for e in w.epochs], [2])

    def test_crossing_needs_explicit_permission_and_a_reason(self):
        with self.assertRaises(ValueError):
            E.resolve(EP, from_ms=NOW - 20 * H, to_ms=NOW, allow_cross=True, now_ms=NOW)
        w = E.resolve(EP, from_ms=NOW - 20 * H, to_ms=NOW, allow_cross=True, reason='сравнение режимов', now_ms=NOW)
        self.assertTrue(w.crosses_reset)
        self.assertEqual(w.cross_reason, 'сравнение режимов')
        self.assertEqual([e['epoch'] for e in w.epochs], [1, 2, 3])

    def test_whole_past_epoch(self):
        w = E.resolve(EP, epoch='2', now_ms=NOW)
        self.assertEqual((w.from_ms, w.to_ms), (NOW - 10 * H, NOW - 2 * H))
        self.assertFalse(w.crosses_reset)

    def test_unknown_epoch_and_ambiguity(self):
        with self.assertRaises(ValueError):
            E.resolve(EP, epoch='9', now_ms=NOW)
        with self.assertRaises(ValueError):
            E.resolve(EP, epoch='2', from_ms=1, to_ms=2, now_ms=NOW)

    def test_all_epochs_is_a_crossing(self):
        with self.assertRaises(E.CrossesResetError):
            E.resolve(EP, epoch='all', now_ms=NOW)
        self.assertTrue(E.resolve(EP, epoch='all', allow_cross=True, reason='обзор', now_ms=NOW).crosses_reset)
        self.assertFalse(E.resolve(EP[:1], epoch='all', now_ms=NOW).crosses_reset)   # одна эпоха — смешивать нечего

    def test_no_marks_yet(self):
        w = E.resolve([], from_ms=NOW - 5 * H, to_ms=NOW, now_ms=NOW)
        self.assertFalse(w.crosses_reset)
        self.assertEqual([e['epoch'] for e in w.epochs], [0])

    def test_bad_window(self):
        with self.assertRaises(ValueError):
            E.resolve(EP, from_ms=NOW, to_ms=NOW - H, now_ms=NOW)

    def test_header_names_epoch_and_experiment(self):
        text = '\n'.join(E.resolve(EP, epoch='2', now_ms=NOW).header(EP))
        self.assertIn('epoch_id=2', text)
        self.assertIn('опыт B', text)
        self.assertIn('crosses_reset=false', text)
        crossed = '\n'.join(E.resolve(EP, from_ms=NOW - 20 * H, to_ms=NOW, allow_cross=True, reason='обзор', now_ms=NOW).header(EP))
        self.assertIn('crosses_reset=true', crossed)
        self.assertIn('обзор', crossed)


class CliTests(unittest.TestCase):
    def parse(self, argv):
        p = E.add_cli_args(argparse.ArgumentParser())
        return p.parse_args(argv)

    def test_positional_start_end_still_work(self):
        a = self.parse(['%d' % (NOW - 9 * H), '%d' % (NOW - 5 * H)])
        w = E.window_from_args(a, EP, now_ms=NOW)
        self.assertEqual((w.from_ms, w.to_ms), (NOW - 9 * H, NOW - 5 * H))

    def test_no_args_is_current_epoch(self):
        w = E.window_from_args(self.parse([]), EP, now_ms=NOW)
        self.assertEqual(w.requested, 'current')

    def test_epoch_flag(self):
        w = E.window_from_args(self.parse(['--epoch', '1']), EP, now_ms=NOW)
        self.assertEqual((w.from_ms, w.to_ms), (NOW - 30 * H, NOW - 10 * H))


class TtlTests(unittest.TestCase):
    def test_warns_beyond_seven_days(self):
        buf = io.StringIO()
        self.assertTrue(E.warn_if_beyond_ttl(NOW - 8 * 24 * H, NOW, buf))
        self.assertIn('ClickHouse', buf.getvalue())
        self.assertFalse(E.warn_if_beyond_ttl(NOW - 6 * 24 * H, NOW, io.StringIO()))


if __name__ == '__main__':
    unittest.main(verbosity=1)
