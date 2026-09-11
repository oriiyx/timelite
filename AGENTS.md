# Instructions for coding agents

## Project

Timelite is a small embedded time-series database project written in C99.
The current code is only a buildable library skeleton. Storage and queries are
future work. Read README.md and the relevant docs/feature notes before editing.
The earlier research is in research/RESEARCH.md; it contains proposals, not all
of which have been accepted. These instructions describe the current direction.

Target devices include x86 and ARM32. Confirm the exact CPU, OS, and ABI before
claiming support. A desktop build does not prove that the device build works.
Rust is excluded as the core implementation language.

## Code style

Use simple, explicit C99 in the style requested by the user:

- Use plain structs, functions, loops, and explicit control flow.
- Use four spaces, braces on their own lines, and snake_case names.
- Prefix public names with timelite_. Keep private functions static.
- Use clear names and short comments that explain intent or a non-obvious rule.
  Write for a junior developer. Avoid jargon, clever wording, and comments that
  only repeat the code.
- Keep the core in timelite.c and the public declarations in timelite.h while
  that remains easy to understand. Split files when there is a concrete need.
- Add no third-party dependency without a concrete reason and agreement on scope.
  Standard C headers and OS headers are fine. Do not recreate them to avoid includes.
- Prefer caller-owned memory and explicit buffer sizes. Do not add hidden
  allocations, global mutable state, background threads, or a general framework.
- Add abstractions only for an actual need. Keep OS calls behind a small storage
  boundary when storage is introduced; do not build unused platform layers now.
- Use fixed-width fields and explicit byte encoding for persistent data. Never
  write native structs directly to disk. Check lengths, overflow, and errors.
- Do not claim crash safety, durability, or thread safety without a defined
  contract and tests. A completed write is not necessarily a durable commit.

## Git workflow

- Work on a feature branch, normally codex/NNN-short-name. Never develop, commit,
  or push directly on main. Changes reach main through a GitHub pull request.
- Check the branch and working tree before editing. Preserve existing user edits
  and staged changes. Do not include unrelated work in a commit.
- Do not commit or push unless the user requests it. When publishing is requested,
  push the feature branch and open a pull request targeting main. Never force-push
  main or bypass branch protection. Merge only when authorized and checks pass.

## Feature notes and instruction maintenance

- Before implementing a feature, create docs/feature/NNN-short-name.md using the
  next unused number. Continue its note for follow-up work on the same feature.
- Record the desired state, scope, planned approach, actual changes, verification
  commands/results, and any remaining work or limits.
- Update the note as the work changes and before handing it back to the user.
  Clearly separate planned work from completed work and unrun checks from passes.
- Review AGENTS.md and CLAUDE.md during each feature. Update them whenever an
  accepted decision changes the workflow, style, architecture, or build process.
  Keep shared rules here and keep Claude's entry point consistent with them.
- Update README.md when setup or usage changes. Do not leave stale instructions.

## Build and verification

- Run make check for a normal local check. It builds the static library and runs
  the example; it is a smoke check, not a database test suite.
- Keep builds warning-free under the default strict C99 flags.
- When changing compiler flags, run make clean first; Make does not track changes
  to command-line variables. CC and AR can be overridden on the make command line.
- Add focused tests as real behavior appears. Storage changes need failure and
  recovery tests; do not add tests that merely duplicate trivial implementation.
- GitHub Actions checks Linux and macOS with Make and Windows with Clang directly.
  Report local results separately from CI and device results. Never claim unrun
  checks passed.
- Do not add database features as part of unrelated setup or documentation work.
