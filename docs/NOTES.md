# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*vectrix — oscillator + pedalboard + vector CRT as two FFGL plugins; PUBLIC MIT, v0.1.0, first CPU audio-rate DSP in the fleet, never run in Resolume*

**vectrix** (created 2026-08-17, `~/projects/resolume/vectrix`, **PUBLIC MIT at
`stoatworks-labs/vectrix`**) — an oscillator, a fourteen-block guitar/eurorack
pedalboard and a cathode ray tube in **X/Y (vector) mode**, as two FFGL plugins:
`VX01` Vectrix (FF_SOURCE) and `VX02` Vectrix Trace (FF_EFFECT). ~12k lines.
Inspired by Ms Mad Lemon's *Vector Synth Visualizer* series on YouTube.

## The governing idea

**Models a route, not a look** — old-cathode's rule, made arithmetic twice:

- The renderer **never computes `1/v`**. It deposits a fixed quantum of energy
  per *sample interval* and spreads it over the screen distance the beam covered,
  so dwell-proportional brightness is what "equal energy per unit time" *means*.
  The fragment shader evaluates the closed-form convolution of a uniform segment
  with a Gaussian spot: energy conserved for any length and any sigma, correct
  saturation when fast, finite when stationary, **no clamps anywhere**.
- **Sources are geometry; brightness is physics.** A source never modulates `z`
  to look bright. Corner brightening comes from the slew limiter and the
  deflection amplifier's bandwidth, at the end of the chain.

## Firsts and shape

- **First CPU audio-rate DSP in the fleet** (96 kHz default; Draft 48 / Fine 192).
  Every coefficient formula lives in `signal/fx/Filters.h` and nowhere else.
- **First `glReadPixels` inside `ProcessOpenGL`** (the Trace source's edge pass,
  PBO-double-buffered one frame late). **Never measured in a real host.**
- `Sample{x,y,z,dt}` carries `dt` **per sample**, which makes brightness
  independent of sample count as an *identity*, not a calibration.
- Opus builds **libogg/libopus/libopusfile from source as submodules** — vcpkg
  triplets are per-architecture and cannot make the universal macOS binary.
  Verified fat. `-DVECTRIX_WITH_OPUS=OFF` drops all three.

## Rules that are the opposite of the fleet's GPU habit

- **A parameter change must NOT clear DSP state.** Delay tails and reverb tanks
  survive knob moves; bypass crossfades 20 ms and never resets.
- **Phase is integrated in `double`**, never `t * frequency` (tinsel's lesson).
- **Stereo means de-correlated** — Y's modulation runs 90° behind X's.

## What v0.1.0 verified, and what it did not

`tools/vxtest` (headless CGL 4.1, deterministic from a frame counter) —
`--energy` 0.0103% over 100:1 speeds, `--dwell` r²=0.99998896, `--identity`
bit-identical, `--drift` 3.1e-08 turns over 10 min. `verify.sh` **21/21**;
`sweep.py` 152 swept, **0 dead**.

**Never run in Resolume.** The OFX build renders on the **CPU** (OpenFX gives no
GL context), replays on any seek, and its Trace source draws no beam.

## Five defects the harness found that nothing else would have

1. **Beam 0 was not zero beam** — the exponential bottomed at 0.1, so exact
   passthrough was unreachable through the parameter list.
2. **Both halation controls were measurably dead** — the bright pass kept a 0.5
   knee a moving trace never reaches. Knee now explicit; Halation maps 0..4.
3. **Every integer-typed preset column held a fraction**, so all 15 presets
   clamped to their minimum (Star drew 3 points). `check_presets.py` now
   enforces the *type*, not just row width.
4. **Shipped defaults drew a comet** — 8 Hz against a P31 that carries nothing
   between frames. Now 120 Hz.
5. A "harness entry point" that froze the clock, making 13 live controls read
   dead. Deleted rather than fixed.

Lesson worth keeping: **`sweep.py` asks whether a control changes the picture,
not whether it changes it *usefully*.** Halation squeaked past the
zero-difference test while being unusable.

## 2026-08-23: the CI Windows job had never once configured

`ci.yml` passed the vcpkg toolchain **unquoted**:

    -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake

A `run:` step on Windows is PowerShell, and unquoted it reached CMake as that
literal text, failing with **"Could not find toolchain file"** — which reads
like a missing vcpkg in the runner image, not a quoting bug in this repo. Every
pull request had been red since the job was added; `release.yml` had the quotes
all along, which is why releases built. `-A x64` added at the same time so CI
configures the way release.yml does. Verified green on the real Windows runner.

**All 22 other Resolume repos already quote it** — vectrix's `ci.yml` was the
only one, so this is not a fleet sweep.

Related: [old cathode](https://github.com/stoatworks-labs/old-cathode/blob/main/docs/NOTES.md) (`old-cathode`) (raster CRT — the sibling this is deliberately
not), [resolume scopes](https://github.com/stoatworks-labs/resolume-scopes/blob/main/docs/NOTES.md) (`resolume-scopes`) (measurement, not synthesis; `ScopeBuffer` and
`SavedGLState` copied from it), [nib](https://github.com/stoatworks-labs/nib/blob/main/docs/NOTES.md) (`nib`) (edge detection as a *look*;
vectrix's Trace makes an ordered *path*), [ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md),
[pipefail grep q trap](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_pipefail_grep_q_trap.md) (reintroduced here from scratch),
**disclaimer scope** (working-practice note, kept in Claude memory), **release workflow** (working-practice note, kept in Claude memory).
