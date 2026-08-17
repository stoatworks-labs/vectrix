#include "../Shaders.h"

namespace vectrix::shaders
{
namespace
{

// ---------------------------------------------------------------------------
// Halation: the bright pass and the quarter-resolution downsample.
// ---------------------------------------------------------------------------
//
// The glass faceplate of a tube is thick, and light leaving a phosphor grain
// scatters sideways inside it before it gets out. On a dim trace you never
// notice; on a bright one it is why a stationary dot has a halo around it and
// why the black next to it is never quite black.
//
// Scattering is proportional to the light there is, so this is not really a
// threshold effect -- but the part below the knee is invisible against the trace
// itself, and skipping it keeps the halo off the faint parts of the figure,
// which is where it would read as a blurred duplicate rather than as glass.
//
// Two things differ from the same pass in `old-cathode`. The taps are converted
// to light *individually* rather than averaged first, because the saturation
// curve is not linear and each grain saturates on its own -- four conversions at
// quarter resolution costs the same as one at full. And the graticule is drawn
// in here as well as in the glass pass, from the same prelude function at the
// same face coordinate, so the lines cast a halo of their own instead of being
// the one thing on the face that does not.
const char* const kBrightFragmentBody = R"(
uniform sampler2D PhosphorTexture;
uniform vec2 SourceSize;
uniform vec2 FaceHalf;      //face half-extents in beam units
uniform float Threshold;

in vec2 uv;

out vec4 fragColor;

void main()
{
	//Four bilinear taps at the corners of the destination footprint: a box
	//downsample that does not leave stair-stepping in the halo.
	vec2 texel = 1.0 / max( SourceSize, vec2( 1.0 ) );
	vec3 sum = phosphorEmission( texture( PhosphorTexture, uv + vec2( -1.0, -1.0 ) * texel ).rg )
	         + phosphorEmission( texture( PhosphorTexture, uv + vec2(  1.0, -1.0 ) * texel ).rg )
	         + phosphorEmission( texture( PhosphorTexture, uv + vec2( -1.0,  1.0 ) * texel ).rg )
	         + phosphorEmission( texture( PhosphorTexture, uv + vec2(  1.0,  1.0 ) * texel ).rg );
	vec3 colour = sum * 0.25;

	colour += GraticuleColour * graticuleAt( ( uv * 2.0 - 1.0 ) * FaceHalf );

	float luma = dot( colour, vec3( 0.299, 0.587, 0.114 ) );
	colour *= smoothstep( Threshold, Threshold + 0.35, luma );

	fragColor = vec4( max( colour, vec3( 0.0 ) ), 1.0 );
}
)";

// ---------------------------------------------------------------------------
// The scatter itself, run twice per iteration: once across, once down.
// ---------------------------------------------------------------------------
//
// A Gaussian is the right shape for the ordinary reason -- repeated scattering
// events converge on one -- and separating it into two passes is what makes a
// wide halo affordable. The taps are placed between texels so each bilinear
// fetch does the work of two; the offsets and weights are the standard
// nine-tap kernel collapsed to three fetches, and they are the same numbers
// `old-cathode` uses so the two plugins' haloes match on screen.
const char* const kBlurFragmentBody = R"(
uniform sampler2D SourceTexture;
uniform vec2 Direction;//one texel along the axis being blurred

in vec2 uv;

out vec4 fragColor;

void main()
{
	const float offsets[ 3 ] = float[]( 0.0, 1.3846153846, 3.2307692308 );
	const float weights[ 3 ] = float[]( 0.2270270270, 0.3162162162, 0.0702702703 );

	vec3 sum = texture( SourceTexture, uv ).rgb * weights[ 0 ];
	for( int i = 1; i < 3; ++i )
	{
		sum += texture( SourceTexture, uv + Direction * offsets[ i ] ).rgb * weights[ i ];
		sum += texture( SourceTexture, uv - Direction * offsets[ i ] ).rgb * weights[ i ];
	}

	fragColor = vec4( sum, 1.0 );
}
)";

} // namespace

const std::string& brightFragment()
{
	static const std::string source = fragmentSource( kBrightFragmentBody );
	return source;
}

const std::string& blurFragment()
{
	static const std::string source = fragmentSource( kBlurFragmentBody );
	return source;
}

} // namespace vectrix::shaders
