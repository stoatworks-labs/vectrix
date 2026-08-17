#include "signal/sources/Oscillator.h"

#include <algorithm>
#include <cmath>

namespace vectrix
{

const Ratio kRatios[ kRatioCount ] = {
	{ 1, 1 }, { 2, 1 }, { 1, 2 }, { 3, 2 }, { 2, 3 }, { 4, 3 }, { 3, 4 },
	{ 5, 4 }, { 4, 5 }, { 5, 3 }, { 3, 5 }, { 3, 1 }, { 1, 3 }, { 5, 2 },
	{ 2, 5 }, { 7, 4 }, { 4, 7 }, { 8, 5 }, { 5, 8 }, { 9, 8 }, { 8, 9 },
};

const char* const kRatioNames[ kRatioCount ] = {
	"1:1", "2:1", "1:2", "3:2", "2:3", "4:3", "3:4",
	"5:4", "4:5", "5:3", "3:5", "3:1", "1:3", "5:2",
	"2:5", "7:4", "4:7", "8:5", "5:8", "9:8", "8:9",
};

namespace
{
/**
	The polyBLEP correction, subtracted at each discontinuity.

	Worth its handful of flops for two reasons that are both about the picture
	rather than the sound. An aliased saw at 300 Hz puts energy at frequencies
	that are not harmonically related to anything, so the figure develops a slow
	crawl that no control explains and that reads as a bug in the phase
	accumulator. And the wavefolder downstream multiplies those aliases by its
	own harmonics, so what starts as a faint shimmer becomes structure.

	polyBLEP is enough here. MinBLEP tables and oscillator oversampling buy
	precision that a deflection signal cannot show.
*/
inline float polyBlep( float t, float dt )
{
	if( dt <= 0.0f )
		return 0.0f;

	if( t < dt )
	{
		t /= dt;
		return t + t - t * t - 1.0f;
	}
	if( t > 1.0f - dt )
	{
		t = ( t - 1.0f ) / dt;
		return t * t + t + t + 1.0f;
	}
	return 0.0f;
}

constexpr double kTwoPi = 6.283185307179586476925286766559;
} // namespace

void Oscillator::Prepare( double sampleRate )
{
	fs = sampleRate > 0.0 ? sampleRate : 96000.0;
	Reset();
}

void Oscillator::Reset()
{
	axisX = Axis{};
	axisY = Axis{};
}

float Oscillator::step( Axis& axis, Wave wave, float width, double offset, bool& blank )
{
	double read = axis.phase + offset;
	read -= std::floor( read );

	const float p  = static_cast< float >( read );
	const float dt = static_cast< float >( axis.inc );

	blank        = false;
	axis.wrapped = false;

	float value = 0.0f;

	switch( wave )
	{
		case Wave::Sine:
			//std::sin directly, not a lookup table. A table's interpolation error
			//shows up as a subtly non-circular circle, which is precisely the
			//artefact this plugin refuses to draw. At 1600 samples a frame it
			//costs microseconds.
			value = static_cast< float >( std::sin( kTwoPi * read ) );
			break;

		case Wave::Triangle:
			//Aliasing here is 12 dB/octave down and genuinely invisible on a
			//deflection signal. Accepted rather than corrected.
			value = 1.0f - 4.0f * std::fabs( p - 0.5f );
			break;

		case Wave::Saw:
			value = 2.0f * p - 1.0f;
			value -= polyBlep( p, dt );
			blank = p < dt || p > 1.0f - dt;
			break;

		case Wave::Ramp:
			value = 1.0f - 2.0f * p;
			value += polyBlep( p, dt );
			blank = p < dt || p > 1.0f - dt;
			break;

		case Wave::Pulse:
		{
			const float w = std::clamp( width, 0.02f, 0.98f );
			value         = p < w ? 1.0f : -1.0f;
			value -= polyBlep( p, dt );
			//The falling edge is the rising edge of a phase shifted by the duty
			//cycle, which is why the same function serves both.
			float shifted = p - w;
			if( shifted < 0.0f )
				shifted += 1.0f;
			value += polyBlep( shifted, dt );
			blank = p < dt || p > 1.0f - dt || ( shifted < dt ) || ( shifted > 1.0f - dt );
			break;
		}

		case Wave::Noise:
			value = rng.Bipolar();
			break;

		case Wave::SampleHold:
			value = axis.holdValue;
			break;

		case Wave::Count:
		default:
			value = 0.0f;
			break;
	}

	axis.phase += axis.inc;
	if( axis.phase >= 1.0 )
	{
		axis.phase -= std::floor( axis.phase );
		axis.wrapped = true;
		if( wave == Wave::SampleHold )
			axis.holdValue = rng.Bipolar();
	}

	return value;
}

void Oscillator::Render( Sample* out, int n, double dtPerSample )
{
	const Ratio r = kRatios[ std::clamp( params.ratioIndex, 0, kRatioCount - 1 ) ];

	const double detune = 1.0 + static_cast< double >( std::clamp( params.detune, -0.02f, 0.02f ) );
	const double fx     = std::clamp( static_cast< double >( params.freqX ), 0.01, 2000.0 );
	const double fy     = params.freeY
	                          ? std::clamp( static_cast< double >( params.freqY ), 0.01, 2000.0 )
	                          : fx * ( static_cast< double >( r.num ) / static_cast< double >( r.den ) );

	axisX.inc = fx / fs;
	axisY.inc = fy * detune / fs;

	const double phaseOffset = static_cast< double >( params.phaseY );

	for( int i = 0; i < n; ++i )
	{
		bool blankX = false;
		bool blankY = false;

		const float x = step( axisX, params.waveX, params.pwmX, 0.0, blankX );

		//Hard sync: Y restarts whenever X wraps. Three lines, a real eurorack
		//idiom, and it aliases badly by design -- that is what it sounds and
		//looks like on the hardware too.
		if( params.hardSync && axisX.wrapped )
		{
			axisY.phase = 0.0;
			if( params.waveY == Wave::SampleHold )
				axisY.holdValue = rng.Bipolar();
		}

		const float y = step( axisY, params.waveY, params.pwmY, phaseOffset, blankY );

		//Blank on Retrace cuts the gun across the discontinuity rather than
		//having the renderer special-case a fast segment. This is the cheapest
		//demonstration in the plugin that the house rule is being obeyed: saw-X
		//against saw-Y becomes a clean raster because the beam is *off* during
		//flyback, not because anything decided not to draw the flyback line.
		const bool blanked = params.blankRetrace && ( blankX || blankY );

		out[ i ].x  = x;
		out[ i ].y  = y;
		out[ i ].z  = blanked ? 0.0f : 1.0f;
		out[ i ].dt = static_cast< float >( dtPerSample );
	}
}

} // namespace vectrix
