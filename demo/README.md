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
- **Square, with Persistence up.** The corners are brighter than the sides
  because the beam decelerates into them. The square source is deliberately not
  arc-length parameterised; making the speed constant would delete the artefact.
- **Fold, on anything.** The Serge wavefolder turns a circle into a rosette, and
  the bright spots in it are turnarounds.
- **Phaser at Mix 1.0 on X only.** No notches at all, only phase shift — which
  between X and Y is a rotation of the figure. Not an affine transform, so no
  shader could produce it.

## Two things that look like bugs on this page and are the plugin's

Both are reproduced rather than corrected, because a demo that quietly fixes the
plugin's data stops being evidence about the plugin.

**A figure is only drawn whole in one frame if its rate is above the frame
rate.** At 96 kHz and 60 fps a frame is 1600 samples; at the shipped default of
Frequency X 0.55 the oscillator runs at 8.2 Hz, so each frame contains about a
seventh of one cycle. On a P31 at persistence ×1 nothing carries over — sixteen
microseconds against a sixteen millisecond frame — so what you see is a comet,
not a Lissajous. Raise Persistence, raise Frequency, or choose P7, and the figure
appears. The same applies to Shape Rate, whose default 0.5 is 1.4 Hz against a
`ShapeParams` struct default of 30 Hz.

**Where `Presets.h` sets an integer-typed control to a fraction, the plugin
rounds it to that control's minimum.** `Vectrex` asks for 0.60 Bits and gets one
bit, which quantises each axis to ±1 and collapses the figure to two dashes;
`Star` asks for 0.09 Sides and gets three; `Rosette Fold` and `Fuzz Box` ask for
0.30 and 0.20 Folds and get one. FF_TYPE_INTEGER parameters hold the integer
itself, not a 0..1 fraction — the same distinction `Controls.h` documents for
option parameters.

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
