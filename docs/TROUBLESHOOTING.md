# RTT Engine — Troubleshooting Guide

Practical fixes for the most common build, test, and runtime problems. Start with the
**Quick Reference**; dig into the sections for details.

---

## ⚡ Quick Reference

| Symptom | Fix |
|---------|-----|
| `fatal error: glm/glm.hpp` / `gtest/gtest.h` / `assimp/...`: No such file | Install missing dev packages (see [Build failures](#-build-failures)) |
| `cannot find -lGL` / `-lX11` / `-lopenal` / `-lcurl` at link time | Install `xorg-dev`, `libopenal-dev`, `libcurl4-openssl-dev` etc. |
| Weird build errors after switching build modes | `make clean` first — `build/` is shared between `MODE=debug/release/asan` |
| `SELF-CHECK FAILED - not booting the engine` | Run the failing suite with a gtest filter, fix it, or re-run with `--skip-tests` |
| Flood of `ASSIMP ERROR: Unable to open file "./assets/World_objects/..."` | Non-fatal — the dir only ships `Bear_DEMO.fbx`. Run from the repo root. |
| Engine exits immediately / prints logic-sim summary | No OpenGL context available (`DISPLAY` unset, no X server) — see [Headless & display](#-headless--display) |
| Character stands still at the start | The cinematic script begins with 2 s of idle. Press any key (interactive) or use more headless frames |
| Crash — process dies with no message | Check `crash.log` (demangled backtrace is written automatically) |
| Black / blank editor viewport | Delete stale runtime config: `engine_ui.ini`, `imgui.ini`, `camera_mode.cfg` |
| Tests crash with `*** stack smashing detected ***` | Rebuild — this was an ODR collision in `tests/test_camera_follow.cpp`, fixed in August 2026 |
| Geo HTTP tests fail / hang | A port from `tests/test_geo_http.cpp` is likely in use — free the port or re-run |

---

## 🔨 Build Failures

### Missing dependencies

The engine needs the following dev packages (Ubuntu 24.04+):

```bash
sudo apt install build-essential git \
  libglfw3-dev libglew-dev libglm-dev libeigen3-dev libassimp-dev \
  libjsoncpp-dev libcurl4-openssl-dev libopenal-dev libgtest-dev
```

Typical symptoms and the package that fixes them:

| Error | Package |
|-------|---------|
| `glm/glm.hpp: No such file` | `libglm-dev` |
| `gtest/gtest.h: No such file` or `cannot find -lgtest` | `libgtest-dev` |
| `assimp/... No such file` or `cannot find -lassimp` | `libassimp-dev` |
| `GLFW/glfw3.h: No such file` or `cannot find -lglfw` | `libglfw3-dev` |
| `cannot find -lX11` / `-lXrandr` / `-lXinerama` | `xorg-dev` |
| `cannot find -lcurl` | `libcurl4-openssl-dev` |
| `cannot find -lopenal` | `libopenal-dev` |
| `json/json.h: No such file` | `libjsoncpp-dev` |
| `Eigen/... No such file` | `libeigen3-dev` |

### Switching build modes without cleaning

`MODE=release` and `MODE=asan` reuse the same `build/` directory as the default debug
build. If you change modes without cleaning, you get a mix of `-O2`/`-fsanitize`/
`-g` objects and may see confusing failures:

```bash
make clean
make MODE=release    # or MODE=asan
```

### Link errors mentioning `main`

The repository has several entry points, each built as its own binary:

- `bin/test_runner` — `tests/test_main.cpp` provides `main`
- `bin/engine` — `test.cpp` provides `main` (test objects except `test_main.cpp` are linked in)
- `bin/editor_app` — `src/editor_main.cpp` provides `main`

A "multiple definition of `main`" error means one of these object sets got mixed up —
build with the provided targets (`make`, `make engine`, `make editor`) rather than
hand-rolling link commands.

### Out of memory during build

`make -j$(nproc)` on machines with little RAM can OOM on the big translation units
(ImGui, glad, renderer). Use fewer jobs:

```bash
make -j2
```

---

## 🧪 Test Failures

### The engine refuses to boot: `SELF-CHECK FAILED`

`make run` runs all 731 tests first and **will not boot the engine if any fail**.
That is by design — fix the failure, or force-boot with:

```bash
./bin/engine --skip-tests
```

To find the failing test quickly, run the suite that matches your change:

```bash
make test-physics       # physics
make test-camera        # camera
make test-play-mode     # character / play mode
make test-integration   # end-to-end character flow
./bin/test_runner --gtest_filter='Animation*'   # any suite via filter
```

### The test phase is slow (~2 minutes)

FBX loading and geo HTTP tests dominate the runtime. For fast iteration use:

```bash
make test-quick         # skips .PERF_*, Integration*, Terrain*
./bin/test_runner --gtest_filter='-*FBX*'   # skip FBX-heavy suites
```

### `*** stack smashing detected ***`

If you see this in `CameraFollowTest`, rebuild from current sources — it was an
ODR collision between the test's mock camera types and the engine's real
`ThirdPersonCamera`, fixed by wrapping the mocks in an anonymous namespace
(August 2026). Stale objects in `build/` may still carry the old code:

```bash
make clean && make
```

### Geo / HTTP tests fail or hang

`tests/test_geo_http.cpp` binds a local HTTP port. If another process (or a
previous stuck test run) holds it, the test may fail or hang. Kill leftover
processes, then re-run:

```bash
pkill -f test_runner || true
make test
```

---

## 🎮 Engine Run Issues

### Working directory must be the repo root

All asset paths are relative (`assets/bot.fbx`, `assets/Idle.fbx`, ...). Run from the
repository root, otherwise:

```
[Engine] FAILED to load assets/bot.fbx - is the working dir the repo root?
[EditorApplication] Play Mode: failed to load character 'assets/bot.fbx'
```

### Flood of `ASSIMP ERROR: Unable to open file "./assets/World_objects/..."`

Non-fatal. The world manager tries to place pre-configured objects
(`stone.fbx`, `Rock0-3.fbx`...) but `assets/World_objects/` only ships
`Bear_DEMO.fbx`. The engine logs the errors and continues. To silence them, add the
missing FBX files to `assets/World_objects/` — the messages are safe to ignore.

### Headless & display

- **No `DISPLAY` at all**: `glfwInit` fails and the engine falls back to a
  **GL-free logic simulation** of the character + motion-matching core, runs the
  frame budget, prints a summary, and exits 0.
- **`--headless`**: hidden window + bounded run (default 900 frames at a fixed 60 Hz
  timestep), exits automatically with a summary. Great for CI.
- **Interactive mode hanging**: that's normal — it's a windowed app. Quit with
  **Esc** or close the window. Use `--headless` in scripts/CI.

> Note: in headless mode the cinematic script advances with a **fixed 60 Hz
> timestep**, not wall-clock time — 120 frames ≈ 2 s of simulation, i.e. the
> first ~2 s are Idle before the character starts walking (at 120 frames it sits
> right at the Idle→Walk boundary). Use at least 900 frames to see the full
> idle → walk → run → jump → return cycle.

### Black or blank viewport in the editor

Stale ImGui layout/state files can leave panels off-screen or blank:

```bash
rm -f imgui.ini engine_ui.ini camera_mode.cfg
make editor
```

(`imgui.ini` is what the current editor app reads; `engine_ui.ini` is left over from
the legacy entry point. Deleting both is harmless — they are gitignored.)

### The character stands still / doesn't react

- Interactive: you're in the **cinematic demo** (boots by default) — the first 2
  seconds are Idle. Press any movement key to take control; **F9** toggles the script.
- Headless: the run may simply be too short to leave the Idle phase — raise `--frames`.

### Motion matching not active

The HUD shows `motion matching: ACTIVE` only when the motion database was built
successfully from the locomotion clips. If it shows `off`, the clips failed to load
(missing/malformed FBX in `assets/`). Check for ASSIMP errors at boot; `make
test-play-mode` validates clip loading independently.

---

## 💥 Crashes

### Reading `crash.log`

On `SIGSEGV`/`SIGABRT`/`SIGFPE` the engine writes a **demangled backtrace** to
`crash.log` in the working directory:

```
=== CRASH LOG ===
Signal: 11
Backtrace (12 frames):
...
Demangled frames:
#0 0x555555... World::createEntity()
#1 0x555555... main
```

The top frames show the exact function that faulted.

### Reproduce under AddressSanitizer

```bash
make test-memory-debug   # rebuilds with ASan+LSan and runs the full suite
# or, for the engine:
make clean && make MODE=asan engine
ASAN_OPTIONS=detect_leaks=1 ./bin/engine --skip-tests --headless --frames 300
```

ASan reports the allocation/use-of-uninitialized sites that a crash log can't.

### Crash during shutdown (exit time)

If the process crashes *after* the main loop when exiting, it is usually a
destructor touching OpenGL after the context is gone. This engine guards those
paths via `glctx::setAlive(false)` before `glfwTerminate()` — a crash here means a
new resource was added without the guard. Run under ASan to find the object.

---

## 🤖 CI (GitHub Actions)

The workflow (`.github/workflows/ci.yml`) installs all dependencies plus `xvfb` and
runs tests under a virtual X server — GLFW window creation **requires an X server**,
so plain `./bin/test_runner` will fail on a headless runner:

```bash
xvfb-run -a ./bin/test_runner --gtest_print_time=1
xvfb-run -a ./bin/engine --skip-tests --headless --frames 300
```

Common CI failures:

- **`cannot find -lX11`**: the runner's `libglfw3-dev` didn't pull in X11 headers —
  `xorg-dev` is in the workflow apt list; keep it there.
- **Mesa "llvmpipe" rendering**: expected in CI (no GPU). Tests are written to pass
  on software GL; if a viewport test fails only in CI, verify it still passes under
  `xvfb-run` locally.
- **Slow jobs**: the engine step uses `--skip-tests` because the suite already ran in
  the same job — don't re-add the self-check to CI.

---

## 🗂️ Runtime files (safe to delete, all gitignored)

| File | Purpose |
|------|---------|
| `crash.log` | Demangled crash backtrace from the last crash |
| `engine_ui.ini`, `imgui.ini` | Stale ImGui layout state |
| `camera_mode.cfg` | Persisted camera mode |
| `gpu_profile.csv` | GPU profiler CSV export |
