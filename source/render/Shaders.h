#pragma once

#include <string>

/**
    Every shader the renderer uses.

    ---------------------------------------------------------------------------
    The rule that matters here
    ---------------------------------------------------------------------------

    **Nothing computes `1 / v`.** The trace pass deposits a fixed quantum of
    energy per sample interval and spreads it over the screen distance the beam
    covered in that interval. Brightness proportional to the reciprocal of the
    beam's speed is then what "equal energy per unit time" *means*, rather than a
    term somebody applied on top -- so the corners of a Lissajous figure are
    bright for the reason they are bright on a tube, and a stationary beam needs
    no special case because there is no reciprocal to blow up.

    The closed form the fragment shader evaluates is the convolution of a uniform
    line segment of length L carrying energy E with an isotropic Gaussian spot of
    width sigma:

        f(u,v) = (E/L) * G_sigma(v) * [ Phi(u/sigma) - Phi((u-L)/sigma) ]

    with u along the segment and v across it, `G_sigma` the normalised 1D
    Gaussian and `Phi` the standard normal CDF. Three properties earned it its
    place: it conserves energy exactly for any L and any sigma, so the total
    light in a frame does not depend on how many samples the engine chose to
    emit; it saturates to a peak areal density proportional to 1/speed once
    L >> sigma, which is the dwell law; and as L -> 0 the L in the denominator
    cancels against the vanishing CDF difference, leaving a finite
    `E * G_sigma(u) * G_sigma(v)`. That last one is why a beam parked at a
    turnaround needs neither a clamp nor a divide-by-zero guard.

    ---------------------------------------------------------------------------
    GLSL reserved words, and why the names here look the way they do
    ---------------------------------------------------------------------------

    `sample`, `input`, `output`, `filter`, `common`, `active`, `shared`, `half`
    and `flat` are all reserved in GLSL. This plugin is about beam *samples*
    running through a *filter* onto a face, so nearly every natural name here is
    a reserved one. Hence `sampleA` / `sampleB`, `ClipTexture`,
    `FilterTransmission`, `deposit`, `halfLen`. They are not stylistic and
    tidying them back costs a shader that will not compile -- which surfaces only
    at runtime, in the diagnostics log, as "the effect does nothing".

    ---------------------------------------------------------------------------
    Assembly
    ---------------------------------------------------------------------------

    `#version 410 core` throughout: macOS caps at GL 4.1, so there is no compute
    stage, no SSBO and no image load/store available anywhere in this renderer,
    and every accumulation has to be done by the blender.

    Two preludes, because a vertex shader cannot have the fragment one.
    `fwidth` is fragment-only, and it appears in the graticule -- which would
    make a vertex stage fail to compile even if it never called the function,
    since GLSL compiles the whole body regardless. So the numeric constants are
    shared by both stages and the helper functions only by the fragment stages.

    Sources are assembled once, on first use, and returned by reference. They are
    functions rather than namespace-scope strings so that nothing depends on the
    initialisation order of two translation units.
*/
namespace vectrix::shaders
{

/// `#version` + the shared constants. For vertex stages.
std::string vertexSource( const char* body );

/// `#version` + the shared constants + the shared helpers (emission, the
/// graticule, the normal CDF). For fragment stages.
std::string fragmentSource( const char* body );

/// Full-screen quad, matching `ffglex::FFGLScreenQuad`'s attribute layout.
/// Shared by every pass except the trace, which sources its own geometry.
const std::string& screenVertex();

/// The trace: one instanced quad per sample interval, additive, into RG32F.
const std::string& traceVertex();
const std::string& traceFragment();

/// Two-layer phosphor decay, and the cascade between the layers.
const std::string& decayFragment();

/// Halation: the bright pass and the separable blur, both at quarter size.
const std::string& brightFragment();
const std::string& blurFragment();

/// The faceplate: curvature, vignette, graticule, the clip behind the glass,
/// and the composite onto the host's framebuffer.
const std::string& glassFragment();

} // namespace vectrix::shaders
