# Contributing to RTT Engine

Thanks for contributing! This guide keeps the repository merge-ready and reviews fast.

## Getting Started

1. **Fork** the repository and clone your fork.
2. Install the [dependencies from the README](README.md#prerequisites-ubuntu-2404).
3. Build and verify the baseline works before changing anything:

```bash
make            # build the test runner
make test       # all 494 tests should pass
make run-headless   # engine self-check + bounded headless boot
```

## Development Workflow

1. Create a feature branch off `main`: `git checkout -b feat/my-change`.
2. Make focused changes. Keep each commit self-contained with a clear message.
3. **Add or update tests** in `tests/` for any new system or behavior (Google Test).
4. Run the relevant suite before committing:

```bash
make test-physics          # or the suite that matches your change
make test-quick            # fast full pass
make                       # ensure everything still compiles
```

5. Run the full suite + engine boot once before opening the PR:

```bash
make test
make run-headless FRAMES=300
```

## Code Conventions

- **C++17**, no external dependencies beyond what the Makefile already links.
- **Match the surrounding style** of the module you touch (naming, formatting, header
  layout, comments). Each subsystem (`ecs/`, `motionMatching/`, `editor/`, ...) has
  established conventions — consistency within a module matters most.
- Keep engine logic **GL-free where possible** so it stays unit-testable headlessly
  (see `editor/PlayModeController.h` for the pattern: pure logic + a thin GL wrapper).
- Prefer small, focused PRs. Include a short description of what changed and why.
- Do not commit: `build/`, `bin/`, `crash.log`, `*.ini` runtime files, or private keys
  (see `.gitignore`).

## Pull Request Process

1. Push your branch and open a PR against `main`.
2. CI runs the full build + test suite (see `.github/workflows/ci.yml`). Make sure it's
   green.
3. Keep the PR title imperative and concise (e.g. "Add frustum culling to the renderer").
4. A maintainer will review; address review feedback in follow-up commits.

## Reporting Bugs

Open an issue with:
- What you ran (`make run`, a test filter, etc.)
- The expected vs. actual behavior
- Any relevant output or a `crash.log` if the engine crashed

The engine writes a demangled backtrace to `crash.log` on a crash — attach it if present.
For common problems, check the [Troubleshooting Guide](docs/TROUBLESHOOTING.md) first.
