#!/usr/bin/env bash
#
# Everything that can be checked without a human, in one command.
#
#     tools/verify.sh
#
# ---------------------------------------------------------------- the point
#
# Half of this file checks things the RELEASE job checks. That is deliberate,
# and it is the fleet's most expensive lesson: a check that only ever runs in
# CI, after a tag, is a check that will catch you after the tag. The bundle
# layout, the plist, the architectures and the signature can all be verified
# here in a second, and the alternative is a failed release and a force-moved
# tag.
#
# The two that have actually bitten this fleet:
#
#   * `CFBundleExecutable` carrying the PREVIOUS plugin's name, because
#     cmake/InfoOFX.plist.in was copied from another repo. Nothing fails: the
#     bundle assembles, the binary is universal, `nm` finds the entry point and
#     a probe renders a correct frame. Then codesign says "code object is not
#     signed at all" and mentions nothing about a plist.
#
#   * A macOS build that is quietly arm64-only, because CMAKE_OSX_ARCHITECTURES
#     was set after the first target existed. The build log calls that a
#     success. Only `lipo` knows.
#
set -uo pipefail

cd "$(dirname "$0")/.."

PASS=0
FAIL=0

ok()    { printf '  \033[32mok\033[0m    %s\n' "$1"; PASS=$((PASS+1)); }
bad()   { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; FAIL=$((FAIL+1)); }
head_() { printf '\n\033[1m%s\033[0m\n' "$1"; }

# ---------------------------------------------------------------------------
head_ "Build (universal, both plugins, OFX, harness)"
# ---------------------------------------------------------------------------
# A fresh configure, because the thing most likely to be stale is the cache that
# decides the architectures.
if cmake -B build -DCMAKE_BUILD_TYPE=Release >/tmp/vectrix-configure.log 2>&1 \
   && cmake --build build -j8 >/tmp/vectrix-build.log 2>&1; then
    ok "configured and built"
else
    bad "build failed -- see /tmp/vectrix-build.log"
    tail -25 /tmp/vectrix-build.log
    exit 1
fi

SRC_BIN="build/Vectrix.bundle/Contents/MacOS/Vectrix"
FX_BIN="build/Vectrix Trace.bundle/Contents/MacOS/Vectrix Trace"
OFX_BUNDLE="build/Vectrix.ofx.bundle"
OFX_BIN="$OFX_BUNDLE/Contents/MacOS/Vectrix.ofx"

# ---------------------------------------------------------------------------
head_ "Architectures"
# ---------------------------------------------------------------------------
# lipo, never the build log. A single-architecture build is a successful build.
for binary in "$SRC_BIN" "$FX_BIN" "$OFX_BIN"; do
    if [ ! -f "$binary" ]; then
        bad "missing: $binary"
        continue
    fi
    archs="$(lipo -archs "$binary" 2>/dev/null)"
    case "$archs" in
        *x86_64*arm64*|*arm64*x86_64*) ok "universal: $(basename "$binary") ($archs)" ;;
        *) bad "NOT universal: $(basename "$binary") ($archs)" ;;
    esac
done

# ---------------------------------------------------------------------------
head_ "Entry points"
# ---------------------------------------------------------------------------
# An OBJECT library keeps the file-scope CFFGLPluginInfo alive. In a STATIC
# archive the linker may drop it, giving a bundle that loads, exports plugMain,
# and reports that it contains no plugins -- so exporting the symbol is
# necessary and not sufficient. The host load is what settles it.
for binary in "$SRC_BIN" "$FX_BIN"; do
    if nm -gU "$binary" 2>/dev/null | grep -q "_plugMain"; then
        ok "exports plugMain: $(basename "$binary")"
    else
        bad "no plugMain: $(basename "$binary")"
    fi
done

if nm -gU "$OFX_BIN" 2>/dev/null | grep -q "_OfxGetPlugin"; then
    ok "exports OfxGetPlugin"
else
    bad "no OfxGetPlugin in the OFX bundle"
fi

# ---------------------------------------------------------------------------
head_ "OFX bundle layout and signing"
# ---------------------------------------------------------------------------
# This whole section exists because it went wrong once, in another repo, and
# only at release time. See the header.
plist_exec="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' \
              "$OFX_BUNDLE/Contents/Info.plist" 2>/dev/null)"
if [ "$plist_exec" = "Vectrix.ofx" ]; then
    ok "CFBundleExecutable is Vectrix.ofx"
else
    bad "CFBundleExecutable is '$plist_exec', not Vectrix.ofx"
fi

if [ -f "$OFX_BUNDLE/Contents/MacOS/$plist_exec" ]; then
    ok "CFBundleExecutable matches a real binary on disk"
else
    bad "CFBundleExecutable names a file that is not there"
fi

# The exact command the release job runs, on a COPY, where it costs a second
# instead of a failed tag.
signdir="$(mktemp -d)"
cp -R "$OFX_BUNDLE" "$signdir/" 2>/dev/null
if codesign --force --sign - --timestamp=none "$signdir/$(basename "$OFX_BUNDLE")" >/dev/null 2>&1; then
    ok "ad-hoc codesign of the OFX bundle succeeds"
else
    bad "codesign fails -- this is the failure that never mentions the plist"
fi
rm -rf "$signdir"

# ---------------------------------------------------------------------------
head_ "The invariants"
# ---------------------------------------------------------------------------
# These are the point of the harness: they turn "brightness follows dwell time"
# from a sentence in AGENTS.md into something a machine checks.
if [ -x build/vxtest ]; then
    for test in energy dwell rate point blank identity fx drift; do
        log="/tmp/vectrix-$test.log"
        if ./build/vxtest "--$test" >"$log" 2>&1; then
            ok "vxtest --$test"
        else
            bad "vxtest --$test -- see $log"
            tail -12 "$log"
        fi
    done
else
    bad "vxtest was not built"
fi

# ---------------------------------------------------------------------------
head_ "Controls"
# ---------------------------------------------------------------------------
# A GLSL uniform name that does not match the C++ is silently ignored --
# glGetUniformLocation returns -1 and glUniform(-1) is a documented no-op -- so
# a control can be stone dead while everything compiles, links, loads and
# renders. Nothing else catches it.
if python3 tools/sweep.py >/tmp/vectrix-sweep.log 2>&1; then
    ok "every parameter changes the output"
else
    bad "dead controls -- see /tmp/vectrix-sweep.log"
    tail -20 /tmp/vectrix-sweep.log
fi

# ---------------------------------------------------------------------------
head_ "Presets"
# ---------------------------------------------------------------------------
# A preset row with too FEW values is aggregate-initialised to zero and compiles
# without a word, so the row silently means something else. The static_assert in
# Vectrix.cpp catches a wrong row *count*; only this catches a short row.
if python3 tools/check_presets.py >/tmp/vectrix-presets.log 2>&1; then
    ok "every preset row is the full width"
else
    bad "a preset row is the wrong length -- see /tmp/vectrix-presets.log"
    cat /tmp/vectrix-presets.log
fi

# ---------------------------------------------------------------------------
head_ "Browser demo"
# ---------------------------------------------------------------------------
# Two copies of a shader is exactly the arrangement that drifts.
if [ -f demo/tools/check_shaders.py ]; then
    if python3 demo/tools/check_shaders.py >/tmp/vectrix-shaders.log 2>&1; then
        ok "demo shaders match the plugin's"
    else
        bad "demo shaders have drifted -- see /tmp/vectrix-shaders.log"
        tail -20 /tmp/vectrix-shaders.log
    fi
fi

# ---------------------------------------------------------------------------
printf '\n\033[1m%d passed, %d failed\033[0m\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
