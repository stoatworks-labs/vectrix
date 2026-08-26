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

## 2026-08-26: the plugin took Resolume down on every Windows start (#5)

`kAudioExtensions` has **no null terminator** — `kAudioExtensionCount` bounds it,
and that constant was used nowhere in the repo. The one place that read the
array, building the file parameter's extension list, walked it for a terminator
instead:

    for( const char* const* e = kAudioExtensions; *e != nullptr; ++e )

So the loop ran off the end and read whatever the linker put next. **On macOS
that was zero and it stopped by luck**, which is why 21/21 in `verify.sh`, 152
swept controls and every offline check passed on a plugin that could not start
its host. MSVC lays `kAudioExtensionCount` immediately after the array, so on
Windows the walk read the integer **7**, constructed `std::string` from
`(const char*)7`, and took `0xc0000005` reading address `0x7` in `ucrtbase`.

It happens in `declareParameters()`, **before `diag::init()`** — so during
Resolume's plugin scan, before a frame, with **no vectrix log written at all**.
The user sees Arena die at startup, every time, with

    ra::WinPluginInstance::load: Loading plugin '...\Vectrix Trace.dll'

as the last line of Resolume's own log, and no way back except deleting the DLL.

**Reproduced on [arena on winlab](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_arena_on_winlab.md), which is what made it a bug and not a
driver story** — that box is llvmpipe with no GPU, and the reporter was on an
RTX 4070. Same crash on both. The minidump named the faulting module and the
address, and `"Retrace Speed"` — the parameter declared immediately before the
audio block — was still on the stack, which brackets the fault to four lines.

The array is now a complete `constexpr` type in the header and the call site
range-fors it, so the idiom is unavailable to write again.

**Not a fleet sweep** — grepping all 39 Resolume repos for the null-walk pattern
returns this site and nothing else.

Lesson worth keeping, and it is the sharper form of the `sweep.py` one above: a
test suite that only ever runs on the developer's platform cannot see undefined
behaviour that the developer's linker happens to make benign. **Load the shipped
Windows binary in a real Windows host before releasing it** — that is now cheap.

## 2026-08-26: 173 controls down to 81, and a rule that was the wrong way round

An external user filed "there are soooooo many parameters, it took a while to
find presets" (#7). Two things came out of chasing it, both measured on the
win-lab Arena rather than reasoned about.

**Resolume honours `SetParamVisibility`, at runtime.** Nothing in the fleet had
proved a host acts on it. It does, for every parameter, and it updates the panel
live when the plugin raises `FF_EVENT_FLAG_VISIBILITY`. So the four source
groups the current Source cannot reach are now hidden, and each of the thirteen
pedals shows only its On switch until it is switched on — not one of them
defaults to on. **173 visible controls become 81**, and switching Delay on
returns exactly its nine.

The On switch is never hidden. Hiding the control that turns a thing back on
leaves an operator with no way back, and that is worse than a long list.

**"Never renumber a released id" was wrong, and it had been shaping this repo.**
A saved composition stores `name`/`value` pairs, and only for parameters that
differ from their default — not indices. Proved rather than assumed: a
composition was saved with three marker values, `PT_PRESET` was moved from the
middle of the list to the end (shifting every Modulation id by one), and
reloading that composition on the renumbered build put all three values back on
the right parameters. So Preset now sits in its own group at the bottom, where
every other plugin in the fleet puts it.

What must never change is a parameter's **name** — a renamed control silently
loses its saved value, and since only non-defaults are written there is nothing
in the file to notice it by. The append-only rule still holds for an option's
elements, which are stored as numbers.

**The afternoon this cost, and why.** The first measurements said 150 rather
than 81, with some hidden parameters honoured and others not — on what looked
like a freshly launched Arena. A hypothesis fitted it exactly: Arena hides a
whole group but shows a partially-hidden one, and the honoured set was precisely
the four fully-hidden source groups, 7+7+9+4 = 27. It was wrong. Arena reloads
the last composition at startup, that composition already held a Vectrix clip
created under the previous layout, and an instance carries the parameter surface
it was born with. Restarting Arena does not clear it. Mount into a clip that has
never held the plugin.

Related: [old cathode](https://github.com/stoatworks-labs/old-cathode/blob/main/docs/NOTES.md) (`old-cathode`) (raster CRT — the sibling this is deliberately
not), [resolume scopes](https://github.com/stoatworks-labs/resolume-scopes/blob/main/docs/NOTES.md) (`resolume-scopes`) (measurement, not synthesis; `ScopeBuffer` and
`SavedGLState` copied from it), [nib](https://github.com/stoatworks-labs/nib/blob/main/docs/NOTES.md) (`nib`) (edge detection as a *look*;
vectrix's Trace makes an ordered *path*), [ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md),
[pipefail grep q trap](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_pipefail_grep_q_trap.md) (reintroduced here from scratch),
**disclaimer scope** (working-practice note, kept in Claude memory), **release workflow** (working-practice note, kept in Claude memory).
