#include "../Shaders.h"

namespace vectrix::shaders
{
namespace
{

// ---------------------------------------------------------------------------
// Two layers, and the cascade between them.
// ---------------------------------------------------------------------------
//
// A P7 is not two phosphors sitting side by side. It is a blue flash layer with
// a yellow-green layer underneath it, and the second one is pumped by the light
// the first one emits -- so the trail is not a dimmer copy of the strike, it is a
// different colour from it, and it only appears where the strike was bright
// enough to excite the layer below. That cascade is this line:
//
//     slow = slow * DecaySlow + fast * (1 - DecayFast) * Transfer
//
// `fast * (1 - DecayFast)` is exactly the excitation the fast layer shed this
// frame. `Transfer` is how much of it the second layer catches. Modelling the
// trail as an independent decay of the same input -- which is what it looks like
// from the outside -- gets the colour right and the *timing* wrong: the trail
// would start the instant the beam arrived instead of building behind it.
//
// This pass runs with blending disabled and the trace pass then draws into the
// same target additively. One buffer, one combine, and the decayed history is
// already sitting there when the deposits land on it.
const char* const kDecayFragmentBody = R"(
uniform sampler2D HistoryTexture;
uniform float DecayFast;
uniform float DecaySlow;
uniform float Transfer;
uniform float Ceiling;

in vec2 uv;

out vec4 fragColor;

void main()
{
	vec2 h = texture( HistoryTexture, uv ).rg;

	//The backstop. Every other guard in the renderer is in front of this buffer;
	//this one is behind it, because a ping-ponged accumulator is the one place
	//where a single bad value is permanent. NaN * DecayFast is NaN, forever, and
	//the operator's only remedy would be to delete the effect and add it again.
	//
	//Comparisons rather than isnan/isinf, for the reason given in Trace.cpp.
	if( !( h.x > -1e30 && h.x < 1e30 ) )
		h.x = 0.0;
	if( !( h.y > -1e30 && h.y < 1e30 ) )
		h.y = 0.0;

	float fast = h.r * DecayFast;
	float slow = h.g * DecaySlow + h.r * ( 1.0 - DecayFast ) * Transfer;

	//The ceiling is not the saturation curve -- that is applied at readout, in
	//the prelude, and it is what actually shapes a bright trace. This is only
	//here so that a runaway cannot climb until it reaches the top of a 32-bit
	//float and becomes an inf that the check above would then have to catch every
	//frame for the rest of the session.
	fragColor = vec4( min( fast, Ceiling ), min( slow, Ceiling ), 0.0, 0.0 );
}
)";

} // namespace

const std::string& decayFragment()
{
	static const std::string source = fragmentSource( kDecayFragmentBody );
	return source;
}

} // namespace vectrix::shaders
