#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""epoch-guard-checker — проверка «анализа метки сброса» (ADR-090 п. 6, CR-F18-018).

Метка сброса позиций (эпоха) обязательна для всех, кто читает историю: окно разбора не должно молча смешивать интервалы до и после
сброса. Проверка не заменяет ревью, но делает пропуск заметным:

  A. Каждая действующая таблица (Postgres и ClickHouse), которую создаёт init.sql или код, отнесена к классу в
     specs/domain/epoch-classes.yaml. Неклассифицированная таблица — нарушение.
  B. Прямое чтение таблицы класса B (журнал; `FROM`/`JOIN <таблица>`) в frontend/api/*.js и tools/** должно сопровождаться пометкой
     `epoch-guard: window` (окно получено помощником эпох) или `epoch-guard: ok(причина)` в тех же строках или в 12 строках выше;
     файл целиком можно пометить `epoch-guard-file: ok(причина)` в первых 30 строках. Чтение без пометки — нарушение.
     Класс A (текущее состояние) читать можно без пометки: «окна до сброса» у него не существует.
  C. Запись реестра о таблице, которой нет в DDL, — предупреждение (кроме `ddl: external`).

Режимы: по умолчанию (report) печатает нарушения и завершается с кодом 0; `--strict` завершается с кодом 1 при нарушениях.
До этапа Э3 CR-F18-018 используется режим report.  Не перехватывает ручной `docker exec psql`: мера — представление ce_epoch_intervals
и runbook «Абсолютный сброс и анализ эпохи».

Запуск:  python3 tools/epoch-guard-checker/check.py [--strict] [--root .]
"""
import argparse
import os
import re
import sys

import yaml

READ_RE = re.compile(r'\b(?:FROM|JOIN)\s+(?:default\.)?([a-z_][a-z0-9_]*)\b', re.IGNORECASE)
DDL_RE = re.compile(r'CREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?(?:default\.)?([a-z_][a-z0-9_]*)\s*\(', re.IGNORECASE)
MARKER_RE = re.compile(r'epoch-guard:\s*(?:window|ok\([^)]+\))')
FILE_MARKER_RE = re.compile(r'epoch-guard-file:\s*ok\([^)]+\)')
LOOKBACK = 12
COMMENT_PREFIXES = ('//', '--', '#', '*', '/*')
SKIP_TABLE_RE = re.compile(r'(_bak_|_backup_|^tmp_|_test$)')          # служебные копии и временные таблицы — вне реестра
DDL_SOURCES = [('infra/postgres', ('.sql',)), ('infra/clickhouse', ('.sql',)), ('frontend/api', ('.js',)), ('cpp', ('.cpp', '.hpp')),
               ('tools', ('.sh', '.py', '.sql')), ('docker', ('.sql', '.sh'))]
READ_SOURCES = [('frontend/api', ('.js',)), ('tools', ('.py', '.sql', '.sh', '.js'))]
SKIP_DIRS = {'node_modules', '__pycache__', 'tests', 'testdata', 'epoch-guard-checker', 'lib', '.git'}


def load_registry(root):
    with open(os.path.join(root, 'specs/domain/epoch-classes.yaml'), encoding='utf-8') as f:
        data = yaml.safe_load(f)['epochRegistry']
    classes = {}
    for engine, tables in data['tables'].items():
        for name, spec in (tables or {}).items():
            classes[name] = dict(spec, engine=engine)
    return classes


def walk(root, sources, include_tests=False):
    for sub, exts in sources:
        base = os.path.join(root, sub)
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for fn in filenames:
                if fn.endswith(exts) and (include_tests or not fn.endswith('.test.js')):
                    yield os.path.join(dirpath, fn)


def discover_tables(root):
    """Имена таблиц, создаваемых DDL в репозитории (кроме служебных копий)."""
    found = {}
    for path in walk(root, DDL_SOURCES):
        try:
            text = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        for m in DDL_RE.finditer(text):
            name = m.group(1).lower()
            if not SKIP_TABLE_RE.search(name):
                found.setdefault(name, os.path.relpath(path, root))
    return found


def unguarded_reads(root, registry):
    """[(файл, строка, таблица)]: чтение таблицы класса B (журнал) без пометки epoch-guard."""
    findings = []
    watched = {n for n, s in registry.items() if s.get('class') == 'B'}
    for path in walk(root, READ_SOURCES):
        try:
            lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
        except OSError:
            continue
        if any(FILE_MARKER_RE.search(l) for l in lines[:30]):
            continue
        for i, line in enumerate(lines):
            if line.lstrip().startswith(COMMENT_PREFIXES):
                continue   # упоминание в комментарии — не чтение
            for m in READ_RE.finditer(line):
                table = m.group(1).lower()
                if table not in watched:
                    continue
                window = lines[max(0, i - LOOKBACK): i + 1]
                if not any(MARKER_RE.search(l) for l in window):
                    findings.append((os.path.relpath(path, root), i + 1, table))
    return findings


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--strict', action='store_true', help='код возврата 1 при нарушениях (по умолчанию только отчёт)')
    ap.add_argument('--root', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    args = ap.parse_args(argv)
    root = os.path.abspath(args.root)
    registry = load_registry(root)
    found = discover_tables(root)

    unclassified = sorted(n for n in found if n not in registry)
    stale = sorted(n for n, s in registry.items() if n not in found and s.get('ddl') != 'external' and s.get('class') != 'E')
    reads = unguarded_reads(root, registry)

    print('epoch-guard-checker: таблиц в реестре %d, найдено в DDL %d' % (len(registry), len(found)))
    for n in unclassified:
        print('НАРУШЕНИЕ  таблица %s (%s) не отнесена к классу в specs/domain/epoch-classes.yaml' % (n, found[n]))
    for f, ln, t in reads:
        print('НАРУШЕНИЕ  %s:%d читает %s (класс %s) без пометки `epoch-guard:`' % (f, ln, t, registry[t]['class']))
    for n in stale:
        print('предупреждение  в реестре таблица %s, но DDL в репозитории не найден (добавьте `ddl: external`, если создаётся вне репозитория)' % n)
    total = len(unclassified) + len(reads)
    print('итого нарушений: %d (неклассифицированных таблиц %d, чтений без пометки %d), предупреждений: %d; режим %s'
          % (total, len(unclassified), len(reads), len(stale), 'strict' if args.strict else 'report'))
    return 1 if (args.strict and total) else 0


if __name__ == '__main__':
    sys.exit(main())
