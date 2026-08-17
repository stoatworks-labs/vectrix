#include "signal/fx/Chain.h"

namespace vectrix
{

void Chain::Prepare( double sampleRate )
{
	vca.Prepare( sampleRate );
	gate.Prepare( sampleRate );
	compressor.Prepare( sampleRate );
	rectifier.Prepare( sampleRate );
	slew.Prepare( sampleRate );
	drive.Prepare( sampleRate );
	ringMod.Prepare( sampleRate );
	bitcrush.Prepare( sampleRate );
	phaser.Prepare( sampleRate );
	flanger.Prepare( sampleRate );
	chorus.Prepare( sampleRate );
	delay.Prepare( sampleRate );
	reverb.Prepare( sampleRate );
	output.Prepare( sampleRate );
}

void Chain::Reset()
{
	vca.Reset();
	gate.Reset();
	compressor.Reset();
	rectifier.Reset();
	slew.Reset();
	drive.Reset();
	ringMod.Reset();
	bitcrush.Reset();
	phaser.Reset();
	flanger.Reset();
	chorus.Reset();
	delay.Reset();
	reverb.Reset();
	output.Reset();
}

void Chain::SetParams( const ChainParams& p )
{
	vca.SetParams( p.vca );
	gate.SetParams( p.gate );
	compressor.SetParams( p.compressor );
	rectifier.SetParams( p.rectifier );
	slew.SetParams( p.slew );
	drive.SetParams( p.drive );
	ringMod.SetParams( p.ringMod, p.sourceFreq );
	bitcrush.SetParams( p.bitcrush );
	phaser.SetParams( p.phaser );
	flanger.SetParams( p.flanger );
	chorus.SetParams( p.chorus );
	delay.SetParams( p.delay );
	reverb.SetParams( p.reverb );
	output.SetParams( p.output );
}

void Chain::Process( Sample* buffer, int n )
{
	//The order is the whole design decision and it is documented on the class.
	//Each block is a no-op when its crossfade has reached zero, so an unused
	//pedalboard costs fourteen branch predictions.
	vca.Process( buffer, n );
	gate.Process( buffer, n );
	compressor.Process( buffer, n );
	rectifier.Process( buffer, n );
	slew.Process( buffer, n );
	drive.Process( buffer, n );
	ringMod.Process( buffer, n );
	bitcrush.Process( buffer, n );
	phaser.Process( buffer, n );
	flanger.Process( buffer, n );
	chorus.Process( buffer, n );
	delay.Process( buffer, n );
	reverb.Process( buffer, n );
	output.Process( buffer, n );
}

} // namespace vectrix
