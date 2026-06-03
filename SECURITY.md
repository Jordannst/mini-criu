# Security Policy

`mini-criu` is an educational Linux checkpoint/restore project. It works close
to sensitive operating-system boundaries, including `ptrace`, `/proc/<pid>/mem`,
raw memory dumps, register state, and process control.

The project is intentionally narrow in scope. It is not a sandbox, malware
analysis system, privilege boundary, container runtime, or CRIU replacement.

## Supported Scope

Security review currently focuses on the `main` branch and the documented
checkpoint/restore demo scope:

- Linux `x86_64`
- same-machine restore
- single-threaded demo target
- narrow demo targets
- restore into a new PID
- fail-closed behavior outside the supported contract

Anything outside that contract should fail with an explicit reason instead of
trying to restore partially or silently.

## Security Boundaries

Please keep these boundaries in mind when using or contributing to the project:

- Run `mini-criu` only on processes and machines you own or are authorized to
  inspect.
- Do not install the binary as setuid or expose it as a remote service.
- Treat checkpoint artifacts as sensitive. They may contain process memory,
  paths, environment values, stack data, or other private runtime state.
- Do not load checkpoint artifacts from untrusted users.
- Do not use the current restore path for production workloads or security
  isolation.
- Prefer explicit validation and fail-closed errors over permissive best-effort
  restore behavior.

## Reporting a Vulnerability

If you find a security issue, please avoid opening a public issue with exploit
details. Use GitHub private vulnerability reporting if it is available on this
repository. If not, contact the maintainer privately at:

```text
keezmisz@gmail.com
```

Useful reports include:

- the affected command or code path
- expected and actual behavior
- target platform and kernel version
- whether the issue requires elevated privileges
- a minimal reproduction, if safe to share
- whether checkpoint artifacts contain sensitive data

I will try to acknowledge reports within 7 days. For valid issues, I will work
on a fix, documentation update, or explicit scope clarification before public
disclosure.

## Examples of Interesting Findings

- unsafe parsing of checkpoint artifacts
- memory corruption in dump or restore paths
- unexpected writes outside the intended checkpoint directory
- restore continuing after validation should have failed
- misuse of `ptrace` or `/proc/<pid>/mem`
- accidental exposure of private memory dump data in logs, examples, or CI

## Non-Goals

The following are currently out of scope unless the project explicitly adds
support later:

- arbitrary binary restore
- multi-threaded restore
- file descriptor, socket, namespace, timer, or signal-frame restore
- privilege escalation testing against third-party systems
- production hardening for untrusted checkpoint input
