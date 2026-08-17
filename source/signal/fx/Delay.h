#pragma once

#include "signal/fx/Filters.h"
#include "signal/fx/Fx.h"

namespace vectrix
{
/// How a time change is negotiated.
enum class DelayTimeMode
{
	Repitch,  ///< The read pointer's rate ramps. The ghosts audibly swoop.
	Crossfade,///< Two read pointers, equal power. Clean, no swoop.
	Count
};

struct DelayParams
{
	bool enabled  = false;
	float timeMs  = 250.0f;
	float feedback = 0.45f;
	float dampLowHz = 4000.0f;  ///< one-pole LP in the feedback path
	float dampHighHz = 100.0f;  ///< one-pole HP in the feedback path
	float mix     = 0.4f;
	Routing routing = Routing::Stereo;
	DelayTimeMode timeMode = DelayTimeMode::Repitch;
};

/**
	The ghost repeats.

	Two things here are consequences rather than decoration, and both are why the
	delay is worth modelling properly instead of drawing copies of the figure.

	**The feedback path is filtered**, one pole down at the top and one at the
	bottom. That is what makes each repeat progressively smoother and
	rounder-cornered than the last -- a ghost is not a faded copy of the figure,
	it is a *low-passed* copy, and a low-passed deflection signal has softer
	corners and a slightly smaller excursion. Pure analogue-delay behaviour, and
	nothing in the renderer has to know about it.

	**Feedback is allowed past 1.0**, up to 1.05, with a `tanh` saturator in the
	loop. A self-oscillating delay is real pedal behaviour, and the saturator is
	exactly why a real analogue delay runs away into a howl and then sits there
	rather than reaching infinity.
*/
class Delay : public FxBlock
{
public:
	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Process( Sample* buffer, int n ) override;
	void SetParams( const DelayParams& p );

	/// Non-zero while any energy remains in the lines. The harness asserts this
	/// survives a parameter change.
	float TailEnergy() const;

private:
	/// 2^19 samples is 2.73 s at 192 kHz; the Time parameter tops out at 2.5 s so
	/// a read can never wrap round into data the write pointer has not reached.
	static constexpr std::size_t kLength = 1u << 19;

	DelayParams params;
	DelayLine lineA, lineB;
	OnePole dampLowA, dampLowB;
	OnePole dampHighA, dampHighB;

	double currentDelay = 0.0; ///< samples; ramps toward targetDelay in Repitch
	double targetDelay  = 0.0;
	double fadePosition = 0.0; ///< Crossfade mode: 0..1 across the swap
	double previousDelay = 0.0;
};

} // namespace vectrix
