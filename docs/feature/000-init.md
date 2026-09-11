# 000: Project bootstrap

Status: bootstrap complete; local checks passed.
Branch: codex/000-init.

## Desired state

A small C99 library project that can build and run an example, with clear
instructions for future AI-assisted development. All work goes through a feature
branch and a pull request into main.

Use plain C, explicit control flow, and short comments that a junior developer
can understand. Keep the starting code small and avoid external dependencies.

## Scope and approach

- Start with timelite.h and timelite.c; single-header packaging can wait.
- Export a version function so the example exercises the library link.
- Build a static library and example with a small Makefile.
- Add GitHub Actions smoke checks for Linux, macOS, and Windows.
- Add shared agent rules, a Claude entry point, and this feature record.
- Leave storage, queries, allocation, and OS adapters for later features.

## Actual changes

- Created the feature branch without changing the existing research edits.
- Added the C99 header, implementation, and example. The core has no allocation,
  OS calls, or external library dependencies. The example uses standard I/O.
- Added a Makefile with strict warnings, a static-library build, a smoke check,
  and a clean target. Toolchain variables can be set on the command line.
- Added a CI workflow that builds and runs the example on three desktop OSes.
- Added AGENTS.md and CLAUDE.md, including feature-note maintenance and the rule
  that accepted workflow/style changes must be reflected in both instruction files.
- Replaced the placeholder README with setup, usage, scope, and contribution notes.
- Ignored build output and local IDE files without removing existing IDE files.

## Verification

Verified on macOS ARM64 with Apple Clang 21.0.0 and GNU Make 3.81:

- make check: passed. Built the static library and example with C99 and strict
  warnings enabled; the example printed Timelite 0.1.0-dev.
- Direct compilation using the README command: passed; the example printed the
  same version. This checks integration without the Makefile or static archive.
- git diff --check: passed for tracked changes.
- Reviewed source, build rules, and documentation. Build output and IDE files are
  ignored. The existing staged and unstaged research changes remain in place.

These are build/link smoke checks. No database behavior exists to test yet.

## Remaining limits

- This is not a database yet; only the version function is implemented.
- No storage format, durability guarantee, or OS storage layer exists.
- GitHub Actions has not run. Windows, Linux, x86, and ARM32 have not been tested
  as part of this bootstrap unless later results are recorded above.
- Existing research changes belong to the user and remain untouched.
- Follow-up: the user requested publishing this branch and merging through a PR
  into main. Publication and CI results will be recorded on the pull request.
  No remote branch-protection settings will be changed.
