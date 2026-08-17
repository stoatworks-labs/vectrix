# vectrix

An oscillator, a pedalboard and a CRT in X/Y mode, as two FFGL plugins for
Resolume Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) +
Windows `.dll`. Public MIT repo.

Read `AGENTS.md` before changing the signal path or the beam maths.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Without Opus (drops 3 submodules): add `-DVECTRIX_WITH_OPUS=OFF`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render a frame offline: `./build/vxtest --out /tmp/frame.png`
- List parameters: `./build/vxtest --list`

## OpenFX build
Built by default; copy `build/Vectrix.ofx.bundle` to `/Library/OFX/Plugins`.
Disable with `-DBUILD_OFX=OFF`.

## Verify
- Everything: `tools/verify.sh`
- Brightness is independent of beam speed: `./build/vxtest --energy`
- Density follows 1/v: `./build/vxtest --dwell`
- Independent of sample count: `./build/vxtest --rate`
- No dead controls: `python3 tools/sweep.py`

## Notes
- **Models a route, not a look.** Bright turnarounds, ghost repeats and halos are
  consequences. If you are tempted to draw one, the chain is wrong somewhere.
- **`1/v` is never computed.** Energy per sample interval is spread over the
  distance covered; the dwell law falls out. Do not "simplify" it.
- **`dt` lives in the sample**, not in a block-wide sample rate. That is what
  makes brightness independent of sample count as an identity.
- **`z` is grid voltage**, not alpha. Blanking is one mechanism, used three ways.
- **Phase is integrated in `double`**, never `t * frequency`. Changing this
  re-introduces a bug that takes ten minutes to appear (`vxtest --drift`).
- **A parameter change must NOT clear DSP state** — the opposite of the fleet's
  GPU habit. Delay tails and reverb tanks survive knob moves.
- **Stereo means de-correlated**: Y's modulation runs 90° behind X's.
- The wavefolder's oversampler is **linear-phase FIR**; an IIR half-band's phase
  response is a skew of the figure.
- Every coefficient formula lives in `signal/fx/Filters.h` and nowhere else.
- Phosphor decay constants are **measured**, in `render/Phosphor.cpp`.
- `sample`, `input`, `output`, `filter`, `common`, `active` are GLSL reserved
  words. Shader errors surface only at runtime, in the diagnostics log.
- `ScopedFBOBinding` restores the framebuffer and **not** the viewport; every
  `Scoped*` binding clears to 0 rather than restoring.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can
  widen it; counts are `FF_TYPE_INTEGER`, which is exempt.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no
  host can instantiate the plugin at all.
- `vectrix_core` is an OBJECT library, not STATIC — the plugin registers itself
  from a file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It exists for the one failure that actually
happens: a shader that will not compile, which otherwise looks like "the effect
does nothing" with no message anywhere. `~/Library/Logs/vectrix/`.
