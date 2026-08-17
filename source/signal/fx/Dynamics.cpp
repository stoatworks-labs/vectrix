#include "signal/fx/Chain.h"

#include <algorithm>
#include <cmath>

namespace vectrix
{
namespace
{
inline float dbToLinear( float db )
{
	return std::pow( 10.0f, db * 0.05f );
}

inline float linearToDb( float linear )
{
	return 20.0f * std::log10( std::max( linear, 1.0e-9f ) );
}

/// The standard one-pole time constant for an envelope follower.
inline float envCoeff( double milliseconds, double fs )
{
	const double seconds = std::max( milliseconds, 0.01 ) * 0.001;
	return static_cast< float >( std::exp( -1.0 / ( fs * seconds ) ) );
}
} // namespace

//---------------------------------------------------------------------------
// VCA
//---------------------------------------------------------------------------

void Vca::Prepare( double sampleRate )
{
	fs = sampleRate;
	bypass.Prepare( sampleRate );
	level.Prepare( sampleRate );
	level.Snap( 1.0f );
}

void Vca::Reset()
{
	level.Snap( params.level );
}

void Vca::SetParams( const VcaParams& p )
{
	params = p;
	SetEnabled( p.enabled );
	level.SetTarget( p.level );
}

void Vca::Process( Sample* buffer, int n )
{
	if( Idle() )
		return;

	RoutingSplit split{ params.routing };

	for( int i = 0; i < n; ++i )
	{
		const float dryX = buffer[ i ].x;
		const float dryY = buffer[ i ].y;
		const float g    = level.Next();

		float a, b;
		split.In( dryX, dryY, a, b );
		float x, y;
		split.Out( a, b, a * g, b * g, x, y );

		blend( dryX, dryY, x, y );
		buffer[ i ].x = x;
		buffer[ i ].y = y;
	}
}

//---------------------------------------------------------------------------
// Gate
//---------------------------------------------------------------------------

void Gate::Prepare( double sampleRate )
{
	fs = sampleRate;
	bypass.Prepare( sampleRate );
	Reset();
	SetParams( params );
}

void Gate::Reset()
{
	state    = State::Closed;
	envelope = 0.0f;
	holdLeft = 0.0;
}

void Gate::SetParams( const GateParams& p )
{
	params = p;
	SetEnabled( p.enabled );

	openLevel = dbToLinear( p.thresholdDb );
	//Six dB of hysteresis. Without it the gate reopens and recloses on every
	//zero crossing of a signal sitting on the threshold, and the beam strobes at
	//the signal frequency -- which reads as a broken plugin rather than as a
	//gate set badly.
	closeLevel = dbToLinear( p.thresholdDb - 6.0f );

	attackCoeff  = envCoeff( p.attackMs, fs );
	releaseCoeff = envCoeff( p.releaseMs, fs );
}

void Gate::Process( Sample* buffer, int n )
{
	if( Idle() )
		return;

	const double holdSeconds = std::max( 0.0f, params.holdMs ) * 0.001;

	for( int i = 0; i < n; ++i )
	{
		const float dryX = buffer[ i ].x;
		const float dryY = buffer[ i ].y;
		const float dryZ = buffer[ i ].z;

		//Radial: how far the beam is from the centre of the screen.
		const float detector = std::sqrt( dryX * dryX + dryY * dryY );

		switch( state )
		{
			case State::Closed:
				if( detector >= openLevel )
					state = State::Attack;
				break;
			case State::Attack:
				if( envelope >= 0.999f )
				{
					state    = State::Open;
					envelope = 1.0f;
				}
				break;
			case State::Open:
				if( detector < closeLevel )
				{
					state    = State::Hold;
					holdLeft = holdSeconds;
				}
				break;
			case State::Hold:
				if( detector >= openLevel )
				{
					state = State::Open;
				}
				else
				{
					holdLeft -= 1.0 / fs;
					if( holdLeft <= 0.0 )
						state = State::Release;
				}
				break;
			case State::Release:
				if( detector >= openLevel )
					state = State::Attack;
				else if( envelope <= 0.0001f )
				{
					state    = State::Closed;
					envelope = 0.0f;
				}
				break;
		}

		const bool opening = state == State::Attack || state == State::Open || state == State::Hold;
		const float coeff  = opening ? attackCoeff : releaseCoeff;
		const float goal   = opening ? 1.0f : 0.0f;
		envelope           = goal + ( envelope - goal ) * coeff;

		float x = dryX;
		float y = dryY;
		float z = dryZ;

		if( params.muteMode )
		{
			//Mute: the deflection itself is attenuated, so the beam is pulled
			//back to the centre of the screen as it closes.
			x *= envelope;
			y *= envelope;
		}
		//Blank, the default, leaves the deflection alone and cuts the gun -- which
		//is what a grid voltage actually does. The beam stays where the signal
		//says it is, so when the gate reopens the trace resumes in the right
		//place instead of flying in from the middle.
		z *= envelope;

		blend( dryX, dryY, x, y );
		//z is crossfaded by hand: `blend` only covers the deflection pair.
		const float w = bypass.Value();
		buffer[ i ].x = x;
		buffer[ i ].y = y;
		buffer[ i ].z = dryZ + ( z - dryZ ) * w;
	}
}

//---------------------------------------------------------------------------
// Compressor
//---------------------------------------------------------------------------

void Compressor::Prepare( double sampleRate )
{
	fs = sampleRate;
	bypass.Prepare( sampleRate );
	Reset();
	SetParams( params );
}

void Compressor::Reset()
{
	envelopeDb = 0.0f;
}

float Compressor::computeGainDb( float inputDb ) const
{
	const float over  = inputDb - params.thresholdDb;
	const float knee  = std::max( params.kneeDb, 0.0f );
	const float slope = 1.0f / std::max( params.ratio, 1.0f ) - 1.0f;

	if( knee <= 0.0f )
		return over <= 0.0f ? 0.0f : over * slope;

	if( over <= -knee * 0.5f )
		return 0.0f;
	if( over >= knee * 0.5f )
		return over * slope;

	//Soft knee: a quadratic that meets the two straight sections with matching
	//value and slope at both ends.
	const float t = over + knee * 0.5f;
	return slope * t * t / ( 2.0f * knee );
}

void Compressor::SetParams( const CompressorParams& p )
{
	params = p;
	SetEnabled( p.enabled );

	attackCoeff  = envCoeff( p.attackMs, fs );
	releaseCoeff = envCoeff( p.releaseMs, fs );

	//Auto makeup is defined as undoing the curve at 0 dBFS, computed from the
	//same function the gain computer uses rather than from a second copy of the
	//formula -- which is how the two end up disagreeing after someone edits the
	//knee.
	makeupDb = p.autoMakeup ? -computeGainDb( 0.0f ) : p.makeupDb;

	ceilingLinear = dbToLinear( p.ceilingDb );
}

void Compressor::Process( Sample* buffer, int n )
{
	if( Idle() )
		return;

	RoutingSplit split{ params.routing };

	for( int i = 0; i < n; ++i )
	{
		const float dryX = buffer[ i ].x;
		const float dryY = buffer[ i ].y;

		float a, b;
		split.In( dryX, dryY, a, b );

		//Linked detection by default: the larger of the two axes drives one
		//gain applied to both, so the figure changes size without changing
		//shape. Unlinked compression *shears* the figure as it moves -- offered
		//through the routing control, but not the default, because it is
		//genuinely unpleasant.
		float detector;
		switch( params.routing )
		{
			case Routing::XOnly: detector = std::fabs( a ); break;
			case Routing::YOnly: detector = std::fabs( b ); break;
			default: detector = std::max( std::fabs( a ), std::fabs( b ) ); break;
		}

		const float inputDb  = linearToDb( detector );
		const float targetDb = computeGainDb( inputDb );

		//Branching smoother: attack when the gain is being pulled further down,
		//release when it is coming back.
		const float coeff = targetDb < envelopeDb ? attackCoeff : releaseCoeff;
		envelopeDb        = targetDb + ( envelopeDb - targetDb ) * coeff;

		float gain = dbToLinear( envelopeDb + makeupDb );

		float outA = a;
		float outB = b;

		if( params.railMode )
		{
			//Rail: the amplifier runs out of headroom and the figure flattens
			//against the edge of the screen. This is what a real deflection amp
			//does when you overdrive it.
			outA = std::clamp( a * gain, -ceilingLinear, ceilingLinear );
			outB = std::clamp( b * gain, -ceilingLinear, ceilingLinear );
		}
		else
		{
			//Gain: the limiter pulls the gain down instead, so the figure
			//shrinks rather than deforming.
			const float peak = std::max( std::fabs( a ), std::fabs( b ) ) * gain;
			if( peak > ceilingLinear )
				gain *= ceilingLinear / peak;
			outA = a * gain;
			outB = b * gain;
		}

		float x, y;
		split.Out( a, b, outA, outB, x, y );

		blend( dryX, dryY, x, y );
		buffer[ i ].x = x;
		buffer[ i ].y = y;
	}
}

} // namespace vectrix
