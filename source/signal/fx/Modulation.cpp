#include "signal/fx/Chain.h"

#include <algorithm>
#include <cmath>

namespace vectrix
{
namespace
{
constexpr double kTwoPi = 6.283185307179586476925286766559;

/**
	Y's modulation runs a quarter cycle behind X's.

	This one constant is the difference between "a soft double image" and "a
	smeared diagonal streak". With both axes modulated in phase, every delayed
	copy of the figure is displaced along the 45-degree diagonal and the three
	copies of a chorus pile up into one blur. A quarter cycle apart, the copies
	orbit the original instead, and the halo has structure in it.
*/
constexpr double kAxisPhaseOffset = 0.25;
} // namespace

//---------------------------------------------------------------------------
// Phaser
//---------------------------------------------------------------------------

void Phaser::Prepare( double sampleRate )
{
	fs = sampleRate;
	bypass.Prepare( sampleRate );
	Reset();
}

void Phaser::Reset()
{
	for( int i = 0; i < kMaxStages; ++i )
	{
		stagesA[ i ].Reset();
		stagesB[ i ].Reset();
	}
	feedbackA = feedbackB = 0.0f;
	lfoPhase              = 0.0;
}

void Phaser::SetParams( const PhaserParams& p )
{
	params = p;
	SetEnabled( p.enabled );
}

void Phaser::Process( Sample* buffer, int n )
{
	if( Idle() )
		return;

	RoutingSplit split{ params.routing };

	const int stages   = std::clamp( params.stages, 1, kMaxStages );
	const float depth  = std::clamp( params.depth, 0.0f, 1.0f );
	const float fb     = std::clamp( params.feedback, 0.0f, 0.95f );
	const float mix    = std::clamp( params.mix, 0.0f, 1.0f );
	const double inc   = std::clamp( static_cast< double >( params.rate ), 0.01, 10.0 ) / fs;
	const double centre = std::clamp( static_cast< double >( params.centre ), 100.0, 8000.0 );

	for( int i = 0; i < n; ++i )
	{
		const float dryX = buffer[ i ].x;
		const float dryY = buffer[ i ].y;

		float a, b;
		split.In( dryX, dryY, a, b );

		//The sweep is exponential in frequency, because that is how the ear and
		//the eye both read it: a linear sweep spends most of its time at the top
		//and lurches through the bottom.
		auto sweepAt = [ & ]( double phase ) {
			const double lfo = std::sin( kTwoPi * phase );
			return centre * std::pow( 4.0, lfo * depth );
		};

		const float coeffA = allpassCoeff( sweepAt( lfoPhase ), fs );
		const float coeffB = allpassCoeff( sweepAt( lfoPhase + kAxisPhaseOffset ), fs );

		float wetA = a + feedbackA * fb;
		for( int s = 0; s < stages; ++s )
			wetA = stagesA[ s ].Process( wetA, coeffA );
		feedbackA = wetA;

		float wetB = b;
		if( split.NeedsBoth() )
		{
			wetB = b + feedbackB * fb;
			for( int s = 0; s < stages; ++s )
				wetB = stagesB[ s ].Process( wetB, coeffB );
			feedbackB = wetB;
		}

		//At Mix 1.0 there is no dry path, so there are no notches at all -- only
		//pure phase shift, which between X and Y is exactly the rotation of a
		//Lissajous ellipse. That is a genuinely different instrument from Mix
		//0.5 and both are worth having; see the class comment.
		const float outA = a + ( wetA - a ) * mix;
		const float outB = split.NeedsBoth() ? b + ( wetB - b ) * mix : b;

		float x, y;
		split.Out( a, b, outA, outB, x, y );

		blend( dryX, dryY, x, y );
		buffer[ i ].x = x;
		buffer[ i ].y = y;

		lfoPhase += inc;
		if( lfoPhase >= 1.0 )
			lfoPhase -= std::floor( lfoPhase );
	}
}

//---------------------------------------------------------------------------
// Flanger
//---------------------------------------------------------------------------

void Flanger::Prepare( double sampleRate )
{
	fs = sampleRate;
	bypass.Prepare( sampleRate );
	//2048 samples is a little over 10 ms at 192 kHz, which is the parameter's top.
	lineA.Prepare( 2048 );
	lineB.Prepare( 2048 );
	Reset();
}

void Flanger::Reset()
{
	lineA.Reset();
	lineB.Reset();
	feedbackA = feedbackB = 0.0f;
	lfoPhase              = 0.0;
}

void Flanger::SetParams( const FlangerParams& p )
{
	params = p;
	SetEnabled( p.enabled );
}

void Flanger::Process( Sample* buffer, int n )
{
	if( Idle() )
		return;

	RoutingSplit split{ params.routing };

	const float depth = std::clamp( params.depth, 0.0f, 1.0f );
	//Negative feedback is allowed and is where the hollow through-zero character
	//comes from, so this clamps symmetrically rather than to [0,1].
	const float fb    = std::clamp( params.feedback, -0.95f, 0.95f );
	const float mix   = std::clamp( params.mix, 0.0f, 1.0f );
	const double inc  = std::clamp( static_cast< double >( params.rate ), 0.05, 5.0 ) / fs;
	const double base = std::clamp( static_cast< double >( params.delayMs ), 0.1, 10.0 ) * 0.001 * fs;

	for( int i = 0; i < n; ++i )
	{
		const float dryX = buffer[ i ].x;
		const float dryY = buffer[ i ].y;

		float a, b;
		split.In( dryX, dryY, a, b );

		auto delayAt = [ & ]( double phase ) {
			const double lfo = std::sin( kTwoPi * phase );
			return std::max( 2.0, base * ( 1.0 + depth * 0.9 * lfo ) );
		};

		const float wetA = lineA.Read( delayAt( lfoPhase ) );
		lineA.Write( a + wetA * fb );
		feedbackA = wetA;

		float wetB = b;
		if( split.NeedsBoth() )
		{
			wetB = lineB.Read( delayAt( lfoPhase + kAxisPhaseOffset ) );
			lineB.Write( b + wetB * fb );
			feedbackB = wetB;
		}

		const float outA = a + ( wetA - a ) * mix;
		const float outB = split.NeedsBoth() ? b + ( wetB - b ) * mix : b;

		float x, y;
		split.Out( a, b, outA, outB, x, y );

		blend( dryX, dryY, x, y );
		buffer[ i ].x = x;
		buffer[ i ].y = y;

		lfoPhase += inc;
		if( lfoPhase >= 1.0 )
			lfoPhase -= std::floor( lfoPhase );
	}
}

//---------------------------------------------------------------------------
// Chorus
//---------------------------------------------------------------------------

void Chorus::Prepare( double sampleRate )
{
	fs = sampleRate;
	bypass.Prepare( sampleRate );
	//16384 samples is 85 ms at 192 kHz -- comfortably past 40 ms base plus 8 ms
	//of sweep.
	lineA.Prepare( 16384 );
	lineB.Prepare( 16384 );
	Reset();
}

void Chorus::Reset()
{
	lineA.Reset();
	lineB.Reset();
	lfoPhase = 0.0;
}

void Chorus::SetParams( const ChorusParams& p )
{
	params = p;
	SetEnabled( p.enabled );
}

void Chorus::Process( Sample* buffer, int n )
{
	if( Idle() )
		return;

	const float depth = std::clamp( params.depth, 0.0f, 1.0f );
	const float mix   = std::clamp( params.mix, 0.0f, 1.0f );
	const double inc  = std::clamp( static_cast< double >( params.rate ), 0.05, 3.0 ) / fs;
	const double base = std::clamp( static_cast< double >( params.delayMs ), 10.0, 40.0 ) * 0.001 * fs;
	const double sweep = depth * 0.008 * fs;

	for( int i = 0; i < n; ++i )
	{
		const float dryX = buffer[ i ].x;
		const float dryY = buffer[ i ].y;

		lineA.Write( dryX );
		lineB.Write( dryY );

		//Three taps per axis, no feedback. Three softly drifting copies at
		//slightly different positions -- the halo as a consequence of three
		//delayed copies rather than as a blur. They drift independently, so the
		//halo has structure a Gaussian blur can never have.
		float wetA = 0.0f;
		float wetB = 0.0f;
		for( int t = 0; t < kTaps; ++t )
		{
			const double phaseA = lfoPhase * kTapRate[ t ];
			const double phaseB = phaseA + kAxisPhaseOffset;
			wetA += lineA.Read( std::max( 2.0, base + sweep * std::sin( kTwoPi * phaseA ) ) );
			wetB += lineB.Read( std::max( 2.0, base + sweep * std::sin( kTwoPi * phaseB ) ) );
		}
		wetA *= 1.0f / kTaps;
		wetB *= 1.0f / kTaps;

		float x = dryX + ( wetA - dryX ) * mix;
		float y = dryY + ( wetB - dryY ) * mix;

		blend( dryX, dryY, x, y );
		buffer[ i ].x = x;
		buffer[ i ].y = y;

		lfoPhase += inc;
		if( lfoPhase >= 1.0 )
			lfoPhase -= std::floor( lfoPhase );
	}
}

} // namespace vectrix
