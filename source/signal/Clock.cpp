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

	//Decide the unit from the first plausible frame delta, then stop asking.
	//0.001..0.5 is a seconds host's frame; 2..500 is a milliseconds host's.
	//Anything outside both is a stall or a scrub and keeps waiting. Until it is
	//decided we assume seconds; the decision lands within two frames, and the
	//clamp below absorbs the single mis-scaled step a late decision produces.
	if( clockScale == 0.0 && lastRawTime >= 0.0 && raw > lastRawTime )
	{
		const double d = raw - lastRawTime;
		if( d >= 0.001 && d <= 0.5 )
			clockScale = 1.0;
		else if( d >= 2.0 && d <= 500.0 )
			clockScale = 0.001;
	}
	lastRawTime = raw;

	const double scaled = raw * ( clockScale == 0.0 ? 1.0 : clockScale );

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
