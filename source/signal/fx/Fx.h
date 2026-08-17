#pragma once

#include "signal/Signal.h"
#include "signal/Smooth.h"

namespace vectrix
{
/**
	How a block treats the two axes.

	This is the control that makes the pictures, and one thing about it is worth
	stating plainly because getting it wrong produces a plausible-looking dud:
	**Stereo must mean de-correlated, not identical.** If a chorus runs the same
	LFO phase on X and on Y, every delayed copy is displaced along the 45-degree
	diagonal and the result reads as one soft blurry double image. Offsetting Y's
	modulation by 90 degrees -- and giving the reverb mutually prime line lengths
	per axis -- is the difference between that and a halo with structure in it.

	`XOnly` and `YOnly` are where most of the interesting pictures come from: an
	effect applied to one deflection axis and not the other is not a filter, it
	is a geometric transform of the figure.
*/
enum class Routing
{
	Stereo,  ///< Both axes, de-correlated.
	XOnly,   ///< X processed, Y passed through untouched.
	YOnly,
	MidSide, ///< M=(x+y)/2, S=(x-y)/2 -- a 45-degree rotation of the deflection plane.
	Mono,    ///< Sum, process once, send to both. Collapses the wet to the diagonal.
	Cross,   ///< Ring modulator only: each axis modulated by the other.
	PingPong,///< Delay only: each axis's output feeds the other's input.
	Count
};

/**
	One pedal.

	`Process` works in place on a block of samples and must not touch `dt`.

	Two rules every block obeys, and the second one is the opposite of the
	fleet's usual GPU habit:

	**Bypass crossfades over 20 milliseconds.** A hard switch puts a step into a
	deflection voltage, which the delay and the reverb downstream will remember
	and hand back as a click, seconds later, long after the operator has stopped
	associating it with the switch.

	**A parameter change never clears state.** Turning the delay time knob must
	not drop the tail on the floor; changing the reverb size must not silence the
	tank. On the GPU side of this fleet, rebuilding on a parameter change is the
	normal and correct thing, which is exactly why this needs writing down.
*/
class FxBlock
{
public:
	virtual ~FxBlock() = default;

	/// Sample rate changed, or the plugin is starting. Allocate here.
	virtual void Prepare( double sampleRate ) = 0;

	/// Drop the tails. Called from the Reset control and on a fresh instance --
	/// NOT from a parameter change.
	virtual void Reset() = 0;

	/// Process a block in place.
	virtual void Process( Sample* buffer, int n ) = 0;

	void SetEnabled( bool on )
	{
		bypass.SetEngaged( on );
	}
	void SnapEnabled( bool on )
	{
		bypass.Snap( on );
	}
	bool Idle() const
	{
		return bypass.Idle();
	}

protected:
	/// Apply the bypass crossfade for one sample. `dryX`/`dryY` are what came in.
	void blend( float dryX, float dryY, float& x, float& y )
	{
		const float w = bypass.Next();
		x             = dryX + ( x - dryX ) * w;
		y             = dryY + ( y - dryY ) * w;
	}

	Crossfade bypass;
	double fs = 96000.0;
};

/// Split a stereo pair into the two values a routing wants processed, and put
/// the result back. Kept in one place so a new block cannot get M/S subtly wrong.
struct RoutingSplit
{
	Routing mode = Routing::Stereo;

	void In( float x, float y, float& a, float& b ) const
	{
		switch( mode )
		{
			case Routing::MidSide:
				a = 0.5f * ( x + y );
				b = 0.5f * ( x - y );
				break;
			case Routing::Mono:
				a = 0.5f * ( x + y );
				b = a;
				break;
			default:
				a = x;
				b = y;
				break;
		}
	}

	void Out( float aIn, float bIn, float a, float b, float& x, float& y ) const
	{
		switch( mode )
		{
			case Routing::MidSide:
				x = a + b;
				y = a - b;
				break;
			case Routing::Mono:
				x = a;
				y = a;
				break;
			case Routing::XOnly:
				x = a;
				y = bIn;
				break;
			case Routing::YOnly:
				x = aIn;
				y = b;
				break;
			default:
				x = a;
				y = b;
				break;
		}
	}

	/// Does this routing need the second channel processed at all? X Only and
	/// Y Only do not, and skipping saves half the work in the common case.
	bool NeedsBoth() const
	{
		return mode != Routing::XOnly && mode != Routing::YOnly;
	}
};

} // namespace vectrix
