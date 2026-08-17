#include "signal/Modulation.h"

#include <algorithm>
#include <cmath>

namespace vectrix
{
namespace
{
/// First and last bin of each band, over the 64 the host delivers.
constexpr int kBandRange[ static_cast< int >( Band::Count ) ][ 2 ] = {
	{ 0, 63 }, //Full Range
	{ 0, 7 },  //Low
	{ 8, 27 }, //Mid
	{ 28, 63 },//High
};
} // namespace

void Modulation::Update( const float* bins, int binCount, double frameSeconds )
{
	//Fast up, slow down. A flash that arrives a frame late reads as broken;
	//one that takes ~150 ms to die away reads as intended.
	const float release = frameSeconds > 0.0
	                          ? 1.0f - static_cast< float >( std::exp( -frameSeconds / 0.15 ) )
	                          : 1.0f;

	const int count = std::min( binCount, kAudioBins );
	for( int i = 0; i < count; ++i )
	{
		//sqrt because bin magnitudes bunch hard against zero: a spectrum used
		//raw responds to the kick drum and to nothing else.
		const float raw = std::sqrt( std::max( 0.0f, bins[ i ] ) );
		if( raw >= level[ i ] )
			level[ i ] = raw;
		else
			level[ i ] += ( raw - level[ i ] ) * release;
	}

	for( int i = 0; i < kLfos; ++i )
	{
		lfoPhase[ i ] += lfoRate[ i ] * frameSeconds;
		lfoPhase[ i ] -= std::floor( lfoPhase[ i ] );
	}
}

void Modulation::UpdateTransport( float bpm, float barPhase, double nowSeconds )
{
	//The host hands over a tempo and a position *within* the current bar, never
	//which bar it is. Recover a continuous bar number without keeping state: the
	//clock estimates how many bars have passed, barPhase gives the exact
	//position inside this one, and the whole number reconciling them is
	//round(estimate - phase). Continuous across the bar line, and exact while
	//the clock estimate stays within half a bar.
	const double tempo      = bpm > 1.0f ? static_cast< double >( bpm ) : 120.0;
	const double barSeconds = 240.0 / tempo;//four beats to the bar
	const double estimate   = nowSeconds / barSeconds;
	const double within     = std::clamp( static_cast< double >( barPhase ), 0.0, 1.0 );

	bars = within + std::round( estimate - within );
}

void Modulation::SetLfo( int index, float rateHz, float depth )
{
	if( index < 0 || index >= kLfos )
		return;
	lfoRate[ index ]  = std::clamp( rateHz, 0.01f, 20.0f );
	lfoDepth[ index ] = std::clamp( depth, 0.0f, 1.0f );
}

void Modulation::SetSlot( int index, const ModSlot& slot )
{
	if( index < 0 || index >= kSlots )
		return;
	slots[ index ] = slot;
}

float Modulation::Level( Band band ) const
{
	const int which = std::clamp( static_cast< int >( band ), 0, static_cast< int >( Band::Count ) - 1 );
	const int first = kBandRange[ which ][ 0 ];
	const int last  = kBandRange[ which ][ 1 ];

	//The mean, not the peak. A peak follows whichever bin happens to be loudest
	//and jumps between them; what the picture wants is the band's whole output
	//at once.
	float sum = 0.0f;
	for( int i = first; i <= last; ++i )
		sum += level[ i ];

	const int count = last - first + 1;
	return count > 0 ? std::clamp( sum / static_cast< float >( count ), 0.0f, 1.0f ) : 0.0f;
}

float Modulation::For( ModTarget target ) const
{
	if( target == ModTarget::None )
		return 0.0f;

	float sum = 0.0f;
	for( const ModSlot& slot : slots )
	{
		if( slot.target != target || slot.amount == 0.0f )
			continue;
		sum += Level( slot.band ) * slot.amount;
	}

	for( int i = 0; i < kLfos; ++i )
	{
		if( lfoDepth[ i ] <= 0.0f )
			continue;
		//The LFOs are wired to whatever the first slot naming this target says,
		//so an LFO with no slot pointed at it does nothing rather than modulating
		//everything at once.
		for( const ModSlot& slot : slots )
		{
			if( slot.target != target )
				continue;
			sum += static_cast< float >( std::sin( 6.283185307179586 * lfoPhase[ i ] ) )
			       * lfoDepth[ i ] * slot.amount;
			break;
		}
	}

	return sum;
}

} // namespace vectrix
