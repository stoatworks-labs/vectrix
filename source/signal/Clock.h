#pragma once

#include <chrono>

namespace vectrix
{
/**
	The plugin's clock, and the thing that decides how many samples a frame is
	worth.

	Two jobs that both sound trivial and are not.

	**What unit is the host's clock in?** The FFGL header never says. Resolume
	hands over milliseconds -- measured live in Arena 7.27.1 at 20.0 per frame at
	its 50 fps, and the SDK's own Particles sample divides by 1000 -- while the
	offline harness, and any host following the header's silence, sends seconds.
	A plugin that guesses wrong runs 1000x fast or freezes, and no offline
	harness can catch it because the harness is the one sending seconds. So the
	unit is decided from the first plausible frame delta and then stuck to.

	**How long was this frame?** FFGL does not say that either, so it comes from
	the difference between two clock readings -- and that difference has to be
	clamped. An unclamped delta after a dropped frame, a window drag or the
	operator scrubbing the transport asks the engine for half a second of audio
	in one block, which is a 30x energy deposit and a white flash. Clamping is
	one line and it is not optional.
*/
class Clock
{
public:
	/// Advance to this frame. `hostTime` is whatever the host last handed to
	/// SetTime, or a negative number if it never called it.
	void Update( double hostTime );

	/// Declare the host's unit instead of letting Update infer it. The offline
	/// harness renders as fast as the GPU allows, so the calibration -- which
	/// measures host time against real elapsed time -- has nothing to measure.
	void SetScaleForTest( double scale ) { clockScale = scale; }

	/// Seconds since the plugin started, normalised. Monotonic.
	double Now() const
	{
		return now;
	}

	/// The duration of the frame just entered, in seconds, already clamped.
	double FrameSeconds() const
	{
		return frameSeconds;
	}

	/// How many samples to synthesise for this frame at this rate. Always at
	/// least 2 -- one sample makes no segment and the renderer would draw
	/// nothing -- and never more than kMaxBlock.
	int SamplesForThisFrame( double sampleRate ) const;

	/// True once the host's clock unit has been decided. Logged once, because
	/// it settles an argument about what a host actually sent.
	bool UnitDecided() const
	{
		return clockScale != 0.0;
	}

	/// 1.0 for a seconds host, 0.001 for a milliseconds host, 0.0 undecided.
	double ClockScale() const
	{
		return clockScale;
	}

	/// Drop every accumulated position. Used by the Reset control and whenever
	/// the sample rate changes under us.
	void Reset();

private:
	/// A frame shorter than this is a duplicate call or a clock that has not
	/// moved; one longer is a stall, a scrub or a dropped frame. Both are
	/// clamped rather than believed.
	static constexpr double kMinFrameSeconds = 1.0 / 240.0;
	static constexpr double kMaxFrameSeconds = 1.0 / 24.0;

	std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();

	double clockScale   = 0.0; ///< 0 until decided, then 1.0 or 0.001.
	double lastWallTime = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastRawTime  = -1.0;
	double now          = 0.0;
	double lastNow      = -1.0;
	double frameSeconds = 1.0 / 60.0;
};

} // namespace vectrix
