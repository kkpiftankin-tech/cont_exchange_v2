#!/usr/bin/env python3
"""Извлекает расчётное ядро HE (пороги хеджа) из архивной страницы входящего документа
`incoming-docs/2026-10-29-porogi-hedzha5-380dc48d.html` в `he_engine.js` (без правок кода ядра).

    python3 tools/hedge-threshold-model/extract_engine.py

Зачем отдельный файл: страница — неизменяемый архив, а для эталонных тестов C++ порта
(`gen_golden.js` → `cpp/ledger/tests/hedge_threshold_model_golden.inc`) и для воспроизведения чисел
экспертом нужен исполняемый модуль. Ядро берётся дословно между `var HE = (function` и `var CE2 =`.
"""
import hashlib
import io
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "incoming-docs" / "2026-10-29-porogi-hedzha5-380dc48d.html"
OUT = pathlib.Path(__file__).resolve().parent / "he_engine.js"


def main() -> int:
    raw = SRC.read_bytes()
    sha8 = hashlib.sha1(raw).hexdigest()[:8]
    if sha8 != "380dc48d":
        print("контрольная сумма архива не совпала: %s" % sha8, file=sys.stderr)
        return 1
    text = raw.decode("utf-8")
    scripts = re.findall(r"<script>(.*?)</script>", text, re.S)
    body = scripts[0]
    i0 = body.index("var HE = (function")
    i1 = body.index("var CE2 = (function")
    engine = body[i0:i1].rstrip() + "\n"
    header = (
        "// Расчётное ядро порогов хеджа HE — дословная выборка из incoming-docs/2026-10-29-porogi-hedzha5-380dc48d.html\n"
        "// (sha1-8 380dc48d). Не править: перегенерируется extract_engine.py. Единицы: позиция z — тыс. USDT,\n"
        "// время — минуты, деньги — USDT; cm, ct, ctr — USDT на 1 тыс. USDT оборота (= 0,1 × bps); K — USDT.\n"
    )
    io.open(OUT, "w", encoding="utf-8").write(header + engine + "\nmodule.exports = HE;\n")
    print("записано %s (%d строк ядра)" % (OUT.relative_to(ROOT), engine.count("\n")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
