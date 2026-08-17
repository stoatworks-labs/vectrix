#include "signal/fx/Chain.h"

#include <algorithm>
#include <cmath>

namespace vectrix
{
namespace
{
constexpr double kTwoPi = 6.283185307179586476925286766559;

/**
	A normalised soft clip.

	The Pade rational `x(27+x^2)/(27+9x^2)` approximates tanh to about 2% at the
	knee for a fifth of the cost -- but it is **non-monotonic beyond about 5.2**,
	and a non-monotonic saturator folds. Since there is a real wavefolder eight
	lines below, an accidental one here would be indistinguishable from it and
	impossible to turn off. Hence the clamp first: it costs one instruction and
	it is the difference between a saturator and a mystery.
*/
inline float softClip( float x )
{
	x              = std::clamp( x, -3.0f, 3.0f );
	const float x2 = x * x;
	return x * ( 27.0f + x2 ) / ( 27.0f + 9.0f * x2 );
}

/// The Serge reflecting wavefolder. Everything above 1 is reflected back down,
/// repeatedly -- which is what turns a circle into a rosette.
inline float wavefold( float x, float folds )
{
	float y = x * folds;
	y       = y - 4.0f * std::round( y * 0.25f );//wrap into [-2, 2]
	if( y > 1.0f )
		y = 2.0f - y;
	else if( y < -1.0f )
		y = -2.0f - y;
	return y;
}
} // namespace

//---------------------------------------------------------------------------
// Rectifier
//---------------------------------------------------------------------------

void Rectifier::Prepare( double sampleRate )
{
	fs = sampleRate;
	bypass.Prepare( sampleRate );
}

void Rectifier::Reset()
{
}

void Rectifier::SetParams( const RectifierParams& p )
{
	params = p;
	SetEnabled( p.enabled );
}

void Rectifier::Process( Sample* buffer, int n )
{
	if( Idle() )
		return;

	RoutingSplit split{ params.routing };
	const float bias = params.bias;

	for( int i = 0; i < n; ++i )
	{
		const float dryX = buffer[ i ].x;
		const float dryY = buffer[ i ].y;

		float a, b;
		split.In( dryX, dryY, a, b );

		//Bias in, bias out: a movable fold point rather than a fixed one at
		//zero. Half-wave on X with the default bias folds the left half of the
		//figure onto x=0 and the beam then *sits* there for half of every cycle,
		//so a bright vertical bar appears. That bar is dwell, not a drawn line.
		auto rect = [ & ]( float v ) {
			const float shifted = v + bias;
			const float folded  = params.fullWave ? std::fabs( shifted ) : std::max( shifted, 0.0f );
			return folded - bias;
		};

		const float outA = rect( a );
		const float outB = split.NeedsBoth() ? rect( b ) : b;

		float x, y;
		split.Out( a, b, outA, outB, x, y );

		blend( dryX, dryY, x, y );
		buffer[ i ].x = x;
		buffer[ i ].y = y;
	}
}

//---------------------------------------------------------------------------
// Slew limiter
//---------------------------------------------------------------------------

void Slew::Prepare( double sampleRate )
{
	fs = sampleRate;
	bypass.Prepare( sampleRate );
	Reset();
}

void Slew::Reset()
{
	currentA = currentB = 0.0f;
}

void Slew::SetParams( const SlewParams& p )
{
	params = p;
	SetEnabled( p.enabled );
}

void Slew::Process( Sample* buffer, int n )
{
	if( Idle() )
		return;

	RoutingSplit split{ params.routing };

	const float rise = std::max( params.rise, 0.5f ) / static_cast< float >( fs );
	const float fall = params.link ? rise : std::max( params.fall, 0.5f ) / static_cast< float >( fs );

	for( int i = 0; i < n; ++i )
	{
		const float dryX = buffer[ i ].x;
		const float dryY = buffer[ i ].y;

		float a, b;
		split.In( dryX, dryY, a, b );

		auto limit = [ & ]( float target, float& current ) {
			const float d    = target - current;
			const float step = d > 0.0f ? rise : fall;
			current += std::clamp( d, -step, step );
			return current;
		};

		const float outA = limit( a, currentA );
		const float outB = split.NeedsBoth() ? limit( b, currentB ) : b;

		float x, y;
		split.Out( a, b, outA, outB, x, y );

		blend( dryX, dryY, x, y );
		buffer[ i ].x = x;
		buffer[ i ].y = y;
	}
}

//---------------------------------------------------------------------------
// Drive / wavefolder
//---------------------------------------------------------------------------

void Drive::Prepare( double sampleRate )
{
	fs = sampleRate;
	bypass.Prepare( sampleRate );
	driveGain.Prepare( sampleRate );
	driveGain.Snap( 1.0f );
	Reset();
}

void Drive::Reset()
{
	for( int i = 0; i < 2; ++i )
	{
		upA[ i ].Reset();
		upB[ i ].Reset();
		downA[ i ].Reset();
		downB[ i ].Reset();
	}
}

void Drive::SetParams( const DriveParams& p )
{
	params = p;
	SetEnabled( p.enabled );
	driveGain.SetTarget( std::pow( 10.0f, std::clamp( p.driveDb, 0.0f, 40.0f ) * 0.05f ) );
}

float Drive::shape( float x ) const
{
	const float clipped = softClip( x );
	if( params.fold <= 0.0f )
		return clipped;

	const float folded = wavefold( clipped, static_cast< float >( std::clamp( params.folds, 1, 8 ) ) );
	return clipped + ( folded - clipped ) * std::clamp( params.fold, 0.0f, 1.0f );
}

void Drive::Process( Sample* buffer, int n )
{
	if( Idle() )
		return;

	RoutingSplit split{ params.routing };

	for( int i = 0; i < n; ++i )
	{
		const float dryX = buffer[ i ].x;
		const float dryY = buffer[ i ].y;
		const float g    = driveGain.Next();

		float a, b;
		split.In( dryX, dryY, a, b );
		a *= g;
		b *= g;

		float outA, outB;

		if( params.oversample )
		{
			//4x, as two cascaded 2x half-band stages.
			//
			//Each 2x step zero-stuffs -- one real sample then one zero -- which
			//halves the average level, so the real sample is doubled going in to
			//put it back. Decimation on the way down keeps the first of each
			//pair, but both must still be *pushed through* the filter or its
			//delay line desynchronises from the input and the whole thing turns
			//into a comb.
			auto run = [ & ]( float in, HalfBand* up, HalfBand* down ) {
				float stage1[ 2 ];
				stage1[ 0 ] = up[ 0 ].Process( in * 2.0f );
				stage1[ 1 ] = up[ 0 ].Process( 0.0f );

				float shaped[ 4 ];
				for( int j = 0; j < 2; ++j )
				{
					shaped[ j * 2 + 0 ] = shape( up[ 1 ].Process( stage1[ j ] * 2.0f ) );
					shaped[ j * 2 + 1 ] = shape( up[ 1 ].Process( 0.0f ) );
				}

				float stage2[ 2 ];
				for( int j = 0; j < 2; ++j )
				{
					stage2[ j ] = down[ 1 ].Process( shaped[ j * 2 + 0 ] );
					( void )down[ 1 ].Process( shaped[ j * 2 + 1 ] );
				}

				const float out = down[ 0 ].Process( stage2[ 0 ] );
				( void )down[ 0 ].Process( stage2[ 1 ] );
				return out;
			};
			outA = run( a, upA, downA );
			outB = split.NeedsBoth() ? run( b, upB, downB ) : b;
		}
		else
		{
			outA = shape( a );
			outB = split.NeedsBoth() ? shape( b ) : b;
		}

		float x, y;
		split.Out( a, b, outA, outB, x, y );

		blend( dryX, dryY, x, y );
		buffer[ i ].x = x;
		buffer[ i ].y = y;
	}
}

//---------------------------------------------------------------------------
// Ring modulator
//---------------------------------------------------------------------------

void RingMod::Prepare( double sampleRate )
{
	fs = sampleRate;
	bypass.Prepare( sampleRate );
	Reset();
}

void RingMod::Reset()
{
	phase = 0.0;
}

void RingMod::SetParams( const RingModParams& p, float sourceFreq )
{
	params = p;
	SetEnabled( p.enabled );

	//Ratio Lock ties the carrier to the source's own frequency, so the
	//modulation stands still relative to the figure instead of crawling through
	//it. Unlocked, the beat between the two is the interesting part.
	const double carrier = p.ratioLock ? static_cast< double >( sourceFreq ) * std::max( 1.0f, std::round( p.freq / std::max( sourceFreq, 0.01f ) ) )
	                                   : static_cast< double >( p.freq );
	inc = std::clamp( carrier, 0.1, 2000.0 ) / fs;
}

void RingMod::Process( Sample* buffer, int n )
{
	if( Idle() )
		return;

	const float depth = std::clamp( params.depth, 0.0f, 1.0f );

	for( int i = 0; i < n; ++i )
	{
		const float dryX = buffer[ i ].x;
		const float dryY = buffer[ i ].y;

		float carrier;
		switch( params.wave )
		{
			case 1: carrier = 1.0f - 4.0f * std::fabs( static_cast< float >( phase ) - 0.5f ); break;
			case 2: carrier = phase < 0.5 ? 1.0f : -1.0f; break;
			default: carrier = static_cast< float >( std::sin( kTwoPi * phase ) ); break;
		}

		float x = dryX;
		float y = dryY;

		if( params.routing == Routing::Cross )
		{
			//Each axis modulated by the *other*, normalised. Free, and the best
			//of the routings: it produces a genuine warp of the figure that no
			//affine transform reaches.
			//
			//Note the normalisation. The naive x' = x*y, y' = y*x is the same
			//product on both axes, which collapses the whole figure onto the
			//diagonal -- correct arithmetic, useless picture.
			const float nx = std::clamp( dryX, -1.0f, 1.0f );
			const float ny = std::clamp( dryY, -1.0f, 1.0f );
			x              = dryX * ( 1.0f - depth + depth * ny );
			y              = dryY * ( 1.0f - depth + depth * nx );
		}
		else
		{
			RoutingSplit split{ params.routing };
			float a, b;
			split.In( dryX, dryY, a, b );
			const float m    = 1.0f - depth + depth * carrier;
			const float outA = a * m;
			const float outB = split.NeedsBoth() ? b * m : b;
			split.Out( a, b, outA, outB, x, y );
		}

		blend( dryX, dryY, x, y );
		buffer[ i ].x = x;
		buffer[ i ].y = y;

		phase += inc;
		if( phase >= 1.0 )
			phase -= std::floor( phase );
	}
}

//---------------------------------------------------------------------------
// Bitcrusher / sample-rate reducer
//---------------------------------------------------------------------------

void Bitcrush::Prepare( double sampleRate )
{
	fs = sampleRate;
	bypass.Prepare( sampleRate );
	Reset();
}

void Bitcrush::Reset()
{
	holdPhase = 1.0;
	heldA = heldB = 0.0f;
}

void Bitcrush::SetParams( const BitcrushParams& p )
{
	params = p;
	SetEnabled( p.enabled );
}

void Bitcrush::Process( Sample* buffer, int n )
{
	if( Idle() )
		return;

	RoutingSplit split{ params.routing };

	const int bits    = std::clamp( params.bits, 1, 16 );
	const float steps = static_cast< float >( 1 << ( bits - 1 ) );
	//Fractional, so the hold period is not forced to a whole number of samples
	//-- otherwise the reduction ratio jumps in visible steps as the knob moves.
	const double holdInc = params.rateHz > 0.0f
	                           ? std::clamp( static_cast< double >( params.rateHz ), 100.0, fs ) / fs
	                           : 1.0;

	for( int i = 0; i < n; ++i )
	{
		const float dryX = buffer[ i ].x;
		const float dryY = buffer[ i ].y;

		float a, b;
		split.In( dryX, dryY, a, b );

		holdPhase += holdInc;
		if( holdPhase >= 1.0 )
		{
			holdPhase -= std::floor( holdPhase );
			heldA = a;
			heldB = b;
		}

		//Quantising the beam's *position* onto a grid, which turns a circle into
		//a staircase of exactly 2^bits steps per axis. The connecting lines
		//between held points come from the slew limiter and the amplifier
		//bandwidth, not from here -- which is why the "digital scope" look is a
		//consequence of two blocks rather than a mode of one.
		auto quantise = [ & ]( float v ) {
			return bits >= 16 ? v : std::round( v * steps ) / steps;
		};

		const float outA = quantise( heldA );
		const float outB = split.NeedsBoth() ? quantise( heldB ) : b;

		float x, y;
		split.Out( a, b, outA, outB, x, y );

		blend( dryX, dryY, x, y );
		buffer[ i ].x = x;
		buffer[ i ].y = y;
	}
}

} // namespace vectrix
