# PM-NNN — &lt;title&gt;

> **Type**: process post-mortem (not service incident).
>
> **Status**: `draft` | `published` | `closed`
>
> **Discovered**: YYYY-MM-DD by &lt;who/what surfaced it&gt;.
>
> **Severity**: low | medium | high (impact on releases, contracts, money invariants).

## Symptoms

Observable, concrete facts. What was wrong, where, and what made it visible.
Include file paths, line numbers, command output snippets — anything reproducible.

## Timeline

Optional. Useful if drift happened over multiple PRs.

- YYYY-MM-DD — event 1
- YYYY-MM-DD — event 2
- YYYY-MM-DD — detection

## Root cause

Why was this allowed to happen by the process? Be specific:

- Which gate didn't exist or didn't catch it.
- Which document/code/spec didn't get updated when it should have.
- Which human assumption was wrong.

Not "we forgot" — "the checker that would have caught this didn't exist".

## Fix

Concrete actions (commits, PRs, ADRs) that remediated the drift. Each one should
be linkable.

## Lessons learned

What's now different in the process so this class of drift can't recur silently:

- New validator, hook, or CI gate.
- New documentation rule.
- New definition or checklist item.

## Related

- AUDIT or task that owns the fix (e.g. `AUDIT-001 T-AUDIT-002`).
- Affected features (F-XX).
- Affected ADRs.

## Open follow-ups

If the fix is partial, list what remains.
