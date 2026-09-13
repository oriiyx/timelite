# Instructions for Claude

Read and follow [AGENTS.md](AGENTS.md) before working on this repository. It is
the shared source for project direction, C99 style, builds, and Git rules.

In particular:

- Work on a feature branch. Main receives changes through pull requests only.
- Preserve user edits and staged changes. Commit or push only when requested.
- Keep C simple, memory ownership explicit, and comments short and clear.
- Create or update docs/feature/NNN-short-name.md with the desired state, actual
  work, verification results, and remaining limits for each feature.
- Review and update both AGENTS.md and CLAUDE.md when accepted project decisions
  change. Keep shared rules in AGENTS.md so these files do not drift apart.
- Update the README when usage changes. Report only checks actually performed.
- Verify with the suite, not ad hoc commands: `make check` while iterating,
  `python3 tools/test.py run local` before handing back, and after
  `python3 tools/test.py prepare` once, `python3 tools/test.py run preflight`
  before a pull request. New C tests are one entry in tools/inventory.json.
  Once required checks pass for unchanged source, stop testing; rerun only for
  changes, failures, or a concrete coverage gap (see AGENTS.md).

The project has v1 lifecycle and a separate v2 sensor batch API with durable WAL
append, recovery, sequential reading over installed segments plus the WAL,
global timestamp order, segment seek and filtered time-range reads (feature 007), and
manual checkpointing with safe WAL reclamation (feature 006: segments behind a
two-slot generation manifest, WAL truncated only after the install is durable).
Feature 009 adds time-range count/minimum/maximum aggregation independent of
the read cursor. Feature 010 adds explicit whole-segment retention with durable
tail/front copies, recovery phases and preserved sequence/timestamp high-water
marks. Feature 005 is the repeatable testing suite. Feature 011 demonstrates the
application-controlled continuous-operation loop without adding automatic policy
or scheduling. Feature 012 is tools/inspect (build/timelite-inspect): `verify`
is read-only stdio parsing that predicts recovery, `status` uses the public API
and recovers on open; it never repairs and adds no public API. Keep format
knowledge in timelite.c; the tool's copied constants are checked by the
`inspect` test. Keep the internal native file layer separate from public APIs.
Preserve Windows library/backend tests and the batch model; Windows namespace
provisioning is unsupported and must return ENOTSUP without a weaker fallback.
Use AGENTS.md, feature 004, feature 006, feature 007 and feature 010 for the qualified durability, memory,
ownership and checkpoint contracts. Never treat an in-place overwrite as atomic. Link exactly one native backend. Report physical-device, Windows
runtime, CI, cross-build and simulated-interruption results separately.
