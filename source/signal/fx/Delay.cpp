#include "signal/fx/Delay.h"

#include <algorithm>
#include <cmath>

namespace vectrix
{

void Delay::Prepare( double sampleRate )
{
	fs = sampleRate;
	bypass.Prepare( sampleRate );

	lineA.Prepare( kLength );
	lineB.Prepare( kLength );
	dampLowA.Prepare( sampleRate );
	dampLowB.Prepare( sampleRate );
	dampHighA.Prepare( sampleRate );
	dampHighB.Prepare( sampleRate );

	currentDelay = targetDelay = previousDelay = 0.001 * 250.0 * sampleRate;
	Reset();
}

void Delay::Reset()
{
	lineA.Reset();
	lineB.Reset();
	dampLowA.Reset();
	dampLowB.Reset();
	dampHighA.Reset();
	dampHighB.Reset();
	fadePosition = 1.0;
}

void Delay::SetParams( const DelayParams& p )
{
	params = p;
	SetEnabled( p.enabled );

	dampLowA.SetCutoff( p.dampLowHz );
	dampLowB.SetCutoff( p.dampLowHz );
	dampHighA.SetCutoff( p.dampHighHz );
	dampHighB.SetCutoff( p.dampHighHz );

	const double wanted = std::clamp( static_cast< double >( p.timeMs ), 1.0, 2500.0 ) * 0.001 * fs;

	if( std::fabs( wanted - targetDelay ) > 0.5 )
	{
		if( p.timeMode == DelayTimeMode::Crossfade )
		{
			//Start a new fade from wherever the old pointer is now, so a second
			//change part-way through the first does not click.
			previousDelay = currentDelay;
			fadePosition  = 0.0;
			currentDelay  = wanted;
		}
		targetDelay = wanted;
	}

	//NOTE: no Reset() here, and that is deliberate. Changing the time must not
	//drop the tail on the floor -- a real pedal's bucket brigade keeps whatever
	//is in it. The fleet's GPU habit of rebuilding on a parameter change is the
	//opposite of what is correct on this side of the plugin.
}

float Delay::TailEnergy() const
{
	//Both lines in full -- see the note on Reverb::TailEnergy. The last value
	//read out says nothing about whether the line still holds a repeat.
	return static_cast< float >( lineA.Energy() + lineB.Energy() );
}

void Delay::Process( Sample* buffer, int n )
{
	if( Idle() )
		return;

	const float feedback = std::clamp( params.feedback, 0.0f, 1.05f );
	const float mix      = std::clamp( params.mix, 0.0f, 1.0f );

	//Repitch ramps the read pointer's rate over roughly 50 ms. The ghosts sweep
	//to their new position, resampling what is already in the line -- which is
	//BBD behaviour, and the sweep is the point rather than an artefact to hide.
	const double rampRate = 1.0 / ( 0.05 * fs );
	const double fadeRate = 1.0 / ( 0.020 * fs );

	const bool pingPong = params.routing == Routing::PingPong;
	RoutingSplit split{ pingPong ? Routing::Stereo : params.routing };

	for( int i = 0; i < n; ++i )
	{
		const float dryX = buffer[ i ].x;
		const float dryY = buffer[ i ].y;

		float a, b;
		split.In( dryX, dryY, a, b );

		if( params.timeMode == DelayTimeMode::Repitch )
		{
			const double d = targetDelay - currentDelay;
			const double step = std::max( std::fabs( targetDelay ), 1.0 ) * rampRate;
			currentDelay += std::clamp( d, -step, step );
		}
		else if( fadePosition < 1.0 )
		{
			fadePosition = std::min( 1.0, fadePosition + fadeRate );
		}

		auto read = [ & ]( DelayLine& line ) {
			if( params.timeMode == DelayTimeMode::Crossfade && fadePosition < 1.0 )
			{
				//Equal power, so the sum of the two tap levels stays constant
				//through the swap rather than dipping in the middle.
				const double t  = fadePosition;
				const float gOld = static_cast< float >( std::cos( t * 1.5707963267948966 ) );
				const float gNew = static_cast< float >( std::sin( t * 1.5707963267948966 ) );
				return line.Read( previousDelay ) * gOld + line.Read( currentDelay ) * gNew;
			}
			return line.Read( currentDelay );
		};

		float wetA = read( lineA );
		float wetB = read( lineB );

		//Filter in the feedback path, not on the output: filtering the output
		//would dull every repeat equally, and what a real delay does is dull
		//each one a little more than the last, because the signal goes round
		//again.
		float fbA = dampHighA.High( dampLowA.Low( wetA ) );
		float fbB = dampHighB.High( dampLowB.Low( wetB ) );

		//The saturator is what stops feedback above unity reaching infinity.
		//It is not a limiter bolted on -- it is the same thing the electronics
		//do, which is why a runaway analogue delay howls and sits there.
		fbA = std::tanh( fbA * feedback );
		fbB = std::tanh( fbB * feedback );

		if( pingPong )
		{
			//Each axis's output feeds the other's input, so the ghost walks
			//diagonally across the screen, alternating sides.
			lineA.Write( a + fbB );
			lineB.Write( b + fbA );
		}
		else
		{
			lineA.Write( a + fbA );
			lineB.Write( b + fbB );
		}

		const float outA = a + ( wetA - a ) * mix;
		const float outB = split.NeedsBoth() ? b + ( wetB - b ) * mix : b;

		float x, y;
		split.Out( a, b, outA, outB, x, y );

		blend( dryX, dryY, x, y );
		buffer[ i ].x = x;
		buffer[ i ].y = y;
	}
}

} // namespace vectrix
