# Security Policy

## Supported Versions

Security fixes are provided for the latest released version. Older releases are
not maintained.

| Version | Supported |
| ------- | ----------------- |
| 0.3.x   | :white_check_mark: |
| < 0.3   | :x: |

## Reporting a Vulnerability

Please **do not** open a public issue for security problems.

Report privately through GitHub's
[private vulnerability reporting](https://github.com/rayyan0x/skipbuild/security/advisories/new)
(Security → Advisories → "Report a vulnerability"). If you cannot use that,
open a minimal issue asking for a private contact channel and share no details
publicly.

Please include:

- A description of the issue and its impact.
- Steps to reproduce (version, OS, compiler, exact commands).
- Any proof-of-concept or logs (redact secrets).

We aim to acknowledge reports within 7 days and to provide a fix or mitigation
timeline after triage. Please allow a reasonable disclosure window before
publishing.

## Scope

skipbuild is a compiler-launcher that stores build artifacts locally and can
optionally read/write a remote cache. Areas of particular interest:

- **Remote cache transport and credentials.** Auth headers are only sent for
  encrypted transports (or when `SKIPBUILD_REMOTE_ALLOW_INSECURE=1` is set
  explicitly). Note that on non-Windows builds there is no TLS client, so
  `https://` remotes are not encrypted.
- **Untrusted cache artifacts.** Remote-supplied artifact names are sanitized to
  a bare filename to prevent path traversal, and zstd blobs are decompressed
  with a bounded, streaming reader to resist decompression bombs. Reported
  weakness in these defenses is in scope.
- **Local cache directory.** Cache files are treated as untrusted input.
- **Command execution.** skipbuild replays the compiler command line; any issue
  that lets a crafted cache entry or configuration execute code is in scope.

## Out of Scope

- Vulnerabilities in the compiler, linker, or build system it wraps.
- The remote server implementation itself (only skipbuild's client is covered).
- Issues that require an attacker to already control the build environment or
  the local cache directory.
