#include "../Shaders.h"

namespace vectrix::shaders
{
namespace
{

// ---------------------------------------------------------------------------
// Constants. Shared by both stages, so they cannot drift.
// ---------------------------------------------------------------------------
//
// `Extent` is the half-width of the quad the trace pass draws around each
// segment, in units of the spot's sigma. 4.5 sigma keeps 99.9993% of a Gaussian,
// and the trace fragment subtracts the profile's value at exactly this distance
// so that what is thrown away is thrown away smoothly rather than as a step. Two
// places have to agree about the number -- the vertex stage sizing the quad and
// the fragment stage subtracting the pedestal -- which is why it lives here and
// not in either of them.
const char* const kConstants = R"(#version 410 core

const float Extent      = 4.5;
const float Sqrt2Pi     = 2.50662827463100050;
const float InvSqrt2Pi  = 0.39894228040143268;
)";

// ---------------------------------------------------------------------------
// The shared fragment prelude: phosphor emission, the graticule, and the
// normal CDF.
// ---------------------------------------------------------------------------
//
// Emission is applied at *readout*, never at deposit. What the phosphor buffer
// holds is excitation -- two scalars, the fast layer and the slow one -- and it
// only becomes light in the two passes that show it. That is what lets the
// operator change phosphor without the history in the buffer being wrong: the
// buffer never contained a colour to be wrong about.
//
// The graticule is here for the same reason. It is evaluated in the bright pass
// and again in the glass pass, at the same face coordinate, from one function.
// Two copies would drift the instant somebody adjusted the line width in the one
// they happened to be looking at, and the symptom -- a halo that is a fraction of
// a division away from the line casting it -- is very hard to see and impossible
// to unsee.
const char* const kFragmentHelpers = R"(
// The two-layer phosphor, as a table entry from Phosphor.cpp.
uniform vec3  FastColour;
uniform vec3  SlowColour;
uniform float PhosphorEfficiency;
uniform float PhosphorSaturation;

// The graticule. `GraticuleLevel` already carries both the operator's strength
// and the resolution fade -- see graticuleAt().
uniform float GraticuleLevel;
uniform float GraticuleDiv;   //beam units per division
uniform vec3  GraticuleColour;

//---------------------------------------------------------------------------
// The standard normal CDF.
//
// GLSL has no erf, so this is the usual tanh approximation, good to about 3e-4.
// The clamp is not tidiness. A driver computing tanh as (e^2x - 1)/(e^2x + 1)
// overflows to inf around x = 44 and then produces inf/inf = NaN -- long after
// the value has stopped changing, and for arguments a beam sitting still
// produces constantly. One NaN deposited into the phosphor buffer survives every
// ping-pong for the life of the plugin, because NaN * decay is NaN.
//---------------------------------------------------------------------------
float ncdf( float x )
{
	float t = clamp( 0.7978845608 * ( x + 0.044715 * x * x * x ), -8.0, 8.0 );
	return 0.5 * ( 1.0 + tanh( t ) );
}

//---------------------------------------------------------------------------
// Excitation -> light.
//
// A phosphor has a finite number of luminescent centres, so pumping it harder
// stops buying proportionally more light. l = e / (1 + e/S) is the simplest
// curve with that shape: at low e it is e to within e^2/S, so the dwell law
// stays exact everywhere it is visible, and it approaches S asymptotically
// rather than clipping.
//
// This is the *only* thing that bounds a stationary beam, and it is deliberately
// the tube that does it rather than a clamp. A clamp would put a hard edge on
// the spot at whatever level it was set to; a real over-driven spot blooms,
// because the centres near the middle saturate first and the ones further out
// are still climbing.
//---------------------------------------------------------------------------
float phosphorSaturate( float e )
{
	return e / ( 1.0 + e / max( PhosphorSaturation, 1e-6 ) );
}

vec3 phosphorEmission( vec2 excitation )
{
	vec2 e = max( excitation, vec2( 0.0 ) );
	return ( phosphorSaturate( e.x ) * FastColour
	       + phosphorSaturate( e.y ) * SlowColour ) * PhosphorEfficiency;
}

//---------------------------------------------------------------------------
// The graticule, as a signed-distance evaluation rather than geometry.
//
// 8 divisions by 10, minor ticks every 0.2 of a division on the two centre axes
// only -- which is where a real scope puts them, because they are for reading a
// timebase off the horizontal and an amplitude off the vertical, not for
// subdividing the whole face.
//
// `face` is in beam units and `GraticuleDiv` converts to divisions, so this
// function knows nothing about the output resolution and does not need to: the
// anti-aliasing comes from the local derivative, which is correct in whichever
// buffer the caller happens to be filling.
//
// The visibility fade is the exception and it arrives as a uniform. Once a
// division is worth fewer than about three output pixels the graticule stops
// being a graticule and becomes a moire generator, so it fades out -- but the
// bright pass runs at quarter resolution, and a fade computed from the local
// derivative would therefore fade the halo out two rungs before the sharp copy
// and leave a graticule casting no light. One number, computed once from the
// output size, keeps them agreeing.
//---------------------------------------------------------------------------
float graticuleAt( vec2 face )
{
	if( GraticuleLevel <= 0.0 )
		return 0.0;

	vec2 g = face / max( GraticuleDiv, 1e-6 );
	const vec2 halfSpan = vec2( 5.0, 4.0 );//10 wide by 8 tall

	//Per-axis, not the length of the pair: the two are independent here and
	//combining them would overstate the rate by root two on every diagonal.
	vec2 rate = max( fwidth( g ), vec2( 1e-6 ) );

	const float lineHalf = 0.015;//1.5% of a division
	const float tickHalf = 0.09; //how far a minor tick reaches off its axis

	vec2 toLine = abs( g - round( g ) );
	float major = max( 1.0 - smoothstep( lineHalf - rate.x, lineHalf + rate.x, toLine.x ),
	                   1.0 - smoothstep( lineHalf - rate.y, lineHalf + rate.y, toLine.y ) );

	vec2 toTick = abs( g - 0.2 * round( g * 5.0 ) );
	float tickX = ( 1.0 - smoothstep( lineHalf - rate.x, lineHalf + rate.x, toTick.x ) )
	            * ( 1.0 - smoothstep( tickHalf, tickHalf + rate.y, abs( g.y ) ) );
	float tickY = ( 1.0 - smoothstep( lineHalf - rate.y, lineHalf + rate.y, toTick.y ) )
	            * ( 1.0 - smoothstep( tickHalf, tickHalf + rate.x, abs( g.x ) ) );

	float ink = max( major, max( tickX, tickY ) );

	//Nothing outside the rectangle. The border lines themselves are at +-5 and
	//+-4, which round() already treats as division lines, so this only has to
	//cut off what lies beyond them.
	vec2 beyond = smoothstep( halfSpan + lineHalf - rate, halfSpan + lineHalf + rate, abs( g ) );
	ink *= ( 1.0 - beyond.x ) * ( 1.0 - beyond.y );

	return ink * GraticuleLevel;
}
)";

// ---------------------------------------------------------------------------
// The full-screen quad.
// ---------------------------------------------------------------------------
//
// Note what is missing: MaxUV. Every other plugin in the fleet scales the
// vertex stage's uv by the host texture's MaxUV, because every pass in those
// plugins reads a host texture. Here exactly one pass does, and in that pass the
// same uv is *also* the geometry of the tube face. Scaling it by the host's
// padding would shrink the tube whenever Resolume handed over a texture larger
// than its picture -- an effect that appears on some machines and not others,
// which is the worst kind. So uv stays 0..1 and the clip lookup applies MaxUV
// where the clip is actually read.
const char* const kScreenVertexBody = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

} // namespace

std::string vertexSource( const char* body )
{
	std::string source = kConstants;
	source += body;
	return source;
}

std::string fragmentSource( const char* body )
{
	std::string source = kConstants;
	source += kFragmentHelpers;
	source += body;
	return source;
}

const std::string& screenVertex()
{
	static const std::string source = vertexSource( kScreenVertexBody );
	return source;
}

} // namespace vectrix::shaders
