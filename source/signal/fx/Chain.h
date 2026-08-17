#pragma once

#include "signal/Rng.h"
#include "signal/fx/Delay.h"
#include "signal/fx/Filters.h"
#include "signal/fx/Fx.h"
#include "signal/fx/Reverb.h"

#include <memory>

namespace vectrix
{
//===========================================================================
// Parameters, one struct per block. Filled from Controls each frame in
// engineering units -- Hz, dB, seconds -- never in 0..1.
//===========================================================================

struct VcaParams
{
	bool enabled = false;
	float level  = 1.0f; ///< linear, 0..2. A deflection gain, so not in dB.
	Routing routing = Routing::Stereo;
};

struct GateParams
{
	bool enabled     = false;
	float thresholdDb = -40.0f;
	float attackMs   = 1.0f;
	float holdMs     = 10.0f;
	float releaseMs  = 50.0f;
	bool muteMode    = false; ///< false = Blank (cut the gun), true = Mute (pull to centre)
};

struct CompressorParams
{
	bool enabled      = false;
	float thresholdDb = -12.0f;
	float ratio       = 4.0f;
	float kneeDb      = 6.0f;
	float attackMs    = 5.0f;
	float releaseMs   = 100.0f;
	float makeupDb    = 0.0f;
	bool autoMakeup   = true;
	float ceilingDb   = 6.0f;
	bool railMode     = false; ///< false = Gain (figure shrinks), true = Rail (figure flattens)
	Routing routing   = Routing::Stereo;
};

struct RectifierParams
{
	bool enabled  = false;
	bool fullWave = false;
	float bias    = 0.0f;
	Routing routing = Routing::XOnly;
};

struct SlewParams
{
	bool enabled = false;
	float rise   = 5000.0f; ///< volts per second
	float fall   = 5000.0f;
	bool link    = true;
	Routing routing = Routing::Stereo;
};

struct DriveParams
{
	bool enabled   = false;
	float driveDb  = 0.0f;
	float fold     = 0.0f; ///< 0..1 dry-to-folded crossfade
	int folds      = 2;    ///< 1..8
	bool oversample = true;
	Routing routing = Routing::Stereo;
};

struct RingModParams
{
	bool enabled = false;
	float freq   = 60.0f;
	int wave     = 0; ///< 0 sine, 1 triangle, 2 square
	float depth  = 0.0f;
	bool ratioLock = false;
	Routing routing = Routing::Cross;
};

struct BitcrushParams
{
	bool enabled = false;
	int bits     = 16;      ///< 16 = transparent
	float rateHz = 0.0f;    ///< 0 = off (full rate)
	Routing routing = Routing::Stereo;
};

struct PhaserParams
{
	bool enabled = false;
	int stages   = 4;
	float rate   = 0.3f;
	float depth  = 0.7f;
	float centre = 800.0f;
	float feedback = 0.5f;
	float mix    = 0.5f;
	Routing routing = Routing::XOnly;
};

struct FlangerParams
{
	bool enabled  = false;
	float rate    = 0.5f;
	float depth   = 0.7f;
	float delayMs = 2.0f;
	float feedback = 0.7f;
	float mix     = 0.5f;
	Routing routing = Routing::Stereo;
};

struct ChorusParams
{
	bool enabled  = false;
	float rate    = 0.4f;
	float depth   = 0.5f;
	float delayMs = 20.0f;
	float mix     = 0.4f;
};

struct OutputParams
{
	float gain    = 1.0f;
	float offsetX = 0.0f;
	float offsetY = 0.0f;
	float rotation = 0.0f; ///< turns
	float skew    = 0.0f;
	bool dcBlock  = true;
	float ampSlew = 20000.0f; ///< volts per second -- the amplifier's own limit
	float bandwidthX = 40000.0f;
	float bandwidthY = 10000.0f;
	float resonance  = 0.707f;
};

/// Everything the chain reads for one block.
struct ChainParams
{
	VcaParams vca;
	GateParams gate;
	CompressorParams compressor;
	RectifierParams rectifier;
	SlewParams slew;
	DriveParams drive;
	RingModParams ringMod;
	BitcrushParams bitcrush;
	PhaserParams phaser;
	FlangerParams flanger;
	ChorusParams chorus;
	DelayParams delay;
	ReverbParams reverb;
	OutputParams output;

	/// The source's X frequency, so the ring modulator's Ratio Lock can follow
	/// it and keep the figure still.
	float sourceFreq = 30.0f;
};

//===========================================================================
// The blocks
//===========================================================================

class Vca : public FxBlock
{
public:
	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Process( Sample* buffer, int n ) override;
	void SetParams( const VcaParams& p );

private:
	VcaParams params;
	Smooth level;
};

/**
	A noise gate whose detector is the beam's distance from the centre.

	`sqrt(x*x + y*y)` rather than a per-axis level, and that is not a shortcut --
	it makes the gate *radial*, so what it removes is the bright dot sitting at
	the origin when the signal is quiet. That dot is the thing a scope operator
	actually wants gone, and a per-axis gate instead blanks a diagonal band and
	looks broken.

	Blank mode is the physically correct one and the default: a CRT's grid
	voltage cuts the beam current without moving the yoke, so when the gate
	reopens the beam is where it should be rather than snapping back from centre.
*/
class Gate : public FxBlock
{
public:
	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Process( Sample* buffer, int n ) override;
	void SetParams( const GateParams& p );

private:
	enum class State
	{
		Closed,
		Attack,
		Open,
		Hold,
		Release
	};

	GateParams params;
	State state       = State::Closed;
	float envelope    = 0.0f;
	double holdLeft   = 0.0;
	float openLevel   = 0.0f; ///< linear threshold
	float closeLevel  = 0.0f; ///< 6 dB of hysteresis below it
	float attackCoeff = 1.0f;
	float releaseCoeff = 1.0f;
};

class Compressor : public FxBlock
{
public:
	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Process( Sample* buffer, int n ) override;
	void SetParams( const CompressorParams& p );

private:
	/// The static gain-computer curve, in dB. Separated out because the auto
	/// makeup is defined as minus this evaluated at 0 dB, and computing that
	/// with a second copy of the formula is how the two drift apart.
	float computeGainDb( float inputDb ) const;

	CompressorParams params;
	float envelopeDb   = 0.0f;
	float attackCoeff  = 1.0f;
	float releaseCoeff = 1.0f;
	float makeupDb     = 0.0f;
	float ceilingLinear = 2.0f;
};

class Rectifier : public FxBlock
{
public:
	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Process( Sample* buffer, int n ) override;
	void SetParams( const RectifierParams& p );

private:
	RectifierParams params;
};

class Slew : public FxBlock
{
public:
	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Process( Sample* buffer, int n ) override;
	void SetParams( const SlewParams& p );

private:
	SlewParams params;
	float currentA = 0.0f;
	float currentB = 0.0f;
};

class Drive : public FxBlock
{
public:
	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Process( Sample* buffer, int n ) override;
	void SetParams( const DriveParams& p );

private:
	float shape( float x ) const;

	DriveParams params;
	HalfBand upA[ 2 ], upB[ 2 ];
	HalfBand downA[ 2 ], downB[ 2 ];
	Smooth driveGain;
};

class RingMod : public FxBlock
{
public:
	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Process( Sample* buffer, int n ) override;
	void SetParams( const RingModParams& p, float sourceFreq );

private:
	RingModParams params;
	double phase = 0.0;
	double inc   = 0.0;
};

class Bitcrush : public FxBlock
{
public:
	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Process( Sample* buffer, int n ) override;
	void SetParams( const BitcrushParams& p );

private:
	BitcrushParams params;
	double holdPhase = 1.0;
	float heldA = 0.0f, heldB = 0.0f;
};

/**
	The phaser -- the block that justifies doing any of this on the CPU.

	A cascade of first-order allpasses shifts phase as a function of frequency.
	Applied to one deflection axis, every frequency component of X is delayed by
	a different amount: a pure sine figure *rotates*, and a complex figure
	*shears*, because its components rotate by different amounts. That is not an
	affine transform of the picture, so no vertex or fragment shader can produce
	it however clever it is.

	Mix matters more here than it does in audio. A phaser needs wet plus dry to
	form its notches -- but at Mix = 1.0 there are no notches at all, just pure
	phase shift, and pure phase shift between X and Y is exactly the rotation of
	a Lissajous ellipse. So Mix 1.0 is "the rotator" and Mix 0.5 is "the phaser",
	and both are worth documenting as separate instruments.
*/
class Phaser : public FxBlock
{
public:
	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Process( Sample* buffer, int n ) override;
	void SetParams( const PhaserParams& p );

private:
	static constexpr int kMaxStages = 12;

	PhaserParams params;
	Allpass1 stagesA[ kMaxStages ];
	Allpass1 stagesB[ kMaxStages ];
	float feedbackA = 0.0f, feedbackB = 0.0f;
	double lfoPhase = 0.0;
};

class Flanger : public FxBlock
{
public:
	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Process( Sample* buffer, int n ) override;
	void SetParams( const FlangerParams& p );

private:
	FlangerParams params;
	DelayLine lineA, lineB;
	float feedbackA = 0.0f, feedbackB = 0.0f;
	double lfoPhase = 0.0;
};

class Chorus : public FxBlock
{
public:
	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Process( Sample* buffer, int n ) override;
	void SetParams( const ChorusParams& p );

private:
	static constexpr int kTaps = 3;
	/// Mutually irrational-ish tap rates. Equal rates would put all three copies
	/// in the same place at the same moment and periodically collapse the halo.
	static constexpr float kTapRate[ kTaps ] = { 0.31f, 0.47f, 0.73f };

	ChorusParams params;
	DelayLine lineA, lineB;
	double lfoPhase = 0.0;
};

/**
	The deflection amplifier. Not bypassable -- there is always an amplifier.

	Its DC block is genuinely last in the chain, because the rectifier and the
	wavefolder both manufacture DC and DC here is a permanent screen offset
	rather than a tonal problem. Its slew limit and its bandwidth are what give
	the trace its corner rounding at the very end, which is where a real machine
	puts them: a deflection amplifier cannot move the beam infinitely fast, and
	the X amplifier in a real scope is faster than the Y one, which is why the
	two bandwidths default to different values.
*/
class Output : public FxBlock
{
public:
	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Process( Sample* buffer, int n ) override;
	void SetParams( const OutputParams& p );

private:
	OutputParams params;
	DcBlock dcX, dcY;
	Svf bandX, bandY;
	float slewX = 0.0f, slewY = 0.0f;
	Smooth gain, offsetX, offsetY;
};

//===========================================================================
// The chain
//===========================================================================

/**
	The fixed pedalboard.

	    VCA -> Gate -> Compressor -> Rectifier -> Slew -> Drive/Fold
	        -> Ring Mod -> Bitcrush -> Phaser -> Flanger -> Chorus
	        -> Delay -> Reverb -> Deflection Amp

	Not user-reorderable. Fourteen blocks is 14! orderings, none of which could
	be tested, and "what order are my pedals in" is the first thing an operator
	gets wrong. The order above is pedalboard orthodoxy and each adjacency has a
	reason:

	- **Gate before compressor, always.** A compressor upstream lifts the noise
	  floor into the gate's threshold and the gate chatters.
	- **Dynamics before distortion.** A wavefolder's character depends entirely
	  on the level going into it, so control the level first.
	- **Distortion before modulation.** A phaser after a fold sweeps a notch
	  through rich harmonics, which is interesting; a fold after a phaser just
	  folds the phaser's peaks, which is mush.
	- **Time effects last, shortest first** -- phaser (microseconds), flanger
	  (milliseconds), chorus (tens), delay (hundreds), reverb (seconds). Reverse
	  any pair and the earlier one's modulation is smeared by the later one's
	  memory.
*/
class Chain
{
public:
	void Prepare( double sampleRate );
	void Reset();
	void SetParams( const ChainParams& p );
	void Process( Sample* buffer, int n );

	/// For the harness: the reverb, so a test can assert its tail survives a
	/// parameter change.
	Reverb& ReverbBlock()
	{
		return reverb;
	}

private:
	Vca vca;
	Gate gate;
	Compressor compressor;
	Rectifier rectifier;
	Slew slew;
	Drive drive;
	RingMod ringMod;
	Bitcrush bitcrush;
	Phaser phaser;
	Flanger flanger;
	Chorus chorus;
	Delay delay;
	Reverb reverb;
	Output output;
};

} // namespace vectrix
