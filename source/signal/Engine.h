#pragma once

#include "Controls.h"
#include "signal/Clock.h"
#include "signal/Signal.h"
#include "signal/fx/Chain.h"
#include "signal/sources/AudioFile.h"
#include "signal/sources/Oscillator.h"
#include "signal/sources/Shapes.h"
#include "signal/sources/Trace.h"
#include "signal/sources/Wire.h"

#include <vector>

namespace vectrix
{
/**
	Owns the sources and the pedalboard, and turns one video frame into a block
	of beam positions.

	The whole of the plugin's signal path is behind this class and none of it
	touches GL, with the single exception of the trace source's edge pass -- so
	everything here is testable offline and, more usefully, is *deterministic*:
	`Render` is handed a sample count and a per-sample duration and reads no
	clock of its own.
*/
class Engine
{
public:
	void Prepare( Detail detail );
	void Reset();

	/// Apply this frame's resolved parameters. Cheap; call every frame.
	void SetParams( const Resolved& resolved );

	/// Content may only change at a block boundary -- a decoded file swapped in
	/// half way through a block tears the path, and the renderer would draw the
	/// tear as a line straight across the picture.
	void SwapContent();

	/// Advance anything that moves per frame rather than per sample: the
	/// wireframe's rotation, the audio file's transport.
	void Advance( double frameSeconds, double hostSeconds );

	/// Synthesise `n` samples and run them through the chain.
	const Sample* Render( int n, double frameSeconds );

	double SampleRate() const
	{
		return fs;
	}
	Detail CurrentDetail() const
	{
		return detail;
	}

	AudioFile& File()
	{
		return audioFile;
	}
	TraceSource& Trace()
	{
		return traceSource;
	}
	Chain& Pedals()
	{
		return chain;
	}

private:
	Source* current();

	Detail detail = Detail::Normal;
	double fs     = 96000.0;

	SourceKind kind = SourceKind::Oscillator;

	Oscillator oscillator;
	Shapes shapes;
	WireSource wire;
	AudioFile audioFile;
	TraceSource traceSource;

	Chain chain;
	std::vector< Sample > block;
};

} // namespace vectrix
