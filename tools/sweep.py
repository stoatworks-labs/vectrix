"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at both ends of its range against a baseline, and
report any that made no difference.

    python3 tools/sweep.py

Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**Most of this plugin is dead most of the time, by design.** There are five
sources and only one runs at once, and fourteen effects that each default to
bypassed. A naive sweep would report about two thirds of the parameter list as
broken and every one of those reports would be correct-and-useless. So each
parameter carries a CONTEXT: the source it needs selected and the block it needs
switched on. That table is the real content of this file.

**A control that only drives the CLOCK looks dead when the clock is pinned.**
Anything whose effect is a rate needs more than one frame to show it, so the
baseline renders several frames and the rate-ish parameters render more.

**Never sweep the FFT buffer parameter.** It is 64 elements the host writes; the
single float value the harness would set is meaningless, and sweeping it reports
a false dead every time.

**Never sweep the About block.** Those are buttons that open a web browser, and
sweeping them opens one tab per press.

**Persistence needs the phosphor to have something to remember.** P31's real
decay is 16 microseconds, so at 60 fps it shows no trail at all -- correctly.
Sweeping Persistence against the default phosphor compares two identical
trail-free frames. It is swept on P7.
"""
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = str(ROOT / "build" / "vxtest")
SCRATCH = tempfile.mkdtemp(prefix="vxsweep")

WIDTH, HEIGHT = 960, 540
FRAMES = 8

# Parameters that cannot or must not be swept, with the reason.
SKIP = {
    "Crossfade Time": "only differs while the delay time is CHANGING, "
                      "and a sweep holds every parameter still",
    "Audio": "an FFT buffer the host fills; its float value is meaningless",
    "About": "a display-only text line",
    "User guide": "a button that opens a web browser",
    "Project page": "a button that opens a web browser",
    "Source on GitHub": "a button that opens a web browser",
    "Support the work": "a button that opens a web browser",
    "Reset": "an event whose whole effect is to undo itself",
    "Audio File": "a file path; there is no fixture to point it at",
    "Seed": "changes noise, which several sources do not use at all",
}

# The world each parameter needs before it can be seen. Everything not listed
# here is swept against BASE with the oscillator running and no effects on.
BASE = {
    "Source": 0,
    "Beam": 0.5,
    "Focus": 0.35,
    "Face Black": 0.85,
    "Graticule": 0.4,
    "Halation": 0.4,
    "Mix": 1.0,
}

OSC = {}
SHAPE = {"Source": 1}
WIRE = {"Source": 2}

# Each effect's own controls need that effect switched on, and several need the
# signal to be doing something the effect can act on.
def fx(enable, **extra):
    ctx = dict(extra)
    ctx[enable] = 1
    return ctx


QUIET_GATE = {"Gate Threshold": 1.0, "Gate Hold": 0.0, "Gate Release": 0.15}

CONTEXT = {
    # -- sources ---------------------------------------------------------
    "Wave X": OSC, "Wave Y": OSC, "Frequency X": OSC, "Ratio": OSC,
    "Free Y": {"Free Y": 1}, "Frequency Y": {"Free Y": 1},
    # 0 and 1 turns are the SAME phase, so the two ends of the range are
    # identical and the control reads dead however well it works.
    "Phase Y": {"_high": 0.5},
    "Detune": OSC, "Hard Sync": OSC,
    # Width only exists on a pulse wave; on a sine it is correctly dead.
    "Width X": {"Wave X": 4}, "Width Y": {"Wave Y": 4},
    # Blanking the flyback of a sine does nothing -- there is no flyback.
    "Blank on Retrace": {"Wave X": 2, "Wave Y": 2},

    "Shape": SHAPE, "Shape Rate": SHAPE,
    "Sides": {"Source": 1, "Shape": 3},
    "Inner Radius": {"Source": 1, "Shape": 4},
    "Petals": {"Source": 1, "Shape": 5}, "Petal Divisor": {"Source": 1, "Shape": 5},
    "Pen Offset": {"Source": 1, "Shape": 6},

    "Mesh": WIRE, "Mesh Detail": WIRE, "Camera": WIRE,
    "Spin X": WIRE, "Spin Y": WIRE, "Spin Z": WIRE,
    "Scroll": {"Source": 2, "Mesh": 3},
    # The beam-path controls only exist for the sources that walk a path.
    "Refresh Rate": WIRE, "Retrace Speed": WIRE,

    # Trace and the audio file need input this harness has no fixture for.
    # They are skipped rather than reported, because "dead" would be a lie.

    # -- effects ---------------------------------------------------------
    # Level defaults to the middle of 0..2, which is unity -- so a VCA switched
    # on at its default does nothing at all, correctly.
    "VCA": fx("VCA", **{"Level": 0.9}),
    "Level": fx("VCA"),
    "VCA Routing": fx("VCA", **{"Level": 0.9}),

    # The gate is a threshold, so it needs a signal quiet enough to cross it.
    # The gate needs three things at once before it can be seen, and the
    # default figure supplies none of them. A threshold the radial detector
    # actually crosses (the Lissajous swings from near the origin out to root
    # two, so 0 dBFS is crossed twice a cycle); no Hold, because 10 ms of hold
    # against a figure whose period is 8 ms means the gate starts to close and
    # is reopened before it gets anywhere -- correct behaviour that looks
    # exactly like a dead control; and a release short enough to matter.
    "Gate": fx("Gate", **QUIET_GATE),
    "Gate Threshold": fx("Gate", **{"Gate Hold": 0.0, "Gate Release": 0.15}),
    "Gate Attack": fx("Gate", **QUIET_GATE),
    "Gate Hold": fx("Gate", **{"Gate Threshold": 1.0, "Gate Release": 0.15}),
    "Gate Release": fx("Gate", **{"Gate Threshold": 1.0, "Gate Hold": 0.0}),
    "Gate Mutes Beam": fx("Gate", **QUIET_GATE),

    # The compressor needs something above its threshold to compress.
    "Compressor": fx("Compressor", **{"Deflection": 0.9}),
    "Threshold": fx("Compressor", **{"Deflection": 0.9}),
    "Ratio": fx("Compressor", **{"Deflection": 0.9}),
    "Knee": fx("Compressor", **{"Deflection": 0.9, "Ratio": 0.8}),
    "Attack": fx("Compressor", **{"Deflection": 0.9}),
    "Release": fx("Compressor", **{"Deflection": 0.9}),
    "Makeup": fx("Compressor", **{"Deflection": 0.9, "Auto Makeup": 0}),
    "Auto Makeup": fx("Compressor", **{"Deflection": 0.9}),
    "Ceiling": fx("Compressor", **{"Deflection": 1.0, "Rail Limiting": 1}),
    "Rail Limiting": fx("Compressor", **{"Deflection": 1.0, "Ceiling": 0.1}),
    "Comp Routing": fx("Compressor", **{"Deflection": 0.9}),

    "Rectifier": fx("Rectifier"), "Full Wave": fx("Rectifier"),
    "Fold Point": fx("Rectifier"), "Rectify Routing": fx("Rectifier"),

    # A slew limit only shows on a signal with somewhere to go quickly.
    "Slew Limiter": fx("Slew Limiter", **{"Wave X": 2, "Wave Y": 2}),
    "Rise": fx("Slew Limiter", **{"Wave X": 2, "Wave Y": 2}),
    "Fall": fx("Slew Limiter", **{"Wave X": 2, "Wave Y": 2, "Link Rise/Fall": 0}),
    "Link Rise/Fall": fx("Slew Limiter", **{"Wave X": 2, "Wave Y": 2, "Rise": 0.3, "Fall": 0.9}),
    "Slew Routing": fx("Slew Limiter", **{"Wave X": 2, "Wave Y": 2, "Rise": 0.3}),

    "Drive": fx("Drive", **{"Gain": 0.6}),
    "Gain": fx("Drive"),
    "Fold": fx("Drive", **{"Gain": 0.3}),
    "Folds": fx("Drive", **{"Gain": 0.3, "Fold": 0.9}),
    "Oversample": fx("Drive", **{"Gain": 0.7, "Fold": 0.9, "Frequency X": 0.75}),
    "Drive Routing": fx("Drive", **{"Gain": 0.3, "Fold": 0.9}),

    "Ring Mod": fx("Ring Mod", **{"Ring Depth": 0.8}),
    # The carrier is unused in Cross routing, which is the default.
    "Carrier": fx("Ring Mod", **{"Ring Depth": 0.8, "Ring Routing": 0}),
    "Carrier Wave": fx("Ring Mod", **{"Ring Depth": 0.8, "Ring Routing": 0}),
    "Ring Depth": fx("Ring Mod"),
    "Ratio Lock": fx("Ring Mod", **{"Ring Depth": 0.8, "Ring Routing": 0}),
    "Ring Routing": fx("Ring Mod", **{"Ring Depth": 0.8}),

    "Bitcrush": fx("Bitcrush", **{"Bits": 3}),
    "Bits": fx("Bitcrush"),
    "Sample Rate": fx("Bitcrush"),
    "Crush Routing": fx("Bitcrush", **{"Bits": 3}),

    "Phaser": fx("Phaser"), "Stages": fx("Phaser"), "Phaser Rate": fx("Phaser"),
    "Phaser Depth": fx("Phaser"), "Centre": fx("Phaser"),
    "Phaser Feedback": fx("Phaser"), "Phaser Mix": fx("Phaser"),
    "Phaser Routing": fx("Phaser", **{"Phaser Mix": 1.0}),

    "Flanger": fx("Flanger"), "Flanger Rate": fx("Flanger"),
    "Flanger Depth": fx("Flanger"), "Flanger Delay": fx("Flanger"),
    "Flanger Feedback": fx("Flanger"), "Flanger Mix": fx("Flanger"),
    "Flanger Routing": fx("Flanger", **{"Flanger Mix": 1.0}),

    "Chorus": fx("Chorus"), "Chorus Rate": fx("Chorus"),
    "Chorus Depth": fx("Chorus"), "Chorus Delay": fx("Chorus"),
    "Chorus Mix": fx("Chorus"),

    # A delay needs long enough for a repeat to arrive.
    "Delay": {"Delay": 1, "Delay Mix": 0.7, "_frames": 40},
    "Delay Time": {"Delay": 1, "Delay Mix": 0.7, "_frames": 40},
    "Delay Feedback": {"Delay": 1, "Delay Mix": 0.7, "_frames": 60},
    "Delay Tone": {"Delay": 1, "Delay Mix": 0.7, "Delay Feedback": 0.8, "_frames": 60},
    "Delay Bass Cut": {"Delay": 1, "Delay Mix": 0.7, "Delay Feedback": 0.8, "_frames": 60},
    "Delay Mix": {"Delay": 1, "_frames": 40},
    "Delay Routing": {"Delay": 1, "Delay Mix": 0.7, "_frames": 40},
    "Sync to Tempo": {"Delay": 1, "Delay Mix": 0.7, "_frames": 40},
    "Division": {"Delay": 1, "Delay Mix": 0.7, "Sync to Tempo": 1, "_frames": 40},

    "Reverb": {"Reverb": 1, "Reverb Mix": 0.7, "_frames": 40},
    "Pre-Delay": {"Reverb": 1, "Reverb Mix": 0.7, "_frames": 40},
    "Decay": {"Reverb": 1, "Reverb Mix": 0.7, "_frames": 60},
    "Damping": {"Reverb": 1, "Reverb Mix": 0.7, "_frames": 40},
    "Diffusion": {"Reverb": 1, "Reverb Mix": 0.7, "_frames": 40},
    "Size": {"Reverb": 1, "Reverb Mix": 0.7, "_frames": 40},
    "Shimmer": {"Reverb": 1, "Reverb Mix": 0.7, "_frames": 60},
    "Reverb Mix": {"Reverb": 1, "_frames": 40},

    # -- the tube --------------------------------------------------------
    # Persistence on P31 compares two trail-free frames, correctly. P7 has a
    # 174 ms slow layer, which at 60 fps is a trail you can measure.
    "Persistence": {"Phosphor": 3, "_frames": 20},
    "Halation Radius": {"Halation": 0.9},
    "Corner Radius": {"Curvature": 0.3},
    "Contrast Filter": {"Face Black": 0.6},
    "Blanking Leak": {"Wave X": 2, "Wave Y": 2, "Blank on Retrace": 1},
    # The amplifier's slew limit only shows on a figure fast enough to outrun
    # it. The default figure's steepest slope is well inside even the slowest
    # setting, so it is correctly invisible there.
    "Amp Slew": {"Frequency X": 0.95},
    # These act on what is behind the glass; the source build has nothing there.
    "Face Black": {"_effect": True},
    "Contrast Filter": {"_effect": True, "Face Black": 0.6},

    # -- modulation ------------------------------------------------------
    # The LFOs do nothing until a slot names a target for them to drive.
    "LFO 1 Rate": {"Mod 1": 1, "Mod 1 Amount": 1.0, "LFO 1 Depth": 0.8, "_frames": 40},
    "LFO 1 Depth": {"Mod 1": 1, "Mod 1 Amount": 1.0, "LFO 1 Rate": 0.6, "_frames": 40},
    "LFO 2 Rate": {"Mod 2": 1, "Mod 2 Amount": 1.0, "LFO 2 Depth": 0.8, "_frames": 40},
    "LFO 2 Depth": {"Mod 2": 1, "Mod 2 Amount": 1.0, "LFO 2 Rate": 0.6, "_frames": 40},
    "Mod 1": {"Mod 1 Amount": 1.0, "LFO 1 Depth": 0.8, "_frames": 40},
    "Mod 1 Amount": {"Mod 1": 1, "LFO 1 Depth": 0.8, "_frames": 40},
    "Mod 2": {"Mod 2 Amount": 1.0, "LFO 2 Depth": 0.8, "_frames": 40},
    "Mod 2 Amount": {"Mod 2": 1, "LFO 2 Depth": 0.8, "_frames": 40},
    "Mod 3": {"Mod 3 Amount": 1.0, "LFO 1 Depth": 0.8, "_frames": 40},
    "Mod 3 Amount": {"Mod 3": 1, "LFO 1 Depth": 0.8, "_frames": 40},
    "Mod 4": {"Mod 4 Amount": 1.0, "LFO 1 Depth": 0.8, "_frames": 40},
    "Mod 4 Amount": {"Mod 4": 1, "LFO 1 Depth": 0.8, "_frames": 40},
}

# Sources with no fixture in the harness. Reported as skipped, not as dead.
SKIP_SOURCES = {
    "File Rate", "Loop", "Start", "End", "Mono Mode", "Swap X/Y", "Invert Y",
    "Lock to Clip",
    "Edge Threshold", "Stability", "Simplify", "Max Strokes",
}

# The band selectors read the same mean three times over a flat synthetic
# spectrum, so they need a shaped one -- which this harness has no way to inject
# without a host. Skipped honestly.
SKIP_SOURCES |= {"Mod 1 Band", "Mod 2 Band", "Mod 3 Band", "Mod 4 Band"}


def parameters():
    """id, name, kind, low, high from the harness's own declaration."""
    out = subprocess.run([BIN, "--list"], capture_output=True, text=True)
    if out.returncode != 0:
        print("could not list parameters:", out.stdout, out.stderr)
        sys.exit(1)

    # `  0  Detail                   option        1.0000   [0 .. 1]`
    found = []
    for line in out.stdout.splitlines():
        m = re.match(
            r"\s*(\d+)\s+(.+?)\s{2,}(\S+)\s+([\d.eE+-]+)\s+\[\s*([\d.eE+-]+)\s*\.\.\s*([\d.eE+-]+)\s*\]",
            line,
        )
        if m:
            found.append(
                (int(m.group(1)), m.group(2).strip(), m.group(3),
                 float(m.group(5)), float(m.group(6)))
            )
    return found


def render(path, overrides, frames, effect=False):
    args = [BIN, "--out", path, "--size", f"{WIDTH}x{HEIGHT}", "--frames", str(frames)]
    if effect:
        # Some controls act on what is BEHIND the glass, and the source build
        # has nothing behind it. They are correctly dead there.
        args.append("--effect")
    merged = dict(BASE)
    merged.update({k: v for k, v in overrides.items() if not k.startswith("_")})
    for name, value in merged.items():
        args += ["--set", f"{name}={value}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", " ".join(args), r.stdout, r.stderr)
        sys.exit(1)
    return pathlib.Path(path).read_bytes()


def pixels(png):
    """Raw RGBA out of the harness's own PNG, so nothing else is a dependency."""
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = int.from_bytes(png[i:i + 4], "big")
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(data[0:4], "big")
            height = int.from_bytes(data[4:8], "big")
        elif kind == b"IDAT":
            idat += data
        i += 12 + length

    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray(width * height * 4)
    previous = bytearray(stride)
    pos = 0
    for row in range(height):
        filter_type = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            b = previous[x]
            c = previous[x - 4] if x >= 4 else 0
            if filter_type == 1:
                line[x] = (line[x] + a) & 255
            elif filter_type == 2:
                line[x] = (line[x] + b) & 255
            elif filter_type == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif filter_type == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        out[row * stride:(row + 1) * stride] = line
        previous = line
    return out


def difference(a, b):
    pa, pb = pixels(a), pixels(b)
    if len(pa) != len(pb):
        return 1.0, len(pa)
    changed = sum(1 for x, y in zip(pa, pb) if x != y)
    return changed / max(len(pa), 1), changed


def main():
    if not pathlib.Path(BIN).exists():
        print(f"{BIN} is not built")
        return 1

    dead = []
    skipped = []
    checked = 0

    for pid, name, kind, low, high in parameters():
        if name in SKIP:
            skipped.append((name, SKIP[name]))
            continue
        if name in SKIP_SOURCES:
            skipped.append((name, "needs input this harness has no fixture for"))
            continue

        context = CONTEXT.get(name, {})
        frames = context.get("_frames", FRAMES)
        effect = context.get("_effect", False)
        low = context.get("_low", low)
        high = context.get("_high", high)

        lo = dict(context)
        hi = dict(context)
        lo[name] = low
        hi[name] = high

        a = render(f"{SCRATCH}/{pid}_lo.png", lo, frames, effect)
        b = render(f"{SCRATCH}/{pid}_hi.png", hi, frames, effect)
        fraction, count = difference(a, b)
        checked += 1

        # Counts, not percentages: a control that moves three pixels and one
        # that moves none look identical as a percentage.
        if count == 0:
            dead.append(name)
            print(f"DEAD  {pid:4d}  {name}")
        else:
            print(f"ok    {pid:4d}  {name}  ({count} subpixels, {fraction * 100:.2f}%)")

    print()
    for name, why in skipped:
        print(f"skip  {name}: {why}")

    print(f"\n{checked} swept, {len(dead)} dead, {len(skipped)} skipped")
    if dead:
        print("\nDEAD CONTROLS: " + ", ".join(dead))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
