#include "Vectrix.h"

#include "Diag.h"
#include "Presets.h"
#include "render/Phosphor.h"
#include "StoatworksAboutParams.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vectrix
{
namespace
{
/// The parameters a preset covers, in the order `presets::Param` declares them.
/// The static_assert below is what stops the two lists drifting apart, which
/// would otherwise show up as a preset quietly writing its Fold value into the
/// Ring Depth slider.
constexpr unsigned int kPresetParamIDs[] = {
	PT_SOURCE, PT_WAVE_X, PT_WAVE_Y, PT_FREQ_X, PT_RATIO, PT_PHASE_Y, PT_BLANK_RETRACE,
	PT_SHAPE, PT_SHAPE_RATE, PT_SHAPE_N, PT_MESH, PT_MESH_DETAIL, PT_REFRESH,

	PT_GATE_ON, PT_GATE_THRESHOLD, PT_COMP_ON, PT_COMP_THRESHOLD, PT_COMP_RATIO, PT_COMP_CEILING,

	PT_RECT_ON, PT_RECT_ROUTING, PT_SLEW_ON, PT_SLEW_RISE, PT_DRIVE_ON, PT_DRIVE_AMOUNT,
	PT_DRIVE_FOLD, PT_DRIVE_FOLDS, PT_RING_ON, PT_RING_FREQ, PT_RING_DEPTH, PT_RING_ROUTING,
	PT_CRUSH_ON, PT_CRUSH_BITS,

	PT_PHASE_ON, PT_PHASE_RATE, PT_PHASE_MIX, PT_PHASE_ROUTING, PT_FLANGE_ON, PT_FLANGE_MIX,
	PT_CHORUS_ON, PT_CHORUS_MIX, PT_DELAY_ON, PT_DELAY_TIME, PT_DELAY_FEEDBACK, PT_DELAY_MIX,
	PT_DELAY_ROUTING, PT_VERB_ON, PT_VERB_DECAY, PT_VERB_MIX,

	PT_BEAM_POWER, PT_BEAM_FOCUS, PT_PHOSPHOR, PT_PERSISTENCE, PT_HALATION, PT_FACE_BLACK,
	PT_GRATICULE,
};

static_assert( sizeof( kPresetParamIDs ) / sizeof( kPresetParamIDs[ 0 ] ) == presets::kParamCount,
               "the preset parameter list and the preset table disagree" );

//The About block is a text line plus one button per link the branding header
//carries. If a user-guide URL is added later this fires, rather than the last
//button quietly going undeclared and the host showing an unnamed control.
static_assert( PT_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About enum run does not match the number of About parameters" );

/// The bottom of the Beam control's travel, over which the exponential is faded
/// linearly to a true zero. Short enough that it costs no useful resolution and
/// long enough that the gun goes out smoothly rather than snapping off.
constexpr float kBeamOffRamp = 0.02f;

/// A defaults table, so that "what does this plugin do when you drop it on a
/// layer" is one readable list rather than scattered through the declaration.
struct Default
{
	unsigned int id;
	float value;
};

constexpr Default kDefaults[] = {
	{ PT_DETAIL, 1.0f }, { PT_SEED, 1.0f },
	{ PT_SOURCE, 0.0f },

	{ PT_FREQ_X, 0.77f }, { PT_RATIO, 3.0f }, { PT_FREQ_Y, 0.77f }, { PT_PHASE_Y, 0.25f },
	{ PT_PWM_X, 0.5f }, { PT_PWM_Y, 0.5f }, { PT_DETUNE, 0.5f },

	{ PT_SHAPE_RATE, 0.88f }, { PT_SHAPE_N, 5.0f }, { PT_SHAPE_INNER, 0.382f },
	{ PT_PETAL_N, 3.0f }, { PT_PETAL_D, 1.0f }, { PT_TROCHOID, 0.4f },

	{ PT_MESH, 3.0f }, { PT_MESH_DETAIL, 8.0f },
	{ PT_SPIN_X, 0.53f }, { PT_SPIN_Y, 0.55f }, { PT_SPIN_Z, 0.5f },
	{ PT_CAMERA, 0.33f }, { PT_SCROLL, 0.625f },

	{ PT_REFRESH, 0.5f }, { PT_RETRACE, 0.37f },

	{ PT_FILE_RATE, 0.5f }, { PT_FILE_LOOP, 1.0f }, { PT_FILE_END, 1.0f },
	{ PT_FILE_MONO, 2.0f }, { PT_FILE_INVERT, 1.0f },

	{ PT_TRACE_THRESHOLD, 0.35f }, { PT_TRACE_STABILITY, 0.5f },
	{ PT_TRACE_SIMPLIFY, 0.4f }, { PT_TRACE_STROKES, 24.0f },

	{ PT_VCA_LEVEL, 0.5f },
	{ PT_GATE_THRESHOLD, 0.33f }, { PT_GATE_ATTACK, 0.37f }, { PT_GATE_HOLD, 0.02f },
	{ PT_GATE_RELEASE, 0.57f },
	{ PT_COMP_THRESHOLD, 0.7f }, { PT_COMP_RATIO, 0.46f }, { PT_COMP_KNEE, 0.25f },
	{ PT_COMP_ATTACK, 0.57f }, { PT_COMP_RELEASE, 0.57f }, { PT_COMP_AUTO, 1.0f },
	{ PT_COMP_CEILING, 0.5f },

	{ PT_RECT_ROUTING, 1.0f }, { PT_RECT_BIAS, 0.5f },
	{ PT_SLEW_RISE, 1.0f }, { PT_SLEW_FALL, 1.0f }, { PT_SLEW_LINK, 1.0f },
	{ PT_DRIVE_FOLDS, 2.0f }, { PT_DRIVE_OVERSAMPLE, 1.0f },
	{ PT_RING_FREQ, 0.65f }, { PT_RING_ROUTING, 5.0f },
	{ PT_CRUSH_BITS, 16.0f }, { PT_CRUSH_RATE, 1.0f },

	{ PT_PHASE_STAGES, 4.0f }, { PT_PHASE_RATE, 0.5f }, { PT_PHASE_DEPTH, 0.7f },
	{ PT_PHASE_CENTRE, 0.47f }, { PT_PHASE_FEEDBACK, 0.53f }, { PT_PHASE_MIX, 0.5f },
	{ PT_PHASE_ROUTING, 1.0f },

	{ PT_FLANGE_RATE, 0.5f }, { PT_FLANGE_DEPTH, 0.7f }, { PT_FLANGE_DELAY, 0.65f },
	{ PT_FLANGE_FEEDBACK, 0.87f }, { PT_FLANGE_MIX, 0.5f },

	{ PT_CHORUS_RATE, 0.5f }, { PT_CHORUS_DEPTH, 0.5f }, { PT_CHORUS_DELAY, 0.33f },
	{ PT_CHORUS_MIX, 0.4f },

	{ PT_DELAY_TIME, 0.7f }, { PT_DELAY_DIVISION, 4.0f }, { PT_DELAY_FEEDBACK, 0.43f },
	{ PT_DELAY_DAMP_LOW, 0.65f }, { PT_DELAY_DAMP_HIGH, 0.41f }, { PT_DELAY_MIX, 0.4f },

	{ PT_VERB_PREDELAY, 0.1f }, { PT_VERB_DECAY, 0.57f }, { PT_VERB_DAMPING, 0.5f },
	{ PT_VERB_DIFFUSION, 0.78f }, { PT_VERB_SIZE, 0.66f }, { PT_VERB_MOD, 0.2f },
	{ PT_VERB_MIX, 0.3f },

	{ PT_OUT_GAIN, 0.5f }, { PT_OUT_OFFSET_X, 0.5f }, { PT_OUT_OFFSET_Y, 0.5f },
	{ PT_OUT_ROTATION, 0.5f }, { PT_OUT_SKEW, 0.5f }, { PT_OUT_DC, 1.0f },
	{ PT_OUT_SLEW, 0.70f }, { PT_OUT_BANDWIDTH_X, 0.84f }, { PT_OUT_BANDWIDTH_Y, 0.66f },
	{ PT_OUT_RESONANCE, 0.06f },

	{ PT_BEAM_POWER, 0.45f }, { PT_BEAM_FOCUS, 0.30f }, { PT_BEAM_DEFOCUS, 0.35f },
	{ PT_BLANK_FLOOR, 0.0f },

	{ PT_PHOSPHOR, 0.0f }, { PT_PERSISTENCE, kPersistenceUnityPoint }, { PT_HALATION, 0.35f },
	{ PT_HALATION_RADIUS, 0.5f }, { PT_FACE_ASPECT, 0.0f }, { PT_CORNER_RADIUS, 1.0f },
	{ PT_DEFLECTION_GAIN, 0.39f }, { PT_CURVATURE, 0.0f }, { PT_VIGNETTE, 0.2f },
	{ PT_FACE_BLACK, 0.85f }, { PT_FILTER_TINT, 0.5f }, { PT_GRATICULE, 0.25f },

	{ PT_MIX, 1.0f }, { PT_PRESET, 0.0f },

	{ PT_LFO1_RATE, 0.3f }, { PT_LFO2_RATE, 0.5f },
	{ PT_MOD1_AMOUNT, 0.5f }, { PT_MOD2_AMOUNT, 0.5f },
	{ PT_MOD3_AMOUNT, 0.5f }, { PT_MOD4_AMOUNT, 0.5f },
};
} // namespace

VectrixPlugin::VectrixPlugin( bool over ) : overInput( over )
{
	//A source takes no input; an effect takes exactly one. Getting this wrong is
	//not a compile error -- the host simply files the plugin under the wrong tab
	//and hands it a texture it was not expecting.
	SetMinInputs( overInput ? 1 : 0 );
	SetMaxInputs( overInput ? 1 : 0 );

	for( const Default& d : kDefaults )
		params[ d.id ] = d.value;

	declareParameters();

	diag::init();
}

void VectrixPlugin::declareParameters()
{
	auto standard = []( VectrixPlugin* self, unsigned int id, const char* name ) {
		self->SetParamInfof( id, name, FF_TYPE_STANDARD );
	};

	// -- Clock ---------------------------------------------------------------
	SetOptionParamInfo( PT_DETAIL, "Detail", kDetailCount, params[ PT_DETAIL ] );
	for( int i = 0; i < kDetailCount; ++i )
		SetParamElementInfo( PT_DETAIL, i, kDetailNames[ i ], static_cast< float >( i ) );
	SetParamInfo( PT_RESET, "Reset", FF_TYPE_EVENT, false );
	//An integer with a real range: the STANDARD clamp is guarded by the type, so
	//FF_TYPE_INTEGER defaults pass through and SetParamRange then widens them.
	SetParamInfo( PT_SEED, "Seed", FF_TYPE_INTEGER, 1.0f );
	SetParamRange( PT_SEED, 1.0f, 9999.0f );

	// -- Source --------------------------------------------------------------
	SetOptionParamInfo( PT_SOURCE, "Source", kSourceCount, params[ PT_SOURCE ] );
	for( int i = 0; i < kSourceCount; ++i )
		SetParamElementInfo( PT_SOURCE, i, kSourceNames[ i ], static_cast< float >( i ) );

	// -- Oscillator ----------------------------------------------------------
	SetOptionParamInfo( PT_WAVE_X, "Wave X", kWaveCount, params[ PT_WAVE_X ] );
	for( int i = 0; i < kWaveCount; ++i )
		SetParamElementInfo( PT_WAVE_X, i, kWaveNames[ i ], static_cast< float >( i ) );
	SetOptionParamInfo( PT_WAVE_Y, "Wave Y", kWaveCount, params[ PT_WAVE_Y ] );
	for( int i = 0; i < kWaveCount; ++i )
		SetParamElementInfo( PT_WAVE_Y, i, kWaveNames[ i ], static_cast< float >( i ) );

	standard( this, PT_FREQ_X, "Frequency X" );
	SetOptionParamInfo( PT_RATIO, "Ratio", kRatioCount, params[ PT_RATIO ] );
	for( int i = 0; i < kRatioCount; ++i )
		SetParamElementInfo( PT_RATIO, i, kRatioNames[ i ], static_cast< float >( i ) );
	SetParamInfo( PT_FREE_Y, "Free Y", FF_TYPE_BOOLEAN, false );
	standard( this, PT_FREQ_Y, "Frequency Y" );
	standard( this, PT_PHASE_Y, "Phase Y" );
	standard( this, PT_PWM_X, "Width X" );
	standard( this, PT_PWM_Y, "Width Y" );
	standard( this, PT_DETUNE, "Detune" );
	SetParamInfo( PT_HARD_SYNC, "Hard Sync", FF_TYPE_BOOLEAN, false );
	SetParamInfo( PT_BLANK_RETRACE, "Blank on Retrace", FF_TYPE_BOOLEAN, false );

	// -- Shape ---------------------------------------------------------------
	SetOptionParamInfo( PT_SHAPE, "Shape", kShapeCount, params[ PT_SHAPE ] );
	for( int i = 0; i < kShapeCount; ++i )
		SetParamElementInfo( PT_SHAPE, i, kShapeNames[ i ], static_cast< float >( i ) );
	standard( this, PT_SHAPE_RATE, "Shape Rate" );
	SetParamInfo( PT_SHAPE_N, "Sides", FF_TYPE_INTEGER, 5.0f );
	SetParamRange( PT_SHAPE_N, 3.0f, 24.0f );
	standard( this, PT_SHAPE_INNER, "Inner Radius" );
	SetParamInfo( PT_PETAL_N, "Petals", FF_TYPE_INTEGER, 3.0f );
	SetParamRange( PT_PETAL_N, 1.0f, 12.0f );
	SetParamInfo( PT_PETAL_D, "Petal Divisor", FF_TYPE_INTEGER, 1.0f );
	SetParamRange( PT_PETAL_D, 1.0f, 12.0f );
	standard( this, PT_TROCHOID, "Pen Offset" );

	// -- Wireframe -----------------------------------------------------------
	SetOptionParamInfo( PT_MESH, "Mesh", kMeshCount, params[ PT_MESH ] );
	for( int i = 0; i < kMeshCount; ++i )
		SetParamElementInfo( PT_MESH, i, kMeshNames[ i ], static_cast< float >( i ) );
	SetParamInfo( PT_MESH_DETAIL, "Mesh Detail", FF_TYPE_INTEGER, 8.0f );
	SetParamRange( PT_MESH_DETAIL, 3.0f, 24.0f );
	standard( this, PT_SPIN_X, "Spin X" );
	standard( this, PT_SPIN_Y, "Spin Y" );
	standard( this, PT_SPIN_Z, "Spin Z" );
	standard( this, PT_CAMERA, "Camera" );
	standard( this, PT_SCROLL, "Scroll" );

	// -- Beam path -----------------------------------------------------------
	standard( this, PT_REFRESH, "Refresh Rate" );
	standard( this, PT_RETRACE, "Retrace Speed" );

	// -- Audio file ----------------------------------------------------------
	{
		std::vector< std::string > extensions;
		for( const char* const* e = kAudioExtensions; *e != nullptr; ++e )
			extensions.emplace_back( *e );
		SetFileParamInfo( PT_FILE, "Audio File", extensions, "" );
	}
	standard( this, PT_FILE_RATE, "File Rate" );
	SetParamInfo( PT_FILE_LOOP, "Loop", FF_TYPE_BOOLEAN, true );
	standard( this, PT_FILE_START, "Start" );
	standard( this, PT_FILE_END, "End" );
	SetOptionParamInfo( PT_FILE_MONO, "Mono Mode", kMonoModeCount, params[ PT_FILE_MONO ] );
	for( int i = 0; i < kMonoModeCount; ++i )
		SetParamElementInfo( PT_FILE_MONO, i, kMonoModeNames[ i ], static_cast< float >( i ) );
	SetParamInfo( PT_FILE_SWAP, "Swap X/Y", FF_TYPE_BOOLEAN, false );
	SetParamInfo( PT_FILE_INVERT, "Invert Y", FF_TYPE_BOOLEAN, true );
	SetParamInfo( PT_FILE_SYNC, "Lock to Clip", FF_TYPE_BOOLEAN, false );

	// -- Trace ---------------------------------------------------------------
	standard( this, PT_TRACE_THRESHOLD, "Edge Threshold" );
	standard( this, PT_TRACE_STABILITY, "Stability" );
	standard( this, PT_TRACE_SIMPLIFY, "Simplify" );
	SetParamInfo( PT_TRACE_STROKES, "Max Strokes", FF_TYPE_INTEGER, 24.0f );
	SetParamRange( PT_TRACE_STROKES, 4.0f, 64.0f );

	auto routingParam = [ & ]( unsigned int id, const char* name, int fallback ) {
		SetOptionParamInfo( id, name, kRoutingCount, params[ id ] );
		for( int i = 0; i < kRoutingCount; ++i )
			SetParamElementInfo( id, i, kRoutingNames[ i ], static_cast< float >( i ) );
		( void )fallback;
	};

	// -- VCA -----------------------------------------------------------------
	SetParamInfo( PT_VCA_ON, "VCA", FF_TYPE_BOOLEAN, false );
	standard( this, PT_VCA_LEVEL, "Level" );
	routingParam( PT_VCA_ROUTING, "VCA Routing", 0 );

	// -- Gate ----------------------------------------------------------------
	SetParamInfo( PT_GATE_ON, "Gate", FF_TYPE_BOOLEAN, false );
	standard( this, PT_GATE_THRESHOLD, "Gate Threshold" );
	standard( this, PT_GATE_ATTACK, "Gate Attack" );
	standard( this, PT_GATE_HOLD, "Gate Hold" );
	standard( this, PT_GATE_RELEASE, "Gate Release" );
	SetParamInfo( PT_GATE_MODE, "Gate Mutes Beam", FF_TYPE_BOOLEAN, false );

	// -- Compressor ----------------------------------------------------------
	SetParamInfo( PT_COMP_ON, "Compressor", FF_TYPE_BOOLEAN, false );
	standard( this, PT_COMP_THRESHOLD, "Threshold" );
	standard( this, PT_COMP_RATIO, "Ratio" );
	standard( this, PT_COMP_KNEE, "Knee" );
	standard( this, PT_COMP_ATTACK, "Attack" );
	standard( this, PT_COMP_RELEASE, "Release" );
	standard( this, PT_COMP_MAKEUP, "Makeup" );
	SetParamInfo( PT_COMP_AUTO, "Auto Makeup", FF_TYPE_BOOLEAN, true );
	standard( this, PT_COMP_CEILING, "Ceiling" );
	SetParamInfo( PT_COMP_LIMIT_MODE, "Rail Limiting", FF_TYPE_BOOLEAN, false );
	routingParam( PT_COMP_ROUTING, "Comp Routing", 0 );

	// -- Rectifier -----------------------------------------------------------
	SetParamInfo( PT_RECT_ON, "Rectifier", FF_TYPE_BOOLEAN, false );
	SetParamInfo( PT_RECT_MODE, "Full Wave", FF_TYPE_BOOLEAN, false );
	standard( this, PT_RECT_BIAS, "Fold Point" );
	routingParam( PT_RECT_ROUTING, "Rectifier Routing", 1 );

	// -- Slew ----------------------------------------------------------------
	SetParamInfo( PT_SLEW_ON, "Slew Limiter", FF_TYPE_BOOLEAN, false );
	standard( this, PT_SLEW_RISE, "Rise" );
	standard( this, PT_SLEW_FALL, "Fall" );
	SetParamInfo( PT_SLEW_LINK, "Link Rise/Fall", FF_TYPE_BOOLEAN, true );
	routingParam( PT_SLEW_ROUTING, "Slew Routing", 0 );

	// -- Drive ---------------------------------------------------------------
	SetParamInfo( PT_DRIVE_ON, "Drive", FF_TYPE_BOOLEAN, false );
	standard( this, PT_DRIVE_AMOUNT, "Gain" );
	standard( this, PT_DRIVE_FOLD, "Fold" );
	SetParamInfo( PT_DRIVE_FOLDS, "Folds", FF_TYPE_INTEGER, 2.0f );
	SetParamRange( PT_DRIVE_FOLDS, 1.0f, 8.0f );
	SetParamInfo( PT_DRIVE_OVERSAMPLE, "Oversample", FF_TYPE_BOOLEAN, true );
	routingParam( PT_DRIVE_ROUTING, "Drive Routing", 0 );

	// -- Ring modulator ------------------------------------------------------
	SetParamInfo( PT_RING_ON, "Ring Mod", FF_TYPE_BOOLEAN, false );
	standard( this, PT_RING_FREQ, "Carrier" );
	SetOptionParamInfo( PT_RING_WAVE, "Carrier Wave", 3, params[ PT_RING_WAVE ] );
	SetParamElementInfo( PT_RING_WAVE, 0, "Sine", 0.0f );
	SetParamElementInfo( PT_RING_WAVE, 1, "Triangle", 1.0f );
	SetParamElementInfo( PT_RING_WAVE, 2, "Square", 2.0f );
	standard( this, PT_RING_DEPTH, "Ring Depth" );
	SetParamInfo( PT_RING_LOCK, "Ratio Lock", FF_TYPE_BOOLEAN, false );
	routingParam( PT_RING_ROUTING, "Ring Routing", 5 );

	// -- Bitcrush ------------------------------------------------------------
	SetParamInfo( PT_CRUSH_ON, "Bitcrush", FF_TYPE_BOOLEAN, false );
	SetParamInfo( PT_CRUSH_BITS, "Bits", FF_TYPE_INTEGER, 16.0f );
	SetParamRange( PT_CRUSH_BITS, 1.0f, 16.0f );
	standard( this, PT_CRUSH_RATE, "Sample Rate" );
	routingParam( PT_CRUSH_ROUTING, "Crush Routing", 0 );

	// -- Phaser --------------------------------------------------------------
	SetParamInfo( PT_PHASE_ON, "Phaser", FF_TYPE_BOOLEAN, false );
	SetParamInfo( PT_PHASE_STAGES, "Stages", FF_TYPE_INTEGER, 4.0f );
	SetParamRange( PT_PHASE_STAGES, 1.0f, 12.0f );
	standard( this, PT_PHASE_RATE, "Phaser Rate" );
	standard( this, PT_PHASE_DEPTH, "Phaser Depth" );
	standard( this, PT_PHASE_CENTRE, "Centre" );
	standard( this, PT_PHASE_FEEDBACK, "Phaser Feedback" );
	standard( this, PT_PHASE_MIX, "Phaser Mix" );
	routingParam( PT_PHASE_ROUTING, "Phaser Routing", 1 );

	// -- Flanger -------------------------------------------------------------
	SetParamInfo( PT_FLANGE_ON, "Flanger", FF_TYPE_BOOLEAN, false );
	standard( this, PT_FLANGE_RATE, "Flanger Rate" );
	standard( this, PT_FLANGE_DEPTH, "Flanger Depth" );
	standard( this, PT_FLANGE_DELAY, "Flanger Delay" );
	standard( this, PT_FLANGE_FEEDBACK, "Flanger Feedback" );
	standard( this, PT_FLANGE_MIX, "Flanger Mix" );
	routingParam( PT_FLANGE_ROUTING, "Flanger Routing", 0 );

	// -- Chorus --------------------------------------------------------------
	SetParamInfo( PT_CHORUS_ON, "Chorus", FF_TYPE_BOOLEAN, false );
	standard( this, PT_CHORUS_RATE, "Chorus Rate" );
	standard( this, PT_CHORUS_DEPTH, "Chorus Depth" );
	standard( this, PT_CHORUS_DELAY, "Chorus Delay" );
	standard( this, PT_CHORUS_MIX, "Chorus Mix" );

	// -- Delay ---------------------------------------------------------------
	SetParamInfo( PT_DELAY_ON, "Delay", FF_TYPE_BOOLEAN, false );
	SetParamInfo( PT_DELAY_SYNC, "Sync to Tempo", FF_TYPE_BOOLEAN, false );
	standard( this, PT_DELAY_TIME, "Delay Time" );
	SetOptionParamInfo( PT_DELAY_DIVISION, "Division", kDelayDivisionCount, params[ PT_DELAY_DIVISION ] );
	for( int i = 0; i < kDelayDivisionCount; ++i )
		SetParamElementInfo( PT_DELAY_DIVISION, i, kDelayDivisionNames[ i ], static_cast< float >( i ) );
	standard( this, PT_DELAY_FEEDBACK, "Delay Feedback" );
	standard( this, PT_DELAY_DAMP_LOW, "Delay Tone" );
	standard( this, PT_DELAY_DAMP_HIGH, "Delay Bass Cut" );
	standard( this, PT_DELAY_MIX, "Delay Mix" );
	routingParam( PT_DELAY_ROUTING, "Delay Routing", 0 );
	SetParamInfo( PT_DELAY_TIME_MODE, "Crossfade Time Changes", FF_TYPE_BOOLEAN, false );

	// -- Reverb --------------------------------------------------------------
	SetParamInfo( PT_VERB_ON, "Reverb", FF_TYPE_BOOLEAN, false );
	standard( this, PT_VERB_PREDELAY, "Pre-Delay" );
	standard( this, PT_VERB_DECAY, "Decay" );
	standard( this, PT_VERB_DAMPING, "Damping" );
	standard( this, PT_VERB_DIFFUSION, "Diffusion" );
	standard( this, PT_VERB_SIZE, "Size" );
	standard( this, PT_VERB_MOD, "Shimmer" );
	standard( this, PT_VERB_MIX, "Reverb Mix" );

	// -- Deflection amplifier ------------------------------------------------
	standard( this, PT_OUT_GAIN, "Deflection" );
	standard( this, PT_OUT_OFFSET_X, "Offset X" );
	standard( this, PT_OUT_OFFSET_Y, "Offset Y" );
	standard( this, PT_OUT_ROTATION, "Rotation" );
	standard( this, PT_OUT_SKEW, "Skew" );
	SetParamInfo( PT_OUT_DC, "DC Block", FF_TYPE_BOOLEAN, true );
	standard( this, PT_OUT_SLEW, "Amp Slew" );
	standard( this, PT_OUT_BANDWIDTH_X, "Bandwidth X" );
	standard( this, PT_OUT_BANDWIDTH_Y, "Bandwidth Y" );
	standard( this, PT_OUT_RESONANCE, "Damping Factor" );

	// -- Beam ----------------------------------------------------------------
	standard( this, PT_BEAM_POWER, "Beam" );
	standard( this, PT_BEAM_FOCUS, "Focus" );
	standard( this, PT_BEAM_DEFOCUS, "Blooming" );
	standard( this, PT_BLANK_FLOOR, "Blanking Leak" );

	// -- Tube ----------------------------------------------------------------
	SetOptionParamInfo( PT_PHOSPHOR, "Phosphor", kPhosphorCount, params[ PT_PHOSPHOR ] );
	for( int i = 0; i < kPhosphorCount; ++i )
		SetParamElementInfo( PT_PHOSPHOR, i, kPhosphorNames[ i ], static_cast< float >( i ) );
	standard( this, PT_PERSISTENCE, "Persistence" );
	standard( this, PT_HALATION, "Halation" );
	standard( this, PT_HALATION_RADIUS, "Halation Radius" );
	standard( this, PT_FACE_ASPECT, "Face Aspect" );
	standard( this, PT_CORNER_RADIUS, "Corner Radius" );
	standard( this, PT_DEFLECTION_GAIN, "Screen Size" );
	standard( this, PT_CURVATURE, "Curvature" );
	standard( this, PT_VIGNETTE, "Vignette" );
	standard( this, PT_FACE_BLACK, "Face Black" );
	standard( this, PT_FILTER_TINT, "Contrast Filter" );
	standard( this, PT_GRATICULE, "Graticule" );

	// -- Output --------------------------------------------------------------
	standard( this, PT_MIX, "Mix" );

	//Factory presets. Element 0 is Custom; picking anything else copies that
	//preset's values into the covered parameters and raises value events so the
	//host re-reads its sliders.
	SetOptionParamInfo( PT_PRESET, "Preset", 1 + presets::kCount, params[ PT_PRESET ] );
	SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
	for( int i = 0; i < presets::kCount; ++i )
		SetParamElementInfo( PT_PRESET, 1 + i, presets::kPresets[ i ].name, static_cast< float >( 1 + i ) );

	// -- Modulation ----------------------------------------------------------
	//
	//Appended after PT_PRESET and given its own group, per the fleet's rule: a
	//released id may never be renumbered, and a lone run sharing an existing
	//group's name renders as a duplicate group header.
	SetBufferParamInfo( PT_AUDIO_FFT, "Audio", kAudioBins, FF_USAGE_FFT );
	for( int i = 0; i < kAudioBins; ++i )
		SetParamElementInfo( PT_AUDIO_FFT, i, "", 0.0f );

	standard( this, PT_LFO1_RATE, "LFO 1 Rate" );
	standard( this, PT_LFO1_DEPTH, "LFO 1 Depth" );
	standard( this, PT_LFO2_RATE, "LFO 2 Rate" );
	standard( this, PT_LFO2_DEPTH, "LFO 2 Depth" );

	const unsigned int modTarget[ 4 ] = { PT_MOD1_TARGET, PT_MOD2_TARGET, PT_MOD3_TARGET, PT_MOD4_TARGET };
	const unsigned int modAmount[ 4 ] = { PT_MOD1_AMOUNT, PT_MOD2_AMOUNT, PT_MOD3_AMOUNT, PT_MOD4_AMOUNT };
	const unsigned int modBand[ 4 ]   = { PT_MOD1_BAND, PT_MOD2_BAND, PT_MOD3_BAND, PT_MOD4_BAND };
	const char* const slotName[ 4 ]   = { "Mod 1", "Mod 2", "Mod 3", "Mod 4" };
	const char* const amountName[ 4 ] = { "Mod 1 Amount", "Mod 2 Amount", "Mod 3 Amount", "Mod 4 Amount" };
	const char* const bandName[ 4 ]   = { "Mod 1 Band", "Mod 2 Band", "Mod 3 Band", "Mod 4 Band" };

	for( int slot = 0; slot < 4; ++slot )
	{
		SetOptionParamInfo( modTarget[ slot ], slotName[ slot ], kModTargetCount, params[ modTarget[ slot ] ] );
		for( int i = 0; i < kModTargetCount; ++i )
			SetParamElementInfo( modTarget[ slot ], i, kModTargetNames[ i ], static_cast< float >( i ) );
		standard( this, modAmount[ slot ], amountName[ slot ] );
		SetOptionParamInfo( modBand[ slot ], bandName[ slot ], kBandCount, params[ modBand[ slot ] ] );
		for( int i = 0; i < kBandCount; ++i )
			SetParamElementInfo( modBand[ slot ], i, kBandNames[ i ], static_cast< float >( i ) );
	}

	// -- About ---------------------------------------------------------------
	//
	//Declared inline rather than through a helper: SetParamInfo is protected on
	//CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_TEXT, "About", FF_TYPE_TEXT, "" );
	{
		FFUInt32 aboutId = PT_ABOUT_TEXT + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}

	// -- Groups --------------------------------------------------------------
	//
	//SetParamGroup collapses RUNS of consecutive ids, so each of these names the
	//first id of its run and the enum's order is what actually defines the panel.
	SetParamGroup( PT_DETAIL, "Clock" );
	SetParamGroup( PT_SOURCE, "Source" );
	SetParamGroup( PT_WAVE_X, "Oscillator" );
	SetParamGroup( PT_SHAPE, "Shape" );
	SetParamGroup( PT_MESH, "Wireframe" );
	SetParamGroup( PT_REFRESH, "Beam Path" );
	SetParamGroup( PT_FILE, "Audio File" );
	SetParamGroup( PT_TRACE_THRESHOLD, "Trace" );
	SetParamGroup( PT_VCA_ON, "VCA" );
	SetParamGroup( PT_GATE_ON, "Gate" );
	SetParamGroup( PT_COMP_ON, "Compressor" );
	SetParamGroup( PT_RECT_ON, "Rectifier" );
	SetParamGroup( PT_SLEW_ON, "Slew" );
	SetParamGroup( PT_DRIVE_ON, "Drive" );
	SetParamGroup( PT_RING_ON, "Ring Modulator" );
	SetParamGroup( PT_CRUSH_ON, "Bitcrush" );
	SetParamGroup( PT_PHASE_ON, "Phaser" );
	SetParamGroup( PT_FLANGE_ON, "Flanger" );
	SetParamGroup( PT_CHORUS_ON, "Chorus" );
	SetParamGroup( PT_DELAY_ON, "Delay" );
	SetParamGroup( PT_VERB_ON, "Reverb" );
	SetParamGroup( PT_OUT_GAIN, "Deflection Amplifier" );
	SetParamGroup( PT_BEAM_POWER, "Beam" );
	SetParamGroup( PT_PHOSPHOR, "Tube" );
	SetParamGroup( PT_MIX, "Output" );
	SetParamGroup( PT_AUDIO_FFT, "Modulation" );
	SetParamGroup( PT_ABOUT_TEXT, "About" );
}

//---------------------------------------------------------------------------
// Parameters
//---------------------------------------------------------------------------

FFResult VectrixPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	params[ index ] = value;

	if( index == PT_PRESET )
	{
		const int chosen = static_cast< int >( std::lround( value ) );
		if( chosen > 0 )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	if( index == PT_RESET && value > 0.5f )
	{
		clock.Reset();
		engine.Reset();
		return FF_SUCCESS;
	}

	//An About button is a press, not a value to keep: it opens a browser.
	if( index > PT_ABOUT_TEXT && index < PT_COUNT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;

	//A slider moved while a preset is active means the operator has taken over,
	//so the dropdown goes back to Custom. Compared by value, because hosts that
	//honour the value events raised by applyPreset echo the preset's own numbers
	//straight back through here -- and that echo must not un-set the preset.
	if( params[ PT_PRESET ] > 0.5f )
	{
		const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
		if( active > 0 && active <= presets::kCount )
		{
			const presets::Preset& preset = presets::kPresets[ active - 1 ];
			for( int j = 0; j < presets::kParamCount; ++j )
			{
				if( kPresetParamIDs[ j ] != index )
					continue;
				if( std::fabs( value - preset.v[ j ] ) > 1e-6f )
				{
					params[ PT_PRESET ] = 0.0f;
					RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
				}
				break;
			}
		}
	}

	return FF_SUCCESS;
}

void VectrixPlugin::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = static_cast< float >( presetIndex );

	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return;

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		//The copy is what changes the picture; the event only tells the host to
		//re-read the slider. A host that ignores it renders the preset correctly
		//and merely shows stale knobs.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

float VectrixPlugin::GetFloatParameter( unsigned int index )
{
	return index < PT_COUNT ? params[ index ] : 0.0f;
}

FFResult VectrixPlugin::SetTextParameter( unsigned int index, const char* value )
{
	if( index == PT_FILE )
	{
		std::lock_guard< std::mutex > lock( textMutex );
		filePath     = value != nullptr ? value : "";
		contentDirty = true;
		return FF_SUCCESS;
	}

	//The About block is display-only, and returning FF_SUCCESS here is not
	//politeness. The SDK's instantiateGL sets EVERY parameter's default on a
	//fresh instance and deletes the instance if any set returns FF_FAIL -- and
	//the base class's SetTextParameter is a stub that returns FF_FAIL. Without
	//this branch no real host can instantiate the plugin at all, while every
	//offline harness that calls the class directly passes happily.
	if( index == PT_ABOUT_TEXT )
		return FF_SUCCESS;

	return FF_FAIL;
}

char* VectrixPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_FILE )
	{
		std::lock_guard< std::mutex > lock( textMutex );
		textReturn[ 0 ]     = '\0';
		const size_t length = std::min( filePath.size(), sizeof( textReturn ) - 1 );
		std::memcpy( textReturn, filePath.data(), length );
		textReturn[ length ] = '\0';
		return textReturn;
	}

	if( index == PT_ABOUT_TEXT )
	{
		//The host is handed a bare pointer, so the string is kept as a member
		//rather than built on the stack here.
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult VectrixPlugin::SetTime( double time )
{
	lastHostTime = time;
	return CFFGLPlugin::SetTime( time );
}

//---------------------------------------------------------------------------
// GL
//---------------------------------------------------------------------------

FFResult VectrixPlugin::InitGL( const FFGLViewportStruct* )
{
	engine.Prepare( static_cast< Detail >( Option( params[ PT_DETAIL ], kDetailCount ) ) );

	if( !beam.InitGL() )
	{
		diag::error( "the beam renderer would not initialise" );
		return FF_FAIL;
	}

	if( overInput && !engine.Trace().InitGL() )
	{
		//Not fatal: the trace source is one of five, and the other four do not
		//need it. Losing it should cost the trace, not the plugin.
		diag::error( "the trace source would not initialise; that source will be blank" );
	}

	glReady = true;
	return FF_SUCCESS;
}

FFResult VectrixPlugin::DeInitGL()
{
	beam.DeInitGL();
	engine.Trace().DeInitGL();
	glReady = false;
	return FF_SUCCESS;
}

void VectrixPlugin::updateAudio()
{
	const ParamInfo* info = FindParamInfo( PT_AUDIO_FFT );
	if( info == nullptr )
		return;

	float bins[ kAudioBins ] = {};
	const size_t count       = std::min< size_t >( info->elements.size(), kAudioBins );
	for( size_t i = 0; i < count; ++i )
		bins[ i ] = info->elements[ i ].value;

	modulation.Update( bins, static_cast< int >( count ), clock.FrameSeconds() );
	modulation.UpdateTransport( bpm, barPhase, clock.Now() );

	modulation.SetLfo( 0, Exponential( params[ PT_LFO1_RATE ], 0.01f, 20.0f ), params[ PT_LFO1_DEPTH ] );
	modulation.SetLfo( 1, Exponential( params[ PT_LFO2_RATE ], 0.01f, 20.0f ), params[ PT_LFO2_DEPTH ] );

	const unsigned int target[ 4 ] = { PT_MOD1_TARGET, PT_MOD2_TARGET, PT_MOD3_TARGET, PT_MOD4_TARGET };
	const unsigned int amount[ 4 ] = { PT_MOD1_AMOUNT, PT_MOD2_AMOUNT, PT_MOD3_AMOUNT, PT_MOD4_AMOUNT };
	const unsigned int band[ 4 ]   = { PT_MOD1_BAND, PT_MOD2_BAND, PT_MOD3_BAND, PT_MOD4_BAND };

	for( int slot = 0; slot < 4; ++slot )
	{
		ModSlot s;
		s.target = static_cast< ModTarget >( Option( params[ target[ slot ] ], kModTargetCount ) );
		//Centred: the amount slider runs -1..1 so a slot can subtract, and 0.5
		//is off. A 0..1 amount would make "no modulation" an end stop, which is
		//the wrong place for a default.
		s.amount = Linear( params[ amount[ slot ] ], -1.0f, 1.0f );
		s.band   = static_cast< Band >( Option( params[ band[ slot ] ], kBandCount ) );
		modulation.SetSlot( slot, s );
	}
}

BeamGeometry::RenderParams VectrixPlugin::renderParams( const Resolved& resolved, double frameSeconds ) const
{
	BeamGeometry::RenderParams rp;

	//Exponential around a calibrated 1.0, the same shape resolume-scopes uses
	//for its trace gain and for the same reason: the useful range is wide.
	//
	//The linear fade over the bottom of the travel is not cosmetic. An
	//exponential alone bottoms out at 0.1 -- a tenth of nominal, never zero --
	//so "Beam 0" would leave the gun on, and the effect build's exact
	//passthrough would be unreachable through the parameter list however
	//correct the faceplate maths was. `vxtest --identity` caught precisely
	//that, and it is the reason this control has a genuine off position.
	const float beam = std::clamp( params[ PT_BEAM_POWER ] + modulation.For( ModTarget::BeamPower ),
	                               0.0f, 1.0f );
	rp.beamPower = std::pow( 10.0f, -1.0f + 2.0f * beam )
	               * std::min( 1.0f, beam * ( 1.0f / kBeamOffRamp ) );
	rp.spotSigma = Exponential( params[ PT_BEAM_FOCUS ], 0.0012f, 0.02f );
	rp.spotDefocus = Linear( params[ PT_BEAM_DEFOCUS ], 0.0f, 2.0f );
	rp.blankFloor  = Linear( params[ PT_BLANK_FLOOR ], 0.0f, 0.1f );

	rp.phosphor    = Option( params[ PT_PHOSPHOR ], kPhosphorCount );
	//Through Phosphor.h's own curve rather than a second copy of it here: that
	//header is where the decade range and the unity point are decided, and a
	//duplicate mapping is how the control and the table drift apart.
	rp.persistence = persistenceMultiplier( std::clamp( params[ PT_PERSISTENCE ]
	                                                    + modulation.For( ModTarget::Persistence ),
	                                                    0.0f, 1.0f ) );

	//0..4, not 0..1. The halo is the trace convolved with a wide Gaussian, so it
	//is inherently far dimmer than what cast it -- and a thin trace is dim to
	//begin with. At unity gain the control moved 124 subpixels of a 2-megapixel
	//frame by one part in 255, which is a control that technically works and is
	//useless. Values above 1 are not a cheat: they stand for more scattering
	//than one pass through the faceplate, which is what a thick tube does.
	rp.halation       = Linear( params[ PT_HALATION ], 0.0f, 4.0f );
	rp.halationRadius = std::clamp( params[ PT_HALATION_RADIUS ], 0.0f, 1.0f );

	//The bright pass's knee, and it has to be set here rather than left at the
	//header's default.
	//
	//A moving trace is nowhere near as bright as a stationary beam: `--point`
	//measures a parked beam emitting 0.92, and a figure traced twice a frame is
	//a small fraction of that. A knee at 0.5 therefore found nothing above it on
	//any ordinary picture, the bloom buffer stayed empty, and BOTH halation
	//controls were stone dead while every line of the bloom chain worked
	//perfectly -- which `tools/sweep.py` caught and nothing else would have.
	//
	//Low enough that a normal trace scatters, high enough that the graticule and
	//a blanked retrace do not.
	rp.halationThreshold = 0.04f;

	rp.tube.faceAspect     = Exponential( params[ PT_FACE_ASPECT ], 1.0f, 2.0f );
	rp.tube.cornerRadius   = std::clamp( params[ PT_CORNER_RADIUS ], 0.0f, 1.0f );
	rp.tube.deflectionGain = Linear( params[ PT_DEFLECTION_GAIN ], 0.4f, 1.4f );
	rp.tube.curvature      = Linear( params[ PT_CURVATURE ], 0.0f, 0.6f );
	rp.tube.vignette       = std::clamp( params[ PT_VIGNETTE ], 0.0f, 1.0f );

	rp.graticule = std::clamp( params[ PT_GRATICULE ], 0.0f, 1.0f );
	rp.faceBlack = std::clamp( params[ PT_FACE_BLACK ], 0.0f, 1.0f );
	rp.opacity   = resolved.mix;

	//The contrast filter, interpolated between clear glass and a filter the
	//colour of the phosphor -- which is how a real scope's filter kills ambient
	//light without killing the trace.
	const float tint = std::clamp( params[ PT_FILTER_TINT ], 0.0f, 1.0f );
	for( int c = 0; c < 3; ++c )
		rp.filterTransmission[ c ] = 1.0f - tint * ( 1.0f - rp.filterTransmission[ c ] );

	rp.frameSeconds = static_cast< float >( frameSeconds );

	return rp;
}

FFResult VectrixPlugin::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( !glReady || pGL == nullptr )
		return FF_FAIL;

	//The host's viewport, read fresh rather than remembered from InitGL:
	//Resolume changes composition resolution without reinitialising a plugin.
	GLint viewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, viewport );
	if( viewport[ 2 ] <= 0 || viewport[ 3 ] <= 0 )
		return FF_FAIL;

	GLuint clipTexture = 0;
	float maxU         = 1.0f;
	float maxV         = 1.0f;

	if( overInput )
	{
		if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
			return FF_FAIL;
		const FFGLTextureStruct& texture = *pGL->inputTextures[ 0 ];
		clipTexture                      = texture.Handle;
		const FFGLTexCoords coords       = GetMaxGLTexCoords( texture );
		maxU                             = coords.s;
		maxV                             = coords.t;
	}

	clock.Update( lastHostTime );
	updateAudio();

	const Resolved resolved = Resolve( params, modulation, static_cast< double >( bpm ) );
	engine.SetParams( resolved );

	if( contentDirty )
	{
		std::string wanted;
		{
			std::lock_guard< std::mutex > lock( textMutex );
			wanted = filePath;
		}
		engine.File().SetPath( wanted );
		engine.SwapContent();
		contentDirty = false;

		if( !engine.File().Note().empty() )
			diag::info( "audio file: " + engine.File().Note() );
	}

	//The trace source needs this frame's strokes before the engine renders, and
	//it is the only part of the signal path that touches GL.
	if( overInput && resolved.source == SourceKind::Trace )
		engine.Trace().Update( clipTexture, maxU, maxV );

	engine.Advance( clock.FrameSeconds(), clock.Now() );

	const int n           = clock.SamplesForThisFrame( engine.SampleRate() );
	const Sample* samples = engine.Render( n, clock.FrameSeconds() );

	const bool drawn = beam.Render( samples, n, renderParams( resolved, clock.FrameSeconds() ),
	                                pGL->HostFBO, viewport, clipTexture, maxU, maxV );

	if( ++frameCounter == 60 )
	{
		//Logged once, because it settles an argument about which unit a host
		//actually sent -- and no offline harness can answer it, since the
		//harness is the thing sending seconds.
		diag::info( std::string( "host clock scale " )
		            + ( clock.ClockScale() == 0.001 ? "0.001 (milliseconds)" : "1.0 (seconds)" ) );
	}

	return drawn ? FF_SUCCESS : FF_FAIL;
}

} // namespace vectrix
