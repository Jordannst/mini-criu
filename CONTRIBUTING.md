# Contributing to mini-criu

Thanks for considering a contribution. `mini-criu` is a small educational
systems project for understanding Linux checkpoint/restore internals in a
readable C codebase.

The project values correctness, explicit limits, and maintainable diagnostics
over broad restore coverage.

## Project Scope

Before contributing, please read:

- [README.md](README.md) for the current checkpoint/restore scope
- [SECURITY.md](SECURITY.md) for safety boundaries and disclosure guidance
- [ROADMAP.md](ROADMAP.md), if present, for planned milestones
- [MANUAL_TESTING_CLI.md](MANUAL_TESTING_CLI.md), if present, for manual
  validation

The current restore path is intentionally narrow. Changes that expand support
should also update validation, negative cases, and documentation.

## Development Setup

Use Linux or WSL from the repository root.

```bash
make clean && make
```

The main outputs are currently:

- `build/mini-criu`
- `build/targets/cpu_bound_target`
- `build/targets/memory_bound_target`

## Validation

Run the smallest relevant check before opening a PR:

```bash
make clean && make
bash scripts/smoke_cli.sh
```

If additional smoke or restore tests exist in your branch, run the relevant
ones and include the commands in the PR. If a check cannot be run on your
machine, mention that in the PR or issue and include the command output you did
capture.

## Contribution Guidelines

- Keep restore behavior fail-closed outside the documented contract.
- Prefer clear validation errors over silent best-effort behavior.
- Keep checkpoint artifacts and process memory data out of committed examples
  unless they are intentionally sanitized.
- Update README, ROADMAP, or manual testing docs when behavior changes.
- Add or update tests for new restore behavior, especially negative cases.
- Keep commits focused and easy to review.
- Avoid broad refactors mixed with behavior changes.

## Pull Request Checklist

Before asking for review, check:

- build succeeds with `make clean && make`
- relevant smoke or restore tests were run
- new limitations or assumptions are documented
- unsafe or privileged behavior is explained
- no private checkpoint data, local paths, or credentials are committed

## Good First Contributions

Good starter tasks include:

- improving diagnostics for fail-closed restore errors
- adding negative tests for malformed checkpoint files
- clarifying README examples
- documenting kernel or distro assumptions
- improving CI coverage for build and smoke checks

For security-sensitive changes, open an issue first so the scope and validation
plan can be discussed.
