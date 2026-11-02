# -*- coding: utf-8 -*-
"""Тесты tools/lib/epochs.py (ADR-090): `python3 tools/lib/test_epochs.py`. База не нужна — метки подаются текстом."""
import argparse
import contextlib
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
        w = E.window_from_args(self.parse(['--epoch', '1', '--warmup-min', '0']), EP, now_ms=NOW)
        self.assertEqual((w.from_ms, w.to_ms), (NOW - 30 * H, NOW - 10 * H))
        # по умолчанию прогрев эпохи (60 мин) исключается
        w = E.window_from_args(self.parse(['--epoch', '1']), EP, now_ms=NOW)
        self.assertEqual((w.from_ms, w.to_ms), (NOW - 30 * H + 60 * 60000, NOW - 10 * H))


class WarmupAndBootsTests(unittest.TestCase):
    def test_warmup_is_skipped_for_epoch_windows(self):
        w = E.resolve(EP, epoch='2', now_ms=NOW, warmup_min=60)
        self.assertEqual(w.from_ms, NOW - 10 * H + 60 * 60000)
        self.assertEqual(w.warmup_min, 60)
        self.assertIn('первые 60 мин эпохи исключены', '\n'.join(w.header(EP)))

    def test_warmup_for_current_epoch_and_zero_disables_it(self):
        w = E.resolve(EP, now_ms=NOW, warmup_min=60)
        self.assertEqual(w.from_ms, NOW - 2 * H + 60 * 60000)
        self.assertEqual(E.resolve(EP, now_ms=NOW, warmup_min=0).from_ms, NOW - 2 * H)

    def test_warmup_not_applied_to_explicit_or_all_windows(self):
        self.assertEqual(E.resolve(EP, from_ms=NOW - 9 * H, to_ms=NOW - 5 * H, now_ms=NOW, warmup_min=60).from_ms, NOW - 9 * H)
        w = E.resolve(EP, epoch='all', allow_cross=True, reason='обзор', now_ms=NOW, warmup_min=60)
        self.assertEqual(w.warmup_min, 0)

    def test_warmup_not_applied_when_epoch_shorter_than_warmup(self):
        short = E.parse_epochs('1\t%d\tabsolute\tкороткая\ta\n2\t%d\tabsolute\tтекущая\ta' % (NOW - 5 * H, NOW - 5 * H + 30 * 60000))
        w = E.resolve(short, epoch='1', now_ms=NOW, warmup_min=60)
        self.assertEqual(w.from_ms, NOW - 5 * H)
        self.assertTrue(w.warmup_skipped)
        self.assertIn('НЕ исключён', '\n'.join(w.header(short)))

    def test_restarts_in_window_first_observation_is_not_a_restart(self):
        boots = [NOW - 20 * H, NOW - 8 * H, NOW - 1 * H]
        self.assertEqual(E.restarts_in(boots, NOW - 30 * H, NOW), [NOW - 8 * H, NOW - 1 * H])
        self.assertEqual(E.restarts_in(boots, NOW - 5 * H, NOW), [NOW - 1 * H])
        self.assertEqual(E.restarts_in(None, 0, NOW), [])
        self.assertEqual(E.restarts_in([5], 0, NOW), [])

    def test_header_warns_about_restarts_and_unknown_boots(self):
        w = E.resolve(EP, epoch='2', now_ms=NOW, boots=[NOW - 20 * H, NOW - 8 * H, NOW - 4 * H])
        text = '\n'.join(w.header(EP))
        self.assertIn('ВНИМАНИЕ: окно пересекает 2 перезапуск', text)
        unknown = '\n'.join(E.resolve(EP, epoch='2', now_ms=NOW, boots=None).header(EP))
        self.assertIn('запуски ledger неизвестны', unknown)

    def test_load_boots(self):
        self.assertEqual(E.load_boots(lambda _: '1793000000000\n1793100000000\n'), [1793000000000, 1793100000000])
        import subprocess

        def boom(_):
            raise subprocess.CalledProcessError(1, 'psql')
        self.assertIsNone(E.load_boots(boom))

    def test_cli_default_warmup_is_sixty(self):
        a = E.add_cli_args(argparse.ArgumentParser()).parse_args([])
        self.assertEqual(a.warmup_min, 60)
        self.assertEqual(E.window_from_args(a, EP, now_ms=NOW).warmup_min, 60)


class BootsRefinementTests(unittest.TestCase):
    def test_negative_warmup_cannot_move_window_before_the_mark(self):
        w = E.resolve(EP, epoch='2', now_ms=NOW, warmup_min=-600)
        self.assertEqual(w.from_ms, NOW - 10 * H)
        self.assertEqual(w.warmup_min, 0)
        w = E.resolve(EP, now_ms=NOW, warmup_min=-600)
        self.assertEqual(w.from_ms, NOW - 2 * H)

    def test_cli_rejects_negative_warmup(self):
        p = E.add_cli_args(argparse.ArgumentParser())
        with self.assertRaises(SystemExit):
            with contextlib.redirect_stderr(io.StringIO()):
                p.parse_args(['--warmup-min', '-5'])
        with self.assertRaises(SystemExit):
            with contextlib.redirect_stderr(io.StringIO()):
                p.parse_args(['--warmup-min', 'abc'])

    def test_restart_marker_is_shifted_left_by_the_observation_lag(self):
        boots = [NOW - 20 * H, NOW - 5 * H]
        # запись о запуске сделана через 60 с после правого края окна: настоящий старт мог быть внутри окна
        self.assertEqual(E.restarts_in(boots, NOW - 9 * H, NOW - 5 * H - 60000), [NOW - 5 * H])
        # позже слэка — запуск точно после окна
        self.assertEqual(E.restarts_in(boots, NOW - 9 * H, NOW - 5 * H - 120000), [])

    def test_boot_started_by_absolute_reset_is_not_a_restart(self):
        at = NOW - 2 * H                                       # эпоха 3: absolute
        boots = [NOW - 20 * H, NOW - 8 * H, at + 30000]
        self.assertEqual(E.restarts_in(boots, at, NOW, EP), [])
        self.assertEqual(E.restarts_in(boots, NOW - 9 * H, NOW, EP), [NOW - 8 * H])   # перезапуск внутри bridge-эпохи остаётся
        # без эпох (как раньше) — виден
        self.assertEqual(E.restarts_in(boots, at, NOW), [at + 30000])
        # позже окна после метки — это уже настоящий перезапуск
        late = [NOW - 20 * H, at + 11 * 60000]
        self.assertEqual(E.restarts_in(late, at, NOW, EP), [at + 11 * 60000])
        # после bridge-метки запуск не освобождается
        self.assertEqual(E.restarts_in([NOW - 20 * H, NOW - 10 * H + 5000], NOW - 10 * H, NOW, EP), [NOW - 10 * H + 5000])

    def test_header_warns_when_window_starts_before_boots_are_known(self):
        w = E.resolve(EP, epoch='2', now_ms=NOW, boots=[NOW - 4 * H])
        self.assertEqual(w.boots_since, NOW - 4 * H)
        self.assertIn('запуски ledger известны только с', '\n'.join(w.header(EP)))
        ok = E.resolve(EP, epoch='2', now_ms=NOW, boots=[NOW - 20 * H, NOW - 4 * H])
        self.assertNotIn('известны только с', '\n'.join(ok.header(EP)))
        empty = E.resolve(EP, epoch='2', now_ms=NOW, boots=[])
        self.assertIn('ещё не наблюдались', '\n'.join(empty.header(EP)))
        self.assertIsNone(E.boots_known_since([]))


class TtlTests(unittest.TestCase):
    def test_warns_beyond_seven_days(self):
        buf = io.StringIO()
        self.assertTrue(E.warn_if_beyond_ttl(NOW - 8 * 24 * H, NOW, buf))
        self.assertIn('ClickHouse', buf.getvalue())
        self.assertFalse(E.warn_if_beyond_ttl(NOW - 6 * 24 * H, NOW, io.StringIO()))


if __name__ == '__main__':
    unittest.main(verbosity=1)
