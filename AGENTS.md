# vectrix — orientation for another LLM (or a newcomer)

An oscillator, a pedalboard and a cathode ray tube in X/Y mode, as two FFGL
plugins for Resolume Arena/Avenue.

`CLAUDE.md` is the command reference. This file is the *why*.

---

## The one idea

**This models a route, not a look.**

Everything on the screen is where a single electron beam was and how long it
lingered there. The bright turnarounds, the ghost repeats, the halo, the rounded
corners of a square are all **consequences** of what the signal does on the way
to the yoke. If you are ever tempted to add one of those directly, stop: either
it already falls out of the chain, or the chain is wrong somewhere and that is
the bug to fix.

The rule is made arithmetic in two places, and both are load-bearing.

**In the renderer, `1/v` is never computed.** A fixed quantum of energy is
deposited per *sample interval* and spread over whatever screen distance the beam
covered in that interval. Brightness proportional to dwell time is then what
"equal energy per unit time" *means*, not a term applied on top of something
else. The fragment shader evaluates the exact convolution of a uniform segment
with a Gaussian spot, which conserves energy for any length and any spot size,
saturates correctly when the beam is fast, and stays finite when it stops — with
no clamp and no divide-by-zero guard anywhere.

**In the signal chain, sources are geometry and brightness is physics.** A source
never modulates `z` to make something look bright. Corner brightening comes from
the slew limiter and the deflection amplifier's bandwidth, at the end of the
chain, which is where a real machine puts them.

### What falls out of it

- A square's corners are bright because the beam decelerates into them. The
  square source is deliberately **not** arc-length parameterised; making the
  speed constant would be tidier and would delete the artefact.
- Sample & Hold draws lines between its steps because the beam travels rather
  than teleporting — the strings come from the slew limiter, not from the source.
- Each delay repeat is smoother and rounder-cornered than the last, because the
  feedback path is low-passed and a low-passed deflection signal has softer
  corners. Nothing draws a "faded copy".
- Saw against saw with **Blank on Retrace** is a clean raster, because the beam
  is genuinely cut off during flyback. This is the cheapest one-click
  demonstration that the rule is being obeyed, which is why it ships as a preset.

---

## The shape of it

CPU synthesises the beam path at audio rate; the GPU integrates its dwell.

```
ProcessOpenGL:
  1 clock.Update(hostTime)     ms/seconds auto-detect; dt clamped to [1/240, 1/24] s
  2 n = SamplesForThisFrame()
  3 engine.SwapContent()       decoded file swapped in -- block boundary ONLY
  4 modulation.Update(...)     FFT + tempo + LFOs, video rate
  5 Resolve(params, mod)       0..1 -> engineering units
  6 [effect] trace.Update()    GPU Sobel -> PBO readback (1 frame late) -> CPU walk
  7 engine.Render(n)           source -> 14 FX blocks, fixed order, in place
  8 beam.Render(...)           decay -> trace -> halation -> glass
```

Internal rate is 96 kHz by default (Draft 48 / Fine 192), so roughly 1600 samples
a frame at 60 fps.

### Directories

- `source/signal/` — everything that makes the beam path. **No GL anywhere in
  here** except `sources/Trace.cpp`, which needs an edge pass. That is what makes
  the whole signal path testable offline.
- `source/render/` — everything that turns a block of samples into a picture.
- `source/signal/fx/Filters.h` — **every coefficient formula in the plugin.** A
  formula written out a second time somewhere else is one that gets corrected in
  one place and not the other.

---

## Traps

Ordered by how much time they will cost you.

### The sample contract is the whole interface — read `signal/Signal.h` first

`dt` is carried **per sample**, not derived from the sample rate. That is what
makes brightness independent of the sample count as an *identity* rather than a
calibration: the light in a frame is `BeamPower × frameDuration × meanBeamCurrent`,
in which `N` does not appear. Derive `dt` from `1/fs` instead and the trace gets
brighter whenever the frame rate dips.

`z` is grid voltage, not alpha and not colour. Making it per-sample is what lets
blanking between strokes, the gate cutting the beam, and retrace blanking all be
the same mechanism instead of three special cases in the renderer.

`±1` is nominal full deflection and **the chain may exceed it**. There is no
clamp in the signal path except the compressor's explicit limiter. Adding one
would delete the ability to overdrive the figure off the screen, which is what an
overdriven amplifier into a yoke does.

### Phase must be integrated, never computed from the clock

`phase += dt * frequency`, in `double`. The obvious `t * frequency` rescales the
whole history the instant the Frequency knob moves, so the figure jumps to a
different point in its cycle on every touch — which for a model of a signal path
is not merely ugly, it is wrong, because a real VCO's phase is continuous through
a frequency change.

`double` is not fussiness. A float accumulator running at 96 kHz loses its
fractional bits within minutes and the figure visibly stalls. Anyone "optimising"
these to float re-introduces the bug in a form that takes ten minutes to show,
which is why `vxtest --drift` exists.

### A parameter change must NOT clear DSP state

This is the exact opposite of the fleet's GPU habit, where rebuilding on a
parameter change is normal and correct. Here, turning the delay time knob must
not drop the tail on the floor and changing the reverb size must not silence the
tank — a real pedal's bucket brigade keeps whatever is in it. `Delay::SetParams`
and `Reverb::SetParams` both carry a comment saying so, and `vxtest --fx` asserts
it.

Bypass **crossfades over 20 ms** rather than switching, for the same reason: a
hard switch puts a step into a deflection voltage, and the delay and reverb
downstream will remember it and hand it back as a click seconds later, long after
the operator has stopped associating it with the switch.

### Stereo must mean de-correlated, not identical

If the chorus runs the same LFO phase on X and on Y, every delayed copy is
displaced along the 45° diagonal and three copies pile up into one blurry double
image. Y's modulation runs a quarter cycle behind (`kAxisPhaseOffset` in
`fx/Modulation.cpp`) and the reverb uses mutually prime line lengths. That single
constant is the difference between a halo with structure and a smeared streak.

### The oversampler must be linear-phase FIR, not polyphase IIR

An IIR half-band is cheaper and steeper and is what an audio effect would use.
Its phase response is not flat — and a frequency-dependent delay between the two
deflection axes **is a skew of the figure**, i.e. a phaser nobody asked for, in a
plugin that has a real phaser three slots later. Phase matters visually here in a
way it does not audibly.

### The ratio is a dropdown, not a slider

A ratio slider lands on 2.001:1 and the figure rotates slowly and forever. The
operator then discovers there is no way to stop it, because every value near the
one they want is also wrong. Exact rationals in a dropdown, with the drift they
might actually want as a separate Detune control they can zero.

### GLSL reserved words, and this plugin walks straight into them

`sample`, `input`, `output`, `filter`, `common`, `active`, `shared` are all
reserved, and a plugin about beam samples wants nearly all of them. Shader errors
surface only at **runtime**, as "the effect does nothing", with the line number in
a file that does not exist. Hence `sampleA`/`sampleB`, `ClipTexture`, `deposit`,
`FilterTransmission`. Do not tidy them back.

### The SDK's traps, all still live

- `ffglex::ScopedFBOBinding` restores the framebuffer and **not the viewport**.
  Every pass sets its own; the host's is restored by hand before the final one.
- Every `ffglex::Scoped*` binding **clears to 0** rather than restoring, so
  allocating an FBO silently unbinds your input texture. `ScopeBuffer::Ensure`
  saves and restores it; that is why this repo uses `ScopeBuffer` and not
  old-cathode's `PassBuffer`.
- `FFGLScopedFBOBinding.h` is not in the umbrella header. Include it by hand.
- `SetParamInfo` clamps a default into 0..1 *before* `SetParamRange` can widen
  it — but **only for `FF_TYPE_STANDARD`**. Counts are `FF_TYPE_INTEGER`, which
  passes through untouched.
- A display-only TEXT parameter **without** a `SetTextParameter` override makes
  `FF_INSTANTIATE_GL` fail for the whole plugin, because the SDK sets every
  default on a fresh instance and deletes it if any set returns FF_FAIL. The
  About block is exactly that. Override it to return FF_SUCCESS.
- `vectrix_core` is an **OBJECT** library. In a STATIC archive the linker may
  drop `CFFGLPluginInfo`'s translation unit, giving a bundle that loads, exports
  `plugMain`, and reports no plugins.
- `glVertexAttribDivisor` is VAO state. Set with the wrong VAO bound, the
  attributes silently become per-vertex and it looks like corrupt geometry.

### NaN poisons the phosphor buffer forever

`NaN × decay = NaN`, and the phosphor ping-pongs, so one NaN survives every frame
for the life of the plugin instance until the host reloads it. Guarded in the
trace vertex shader **and** clamped in the decay pass. Both, not either.

### `$<TARGET_OBJECTS:>` throws away everything except the objects

The OFX target used to splice `vectrix_core` in as
`$<TARGET_OBJECTS:vectrix_core>`. That gives you the compiled objects and
nothing else — no include directories, no compile definitions, none of the
library's usage requirements. GLEW reaches this plugin through
`vectrix_core`'s PUBLIC link to `GLEW::GLEW`, so the OFX build failed on Windows
with `Cannot open include file: 'GL/glew.h'` from inside `FFGL.h`.

Two things made it expensive. **macOS cannot see it at all** — there the SDK
headers reach the system OpenGL framework and GLEW is never linked — so a
platform-neutral-looking line was Windows-only. And the two FFGL bundles built
fine in the same run, because they *link* the core; only the one target that
spliced it failed, which is the clue if you meet this again.

`target_link_libraries(target PRIVATE vectrix_core)` on an OBJECT library gives
both the objects and the usage requirements. Use that.

### `set -o pipefail` plus `grep -q` is a race, and the big binary loses

`tools/verify.sh` runs under `pipefail`. `nm -gU "$bin" | grep -q _OfxGetPlugin`
therefore reports failure **because** the symbol was found: `grep -q` exits at
the first match, `nm` takes SIGPIPE, and `pipefail` propagates it. It is a race
against how fast the producer finishes, so the two small FFGL bundles passed and
the 2.8 MB OFX bundle failed — while the identical command at a prompt printed
the symbol happily. Capture into a variable first; `symbols_of()` does.

### A control can be alive, correct, and still useless

Both halation controls once moved 124 subpixels of a two-megapixel frame by one
part in 255. Every line of the bloom chain worked. The bright pass's knee was
simply left at the header's 0.5 while a moving trace emits a small fraction of
that — `vxtest --point` measures a *parked* beam at 0.92 — so nothing ever got
above the threshold and the bloom buffer stayed empty.

Two things follow. The knee is now set explicitly in `renderParams()` rather than
inherited from a default, and `Halation` maps to 0..4 rather than 0..1, because
the halo is the trace convolved with a wide Gaussian and is inherently far dimmer
than what cast it.

The general point is worth keeping: **`sweep.py` asks whether a control changes
the picture, not whether it changes it usefully.** A control that squeaks past
the zero-difference test is a control nobody can operate. When one is only just
alive, find out why before moving on.

### Detail is not purely a cost control

It changes the sample rate, and the deflection amplifier's bandwidth is clamped
to just under Nyquist by `Svf::Set`. Bandwidth X defaults to about 39.7 kHz,
which is *above* Draft's Nyquist and at 83% of Normal's — so switching Detail
also moves the amplifier's corner, and the picture changes a little for a reason
that is nothing to do with sample count.

That is physically defensible (an amplifier cannot pass what the rate cannot
carry) and it is still a surprise. `vxtest --rate` prints the comparison twice,
once with the amplifier held inside every rate's Nyquist, and about 62% of the
Fine-vs-Normal difference turns out to be the amplifier rather than the renderer.
The remaining margin against the test's 1% limit is not large: raising the
default figure speed or lowering Focus will trip it, and the fix then is to widen
the tolerance only after checking the amplifier is not the cause.

### Denormals, on x86 only

A reverb tail decaying below ~1e-38 triggers denormal handling, so the symptom is
a frame-rate collapse that starts *after* the signal stops — which reads as a
leak rather than as an FPU stall. Every feedback path goes through
`vectrix::flush`. This has not been tested on an Intel Mac.

---

## Relationship to the three siblings it sounds like

This is the fleet's fourth CRT-adjacent plugin and the overlap question is fair.

| sibling | what it is | why this is not it |
|---|---|---|
| **old-cathode** | an analogue *television* signal path — composite encode, damage, decode — on a **raster** CRT | vectrix is a **vector** CRT. No raster, no subcarrier, no scanlines: the beam goes where the signal says. old-cathode damages someone else's picture; vectrix generates the signal in the first place |
| **resolume-scopes** | a vectorscope as a **measurement instrument**, whose correctness claim is that the trace lands in the box | vectrix is a **synthesiser**. scopes measures an incoming picture and must not lie about it; vectrix has no incoming measurement to be faithful to |
| **nib** | edge detection as a **look** — XDoG steered by a flow field | vectrix's Trace source produces an **ordered path the beam walks**, which is a different problem: contour extraction and stroke ordering, not filtering. nib makes an image of lines; vectrix makes a route |

Code genuinely shared: `ScopeBuffer` and `SavedGLState` are copied from
resolume-scopes, and the halation pass is old-cathode's.

---

## Checking your work

`tools/vxtest` drives the real plugin class through the real FFGL sequence in a
headless CGL 4.1 core context. Time comes from the frame counter, so every run is
deterministic.

The invariant tests are the point of the harness — they are what turns the
paragraph at the top of this file into something a machine can check:

| flag | what it proves | what it cannot |
|---|---|---|
| `--energy` | one sweep at ten speeds spanning 100:1 deposits the same total light to within 0.5%. **The test the plugin exists to pass.** | nothing about whether it *looks* right |
| `--dwell` | measured line density tracks `1/v` on a decelerating segment | — |
| `--rate` | the same figure at N = 500/2000/8000 renders the same | that a host's real frame timing behaves |
| `--point` | a stationary beam reaches the analytic peak and stays finite | — |
| `--blank` | `z = 0` deposits nothing and does not leak past its endpoints | — |
| `--identity` | the effect build at Beam 0 is bit-identical to its input | that the input really is premultiplied |
| `--fx` | the Householder matrix is orthogonal; a parameter change preserves the reverb tail | that any effect sounds or looks like its name |
| `--drift` | ten minutes of phase accumulation stays exact | — |
| `tools/sweep.py` | every parameter changes the output | that it changes it *correctly* |

`sweep.py` is the only thing that catches a mistyped uniform name, since
`glGetUniformLocation` returns -1 and `glUniform(-1)` is a documented no-op — so
a control can be stone dead while everything compiles, links, loads and renders.

---

## What is genuinely verified, and what is assumed

**Verified**, by `tools/verify.sh` on one M4 Max:

- The energy, dwell, rate, point, blank and identity invariants above.
- The Householder matrix preserves norm to float precision.
- Every parameter measurably changes the output.
- The macOS bundles are universal (checked with `lipo`, not the build log) and
  export `plugMain`.
- libopus, libogg and libopusfile build from source as universal static
  libraries and `opusfile` links and opens files.

**Assumed, and not yet checked:**

- ~~Never rendered through Resolume.~~ **Both plugins were loaded and run in
  Resolume Arena on 2026-08-17** (Allan's own report), so the parameter groups,
  the dropdowns and Arena's real texture sizes are confirmed to work. What that
  does *not* settle is whether the input is genuinely premultiplied — the
  effect build divides alpha out on the way in, and a picture that looks right
  on opaque footage would look right either way.
- **Whether Resolume's `SetTime` for a *source* plugin is the clip transport or
  the composition clock is not known.** It matters for the Locked file sync mode.
- **The trace readback has never been measured in a real host.** This is the
  first plugin in the fleet to call `glReadPixels` inside `ProcessOpenGL`. The
  PBO path should make it asynchronous; that is reasoning, not measurement.
- **`GL_RG32F` blending has only been exercised on Apple silicon.** It is
  required in GL 4.1 but runs at reduced rate on some parts.
- **Nothing has been run on Intel**, so the denormal flushing is untested where
  it matters.
- The Windows build compiles in CI and has never been run.
- No performance figure comes from CI — hosted macOS runners have no GPU.
- **The OpenFX build has been rendered and eyeballed, never opened in a real
  host.** Resolve, Nuke and Natron have all not seen it. Its renderer is a CPU
  mirror of the GLSL rather than the GLSL, so the two agreeing is a claim about
  two transcriptions, not one measured fact — `--identity`-style passthrough is
  exact there, but nothing cross-checks the CPU trace against the GPU trace
  pixel for pixel.

---

## Things deliberately not done

Streaming audio from disk (decode-to-memory with a cap and a truncation note
instead); a user-patchable FX order (fourteen blocks is 14! orderings, none of
which could be tested); two source slots; convolution reverb; multiband anything;
MinBLEP or BLIT oscillators; a DSP thread (the whole chain is about 0.4 ms); GPU
compute for the trace (GL 4.1 has none); spot astigmatism; screen burn.

And the one that is obviously in the spirit and is a v2 headline rather than a
v0.1 feature: **an actual X/Y audio output driving a real oscilloscope.** That is
a platform-audio dependency and a second clock domain, and it deserves to be
designed rather than bolted on.

---

## Conventions

Tabs. British spelling in prose. Comments explain *why*, and especially what goes
wrong — a comment that restates the code earns nothing. Public MIT repo:
"commit" means commit **and** push.

## Notes

`docs/NOTES.md` carries this repo's working notes — current status, decisions
already made, and the traps that have actually bitten. Read it before changing
anything non-obvious. Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).
