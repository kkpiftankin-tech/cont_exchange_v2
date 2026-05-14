# Document Ingestion Methodology

## Purpose

This document defines how incoming documents are transformed into structured docs-as-code artifacts.

Incoming documents are not copied as monolithic files. They are decomposed into fragments, classified, mapped, normalized, inserted, linked, validated, and finally transformed into implementation tasks.

## Pipeline

1. Register source document.
2. Preserve source document in `incoming-docs/`.
3. Create metadata file.
4. Segment source document into fragments.
5. Classify fragments.
6. Map fragments to target artifacts.
7. Normalize fragments using repository templates.
8. Insert or merge into target documentation.
9. Create bidirectional links.
10. Update traceability.
11. Validate coverage.
12. Create implementation tasks.
13. Stop before code generation.

## Source Document Rule

`incoming-docs/` is an immutable archive.

Never edit original incoming files. If user supplies a corrected version, register it as a new entry (`IN-NNN`) and link it from the previous entry's meta file.

## Fragment Rule

Every meaningful section of an incoming document must receive a fragment ID.

Fragment ID format:

`IN-XXX-FR-YYY`

Example:

`IN-001-FR-007`

## Target Artifact Rule

Every fragment must map to at least one target artifact.

If a fragment cannot be mapped, mark it as:

`unmapped-needs-review`

## Merge Rule

If the target artifact exists, merge instead of overwriting.

## Conflict Rule

If the incoming fragment contradicts existing documentation, add `Conflict Notes`.

If the conflict is architectural, create an ADR in `docs/03-architecture/adr/`.

## Code Rule

Do not generate code directly from incoming documents.

Generate implementation tasks only after documentation and traceability are complete.

## Related

- Skill: [`.claude/skills/ingest-docs/SKILL.md`](../../.claude/skills/ingest-docs/SKILL.md)
- Repository map: [repository-map.md](repository-map.md)
- Sequence diagram rules: [sequence-diagram-rules.md](sequence-diagram-rules.md)
- Artifact templates: [artifact-templates.md](artifact-templates.md)
- Traceability: [../traceability/](../traceability/)
- Implementation plan: [../implementation-plan/](../implementation-plan/)
