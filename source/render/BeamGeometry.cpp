#include "BeamGeometry.h"

#include "Diag.h"
#include "GLState.h"
#include "Phosphor.h"
#include "Shaders.h"

// Explicitly, and not through the umbrella: FFGLSDK.h includes every other
// ffglex header and omits this one. Leaving it out compiles right up until the
// first ScopedFBOBinding, at which point the error names a type nobody expected
// to be missing.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <string>

using namespace ffglex;

namespace vectrix
{
namespace
{
/// A runaway cannot be allowed to climb until it reaches the top of a 32-bit
/// float and turns into an inf. Well below that, and far above anything the
/// saturation curve will let through, so it never shapes a picture -- it only
/// stops one from becoming unrecoverable.
constexpr float kExcitationCeiling = 1.0e6f;

float smoothstep01( float edge0, float edge1, float x )
{
	const float t = std::clamp( ( x - edge0 ) / std::max( edge1 - edge0, 1e-6f ), 0.0f, 1.0f );
	return t * t * ( 3.0f - 2.0f * t );
}
} // namespace

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

bool BeamGeometry::InitGL()
{
	struct
	{
		FFGLShader& shader;
		const std::string& vertex;
		const std::string& fragment;
		const char* name;
	} stages[] = {
		{ traceShader, shaders::traceVertex(), shaders::traceFragment(), "trace" },
		{ decayShader, shaders::screenVertex(), shaders::decayFragment(), "decay" },
		{ brightShader, shaders::screenVertex(), shaders::brightFragment(), "halation bright pass" },
		{ blurShader, shaders::screenVertex(), shaders::blurFragment(), "halation blur" },
		{ glassShader, shaders::screenVertex(), shaders::glassFragment(), "glass" }
	};

	for( auto& stage : stages )
	{
		if( !stage.shader.Compile( stage.vertex, stage.fragment ) )
		{
			// Failing out of InitGL is invisible to the operator: the effect
			// simply does nothing in Resolume, with no message anywhere. Naming
			// the stage here is the only record of which one it was.
			diag::error( std::string( "shader failed to compile: " ) + stage.name
			             + " - the effect will do nothing" );
			return false;
		}
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		return false;
	}

	glGenVertexArrays( 1, &traceVAO );
	glGenBuffers( 1, &traceVBO );
	if( traceVAO == 0 || traceVBO == 0 )
	{
		diag::error( "failed to create the trace vertex array" );
		return false;
	}

	//-----------------------------------------------------------------------
	// One buffer of Samples, read twice.
	//
	// Attribute 0 starts at the beginning and attribute 1 one element in, so
	// instance i sees sample i and sample i+1 with nothing duplicated and half
	// the bandwidth of an expanded segment list. It costs one thing: the draw
	// must ask for n-1 instances, because n instances would read one Sample past
	// the end of the buffer.
	//
	// glVertexAttribDivisor is VAO state, not global state, so it has to be set
	// with this VAO bound. Set it with no VAO bound and it lands nowhere; set it
	// with the wrong one bound and it lands somewhere worse.
	//-----------------------------------------------------------------------
	glBindVertexArray( traceVAO );
	glBindBuffer( GL_ARRAY_BUFFER, traceVBO );

	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 4, GL_FLOAT, GL_FALSE, sizeof( Sample ), nullptr );
	glVertexAttribDivisor( 0, 1 );

	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 4, GL_FLOAT, GL_FALSE, sizeof( Sample ),
	                       reinterpret_cast< const GLvoid* >( sizeof( Sample ) ) );
	glVertexAttribDivisor( 1, 1 );

	glBindVertexArray( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	phosphorIndex = 0;
	faceHeight    = 0;

	return true;
}

void BeamGeometry::DeInitGL()
{
	traceShader.FreeGLResources();
	decayShader.FreeGLResources();
	brightShader.FreeGLResources();
	blurShader.FreeGLResources();
	glassShader.FreeGLResources();

	quad.Release();

	if( traceVBO != 0 )
	{
		glDeleteBuffers( 1, &traceVBO );
		traceVBO = 0;
	}
	if( traceVAO != 0 )
	{
		glDeleteVertexArrays( 1, &traceVAO );
		traceVAO = 0;
	}

	for( auto& buffer : phosphorBuffer )
		buffer.Destroy();
	for( auto& buffer : bloomBuffer )
		buffer.Destroy();

	faceHeight = 0;
}

// ---------------------------------------------------------------------------
// Uniforms shared by every pass that turns excitation into light
// ---------------------------------------------------------------------------

void BeamGeometry::SetPreludeUniforms( FFGLShader& shader, const RenderParams& params,
                                       float graticuleLevel, float graticuleDiv )
{
	const PhosphorSpec& spec = phosphor( params.phosphor );

	shader.Set( "FastColour", spec.fastColour[ 0 ], spec.fastColour[ 1 ], spec.fastColour[ 2 ] );
	shader.Set( "SlowColour", spec.slowColour[ 0 ], spec.slowColour[ 1 ], spec.slowColour[ 2 ] );
	shader.Set( "PhosphorEfficiency", spec.efficiency );
	shader.Set( "PhosphorSaturation", std::max( spec.saturation, 1e-4f ) );

	shader.Set( "GraticuleLevel", graticuleLevel );
	shader.Set( "GraticuleDiv", graticuleDiv );
	shader.Set( "GraticuleColour", params.graticuleColour[ 0 ],
	            params.graticuleColour[ 1 ], params.graticuleColour[ 2 ] );
}

// ---------------------------------------------------------------------------
// One frame
// ---------------------------------------------------------------------------

bool BeamGeometry::Render( const Sample* samples, int n, const RenderParams& params,
                           GLuint hostFBO, const GLint viewport[ 4 ],
                           GLuint clipTexture, float maxU, float maxV )
{
	if( viewport == nullptr )
		return false;

	// Captured here and restored by the destructor, so that every one of the
	// early returns below puts the context back. FFGL requires it, and Resolume
	// renders the rest of the composition with whatever it finds -- a plugin that
	// bails out with additive blending still enabled makes the *next* effect in
	// the chain look broken, which is where the bug report comes from.
	ScopedGLState state;

	if( !traceShader.IsReady() || !decayShader.IsReady() || !brightShader.IsReady()
	    || !blurShader.IsReady() || !glassShader.IsReady() )
		return false;

	if( samples == nullptr )
		n = 0;

	// The last sample has no interval after it, so there are n-1 intervals.
	const int segmentCount = std::max( 0, n - 1 );

	const float outputWidth  = std::max( 1.0f, static_cast< float >( viewport[ 2 ] ) );
	const float outputHeight = std::max( 1.0f, static_cast< float >( viewport[ 3 ] ) );

	//-----------------------------------------------------------------------
	// Buffers. Sized by the spot, not by the composition -- see Tube.h.
	//-----------------------------------------------------------------------
	const float spotSigma = std::max( params.spotSigma, 1e-5f );

	// Beam units run to 1 at half the face height, so a sigma expressed in them
	// is twice its fraction of the full height. The *undefocused* sigma is the
	// one that sets the resolution, because it is the smallest.
	const float spotFraction = spotSigma * 0.5f;

	const int wantHeight = faceSizeFor( spotFraction, static_cast< int >( outputHeight ) );
	const int wantWidth  = faceWidthFor( wantHeight, params.tube.faceAspect );
	const int bloomWidth  = std::max( 1, wantWidth / 4 );
	const int bloomHeight = std::max( 1, wantHeight / 4 );

	// RG32F and not RGBA16F. The two channels are excitations that accumulate
	// over hundreds of frames on a long phosphor, and half-float runs out of
	// mantissa exactly where a bright dwell has been building for a while --
	// the accumulator stops climbing and the brightest part of the picture is
	// the part that stops responding.
	if( !phosphorBuffer[ 0 ].Ensure( wantWidth, wantHeight, GL_RG32F, ScopeBuffer::Smooth )
	    || !phosphorBuffer[ 1 ].Ensure( wantWidth, wantHeight, GL_RG32F, ScopeBuffer::Smooth )
	    || !bloomBuffer[ 0 ].Ensure( bloomWidth, bloomHeight, GL_RGBA16F, ScopeBuffer::Smooth )
	    || !bloomBuffer[ 1 ].Ensure( bloomWidth, bloomHeight, GL_RGBA16F, ScopeBuffer::Smooth ) )
	{
		diag::error( "could not allocate the face buffers" );
		return false;
	}

	faceHeight = wantHeight;

	if( params.clearHistory )
	{
		phosphorBuffer[ 0 ].ClearTo( 0.0f, 0.0f, 0.0f, 0.0f );
		phosphorBuffer[ 1 ].ClearTo( 0.0f, 0.0f, 0.0f, 0.0f );
	}

	const int target  = phosphorIndex;
	const int history = 1 - phosphorIndex;

	//-----------------------------------------------------------------------
	// Everything the shape of the face implies, worked out once.
	//-----------------------------------------------------------------------
	const float faceAspect = std::max( params.tube.faceAspect, 0.05f );
	const float outputAspect = outputWidth / outputHeight;
	const float faceFit    = faceFitScale( outputAspect, faceAspect );
	const float division   = graticuleDivision( params.tube );

	// One square output unit is half the output height in pixels, so this is how
	// many output pixels a division is worth. Below about three of them the
	// graticule is a moire generator rather than a graticule, so it fades -- and
	// it fades on this one number, computed here, rather than on each pass's own
	// derivative. The bright pass runs at quarter size; a local fade would take
	// the halo away two rungs before the sharp copy and leave lines casting no
	// light at all.
	const float pixelsPerDivision = division * faceFit * outputHeight * 0.5f;
	const float graticuleLevel    = std::max( params.graticule, 0.0f )
	                             * smoothstep01( 2.0f, 4.0f, pixelsPerDivision );

	const PhosphorDecay decay = decayFor( phosphor( params.phosphor ),
	                                      params.persistence,
	                                      std::max( params.frameSeconds, 1.0e-5f ) );

	//-----------------------------------------------------------------------
	// Upload the block. GL_STREAM_DRAW and a fresh glBufferData every frame:
	// the whole point is that the driver orphans the old storage rather than
	// waiting for the previous frame's draw to finish reading it.
	//-----------------------------------------------------------------------
	if( n > 0 )
	{
		glBindBuffer( GL_ARRAY_BUFFER, traceVBO );
		glBufferData( GL_ARRAY_BUFFER,
		              static_cast< GLsizeiptr >( static_cast< size_t >( n ) * sizeof( Sample ) ),
		              samples, GL_STREAM_DRAW );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
	}

	//-----------------------------------------------------------------------
	// 1 and 2. Decay, then deposit, into the same target.
	//-----------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( phosphorBuffer[ target ].GetGLID(), ScopedFBOBinding::RB_REVERT );

		// ScopedFBOBinding restores the framebuffer and says nothing about the
		// viewport. Without this the whole pass renders into a rectangle the
		// size of the host's output, in a buffer that is a different size.
		glViewport( 0, 0, wantWidth, wantHeight );

		{
			glDisable( GL_BLEND );

			ScopedShaderBinding shader( decayShader.GetGLID() );

			glActiveTexture( GL_TEXTURE0 );
			glBindTexture( GL_TEXTURE_2D, phosphorBuffer[ history ].GetTextureInfo().Handle );

			decayShader.Set( "HistoryTexture", 0 );
			decayShader.Set( "DecayFast", decay.fast );
			decayShader.Set( "DecaySlow", decay.slow );
			decayShader.Set( "Transfer", decay.transfer );
			decayShader.Set( "Ceiling", kExcitationCeiling );
			quad.Draw();
		}

		if( segmentCount > 0 && params.beamPower > 0.0f )
		{
			// Sum, not max().
			//
			// `old-cathode`'s phosphor pass takes max() against the decayed
			// history and it is right to, because its input is a *level* -- the
			// brightness the signal says that point on the screen should have,
			// and two claims about the same level do not add up to twice it.
			//
			// This input is a *deposit*: the energy one interval of beam put on
			// the glass. Two intervals crossing the same texel really did put
			// twice the energy there, and a max() would silently throw away
			// every crossing in the figure -- which is exactly where an
			// oscilloscope trace is brightest. Do not "fix" this to match the
			// sibling.
			setAdditiveBlend();

			ScopedShaderBinding shader( traceShader.GetGLID() );

			traceShader.Set( "BeamPower", params.beamPower );
			traceShader.Set( "SpotSigma", spotSigma );
			traceShader.Set( "SpotDefocus", std::max( params.spotDefocus, 0.0f ) );
			traceShader.Set( "BlankFloor", std::clamp( params.blankFloor, 0.0f, 1.0f ) );
			traceShader.Set( "DensityFloor", std::max( params.densityFloor, 0.0f ) );
			traceShader.Set( "DeflectionGain", params.tube.deflectionGain );
			traceShader.Set( "FaceAspect", faceAspect );

			// Instance i reads samples i and i+1, so the highest index touched is
			// segmentCount. Asserted here, at the draw, because this is where
			// getting it wrong costs something: one instance too many and
			// attribute 1 reads a Sample past the end of the buffer, which on most
			// drivers returns zeroes and on some returns whatever was there --
			// a segment from the last point of the figure to somewhere arbitrary,
			// on some machines and not others.
			assert( segmentCount < n && "n instances would read one Sample past the end" );

			glBindVertexArray( traceVAO );
			glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, segmentCount );
			glBindVertexArray( 0 );
		}
	}

	//-----------------------------------------------------------------------
	// 3 to 5. Halation, all at quarter size.
	//-----------------------------------------------------------------------
	const bool useHalation = params.halation > 0.001f;
	if( useHalation )
	{
		{
			ScopedFBOBinding fbo( bloomBuffer[ 0 ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			glViewport( 0, 0, bloomWidth, bloomHeight );
			glDisable( GL_BLEND );

			ScopedShaderBinding shader( brightShader.GetGLID() );

			glActiveTexture( GL_TEXTURE0 );
			glBindTexture( GL_TEXTURE_2D, phosphorBuffer[ target ].GetTextureInfo().Handle );

			SetPreludeUniforms( brightShader, params, graticuleLevel, division );
			brightShader.Set( "PhosphorTexture", 0 );
			brightShader.Set( "SourceSize", static_cast< float >( wantWidth ),
			                  static_cast< float >( wantHeight ) );
			brightShader.Set( "FaceHalf", faceAspect, 1.0f );
			brightShader.Set( "Threshold", params.halationThreshold );
			quad.Draw();
		}

		// One iteration is a tight halo, three is a wide soft one, and three is
		// also the ceiling: each is two full-screen passes over the quarter-size
		// buffer, and past three the halo is wider than the face and stops being
		// scattering in glass at all.
		const int iterations = std::clamp(
			1 + static_cast< int >( std::lround( 2.0f * std::clamp( params.halationRadius, 0.0f, 1.0f ) ) ),
			1, 3 );

		for( int i = 0; i < iterations; ++i )
		{
			const struct
			{
				int from;
				int to;
				float dx;
				float dy;
			} blurs[] = {
				{ 0, 1, 1.0f / static_cast< float >( bloomWidth ), 0.0f },
				{ 1, 0, 0.0f, 1.0f / static_cast< float >( bloomHeight ) }
			};

			// Across then down, so every iteration ends back in bloomBuffer[0]
			// and the glass pass never has to ask which one it landed in.
			for( const auto& pass : blurs )
			{
				ScopedFBOBinding fbo( bloomBuffer[ pass.to ].GetGLID(), ScopedFBOBinding::RB_REVERT );
				glViewport( 0, 0, bloomWidth, bloomHeight );
				glDisable( GL_BLEND );

				ScopedShaderBinding shader( blurShader.GetGLID() );

				glActiveTexture( GL_TEXTURE0 );
				glBindTexture( GL_TEXTURE_2D, bloomBuffer[ pass.from ].GetTextureInfo().Handle );

				blurShader.Set( "SourceTexture", 0 );
				blurShader.Set( "Direction", pass.dx, pass.dy );
				quad.Draw();
			}
		}
	}

	//-----------------------------------------------------------------------
	// 6. The glass, straight into whatever the host handed us.
	//-----------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, hostFBO );

		// By hand, because nothing above restored it: the last scoped binding
		// put the framebuffer back and left the viewport pointing at a
		// quarter-size bloom buffer.
		glViewport( viewport[ 0 ], viewport[ 1 ], viewport[ 2 ], viewport[ 3 ] );

		// Blending off. This pass writes the layer's whole content, premultiplied
		// and with its own alpha; compositing that onto the rest of the
		// composition is Resolume's job. Leaving the host's blend enabled would
		// composite it twice, which reads as the effect being washed out at low
		// opacity and is very easy to mistake for a bad Opacity mapping.
		glDisable( GL_BLEND );

		ScopedShaderBinding shader( glassShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, phosphorBuffer[ target ].GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, bloomBuffer[ 0 ].GetTextureInfo().Handle );

		// The source build has no clip, and the shader never samples ClipTexture
		// in that case -- but the sampler is still declared and still bound to a
		// unit, and binding texture 0 there makes the driver complain about a
		// sampler pointing at an unloadable texture. It is only a warning and the
		// picture is correct either way; it is also a warning per plugin instance
		// in Resolume's log, from a plugin that is working perfectly. Pointing the
		// unused unit at a texture that does exist costs nothing and says nothing.
		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, clipTexture != 0
		                                  ? clipTexture
		                                  : phosphorBuffer[ target ].GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE0 );

		SetPreludeUniforms( glassShader, params, graticuleLevel, division );

		glassShader.Set( "PhosphorTexture", 0 );
		glassShader.Set( "BloomTexture", 1 );
		glassShader.Set( "ClipTexture", 2 );

		glassShader.Set( "OutputSize", outputWidth, outputHeight );
		glassShader.Set( "FaceHalf", faceAspect, 1.0f );
		glassShader.Set( "FaceFit", faceFit );
		glassShader.Set( "CornerRadius", std::clamp( params.tube.cornerRadius, 0.0f, 1.0f ) );
		glassShader.Set( "Curvature", std::max( params.tube.curvature, 0.0f ) );
		glassShader.Set( "Vignette", std::clamp( params.tube.vignette, 0.0f, 1.0f ) );
		glassShader.Set( "Halation", useHalation ? params.halation : 0.0f );

		// No perspective control is exposed yet, and these are set explicitly to
		// zero rather than left unset: an unset uniform is zero on every driver
		// anybody has, and is guaranteed by nobody.
		glassShader.Set( "PerspectiveX", 0.0f );
		glassShader.Set( "PerspectiveY", 0.0f );

		glassShader.Set( "FilterTransmission", params.filterTransmission[ 0 ],
		                 params.filterTransmission[ 1 ], params.filterTransmission[ 2 ] );
		glassShader.Set( "FaceBlack", std::clamp( params.faceBlack, 0.0f, 1.0f ) );
		glassShader.Set( "Opacity", std::clamp( params.opacity, 0.0f, 1.0f ) );

		glassShader.Set( "HasClip", clipTexture != 0 ? 1.0f : 0.0f );
		glassShader.Set( "ClipMaxUV", maxU, maxV );

		quad.Draw();
	}

	//-----------------------------------------------------------------------
	// Put the texture units back by hand. Every ffglex Scoped* binding clears to
	// zero on the way out instead of restoring, so there is nothing to be gained
	// by having used them here -- but there is something to be lost by leaving
	// three units bound to our own buffers when the host draws the next layer.
	//-----------------------------------------------------------------------
	glActiveTexture( GL_TEXTURE2 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	phosphorIndex = history;

	return true;
}

} // namespace vectrix
