#pragma once

#include "signal/Rng.h"
#include "signal/Smooth.h"
#include "signal/sources/Source.h"

namespace vectrix
{
/// One axis's waveform.
enum class Wave
{
	Sine,
	Triangle,
	Saw,
	Ramp,
	Pulse,
	Noise,
	SampleHold,
	Count
};

/**
	The frequency ratio between the two axes, as a small rational.

	A *slider* here would be a bug that looks like a feature. Landing on 2.001:1
	instead of 2:1 makes the figure rotate slowly and forever, and the operator
	discovers there is no way to stop it -- the control has no detent and every
	value near the one they want is also wrong. So the ratio is a dropdown of
	exact rationals, and the drift they might actually want is a separate Detune
	control they can set to zero.
*/
struct Ratio
{
	int num;
	int den;
};

constexpr int kRatioCount = 21;
extern const Ratio kRatios[ kRatioCount ];
extern const char* const kRatioNames[ kRatioCount ];

/// Everything the oscillator reads. Filled from Controls each block.
struct OscillatorParams
{
	Wave waveX      = Wave::Sine;
	Wave waveY      = Wave::Sine;
	float freqX     = 30.0f; ///< Hz
	int ratioIndex  = 0;     ///< index into kRatios
	bool freeY      = false; ///< ignore the ratio and use freqY directly
	float freqY     = 30.0f; ///< Hz, used only when freeY
	float phaseY    = 0.25f; ///< turns, 0..1. 0.25 = 90 degrees = a circle
	float pwmX      = 0.5f;
	float pwmY      = 0.5f;
	float detune    = 0.0f; ///< fraction, +-0.02
	bool hardSync   = false;
	bool blankRetrace = false;
};

/**
	Two independent oscillators driving X and Y.

	The phases are **integrated**, never computed as `time * frequency`. The
	obvious form rescales the whole history the instant the Frequency control
	moves, so the figure jumps to a different point in its cycle on every knob
	touch -- which for a model of a signal path is not merely ugly, it is wrong:
	a real VCO's phase is continuous through a frequency change.

	The honest consequence, stated because it is not guessable: changing the
	ratio leaves a permanent phase offset between the two axes, so the figure
	comes back in a different orientation than it left in. That is what Phase Y
	is for, and what Reset is for. The alternative -- deriving Y's phase from X's
	-- is stable under ratio changes and jumps the figure on every frequency
	change instead, which is the worse of the two.
*/
class Oscillator : public Source
{
public:
	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Render( Sample* out, int n, double dtPerSample ) override;

	void SetParams( const OscillatorParams& p )
	{
		params = p;
	}
	void SetSeed( std::uint64_t seed )
	{
		rng.Seed( seed );
	}

private:
	struct Axis
	{
		double phase    = 0.0; ///< turns, [0,1). double: a float accumulator run
		                       ///< at audio rate loses its fractional bits within
		                       ///< minutes and the figure visibly stalls.
		double inc      = 0.0;
		float holdValue = 0.0f; ///< sample & hold's current step
		bool wrapped    = false;
	};

	/// Advance one axis and return its value.
	///
	/// The waveform is read at `phase + offset` while the accumulator advances
	/// from `phase`, which is what keeps Phase Y a standing offset rather than a
	/// one-shot nudge that does nothing the second time the knob is turned.
	/// `blank` is set when this sample sits inside a discontinuity that Blank on
	/// Retrace should cut the gun for.
	float step( Axis& axis, Wave wave, float width, double offset, bool& blank );

	OscillatorParams params;
	Axis axisX;
	Axis axisY;
	Rng rng;
	double fs = 96000.0;
};

} // namespace vectrix
