#include "signal/fx/Reverb.h"

#include <algorithm>
#include <cmath>

namespace vectrix
{
namespace
{
constexpr double kTwoPi = 6.283185307179586476925286766559;

/// Slow, mutually unrelated modulation rates -- one per line. Related rates
/// would put every line's wobble in step and the shimmer would turn into a
/// single slow warble of the whole halo.
constexpr double kModRate[ 8 ] = { 0.11, 0.17, 0.23, 0.29, 0.13, 0.19, 0.31, 0.37 };
} // namespace

void Reverb::Householder( std::array< float, 8 >& v )
{
	//H = I - (2/N)J. Nine operations for an exactly orthogonal 8x8 mix.
	float sum = 0.0f;
	for( int i = 0; i < 8; ++i )
		sum += v[ i ];
	const float s = sum * ( 2.0f / 8.0f );
	for( int i = 0; i < 8; ++i )
		v[ i ] = v[ i ] - s;
}

void Reverb::Prepare( double sampleRate )
{
	fs = sampleRate;
	bypass.Prepare( sampleRate );

	const double scale = fs / 48000.0;

	for( int i = 0; i < kLines; ++i )
	{
		//2^15 covers the longest line at 192 kHz and twice the Size range.
		lines[ i ].Prepare( 1u << 15 );
		damping[ i ].Prepare( sampleRate );
		modPhase[ i ] = static_cast< double >( i ) / kLines;
		modInc[ i ]   = kModRate[ i ] / fs;
	}

	for( int i = 0; i < kDiffusers; ++i )
	{
		const std::size_t length = static_cast< std::size_t >( std::max( 1.0, kDiffuserLength[ i ] * scale ) );
		diffuserA[ i ].Prepare( length );
		diffuserB[ i ].Prepare( length );
	}

	preDelayA.Prepare( 1u << 16 );
	preDelayB.Prepare( 1u << 16 );

	Reset();
	SetParams( params );
}

void Reverb::Reset()
{
	for( int i = 0; i < kLines; ++i )
	{
		lines[ i ].Reset();
		damping[ i ].Reset();
	}
	for( int i = 0; i < kDiffusers; ++i )
	{
		diffuserA[ i ].Reset();
		diffuserB[ i ].Reset();
	}
	preDelayA.Reset();
	preDelayB.Reset();
	state.fill( 0.0f );
}

void Reverb::SetParams( const ReverbParams& p )
{
	params = p;
	SetEnabled( p.enabled );

	const double scale = ( fs / 48000.0 ) * std::clamp( static_cast< double >( p.size ), 0.25, 2.0 );
	const double rt60  = std::clamp( static_cast< double >( p.decaySec ), 0.1, 20.0 );

	for( int i = 0; i < kLines; ++i )
	{
		lineDelay[ i ] = std::max( 4.0, kBaseLength[ i ] * scale );

		//The standard FDN decay law: g = 10^(-3L/(fs*RT60)). Per-line, so every
		//line decays at the same *rate* despite having different lengths. Equal
		//gains instead makes the short lines die last and the tail flutters.
		lineGain[ i ] = static_cast< float >(
			std::pow( 10.0, -3.0 * lineDelay[ i ] / ( fs * rt60 ) ) );

		//Damping is what makes the halo soften as it decays rather than staying
		//equally sharp all the way down.
		const double cutoff = 500.0 + ( 1.0 - std::clamp( p.damping, 0.0f, 1.0f ) ) * 19500.0;
		damping[ i ].SetCutoff( cutoff );
	}

	preDelaySamples = std::clamp( static_cast< double >( p.preDelayMs ), 0.0, 200.0 ) * 0.001 * fs;

	//NOTE: no Reset(). See Delay::SetParams -- the tank keeps what is in it.
}

float Reverb::TailEnergy() const
{
	//The whole tank, not the last set of taps.
	//
	//The obvious implementation -- sum the taps this block just read -- is wrong
	//in precisely the case this accessor exists for. With a pre-delay and lines
	//thousands of samples long, a tank can be full of signal that has been
	//*written* while the read pointers are still walking through the silence
	//that preceded the strike. The taps are zero, the tank is not empty, and a
	//test asserting "a parameter change preserved the tail" would pass or fail
	//on how long the block happened to be.
	double sum = 0.0;
	for( int i = 0; i < kLines; ++i )
		sum += lines[ i ].Energy();
	return static_cast< float >( sum );
}

void Reverb::Process( Sample* buffer, int n )
{
	if( Idle() )
		return;

	const float mix       = std::clamp( params.mix, 0.0f, 1.0f );
	const float diffusion = std::clamp( params.diffusion, 0.0f, 0.9f );
	const float modDepth  = std::clamp( params.modulation, 0.0f, 1.0f ) * 2.0f;

	for( int i = 0; i < n; ++i )
	{
		const float dryX = buffer[ i ].x;
		const float dryY = buffer[ i ].y;

		preDelayA.Write( dryX );
		preDelayB.Write( dryY );
		float inA = preDelaySamples > 1.0 ? preDelayA.Read( preDelaySamples ) : dryX;
		float inB = preDelaySamples > 1.0 ? preDelayB.Read( preDelaySamples ) : dryY;

		//Input diffusion. Without these the FDN is grainy on impulsive input --
		//and a scope figure is extremely impulsive at every corner, so the halo
		//would arrive as a burst of separate ticks rather than a wash. Four
		//allpasses, cheap, and load-bearing.
		for( int d = 0; d < kDiffusers; ++d )
		{
			inA = diffuserA[ d ].Process( inA, diffusion );
			inB = diffuserB[ d ].Process( inB, diffusion );
		}

		//Read every line, with a slow wobble on the read position. The wobble
		//breaks the metallic ringing a fixed-length FDN develops; on the picture
		//the halo shimmers instead of standing still.
		std::array< float, kLines > taps{};
		for( int l = 0; l < kLines; ++l )
		{
			const double wobble = modDepth * std::sin( kTwoPi * modPhase[ l ] );
			taps[ l ]           = lines[ l ].Read( lineDelay[ l ] + wobble );
			modPhase[ l ] += modInc[ l ];
			if( modPhase[ l ] >= 1.0 )
				modPhase[ l ] -= std::floor( modPhase[ l ] );
		}

		state = taps;
		Householder( state );

		for( int l = 0; l < kLines; ++l )
		{
			float v = damping[ l ].Low( state[ l ] * lineGain[ l ] );

			//X feeds the first half, Y the second. The matrix does the rest of
			//the cross-coupling.
			if( l < 4 )
				v += inA * 0.5f;
			else
				v += inB * 0.5f;

			lines[ l ].Write( v );
		}

		//X reads the even lines, Y the odd ones -- so the two outputs are taken
		//from overlapping but different subsets and are naturally de-correlated.
		const float wetA = ( taps[ 0 ] + taps[ 2 ] + taps[ 4 ] + taps[ 6 ] ) * 0.5f;
		const float wetB = ( taps[ 1 ] + taps[ 3 ] + taps[ 5 ] + taps[ 7 ] ) * 0.5f;

		float x = dryX + ( wetA - dryX ) * mix;
		float y = dryY + ( wetB - dryY ) * mix;

		blend( dryX, dryY, x, y );
		buffer[ i ].x = x;
		buffer[ i ].y = y;
	}
}

} // namespace vectrix
