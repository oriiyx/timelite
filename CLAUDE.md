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

The project has a library skeleton and internal Linux/macOS file I/O, not a
working database. Keep the internal file layer separate from the public API and
preserve the Windows library smoke build. Do not implement database storage
formats or queries unless the task calls for them.
