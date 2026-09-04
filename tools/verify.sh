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

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the effect does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
    local dir bad=0 n=0 shader

    if ! command -v glslc >/dev/null 2>&1; then
        printf '   skipped: glslc not installed (brew install shaderc)\n'
        return 0
    fi

    dir="$( mktemp -d )"

    python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/render/shaders/Prelude.cpp",
	"source/render/shaders/Trace.cpp",
	"source/render/shaders/Decay.cpp",
	"source/render/shaders/Bloom.cpp",
	"source/render/shaders/Glass.cpp",
	"source/signal/sources/Trace.cpp",
]

# Shaders the plugin assembles at run time.
# Mirrors vertexSource()/fragmentSource() in Prelude.cpp and their call sites.
ASSEMBLED = {
	"screenVertex":   [ "kConstants", "kScreenVertexBody" ],
	"traceVertex":    [ "kConstants", "kTraceVertexBody" ],
	"traceFragment":  [ "kConstants", "kFragmentHelpers", "kTraceFragmentBody" ],
	"decayFragment":  [ "kConstants", "kFragmentHelpers", "kDecayFragmentBody" ],
	"brightFragment": [ "kConstants", "kFragmentHelpers", "kBrightFragmentBody" ],
	"blurFragment":   [ "kConstants", "kFragmentHelpers", "kBlurFragmentBody" ],
	"glassFragment":  [ "kConstants", "kFragmentHelpers", "kGlassFragmentBody" ],
}

named, unnamed = {}, []
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(?:(\w+)\s*(?:\[\s*\])?\s*=\s*)?R"\((.*?)\)"', text, re.S ):
		if m.group( 1 ): named[ m.group( 1 ) ] = m.group( 2 )
		else:            unnamed.append( m.group( 2 ) )
	for m in re.finditer( r'(\w+)\s*=\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+);', text ):
		named.setdefault( m.group( 1 ), "".join(
			s.encode().decode( "unicode_escape" )
			for s in re.findall( r'"((?:[^"\\\n]|\\.)*)"', m.group( 2 ) ) ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

def piece( p ):
	# An int indexes the raw strings that are not assigned to a name, in source
	# order. A literal starts with #version. Anything else names a constant
	# above -- and a name that has moved is a KeyError here, not a silent skip.
	if isinstance( p, int ):       return unnamed[ p ]
	if p.startswith( "#version" ): return p
	return named[ p ]

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )

for name, parts in ASSEMBLED.items():
	emit( name, "".join( piece( p ) for p in parts ) )
SHADERS_PY

    for shader in "$dir"/*.vert "$dir"/*.frag; do
        [ -e "$shader" ] || continue
        n=$(( n + 1 ))
        if ! glslc --target-env=opengl4.5 -fauto-map-locations \
               "$shader" -o /dev/null 2>"$dir/err"; then
            printf '   %s does not compile\n' "$( basename "$shader" )"
            sed "s|$dir/||; s|^|      |" "$dir/err"
            bad=$(( bad + 1 ))
        fi
    done

    if [ "$n" -eq 0 ]; then
        # No shaders at all is a FAILURE, not a pass. It means the extraction
        # above has lost track of where this repo keeps its GLSL, and a check
        # that silently looks at nothing is worse than no check.
        printf '   no shaders were extracted -- the extraction has gone stale\n'
        rm -rf "$dir"
        return 1
    fi

    if [ "$bad" -eq 0 ]; then
        printf '   %d shaders, all compile\n' "$n"
    fi
    rm -rf "$dir"
    return "$bad"
}

#---------------------------------------------------------------------------
head_ "Shaders"
#---------------------------------------------------------------------------
if shaders_compile; then
    ok "every shader compiles"
else
    bad "a shader does not compile"
fi

# ---------------------------------------------------------------------------
head_ "Build (universal, both plugins, OFX, harness)"
# ---------------------------------------------------------------------------
# The build directory is DELETED first, and that is not belt and braces.
#
# `cmake -B build` on an existing tree re-uses the cache, and the cache is
# exactly where the architecture list and the BUILD_OFX switch live. A developer
# who configured once with `-DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_OFX=OFF` for
# a fast iteration loop -- which is the documented way to work in CLAUDE.md --
# leaves a tree where this script happily rebuilds, finds a single-architecture
# binary and no OFX bundle, and reports both as defects in the source.
#
# This file used to carry a comment claiming it did a fresh configure while
# doing nothing of the kind. It cost a confusing verify run to notice, and had it
# gone the other way -- a cache that happened to be right -- it would have cost a
# single-architecture release instead.
rm -rf build

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
#
# The symbol table is captured and matched with `case`, so there is NO PIPELINE
# at any point. That is not a style choice and the obvious alternatives are both
# wrong.
#
# `nm -gU "$bin" | grep -q _OfxGetPlugin` under `set -o pipefail` fails BECAUSE
# the symbol was found: `grep -q` exits at the first match, `nm` takes SIGPIPE,
# and pipefail propagates it. It is a race against how fast the producer
# finishes, so it bit the 2.8 MB OFX bundle while the identical line against the
# two smaller FFGL bundles passed, in the same run -- and the same command at a
# prompt printed the symbol happily.
#
# Piping a captured variable through `printf` makes the writer small and the race
# rare. Rare is not gone, and a check that fails one run in fifty is worse than
# one that fails every time. Hence `case`.
symbols_of() {
    nm -gU "$1" 2>/dev/null || true
}

for binary in "$SRC_BIN" "$FX_BIN"; do
    case "$(symbols_of "$binary")" in
        *_plugMain*) ok "exports plugMain: $(basename "$binary")" ;;
        *) bad "no plugMain: $(basename "$binary")" ;;
    esac
done

case "$(symbols_of "$OFX_BIN")" in
    *_OfxGetPlugin*) ok "exports OfxGetPlugin" ;;
    *) bad "no OfxGetPlugin in the OFX bundle" ;;
esac

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
    for test in energy dwell rate point blank identity fx drift names; do
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
