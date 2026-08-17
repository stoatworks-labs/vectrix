#pragma once

#include "signal/Signal.h"

namespace vectrix
{
/// Which generator is driving the beam.
enum class SourceKind
{
	Oscillator,
	Shape,
	Wireframe,
	AudioFile,
	Trace, ///< Effect build only. Declared in both so parameter ids do not shift.
	Count
};

/**
	A generator of beam positions.

	**Sources are geometry; brightness is physics.** A source decides where the
	beam goes and whether the gun is on. It never modulates `z` to make something
	look bright -- corner brightening comes from the slew limiter and the
	deflection amplifier's bandwidth downstream, which is where a real machine
	puts it. The only legitimate use of `z` in a source is genuine blanking:
	retrace, the jump between two strokes, the hidden part of a ridgeline.

	`Render` is handed `dtPerSample` and never reads a clock. That one constraint
	is what makes the offline harness and the host bit-identical, and it is why
	every test in `vxtest` can assert an exact number rather than a tolerance
	that hides a drift.
*/
class Source
{
public:
	virtual ~Source() = default;

	/// The sample rate changed, or the plugin is starting. Allocate here.
	virtual void Prepare( double sampleRate ) = 0;

	/// Drop all accumulated phase and position.
	virtual void Reset() = 0;

	/// Fill `n` samples. `dtPerSample` is written into each sample's `dt` unless
	/// the source has a reason to vary it.
	virtual void Render( Sample* out, int n, double dtPerSample ) = 0;
};

} // namespace vectrix
