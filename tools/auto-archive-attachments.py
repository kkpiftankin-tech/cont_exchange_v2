#!/usr/bin/env python3
"""
UserPromptSubmit hook: автоматически сохраняет вложения в incoming-docs/.

Механика:
  Claude Code не хранит вложения как отдельные файлы — содержимое попадает
  в промт inline (как <document>...<source>...<document_content>... блоки).
  Этот хук парсит такие блоки и сохраняет каждое вложение на диск, чтобы
  incoming-docs/ оставался самодостаточным архивом.

Протокол:
  - stdin: JSON от Claude Code; поле "prompt" содержит текст промта.
  - stdout: JSON c hookSpecificOutput.additionalContext (отчёт ассистенту).
  - exit code: всегда 0 (хук не должен блокировать промт).

Поведение:
  - Каждый <document> сохраняется как incoming-docs/YYYY-MM-DD-<slug>-<hash8>.md.
  - <hash8> — первые 8 hex от sha1 содержимого, чтобы re-uploads одного файла
    не плодили дубликаты.
  - Префиксует файл двумя HTML-комментариями: дата авто-архивации и оригинальное
    имя source.
  - Если файл уже существует (тот же hash) — пропускает, отчитывается «(уже сохранён)».
  - При любой ошибке тихо выходит с 0; пропуск архивации не должен ломать чат.

Регистрация:
  .claude/settings.json (shared, AUDIT-001 T-AUDIT-004):
    "hooks": {
      "UserPromptSubmit": [
        {"hooks": [{"type": "command",
                    "command": "python3 \\"$CLAUDE_PROJECT_DIR/tools/auto-archive-attachments.py\\""}]}
      ]
    }

Self-test:
  python3 tools/auto-archive-attachments.py --self-test
  Возвращает exit 0 если parser корректно извлекает <document> блоки,
  exit 1 при поломке regex / API. CI запускает это smoke-test'ом, чтобы
  ловить regression раньше пользователей.
"""

import datetime
import hashlib
import json
import os
import pathlib
import re
import sys

DOCUMENT_PATTERN = re.compile(
    r"<(?:antml:)?document\b[^>]*>"
    r"\s*<(?:antml:)?source>(?P<source>[^<]+)</(?:antml:)?source>"
    r"\s*<(?:antml:)?document_content>(?P<content>.*?)</(?:antml:)?document_content>"
    r"\s*</(?:antml:)?document>",
    re.DOTALL,
)


def slugify(name: str, max_len: int = 60) -> str:
    """Sanitize a filename into a filesystem-safe slug, preserving cyrillic."""
    stem = pathlib.Path(name).stem
    # Replace whitespace and path-unfriendly chars with single dash.
    slug = re.sub(r"[\s/\\:*?\"<>|]+", "-", stem).strip("-")
    return slug[:max_len] or "untitled"


def archive_documents(prompt: str, repo_root: pathlib.Path) -> list[str]:
    """Extract <document> blocks from prompt and persist each. Returns report lines."""
    archive_dir = repo_root / "incoming-docs"
    archive_dir.mkdir(parents=True, exist_ok=True)

    date = datetime.date.today().isoformat()
    saved: list[str] = []

    for match in DOCUMENT_PATTERN.finditer(prompt):
        source = match.group("source").strip()
        content = match.group("content")
        if not content.strip():
            continue

        slug = slugify(source)
        digest = hashlib.sha1(content.encode("utf-8", errors="replace")).hexdigest()[:8]
        target = archive_dir / f"{date}-{slug}-{digest}.md"
        rel = target.relative_to(repo_root)

        if target.exists():
            saved.append(f"(уже сохранён) {rel}")
            continue

        header = (
            f"<!-- auto-archived by UserPromptSubmit hook on {date} -->\n"
            f"<!-- original source filename: {source} -->\n"
            f"<!-- content sha1: {digest} -->\n\n"
        )
        target.write_text(header + content.strip() + "\n", encoding="utf-8")
        saved.append(f"created {rel}")

    return saved


SELF_TEST_PROMPT = """\
Lorem ipsum
<document index="1" media_type="text/markdown">
<source>self-test-sample.md</source>
<document_content>
# Self-test document
Hello, world.
</document_content>
</document>
Trailing text.
"""


def run_self_test() -> int:
    """Smoke-test parser without touching the filesystem.

    Runs the regex against a known prompt and confirms that exactly
    one block is matched with the expected source/content. Returns 0
    on success, 1 on any mismatch (CI fails on non-zero).
    """
    matches = list(DOCUMENT_PATTERN.finditer(SELF_TEST_PROMPT))
    if len(matches) != 1:
        sys.stderr.write(
            f"[auto-archive-attachments] self-test FAIL: "
            f"expected 1 match, got {len(matches)}\n"
        )
        return 1
    m = matches[0]
    if m.group("source").strip() != "self-test-sample.md":
        sys.stderr.write(
            f"[auto-archive-attachments] self-test FAIL: source mismatch: "
            f"{m.group('source')!r}\n"
        )
        return 1
    if "Hello, world." not in m.group("content"):
        sys.stderr.write(
            "[auto-archive-attachments] self-test FAIL: content mismatch\n"
        )
        return 1

    # Verify slug + digest stable for sanity.
    slug = slugify(m.group("source").strip())
    digest = hashlib.sha1(m.group("content").encode("utf-8")).hexdigest()[:8]
    if slug != "self-test-sample":
        sys.stderr.write(
            f"[auto-archive-attachments] self-test FAIL: slug={slug!r}\n"
        )
        return 1
    if len(digest) != 8:
        sys.stderr.write(
            f"[auto-archive-attachments] self-test FAIL: digest={digest!r}\n"
        )
        return 1
    print("[auto-archive-attachments] self-test OK")
    return 0


def main() -> int:
    if "--self-test" in sys.argv[1:]:
        return run_self_test()

    try:
        payload = json.load(sys.stdin)
    except Exception:
        return 0

    prompt = payload.get("prompt") or ""
    if not isinstance(prompt, str) or "<document" not in prompt:
        return 0

    repo_root = pathlib.Path(
        os.environ.get("CLAUDE_PROJECT_DIR") or os.getcwd()
    ).resolve()

    try:
        saved = archive_documents(prompt, repo_root)
    except Exception as exc:
        # Don't block prompt on any internal error.
        sys.stderr.write(f"[auto-archive-attachments] error: {exc}\n")
        return 0

    if not saved:
        return 0

    lines = ["Auto-archived attachments → `incoming-docs/`:"]
    lines += [f"  - {line}" for line in saved]
    lines.append("")
    lines.append(
        "Ingestion pipeline: `.claude/skills/ingest-docs/SKILL.md`. "
        "Если нужно зарегистрировать как `IN-NNN` (meta + fragment-map), "
        "сделайте это явно по запросу пользователя."
    )
    additional = "\n".join(lines)

    output = {
        "hookSpecificOutput": {
            "hookEventName": "UserPromptSubmit",
            "additionalContext": additional,
        }
    }
    print(json.dumps(output, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
