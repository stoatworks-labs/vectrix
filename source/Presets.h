#pragma once

/**
	Factory presets: instruments, not slider positions that happened to look good.

	These matter more here than in any other plugin in the fleet, because vectrix
	has about a hundred and fifty controls and the honest answer to "how do I use
	this" is "start from one of these and turn one knob". Each is a whole signal
	path -- a source, a set of pedals in a particular order at particular
	settings, and a tube -- so it is a coherent *machine*, and moving one control
	on it does something you can predict.

	The values live in the same 0..1 parameter space both builds expose, so ONE
	table drives both FFGL and OpenFX and a preset looks identical in Resolume
	and in Resolve.

	Element 0 of the host-facing dropdown is "Custom" and is not in this table:
	it means the sliders are the truth.

	Presets deliberately do NOT cover Seed, Reset, the file path, Detail, or the
	tube's framing controls -- which machine you are playing is a different
	question from which room it is in and which file is loaded.
*/

namespace vectrix
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds this
/// order to its ParamIDs and the OFX build to its param handles; both
/// static_assert against kParamCount, so the three lists cannot drift apart
/// silently.
enum Param
{
	kSource,
	kWaveX,
	kWaveY,
	kFreqX,
	kRatio,
	kPhaseY,
	kBlankRetrace,
	kShape,
	kShapeRate,
	kShapeN,
	kMesh,
	kMeshDetail,
	kRefresh,

	kGateOn,
	kGateThreshold,
	kCompOn,
	kCompThreshold,
	kCompRatio,
	kCompCeiling,

	kRectOn,
	kRectRouting,
	kSlewOn,
	kSlewRise,
	kDriveOn,
	kDriveAmount,
	kDriveFold,
	kDriveFolds,
	kRingOn,
	kRingFreq,
	kRingDepth,
	kRingRouting,
	kCrushOn,
	kCrushBits,

	kPhaserOn,
	kPhaserRate,
	kPhaserMix,
	kPhaserRouting,
	kFlangeOn,
	kFlangeMix,
	kChorusOn,
	kChorusMix,
	kDelayOn,
	kDelayTime,
	kDelayFeedback,
	kDelayMix,
	kDelayRouting,
	kVerbOn,
	kVerbDecay,
	kVerbMix,

	kBeamPower,
	kBeamFocus,
	kPhosphor,
	kPersistence,
	kHalation,
	kFaceBlack,
	kGraticule,

	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

/// Option parameters are stored as their element *value* here, not as a 0..1
/// fraction, because that is what the host holds for them. Mixing the two up
/// gives a preset that silently selects entry 0 of every dropdown.
inline constexpr Preset kPresets[] = {
	//                    src  wX   wY  freqX ratio phY blank shape sRate sN mesh mDet refr | gate gThr comp cThr cRat cCeil | rect rRout slew sRise drv dAmt dFold dFolds ring rFrq rDep rRout crush bits | phs pRate pMix pRout flg fMix cho cMix dly dTime dFb dMix dRout vrb vDec vMix | beam focus phos pers halo black grat
	{ "Lissajous", { 0, 0, 0, 0.77f, 3, 0.25f, 0, 0, 0.88f, 5, 0, 8, 0.5f, 0, 0.3f, 0, 0.5f, 0.3f, 0.5f, 0, 1, 0, 1.0f, 0, 0, 0, 2, 0, 0.3f, 0, 5, 0, 16, 0, 0.2f, 0.5f, 1, 0, 0.4f, 0, 0.3f, 0, 0.5f, 0.3f, 0.3f, 0, 0, 0.4f, 0.2f, 0.45f, 0.30f, 0, 0.35f, 0.25f, 0.85f, 0.30f } },
	{ "Rosette Fold", { 0, 0, 0, 0.77f, 0, 0.25f, 0, 0, 0.88f, 5, 0, 8, 0.5f, 0, 0.3f, 1, 0.5f, 0.4f, 0.5f, 0, 1, 0, 1.0f, 1, 0.35f, 0.75f, 3, 0, 0.3f, 0, 5, 0, 16, 0, 0.2f, 0.5f, 1, 0, 0.4f, 0, 0.3f, 0, 0.5f, 0.3f, 0.3f, 0, 0, 0.4f, 0.25f, 0.50f, 0.28f, 0, 0.30f, 0.30f, 0.85f, 0.25f } },
	{ "Spring Halo", { 1, 0, 0, 0.77f, 0, 0.25f, 0, 0, 0.88f, 5, 0, 8, 0.5f, 0, 0.3f, 1, 0.4f, 0.4f, 0.5f, 0, 1, 0, 1.0f, 0, 0, 0, 2, 0, 0.3f, 0, 5, 0, 16, 0, 0.2f, 0.5f, 1, 0, 0.4f, 1, 0.45f, 0, 0.5f, 0.3f, 0.3f, 0, 1, 0.55f, 0.45f, 0.40f, 0.32f, 3, 0.55f, 0.45f, 0.85f, 0.20f } },
	{ "Tape Ghosts", { 0, 0, 0, 0.77f, 3, 0.25f, 0, 0, 0.88f, 5, 0, 8, 0.5f, 0, 0.3f, 1, 0.4f, 0.4f, 0.5f, 0, 1, 0, 1.0f, 0, 0, 0, 2, 0, 0.3f, 0, 5, 0, 16, 0, 0.2f, 0.5f, 1, 0, 0.4f, 0, 0.3f, 1, 0.55f, 0.62f, 0.55f, 6, 0, 0.4f, 0.2f, 0.42f, 0.30f, 2, 0.45f, 0.35f, 0.85f, 0.25f } },
	{ "The Rotator", { 0, 0, 0, 0.77f, 3, 0.25f, 0, 0, 0.88f, 5, 0, 8, 0.5f, 0, 0.3f, 0, 0.5f, 0.3f, 0.5f, 0, 1, 0, 1.0f, 0, 0, 0, 2, 0, 0.3f, 0, 5, 0, 16, 1, 0.25f, 1.0f, 1, 0, 0.4f, 0, 0.3f, 0, 0.5f, 0.3f, 0.3f, 0, 0, 0.4f, 0.2f, 0.45f, 0.30f, 0, 0.35f, 0.28f, 0.85f, 0.30f } },
	{ "Raster", { 0, 2, 2, 0.79f, 1, 0.0f, 1, 0, 0.88f, 5, 0, 8, 0.5f, 0, 0.3f, 0, 0.5f, 0.3f, 0.5f, 0, 1, 0, 1.0f, 0, 0, 0, 2, 0, 0.3f, 0, 5, 0, 16, 0, 0.2f, 0.5f, 1, 0, 0.4f, 0, 0.3f, 0, 0.5f, 0.3f, 0.3f, 0, 0, 0.4f, 0.2f, 0.40f, 0.28f, 0, 0.30f, 0.20f, 0.90f, 0.10f } },
	{ "Constellation", { 0, 6, 6, 0.62f, 0, 0.25f, 0, 0, 0.88f, 5, 0, 8, 0.5f, 0, 0.3f, 0, 0.5f, 0.3f, 0.5f, 0, 1, 1, 0.45f, 0, 0, 0, 2, 0, 0.3f, 0, 5, 0, 16, 0, 0.2f, 0.5f, 1, 0, 0.4f, 0, 0.3f, 0, 0.5f, 0.3f, 0.3f, 0, 0, 0.4f, 0.2f, 0.48f, 0.30f, 3, 0.50f, 0.30f, 0.85f, 0.25f } },
	{ "Mountains", { 2, 0, 0, 0.77f, 0, 0.25f, 0, 0, 0.88f, 5, 3, 14, 0.42f, 0, 0.3f, 0, 0.5f, 0.3f, 0.5f, 0, 1, 0, 1.0f, 0, 0, 0, 2, 0, 0.3f, 0, 5, 0, 16, 0, 0.2f, 0.5f, 1, 0, 0.4f, 0, 0.3f, 0, 0.5f, 0.3f, 0.3f, 0, 0, 0.4f, 0.2f, 0.52f, 0.26f, 0, 0.35f, 0.22f, 0.90f, 0.0f } },
	{ "Tunnel Run", { 2, 0, 0, 0.77f, 0, 0.25f, 0, 0, 0.88f, 5, 2, 10, 0.48f, 0, 0.3f, 0, 0.5f, 0.3f, 0.5f, 0, 1, 0, 1.0f, 0, 0, 0, 2, 0, 0.3f, 0, 5, 0, 16, 0, 0.2f, 0.5f, 1, 0, 0.4f, 0, 0.3f, 1, 0.35f, 0.35f, 0.30f, 0, 0, 0.4f, 0.2f, 0.50f, 0.28f, 0, 0.40f, 0.30f, 0.88f, 0.0f } },
	{ "Spinning Cube", { 2, 0, 0, 0.77f, 0, 0.25f, 0, 0, 0.88f, 5, 0, 8, 0.45f, 0, 0.3f, 0, 0.5f, 0.3f, 0.5f, 0, 1, 0, 1.0f, 0, 0, 0, 2, 0, 0.3f, 0, 5, 0, 16, 0, 0.2f, 0.5f, 1, 0, 0.4f, 0, 0.3f, 0, 0.5f, 0.3f, 0.3f, 0, 0, 0.4f, 0.2f, 0.48f, 0.30f, 0, 0.35f, 0.25f, 0.88f, 0.15f } },
	{ "Star", { 1, 0, 0, 0.77f, 0, 0.25f, 0, 4, 0.88f, 5, 0, 8, 0.5f, 0, 0.3f, 0, 0.5f, 0.3f, 0.5f, 0, 1, 1, 0.75f, 0, 0, 0, 2, 0, 0.3f, 0, 5, 0, 16, 0, 0.2f, 0.5f, 1, 0, 0.4f, 0, 0.3f, 0, 0.5f, 0.3f, 0.3f, 0, 0, 0.4f, 0.2f, 0.46f, 0.30f, 1, 0.40f, 0.28f, 0.85f, 0.25f } },
	{ "Scope Music", { 3, 0, 0, 0.77f, 0, 0.25f, 0, 0, 0.88f, 5, 0, 8, 0.5f, 1, 0.25f, 1, 0.35f, 0.45f, 0.6f, 0, 1, 0, 1.0f, 0, 0, 0, 2, 0, 0.3f, 0, 5, 0, 16, 0, 0.2f, 0.5f, 1, 0, 0.4f, 0, 0.3f, 0, 0.5f, 0.3f, 0.3f, 0, 0, 0.4f, 0.2f, 0.50f, 0.28f, 3, 0.50f, 0.35f, 0.90f, 0.0f } },
	{ "Fuzz Box", { 0, 0, 0, 0.77f, 5, 0.25f, 0, 0, 0.88f, 5, 0, 8, 0.5f, 0, 0.3f, 1, 0.35f, 0.55f, 0.6f, 0, 1, 0, 1.0f, 1, 0.80f, 0.35f, 4, 0, 0.3f, 0, 5, 0, 16, 0, 0.2f, 0.5f, 1, 1, 0.35f, 0, 0.3f, 0, 0.5f, 0.3f, 0.3f, 0, 0, 0.4f, 0.2f, 0.42f, 0.32f, 0, 0.35f, 0.40f, 0.85f, 0.20f } },
	{ "Ring Warp", { 1, 0, 0, 0.77f, 0, 0.25f, 0, 0, 0.88f, 5, 0, 8, 0.5f, 0, 0.3f, 0, 0.5f, 0.3f, 0.5f, 0, 1, 0, 1.0f, 0, 0, 0, 2, 1, 0.45f, 0.65f, 5, 0, 16, 0, 0.2f, 0.5f, 1, 0, 0.4f, 0, 0.3f, 0, 0.5f, 0.3f, 0.3f, 0, 1, 0.35f, 0.25f, 0.46f, 0.30f, 0, 0.35f, 0.30f, 0.85f, 0.25f } },
	{ "Vectrex", { 1, 0, 0, 0.77f, 0, 0.25f, 0, 3, 0.88f, 6, 0, 8, 0.55f, 0, 0.3f, 0, 0.5f, 0.3f, 0.5f, 0, 1, 1, 0.85f, 0, 0, 0, 2, 0, 0.3f, 0, 5, 1, 5, 0, 0.2f, 0.5f, 1, 0, 0.4f, 0, 0.3f, 0, 0.5f, 0.3f, 0.3f, 0, 0, 0.4f, 0.2f, 0.55f, 0.22f, 0, 0.30f, 0.45f, 0.95f, 0.0f } },
};

inline constexpr int kCount = static_cast< int >( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace vectrix
