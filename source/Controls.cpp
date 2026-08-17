#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace vectrix
{

const char* const kDetailNames[ kDetailCount ] = { "Draft", "Normal", "Fine" };

const char* const kSourceNames[ kSourceCount ] = {
	"Oscillator", "Shape", "Wireframe", "Audio File", "Trace Input"
};

const char* const kWaveNames[ kWaveCount ] = {
	"Sine", "Triangle", "Saw", "Ramp", "Pulse", "Noise", "Sample & Hold"
};

const char* const kShapeNames[ kShapeCount ] = {
	"Circle", "Square", "Diamond", "Polygon", "Star", "Rose", "Spirograph"
};

const char* const kMeshNames[ kMeshCount ] = { "Cube", "Globe", "Tunnel", "Mountains" };

const char* const kRoutingNames[ kRoutingCount ] = {
	"Stereo", "X Only", "Y Only", "Mid/Side", "Mono", "Cross", "Ping-Pong"
};

const char* const kBandNames[ kBandCount ] = { "Full Range", "Low", "Mid", "High" };

const char* const kModTargetNames[ kModTargetCount ] = {
	"None", "Source Rate", "Fold", "Ring Depth", "Phaser Rate",
	"Delay Mix", "Reverb Mix", "Beam", "Persistence"
};

const char* const kMonoModeNames[ kMonoModeCount ] = { "X Only", "Derivative", "Delayed" };

const char* const kDelayDivisionNames[ kDelayDivisionCount ] = {
	"1/16", "1/8T", "1/8", "1/4T", "1/4", "1/2", "1 Bar", "2 Bars"
};

const char* const kPhosphorNames[ kPhosphorCount ] = {
	"P31 Green", "P1 Green", "P2 Long Green", "P7 Blue/Amber", "P11 Blue", "P39 Very Long"
};

int Option( float value, int count )
{
	//An option parameter holds its element *value*, not a 0..1 fraction, so it
	//is rounded rather than scaled. Getting this backwards gives a dropdown
	//permanently stuck on its first entry.
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

float Exponential( float value, float low, float high )
{
	const float t = std::clamp( value, 0.0f, 1.0f );
	return low * std::pow( high / low, t );
}

float Linear( float value, float low, float high )
{
	return low + ( high - low ) * std::clamp( value, 0.0f, 1.0f );
}

float DelayDivisionBars( int index )
{
	//In bars, so the delay time is bars * 240/bpm seconds.
	switch( std::clamp( index, 0, kDelayDivisionCount - 1 ) )
	{
		case 0: return 1.0f / 16.0f;
		case 1: return 1.0f / 12.0f;//eighth triplet
		case 2: return 1.0f / 8.0f;
		case 3: return 1.0f / 6.0f; //quarter triplet
		case 4: return 1.0f / 4.0f;
		case 5: return 1.0f / 2.0f;
		case 6: return 1.0f;
		case 7:
		default: return 2.0f;
	}
}

namespace
{
inline bool boolean( float v )
{
	return v > 0.5f;
}

inline Routing routing( float v )
{
	return static_cast< Routing >( Option( v, kRoutingCount ) );
}

/// Apply a modulation to a 0..1 parameter before it is mapped, so the
/// modulation lands in the parameter's own curve rather than on top of it. A
/// modulated Frequency should sweep exponentially like the knob does; adding
/// hertz after the map would make it lurch at the bottom of the range.
inline float modulated( float base, float amount )
{
	return std::clamp( base + amount, 0.0f, 1.0f );
}
} // namespace

Resolved Resolve( const float* p, const Modulation& mod, double bpm )
{
	Resolved r;

	r.detail = static_cast< Detail >( Option( p[ PT_DETAIL ], kDetailCount ) );
	r.source = static_cast< SourceKind >( Option( p[ PT_SOURCE ], kSourceCount ) );

	const float rateMod = mod.For( ModTarget::SourceRate );

	// -- Oscillator ----------------------------------------------------------
	r.oscillator.waveX        = static_cast< Wave >( Option( p[ PT_WAVE_X ], kWaveCount ) );
	r.oscillator.waveY        = static_cast< Wave >( Option( p[ PT_WAVE_Y ], kWaveCount ) );
	r.oscillator.freqX        = Exponential( modulated( p[ PT_FREQ_X ], rateMod ), 0.01f, 2000.0f );
	r.oscillator.ratioIndex   = Option( p[ PT_RATIO ], kRatioCount );
	r.oscillator.freeY        = boolean( p[ PT_FREE_Y ] );
	r.oscillator.freqY        = Exponential( p[ PT_FREQ_Y ], 0.01f, 2000.0f );
	r.oscillator.phaseY       = std::clamp( p[ PT_PHASE_Y ], 0.0f, 1.0f );
	r.oscillator.pwmX         = Linear( p[ PT_PWM_X ], 0.02f, 0.98f );
	r.oscillator.pwmY         = Linear( p[ PT_PWM_Y ], 0.02f, 0.98f );
	r.oscillator.detune       = Linear( p[ PT_DETUNE ], -0.02f, 0.02f );
	r.oscillator.hardSync     = boolean( p[ PT_HARD_SYNC ] );
	r.oscillator.blankRetrace = boolean( p[ PT_BLANK_RETRACE ] );

	// -- Shape ---------------------------------------------------------------
	r.shape.kind     = static_cast< ShapeKind >( Option( p[ PT_SHAPE ], kShapeCount ) );
	r.shape.rate     = Exponential( modulated( p[ PT_SHAPE_RATE ], rateMod ), 0.01f, 200.0f );
	r.shape.sides    = std::clamp( static_cast< int >( std::lround( p[ PT_SHAPE_N ] ) ), 3, 24 );
	r.shape.inner    = std::clamp( p[ PT_SHAPE_INNER ], 0.05f, 0.95f );
	r.shape.petalN   = std::clamp( static_cast< int >( std::lround( p[ PT_PETAL_N ] ) ), 1, 12 );
	r.shape.petalD   = std::clamp( static_cast< int >( std::lround( p[ PT_PETAL_D ] ) ), 1, 12 );
	r.shape.trochoid = Linear( p[ PT_TROCHOID ], 0.0f, 1.5f );

	// -- Wireframe -----------------------------------------------------------
	r.wire.mesh   = static_cast< MeshKind >( Option( p[ PT_MESH ], kMeshCount ) );
	r.wire.detail = std::clamp( static_cast< int >( std::lround( p[ PT_MESH_DETAIL ] ) ), 3, 24 );
	r.wire.spinX  = Linear( p[ PT_SPIN_X ], -2.0f, 2.0f );
	r.wire.spinY  = Linear( p[ PT_SPIN_Y ], -2.0f, 2.0f );
	r.wire.spinZ  = Linear( p[ PT_SPIN_Z ], -2.0f, 2.0f );
	r.wire.camera = Linear( p[ PT_CAMERA ], 2.0f, 8.0f );
	r.wire.scroll = Linear( p[ PT_SCROLL ], -4.0f, 4.0f );
	r.wire.seed   = static_cast< std::uint32_t >( std::lround( p[ PT_SEED ] ) );

	// -- Audio file ----------------------------------------------------------
	r.audioFile.rate  = Exponential( modulated( p[ PT_FILE_RATE ], rateMod ), 0.05f, 4.0f );
	r.audioFile.loop  = boolean( p[ PT_FILE_LOOP ] );
	r.audioFile.start = std::clamp( p[ PT_FILE_START ], 0.0f, 1.0f );
	r.audioFile.end   = std::clamp( p[ PT_FILE_END ], 0.0f, 1.0f );
	r.audioFile.monoMode = static_cast< MonoMode >( Option( p[ PT_FILE_MONO ], kMonoModeCount ) );
	r.audioFile.swapXY   = boolean( p[ PT_FILE_SWAP ] );
	r.audioFile.invertY  = boolean( p[ PT_FILE_INVERT ] );
	r.audioFile.sync     = boolean( p[ PT_FILE_SYNC ] ) ? AudioSync::Locked : AudioSync::Free;

	// -- Trace ---------------------------------------------------------------
	r.trace.threshold  = std::clamp( p[ PT_TRACE_THRESHOLD ], 0.02f, 0.95f );
	r.trace.stability  = std::clamp( p[ PT_TRACE_STABILITY ], 0.0f, 0.95f );
	r.trace.simplify   = std::clamp( p[ PT_TRACE_SIMPLIFY ], 0.0f, 1.0f );
	r.trace.maxStrokes = std::clamp( static_cast< int >( std::lround( p[ PT_TRACE_STROKES ] ) ), 4, 64 );

	// -- Shared beam path ----------------------------------------------------
	r.walker.refreshHz    = Exponential( modulated( p[ PT_REFRESH ], rateMod ), 5.0f, 120.0f );
	r.walker.retraceSpeed = Linear( p[ PT_RETRACE ], 1.0f, 20.0f );

	// -- VCA -----------------------------------------------------------------
	r.chain.vca.enabled = boolean( p[ PT_VCA_ON ] );
	r.chain.vca.level   = Linear( p[ PT_VCA_LEVEL ], 0.0f, 2.0f );
	r.chain.vca.routing = routing( p[ PT_VCA_ROUTING ] );

	// -- Gate ----------------------------------------------------------------
	r.chain.gate.enabled     = boolean( p[ PT_GATE_ON ] );
	r.chain.gate.thresholdDb = Linear( p[ PT_GATE_THRESHOLD ], -60.0f, 0.0f );
	r.chain.gate.attackMs    = Exponential( p[ PT_GATE_ATTACK ], 0.1f, 50.0f );
	r.chain.gate.holdMs      = Linear( p[ PT_GATE_HOLD ], 0.0f, 500.0f );
	r.chain.gate.releaseMs   = Exponential( p[ PT_GATE_RELEASE ], 1.0f, 1000.0f );
	r.chain.gate.muteMode    = boolean( p[ PT_GATE_MODE ] );

	// -- Compressor ----------------------------------------------------------
	r.chain.compressor.enabled     = boolean( p[ PT_COMP_ON ] );
	r.chain.compressor.thresholdDb = Linear( p[ PT_COMP_THRESHOLD ], -40.0f, 0.0f );
	r.chain.compressor.ratio       = Exponential( p[ PT_COMP_RATIO ], 1.0f, 20.0f );
	r.chain.compressor.kneeDb      = Linear( p[ PT_COMP_KNEE ], 0.0f, 24.0f );
	r.chain.compressor.attackMs    = Exponential( p[ PT_COMP_ATTACK ], 0.1f, 100.0f );
	r.chain.compressor.releaseMs   = Exponential( p[ PT_COMP_RELEASE ], 5.0f, 1000.0f );
	r.chain.compressor.makeupDb    = Linear( p[ PT_COMP_MAKEUP ], 0.0f, 24.0f );
	r.chain.compressor.autoMakeup  = boolean( p[ PT_COMP_AUTO ] );
	r.chain.compressor.ceilingDb   = Linear( p[ PT_COMP_CEILING ], 0.0f, 12.0f );
	r.chain.compressor.railMode    = boolean( p[ PT_COMP_LIMIT_MODE ] );
	r.chain.compressor.routing     = routing( p[ PT_COMP_ROUTING ] );

	// -- Rectifier -----------------------------------------------------------
	r.chain.rectifier.enabled  = boolean( p[ PT_RECT_ON ] );
	r.chain.rectifier.fullWave = boolean( p[ PT_RECT_MODE ] );
	r.chain.rectifier.bias     = Linear( p[ PT_RECT_BIAS ], -1.0f, 1.0f );
	r.chain.rectifier.routing  = routing( p[ PT_RECT_ROUTING ] );

	// -- Slew ----------------------------------------------------------------
	r.chain.slew.enabled = boolean( p[ PT_SLEW_ON ] );
	r.chain.slew.rise    = Exponential( p[ PT_SLEW_RISE ], 0.5f, 5000.0f );
	r.chain.slew.fall    = Exponential( p[ PT_SLEW_FALL ], 0.5f, 5000.0f );
	r.chain.slew.link    = boolean( p[ PT_SLEW_LINK ] );
	r.chain.slew.routing = routing( p[ PT_SLEW_ROUTING ] );

	// -- Drive / fold --------------------------------------------------------
	r.chain.drive.enabled    = boolean( p[ PT_DRIVE_ON ] );
	r.chain.drive.driveDb    = Linear( p[ PT_DRIVE_AMOUNT ], 0.0f, 40.0f );
	r.chain.drive.fold       = std::clamp( p[ PT_DRIVE_FOLD ] + mod.For( ModTarget::Fold ), 0.0f, 1.0f );
	r.chain.drive.folds      = std::clamp( static_cast< int >( std::lround( p[ PT_DRIVE_FOLDS ] ) ), 1, 8 );
	r.chain.drive.oversample = boolean( p[ PT_DRIVE_OVERSAMPLE ] );
	r.chain.drive.routing    = routing( p[ PT_DRIVE_ROUTING ] );

	// -- Ring modulator ------------------------------------------------------
	r.chain.ringMod.enabled   = boolean( p[ PT_RING_ON ] );
	r.chain.ringMod.freq      = Exponential( p[ PT_RING_FREQ ], 0.1f, 2000.0f );
	r.chain.ringMod.wave      = Option( p[ PT_RING_WAVE ], 3 );
	r.chain.ringMod.depth     = std::clamp( p[ PT_RING_DEPTH ] + mod.For( ModTarget::RingDepth ), 0.0f, 1.0f );
	r.chain.ringMod.ratioLock = boolean( p[ PT_RING_LOCK ] );
	r.chain.ringMod.routing   = routing( p[ PT_RING_ROUTING ] );

	// -- Bitcrush ------------------------------------------------------------
	r.chain.bitcrush.enabled = boolean( p[ PT_CRUSH_ON ] );
	r.chain.bitcrush.bits    = std::clamp( static_cast< int >( std::lround( p[ PT_CRUSH_BITS ] ) ), 1, 16 );
	//At the top of its travel the rate reducer is off, not merely fast: an
	//"almost off" sample-and-hold still beats against the source and crawls.
	r.chain.bitcrush.rateHz  = p[ PT_CRUSH_RATE ] >= 0.999f ? 0.0f
	                                                        : Exponential( p[ PT_CRUSH_RATE ], 100.0f, 48000.0f );
	r.chain.bitcrush.routing = routing( p[ PT_CRUSH_ROUTING ] );

	// -- Phaser --------------------------------------------------------------
	r.chain.phaser.enabled  = boolean( p[ PT_PHASE_ON ] );
	r.chain.phaser.stages   = std::clamp( static_cast< int >( std::lround( p[ PT_PHASE_STAGES ] ) ), 1, 12 );
	r.chain.phaser.rate     = Exponential( modulated( p[ PT_PHASE_RATE ], mod.For( ModTarget::PhaserRate ) ), 0.01f, 10.0f );
	r.chain.phaser.depth    = std::clamp( p[ PT_PHASE_DEPTH ], 0.0f, 1.0f );
	r.chain.phaser.centre   = Exponential( p[ PT_PHASE_CENTRE ], 100.0f, 8000.0f );
	r.chain.phaser.feedback = Linear( p[ PT_PHASE_FEEDBACK ], 0.0f, 0.95f );
	r.chain.phaser.mix      = std::clamp( p[ PT_PHASE_MIX ], 0.0f, 1.0f );
	r.chain.phaser.routing  = routing( p[ PT_PHASE_ROUTING ] );

	// -- Flanger -------------------------------------------------------------
	r.chain.flanger.enabled  = boolean( p[ PT_FLANGE_ON ] );
	r.chain.flanger.rate     = Exponential( p[ PT_FLANGE_RATE ], 0.05f, 5.0f );
	r.chain.flanger.depth    = std::clamp( p[ PT_FLANGE_DEPTH ], 0.0f, 1.0f );
	r.chain.flanger.delayMs  = Exponential( p[ PT_FLANGE_DELAY ], 0.1f, 10.0f );
	r.chain.flanger.feedback = Linear( p[ PT_FLANGE_FEEDBACK ], -0.95f, 0.95f );
	r.chain.flanger.mix      = std::clamp( p[ PT_FLANGE_MIX ], 0.0f, 1.0f );
	r.chain.flanger.routing  = routing( p[ PT_FLANGE_ROUTING ] );

	// -- Chorus --------------------------------------------------------------
	r.chain.chorus.enabled = boolean( p[ PT_CHORUS_ON ] );
	r.chain.chorus.rate    = Exponential( p[ PT_CHORUS_RATE ], 0.05f, 3.0f );
	r.chain.chorus.depth   = std::clamp( p[ PT_CHORUS_DEPTH ], 0.0f, 1.0f );
	r.chain.chorus.delayMs = Linear( p[ PT_CHORUS_DELAY ], 10.0f, 40.0f );
	r.chain.chorus.mix     = std::clamp( p[ PT_CHORUS_MIX ], 0.0f, 1.0f );

	// -- Delay ---------------------------------------------------------------
	r.chain.delay.enabled = boolean( p[ PT_DELAY_ON ] );
	if( boolean( p[ PT_DELAY_SYNC ] ) )
	{
		//A ghost that lands on the beat is the entire reason the host bothers to
		//tell us the tempo.
		const double tempo   = bpm > 1.0 ? bpm : 120.0;
		const double barSecs = 240.0 / tempo;
		r.chain.delay.timeMs = static_cast< float >(
			DelayDivisionBars( Option( p[ PT_DELAY_DIVISION ], kDelayDivisionCount ) ) * barSecs * 1000.0 );
		r.chain.delay.timeMs = std::clamp( r.chain.delay.timeMs, 1.0f, 2500.0f );
	}
	else
	{
		r.chain.delay.timeMs = Exponential( p[ PT_DELAY_TIME ], 1.0f, 2500.0f );
	}
	r.chain.delay.feedback   = Linear( p[ PT_DELAY_FEEDBACK ], 0.0f, 1.05f );
	r.chain.delay.dampLowHz  = Exponential( p[ PT_DELAY_DAMP_LOW ], 200.0f, 20000.0f );
	r.chain.delay.dampHighHz = Exponential( p[ PT_DELAY_DAMP_HIGH ], 20.0f, 1000.0f );
	r.chain.delay.mix        = std::clamp( p[ PT_DELAY_MIX ] + mod.For( ModTarget::DelayMix ), 0.0f, 1.0f );
	r.chain.delay.routing    = routing( p[ PT_DELAY_ROUTING ] );
	r.chain.delay.timeMode   = boolean( p[ PT_DELAY_TIME_MODE ] ) ? DelayTimeMode::Crossfade
	                                                              : DelayTimeMode::Repitch;

	// -- Reverb --------------------------------------------------------------
	r.chain.reverb.enabled    = boolean( p[ PT_VERB_ON ] );
	r.chain.reverb.preDelayMs = Linear( p[ PT_VERB_PREDELAY ], 0.0f, 200.0f );
	r.chain.reverb.decaySec   = Exponential( p[ PT_VERB_DECAY ], 0.1f, 20.0f );
	r.chain.reverb.damping    = std::clamp( p[ PT_VERB_DAMPING ], 0.0f, 1.0f );
	r.chain.reverb.diffusion  = Linear( p[ PT_VERB_DIFFUSION ], 0.0f, 0.9f );
	r.chain.reverb.size       = Exponential( p[ PT_VERB_SIZE ], 0.25f, 2.0f );
	r.chain.reverb.modulation = std::clamp( p[ PT_VERB_MOD ], 0.0f, 1.0f );
	r.chain.reverb.mix        = std::clamp( p[ PT_VERB_MIX ] + mod.For( ModTarget::ReverbMix ), 0.0f, 1.0f );

	// -- Deflection amplifier ------------------------------------------------
	r.chain.output.gain       = Linear( p[ PT_OUT_GAIN ], 0.0f, 2.0f );
	r.chain.output.offsetX    = Linear( p[ PT_OUT_OFFSET_X ], -1.0f, 1.0f );
	r.chain.output.offsetY    = Linear( p[ PT_OUT_OFFSET_Y ], -1.0f, 1.0f );
	r.chain.output.rotation   = Linear( p[ PT_OUT_ROTATION ], -0.5f, 0.5f );
	r.chain.output.skew       = Linear( p[ PT_OUT_SKEW ], -1.0f, 1.0f );
	r.chain.output.dcBlock    = boolean( p[ PT_OUT_DC ] );
	r.chain.output.ampSlew    = Exponential( p[ PT_OUT_SLEW ], 100.0f, 200000.0f );
	r.chain.output.bandwidthX = Exponential( p[ PT_OUT_BANDWIDTH_X ], 1000.0f, 80000.0f );
	r.chain.output.bandwidthY = Exponential( p[ PT_OUT_BANDWIDTH_Y ], 1000.0f, 80000.0f );
	r.chain.output.resonance  = Linear( p[ PT_OUT_RESONANCE ], 0.5f, 4.0f );

	//The ring modulator's Ratio Lock needs to know what the source is doing.
	r.chain.sourceFreq = r.source == SourceKind::Shape ? r.shape.rate : r.oscillator.freqX;

	r.mix = std::clamp( p[ PT_MIX ], 0.0f, 1.0f );

	return r;
}

} // namespace vectrix
