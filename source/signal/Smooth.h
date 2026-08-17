#pragma once

#include <cmath>

namespace vectrix
{
/**
	A one-pole smoother for a parameter.

	FFGL delivers one parameter value per video frame. Applied raw, every knob
	move is a step change at a block boundary -- and a step change in a deflection
	voltage is a discontinuity the delay lines and the reverb will faithfully
	remember and repeat back at you as a click, for seconds. Smoothing is not
	polish here; it is what stops a knob move being audible in the picture a
	second later.
*/
class Smooth
{
public:
	/// `tau` is the time constant in seconds. 5 ms is the house default: fast
	/// enough that a knob feels connected, slow enough to cross a frame.
	void Prepare( double sampleRate, double tau = 0.005 )
	{
		coeff = sampleRate > 0.0 && tau > 0.0
		            ? 1.0f - static_cast< float >( std::exp( -1.0 / ( sampleRate * tau ) ) )
		            : 1.0f;
	}

	/// Jump straight to a value without gliding. For a fresh instance and for a
	/// preset recall, where a glide from whatever was there before is wrong.
	void Snap( float value )
	{
		current = value;
	}

	void SetTarget( float value )
	{
		target = value;
	}

	float Next()
	{
		current += ( target - current ) * coeff;
		return current;
	}

	float Value() const
	{
		return current;
	}

	/// True once the glide has effectively finished. Blocks that recompute
	/// expensive coefficients use this to stop recomputing them every sample.
	bool Settled() const
	{
		return std::fabs( target - current ) < 1e-6f;
	}

private:
	float coeff   = 1.0f;
	float current = 0.0f;
	float target  = 0.0f;
};

/**
	An equal-power crossfade used for bypass.

	Every FX block crossfades over 20 ms rather than switching, and -- this is
	the part that matters -- **it does not clear its state when bypassed**. A
	delay that forgot its buffer on bypass would drop its tail on the floor and
	come back silent; the physical analogue is a true-bypass footswitch, which
	does not empty the tank.
*/
class Crossfade
{
public:
	void Prepare( double sampleRate, double seconds = 0.020 )
	{
		step = sampleRate > 0.0 && seconds > 0.0
		           ? static_cast< float >( 1.0 / ( sampleRate * seconds ) )
		           : 1.0f;
	}

	void SetEngaged( bool on )
	{
		target = on ? 1.0f : 0.0f;
	}

	void Snap( bool on )
	{
		target = current = on ? 1.0f : 0.0f;
	}

	float Next()
	{
		if( current < target )
			current = current + step > target ? target : current + step;
		else if( current > target )
			current = current - step < target ? target : current - step;
		return current;
	}

	float Value() const
	{
		return current;
	}

	/// Fully bypassed and not moving -- the block can skip its work entirely.
	bool Idle() const
	{
		return current <= 0.0f && target <= 0.0f;
	}

private:
	float step    = 1.0f;
	float current = 0.0f;
	float target  = 0.0f;
};

} // namespace vectrix
