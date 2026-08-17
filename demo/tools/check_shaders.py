"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.

------------------------------------------------------------------- why

`demo/plugin.js` holds nine GLSL fragments and so does `source/render/shaders/`.
That is two copies of the same text, and two copies drift -- quietly, because a
demo that renders a *plausible* picture looks exactly like a demo that renders
the right one. The whole claim of these pages is that they run the plugin's own
shader rather than something reimplemented to look similar, so the claim needs
something enforcing it.

Nothing else can. `vxtest` drives the real plugin class through the real FFGL
sequence and has no idea this page exists.

------------------------------------------------------------------- what it does

Pulls each `R"( ... )"` body out of the C++ and each matching backtick literal
out of `plugin.js`, and compares them exactly -- no whitespace normalisation, no
comment stripping. A comment that has been updated on one side and not the other
is exactly the drift worth catching, because comments in this repo carry the
measurements that justify the code: the phosphor taus, the 4.5-sigma pedestal,
the reason `max()` would be wrong in the trace pass.

The one transformation is a decode, not a normalisation. Several of these
shaders quote a uniform name in a comment with backticks, and a backtick cannot
appear raw inside a JavaScript template literal, so `plugin.js` escapes it as
\\`. This unescapes that and *rejects any other backslash on the JS side* --
there are none anywhere in the C++, so a second escape could only be somebody
hiding a difference.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# JS constant, C++ file, C++ symbol.
#
# Nine and not five: the plugin assembles every stage out of shared pieces --
# the numeric constants for both stages, the emission/graticule/CDF helpers for
# the fragment ones -- and the demo has to carry the pieces rather than the
# assembled strings, or a change to a prelude would go unnoticed in every shader
# that includes it.
SHADERS = [
    ("CONSTANTS", "source/render/shaders/Prelude.cpp", "kConstants"),
    ("FRAGMENT_HELPERS", "source/render/shaders/Prelude.cpp", "kFragmentHelpers"),
    ("SCREEN_VERTEX_BODY", "source/render/shaders/Prelude.cpp", "kScreenVertexBody"),
    ("TRACE_VERTEX_BODY", "source/render/shaders/Trace.cpp", "kTraceVertexBody"),
    ("TRACE_FRAGMENT_BODY", "source/render/shaders/Trace.cpp", "kTraceFragmentBody"),
    ("DECAY_FRAGMENT_BODY", "source/render/shaders/Decay.cpp", "kDecayFragmentBody"),
    ("BRIGHT_FRAGMENT_BODY", "source/render/shaders/Bloom.cpp", "kBrightFragmentBody"),
    ("BLUR_FRAGMENT_BODY", "source/render/shaders/Bloom.cpp", "kBlurFragmentBody"),
    ("GLASS_FRAGMENT_BODY", "source/render/shaders/Glass.cpp", "kGlassFragmentBody"),
]


def from_cpp(path, symbol):
    with open(os.path.join(REPO, path)) as handle:
        source = handle.read()
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    if match is None:
        return None
    return match.group(1)


def from_js(source, name):
    match = re.search(r'^const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None

    body = match.group(1)

    # Undo the one escape the literal needs, and refuse the rest. The C++ carries
    # no backslash at all, so a stray one here is either a typo or a difference
    # being smuggled through the decoder.
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not an escaped backtick, at line {upto.count(chr(10)) + 1}"

    return body.replace("\\`", "`"), None


def main():
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()

    problems = 0
    for name, path, symbol in SHADERS:
        cpp_text = from_cpp(path, symbol)
        js_text, complaint = from_js(js, name)

        if cpp_text is None:
            print(f"FAIL  {symbol} not found in {path}")
            problems += 1
            continue
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue

        if cpp_text == js_text:
            print(f"ok    {name:<20} matches {symbol} ({len(cpp_text)} chars)")
            continue

        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in {path}")

        cpp_lines = cpp_text.splitlines()
        js_lines = js_text.splitlines()
        for i in range(max(len(cpp_lines), len(js_lines))):
            a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
            b = js_lines[i] if i < len(js_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break

    print()
    if problems:
        print(f"{problems} shader(s) differ -- copy the C++ across, do not edit plugin.js by hand")
        return 1

    print(f"all {len(SHADERS)} shaders are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
