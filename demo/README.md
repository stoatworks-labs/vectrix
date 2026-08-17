# demo/ — the browser demo

Live at **https://vectrix-demo.stoatworks-labs.com**.

**This is not the plugin.** The tube is: all six passes are the GLSL from
[`source/render/shaders/`](../source/render/shaders), copied across unedited and
run in WebGL2 in the same order `BeamGeometry::Render` runs them. The signal
chain is not: it is a JavaScript port of part of [`source/signal/`](../source/signal),
and what is missing is said on the page rather than approximated.

The page carries the banner and lists what it does not reproduce at the foot.

## What is here and what is not

| | ported | not ported |
|---|---|---|
| sources | Oscillator (all seven waveforms, the exact-rational ratio, phase, PWM, hard sync, blank on retrace), Shape (all seven) | Wireframe, Audio File, Trace Input |
| effects | Rectifier, Slew, Drive/Fold, Ring Mod, Bitcrush, Phaser, Delay, Deflection Amplifier | VCA, Gate, Compressor, Flanger, Chorus, Reverb |
| renderer | everything — decay, trace, bright, blur, glass, both builds | — |
| host | — | tempo sync, the FFT/LFO modulation matrix, the audio-file path |

Ninety-one controls of the plugin's hundred and fifty. The seven effects that are
here are the ones that change the *figure*; the five that are not change its
dynamics, and a second implementation of a compressor that nothing checks would
be worth less than an honest gap.

The **Bundle** dropdown above the canvas picks which of the two plugins is being
shown. *Vectrix Trace* is the effect build, with the clip painted **on** the tube
face — curved with the glass, behind the graticule, sharing the halation. Taking
the clip *as* the signal is the Trace source, which is the part of that plugin
not ported.

## What is worth doing on it

- **Raster.** Saw against saw with **Blank on Retrace** on. The flyback lines are
  absent because the beam is genuinely cut off during them, not because anything
  declined to draw them. It is the cheapest one-click demonstration on the page
  that the house rule is being obeyed.
- **Constellation.** Sample & Hold on a P7. The strike is blue, the trail is
  yellow-green and builds *behind* the beam, and the lines between the steps come
  from the slew limiter — the beam travels rather than teleporting.
- **Square.** The corners are brighter than the sides because the beam
  decelerates into them. The square source is deliberately not arc-length
  parameterised; making the speed constant would delete the artefact.
- **Fold, on anything.** The Serge wavefolder turns a circle into a rosette, and
  the bright spots in it are turnarounds.
- **Phaser at Mix 1.0 on X only.** No notches at all, only phase shift — which
  between X and Y is a rotation of the figure. Not an affine transform, so no
  shader could produce it.

## One thing that looks like a bug on this page and is the phosphor being right

**Whether a figure stands still is a race between its rate and the frame rate.**
At 96 kHz and 60 fps a frame is 1600 samples, and the beam only gets round the
whole figure if it is drawing it at least sixty times a second. The defaults are
set for that — Frequency X 0.77 is about 120 Hz, so a 3:2 Lissajous closes twice
inside a frame, and Shape Rate 0.88 is about 60 Hz, one closed loop per frame.

Take Frequency X well below 60 Hz and it becomes a comet instead: one frame's
worth of route, with nothing behind it, because a P31 at persistence ×1 keeps
nothing between frames — sixteen microseconds against a sixteen millisecond
frame. That is not the model failing, it is what a P31 *is*, and it is why every
oscilloscope photograph of a slow sweep was taken with the shutter open. Raise
Persistence or choose P7 for a tube that remembers.

## Keeping this page in step with the C++

Four things here mirror numbers that live in `source/` and nothing enforces them
the way `check_shaders.py` enforces the GLSL. If you change any of them there,
change them in `plugin.js` too:

| in the C++ | in `plugin.js` |
|---|---|
| `kDefaults` in `Vectrix.cpp` | the `default:` on each parameter |
| `kPresets` in `Presets.h` | the `presets:` block, mirrored column for column |
| `Resolve()` in `Controls.cpp` | `resolve()` |
| `renderParams()` in `Vectrix.cpp` | the `r.render` half of `resolve()` |

Two of those carry a trap. **`kBeamOffRamp`**: the Beam control fades linearly to
a true zero over the bottom two per cent of its travel, because the exponential
alone bottoms out at a tenth of nominal and "Beam 0" would leave the gun on.
**Halation runs to 4×, and the bright pass's knee is 0.04, not the header's
0.5** — at 0.5 the pass finds nothing on an ordinary picture and both halation
controls are measurably dead.

`sides`, `driveFolds`, `crushBits`, `petalN`, `petalD` and `phaseStages` are
FF_TYPE_INTEGER in the plugin and hold the integer itself, not a 0..1 fraction.
The demo kit has no integer control, so they are dropdowns of their own values
and `integerDefault()` converts. **Write the plugin's number, never the index.**

## Editing it

- `plugin.js` — the shaders, the signal port, the parameter declarations. **When
  a shader in [`source/render/shaders/`](../source/render/shaders) changes,
  change it here too.** That is *enforced*: `demo/tools/check_shaders.py`
  compares all nine constants to the C++ character for character, and the repo's
  `tools/verify.sh` runs it. Nine constants and not five, because the plugin assembles each stage out of
  shared preludes and the demo has to carry the pieces, or a change to a prelude
  would go unnoticed in every shader that includes it.
- The shaders quote uniform names in backticks in their comments, and a backtick
  cannot appear raw in a JavaScript template literal, so `plugin.js` escapes them
  as `` \` ``. The checker decodes that one escape and rejects every other
  backslash — there are none in the C++, so a second escape could only be
  something being hidden.
- `vendor/` — the shared kit, vendored from `stoatworks-backend/resolume-demo/`.
  **Do not edit these.** Fix the master and re-run its `sync.sh`, which lists this
  repo; `sync.sh --check` reports drift.
- There is deliberately no `page` in `plugin.js` until the website project page
  exists — the kit leaves a missing link out rather than rendering one that 404s.

## Serving it locally

No build step.

```bash
python3 -m http.server 8178 --directory demo
```

Two traps when checking it in a browser pane rather than by hand. A **hidden pane
never fires `requestAnimationFrame`**, so take a screenshot before expecting
anything to have rendered — and evaluating JavaScript in the pane hides it, which
means any measurement that waits on rAF will simply never resolve. And the
context is `preserveDrawingBuffer: false`, so `readPixels` from outside the render
callback returns zeros: it looks like a passing byte comparison and is not one.

State goes in the query string, which is what **Copy link** produces:

```
?source=1&shape=4&shapeRate=0.9&phosphor=3
```

## Deploying

From the repo root:

```bash
cf-run npx wrangler deploy
```

Then verify by content rather than by status code — a wrong page still answers
200:

```bash
curl -s 'https://vectrix-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'
```

`.assetsignore` keeps `README.md` and `tools/` off the public URL; both 404 in
production, which is worth re-checking after any change to what lives in here.
