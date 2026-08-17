#include "signal/fx/Chain.h"

#include <algorithm>
#include <cmath>

namespace vectrix
{

void Output::Prepare( double sampleRate )
{
	fs = sampleRate;
	bypass.Prepare( sampleRate );
	bypass.Snap( true );//always on -- there is always an amplifier

	dcX.Prepare( sampleRate );
	dcY.Prepare( sampleRate );
	bandX.Prepare( sampleRate );
	bandY.Prepare( sampleRate );

	gain.Prepare( sampleRate );
	offsetX.Prepare( sampleRate );
	offsetY.Prepare( sampleRate );
	gain.Snap( 1.0f );
	offsetX.Snap( 0.0f );
	offsetY.Snap( 0.0f );

	Reset();
	SetParams( params );
}

void Output::Reset()
{
	dcX.Reset();
	dcY.Reset();
	bandX.Reset();
	bandY.Reset();
	slewX = slewY = 0.0f;
}

void Output::SetParams( const OutputParams& p )
{
	params = p;
	gain.SetTarget( p.gain );
	offsetX.SetTarget( p.offsetX );
	offsetY.SetTarget( p.offsetY );

	//The X amplifier in a real scope is faster than the Y one -- the horizontal
	//channel drives the sweep and is built for it. Defaulting them differently
	//is not a preference, it is what the hardware is like, and it is why a fast
	//figure leans slightly.
	bandX.Set( std::clamp( static_cast< double >( p.bandwidthX ), 1000.0, fs * 0.49 ), p.resonance );
	bandY.Set( std::clamp( static_cast< double >( p.bandwidthY ), 1000.0, fs * 0.49 ), p.resonance );
}

void Output::Process( Sample* buffer, int n )
{
	const float slewStep = std::max( params.ampSlew, 1.0f ) / static_cast< float >( fs );

	const float rot  = params.rotation * 6.283185307179586f;
	const float cosR = std::cos( rot );
	const float sinR = std::sin( rot );

	for( int i = 0; i < n; ++i )
	{
		float x = buffer[ i ].x;
		float y = buffer[ i ].y;

		//DC first, and this is the reason the amplifier is genuinely last in the
		//chain. The rectifier and the wavefolder both manufacture DC as a matter
		//of course, and DC on a deflection axis is not a tone -- it is the whole
		//figure sitting permanently off the centre of the screen, which the
		//operator then tries to correct with Offset and cannot get back.
		if( params.dcBlock )
		{
			x = dcX.Process( x );
			y = dcY.Process( y );
		}

		//Geometry: skew then rotate. Both are properties of how the yoke is
		//mounted and wound, which is why they live in the amplifier and not in
		//the source.
		x += y * params.skew;
		const float rx = x * cosR - y * sinR;
		const float ry = x * sinR + y * cosR;
		x              = rx;
		y              = ry;

		const float g = gain.Next();
		x             = x * g + offsetX.Next();
		y             = y * g + offsetY.Next();

		//The amplifier cannot move the beam infinitely fast. This is what
		//rounds the corner of a square at the very last moment, and it is also
		//the plugin's backstop against a pathological segment: a slew-limited
		//deflection cannot produce a screen-crossing jump in one sample.
		x = slewX + std::clamp( x - slewX, -slewStep, slewStep );
		y = slewY + std::clamp( y - slewY, -slewStep, slewStep );
		slewX = x;
		slewY = y;

		//And it has finite bandwidth, which softens what the slew limit left.
		x = bandX.Low( x );
		y = bandY.Low( y );

		buffer[ i ].x = x;
		buffer[ i ].y = y;
	}
}

} // namespace vectrix
