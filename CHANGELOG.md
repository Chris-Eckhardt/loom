# Changelog

All notable changes to loom are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
While loom is below 1.0, minor version bumps may contain breaking API changes.

## [9-29-2026]

## [0.1.0] - 9-29-2026

First release.

### Added
- Fiber-based job system: `init`, `run`, `quit` and shutdown.
- `kickJob` and `kickJobOnMain`, including lambda-friendly overloads.
- Counters: `waitForCounter`, `freeCounter` and `waitForCounterAndFree`.
- `parallelFor`.
- Job submission from external (non-worker) threads.
- `physicalCores()` and `threadCount()` queries.
- Chase-Lev work-stealing deque, spinlock, CPU affinity and fiber abstractions.
- CMake package config (`find_package(loom)`) and install rules.
- `version.h` with `LOOM_VERSION_*` macros, generated from the CMake project version.

### Fixed
- `waitForCounter` no longer fails silently when the job system quits.

[9-29-2026]: https://github.com/Chris-Eckhardt/loom/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/Chris-Eckhardt/loom/releases/tag/v0.1.0
