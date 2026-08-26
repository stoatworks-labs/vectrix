#pragma once

#include "signal/Modulation.h"
#include "signal/Signal.h"
#include "signal/fx/Chain.h"
#include "signal/sources/AudioFile.h"
#include "signal/sources/Oscillator.h"
#include "signal/sources/Shapes.h"
#include "signal/sources/Trace.h"
#include "signal/sources/Wire.h"

namespace vectrix
{
/**
	Parameter ids.

	**The declaration order in Vectrix.cpp is the order the host shows them, and
	`SetParamGroup` collapses *runs* of consecutive ids into one group.** So
	reordering this enum does not merely rearrange the panel -- it silently
	splits a group in two, or merges two into one, and the result renders as a
	duplicated group header that reads as a bug.

	**Never renumber a released id.** A saved composition refers to parameters by
	number, so anything added after v0.1.0 goes on the end, after PT_PRESET, with
	its own group name.
*/
enum ParamId : unsigned int
{
	// -- Clock ---------------------------------------------------------------
	PT_DETAIL = 0,
	PT_RESET,
	PT_SEED,

	// -- Source --------------------------------------------------------------
	PT_SOURCE,

	// -- Oscillator ----------------------------------------------------------
	PT_WAVE_X,
	PT_WAVE_Y,
	PT_FREQ_X,
	PT_RATIO,
	PT_FREE_Y,
	PT_FREQ_Y,
	PT_PHASE_Y,
	PT_PWM_X,
	PT_PWM_Y,
	PT_DETUNE,
	PT_HARD_SYNC,
	PT_BLANK_RETRACE,

	// -- Shape ---------------------------------------------------------------
	PT_SHAPE,
	PT_SHAPE_RATE,
	PT_SHAPE_N,
	PT_SHAPE_INNER,
	PT_PETAL_N,
	PT_PETAL_D,
	PT_TROCHOID,

	// -- Wireframe -----------------------------------------------------------
	PT_MESH,
	PT_MESH_DETAIL,
	PT_SPIN_X,
	PT_SPIN_Y,
	PT_SPIN_Z,
	PT_CAMERA,
	PT_SCROLL,

	// -- Beam path (shared by wireframe and trace) ---------------------------
	PT_REFRESH,
	PT_RETRACE,

	// -- Audio file ----------------------------------------------------------
	PT_FILE,
	PT_FILE_RATE,
	PT_FILE_LOOP,
	PT_FILE_START,
	PT_FILE_END,
	PT_FILE_MONO,
	PT_FILE_SWAP,
	PT_FILE_INVERT,
	PT_FILE_SYNC,

	// -- Trace ---------------------------------------------------------------
	PT_TRACE_THRESHOLD,
	PT_TRACE_STABILITY,
	PT_TRACE_SIMPLIFY,
	PT_TRACE_STROKES,

	// -- VCA -----------------------------------------------------------------
	PT_VCA_ON,
	PT_VCA_LEVEL,
	PT_VCA_ROUTING,

	// -- Gate ----------------------------------------------------------------
	PT_GATE_ON,
	PT_GATE_THRESHOLD,
	PT_GATE_ATTACK,
	PT_GATE_HOLD,
	PT_GATE_RELEASE,
	PT_GATE_MODE,

	// -- Compressor ----------------------------------------------------------
	PT_COMP_ON,
	PT_COMP_THRESHOLD,
	PT_COMP_RATIO,
	PT_COMP_KNEE,
	PT_COMP_ATTACK,
	PT_COMP_RELEASE,
	PT_COMP_MAKEUP,
	PT_COMP_AUTO,
	PT_COMP_CEILING,
	PT_COMP_LIMIT_MODE,
	PT_COMP_ROUTING,

	// -- Rectifier -----------------------------------------------------------
	PT_RECT_ON,
	PT_RECT_MODE,
	PT_RECT_BIAS,
	PT_RECT_ROUTING,

	// -- Slew ----------------------------------------------------------------
	PT_SLEW_ON,
	PT_SLEW_RISE,
	PT_SLEW_FALL,
	PT_SLEW_LINK,
	PT_SLEW_ROUTING,

	// -- Drive / fold --------------------------------------------------------
	PT_DRIVE_ON,
	PT_DRIVE_AMOUNT,
	PT_DRIVE_FOLD,
	PT_DRIVE_FOLDS,
	PT_DRIVE_OVERSAMPLE,
	PT_DRIVE_ROUTING,

	// -- Ring modulator ------------------------------------------------------
	PT_RING_ON,
	PT_RING_FREQ,
	PT_RING_WAVE,
	PT_RING_DEPTH,
	PT_RING_LOCK,
	PT_RING_ROUTING,

	// -- Bitcrush ------------------------------------------------------------
	PT_CRUSH_ON,
	PT_CRUSH_BITS,
	PT_CRUSH_RATE,
	PT_CRUSH_ROUTING,

	// -- Phaser --------------------------------------------------------------
	PT_PHASE_ON,
	PT_PHASE_STAGES,
	PT_PHASE_RATE,
	PT_PHASE_DEPTH,
	PT_PHASE_CENTRE,
	PT_PHASE_FEEDBACK,
	PT_PHASE_MIX,
	PT_PHASE_ROUTING,

	// -- Flanger -------------------------------------------------------------
	PT_FLANGE_ON,
	PT_FLANGE_RATE,
	PT_FLANGE_DEPTH,
	PT_FLANGE_DELAY,
	PT_FLANGE_FEEDBACK,
	PT_FLANGE_MIX,
	PT_FLANGE_ROUTING,

	// -- Chorus --------------------------------------------------------------
	PT_CHORUS_ON,
	PT_CHORUS_RATE,
	PT_CHORUS_DEPTH,
	PT_CHORUS_DELAY,
	PT_CHORUS_MIX,

	// -- Delay ---------------------------------------------------------------
	PT_DELAY_ON,
	PT_DELAY_SYNC,
	PT_DELAY_TIME,
	PT_DELAY_DIVISION,
	PT_DELAY_FEEDBACK,
	PT_DELAY_DAMP_LOW,
	PT_DELAY_DAMP_HIGH,
	PT_DELAY_MIX,
	PT_DELAY_ROUTING,
	PT_DELAY_TIME_MODE,

	// -- Reverb --------------------------------------------------------------
	PT_VERB_ON,
	PT_VERB_PREDELAY,
	PT_VERB_DECAY,
	PT_VERB_DAMPING,
	PT_VERB_DIFFUSION,
	PT_VERB_SIZE,
	PT_VERB_MOD,
	PT_VERB_MIX,

	// -- Deflection amplifier ------------------------------------------------
	PT_OUT_GAIN,
	PT_OUT_OFFSET_X,
	PT_OUT_OFFSET_Y,
	PT_OUT_ROTATION,
	PT_OUT_SKEW,
	PT_OUT_DC,
	PT_OUT_SLEW,
	PT_OUT_BANDWIDTH_X,
	PT_OUT_BANDWIDTH_Y,
	PT_OUT_RESONANCE,

	// -- Beam ----------------------------------------------------------------
	PT_BEAM_POWER,
	PT_BEAM_FOCUS,
	PT_BEAM_DEFOCUS,
	PT_BLANK_FLOOR,

	// -- Tube ----------------------------------------------------------------
	PT_PHOSPHOR,
	PT_PERSISTENCE,
	PT_HALATION,
	PT_HALATION_RADIUS,
	PT_FACE_ASPECT,
	PT_CORNER_RADIUS,
	PT_DEFLECTION_GAIN,
	PT_CURVATURE,
	PT_VIGNETTE,
	PT_FACE_BLACK,
	PT_FILTER_TINT,
	PT_GRATICULE,

	// -- Output --------------------------------------------------------------
	PT_MIX,

	// -- Modulation (appended: its own group, per the fleet's append rule) ----
	PT_AUDIO_FFT,
	PT_LFO1_RATE,
	PT_LFO1_DEPTH,
	PT_LFO2_RATE,
	PT_LFO2_DEPTH,
	PT_MOD1_TARGET,
	PT_MOD1_AMOUNT,
	PT_MOD1_BAND,
	PT_MOD2_TARGET,
	PT_MOD2_AMOUNT,
	PT_MOD2_BAND,
	PT_MOD3_TARGET,
	PT_MOD3_AMOUNT,
	PT_MOD3_BAND,
	PT_MOD4_TARGET,
	PT_MOD4_AMOUNT,
	PT_MOD4_BAND,

	// -- Preset --------------------------------------------------------------
	//
	// Last, and its own group, so it is where every other plugin in the fleet
	// puts it. It used to sit inside Output with the seventeen Modulation
	// controls after it, which is a long way to scroll for the control most
	// likely to be wanted first (#7).
	PT_PRESET,

	// -- The Stoatworks About block ------------------------------------------
	//
	// One display-only text line followed by one button per link. The number of
	// buttons is decided by which URLs the branding header actually carries, so
	// Vectrix.cpp static_asserts this run against `about::kParamCount` -- adding
	// a user-guide URL later would otherwise silently shift PT_COUNT and leave
	// the last button undeclared.
	PT_ABOUT_TEXT,
	PT_ABOUT_BUTTON_1,
	PT_ABOUT_BUTTON_2,
	PT_ABOUT_BUTTON_3,
	PT_ABOUT_BUTTON_4,

	PT_COUNT
};

/// Option-parameter element counts, so the declaration and the reader cannot
/// disagree about how many entries a dropdown has.
constexpr int kDetailCount   = static_cast< int >( Detail::Count );
constexpr int kSourceCount   = static_cast< int >( SourceKind::Count );
constexpr int kWaveCount     = static_cast< int >( Wave::Count );
constexpr int kShapeCount    = static_cast< int >( ShapeKind::Count );
constexpr int kMeshCount     = static_cast< int >( MeshKind::Count );
constexpr int kRoutingCount  = static_cast< int >( Routing::Count );
constexpr int kBandCount     = static_cast< int >( Band::Count );
constexpr int kModTargetCount = static_cast< int >( ModTarget::Count );
constexpr int kMonoModeCount = 3;
constexpr int kDelayDivisionCount = 8;
constexpr int kPhosphorCount = 6;

extern const char* const kDetailNames[ kDetailCount ];
extern const char* const kSourceNames[ kSourceCount ];
extern const char* const kWaveNames[ kWaveCount ];
extern const char* const kShapeNames[ kShapeCount ];
extern const char* const kMeshNames[ kMeshCount ];
extern const char* const kRoutingNames[ kRoutingCount ];
extern const char* const kBandNames[ kBandCount ];
extern const char* const kModTargetNames[ kModTargetCount ];
extern const char* const kMonoModeNames[ kMonoModeCount ];
extern const char* const kDelayDivisionNames[ kDelayDivisionCount ];
extern const char* const kPhosphorNames[ kPhosphorCount ];

//---------------------------------------------------------------------------
// 0..1 to engineering units.
//
// Every continuous host parameter is 0..1 and mapped here. That is not a
// stylistic choice: `CFFGLPluginManager::SetParamInfo` clamps a default into
// 0..1 *before* returning, and `SetParamRange` can only be called afterwards
// because it looks the parameter up by id. So a STANDARD parameter declared in
// hertz cannot declare a default in hertz -- 800 silently becomes 1.
//
// Integer-typed parameters are the documented exception: the clamp is guarded
// by `if( pType == FF_TYPE_STANDARD )`, so FF_TYPE_INTEGER defaults pass through
// untouched and SetParamRange then widens them. Counts use that.
//---------------------------------------------------------------------------

/// Read an option parameter's element value as an index.
int Option( float value, int count );

/// Exponential map, for anything measured in hertz or seconds where the useful
/// range spans decades and a linear slider would spend nine tenths of its travel
/// in the top octave.
float Exponential( float value, float low, float high );

/// Linear map.
float Linear( float value, float low, float high );

/// Beat divisions, as a multiple of a bar.
float DelayDivisionBars( int index );

/// Assemble every block's parameters from the raw 0..1 array plus modulation.
struct Resolved
{
	Detail detail = Detail::Normal;
	SourceKind source = SourceKind::Oscillator;
	OscillatorParams oscillator;
	ShapeParams shape;
	WireParams wire;
	AudioFileParams audioFile;
	TraceParams trace;
	PathWalker::Config walker;
	ChainParams chain;
	float mix = 1.0f;
};

Resolved Resolve( const float* params, const Modulation& mod, double bpm );

} // namespace vectrix
