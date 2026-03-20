# mini-criu

`mini-criu` is a small academic prototype written in C for exploring Linux process checkpoint and partial restore ideas from user space. It is inspired by CRIU, but it is intentionally limited in scope and should be read as a systems-programming study project, not as a CRIU replacement.

The project focuses on a simple CLI workflow around:

- selecting a target process
- freezing and snapshotting register state
- parsing memory metadata from `/proc/<pid>/maps`
- dumping selected raw memory from `/proc/<pid>/mem`
- loading a checkpoint back into an experimental restore flow
- running controlled resume experiments and detailed diagnostics

## Overview

`mini-criu` exists to make low-level checkpoint/restore ideas easier to study in a small codebase. It explores practical pieces of process state handling such as:

- `ptrace` attach, stop, register capture, and register injection
- `/proc/<pid>/maps` parsing
- `/proc/<pid>/mem` reading and limited write-back
- checkpoint directory generation and metadata validation
- experimental restore-target setup and controlled post-restore observation

The restore side is still partial. The current code can prepare and probe a restore attempt, but it does not yet achieve stable process resurrection.

## Current Capabilities

The current prototype can:

- choose a target process with `set-target <pid>`
- freeze the target and capture CPU registers
- create checkpoint output including:
  - `checkpoint.info`
  - `regs.dump`
  - `mem.meta`
  - `mem.dump`
- keep one snapshot event coherent across `freeze` and `dump-memory`
- parse memory-region metadata and classify restore candidates
- load and validate an existing checkpoint directory
- build an internal restore plan from checkpoint metadata
- create a temporary restore target process
- inject an early subset of register state
- prepare executable and file-backed mappings conservatively
- write back a partial set of memory bytes
- run a controlled resume experiment
- print detailed restore diagnostics when the target stops again

## Current Restore Flow

The `restore <checkpoint_dir>` command currently performs an experimental restore-preparation flow:

1. validate the checkpoint directory and required files
2. load register data and memory metadata
3. build a restore mapping/write-back plan
4. create a temporary restore target
5. apply early register state
6. prepare or map selected executable and file-backed regions
7. write back a limited set of memory bytes
8. run a controlled resume attempt
9. report diagnostic information if the target stops or crashes

This is useful for studying restore mechanics and failure modes, but it is still not a full restore implementation.

## Current Limitations

Important limitations are still present:

- full stable process resurrection is not achieved
- restore remains partial and experimental
- post-resume stack/control-flow inconsistency is still the main blocker
- file descriptor restoration is not implemented
- socket restoration is not implemented
- multithreaded restore is not implemented
- the project does not attempt production-grade CRIU behavior

In short: checkpoint creation is already useful and concrete, but restore is still best understood as an investigative prototype.

## Build

Build the CLI and example targets with:

```bash
make
```

Produced binaries:

- `build/mini-criu`
- `build/targets/cpu_bound_target`
- `build/targets/memory_bound_target`

Clean build artifacts with:

```bash
make clean
```

## Run

Recommended launcher:

```bash
./run-mini-criu
```

Run the CLI directly:

```bash
./build/mini-criu
```

Run a single command without entering the interactive shell:

```bash
./run-mini-criu help
./run-mini-criu status
./run-mini-criu restore checkpoints/example
```

## Commands

- `help`
- `status`
- `set-target <pid>`
- `freeze`
- `dump-memory`
- `restore <checkpoint_dir>`
- `clear`
- `/clear`
- `exit`

## Example Workflow

Start a dummy target in another terminal:

```bash
./build/targets/cpu_bound_target
```

Start the CLI:

```bash
./run-mini-criu
```

Checkpoint flow:

```text
mini-criu> set-target 12345
mini-criu> status
mini-criu> freeze
mini-criu> dump-memory
mini-criu> exit
```

What happens:

- `freeze` attaches to the target, captures register state, writes `checkpoint.info` and `regs.dump`, and keeps the target stopped for the active snapshot
- `dump-memory` continues the same snapshot, writes `mem.meta` and `mem.dump`, then releases the target again

Restore experiment:

```bash
./build/mini-criu restore checkpoints/checkpoint-pid-12345-YYYYMMDD-HHMMSS
```

What the restore command currently does:

- loads and validates checkpoint files
- prepares a temporary restore target
- applies early register state
- performs partial mapping and write-back work
- attempts a controlled resume
- prints diagnostic output about the observed failure mode

## Checkpoint Files

A typical checkpoint directory may contain:

- `checkpoint.info` for high-level checkpoint metadata
- `regs.dump` for saved CPU register values
- `mem.meta` for memory-region metadata and dump layout
- `mem.dump` for raw memory bytes from selected regions

## Repository Layout

- `src/main.c` and `src/cli.c`: CLI entry and command handling
- `src/freeze.c`: target attach, register capture, and snapshot start
- `src/memory_dump.c`: memory metadata parsing and raw memory dumping
- `src/restore.c`: checkpoint loading, restore preparation, experimental resume, and diagnostics
- `src/utils.c`: shared helpers
- `targets/`: simple dummy processes for testing
- `include/mini_criu.h`: shared declarations

## Project Status

`mini-criu` is currently a solid checkpointing prototype with a detailed experimental restore path. It already demonstrates real systems mechanisms and produces meaningful checkpoint artifacts, but restore is still incomplete and unstable after resume. The repository should be read as an academic exploration project that is honest about its current limits.
