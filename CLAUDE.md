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

The project has v1 lifecycle and a separate v2 sensor batch API with durable WAL
append, recovery and sequential reading. Checkpointing and WAL reclamation belong
to feature 005. Keep the internal native file layer separate from public APIs.
Preserve Windows library/backend tests and the batch model; Windows namespace
provisioning is unsupported and must return ENOTSUP without a weaker fallback.
Use AGENTS.md and feature 004 for the qualified durability, memory and ownership
contracts. Link exactly one native backend. Report physical-device, Windows
runtime, CI, cross-build and simulated-interruption results separately.
