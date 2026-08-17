#include "../Shaders.h"

namespace vectrix::shaders
{
namespace
{

// ---------------------------------------------------------------------------
// The trace: one instanced quad per sample interval.
// ---------------------------------------------------------------------------
//
// The two attributes are the same buffer bound twice, the second offset by one
// element, so instance i sees sample i and sample i+1 with nothing duplicated
// and nothing uploaded twice. That is also why the instance count has to be
// n-1: drawing n instances reads one Sample past the end of the buffer, which
// on most drivers returns zeroes and on some returns whatever was there.
//
// `sampleA` and `sampleB` rather than the obvious names because `sample` is a
// GLSL reserved word. See Shaders.h.
const char* const kTraceVertexBody = R"(
layout( location = 0 ) in vec4 sampleA;//x, y volts; z grid; w dt seconds
layout( location = 1 ) in vec4 sampleB;

uniform float BeamPower;
uniform float SpotSigma;      //beam units; 1 unit = half the face height
uniform float SpotDefocus;    //how much a full-current beam swells
uniform float BlankFloor;     //what a fully cut-off beam still puts on the glass
uniform float DensityFloor;   //peak areal density below which a segment is not drawn
uniform float DeflectionGain; //beam units per volt: underscan or overscan
uniform float FaceAspect;     //face width / face height

flat out float segLength;
flat out float segSigma;
flat out float segEnergy;
out vec2 segUV;               //x along the segment from its centre, y across

//---------------------------------------------------------------------------
// Not isnan()/isinf().
//
// Those are the right functions and they are also the first thing a shader
// compiler running with fast-math assumptions folds to constant false, on the
// grounds that the value cannot happen. The value is exactly what we are here
// about: one NaN sample -- an oscillator that divided by a zero frequency, an
// audio file with a denormal run, a filter that went unstable for one block --
// deposits a NaN into the phosphor buffer, and NaN * decay is NaN for the rest
// of the session. A comparison chain has no intrinsic to fold.
//---------------------------------------------------------------------------
bool usable( float v )
{
	return v > -1e30 && v < 1e30;
}

bool usable2( vec2 v )
{
	return usable( v.x ) && usable( v.y );
}

void main()
{
	vec2 a = sampleA.xy * DeflectionGain;
	vec2 b = sampleB.xy * DeflectionGain;

	//The grid voltage over the interval, averaged across its two ends. The floor
	//is what a cut-off beam still manages: a real gun does not reach zero
	//emission, and a retrace that leaves absolutely nothing behind looks wrong in
	//a way people describe as "too clean".
	float zBar = mix( BlankFloor, 1.0, clamp( 0.5 * ( sampleA.z + sampleB.z ), 0.0, 1.0 ) );

	//The whole brightness model, in one line. Energy per interval, not intensity
	//per pixel: the fragment stage spreads this over however far the beam went,
	//so nothing anywhere divides by a speed. `dt` is per-sample precisely so this
	//is independent of how many samples the engine chose to emit -- see
	//signal/Signal.h.
	float energy = BeamPower * max( sampleA.w, 0.0 ) * zBar;

	//More current is a fatter spot. Same reason a CRT's highlights swell.
	float sigma = SpotSigma * ( 1.0 + SpotDefocus * zBar );

	vec2 delta = b - a;
	float span = length( delta );
	vec2 dir   = span > 1e-9 ? delta / span : vec2( 1.0, 0.0 );

	//A floor of a twentieth of a spot, so the box is never zero-area and the
	//fragment stage's short branch has a length to divide by if it ever wanted
	//one. It does not: below a quarter of a sigma it uses the point limit, where
	//the length cancels.
	float len = max( span, 0.05 * sigma );

	//Peak areal density of the finished segment: the value at the middle of a
	//long one, E / (L * sigma * sqrt(2pi)). Culling on this rather than on energy
	//is what makes the cull mean something -- a fast sweep and a slow one can
	//carry the same energy and only one of them is visible.
	float density = energy / max( len * sigma * Sqrt2Pi, 1e-30 );

	bool ok = usable2( a ) && usable2( b )
	       && usable( energy ) && usable( sigma )
	       && energy > 0.0 && sigma > 0.0
	       && density >= DensityFloor;

	if( !ok )
	{
		//A degenerate quad: the same clip position for all four corners, so it
		//has no area and rasterises nothing whatever the driver does with it,
		//and outside the frustum as well. Returning without writing gl_Position
		//would be undefined; discarding in the fragment stage would be too late,
		//because a NaN position can already have taken the primitive somewhere
		//enormous.
		segLength   = 0.0;
		segSigma    = 1.0;
		segEnergy   = 0.0;
		segUV       = vec2( 0.0 );
		gl_Position = vec4( 2.0, 2.0, 0.0, 1.0 );
		return;
	}

	//An oriented box around the capsule, padded by Extent sigma on all sides.
	float halfAlong  = 0.5 * len + Extent * sigma;
	float halfAcross = Extent * sigma;

	//Corners from the vertex index, as a triangle strip: 0 -> (-,-), 1 -> (+,-),
	//2 -> (-,+), 3 -> (+,+). No vertex buffer for four signs.
	float sx = ( ( gl_VertexID & 1 ) == 0 ) ? -1.0 : 1.0;
	float sy = ( ( gl_VertexID & 2 ) == 0 ) ? -1.0 : 1.0;

	vec2 centre = a + dir * ( 0.5 * len );
	vec2 perp   = vec2( -dir.y, dir.x );
	vec2 pos    = centre + dir * ( sx * halfAlong ) + perp * ( sy * halfAcross );

	segLength = len;
	segSigma  = sigma;
	segEnergy = energy;
	segUV     = vec2( sx * halfAlong, sy * halfAcross );

	//Beam units are isotropic -- one unit is half the face height in both axes --
	//so the spot is round and stays round on a 4:3 face. The divide is the only
	//place the face's shape enters the trace at all.
	gl_Position = vec4( pos.x / FaceAspect, pos.y, 0.0, 1.0 );
}
)";

// ---------------------------------------------------------------------------
// The closed form, evaluated per fragment.
// ---------------------------------------------------------------------------
//
//     f(u,v) = (E/L) * G_sigma(v) * [ Phi(u/sigma) - Phi((u-L)/sigma) ]
//
// written below with u measured from the segment's *centre*, so the CDF
// difference reads Phi((u + L/2)/sigma) - Phi((u - L/2)/sigma). That is the same
// expression with its origin shifted by half a length, and centring it is what
// makes the short branch's point limit land on the segment's midpoint instead of
// half a spot behind it.
const char* const kTraceFragmentBody = R"(
flat in float segLength;
flat in float segSigma;
flat in float segEnergy;
in vec2 segUV;

out vec4 fragColor;

void main()
{
	float inv = 1.0 / segSigma;
	float u   = segUV.x;
	float v   = segUV.y;

	//Across the segment: a normalised Gaussian, minus the value it has at the
	//quad's own boundary.
	//
	//Without the subtraction the profile is cut off at 4.5 sigma with a step of
	//about 1.5e-5 of the peak. For one segment that is invisible. At a turnaround
	//where a thousand segments overlap it is a thousand times 1.5e-5 along a line
	//that is exactly where those thousand boxes end -- a visible polygon edge in
	//the middle of the brightest part of the picture, which is precisely the part
	//the plugin exists to render.
	float across   = InvSqrt2Pi * inv * exp( -0.5 * v * v * inv * inv );
	float pedestal = InvSqrt2Pi * inv * exp( -0.5 * Extent * Extent );
	across = max( across - pedestal, 0.0 );

	float along;
	if( segLength < 0.25 * segSigma )
	{
		//The point limit, taken explicitly rather than left to the CDF.
		//
		//A difference of two nearly equal numbers, each carrying the tanh
		//approximation's ~3e-4 of error, is a difference whose own error is
		//several percent by the time L is a quarter of a sigma. And it is a
		//short segment precisely at the turnarounds and the stationary dots --
		//the features this plugin is about. The limit costs one exp and is exact.
		//
		//`segLength` is flat, so this branch is dynamically uniform across the
		//primitive and the derivatives in neighbouring passes stay defined.
		along = InvSqrt2Pi * inv * exp( -0.5 * u * u * inv * inv );
	}
	else
	{
		float halfLen = 0.5 * segLength;//`half` is a GLSL reserved word
		along = ( ncdf( ( u + halfLen ) * inv ) - ncdf( ( u - halfLen ) * inv ) ) / segLength;
	}

	//R is the fast layer's excitation. The slow layer is not deposited into: it
	//is pumped only by what the fast layer sheds, in the decay pass.
	float deposit = segEnergy * across * along;
	fragColor = vec4( deposit, 0.0, 0.0, 0.0 );
}
)";

} // namespace

const std::string& traceVertex()
{
	static const std::string source = vertexSource( kTraceVertexBody );
	return source;
}

const std::string& traceFragment()
{
	static const std::string source = fragmentSource( kTraceFragmentBody );
	return source;
}

} // namespace vectrix::shaders
