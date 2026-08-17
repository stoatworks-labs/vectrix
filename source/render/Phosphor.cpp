#include "Phosphor.h"

#include <algorithm>
#include <cmath>

namespace vectrix
{
namespace
{
// ---------------------------------------------------------------------------
// The table.
// ---------------------------------------------------------------------------
//
// Taus are t10 / ln(10) from the published decay-to-10% figures, so the numbers
// in the comments are the ones you can look up and the numbers in the code are
// the ones the exponential wants.
//
// Colours are the dominant emission wavelength taken into linear sRGB and
// normalised so the largest component is 1. They are approximations and they are
// necessarily clipped -- a 520 nm line is well outside sRGB and no display in
// this pipeline can show it -- but the *relationships* survive the clipping,
// which is what matters here: P11 is visibly bluer than P7's flash, and P7's
// trail is visibly yellower than P31. Brightness is not in these vectors. It is
// in `efficiency`, on purpose, so that changing a colour cannot silently change
// how bright a phosphor is.
const PhosphorSpec kPhosphors[] = {
	//-----------------------------------------------------------------------
	// P31 -- ZnS:Cu. The standard oscilloscope phosphor and the reference for
	// everything else here. 520 nm, very efficient, and *fast*: 38 us to 10%,
	// with a weak 1 ms tail that is the only reason it photographs at all. At
	// persistence x1 this shows no trail whatsoever at any sane frame rate.
	//-----------------------------------------------------------------------
	{ "P31",
	  16.0e-6f, { 0.18f, 1.00f, 0.38f },
	  400.0e-6f, { 0.22f, 1.00f, 0.42f },
	  0.02f,
	  1.00f,
	  12.0f },

	//-----------------------------------------------------------------------
	// P1 -- Zn2SiO4:Mn, willemite. The pre-war medium-persistence green, and
	// still what most people picture when they picture a radar or a scope.
	// 24 ms to 10%, single layer: what you see is one exponential and nothing
	// else, which is why its trail looks cleaner than P2's.
	//-----------------------------------------------------------------------
	{ "P1",
	  10.4e-3f, { 0.22f, 1.00f, 0.34f },
	  0.0f, { 0.0f, 0.0f, 0.0f },
	  0.0f,
	  0.55f,
	  6.0f },

	//-----------------------------------------------------------------------
	// P2 -- the medium-persistence compromise: a fast blue-green component with
	// a slower yellow-green one behind it, and a heavy transfer between them.
	// The trail is a different colour from the strike and it lags it, which is
	// exactly what the cascade is for.
	//-----------------------------------------------------------------------
	{ "P2",
	  15.0e-3f, { 0.30f, 1.00f, 0.40f },
	  52.0e-3f, { 0.55f, 1.00f, 0.22f },
	  0.35f,
	  0.62f,
	  6.0f },

	//-----------------------------------------------------------------------
	// P7 -- the long-persistence cascade screen, and the reason this renderer
	// has two layers at all. A blue 440 nm flash layer, 90 us to 10%, sitting on
	// a yellow-green 558 nm layer with a 400 ms decay that the flash layer pumps
	// optically. The strike is blue, the trail is yellow-green, and no amount of
	// tinting one decay gets you that.
	//-----------------------------------------------------------------------
	{ "P7",
	  39.0e-6f, { 0.28f, 0.38f, 1.00f },
	  174.0e-3f, { 0.72f, 1.00f, 0.20f },
	  0.75f,
	  0.48f,
	  4.0f },

	//-----------------------------------------------------------------------
	// P11 -- ZnS:Ag. The photographic blue: made to expose film, not to be
	// looked at, which is why its luminous efficiency is a third of P31's while
	// its radiant efficiency is comparable. 50 us to 10%, single layer.
	//-----------------------------------------------------------------------
	{ "P11",
	  22.0e-6f, { 0.22f, 0.42f, 1.00f },
	  0.0f, { 0.0f, 0.0f, 0.0f },
	  0.0f,
	  0.35f,
	  8.0f },

	//-----------------------------------------------------------------------
	// P39 -- Zn2SiO4:Mn,As. Long-persistence green, 150 ms to 10%, single layer.
	// The storage-scope and radar phosphor: a whole figure stays on the glass
	// between refreshes. Saturates early and burns, which is the same property
	// seen twice, and the low saturation figure here is the burn.
	//-----------------------------------------------------------------------
	{ "P39",
	  65.0e-3f, { 0.24f, 1.00f, 0.32f },
	  0.0f, { 0.0f, 0.0f, 0.0f },
	  0.0f,
	  0.85f,
	  3.0f },
};

constexpr int kCount = static_cast< int >( sizeof( kPhosphors ) / sizeof( kPhosphors[ 0 ] ) );

/// A decay factor of exactly 1 is a buffer that never empties, and the operator
/// has no way back from it short of deleting the effect. This is the ceiling: a
/// little under one, so even the longest phosphor at x1000 still drains.
constexpr float kMaxDecay = 0.9995f;

float decayFactor( float tau, float frameSeconds )
{
	if( !( tau > 0.0f ) || !( frameSeconds > 0.0f ) )
		return 0.0f;

	//No guard needed on the exponent's magnitude: exp() of a large negative
	//number underflows to zero, which is the correct answer -- a phosphor whose
	//tau is a thousandth of a frame really has gone out.
	const float factor = std::exp( -frameSeconds / tau );
	return std::clamp( factor, 0.0f, kMaxDecay );
}
} // namespace

int phosphorCount()
{
	return kCount;
}

const PhosphorSpec& phosphor( int index )
{
	if( index < 0 || index >= kCount )
		return kPhosphors[ 0 ];
	return kPhosphors[ index ];
}

PhosphorDecay decayFor( const PhosphorSpec& spec, float persistenceMultiplier, float frameSeconds )
{
	const float mult = std::max( persistenceMultiplier, 1e-4f );

	PhosphorDecay decay;
	decay.fast = decayFactor( spec.tauFast * mult, frameSeconds );

	//A phosphor with no second layer gets a zero decay and a zero transfer, not a
	//tiny one. Otherwise the slow channel accumulates a hundredth of the trace
	//every frame and never quite lets go of it, and the operator sees a faint
	//permanent ghost on a phosphor that has no trail at all.
	if( spec.tauSlow > 0.0f && spec.transfer > 0.0f )
	{
		decay.slow     = decayFactor( spec.tauSlow * mult, frameSeconds );
		decay.transfer = std::clamp( spec.transfer, 0.0f, 1.0f );
	}
	else
	{
		decay.slow     = 0.0f;
		decay.transfer = 0.0f;
	}

	return decay;
}

float persistenceMultiplier( float normalised )
{
	//x0.1 to x1000: four decades, with x1 at a quarter of the way up. The
	//interpolation is in the log, so the control's feel is the same everywhere
	//along it -- which is the whole reason for a log control and not a taper
	//somebody hand-fitted.
	const float t        = std::clamp( normalised, 0.0f, 1.0f );
	const float decades  = -1.0f + 4.0f * t;
	return std::pow( 10.0f, decades );
}

static_assert( kCount == 6, "the six phosphors are documented one by one in Phosphor.h" );

} // namespace vectrix
