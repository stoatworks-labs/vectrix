#include "../Shaders.h"

namespace vectrix::shaders
{
namespace
{

// ---------------------------------------------------------------------------
// The faceplate: where excitation stops being a number and becomes light coming
// off a sheet of glass that you are looking at from somewhere.
// ---------------------------------------------------------------------------
//
// The order of operations is the physical one, and it matters:
//
//   1. Undo the view. Work out which point on the tube's face this output pixel
//      is looking at, by intersecting the eye ray with the plane of the screen.
//      Everything after this is in the tube's own coordinates.
//   2. Undo the curvature. The face is not flat, so what is painted on it
//      bulges; sampling has to bulge the opposite way.
//   3. Read the phosphor, turn it into light, add the graticule and the halo.
//   4. Put the clip behind all of it, through the filter.
//
// **The clip is painted on the face, not composited behind the plugin.** It is
// sampled at the face coordinate after steps 1 and 2, so it curves with the
// glass, it vignettes, it sits underneath the graticule and it shares the
// halation. And the faceplate multiplies it in the shader rather than through a
// blend func, because no blend func can express "the filter darkens what is
// behind it": a blend equation gets the plugin's output and the destination, and
// the filter is neither.
//
// **Exact passthrough is by construction, not by tuning.** At Beam 0, FaceBlack
// 0, Curvature 0, Vignette 0, Graticule 0 and no perspective, every operation
// below is either an addition of a literal zero or a multiplication by a literal
// one, and the clip coordinate is the output coordinate rather than a value that
// rounds to it. That is what the branches on FaceBlack and on the warp are for.
// A faceplate that multiplied by 0.999 would be invisible in isolation and
// perfectly visible in an A/B against the bypassed layer.
const char* const kGlassFragmentBody = R"(
uniform sampler2D PhosphorTexture;
uniform sampler2D BloomTexture;
uniform sampler2D ClipTexture;

uniform vec2  OutputSize;
uniform vec2  FaceHalf;      //face half-extents in beam units: (aspect, 1)
uniform float FaceFit;       //beam units -> square output units
uniform float CornerRadius;  //0..1 of the shorter half-extent; 1 on a square face is a circle
uniform float Curvature;
uniform float Vignette;
uniform float Halation;
uniform float PerspectiveX;
uniform float PerspectiveY;

uniform vec3  FilterTransmission;//what the contrast filter passes, per channel
uniform float FaceBlack;         //how much of the faceplate is actually in front
uniform float Opacity;

uniform float HasClip;  //0 for the source build, which has no input at all
uniform vec2  ClipMaxUV;//the host's padding, applied only where the clip is read

in vec2 uv;

out vec4 fragColor;

const float FOCAL = 2.4;

mat3 rotationX( float a )
{
	float s = sin( a ), c = cos( a );
	return mat3( 1.0, 0.0, 0.0,
	             0.0, c, s,
	             0.0, -s, c );
}

mat3 rotationY( float a )
{
	float s = sin( a ), c = cos( a );
	return mat3( c, 0.0, -s,
	             0.0, 1.0, 0.0,
	             s, 0.0, c );
}

void main()
{
	float aspect = OutputSize.x / max( OutputSize.y, 1.0 );

	//----------------------------------------------------------------------
	// 1. Undo the view.
	//----------------------------------------------------------------------
	vec2 p = uv * 2.0 - 1.0;
	p.x *= aspect;//square units, so a rotation is a rotation

	mat3 orientation = rotationY( PerspectiveX ) * rotationX( PerspectiveY );
	vec3 dir    = vec3( p, FOCAL );
	vec3 normal = orientation * vec3( 0.0, 0.0, 1.0 );
	vec3 centre = vec3( 0.0, 0.0, FOCAL );

	//Guarded rather than branched: an early return here would leave the
	//derivatives below undefined for the whole quad, and both the face edge and
	//the graticule are anti-aliased on them.
	float denom = dot( normal, dir );
	denom = denom >= 0.0 ? max( denom, 1e-4 ) : min( denom, -1e-4 );
	float t = dot( normal, centre ) / denom;

	vec3 local    = transpose( orientation ) * ( t * dir - centre );
	float inFront = step( 1e-4, t );//the face is behind the eye at absurd angles

	//At zero perspective this is exact: the rotations are identity, t is exactly
	//one, and local.xy comes back as p unchanged.
	vec2 viewed = local.xy / max( FaceFit, 1e-6 );

	//----------------------------------------------------------------------
	// 2. Undo the curvature. The face bulges, so the sampling pinches.
	//----------------------------------------------------------------------
	//
	// Divided through by the expansion at the corner, which is overscan: a set
	// deliberately scans a raster larger than its own face so the picture reaches
	// the bezel on all four sides. Without it the distortion pulls the corners
	// inside the glass and shows black beyond them, and the shape you see at the
	// edge becomes an artefact of the curvature rather than the shape of the
	// tube -- which leaves Corner Radius doing nothing at any useful curvature.
	//
	// The coupling to Curvature is real: more distortion needs more overscan to
	// cover the same face. Kept exactly as `old-cathode` has it, so the two
	// plugins' tubes are the same tube.
	vec2 n = viewed / FaceHalf;//+-1 at the face edges, so 0.5*dot at the corner is 1
	float bulge = ( 1.0 + Curvature * 0.5 * dot( n, n ) ) / ( 1.0 + Curvature );
	vec2 face = viewed * bulge;

	vec2 faceUV = face / ( 2.0 * FaceHalf ) + 0.5;

	//----------------------------------------------------------------------
	// 3. The glass itself.
	//----------------------------------------------------------------------
	//
	// A rounded rectangle in the tube's own coordinates, so the bezel keeps its
	// shape when the set is turned away from you. Radius 1 on a square face
	// reduces this exactly to length(face) - 1, which is a circle -- which is why
	// a lab scope and a television are the same geometry with different numbers
	// rather than two code paths.
	float radius = clamp( CornerRadius, 0.0, 1.0 ) * min( FaceHalf.x, FaceHalf.y );
	vec2 q   = abs( face ) - ( FaceHalf - radius );
	float sd = length( max( q, vec2( 0.0 ) ) ) + min( max( q.x, q.y ), 0.0 ) - radius;
	float aa = max( fwidth( sd ), 1e-5 );
	float faceMask = ( 1.0 - smoothstep( -aa, aa, sd ) ) * inFront;

	//Curvature can push the sample outside the face buffer before the mask cuts
	//it off, and a clamped texture edge out there would smear the last row of
	//phosphor across the bezel.
	vec2 beyond = step( vec2( 0.0 ), -faceUV ) + step( vec2( 1.0 ), faceUV );
	faceMask *= 1.0 - clamp( beyond.x + beyond.y, 0.0, 1.0 );

	//1.0 - 0.0 * anything is exactly 1.0.
	float vig = 1.0 - Vignette * smoothstep( 0.25, 1.5, length( n * vec2( 0.92, 1.0 ) ) );

	vec3 emission = phosphorEmission( texture( PhosphorTexture, faceUV ).rg ) * vig;
	emission += GraticuleColour * graticuleAt( face );
	emission *= faceMask;

	vec3 halo = texture( BloomTexture, faceUV ).rgb * faceMask;

	//----------------------------------------------------------------------
	// 4. What is behind the glass.
	//----------------------------------------------------------------------
	vec4 clipTexel = vec4( 0.0 );
	if( HasClip > 0.5 )
	{
		//The warp, undone back into output coordinates. At zero curvature and
		//zero perspective the round trip through `aspect` is a multiply and a
		//divide that are not required to cancel in floating point, so that case
		//takes the coordinate it already has instead of one that is nearly it.
		vec2 clipUV;
		if( Curvature != 0.0 || PerspectiveX != 0.0 || PerspectiveY != 0.0 )
		{
			vec2 warped = local.xy * bulge;
			clipUV = vec2( warped.x / aspect, warped.y ) * 0.5 + 0.5;
		}
		else
		{
			clipUV = uv;
		}
		clipTexel = texture( ClipTexture, clipUV * ClipMaxUV );
	}

	//The filter carries its tint into the colour and only the mask into the
	//alpha: a green contrast filter darkens what is behind it without making it
	//any less opaque. Both are literal ones when the faceplate is not there.
	vec3 faceplate   = vec3( 1.0 );
	float faceplateA = 1.0;
	if( FaceBlack > 0.0 )
	{
		faceplate  = mix( vec3( 1.0 ), FilterTransmission * faceMask, FaceBlack );
		faceplateA = mix( 1.0, faceMask, FaceBlack );
	}

	vec3 through   = clipTexel.rgb * faceplate;
	float throughA = clipTexel.a * faceplateA;

	vec3 emitted = emission + halo * Halation;
	vec3 colour  = through + emitted;

	//----------------------------------------------------------------------
	// Alpha, premultiplied, which is what Resolume composites in.
	//----------------------------------------------------------------------
	//
	// The tube's own light is opaque -- glass that is emitting is not something
	// you see through. Beyond that:
	//
	//   source build: the face is the object, so alpha is the face mask and the
	//                 layer below shows around it.
	//   effect build: the clip keeps its own alpha, and the faceplate is only as
	//                 opaque as FaceBlack says it is. At FaceBlack 0 that is
	//                 zero, which is what makes the passthrough exact for a
	//                 semi-transparent clip as well as for an opaque one.
	float emissive = clamp( max( emitted.r, max( emitted.g, emitted.b ) ), 0.0, 1.0 );
	float coverage = HasClip > 0.5 ? max( FaceBlack * faceMask, emissive ) : faceMask;

	float alpha = clamp( max( throughA, coverage ), 0.0, 1.0 ) * Opacity;

	//Only the floor is clamped. Nothing here can produce negative light, so a
	//negative value means the clip arrived with one; clamping the ceiling as well
	//would quietly change a passthrough of any clip that carries values above 1,
	//which some do.
	fragColor = vec4( max( colour, vec3( 0.0 ) ), alpha );
}
)";

} // namespace

const std::string& glassFragment()
{
	static const std::string source = fragmentSource( kGlassFragmentBody );
	return source;
}

} // namespace vectrix::shaders
