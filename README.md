# mini-criu

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Language: C](https://img.shields.io/badge/language-C-blue.svg)](#project-structure)
[![Platform: Linux](https://img.shields.io/badge/platform-Linux%20%2F%20WSL-2ea44f.svg)](#build)
[![Status: Experimental](https://img.shields.io/badge/status-experimental-orange.svg)](#current-status)

`mini-criu` is a small educational Linux checkpoint/restore project written in
C. It is inspired by CRIU, but intentionally scoped down so the core mechanics
are easier to inspect, test, and reason about.

The project focuses on practical low-level pieces behind checkpoint/restore:

- freezing a target process with `ptrace`
- capturing register state
- reading process memory layout from `/proc/<pid>/maps`
- dumping selected raw memory through `/proc/<pid>/mem`
- storing checkpoint metadata in simple text artifacts
- preparing an experimental restore path with controlled resume diagnostics

`mini-criu` is not a CRIU replacement. The checkpoint path is concrete, while
restore is still experimental and focused on diagnosis rather than full Linux
process recovery.

> [!NOTE]
> This repository is built as a readable systems-learning project. It aims to
> expose real checkpoint/restore mechanics without claiming production-grade
> process recovery.

> [!WARNING]
> Checkpoint artifacts may contain private process memory. Do not run
> `mini-criu` against processes or machines you do not own or have permission
> to inspect.

## At a Glance

| Area | Current state |
| --- | --- |
| Language | C |
| Platform | Linux / WSL |
| Interface | Interactive CLI and one-shot commands |
| Checkpoint | Concrete register and selected-memory dump flow |
| Restore | Experimental partial restore with controlled resume diagnostics |
| Safety stance | Narrow scope, explicit limitations, fail-closed expectations |
| License | MIT |

## Contents

- [Why This Exists](#why-this-exists)
- [Current Status](#current-status)
- [What Works Today](#what-works-today)
- [Restore Flow](#restore-flow)
- [Limitations](#limitations)
- [Security and Scope](#security-and-scope)
- [Build](#build)
- [Quick Start](#quick-start)
- [Usage](#usage)
- [Project Structure](#project-structure)
- [Validation](#validation)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License](#license)

## Why This Exists

Checkpoint/restore is a deep systems problem. A complete implementation has to
deal with memory mappings, registers, stacks, file descriptors, sockets,
threads, signals, namespaces, timers, ASLR, libc runtime state, and many other
details.

`mini-criu` exists to make a useful slice of that problem approachable. The
goal is to keep the code small enough to study while still touching real Linux
interfaces such as `ptrace`, `/proc/<pid>/maps`, and `/proc/<pid>/mem`.

## Current Status

`mini-criu` can currently create real checkpoint artifacts and run an
experimental restore preparation flow. Restore includes checkpoint loading,
target setup, register application, partial memory write-back, controlled
resume observation, and detailed diagnostics when the restored target does not
continue cleanly.

The project is deliberately honest about its limits: restore is not yet stable
for arbitrary Linux processes.

## What Works Today

`mini-criu` currently supports:

- `freeze <pid>` to stop a target process and capture a register snapshot
- `dump-memory` to write memory metadata and raw memory bytes
- `list` to show saved checkpoints with short flags such as `F0001`
- `resume <flag>` to release the original frozen process when possible
- `restore <flag>` to load checkpoint artifacts and run a controlled partial
  restore experiment
- diagnostics that connect restore failures to memory layout, register state,
  stack state, and resume behavior

Checkpoint artifacts currently include:

- `checkpoint.info` - checkpoint metadata and status
- `regs.dump` - register snapshot
- `mem.meta` - selected memory-region metadata
- `mem.dump` - raw bytes for selected dumped regions

## Restore Flow

`restore <flag_checkpoint>` currently follows an experimental flow:

1. Validate the checkpoint directory and required files.
2. Load checkpoint identity and register state.
3. Load memory-region metadata from `mem.meta`.
4. Verify that `mem.dump` matches the recorded metadata.
5. Prepare a restore target process.
6. Apply initial register state where possible.
7. Attempt selected memory mapping and write-back.
8. Run a short controlled resume experiment.
9. Collect diagnostics if the target stops, exits, or crashes.

This flow is useful for studying restore mechanics and failure modes. It should
not be interpreted as a complete process restore engine.

## Limitations

The project does not currently support:

- general-purpose process restore
- arbitrary binary restore
- stable PIE/ASLR-aware restore
- complete heap, stack, libc, or TLS runtime restoration
- file descriptor restore
- socket restore
- multi-threaded restore
- signal frame restore
- namespace restore
- timer restore
- production use with untrusted checkpoint input

These limitations are part of the current design. The project should fail
explicitly and remain clear about what is supported.

## Security and Scope

`mini-criu` touches sensitive Linux internals such as `ptrace`,
`/proc/<pid>/mem`, raw memory dumps, register state, and process control. Use
it only on processes and machines you own or are authorized to inspect.

> [!IMPORTANT]
> Treat checkpoints like sensitive artifacts. A memory dump can include stack
> data, paths, environment values, and other private runtime state.

Security expectations:

- checkpoint artifacts may contain private process memory
- checkpoint artifacts should be treated as sensitive
- untrusted checkpoint artifacts are not supported
- restore should fail closed outside the documented demo scope
- the binary should not be installed as setuid
- the binary should not be exposed as a remote service
- the project is for education and experimentation, not production isolation

For vulnerability reporting and detailed security boundaries, see
[SECURITY.md](SECURITY.md).

## Build

Build the CLI and demo targets:

```bash
make
```

Main build outputs:

- `build/mini-criu`
- `build/targets/cpu_bound_target`
- `build/targets/memory_bound_target`

Clean build artifacts:

```bash
make clean
```

## Quick Start

```bash
make clean && make
./build/targets/cpu_bound_target
```

In a second terminal:

```bash
./build/mini-criu
```

Inside the CLI:

```text
mini-criu> freeze <pid>
mini-criu> dump-memory
mini-criu> list
mini-criu> resume F0001
```

> [!TIP]
> Use `list` after a checkpoint to find the short checkpoint flag, such as
> `F0001`, before running `resume` or `restore`.

## Usage

Start the interactive CLI:

```bash
./run-mini-criu
```

Or run the compiled binary directly:

```bash
./build/mini-criu
```

Run one command without entering the interactive shell:

```bash
./build/mini-criu help
./build/mini-criu status
./build/mini-criu list
./build/mini-criu resume F0001
./build/mini-criu restore F0001
```

## CLI Commands

- `help`
- `status`
- `freeze <pid>`
- `dump-memory`
- `list`
- `resume <flag_checkpoint>`
- `restore <flag_checkpoint>`
- `clear`
- `/clear`
- `exit`

## Example Checkpoint Flow

Run a target process in another terminal:

```bash
./build/targets/cpu_bound_target
```

Start the CLI:

```bash
./build/mini-criu
```

Create a checkpoint:

```text
mini-criu> freeze 12345
mini-criu> dump-memory
mini-criu> list
mini-criu> exit
```

Resume the original frozen process:

```bash
./build/mini-criu resume F0001
```

Run the experimental restore flow:

```bash
./build/mini-criu restore F0001
```

## Project Structure

- `src/main.c` - CLI entry point
- `src/cli.c` - command parsing and interactive shell
- `src/freeze.c` - process freeze and register capture
- `src/memory_dump.c` - memory metadata and raw memory dumping
- `src/restore.c` - checkpoint loading, partial restore preparation,
  controlled resume experiments, and diagnostics
- `src/utils.c` - shared helpers
- `include/mini_criu.h` - shared declarations
- `targets/` - simple demo targets for local testing

## Validation

At minimum, verify that the project builds:

```bash
make clean && make
```

Because restore touches process state and raw memory, manual testing should be
done on a Linux or WSL environment that you control.

## Roadmap

Near-term work focuses on:

- clearer restore diagnostics
- safer checkpoint artifact validation
- better negative-case coverage
- documented kernel and target assumptions
- CI coverage for build and smoke checks
- incremental restore hardening without overstating support

The long-term goal is not to become a production CRIU clone. The goal is to
remain a readable and honest learning project for Linux checkpoint/restore
internals.

## Contributing

Contributions are welcome when they preserve the project's narrow scope and
fail-closed behavior. Start with [CONTRIBUTING.md](CONTRIBUTING.md) and
[SECURITY.md](SECURITY.md).

Good first contributions include diagnostics, negative tests, documentation,
and build or CI improvements.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).
