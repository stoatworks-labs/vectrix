#include "Clock.h"

#include "Signal.h"

#include <algorithm>
#include <cmath>

namespace vectrix
{

double sampleRateFor( Detail detail )
{
	switch( detail )
	{
		case Detail::Draft: return 48000.0;
		case Detail::Fine: return 192000.0;
		case Detail::Normal:
		default: return 96000.0;
	}
}

/// Frames that must agree before the host's clock unit is settled.
static constexpr int kClockVotes = 4;

void Clock::Update( double hostTime )
{
	double raw;
	if( hostTime >= 0.0 )
	{
		raw = hostTime;
	}
	else
	{
		//No host clock at all -- the offline harness before it starts driving
		//SetTime, or a host that never calls it. The wall clock is already in
		//seconds, so the unit question does not arise and the scale must not be
		//applied to it.
		const auto elapsed = std::chrono::steady_clock::now() - startTime;
		raw                = std::chrono::duration< double >( elapsed ).count();

		now          = raw;
		frameSeconds = std::clamp( lastNow >= 0.0 ? now - lastNow : kMaxFrameSeconds,
		                           kMinFrameSeconds, kMaxFrameSeconds );
		lastNow      = now;
		return;
	}

	//Decide the unit by measuring the host's clock against a real one. The
	//ratio is ~1 for a seconds host and ~1000 for a milliseconds host, and
	//nothing plausible sits between, so both bands are wide and a frame that
	//fits neither simply does not vote. This replaced a guess made from the
	//magnitude of one frame delta, which decided nothing between 0.5 and 2.0,
	//could lock to "seconds" off a burst of sub-0.5 ms frames at load, and
	//assumed seconds while undecided -- precisely the millisecond host's wrong
	//answer.
	const double wallNow =
	    std::chrono::duration< double >( std::chrono::steady_clock::now() - startTime ).count();

	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		//A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			//Several frames rather than one, so a single odd frame cannot
			//decide it alone.
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	lastRawTime  = raw;
	lastWallTime = wallNow;

	//Until the unit is settled, run on the real clock rather than assume one:
	//wrong in origin but right in rate, where assuming seconds would be a
	//thousand times fast on Resolume.
	const double scaled = clockScale != 0.0 ? raw * clockScale : wallNow;

	//Monotonic on the way out. A host that scrubs backwards hands us a negative
	//delta; the figure should carry on from where it is rather than unwinding,
	//because a deflection amplifier has no idea the transport moved.
	if( lastNow >= 0.0 )
	{
		const double delta = scaled - lastNow;
		frameSeconds       = std::clamp( delta, kMinFrameSeconds, kMaxFrameSeconds );
		now += frameSeconds;
	}
	else
	{
		frameSeconds = 1.0 / 60.0;
		now          = 0.0;
	}

	lastNow = scaled;
}

int Clock::SamplesForThisFrame( double sampleRate ) const
{
	if( !( sampleRate > 0.0 ) )
		return 2;

	const double wanted = frameSeconds * sampleRate;
	const int n         = static_cast< int >( std::lround( wanted ) );

	//At least two: one sample is a point with no interval after it, and the
	//renderer draws segments, so a one-sample block draws nothing at all.
	return std::clamp( n, 2, kMaxBlock );
}

void Clock::Reset()
{
	startTime   = std::chrono::steady_clock::now();
	lastRawTime = -1.0;
	lastNow     = -1.0;
	now         = 0.0;
	//clockScale deliberately survives: the host has not changed, and
	//re-deciding costs two frames of wrong-rate motion every time Reset is hit.
}

} // namespace vectrix
