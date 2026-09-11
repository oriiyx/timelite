# Timelite

A small embedded time-series database project in C99. The current version is a
library skeleton: it builds, links into an example, and reports its version.
It does not store or query data yet.

The direction is a small C API, explicit memory ownership, no third-party
dependencies, and a narrow scope for sensor history. Target devices include x86
and ARM32; device support has not been verified yet.

## Build

On Linux or macOS, install a C99 compiler, Make, and ar, then run:

```sh
make check
```

This creates build/libtimelite.a and build/basic, then prints:

```text
Timelite 0.1.0-dev
```

Use make clean to remove build output. To select another compiler, use
make CC=clang check. Clean first when changing compiler flags or toolchains.

You can also compile the library directly into an application:

```sh
mkdir -p build
cc -std=c99 -Wall -Wextra -Wpedantic -Werror -I. timelite.c examples/basic.c -o build/basic-direct
./build/basic-direct
```

On Windows, with Clang on PATH, run these commands in PowerShell:

```powershell
New-Item -ItemType Directory -Force build
clang -std=c99 -Wall -Wextra -Wpedantic -Werror -I. timelite.c examples/basic.c -o build/basic.exe
./build/basic.exe
```

The Makefile uses Unix shell commands. GitHub Actions has Linux, macOS, and
Windows smoke checks; those are separate from validation on the target devices.

## Files

- timelite.h: public declarations.
- timelite.c: library implementation.
- examples/basic.c: a small application that calls the library.
- [AGENTS.md](AGENTS.md) and [CLAUDE.md](CLAUDE.md): coding-agent instructions.
- [docs/feature/000-init.md](docs/feature/000-init.md): bootstrap scope and results.
- [research/RESEARCH.md](research/RESEARCH.md): earlier feasibility research.

## Contributions

Use a feature branch and a pull request into main. Do not push directly to main.
Each feature needs a numbered note in docs/feature describing the desired state,
actual changes, verification, and remaining work. Keep the agent instructions
and this README current as decisions change.

Licensed under the [MIT license](LICENSE).
