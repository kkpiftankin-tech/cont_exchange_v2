# -*- coding: utf-8 -*-
"""Метка сброса позиций (эпоха) для скриптов разбора (ADR-090 п. 6, CR-F18-018, Э1).

Сброс создаёт запись в ce_reset_epoch (номер, момент, кто, название эксперимента). Любой разбор истории обязан знать, к какой эпохе
относится его окно, и не смешивать интервалы до и после сброса. Эта библиотека — единственное место, где скрипты сопоставляют окно с
метками: она САМА отказывается строить окно через метку (CrossesResetError), если вызывающий явно не разрешил это и не назвал причину.
Эпоха 0 виртуальная («до первой метки»); эпоха N занимает полуоткрытый интервал [at_N, at_{N+1}).

Запуск скриптов — на dev-хосте (читают Postgres через `docker exec`). Подключение из скрипта:

    import os, sys
    sys.path.insert(0, os.environ.get('CE_TOOLS_LIB') or '/home/nik/cont_exchange_v2/tools/lib')
    import epochs as E

Стандартные параметры командной строки добавляет add_cli_args(parser); окно строит window_from_args(args, epochs, ...).
"""
import subprocess
import sys
import time

HOUR_MS = 3600 * 1000
CH_TTL_DAYS = 7                       # срок хранения ClickHouse-аналитики (ADR-090, OQ-F18v2-64)
VIRTUAL = {'epoch': 0, 'at_ms': 0, 'kind': 'virtual', 'note': '', 'actor': ''}

_SQL = ("SELECT epoch_id, (EXTRACT(EPOCH FROM reset_at)*1000)::bigint, kind, note, actor "
        "FROM ce_reset_epoch ORDER BY epoch_id")


class EpochsUnavailable(RuntimeError):
    """Метки недоступны (таблицы нет или Postgres не ответил): окно проверить нельзя, молча продолжать нельзя."""


class CrossesResetError(RuntimeError):
    """Окно содержит метку сброса: интервалы разных эпох смешивать нельзя без явного разрешения."""

    def __init__(self, markers, from_ms, to_ms):
        self.markers = list(markers)
        self.from_ms = from_ms
        self.to_ms = to_ms
        super().__init__(
            'окно %d..%d мс пересекает %d метк(и) сброса (%s): интервалы до и после сброса нельзя смешивать. '
            'Выберите одну эпоху (--epoch N | current) или разрешите явно: --allow-cross-reset --cross-reason "причина".'
            % (from_ms, to_ms, len(self.markers), ', '.join(str(m) for m in self.markers)))


def _psql(sql):
    return subprocess.run(['docker', 'exec', '-i', 'infra-postgres-1', 'psql', '-U', 'cex', '-d', 'cex', '-At', '-F', '\t', '-c', sql],
                          capture_output=True, text=True, check=True).stdout


def parse_epochs(text):
    """Строки «номер<TAB>момент_мс<TAB>вид<TAB>название<TAB>автор» → отсортированный список эпох. Порча порядка — ошибка, а не догадка."""
    out = []
    for line in str(text).splitlines():
        p = line.split('\t')
        if len(p) < 2 or not p[0].strip().isdigit():
            continue
        out.append({'epoch': int(p[0]), 'at_ms': int(float(p[1])), 'kind': p[2] if len(p) > 2 else '',
                    'note': p[3] if len(p) > 3 else '', 'actor': p[4] if len(p) > 4 else ''})
    out.sort(key=lambda e: e['epoch'])
    for i in range(1, len(out)):
        if not (out[i]['at_ms'] > out[i - 1]['at_ms']):
            raise EpochsUnavailable('метки сброса испорчены: время эпохи %d не больше времени эпохи %d' % (out[i]['epoch'], out[i - 1]['epoch']))
    return out


def load_epochs(psql=None):
    """Список эпох (может быть пустым — меток ещё не было). psql(sql) → текст; по умолчанию docker exec на dev-хосте."""
    run = psql or _psql
    try:
        return parse_epochs(run(_SQL))
    except subprocess.CalledProcessError as e:
        raise EpochsUnavailable('не удалось прочитать ce_reset_epoch (BFF создаёт таблицу при первом запуске после Э1): %s'
                                % ((e.stderr or str(e)).strip()[:200],))
    except FileNotFoundError as e:
        raise EpochsUnavailable('нет docker/psql на этой машине (скрипты запускаются на dev-хосте): %s' % e)


_SQL_BOOTS = "SELECT (EXTRACT(EPOCH FROM first_seen_at)*1000)::bigint FROM ce_ledger_boot ORDER BY first_seen_at"


def load_boots(psql=None):
    """Моменты запусков ledger (мс; первое наблюдение BFF каждого boot_id, по возрастанию) или None, если таблицы нет (OQ-F18v2-63).
    Первая запись — первое наблюдение, а не перезапуск; перезапуски — со второй."""
    run = psql or _psql
    try:
        out = run(_SQL_BOOTS)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    return [int(float(l.split('\t')[0])) for l in str(out).splitlines() if l.strip()]


BOOT_SLACK_MS = 90 * 1000                   # BFF видит запуск позже его настоящего старта (опрос 60 с + кэш 30 с)
ABSOLUTE_BOOT_GRACE_MS = 10 * 60 * 1000     # запуск ledger, начатый абсолютным сбросом, — часть сброса, а не перезапуск посреди эпохи


def restarts_in(boots, from_ms, to_ms, epochs=None, slack_ms=BOOT_SLACK_MS):
    """Перезапуски ledger внутри окна (from, to]: остатки площадок в памяти ledger при них обнулялись.
    Метка запуска — момент первого наблюдения, он позже настоящего старта до slack_ms, поэтому окно, кончающееся в этот промежуток, тоже считается
    пересекшим перезапуск. Запуск в первые 10 мин после метки kind='absolute' (его поднимает сам скрипт сброса) перезапуском не считается."""
    absolute = [e['at_ms'] for e in (epochs or []) if e.get('kind') == 'absolute']
    return [t for t in (boots or [])[1:]
            if from_ms < t and t - slack_ms <= to_ms and not any(a <= t <= a + ABSOLUTE_BOOT_GRACE_MS for a in absolute)]


def boots_known_since(boots):
    """Момент, с которого запуски ledger известны (первое наблюдение BFF), или None. Окно, начавшееся раньше, может содержать неизвестные перезапуски."""
    return boots[0] if boots else None


def current(epochs):
    return epochs[-1] if epochs else VIRTUAL


def find(epochs, n):
    if n == 0:
        return VIRTUAL
    for e in epochs:
        if e['epoch'] == n:
            return e
    return None


def epoch_at(epochs, ts_ms):
    """Номер эпохи момента ts_ms: наибольшая at_ms <= ts_ms; раньше первой метки — 0."""
    ans = 0
    for e in epochs:
        if e['at_ms'] <= ts_ms:
            ans = e['epoch']
        else:
            break
    return ans


def intervals(epochs):
    """[(номер, from_ms, to_ms|None)]: виртуальная 0, затем эпохи по меткам; у текущей правой границы нет."""
    out = [(0, 0, epochs[0]['at_ms'] if epochs else None)]
    for i, e in enumerate(epochs):
        out.append((e['epoch'], e['at_ms'], epochs[i + 1]['at_ms'] if i + 1 < len(epochs) else None))
    return out


class Window(object):
    """Результат resolve(): границы выборки и сведения об эпохах для шапки отчёта."""

    def __init__(self, from_ms, to_ms, epochs_in, markers, crosses, reason, requested, warmup_min=0, warmup_skipped=False, restarts=None, boots_known=True,
                 boots_since=None):
        self.warmup_min = warmup_min            # исключённый прогрев эпохи, мин (0 — не исключался)
        self.warmup_skipped = warmup_skipped    # прогрев не исключён, потому что окно не длиннее прогрева
        self.restarts = restarts or []          # перезапуски ledger в окне (мс)
        self.boots_known = boots_known          # False — таблицы запусков нет, перезапуски неизвестны
        self.boots_since = boots_since          # момент, с которого запуски известны (мс) или None
        self.from_ms = from_ms
        self.to_ms = to_ms
        self.epochs = epochs_in            # эпохи, которых касается окно
        self.markers = markers             # метки внутри окна
        self.crosses_reset = crosses
        self.cross_reason = reason
        self.requested = requested         # как окно было задано: 'epoch=N' | 'current' | 'explicit'

    def header(self, all_epochs):
        """Строки шапки отчёта: номер эпохи, момент, название эксперимента и признак пересечения метки."""
        fmt = lambda ms: time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(ms / 1000.0)) + ' UTC' if ms else '—'
        lines = ['epoch_id=%s  (текущая в базе: %d)  окно: %s .. %s  [%s]' % (
            ','.join(str(e['epoch']) for e in self.epochs) or '0', current(all_epochs)['epoch'], fmt(self.from_ms), fmt(self.to_ms), self.requested)]
        for e in self.epochs:
            lines.append('  эпоха %d: с %s, вид=%s, «%s», автор=%s' % (e['epoch'], fmt(e['at_ms']), e['kind'] or '—', e['note'], e['actor'] or '—'))
        lines.append('crosses_reset=%s%s' % ('true' if self.crosses_reset else 'false',
                                             ('  причина: ' + self.cross_reason) if self.crosses_reset else ''))
        if self.warmup_min:
            lines.append('прогрев модели порогов: первые %d мин эпохи исключены (--warmup-min 0, чтобы включить)' % self.warmup_min)
        elif self.warmup_skipped:
            lines.append('прогрев модели порогов НЕ исключён: окно не длиннее прогрева')
        if self.restarts:
            lines.append('ВНИМАНИЕ: окно пересекает %d перезапуск(ов) ledger (%s): остатки площадок в памяти ledger при них обнулялись, '
                         'позиции остались — сравнение до и после перезапуска некорректно' % (
                             len(self.restarts), ', '.join(time.strftime('%m-%d %H:%M', time.gmtime(t / 1000.0)) for t in self.restarts)))
        if not self.boots_known:
            lines.append('запуски ledger неизвестны (таблицы ce_ledger_boot нет): перезапуски в окне проверить нельзя')
        elif self.boots_since is None:
            lines.append('запуски ledger ещё не наблюдались (ce_ledger_boot пуста): перезапуски в окне проверить нельзя')
        elif self.from_ms < self.boots_since:
            lines.append('ВНИМАНИЕ: запуски ledger известны только с %s — перезапуски в начале окна могли остаться незамеченными' % fmt(self.boots_since))
        return lines


def resolve(epochs, from_ms=None, to_ms=None, epoch=None, allow_cross=False, reason=None, now_ms=None, warmup_min=0, boots=None):
    """Окно разбора с проверкой метки.

    epoch: None | 'current' | 'all' | номер. Без epoch и без from/to — текущая эпоха до сейчас.
    from/to заданы явно: если внутри окна есть метка и allow_cross не разрешено — CrossesResetError.
    allow_cross разрешает пересечение ТОЛЬКО вместе с непустой причиной (она попадает в шапку отчёта).
    """
    now = int(now_ms if now_ms is not None else time.time() * 1000)
    warmup_min = max(0, int(warmup_min or 0))   # отрицательный прогрев сдвинул бы начало окна ДО метки, мимо проверки пересечения
    cur = current(epochs)
    if epoch is not None and (from_ms is not None or to_ms is not None):
        raise ValueError('--epoch нельзя сочетать с явными START/END')
    requested = 'explicit'
    if epoch is None and from_ms is None and to_ms is None:
        epoch = 'current'
    if epoch is not None:
        if epoch == 'current':
            requested, f, t = 'current', cur['at_ms'], now
        elif epoch == 'all':
            requested, f, t = 'all', (epochs[0]['at_ms'] if epochs else 0), now
        else:
            n = int(epoch)
            e = find(epochs, n)
            if e is None:
                raise ValueError('эпохи %d нет (есть: %s)' % (n, ', '.join(str(x['epoch']) for x in epochs) or 'меток не было'))
            requested, f = 'epoch=%d' % n, e['at_ms']
            nxt = [w for w in intervals(epochs) if w[0] == n][0][2]
            t = nxt if nxt is not None else now
        from_ms, to_ms = f, t
    else:
        from_ms = int(from_ms) if from_ms is not None else 0
        to_ms = int(to_ms) if to_ms is not None else now
        if not (to_ms > from_ms):
            raise ValueError('END должен быть больше START')
    markers = [e['at_ms'] for e in epochs if from_ms < e['at_ms'] < to_ms]
    crosses = len(markers) > 0
    if requested == 'all' and len(epochs) > 0:
        crosses = len(epochs) > 1 or crosses
    if crosses:
        if not allow_cross:
            raise CrossesResetError(markers or [e['at_ms'] for e in epochs], from_ms, to_ms)
        if not (reason or '').strip():
            raise ValueError('--allow-cross-reset требует --cross-reason "причина": она попадает в шапку отчёта')
    touched = [VIRTUAL if w[0] == 0 else find(epochs, w[0]) for w in intervals(epochs)
               if (w[2] is None or w[2] > from_ms) and w[1] < to_ms]
    # Прогрев (OQ-F18v2-62): первый час эпохи модель порогов копит оценки потока — несопоставим с остальным временем. Исключается по умолчанию
    # только когда окно задано эпохой (current / N), не для явных START..END и не для «all».
    applied, skipped = 0, False
    if warmup_min and requested != 'explicit' and requested != 'all' and touched and touched[0]['epoch'] > 0:
        shifted = max(from_ms, touched[-1]['at_ms'] if requested == 'current' else touched[0]['at_ms']) + warmup_min * 60000
        if shifted < to_ms:
            from_ms, applied = shifted, warmup_min
        else:
            skipped = True
    restarts = restarts_in(boots, from_ms, to_ms, epochs)
    return Window(from_ms, to_ms, touched, markers, crosses, (reason or '').strip(), requested, applied, skipped, restarts, boots is not None,
                  boots_known_since(boots))


def split_by_epoch(rows, ts_ms_of, epochs):
    """Строки → {номер эпохи: [строки]} (для разбора по эпохам, когда пересечение разрешено)."""
    out = {}
    for r in rows:
        out.setdefault(epoch_at(epochs, ts_ms_of(r)), []).append(r)
    return out


def warn_if_beyond_ttl(from_ms, now_ms=None, stream=None):
    """ClickHouse хранит аналитику 7 суток: окно, начавшееся раньше, неполно по таблицам ClickHouse (OQ-F18v2-64)."""
    now = int(now_ms if now_ms is not None else time.time() * 1000)
    if now - from_ms > CH_TTL_DAYS * 24 * HOUR_MS:
        (stream or sys.stderr).write('ВНИМАНИЕ: окно начинается раньше %d суток назад — таблицы ClickHouse (execution_venue, venue_snapshots, ...) за этот '
                                     'период усечены сроком хранения, разбор неполон (OQ-F18v2-64).\n' % CH_TTL_DAYS)
        return True
    return False


def _nonneg_int(text):
    import argparse
    try:
        n = int(text)
    except ValueError:
        n = -1
    if n < 0:
        raise argparse.ArgumentTypeError('нужно целое число минут >= 0, получено %r' % (text,))
    return n


def add_cli_args(parser):
    """Единые параметры окна для всех скриптов разбора."""
    parser.add_argument('start', nargs='?', type=int, help='начало окна, мс (необязательно; без start/end — текущая эпоха)')
    parser.add_argument('end', nargs='?', type=int, help='конец окна, мс (необязательно)')
    parser.add_argument('--epoch', default=None, help='current | all | номер эпохи (целиком); нельзя вместе с START/END')
    parser.add_argument('--allow-cross-reset', action='store_true', help='разрешить окну пересекать метку сброса (нужна --cross-reason)')
    parser.add_argument('--cross-reason', default='', help='причина смешения эпох; попадает в шапку отчёта')
    parser.add_argument('--warmup-min', type=_nonneg_int, default=60, help='исключить первые N минут эпохи (прогрев оценок модели порогов); 0 — не исключать; '
                        'действует, когда окно задано эпохой (по умолчанию текущая или --epoch N)')
    return parser


def window_from_args(args, epochs, now_ms=None, boots=None):
    return resolve(epochs, from_ms=args.start, to_ms=args.end, epoch=args.epoch, allow_cross=args.allow_cross_reset,
                   reason=args.cross_reason, now_ms=now_ms, warmup_min=args.warmup_min, boots=boots)
