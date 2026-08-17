#include "signal/Engine.h"

#include <algorithm>

namespace vectrix
{

void Engine::Prepare( Detail wanted )
{
	detail = wanted;
	fs     = sampleRateFor( detail );

	block.assign( kMaxBlock, Sample{} );

	oscillator.Prepare( fs );
	shapes.Prepare( fs );
	wire.Prepare( fs );
	audioFile.Prepare( fs );
	traceSource.Prepare( fs );
	chain.Prepare( fs );
}

void Engine::Reset()
{
	oscillator.Reset();
	shapes.Reset();
	wire.Reset();
	audioFile.Reset();
	traceSource.Reset();
	chain.Reset();
}

Source* Engine::current()
{
	switch( kind )
	{
		case SourceKind::Shape: return &shapes;
		case SourceKind::Wireframe: return &wire;
		case SourceKind::AudioFile: return &audioFile;
		case SourceKind::Trace: return &traceSource;
		case SourceKind::Oscillator:
		default: return &oscillator;
	}
}

void Engine::SetParams( const Resolved& r )
{
	//A change of sample rate rebuilds every buffer in the plugin, so it is the
	//one parameter that genuinely cannot be applied mid-block. Detail is a
	//dropdown rather than a slider partly for this reason.
	if( r.detail != detail )
		Prepare( r.detail );

	kind = r.source;

	oscillator.SetParams( r.oscillator );
	shapes.SetParams( r.shape );
	wire.SetParams( r.wire );
	wire.SetWalker( r.walker );
	audioFile.SetParams( r.audioFile );
	traceSource.SetParams( r.trace );
	traceSource.SetWalker( r.walker );

	chain.SetParams( r.chain );
}

void Engine::SwapContent()
{
	audioFile.ReloadIfDirty();
}

void Engine::Advance( double frameSeconds, double hostSeconds )
{
	audioFile.SetHostSeconds( hostSeconds );

	//Only the selected source is advanced. Advancing the wireframe while the
	//oscillator is playing would burn three octaves of noise every frame for a
	//picture nobody is looking at.
	if( kind == SourceKind::Wireframe )
		wire.Advance( frameSeconds );
}

const Sample* Engine::Render( int n, double frameSeconds )
{
	n = std::clamp( n, 2, kMaxBlock );

	//dt per sample comes from the frame's real duration divided by the samples
	//being made for it, NOT from 1/fs. The two differ whenever the frame rate
	//and the sample count disagree -- a dropped frame, a scrub, a host running
	//at 47.9 fps -- and it is this value, carried in every sample, that keeps
	//the trace's brightness independent of all of that.
	const double dt = frameSeconds / static_cast< double >( n - 1 );

	current()->Render( block.data(), n, dt );
	chain.Process( block.data(), n );

	return block.data();
}

} // namespace vectrix
