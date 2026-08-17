#pragma once

#include "ScopeBuffer.h"
#include "Tube.h"
#include "signal/Signal.h"

#include <FFGLSDK.h>

namespace vectrix
{
/**
    Everything the sample stream has to pass through to become light.

    ---------------------------------------------------------------------------
    What this class is doing that a line renderer is not
    ---------------------------------------------------------------------------

    A conventional vector renderer draws a polyline and then multiplies the
    brightness by something proportional to `1/speed` so that slow parts of the
    figure look brighter. That works until the beam stops, at which point the
    reciprocal has to be clamped, and the clamp is then the thing deciding how
    bright a stationary dot is.

    This one never computes a reciprocal. Each sample interval deposits a fixed
    quantum of energy, `BeamPower * dt * z`, spread over the distance the beam
    actually covered in that interval. Brightness proportional to `1/speed`
    falls out of that as the definition of equal energy per unit time, and the
    stationary case needs nothing special: as the distance goes to zero the
    closed form in `shaders/Trace.cpp` tends to a finite Gaussian spot, and what
    finally bounds it is the phosphor's own saturation rather than a number
    somebody picked.

    ---------------------------------------------------------------------------
    Six passes
    ---------------------------------------------------------------------------

      1. Decay      the phosphor buffer into the other phosphor buffer, two
                    layers with a cascade between them.
      2. Trace      one instanced quad per interval, additive, into the same
                    target the decay just wrote -- so there is no third buffer
                    and no combine pass.
      3. Bright     emission plus graticule, thresholded, at quarter size.
      4/5. Blur     separable Gaussian, ping-ponged, one to three times.
      6. Glass      the faceplate, the clip behind it, straight into the host's
                    framebuffer.

    ---------------------------------------------------------------------------
    Traps this class is built around
    ---------------------------------------------------------------------------

    - `ffglex::ScopedFBOBinding` restores the framebuffer but **not** the
      viewport. Every scoped bind here is followed by an explicit `glViewport`,
      and the host's is put back by hand before the final pass.
    - Every `ffglex::Scoped*` binding **clears to 0** on the way out rather than
      restoring what was there. Texture units are unbound by hand at the end.
    - `ffglex::FFGLShader::Set` has no integer-vector overload, so every size and
      offset uniform is declared `vec2` in GLSL and converted here.
    - A uniform name that does not match the GLSL is silently ignored --
      `glUniform` with location -1 is a documented no-op. There is no error and
      no warning; the value simply never arrives.
*/
class BeamGeometry
{
public:
	/// Everything the renderer needs for one frame. Ranges are physical, not
	/// 0..1: mapping the host's parameters onto these is Controls.cpp's job, and
	/// keeping that mapping out of here is what lets the offline harness ask for
	/// a specific sigma rather than for a slider position.
	struct RenderParams
	{
		//--- the beam ------------------------------------------------------
		float beamPower = 1.0f;   ///< Energy per second of beam-on time.
		float spotSigma = 0.006f; ///< Beam units. 1 unit = half the face height.
		float spotDefocus = 0.35f;///< Extra sigma at full beam current.
		float blankFloor  = 0.0f; ///< What a fully cut-off beam still deposits.

		/// Peak areal density below which a segment is not drawn at all. Culling
		/// on density and not on energy is what makes it mean something: a fast
		/// sweep and a slow one can carry identical energy and only one of them
		/// is visible.
		float densityFloor = 1.0e-4f;

		//--- the phosphor --------------------------------------------------
		int phosphor       = 0;   ///< Index into the table in Phosphor.h.
		float persistence  = 1.0f;///< Multiplier on both taus. 1 is the real phosphor.

		//--- the glass -----------------------------------------------------
		float halation          = 0.35f;///< How much scattered light comes back out.
		float halationRadius    = 0.5f; ///< 0..1; sets how many blur iterations.
		float halationThreshold = 0.5f; ///< Luma knee of the bright pass.

		TubeSpec tube;

		float graticule          = 0.5f;
		float graticuleColour[ 3 ] = { 0.30f, 0.42f, 0.38f };

		/// What the contrast filter in front of the phosphor passes, per channel.
		/// A real scope's filter is the colour of its phosphor, which is how it
		/// kills ambient light without killing the trace.
		float filterTransmission[ 3 ] = { 0.10f, 0.16f, 0.11f };

		/// How much of the faceplate is actually in front of what is behind it.
		/// 0 is exact passthrough, by construction.
		float faceBlack = 1.0f;
		float opacity   = 1.0f;

		float frameSeconds = 1.0f / 60.0f;

		/// Drop the phosphor history. For a Reset control, and for the first
		/// frame after the host reloads a composition.
		bool clearHistory = false;
	};

	bool InitGL();
	void DeInitGL();

	/// Draw one frame.
	///
	/// `samples` is a block of `n` samples; the renderer draws `n - 1` intervals,
	/// because the last sample has no interval after it. `viewport` is the host's
	/// own, read fresh from GL rather than remembered from InitGL -- Resolume
	/// changes composition resolution without reinitialising a plugin.
	///
	/// `clipTexture == 0` is the source build, which has no input at all. For the
	/// effect build the clip is painted on the tube face, and `maxU`/`maxV` are
	/// the host texture's usable extent.
	bool Render( const Sample* samples, int n, const RenderParams& params,
	             GLuint hostFBO, const GLint viewport[ 4 ],
	             GLuint clipTexture, float maxU, float maxV );

	/// Texels down the face buffer this frame, for the diagnostics log and the
	/// offline harness. Zero before the first successful Render.
	int FaceHeight() const
	{
		return faceHeight;
	}

private:
	/// Uniforms declared by the shared fragment prelude, and therefore needed by
	/// every pass that turns excitation into light.
	void SetPreludeUniforms( ffglex::FFGLShader& shader, const RenderParams& params,
	                         float graticuleLevel, float graticuleDiv );

	ffglex::FFGLShader traceShader;
	ffglex::FFGLShader decayShader;
	ffglex::FFGLShader brightShader;
	ffglex::FFGLShader blurShader;
	ffglex::FFGLShader glassShader;

	ffglex::FFGLScreenQuad quad;

	/// Ping-ponged: one holds last frame's excitation while the other is written.
	ScopeBuffer phosphorBuffer[ 2 ];
	/// Ping-ponged by the separable blur. Each full iteration ends back in [0].
	ScopeBuffer bloomBuffer[ 2 ];

	GLuint traceVAO = 0;
	GLuint traceVBO = 0;

	int phosphorIndex = 0;
	int faceHeight    = 0;
};

} // namespace vectrix
