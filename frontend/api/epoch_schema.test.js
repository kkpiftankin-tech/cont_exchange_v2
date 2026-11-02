// Self-contained node-тест: `node epoch_schema.test.js` (у frontend/api нет jest).
// Статические проверки схемы метки сброса. Поведение на настоящем Postgres проверяет testdata/epoch_schema_check.sh.
const fs = require('fs');
const path = require('path');
const { ALL_STATEMENTS, sqlScript } = require('./epoch_schema');

let failed = 0;
function check(name, cond) {
  if (cond) { console.log('[OK] ' + name); } else { console.error('[FAIL] ' + name); failed++; }
}
const norm = (s) => String(s).replace(/\s+/g, ' ').trim();

const initPath = path.join(__dirname, '..', '..', 'infra', 'postgres', 'init.sql');
const init = fs.existsSync(initPath) ? norm(fs.readFileSync(initPath, 'utf8')) : null;

check('есть операторы схемы', ALL_STATEMENTS.length >= 8);
check('каждый оператор — только создание (CREATE ... IF NOT EXISTS / OR REPLACE) или DO-блок, без ALTER/DROP/DELETE/TRUNCATE-операторов верхнего уровня',
  ALL_STATEMENTS.every((s) => /^(CREATE (UNIQUE )?(INDEX IF NOT EXISTS|TABLE IF NOT EXISTS|OR REPLACE)|DO \$\$)/.test(s.trim())));
check('ни один оператор не меняет существующие таблицы (ALTER TABLE отсутствует)', !ALL_STATEMENTS.some((s) => /\bALTER TABLE\b/i.test(s)));
check('есть функция выдачи номера, эпоха по времени, текущая эпоха, окна, досборка',
  ['ce_epoch_open', 'ce_epoch_at', 'ce_current_epoch', 'ce_epoch_windows', 'ce_epoch_backfill_legacy', 'ce_epoch_intervals'].every((n) => ALL_STATEMENTS.some((s) => s.includes(n))));
check('таблица защищена от изменения (триггеры на UPDATE/DELETE и TRUNCATE)', ALL_STATEMENTS.join('\n').includes('BEFORE INSERT OR UPDATE OR DELETE') && ALL_STATEMENTS.join('\n').includes('BEFORE TRUNCATE'));
check('название эксперимента ограничено 1-200 знаков на уровне таблицы', ALL_STATEMENTS[0].includes('BETWEEN 1 AND 200'));
check('вид метки ограничен списком', ALL_STATEMENTS[0].includes("('absolute', 'bridge', 'legacy')"));
check('sqlScript: каждый оператор завершён точкой с запятой', sqlScript().trimEnd().endsWith(';') && sqlScript().split('\n\n').length === ALL_STATEMENTS.length);

if (init === null) {
  console.log('[SKIP] init.sql не найден (запуск вне репозитория) — сверка пропущена');
} else {
  check('init.sql содержит схему метки', init.includes('ce_reset_epoch'));
  const missing = ALL_STATEMENTS.filter((s) => !init.includes(norm(s)));
  check('init.sql содержит КАЖДЫЙ оператор epoch_schema.js дословно (с точностью до пробелов) — иначе расхождение нового тома и живого', missing.length === 0);
  if (missing.length) console.error('нет в init.sql:\n' + missing.map((s) => '  ' + norm(s).slice(0, 100)).join('\n'));
}

if (failed) { console.error(`\n${failed} проверок не прошли`); process.exit(1); }
console.log('\nвсе проверки epoch_schema.js прошли');
