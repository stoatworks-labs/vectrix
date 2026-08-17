#pragma once

#include <array>

namespace vectrix
{
/// The 64-bin spectrum Resolume delivers through an FF_USAGE_FFT buffer
/// parameter. Video rate, not audio rate -- this is a modulation source, not a
/// signal source. The audio file source is where actual audio-rate samples come
/// from.
constexpr int kAudioBins = 64;

/// Which slice of the spectrum a modulation slot follows. The boundaries are a
/// three-way cabinet's crossover points rather than equal thirds, because a
/// woofer is done by a few hundred hertz and a tweeter does not start until
/// several kHz -- equal thirds would give the bass band almost no content.
enum class Band
{
	FullRange,
	Low,
	Mid,
	High,
	Count
};

/// What a modulation slot drives. Deliberately short: a full matrix over 150
/// parameters is a second plugin, and four well-chosen destinations cover the
/// things people actually want moving.
enum class ModTarget
{
	None,
	SourceRate,
	Fold,
	RingDepth,
	PhaserRate,
	DelayMix,
	ReverbMix,
	BeamPower,
	Persistence,
	Count
};

struct ModSlot
{
	ModTarget target = ModTarget::None;
	float amount     = 0.0f; ///< -1..1, so a slot can subtract
	Band band        = Band::FullRange;
};

/**
	Video-rate modulation: the host's spectrum, its tempo, and two LFOs.

	Everything here updates once per frame. That is a real limit -- FFGL delivers
	one parameter value and one spectrum per frame -- and it is why these drive
	*parameters* rather than being mixed into the signal. A 60 Hz control signal
	stepped into an audio-rate chain would be a buzz; smoothed into a parameter
	it is a slow sweep, which is what it is for.
*/
class Modulation
{
public:
	void Update( const float* bins, int binCount, double frameSeconds );
	void UpdateTransport( float bpm, float barPhase, double nowSeconds );

	void SetLfo( int index, float rateHz, float depth );
	void SetSlot( int index, const ModSlot& slot );

	/// The modulation applied to one target, summed over every slot that names
	/// it. Zero when nothing does, so an unmodulated parameter is exactly itself.
	float For( ModTarget target ) const;

	/// A continuous bar count recovered from the host's tempo and bar phase.
	double Bars() const
	{
		return bars;
	}
	float Level( Band band ) const;

	static constexpr int kSlots = 4;
	static constexpr int kLfos  = 2;

private:
	std::array< float, kAudioBins > level{};
	std::array< ModSlot, kSlots > slots{};
	float lfoRate[ kLfos ]  = { 0.2f, 0.5f };
	float lfoDepth[ kLfos ] = { 0.0f, 0.0f };
	double lfoPhase[ kLfos ] = { 0.0, 0.25 };
	double bars              = 0.0;
};

} // namespace vectrix
