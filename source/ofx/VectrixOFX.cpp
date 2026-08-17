/// The OpenFX builds of Vectrix, for DaVinci Resolve, Nuke, Natron, Vegas and
/// other OFX hosts. Two plugins from this one file, as the FFGL side ships two
/// bundles: "Vectrix" is a generator and "Vectrix Trace" puts the tube in front
/// of the incoming clip.
///
/// ===========================================================================
/// What is shared, and what is mirrored
/// ===========================================================================
///
/// **The whole signal path is shared.** `Engine` owns the sources and the
/// fourteen-block pedalboard and touches no GL, which is exactly why it can be
/// linked straight in here: the oscillator, the shapes, the wireframe, the
/// audio file decoder, the chain and `Controls.cpp`'s `Resolve` are literally
/// the same objects `vxtest` measures. `Presets.h` is the same table, bound in
/// the same order and asserted against `presets::kParamCount` below.
///
/// What is mirrored is the **renderer**, because the renderer is GLSL. Every
/// function in the `cpu` namespace below is a transcription of a pass in
/// `render/shaders/`, constant for constant: the trace's closed form, the
/// two-layer decay cascade, the phosphor emission and graticule prelude, the
/// halation bright pass and its blur, and the glass. **When editing one of
/// those shaders, edit the matching function here.** `Phosphor.cpp` and
/// `Tube.cpp` are already CPU code and are called, not copied.
///
/// ===========================================================================
/// Why the CPU, and not the OFX OpenGL render suite
/// ===========================================================================
///
/// An OFX host hands a plugin a buffer and expects pixels back. `kOfxOpenGLRender`
/// exists, it is optional, and no host guarantees it: Resolve will call the CPU
/// path whenever it feels like it, and Natron has no GL path at all. A plugin
/// that only knows how to draw through GL is a plugin that does not render. So
/// this build asks for no context and creates none, the same choice every other
/// OpenFX port in this fleet made.
///
/// The honest cost, stated plainly: this is **slower than the FFGL build and
/// not interactive**. The trace pass alone evaluates the closed form over an
/// oriented box per sample interval — roughly sixteen hundred of them a frame —
/// and it is threaded across the face buffer's rows to make that bearable. It
/// is fine for an offline render and it is not a VJ tool. That is what the FFGL
/// build is for.
///
/// ===========================================================================
/// Frame order, and the one thing that genuinely differs
/// ===========================================================================
///
/// Vectrix is not a pure function of (time, parameters) the way most of this
/// fleet is. Phase accumulators, delay lines, reverb tanks and the phosphor
/// itself all carry history, and that is deliberate — `AGENTS.md` is explicit
/// that a parameter change must not clear DSP state. OFX, meanwhile, may ask
/// for any frame in any order.
///
/// So this build **replays**. The instance remembers the frame it last
/// simulated and steps forward one frame at a time, which makes a linear render
/// exact. Asking for an earlier frame, or for a jump of more than
/// `kWarmUpFrames`, restarts the simulation `kWarmUpFrames` frames early and
/// runs into the requested frame. Two seconds of warm-up settles the phosphor,
/// the slew limiter, the delay and everything shorter; a reverb tail longer
/// than the window is the one thing that will not be bit-identical to a linear
/// render through the same frame. Bounded, stated, and far better than either
/// alternative (re-simulating from zero costs O(t) per frame; not replaying at
/// all means the picture depends on the order the host happened to ask).
///
/// ===========================================================================
/// Inert on OFX by design
/// ===========================================================================
///
/// The controls below are declared, keep their positions and do nothing. They
/// are present rather than absent because the preset table stores dropdown
/// element *values*, and a build with a shorter list is a build where a preset
/// selects the wrong entry.
///
/// - **No tempo.** OFX carries no transport tempo, so `Resolve` is handed a
///   fixed 120 bpm. "Sync to Tempo" and the Division dropdown therefore work,
///   and work off 120 rather than off the edit. Delay Time is the control that
///   behaves identically in both builds.
/// - **No audio spectrum.** There is no route from a host's audio to a video
///   plugin in OFX, so the FFT buffer parameter has no equivalent and is not
///   declared. Its four Mod slots and their Band dropdowns are declared and
///   sum to zero. **The two LFOs are not inert** — they need nothing from the
///   host but a frame duration, so they run, and a Mod slot pointed at a target
///   is driven by them exactly as in Resolume.
/// - **Trace Input renders nothing.** `TraceSource` reduces the clip to an edge
///   mask with a GPU pass and reads it back through a PBO; there is no CPU
///   entry point into its contour extraction, and adding one would mean editing
///   the shared source. Selecting it leaves the beam cut off — the tube, the
///   graticule and (on the effect build) the clip behind the glass all still
///   render, so the result is a scope with no signal on it rather than a black
///   frame. The parameter's hint says so.
/// - **Reset is a recompute.** In FFGL it drops the accumulated state of a
///   free-running clock. Here the picture at a frame is decided by the frame,
///   so pressing it only discards the replay and re-simulates.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

#include "../Controls.h"
#include "../Presets.h"
#include "../render/Phosphor.h"
#include "../render/Tube.h"
#include "../signal/Engine.h"
#include "../signal/Modulation.h"
#include "../signal/Signal.h"

namespace
{
using namespace vectrix;

constexpr const char* kSourceIdentifier = "com.stoatworks.vectrix";
constexpr const char* kTraceIdentifier  = "com.stoatworks.vectrixtrace";
constexpr const char* kPluginGrouping   = "Stoatworks";

constexpr const char* kPluginDescription =
	"An oscillator, a pedalboard and a cathode ray tube in X/Y mode.\n\n"
	"Everything on the screen is where a single electron beam was and how long "
	"it lingered there. The bright turnarounds, the ghost repeats, the halo and "
	"the rounded corners of a square are consequences of what the signal does "
	"on the way to the yoke, not effects drawn on top of it: a fixed quantum of "
	"energy is deposited per sample interval and spread over the distance the "
	"beam covered, so brightness proportional to dwell time is what equal energy "
	"per unit time means rather than a term applied afterwards.\n\n"
	"Sources are geometry and brightness is physics. Phosphors are a measured "
	"table of real JEDEC types, two layers with a cascade between them, so a P7's "
	"trail is a different colour from its strike and builds behind the beam.\n\n"
	"This OpenFX build renders on the CPU: there is no OpenGL context to be had "
	"in an OFX host. It is built for offline rendering rather than for live use.\n\n"
	"https://stoatworks-labs.com";

//---------------------------------------------------------------------------
// The replay window. Two seconds at 60 fps.
//
// Long enough for the phosphor, the slew limiter, the gate, the delay and the
// oscillators' phase to settle into the same state a linear render would have
// reached; short enough that scrubbing a timeline does not stall the host for
// minutes. See the header comment.
//---------------------------------------------------------------------------
constexpr int kWarmUpFrames = 120;

/// Mirrors `BeamGeometry.cpp`. A runaway must not be allowed to climb until it
/// reaches the top of a 32-bit float and turns into an inf.
constexpr float kExcitationCeiling = 1.0e6f;

/// Mirrors `shaders/Prelude.cpp`'s kConstants. `Extent` is the half-width of
/// the trace's box in units of sigma, and the fragment stage subtracts the
/// profile's value at exactly this distance, so the two have to agree.
constexpr float kExtent     = 4.5f;
constexpr float kSqrt2Pi    = 2.50662827463100050f;
constexpr float kInvSqrt2Pi = 0.39894228040143268f;

inline float saturate( float v )
{
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

/// GLSL smoothstep.
inline float smoothstep( float edge0, float edge1, float x )
{
	const float t = saturate( ( x - edge0 ) / ( edge1 - edge0 ) );
	return t * t * ( 3.0f - 2.0f * t );
}

/// GLSL step.
inline float step( float edge, float x )
{
	return x < edge ? 0.0f : 1.0f;
}

/// GLSL mix.
inline float mix( float a, float b, float t )
{
	return a + ( b - a ) * t;
}

//===========================================================================
// A plane of floats, standing in for a GL texture.
//
// Bottom row first, exactly as GL stores one and exactly as OFX hands its
// images over -- so nothing anywhere in this file flips a row. The signal
// domain is +Y up (see signal/Signal.h), the face buffer's texel row 0 sits at
// NDC y = -1, and OFX's y1 is the bottom of the image. All three agree, and the
// moment one of them is "corrected" the whole picture is upside down.
//===========================================================================
struct Plane
{
	int w = 0, h = 0, c = 0;
	std::vector< float > v;

	void reset( int width, int height, int channels )
	{
		w = std::max( 1, width );
		h = std::max( 1, height );
		c = channels;
		v.assign( static_cast< size_t >( w ) * h * c, 0.0f );
	}

	void clear()
	{
		std::fill( v.begin(), v.end(), 0.0f );
	}

	bool matches( int width, int height, int channels ) const
	{
		return w == width && h == height && c == channels && !v.empty();
	}

	float* at( int x, int y )
	{
		return v.data() + ( static_cast< size_t >( y ) * w + x ) * c;
	}
	const float* at( int x, int y ) const
	{
		return v.data() + ( static_cast< size_t >( y ) * w + x ) * c;
	}

	/// GL_LINEAR with GL_CLAMP_TO_EDGE, which is what `ScopeBuffer::Smooth`
	/// asks for.
	void sample( float u, float vCoord, float* out ) const
	{
		// A plane nothing has rendered into yet reads as black rather than off the
		// end of an empty vector. Only reachable if a host asks for the glass
		// before a single frame has been simulated, which nothing does -- but the
		// cost of being wrong about that is an out-of-bounds read inside a host.
		if( v.empty() )
		{
			for( int k = 0; k < c; ++k )
				out[ k ] = 0.0f;
			return;
		}

		const float fx = u * static_cast< float >( w ) - 0.5f;
		const float fy = vCoord * static_cast< float >( h ) - 0.5f;

		const int x0 = static_cast< int >( std::floor( fx ) );
		const int y0 = static_cast< int >( std::floor( fy ) );
		const float tx = fx - static_cast< float >( x0 );
		const float ty = fy - static_cast< float >( y0 );

		const int xa = std::clamp( x0, 0, w - 1 );
		const int xb = std::clamp( x0 + 1, 0, w - 1 );
		const int ya = std::clamp( y0, 0, h - 1 );
		const int yb = std::clamp( y0 + 1, 0, h - 1 );

		const float* p00 = at( xa, ya );
		const float* p10 = at( xb, ya );
		const float* p01 = at( xa, yb );
		const float* p11 = at( xb, yb );

		const float w00 = ( 1.0f - tx ) * ( 1.0f - ty );
		const float w10 = tx * ( 1.0f - ty );
		const float w01 = ( 1.0f - tx ) * ty;
		const float w11 = tx * ty;

		for( int k = 0; k < c; ++k )
			out[ k ] = p00[ k ] * w00 + p10[ k ] * w10 + p01[ k ] * w01 + p11[ k ] * w11;
	}
};

//===========================================================================
// The shared fragment prelude, from shaders/Prelude.cpp.
//===========================================================================

/// The graticule's uniforms, plus the phosphor table entry. One struct so the
/// bright pass and the glass pass cannot be handed different numbers -- which
/// in the GL build is what `SetPreludeUniforms` is for.
struct Prelude
{
	const PhosphorSpec* spec = nullptr;
	float graticuleLevel     = 0.0f;
	float graticuleDiv       = 1.0f;
	float graticuleColour[ 3 ] = { 0.30f, 0.42f, 0.38f };
};

//= mirrored from Prelude.cpp: the standard normal CDF.
//
// The clamp is kept even though std::tanh does not overflow the way a driver's
// (e^2x-1)/(e^2x+1) does: the two builds have to produce the same number, and a
// difference of 3e-4 at a turnaround where a thousand segments overlap is not a
// difference of 3e-4.
inline float ncdf( float x )
{
	const float t = std::clamp( 0.7978845608f * ( x + 0.044715f * x * x * x ), -8.0f, 8.0f );
	return 0.5f * ( 1.0f + std::tanh( t ) );
}

//= mirrored: excitation -> light. The only thing that bounds a stationary beam,
// and deliberately the tube rather than a clamp.
inline float phosphorSaturate( float e, float saturation )
{
	return e / ( 1.0f + e / std::max( saturation, 1e-6f ) );
}

//= mirrored: phosphorEmission().
inline void phosphorEmission( const Prelude& pre, float fastE, float slowE, float* rgb )
{
	const PhosphorSpec& spec = *pre.spec;
	const float f = phosphorSaturate( std::max( fastE, 0.0f ), std::max( spec.saturation, 1e-4f ) );
	const float s = phosphorSaturate( std::max( slowE, 0.0f ), std::max( spec.saturation, 1e-4f ) );

	for( int k = 0; k < 3; ++k )
		rgb[ k ] = ( f * spec.fastColour[ k ] + s * spec.slowColour[ k ] ) * spec.efficiency;
}

//= mirrored: graticuleAt(). `rateX`/`rateY` stand in for fwidth(g) -- GLSL's
// derivative is itself a finite difference over the 2x2 quad, so every caller
// here computes the real one rather than approximating it.
float graticuleAt( const Prelude& pre, float faceX, float faceY, float rateX, float rateY )
{
	if( pre.graticuleLevel <= 0.0f )
		return 0.0f;

	const float div = std::max( pre.graticuleDiv, 1e-6f );
	const float gx  = faceX / div;
	const float gy  = faceY / div;

	const float halfSpanX = 5.0f;//10 wide
	const float halfSpanY = 4.0f;//by 8 tall

	const float rx = std::max( rateX, 1e-6f );
	const float ry = std::max( rateY, 1e-6f );

	constexpr float lineHalf = 0.015f;//1.5% of a division
	constexpr float tickHalf = 0.09f; //how far a minor tick reaches off its axis

	const float toLineX = std::fabs( gx - std::round( gx ) );
	const float toLineY = std::fabs( gy - std::round( gy ) );
	const float major   = std::max( 1.0f - smoothstep( lineHalf - rx, lineHalf + rx, toLineX ),
	                                1.0f - smoothstep( lineHalf - ry, lineHalf + ry, toLineY ) );

	const float toTickX = std::fabs( gx - 0.2f * std::round( gx * 5.0f ) );
	const float toTickY = std::fabs( gy - 0.2f * std::round( gy * 5.0f ) );
	const float tickX   = ( 1.0f - smoothstep( lineHalf - rx, lineHalf + rx, toTickX ) )
	                    * ( 1.0f - smoothstep( tickHalf, tickHalf + ry, std::fabs( gy ) ) );
	const float tickY   = ( 1.0f - smoothstep( lineHalf - ry, lineHalf + ry, toTickY ) )
	                    * ( 1.0f - smoothstep( tickHalf, tickHalf + rx, std::fabs( gx ) ) );

	float ink = std::max( major, std::max( tickX, tickY ) );

	const float beyondX = smoothstep( halfSpanX + lineHalf - rx, halfSpanX + lineHalf + rx, std::fabs( gx ) );
	const float beyondY = smoothstep( halfSpanY + lineHalf - ry, halfSpanY + lineHalf + ry, std::fabs( gy ) );
	ink *= ( 1.0f - beyondX ) * ( 1.0f - beyondY );

	return ink * pre.graticuleLevel;
}

//===========================================================================
// The renderer's parameters, in physical units.
//
// The same struct `BeamGeometry::RenderParams` is, with the same defaults, so
// `renderParams()` below is line for line the FFGL build's.
//===========================================================================
struct RenderParams
{
	float beamPower    = 1.0f;
	float spotSigma    = 0.006f;
	float spotDefocus  = 0.35f;
	float blankFloor   = 0.0f;
	float densityFloor = 1.0e-4f;

	int phosphor      = 0;
	float persistence = 1.0f;

	float halation          = 0.35f;
	float halationRadius    = 0.5f;
	float halationThreshold = 0.5f;

	TubeSpec tube;

	float graticule            = 0.5f;
	float graticuleColour[ 3 ] = { 0.30f, 0.42f, 0.38f };
	float filterTransmission[ 3 ] = { 0.10f, 0.16f, 0.11f };

	float faceBlack = 1.0f;
	float opacity   = 1.0f;

	float frameSeconds = 1.0f / 60.0f;
};

/// Everything the glass pass needs, worked out once per frame so the
/// multi-threaded pixel loop reads and never computes it.
struct GlassSetup
{
	const Plane* phosphor = nullptr;
	const Plane* bloom    = nullptr;

	Prelude prelude;

	float outputW = 1.0f, outputH = 1.0f;
	float aspect  = 1.0f;
	float faceHalfX = 1.0f, faceHalfY = 1.0f;
	float faceFit   = 1.0f;
	float cornerRadius = 1.0f;
	float curvature    = 0.0f;
	float vignette     = 0.0f;
	float halation     = 0.0f;

	float filterTransmission[ 3 ] = { 1.0f, 1.0f, 1.0f };
	float faceBlack = 0.0f;
	float opacity   = 1.0f;

	bool hasClip = false;
};

//===========================================================================
// The CPU tube. One instance per plugin instance, and it holds the phosphor
// history across frames exactly as the ping-ponged GL buffers do.
//===========================================================================
class TubeRenderer
{
public:
	/// Passes 1 to 5: decay, deposit, then halation. Leaves this frame's
	/// excitation in `phosphor[current]` and the halo in `bloom[0]`, which is
	/// what the glass pass reads. `outputHeight` sizes the face buffer -- by the
	/// spot, not by the composition; see Tube.h.
	void RenderFace( const Sample* samples, int n, const RenderParams& params, int outputHeight )
	{
		const float spotSigma = std::max( params.spotSigma, 1e-5f );

		// Beam units run to 1 at half the face height, so a sigma expressed in
		// them is twice its fraction of the full height. The *undefocused* sigma
		// sets the resolution because it is the smallest.
		const float spotFraction = spotSigma * 0.5f;

		const int wantHeight  = faceSizeFor( spotFraction, outputHeight );
		const int wantWidth   = faceWidthFor( wantHeight, params.tube.faceAspect );
		const int bloomWidth  = std::max( 1, wantWidth / 4 );
		const int bloomHeight = std::max( 1, wantHeight / 4 );

		// Reallocating throws the phosphor history away, which is exactly what
		// ScopeBuffer::Ensure does on a size change -- the trail vanishes and the
		// picture flashes. The ladder in Tube.cpp is what keeps that rare.
		if( !phosphor[ 0 ].matches( wantWidth, wantHeight, 2 ) )
		{
			phosphor[ 0 ].reset( wantWidth, wantHeight, 2 );
			phosphor[ 1 ].reset( wantWidth, wantHeight, 2 );
			index = 0;
		}
		if( !bloom[ 0 ].matches( bloomWidth, bloomHeight, 3 ) )
		{
			bloom[ 0 ].reset( bloomWidth, bloomHeight, 3 );
			bloom[ 1 ].reset( bloomWidth, bloomHeight, 3 );
		}

		const int target  = index;
		const int history = 1 - index;

		const float faceAspect = std::max( params.tube.faceAspect, 0.05f );

		const PhosphorSpec& spec  = vectrix::phosphor( params.phosphor );
		const PhosphorDecay decay = decayFor( spec, params.persistence,
		                                      std::max( params.frameSeconds, 1.0e-5f ) );

		decayPass( phosphor[ history ], phosphor[ target ], decay );
		tracePass( samples, n, params, phosphor[ target ], faceAspect, spotSigma );

		index = history;
		current = target;
	}

	/// Passes 3 to 5, which need the prelude and therefore this frame's
	/// graticule level. Kept out of RenderFace only because the level is derived
	/// from the output size, which the face buffer knows nothing about.
	void RenderHalation( const RenderParams& params, const Prelude& pre )
	{
		haloReady = params.halation > 0.001f;
		if( !haloReady )
		{
			bloom[ 0 ].clear();
			return;
		}

		brightPass( phosphor[ current ], bloom[ 0 ], pre, params );

		// One iteration is a tight halo, three a wide soft one, and three is the
		// ceiling: past that the halo is wider than the face and stops being
		// scattering in glass at all.
		const int iterations = std::clamp(
			1 + static_cast< int >( std::lround( 2.0f * std::clamp( params.halationRadius, 0.0f, 1.0f ) ) ),
			1, 3 );

		for( int i = 0; i < iterations; ++i )
		{
			// Across then down, so every iteration ends back in bloom[0] and the
			// glass pass never has to ask which one it landed in.
			blurPass( bloom[ 0 ], bloom[ 1 ], 1.0f / static_cast< float >( bloom[ 0 ].w ), 0.0f );
			blurPass( bloom[ 1 ], bloom[ 0 ], 0.0f, 1.0f / static_cast< float >( bloom[ 0 ].h ) );
		}
	}

	void Clear()
	{
		phosphor[ 0 ].clear();
		phosphor[ 1 ].clear();
		bloom[ 0 ].clear();
		bloom[ 1 ].clear();
	}

	const Plane& Phosphor() const
	{
		return phosphor[ current ];
	}
	const Plane& Bloom() const
	{
		return bloom[ 0 ];
	}
	bool HaloReady() const
	{
		return haloReady;
	}

private:
	//= mirrored from shaders/Decay.cpp -------------------------------------
	static void decayPass( const Plane& from, Plane& to, const PhosphorDecay& decay )
	{
		const size_t count = static_cast< size_t >( to.w ) * to.h;
		for( size_t i = 0; i < count; ++i )
		{
			float hx = from.v[ i * 2 + 0 ];
			float hy = from.v[ i * 2 + 1 ];

			// The backstop, and the reason it is behind the buffer rather than in
			// front of it: a ping-ponged accumulator is the one place a single bad
			// value is permanent. NaN * DecayFast is NaN forever, and the
			// operator's only remedy would be to delete the effect and add it
			// again. Comparisons rather than isnan, for the reason Trace.cpp gives.
			if( !( hx > -1e30f && hx < 1e30f ) )
				hx = 0.0f;
			if( !( hy > -1e30f && hy < 1e30f ) )
				hy = 0.0f;

			const float fast = hx * decay.fast;
			const float slow = hy * decay.slow + hx * ( 1.0f - decay.fast ) * decay.transfer;

			to.v[ i * 2 + 0 ] = std::min( fast, kExcitationCeiling );
			to.v[ i * 2 + 1 ] = std::min( slow, kExcitationCeiling );
		}
	}

	//= mirrored from shaders/Trace.cpp -------------------------------------
	//
	// One oriented box per sample interval, summed into the fast layer. Sum and
	// not max(): the input is a *deposit*, and two intervals crossing the same
	// texel really did put twice the energy there. A max() would throw away
	// every crossing in the figure, which is exactly where a trace is brightest.
	struct Segment
	{
		float cx, cy;              ///< centre, beam units
		float dx, dy;              ///< unit direction
		float halfAlong, halfAcross;
		float len, invSigma, energy;
		bool pointLimit;           ///< segLength < 0.25 * segSigma
		float pedestal;
	};

	static bool usable( float v )
	{
		return v > -1e30f && v < 1e30f;
	}

	void tracePass( const Sample* samples, int n, const RenderParams& params,
	                Plane& target, float faceAspect, float spotSigma )
	{
		// The last sample has no interval after it, so there are n-1 intervals.
		const int segmentCount = ( samples == nullptr ) ? 0 : std::max( 0, n - 1 );
		if( segmentCount <= 0 || params.beamPower <= 0.0f )
			return;

		const float defocus     = std::max( params.spotDefocus, 0.0f );
		const float blankFloor  = std::clamp( params.blankFloor, 0.0f, 1.0f );
		const float densityFloor = std::max( params.densityFloor, 0.0f );
		const float gain        = params.tube.deflectionGain;

		segments.clear();
		segments.reserve( static_cast< size_t >( segmentCount ) );

		for( int i = 0; i < segmentCount; ++i )
		{
			const Sample& sa = samples[ i ];
			const Sample& sb = samples[ i + 1 ];

			const float ax = sa.x * gain, ay = sa.y * gain;
			const float bx = sb.x * gain, by = sb.y * gain;

			// The grid voltage over the interval, averaged across its two ends.
			// The floor is what a cut-off beam still manages -- a real gun does not
			// reach zero emission.
			const float zBar = mix( blankFloor, 1.0f, saturate( 0.5f * ( sa.z + sb.z ) ) );

			// The whole brightness model, in one line: energy per interval, not
			// intensity per pixel. `dt` is per-sample precisely so this is
			// independent of how many samples the engine chose to emit.
			const float energy = params.beamPower * std::max( sa.dt, 0.0f ) * zBar;
			const float sigma  = spotSigma * ( 1.0f + defocus * zBar );

			const float ddx = bx - ax, ddy = by - ay;
			const float span = std::sqrt( ddx * ddx + ddy * ddy );
			const float dirX = span > 1e-9f ? ddx / span : 1.0f;
			const float dirY = span > 1e-9f ? ddy / span : 0.0f;

			const float len = std::max( span, 0.05f * sigma );

			// Peak areal density of the finished segment. Culling on this rather
			// than on energy is what makes the cull mean something: a fast sweep
			// and a slow one can carry the same energy and only one is visible.
			const float density = energy / std::max( len * sigma * kSqrt2Pi, 1e-30f );

			const bool ok = usable( ax ) && usable( ay ) && usable( bx ) && usable( by )
			             && usable( energy ) && usable( sigma )
			             && energy > 0.0f && sigma > 0.0f
			             && density >= densityFloor;
			if( !ok )
				continue;//the GL build's degenerate quad, which rasterises nothing

			Segment seg;
			seg.halfAlong  = 0.5f * len + kExtent * sigma;
			seg.halfAcross = kExtent * sigma;
			seg.cx = ax + dirX * ( 0.5f * len );
			seg.cy = ay + dirY * ( 0.5f * len );
			seg.dx = dirX;
			seg.dy = dirY;
			seg.len      = len;
			seg.invSigma = 1.0f / sigma;
			seg.energy   = energy;
			seg.pointLimit = len < 0.25f * sigma;
			seg.pedestal   = kInvSqrt2Pi * seg.invSigma * std::exp( -0.5f * kExtent * kExtent );
			segments.push_back( seg );
		}

		if( segments.empty() )
			return;

		const int faceW = target.w;
		const int faceH = target.h;

		// beam units -> face texel. The GL build's viewport is the whole face
		// buffer and gl_Position is (pos.x / FaceAspect, pos.y), so NDC -1..1
		// covers 0..faceW and 0..faceH. The divide by the aspect is the only place
		// the face's shape enters the trace at all, which is what keeps the spot
		// round on a 4:3 face.
		const float toPixelX = 0.5f * static_cast< float >( faceW ) / faceAspect;
		const float toPixelY = 0.5f * static_cast< float >( faceH );

		const unsigned hardware = std::max( 1u, std::thread::hardware_concurrency() );
		const int threads = static_cast< int >( std::min< unsigned >( hardware, 16u ) );

		// Split by ROW, not by segment. Each thread owns a disjoint band of the
		// face buffer, so the additive accumulation needs no atomics and no
		// per-thread copy of a buffer that can be four megapixels.
		auto band = [ & ]( int y0, int y1 ) {
			for( const Segment& seg : segments )
			{
				// The oriented box's four corners, into texel space.
				const float ex = seg.dx * seg.halfAlong;
				const float ey = seg.dy * seg.halfAlong;
				const float px = -seg.dy * seg.halfAcross;
				const float py = seg.dx * seg.halfAcross;

				float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
				for( int corner = 0; corner < 4; ++corner )
				{
					const float sx = ( corner & 1 ) ? 1.0f : -1.0f;
					const float sy = ( corner & 2 ) ? 1.0f : -1.0f;
					const float bx = seg.cx + ex * sx + px * sy;
					const float by = seg.cy + ey * sx + py * sy;

					const float fx = bx * toPixelX + 0.5f * static_cast< float >( faceW ) - 0.5f;
					const float fy = by * toPixelY + 0.5f * static_cast< float >( faceH ) - 0.5f;

					minX = std::min( minX, fx );
					maxX = std::max( maxX, fx );
					minY = std::min( minY, fy );
					maxY = std::max( maxY, fy );
				}

				int x0 = std::max( 0, static_cast< int >( std::floor( minX ) ) );
				int x1 = std::min( faceW - 1, static_cast< int >( std::ceil( maxX ) ) );
				int ry0 = std::max( y0, static_cast< int >( std::floor( minY ) ) );
				int ry1 = std::min( y1 - 1, static_cast< int >( std::ceil( maxY ) ) );
				if( x0 > x1 || ry0 > ry1 )
					continue;

				const float halfLen = 0.5f * seg.len;

				for( int y = ry0; y <= ry1; ++y )
				{
					const float beamY = ( ( static_cast< float >( y ) + 0.5f )
					                      / static_cast< float >( faceH ) * 2.0f - 1.0f );
					float* row = target.at( x0, y );

					for( int x = x0; x <= x1; ++x, row += 2 )
					{
						const float beamX = ( ( static_cast< float >( x ) + 0.5f )
						                      / static_cast< float >( faceW ) * 2.0f - 1.0f )
						                    * faceAspect;

						const float qx = beamX - seg.cx;
						const float qy = beamY - seg.cy;
						const float u  = qx * seg.dx + qy * seg.dy;
						const float vv = -qx * seg.dy + qy * seg.dx;

						if( u < -seg.halfAlong || u > seg.halfAlong
						    || vv < -seg.halfAcross || vv > seg.halfAcross )
							continue;

						// Across the segment: a normalised Gaussian, minus the value
						// it has at the box's own boundary. Without the subtraction
						// the profile is cut off with a step of 1.5e-5 of the peak --
						// invisible for one segment, and a visible polygon edge where
						// a thousand overlap, which is precisely the part of the
						// picture this plugin exists to render.
						const float inv = seg.invSigma;
						float across = kInvSqrt2Pi * inv * std::exp( -0.5f * vv * vv * inv * inv )
						               - seg.pedestal;
						if( across <= 0.0f )
							continue;

						float along;
						if( seg.pointLimit )
						{
							// The point limit, taken explicitly. A difference of two
							// nearly equal CDFs each carrying ~3e-4 of error is several
							// percent by the time L is a quarter of a sigma -- and it is
							// a short segment exactly at the turnarounds and the
							// stationary dots, which are the features this is about.
							along = kInvSqrt2Pi * inv * std::exp( -0.5f * u * u * inv * inv );
						}
						else
						{
							along = ( ncdf( ( u + halfLen ) * inv ) - ncdf( ( u - halfLen ) * inv ) ) / seg.len;
						}

						// R is the fast layer. The slow layer is not deposited into:
						// it is pumped only by what the fast layer sheds, in the decay
						// pass.
						row[ 0 ] += seg.energy * across * along;
					}
				}
			}
		};

		if( threads <= 1 || faceH < threads * 2 )
		{
			band( 0, faceH );
			return;
		}

		std::vector< std::thread > pool;
		pool.reserve( static_cast< size_t >( threads ) );
		const int rows = ( faceH + threads - 1 ) / threads;
		for( int t = 0; t < threads; ++t )
		{
			const int y0 = t * rows;
			const int y1 = std::min( faceH, y0 + rows );
			if( y0 >= y1 )
				break;
			pool.emplace_back( [ &band, y0, y1 ] { band( y0, y1 ); } );
		}
		for( std::thread& th : pool )
			th.join();
	}

	//= mirrored from shaders/Bloom.cpp -------------------------------------
	static void brightPass( const Plane& phosphorPlane, Plane& out, const Prelude& pre,
	                        const RenderParams& params )
	{
		const float texelX = 1.0f / std::max( static_cast< float >( phosphorPlane.w ), 1.0f );
		const float texelY = 1.0f / std::max( static_cast< float >( phosphorPlane.h ), 1.0f );

		const float faceHalfX = std::max( params.tube.faceAspect, 0.05f );
		const float faceHalfY = 1.0f;

		// fwidth(g) in this pass: face is an affine function of uv, so the rate is
		// one texel of THIS buffer, per axis, and nothing else.
		const float div   = std::max( pre.graticuleDiv, 1e-6f );
		const float rateX = 2.0f * faceHalfX / ( static_cast< float >( out.w ) * div );
		const float rateY = 2.0f * faceHalfY / ( static_cast< float >( out.h ) * div );

		const float threshold = params.halationThreshold;

		for( int y = 0; y < out.h; ++y )
		{
			const float v = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( out.h );
			for( int x = 0; x < out.w; ++x )
			{
				const float u = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( out.w );

				// Four bilinear taps at the corners of the destination footprint,
				// converted to light INDIVIDUALLY -- the saturation curve is not
				// linear and each grain saturates on its own.
				float colour[ 3 ] = { 0.0f, 0.0f, 0.0f };
				const float offsets[ 4 ][ 2 ] = { { -1.0f, -1.0f }, { 1.0f, -1.0f },
				                                  { -1.0f, 1.0f }, { 1.0f, 1.0f } };
				for( const auto& off : offsets )
				{
					float e[ 2 ];
					phosphorPlane.sample( u + off[ 0 ] * texelX, v + off[ 1 ] * texelY, e );
					float lit[ 3 ];
					phosphorEmission( pre, e[ 0 ], e[ 1 ], lit );
					for( int k = 0; k < 3; ++k )
						colour[ k ] += lit[ k ];
				}
				for( int k = 0; k < 3; ++k )
					colour[ k ] *= 0.25f;

				// The graticule is drawn here as well as in the glass pass, from the
				// same function at the same face coordinate, so the lines cast a halo
				// of their own instead of being the one thing on the face that does not.
				const float faceX = ( u * 2.0f - 1.0f ) * faceHalfX;
				const float faceY = ( v * 2.0f - 1.0f ) * faceHalfY;
				const float ink   = graticuleAt( pre, faceX, faceY, rateX, rateY );
				for( int k = 0; k < 3; ++k )
					colour[ k ] += pre.graticuleColour[ k ] * ink;

				const float luma = colour[ 0 ] * 0.299f + colour[ 1 ] * 0.587f + colour[ 2 ] * 0.114f;
				const float knee = smoothstep( threshold, threshold + 0.35f, luma );

				float* dst = out.at( x, y );
				for( int k = 0; k < 3; ++k )
					dst[ k ] = std::max( colour[ k ] * knee, 0.0f );
			}
		}
	}

	//= mirrored: the nine-tap Gaussian collapsed to three bilinear fetches. The
	// same numbers old-cathode uses, so the two plugins' haloes match on screen.
	static void blurPass( const Plane& from, Plane& to, float dirX, float dirY )
	{
		static const float offsets[ 3 ] = { 0.0f, 1.3846153846f, 3.2307692308f };
		static const float weights[ 3 ] = { 0.2270270270f, 0.3162162162f, 0.0702702703f };

		for( int y = 0; y < to.h; ++y )
		{
			const float v = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( to.h );
			for( int x = 0; x < to.w; ++x )
			{
				const float u = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( to.w );

				float sum[ 3 ] = { 0.0f, 0.0f, 0.0f };
				float tap[ 3 ];

				from.sample( u, v, tap );
				for( int k = 0; k < 3; ++k )
					sum[ k ] += tap[ k ] * weights[ 0 ];

				for( int i = 1; i < 3; ++i )
				{
					from.sample( u + dirX * offsets[ i ], v + dirY * offsets[ i ], tap );
					for( int k = 0; k < 3; ++k )
						sum[ k ] += tap[ k ] * weights[ i ];
					from.sample( u - dirX * offsets[ i ], v - dirY * offsets[ i ], tap );
					for( int k = 0; k < 3; ++k )
						sum[ k ] += tap[ k ] * weights[ i ];
				}

				float* dst = to.at( x, y );
				for( int k = 0; k < 3; ++k )
					dst[ k ] = sum[ k ];
			}
		}
	}

	Plane phosphor[ 2 ];
	Plane bloom[ 2 ];
	std::vector< Segment > segments;
	int index     = 0;///< which buffer the next frame writes into
	int current   = 0;///< which buffer this frame's excitation is in
	bool haloReady = false;
};

//===========================================================================
// The glass pass, per output pixel.
//= mirrored from shaders/Glass.cpp
//
// Perspective is fixed at zero in both builds -- the GL shader sets both
// uniforms to a literal zero -- which collapses step 1 of the shader's four to
// `local.xy = p` and `inFront = 1`. Every other step is here as written.
//===========================================================================

/// The rounded-rectangle field, and the face coordinate that produced it. One
/// function so the mask, the graticule and their derivatives all come from the
/// same evaluation.
struct FacePoint
{
	float x = 0.0f, y = 0.0f;///< face, beam units
	float sd = 0.0f;         ///< signed distance to the face's edge
	float bulge = 1.0f;
	float nx = 0.0f, ny = 0.0f;
};

inline FacePoint faceAt( const GlassSetup& s, float u, float v )
{
	FacePoint f;

	float px = u * 2.0f - 1.0f;
	const float py = v * 2.0f - 1.0f;
	px *= s.aspect;//square units, so a rotation would be a rotation

	const float viewedX = px / std::max( s.faceFit, 1e-6f );
	const float viewedY = py / std::max( s.faceFit, 1e-6f );

	// Undo the curvature: the face bulges, so the sampling pinches. Divided
	// through by the expansion at the corner, which is overscan -- kept exactly
	// as old-cathode has it, so the two plugins' tubes are the same tube.
	f.nx = viewedX / s.faceHalfX;
	f.ny = viewedY / s.faceHalfY;
	f.bulge = ( 1.0f + s.curvature * 0.5f * ( f.nx * f.nx + f.ny * f.ny ) ) / ( 1.0f + s.curvature );

	f.x = viewedX * f.bulge;
	f.y = viewedY * f.bulge;

	const float radius = std::clamp( s.cornerRadius, 0.0f, 1.0f ) * std::min( s.faceHalfX, s.faceHalfY );
	const float qx = std::fabs( f.x ) - ( s.faceHalfX - radius );
	const float qy = std::fabs( f.y ) - ( s.faceHalfY - radius );
	const float mx = std::max( qx, 0.0f );
	const float my = std::max( qy, 0.0f );
	f.sd = std::sqrt( mx * mx + my * my ) + std::min( std::max( qx, qy ), 0.0f ) - radius;

	return f;
}

//===========================================================================
// The OFX processors.
//===========================================================================
class VectrixProcessorBase : public OFX::ImageProcessor
{
public:
	explicit VectrixProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setSetup( const GlassSetup* s, OFX::Image* src )
	{
		setup  = s;
		srcImg = src;
	}

protected:
	const GlassSetup* setup = nullptr;
	OFX::Image* srcImg      = nullptr;
};

template< class PIX, int nComponents, int maxValue >
class VectrixProcessor : public VectrixProcessorBase
{
public:
	explicit VectrixProcessor( OFX::ImageEffect& effect ) :
		VectrixProcessorBase( effect )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const GlassSetup& s   = *setup;
		const OfxRectI bounds = _dstImg->getBounds();
		const int outW        = std::max( 1, bounds.x2 - bounds.x1 );
		const int outH        = std::max( 1, bounds.y2 - bounds.y1 );

		const float invW = 1.0f / static_cast< float >( outW );
		const float invH = 1.0f / static_cast< float >( outH );

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dstPix = static_cast< PIX* >( _dstImg->getPixelAddress( window.x1, y ) );

			// OFX rows run bottom-up and so does GL's uv, so this is the shader's
			// `uv` with nothing flipped. See Plane.
			const float v = ( static_cast< float >( y - bounds.y1 ) + 0.5f ) * invH;

			for( int x = window.x1; x < window.x2; ++x, dstPix += nComponents )
			{
				const float u = ( static_cast< float >( x - bounds.x1 ) + 0.5f ) * invW;

				//--- the face, and the derivatives the anti-aliasing needs -----
				//
				// GLSL's fwidth is itself a finite difference over the 2x2 quad, so
				// taking the real one here is not an approximation of the shader --
				// it is the same thing the shader does.
				const FacePoint f  = faceAt( s, u, v );
				const FacePoint fx = faceAt( s, u + invW, v );
				const FacePoint fy = faceAt( s, u, v + invH );

				const float aa = std::max( std::fabs( fx.sd - f.sd ) + std::fabs( fy.sd - f.sd ), 1e-5f );
				float faceMask = 1.0f - smoothstep( -aa, aa, f.sd );

				const float faceUVx = f.x / ( 2.0f * s.faceHalfX ) + 0.5f;
				const float faceUVy = f.y / ( 2.0f * s.faceHalfY ) + 0.5f;

				// Curvature can push the sample outside the face buffer before the
				// mask cuts it off, and a clamped texture edge out there would smear
				// the last row of phosphor across the bezel.
				const float beyondX = step( 0.0f, -faceUVx ) + step( 1.0f, faceUVx );
				const float beyondY = step( 0.0f, -faceUVy ) + step( 1.0f, faceUVy );
				faceMask *= 1.0f - saturate( beyondX + beyondY );

				//1.0 - 0.0 * anything is exactly 1.0.
				const float vnx = f.nx * 0.92f;
				const float vig = 1.0f - s.vignette
				                  * smoothstep( 0.25f, 1.5f,
				                                std::sqrt( vnx * vnx + f.ny * f.ny ) );

				float emission[ 3 ] = { 0.0f, 0.0f, 0.0f };
				if( faceMask > 0.0f )
				{
					float e[ 2 ];
					s.phosphor->sample( faceUVx, faceUVy, e );
					phosphorEmission( s.prelude, e[ 0 ], e[ 1 ], emission );
					for( int k = 0; k < 3; ++k )
						emission[ k ] *= vig;
				}

				const float div = std::max( s.prelude.graticuleDiv, 1e-6f );
				const float rateX = ( std::fabs( fx.x - f.x ) + std::fabs( fy.x - f.x ) ) / div;
				const float rateY = ( std::fabs( fx.y - f.y ) + std::fabs( fy.y - f.y ) ) / div;
				const float ink   = graticuleAt( s.prelude, f.x, f.y, rateX, rateY );
				for( int k = 0; k < 3; ++k )
				{
					emission[ k ] += s.prelude.graticuleColour[ k ] * ink;
					emission[ k ] *= faceMask;
				}

				float halo[ 3 ] = { 0.0f, 0.0f, 0.0f };
				if( s.halation > 0.0f && faceMask > 0.0f )
				{
					s.bloom->sample( faceUVx, faceUVy, halo );
					for( int k = 0; k < 3; ++k )
						halo[ k ] *= faceMask;
				}

				//--- what is behind the glass ---------------------------------
				float clipTexel[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
				if( s.hasClip )
					readClip( s, x, y, bounds, outW, outH, u, v, f, clipTexel );

				// The filter carries its tint into the colour and only the mask into
				// the alpha: a green contrast filter darkens what is behind it
				// without making it any less opaque. Both are literal ones when the
				// faceplate is not there, which is what makes the passthrough exact.
				float faceplate[ 3 ] = { 1.0f, 1.0f, 1.0f };
				float faceplateA     = 1.0f;
				if( s.faceBlack > 0.0f )
				{
					for( int k = 0; k < 3; ++k )
						faceplate[ k ] = mix( 1.0f, s.filterTransmission[ k ] * faceMask, s.faceBlack );
					faceplateA = mix( 1.0f, faceMask, s.faceBlack );
				}

				float colour[ 3 ];
				float emitted[ 3 ];
				for( int k = 0; k < 3; ++k )
				{
					emitted[ k ] = emission[ k ] + halo[ k ] * s.halation;
					colour[ k ]  = clipTexel[ k ] * faceplate[ k ] + emitted[ k ];
				}
				const float throughA = clipTexel[ 3 ] * faceplateA;

				// The tube's own light is opaque -- glass that is emitting is not
				// something you see through.
				const float emissive = saturate( std::max( emitted[ 0 ], std::max( emitted[ 1 ], emitted[ 2 ] ) ) );
				const float coverage = s.hasClip ? std::max( s.faceBlack * faceMask, emissive ) : faceMask;
				const float alpha    = saturate( std::max( throughA, coverage ) ) * s.opacity;

				// Only the floor is clamped. Nothing here can produce negative light,
				// so a negative value means the clip arrived with one; clamping the
				// ceiling as well would quietly change a passthrough of any clip that
				// carries values above 1, which some do.
				dstPix[ 0 ] = toPix( std::max( colour[ 0 ], 0.0f ) );
				dstPix[ 1 ] = toPix( std::max( colour[ 1 ], 0.0f ) );
				dstPix[ 2 ] = toPix( std::max( colour[ 2 ], 0.0f ) );
				if( nComponents == 4 )
					dstPix[ 3 ] = toPix( alpha );
			}
		}
	}

private:
	static PIX toPix( float value )
	{
		if( maxValue == 1 )
			return static_cast< PIX >( value );
		const float scaled = value * static_cast< float >( maxValue );
		return static_cast< PIX >( std::clamp( scaled, 0.0f, static_cast< float >( maxValue ) ) + 0.5f );
	}

	void readClip( const GlassSetup& s, int x, int y, const OfxRectI& bounds,
	               int outW, int outH, float u, float v, const FacePoint& f, float* out ) const
	{
		if( srcImg == nullptr )
			return;

		// At zero curvature the round trip through `aspect` is a multiply and a
		// divide that are not required to cancel in floating point, so that case
		// takes the pixel it already has rather than one that is nearly it. That
		// is what makes the passthrough exact rather than merely close.
		if( s.curvature == 0.0f )
		{
			const PIX* pix = static_cast< const PIX* >( srcImg->getPixelAddress( x, y ) );
			if( pix == nullptr )
				return;
			const float inv = 1.0f / static_cast< float >( maxValue );
			out[ 0 ] = static_cast< float >( pix[ 0 ] ) * inv;
			out[ 1 ] = static_cast< float >( pix[ 1 ] ) * inv;
			out[ 2 ] = static_cast< float >( pix[ 2 ] ) * inv;
			out[ 3 ] = nComponents == 4 ? static_cast< float >( pix[ 3 ] ) * inv : 1.0f;
			return;
		}

		( void )u;
		( void )v;

		// The warp, undone back into output coordinates.
		const float px = ( u * 2.0f - 1.0f ) * s.aspect;
		const float py = v * 2.0f - 1.0f;
		const float warpedX = px * f.bulge;
		const float warpedY = py * f.bulge;
		const float clipU   = ( warpedX / s.aspect ) * 0.5f + 0.5f;
		const float clipV   = warpedY * 0.5f + 0.5f;

		// Bilinear, clamped, against the source image's own bounds -- the OFX
		// equivalent of the shader's GL_CLAMP_TO_EDGE sampler. There is no MaxUV
		// here: an OFX clip has no padding, so ClipMaxUV is (1,1) by construction.
		const OfxRectI srcBounds = srcImg->getBounds();
		const int srcW = std::max( 1, srcBounds.x2 - srcBounds.x1 );
		const int srcH = std::max( 1, srcBounds.y2 - srcBounds.y1 );
		( void )outW;
		( void )outH;
		( void )bounds;

		const float fx = clipU * static_cast< float >( srcW ) - 0.5f;
		const float fy = clipV * static_cast< float >( srcH ) - 0.5f;
		const int x0 = static_cast< int >( std::floor( fx ) );
		const int y0 = static_cast< int >( std::floor( fy ) );
		const float tx = fx - static_cast< float >( x0 );
		const float ty = fy - static_cast< float >( y0 );

		const float wgt[ 4 ] = { ( 1.0f - tx ) * ( 1.0f - ty ), tx * ( 1.0f - ty ),
		                         ( 1.0f - tx ) * ty, tx * ty };
		const int ox[ 4 ] = { 0, 1, 0, 1 };
		const int oy[ 4 ] = { 0, 0, 1, 1 };

		const float inv = 1.0f / static_cast< float >( maxValue );
		for( int t = 0; t < 4; ++t )
		{
			const int sx = std::clamp( x0 + ox[ t ], 0, srcW - 1 ) + srcBounds.x1;
			const int sy = std::clamp( y0 + oy[ t ], 0, srcH - 1 ) + srcBounds.y1;
			const PIX* pix = static_cast< const PIX* >( srcImg->getPixelAddress( sx, sy ) );
			if( pix == nullptr )
				continue;
			out[ 0 ] += static_cast< float >( pix[ 0 ] ) * inv * wgt[ t ];
			out[ 1 ] += static_cast< float >( pix[ 1 ] ) * inv * wgt[ t ];
			out[ 2 ] += static_cast< float >( pix[ 2 ] ) * inv * wgt[ t ];
			out[ 3 ] += ( nComponents == 4 ? static_cast< float >( pix[ 3 ] ) * inv : 1.0f ) * wgt[ t ];
		}
	}
};

//===========================================================================
// The parameter table.
//
// ONE list drives the description, the per-frame read into the 0..1 array
// `Resolve` wants, and the preset writer. Declaring those three separately is
// how a control ends up described but never read -- which compiles, links,
// loads and renders, and is stone dead.
//
// The names, the labels, the defaults and the option lists are the FFGL build's
// (`Vectrix.cpp::declareParameters` and its `kDefaults` table), so the two hosts
// agree about what every control is and where it starts.
//===========================================================================
enum class Kind
{
	Slider, ///< FF_TYPE_STANDARD: 0..1, mapped in Controls.cpp
	Toggle, ///< FF_TYPE_BOOLEAN
	Count,  ///< FF_TYPE_INTEGER with a real range
	Option, ///< a dropdown holding its element VALUE, read through Option()
	File,   ///< the audio file path
	Button, ///< FF_TYPE_EVENT
	Absent  ///< declared in the FFGL build and impossible here
};

struct Decl
{
	unsigned int id;
	Kind kind;
	const char* name;
	const char* label;
	const char* hint;
	float def;
	float lo;
	float hi;
	const char* const* options;
	int optionCount;
	const char* group;///< non-null starts a new group, mirroring SetParamGroup
};

const char* const kRingWaveNames[ 3 ] = { "Sine", "Triangle", "Square" };

#define SLIDER( id, name, label, def, hint ) \
	{ id, Kind::Slider, name, label, hint, def, 0.0f, 1.0f, nullptr, 0, nullptr }
#define SLIDERG( id, name, label, def, hint, group ) \
	{ id, Kind::Slider, name, label, hint, def, 0.0f, 1.0f, nullptr, 0, group }
#define TOGGLE( id, name, label, def, hint ) \
	{ id, Kind::Toggle, name, label, hint, def, 0.0f, 1.0f, nullptr, 0, nullptr }
#define TOGGLEG( id, name, label, def, hint, group ) \
	{ id, Kind::Toggle, name, label, hint, def, 0.0f, 1.0f, nullptr, 0, group }
#define COUNT( id, name, label, def, lo, hi, hint ) \
	{ id, Kind::Count, name, label, hint, def, lo, hi, nullptr, 0, nullptr }
#define OPTION( id, name, label, def, table, n, hint ) \
	{ id, Kind::Option, name, label, hint, def, 0.0f, 1.0f, table, n, nullptr }
#define OPTIONG( id, name, label, def, table, n, hint, group ) \
	{ id, Kind::Option, name, label, hint, def, 0.0f, 1.0f, table, n, group }

const Decl kDecls[] = {
	//--- Clock -------------------------------------------------------------
	OPTIONG( PT_DETAIL, "detail", "Detail", 1.0f, kDetailNames, kDetailCount,
	         "The internal sample rate: 48, 96 or 192 kHz. A cost dial with a visible "
	         "symptom -- below Normal, figures above a few hundred hertz polygonalise. "
	         "Normal is what everything is calibrated against.",
	         "Clock" ),
	{ PT_RESET, Kind::Button, "reset", "Reset",
	  "Discard the simulation and run it again. In Resolume this drops a free-running "
	  "clock; here the picture at a frame is decided by the frame, so this is a "
	  "recompute rather than a transport action.",
	  0.0f, 0.0f, 1.0f, nullptr, 0, nullptr },
	COUNT( PT_SEED, "seed", "Seed", 1.0f, 1.0f, 9999.0f,
	       "Which noise, and which mountain range." ),

	//--- Source ------------------------------------------------------------
	OPTIONG( PT_SOURCE, "source", "Source", 0.0f, kSourceNames, kSourceCount,
	         "What is driving the beam. Trace Input reduces the incoming clip to an "
	         "ordered contour path -- it needs a GPU edge pass and a readback, neither "
	         "of which an OpenFX host provides, so in this build it leaves the beam cut "
	         "off. The other four are the same code Resolume runs.",
	         "Source" ),

	//--- Oscillator --------------------------------------------------------
	OPTIONG( PT_WAVE_X, "waveX", "Wave X", 0.0f, kWaveNames, kWaveCount,
	         "The waveform on the horizontal deflection.", "Oscillator" ),
	OPTION( PT_WAVE_Y, "waveY", "Wave Y", 0.0f, kWaveNames, kWaveCount,
	        "The waveform on the vertical deflection." ),
	SLIDER( PT_FREQ_X, "freqX", "Frequency X", 0.55f,
	        "0.01 Hz to 2 kHz, exponentially. Phase is integrated, so moving this "
	        "does not jump the figure to a different point in its cycle." ),
	OPTION( PT_RATIO, "ratio", "Ratio", 3.0f, kRatioNames, kRatioCount,
	        "Y against X, as an exact rational. A slider here would land on 2.001:1 "
	        "and the figure would rotate slowly and forever; Detune is the control "
	        "for the drift you might actually want." ),
	TOGGLE( PT_FREE_Y, "freeY", "Free Y", 0.0f,
	        "Ignore the ratio and set Y's frequency directly." ),
	SLIDER( PT_FREQ_Y, "freqY", "Frequency Y", 0.55f, "Used only under Free Y." ),
	SLIDER( PT_PHASE_Y, "phaseY", "Phase Y", 0.25f,
	        "Y's phase offset in turns. 0.25 is 90 degrees, which turns a diagonal "
	        "line into a circle." ),
	SLIDER( PT_PWM_X, "widthX", "Width X", 0.5f, "Pulse width on X." ),
	SLIDER( PT_PWM_Y, "widthY", "Width Y", 0.5f, "Pulse width on Y." ),
	SLIDER( PT_DETUNE, "detune", "Detune", 0.5f,
	        "+-2% off the exact ratio. Zero at the centre of the slider, which is "
	        "where a figure stops rotating." ),
	TOGGLE( PT_HARD_SYNC, "hardSync", "Hard Sync", 0.0f,
	        "Reset Y's phase whenever X wraps." ),
	TOGGLE( PT_BLANK_RETRACE, "blankRetrace", "Blank on Retrace", 0.0f,
	        "Cut the beam during a saw's flyback. Saw against saw with this on is a "
	        "clean raster, and is the cheapest demonstration that the beam is genuinely "
	        "being modelled rather than drawn." ),

	//--- Shape -------------------------------------------------------------
	OPTIONG( PT_SHAPE, "shape", "Shape", 0.0f, kShapeNames, kShapeCount,
	         "A closed figure walked at a constant rate. The square is deliberately "
	         "not arc-length parameterised: its corners are bright because the beam "
	         "decelerates into them.",
	         "Shape" ),
	SLIDER( PT_SHAPE_RATE, "shapeRate", "Shape Rate", 0.5f,
	        "How many times a second the figure is drawn, 0.01 to 200." ),
	COUNT( PT_SHAPE_N, "sides", "Sides", 5.0f, 3.0f, 24.0f, "Polygon and star only." ),
	SLIDER( PT_SHAPE_INNER, "innerRadius", "Inner Radius", 0.382f, "Star only." ),
	COUNT( PT_PETAL_N, "petals", "Petals", 3.0f, 1.0f, 12.0f, "Rose only." ),
	COUNT( PT_PETAL_D, "petalDivisor", "Petal Divisor", 1.0f, 1.0f, 12.0f, "Rose only." ),
	SLIDER( PT_TROCHOID, "penOffset", "Pen Offset", 0.4f, "Spirograph only." ),

	//--- Wireframe ---------------------------------------------------------
	OPTIONG( PT_MESH, "mesh", "Mesh", 3.0f, kMeshNames, kMeshCount,
	         "The solid the beam walks. Hidden edges are blanked, not deleted -- the "
	         "beam still travels to them.",
	         "Wireframe" ),
	COUNT( PT_MESH_DETAIL, "meshDetail", "Mesh Detail", 8.0f, 3.0f, 24.0f,
	       "Segments per ring, or ridges per range." ),
	SLIDER( PT_SPIN_X, "spinX", "Spin X", 0.53f, "Turns per second, -2 to 2." ),
	SLIDER( PT_SPIN_Y, "spinY", "Spin Y", 0.55f, "Turns per second, -2 to 2." ),
	SLIDER( PT_SPIN_Z, "spinZ", "Spin Z", 0.5f, "Turns per second, -2 to 2." ),
	SLIDER( PT_CAMERA, "camera", "Camera", 0.33f, "How far back the eye sits." ),
	SLIDER( PT_SCROLL, "scroll", "Scroll", 0.625f, "Tunnel and mountains only." ),

	//--- Beam path ---------------------------------------------------------
	SLIDERG( PT_REFRESH, "refresh", "Refresh Rate", 0.5f,
	         "How many times a second the whole figure is drawn, 5 to 120. A figure "
	         "made of many strokes refreshes more slowly than one made of few, because "
	         "the blanked jumps between them are still travel.",
	         "Beam Path" ),
	SLIDER( PT_RETRACE, "retrace", "Retrace Speed", 0.37f,
	        "How much faster a blanked jump is than a drawn stroke." ),

	//--- Audio file --------------------------------------------------------
	{ PT_FILE, Kind::File, "audioFile", "Audio File",
	  "Oscilloscope music: WAV, AIFF, FLAC, MP3, Ogg or Opus. X is the left channel "
	  "and Y the right. Decoded to memory and truncated at eight minutes.",
	  0.0f, 0.0f, 1.0f, nullptr, 0, "Audio File" },
	SLIDER( PT_FILE_RATE, "fileRate", "File Rate", 0.5f, "0.05x to 4x the file's own rate." ),
	TOGGLE( PT_FILE_LOOP, "fileLoop", "Loop", 1.0f, "" ),
	SLIDER( PT_FILE_START, "fileStart", "Start", 0.0f, "" ),
	SLIDER( PT_FILE_END, "fileEnd", "End", 1.0f, "" ),
	OPTION( PT_FILE_MONO, "monoMode", "Mono Mode", 2.0f, kMonoModeNames, kMonoModeCount,
	        "What to put on Y when the file has only one channel." ),
	TOGGLE( PT_FILE_SWAP, "swapXY", "Swap X/Y", 0.0f, "" ),
	TOGGLE( PT_FILE_INVERT, "invertY", "Invert Y", 1.0f,
	        "Oscilloscope music is authored for a scope whose Y is inverted against "
	        "the convention here." ),
	TOGGLE( PT_FILE_SYNC, "lockToClip", "Lock to Clip", 0.0f,
	        "Pin the read position to the timeline instead of free-running, so "
	        "scrubbing scrubs the file." ),

	//--- Trace -------------------------------------------------------------
	SLIDERG( PT_TRACE_THRESHOLD, "edgeThreshold", "Edge Threshold", 0.35f,
	         "Trace Input only, and Trace Input does not run in an OpenFX host -- see "
	         "the Source control.",
	         "Trace" ),
	SLIDER( PT_TRACE_STABILITY, "stability", "Stability", 0.5f, "Trace Input only." ),
	SLIDER( PT_TRACE_SIMPLIFY, "simplify", "Simplify", 0.4f, "Trace Input only." ),
	COUNT( PT_TRACE_STROKES, "maxStrokes", "Max Strokes", 24.0f, 4.0f, 64.0f, "Trace Input only." ),

	//--- VCA ---------------------------------------------------------------
	TOGGLEG( PT_VCA_ON, "vca", "VCA", 0.0f, "", "VCA" ),
	SLIDER( PT_VCA_LEVEL, "vcaLevel", "Level", 0.5f, "0 to 2x. Past 1 the figure leaves the glass." ),
	OPTION( PT_VCA_ROUTING, "vcaRouting", "VCA Routing", 0.0f, kRoutingNames, kRoutingCount,
	        "Which axes the block acts on. Mid/Side works on the sum and difference, "
	        "which is a rotation of the axes by 45 degrees." ),

	//--- Gate --------------------------------------------------------------
	TOGGLEG( PT_GATE_ON, "gate", "Gate", 0.0f, "", "Gate" ),
	SLIDER( PT_GATE_THRESHOLD, "gateThreshold", "Gate Threshold", 0.33f, "-60 to 0 dB." ),
	SLIDER( PT_GATE_ATTACK, "gateAttack", "Gate Attack", 0.37f, "0.1 to 50 ms." ),
	SLIDER( PT_GATE_HOLD, "gateHold", "Gate Hold", 0.02f, "0 to 500 ms." ),
	SLIDER( PT_GATE_RELEASE, "gateRelease", "Gate Release", 0.57f, "1 ms to 1 s." ),
	TOGGLE( PT_GATE_MODE, "gateMutesBeam", "Gate Mutes Beam", 0.0f,
	        "Cut the gun rather than the deflection, so a closed gate leaves nothing "
	        "on the glass instead of parking a dot at the origin." ),

	//--- Compressor --------------------------------------------------------
	TOGGLEG( PT_COMP_ON, "compressor", "Compressor", 0.0f, "", "Compressor" ),
	SLIDER( PT_COMP_THRESHOLD, "compThreshold", "Threshold", 0.7f, "-40 to 0 dB." ),
	SLIDER( PT_COMP_RATIO, "compRatio", "Ratio", 0.46f, "1:1 to 20:1." ),
	SLIDER( PT_COMP_KNEE, "compKnee", "Knee", 0.25f, "0 to 24 dB." ),
	SLIDER( PT_COMP_ATTACK, "compAttack", "Attack", 0.57f, "0.1 to 100 ms." ),
	SLIDER( PT_COMP_RELEASE, "compRelease", "Release", 0.57f, "5 ms to 1 s." ),
	SLIDER( PT_COMP_MAKEUP, "compMakeup", "Makeup", 0.0f, "0 to 24 dB." ),
	TOGGLE( PT_COMP_AUTO, "compAutoMakeup", "Auto Makeup", 1.0f, "" ),
	SLIDER( PT_COMP_CEILING, "compCeiling", "Ceiling", 0.5f,
	        "The one explicit limiter in the signal path. Everything else is allowed "
	        "past full deflection." ),
	TOGGLE( PT_COMP_LIMIT_MODE, "railLimiting", "Rail Limiting", 0.0f,
	        "Clip at the ceiling the way an amplifier hitting its rails does, instead "
	        "of turning the gain down." ),
	OPTION( PT_COMP_ROUTING, "compRouting", "Comp Routing", 0.0f, kRoutingNames, kRoutingCount, "" ),

	//--- Rectifier ---------------------------------------------------------
	TOGGLEG( PT_RECT_ON, "rectifier", "Rectifier", 0.0f, "", "Rectifier" ),
	TOGGLE( PT_RECT_MODE, "fullWave", "Full Wave", 0.0f, "" ),
	SLIDER( PT_RECT_BIAS, "foldPoint", "Fold Point", 0.5f, "Where the fold happens, -1 to 1." ),
	OPTION( PT_RECT_ROUTING, "rectRouting", "Rectifier Routing", 1.0f, kRoutingNames, kRoutingCount, "" ),

	//--- Slew --------------------------------------------------------------
	TOGGLEG( PT_SLEW_ON, "slew", "Slew Limiter", 0.0f,
	         "The reason a sample and hold draws lines between its steps: the beam "
	         "travels rather than teleporting.",
	         "Slew" ),
	SLIDER( PT_SLEW_RISE, "slewRise", "Rise", 1.0f, "0.5 to 5000 volts per second." ),
	SLIDER( PT_SLEW_FALL, "slewFall", "Fall", 1.0f, "0.5 to 5000 volts per second." ),
	TOGGLE( PT_SLEW_LINK, "slewLink", "Link Rise/Fall", 1.0f, "" ),
	OPTION( PT_SLEW_ROUTING, "slewRouting", "Slew Routing", 0.0f, kRoutingNames, kRoutingCount, "" ),

	//--- Drive -------------------------------------------------------------
	TOGGLEG( PT_DRIVE_ON, "drive", "Drive", 0.0f, "", "Drive" ),
	SLIDER( PT_DRIVE_AMOUNT, "driveGain", "Gain", 0.0f, "0 to 40 dB into the shaper." ),
	SLIDER( PT_DRIVE_FOLD, "driveFold", "Fold", 0.0f,
	        "Blend from a saturator into a wavefolder. The oversampler is linear-phase "
	        "FIR on purpose: an IIR half-band's phase response is a skew of the figure." ),
	COUNT( PT_DRIVE_FOLDS, "folds", "Folds", 2.0f, 1.0f, 8.0f, "" ),
	TOGGLE( PT_DRIVE_OVERSAMPLE, "oversample", "Oversample", 1.0f, "" ),
	OPTION( PT_DRIVE_ROUTING, "driveRouting", "Drive Routing", 0.0f, kRoutingNames, kRoutingCount, "" ),

	//--- Ring modulator ----------------------------------------------------
	TOGGLEG( PT_RING_ON, "ringMod", "Ring Mod", 0.0f, "", "Ring Modulator" ),
	SLIDER( PT_RING_FREQ, "carrier", "Carrier", 0.65f, "0.1 Hz to 2 kHz." ),
	OPTION( PT_RING_WAVE, "carrierWave", "Carrier Wave", 0.0f, kRingWaveNames, 3, "" ),
	SLIDER( PT_RING_DEPTH, "ringDepth", "Ring Depth", 0.0f, "" ),
	TOGGLE( PT_RING_LOCK, "ratioLock", "Ratio Lock", 0.0f,
	        "Tie the carrier to the source's frequency, so the figure stops crawling." ),
	OPTION( PT_RING_ROUTING, "ringRouting", "Ring Routing", 5.0f, kRoutingNames, kRoutingCount,
	        "Cross by default: X modulated by Y and Y by X, which is what makes this a "
	        "geometric warp rather than a tremolo." ),

	//--- Bitcrush ----------------------------------------------------------
	TOGGLEG( PT_CRUSH_ON, "bitcrush", "Bitcrush", 0.0f, "", "Bitcrush" ),
	COUNT( PT_CRUSH_BITS, "bits", "Bits", 16.0f, 1.0f, 16.0f, "" ),
	SLIDER( PT_CRUSH_RATE, "crushRate", "Sample Rate", 1.0f,
	        "100 Hz to 48 kHz. At the top of its travel the rate reducer is off, not "
	        "merely fast -- an almost-off sample and hold still beats against the "
	        "source and crawls." ),
	OPTION( PT_CRUSH_ROUTING, "crushRouting", "Crush Routing", 0.0f, kRoutingNames, kRoutingCount, "" ),

	//--- Phaser ------------------------------------------------------------
	TOGGLEG( PT_PHASE_ON, "phaser", "Phaser", 0.0f, "", "Phaser" ),
	COUNT( PT_PHASE_STAGES, "stages", "Stages", 4.0f, 1.0f, 12.0f, "" ),
	SLIDER( PT_PHASE_RATE, "phaserRate", "Phaser Rate", 0.5f, "0.01 to 10 Hz." ),
	SLIDER( PT_PHASE_DEPTH, "phaserDepth", "Phaser Depth", 0.7f, "" ),
	SLIDER( PT_PHASE_CENTRE, "phaserCentre", "Centre", 0.47f, "100 Hz to 8 kHz." ),
	SLIDER( PT_PHASE_FEEDBACK, "phaserFeedback", "Phaser Feedback", 0.53f, "" ),
	SLIDER( PT_PHASE_MIX, "phaserMix", "Phaser Mix", 0.5f, "" ),
	OPTION( PT_PHASE_ROUTING, "phaserRouting", "Phaser Routing", 1.0f, kRoutingNames, kRoutingCount, "" ),

	//--- Flanger -----------------------------------------------------------
	TOGGLEG( PT_FLANGE_ON, "flanger", "Flanger", 0.0f, "", "Flanger" ),
	SLIDER( PT_FLANGE_RATE, "flangerRate", "Flanger Rate", 0.5f, "0.05 to 5 Hz." ),
	SLIDER( PT_FLANGE_DEPTH, "flangerDepth", "Flanger Depth", 0.7f, "" ),
	SLIDER( PT_FLANGE_DELAY, "flangerDelay", "Flanger Delay", 0.65f, "0.1 to 10 ms." ),
	SLIDER( PT_FLANGE_FEEDBACK, "flangerFeedback", "Flanger Feedback", 0.87f, "-0.95 to 0.95." ),
	SLIDER( PT_FLANGE_MIX, "flangerMix", "Flanger Mix", 0.5f, "" ),
	OPTION( PT_FLANGE_ROUTING, "flangerRouting", "Flanger Routing", 0.0f, kRoutingNames, kRoutingCount, "" ),

	//--- Chorus ------------------------------------------------------------
	TOGGLEG( PT_CHORUS_ON, "chorus", "Chorus", 0.0f,
	         "Y's modulation runs a quarter cycle behind X's. Running them in phase "
	         "would displace every copy along the 45 degree diagonal and pile three of "
	         "them into one blurry double image.",
	         "Chorus" ),
	SLIDER( PT_CHORUS_RATE, "chorusRate", "Chorus Rate", 0.5f, "0.05 to 3 Hz." ),
	SLIDER( PT_CHORUS_DEPTH, "chorusDepth", "Chorus Depth", 0.5f, "" ),
	SLIDER( PT_CHORUS_DELAY, "chorusDelay", "Chorus Delay", 0.33f, "10 to 40 ms." ),
	SLIDER( PT_CHORUS_MIX, "chorusMix", "Chorus Mix", 0.4f, "" ),

	//--- Delay -------------------------------------------------------------
	TOGGLEG( PT_DELAY_ON, "delay", "Delay", 0.0f,
	         "Each repeat is smoother and rounder-cornered than the last, because the "
	         "feedback path is low-passed and a low-passed deflection signal has softer "
	         "corners. Nothing draws a faded copy.",
	         "Delay" ),
	TOGGLE( PT_DELAY_SYNC, "delaySync", "Sync to Tempo", 0.0f,
	        "OpenFX carries no transport tempo, so this build works off a fixed 120 bpm. "
	        "Delay Time is the control that behaves identically in Resolume and here." ),
	SLIDER( PT_DELAY_TIME, "delayTime", "Delay Time", 0.7f, "1 ms to 2.5 s." ),
	OPTION( PT_DELAY_DIVISION, "delayDivision", "Division", 4.0f,
	        kDelayDivisionNames, kDelayDivisionCount, "Against 120 bpm here. See Sync to Tempo." ),
	SLIDER( PT_DELAY_FEEDBACK, "delayFeedback", "Delay Feedback", 0.43f, "Up to 1.05, which runs away." ),
	SLIDER( PT_DELAY_DAMP_LOW, "delayTone", "Delay Tone", 0.65f, "" ),
	SLIDER( PT_DELAY_DAMP_HIGH, "delayBassCut", "Delay Bass Cut", 0.41f, "" ),
	SLIDER( PT_DELAY_MIX, "delayMix", "Delay Mix", 0.4f, "" ),
	OPTION( PT_DELAY_ROUTING, "delayRouting", "Delay Routing", 0.0f, kRoutingNames, kRoutingCount, "" ),
	TOGGLE( PT_DELAY_TIME_MODE, "delayCrossfade", "Crossfade Time Changes", 0.0f,
	        "Crossfade rather than repitch when the time changes. A real bucket brigade "
	        "repitches, which is the default." ),

	//--- Reverb ------------------------------------------------------------
	TOGGLEG( PT_VERB_ON, "reverb", "Reverb", 0.0f, "", "Reverb" ),
	SLIDER( PT_VERB_PREDELAY, "preDelay", "Pre-Delay", 0.1f, "0 to 200 ms." ),
	SLIDER( PT_VERB_DECAY, "verbDecay", "Decay", 0.57f, "0.1 to 20 s." ),
	SLIDER( PT_VERB_DAMPING, "verbDamping", "Damping", 0.5f, "" ),
	SLIDER( PT_VERB_DIFFUSION, "verbDiffusion", "Diffusion", 0.78f, "" ),
	SLIDER( PT_VERB_SIZE, "verbSize", "Size", 0.66f, "" ),
	SLIDER( PT_VERB_MOD, "shimmer", "Shimmer", 0.2f, "" ),
	SLIDER( PT_VERB_MIX, "verbMix", "Reverb Mix", 0.3f, "" ),

	//--- Deflection amplifier ----------------------------------------------
	SLIDERG( PT_OUT_GAIN, "deflection", "Deflection", 0.5f,
	         "0 to 2x. Full deflection is nominal, not a limit -- the chain is allowed "
	         "to push the figure off the glass, which is what an overdriven amplifier "
	         "into a yoke does.",
	         "Deflection Amplifier" ),
	SLIDER( PT_OUT_OFFSET_X, "offsetX", "Offset X", 0.5f, "" ),
	SLIDER( PT_OUT_OFFSET_Y, "offsetY", "Offset Y", 0.5f, "" ),
	SLIDER( PT_OUT_ROTATION, "rotation", "Rotation", 0.5f, "" ),
	SLIDER( PT_OUT_SKEW, "skew", "Skew", 0.5f, "" ),
	TOGGLE( PT_OUT_DC, "dcBlock", "DC Block", 1.0f, "" ),
	SLIDER( PT_OUT_SLEW, "ampSlew", "Amp Slew", 0.70f,
	        "The amplifier's own slew limit. This and the bandwidth below are where "
	        "corner brightening comes from -- a real machine puts them here too." ),
	SLIDER( PT_OUT_BANDWIDTH_X, "bandwidthX", "Bandwidth X", 0.84f, "1 to 80 kHz." ),
	SLIDER( PT_OUT_BANDWIDTH_Y, "bandwidthY", "Bandwidth Y", 0.66f,
	        "1 to 80 kHz. Lower than X on a real yoke, because the vertical coils are "
	        "heavier." ),
	SLIDER( PT_OUT_RESONANCE, "dampingFactor", "Damping Factor", 0.06f,
	        "How the deflection stage rings on a step." ),

	//--- Beam --------------------------------------------------------------
	SLIDERG( PT_BEAM_POWER, "beam", "Beam", 0.45f,
	         "Energy per second of beam-on time, exponentially around a calibrated 1.0. "
	         "Not a brightness: the same energy over a longer stroke is a dimmer line.",
	         "Beam" ),
	SLIDER( PT_BEAM_FOCUS, "focus", "Focus", 0.30f,
	        "The spot's sigma, 0.0012 to 0.02 of half the face height. This also sizes "
	        "the face buffer, so it is the control that decides how long a frame takes." ),
	SLIDER( PT_BEAM_DEFOCUS, "blooming", "Blooming", 0.35f,
	        "How much a full-current beam swells. The same reason a CRT's highlights do." ),
	SLIDER( PT_BLANK_FLOOR, "blankingLeak", "Blanking Leak", 0.0f,
	        "What a fully cut-off gun still puts on the glass. A retrace that leaves "
	        "absolutely nothing behind looks wrong in a way people describe as too clean." ),

	//--- Tube --------------------------------------------------------------
	OPTIONG( PT_PHOSPHOR, "phosphor", "Phosphor", 0.0f, kPhosphorNames, kPhosphorCount,
	         "A measured table of real JEDEC types, not a set of tints. Two things "
	         "follow and both read as bugs: P31 at unity persistence shows no trail at "
	         "all -- its decay constant is sixteen microseconds against a sixteen "
	         "millisecond frame -- and changing type changes the brightness by up to "
	         "three times, because P11 has about a third of P31's luminous efficiency.",
	         "Tube" ),
	SLIDER( PT_PERSISTENCE, "persistence", "Persistence", 0.35f,
	        "A multiplier on the phosphor's decay constant, x0.1 to x1000. A multiplier "
	        "on tau rather than a per-frame factor, so the trail is the same length at "
	        "any frame rate." ),
	SLIDER( PT_HALATION, "halation", "Halation", 0.35f,
	        "Light scattering sideways inside the faceplate before it gets out." ),
	SLIDER( PT_HALATION_RADIUS, "halationRadius", "Halation Radius", 0.5f, "" ),
	SLIDER( PT_FACE_ASPECT, "faceAspect", "Face Aspect", 0.0f,
	        "1:1 for a lab scope, 4:3 for a vector monitor. The only thing that knows "
	        "the face is not square, so the spot stays round on a wide face." ),
	SLIDER( PT_CORNER_RADIUS, "cornerRadius", "Corner Radius", 1.0f,
	        "Radius 1 on a square face already is a circle: the distance field reduces "
	        "exactly to length(p) - 1. A round tube is not a special case." ),
	SLIDER( PT_DEFLECTION_GAIN, "screenSize", "Screen Size", 0.39f,
	        "Beam units per volt. Under 1 underscans, so full deflection lands inside "
	        "the glass and an overdriven signal is visibly overdriven." ),
	SLIDER( PT_CURVATURE, "curvature", "Curvature", 0.0f, "How much the glass bulges." ),
	SLIDER( PT_VIGNETTE, "vignette", "Vignette", 0.2f, "" ),
	SLIDER( PT_FACE_BLACK, "faceBlack", "Face Black", 0.85f,
	        "How much of the faceplate is actually in front of what is behind it. Zero "
	        "is exact passthrough, by construction rather than by tuning." ),
	SLIDER( PT_FILTER_TINT, "contrastFilter", "Contrast Filter", 0.5f,
	        "Interpolates between clear glass and a filter the colour of the phosphor, "
	        "which is how a real scope's filter kills ambient light without killing the "
	        "trace." ),
	SLIDER( PT_GRATICULE, "graticule", "Graticule", 0.25f,
	        "10 by 8 divisions inscribed in the face, solved against the same distance "
	        "field the faceplate uses. Fades out below three output pixels a division, "
	        "where it would be a moire generator." ),

	//--- Output ------------------------------------------------------------
	SLIDERG( PT_MIX, "mix", "Mix", 1.0f, "The plugin's own opacity.", "Output" ),
	OPTION( PT_PRESET, "preset", "Preset", 0.0f, nullptr, 0,
	        "Whole instruments, not slider positions that happened to look good: a "
	        "source, a set of pedals in a particular order, and a tube. Picking one "
	        "sets the controls it covers; editing any of them afterwards falls back to "
	        "Custom. Never touches Seed, the file path, Detail or the tube's framing." ),

	//--- Modulation --------------------------------------------------------
	//
	// PT_AUDIO_FFT has no OpenFX equivalent -- there is no route from a host's
	// audio to a video plugin -- so it is not declared. The slots and bands are,
	// and sum to zero. The two LFOs need nothing but a frame duration and work.
	{ PT_AUDIO_FFT, Kind::Absent, "audioFft", "Audio", "", 0.0f, 0.0f, 1.0f, nullptr, 0, nullptr },
	SLIDERG( PT_LFO1_RATE, "lfo1Rate", "LFO 1 Rate", 0.3f, "0.01 to 20 Hz.", "Modulation" ),
	SLIDER( PT_LFO1_DEPTH, "lfo1Depth", "LFO 1 Depth", 0.0f,
	        "An LFO drives whichever Mod slot names a target, so an LFO with no slot "
	        "pointed at it does nothing rather than modulating everything at once." ),
	SLIDER( PT_LFO2_RATE, "lfo2Rate", "LFO 2 Rate", 0.5f, "0.01 to 20 Hz." ),
	SLIDER( PT_LFO2_DEPTH, "lfo2Depth", "LFO 2 Depth", 0.0f, "" ),

	OPTION( PT_MOD1_TARGET, "mod1", "Mod 1", 0.0f, kModTargetNames, kModTargetCount, "" ),
	SLIDER( PT_MOD1_AMOUNT, "mod1Amount", "Mod 1 Amount", 0.5f,
	        "Centred: -1 to 1, so a slot can subtract, and the middle is off." ),
	OPTION( PT_MOD1_BAND, "mod1Band", "Mod 1 Band", 0.0f, kBandNames, kBandCount,
	        "Which slice of the spectrum. Inert in OpenFX -- there is no audio here -- "
	        "so a slot with a band and no LFO sums to zero." ),
	OPTION( PT_MOD2_TARGET, "mod2", "Mod 2", 0.0f, kModTargetNames, kModTargetCount, "" ),
	SLIDER( PT_MOD2_AMOUNT, "mod2Amount", "Mod 2 Amount", 0.5f, "" ),
	OPTION( PT_MOD2_BAND, "mod2Band", "Mod 2 Band", 0.0f, kBandNames, kBandCount, "" ),
	OPTION( PT_MOD3_TARGET, "mod3", "Mod 3", 0.0f, kModTargetNames, kModTargetCount, "" ),
	SLIDER( PT_MOD3_AMOUNT, "mod3Amount", "Mod 3 Amount", 0.5f, "" ),
	OPTION( PT_MOD3_BAND, "mod3Band", "Mod 3 Band", 0.0f, kBandNames, kBandCount, "" ),
	OPTION( PT_MOD4_TARGET, "mod4", "Mod 4", 0.0f, kModTargetNames, kModTargetCount, "" ),
	SLIDER( PT_MOD4_AMOUNT, "mod4Amount", "Mod 4 Amount", 0.5f, "" ),
	OPTION( PT_MOD4_BAND, "mod4Band", "Mod 4 Band", 0.0f, kBandNames, kBandCount, "" ),

	//--- About -------------------------------------------------------------
	//
	// The FFGL build's About block is a text line plus one button per link, and
	// a button that opens a browser has no place in a render node. The links are
	// in the plugin description instead.
	{ PT_ABOUT_TEXT, Kind::Absent, "about", "About", "", 0.0f, 0.0f, 1.0f, nullptr, 0, nullptr },
	{ PT_ABOUT_BUTTON_1, Kind::Absent, "about1", "", "", 0.0f, 0.0f, 1.0f, nullptr, 0, nullptr },
	{ PT_ABOUT_BUTTON_2, Kind::Absent, "about2", "", "", 0.0f, 0.0f, 1.0f, nullptr, 0, nullptr },
	{ PT_ABOUT_BUTTON_3, Kind::Absent, "about3", "", "", 0.0f, 0.0f, 1.0f, nullptr, 0, nullptr },
};

#undef SLIDER
#undef SLIDERG
#undef TOGGLE
#undef TOGGLEG
#undef COUNT
#undef OPTION
#undef OPTIONG

constexpr int kDeclCount = static_cast< int >( sizeof( kDecls ) / sizeof( kDecls[ 0 ] ) );

/// One declaration per parameter id, in id order, or the panel is a different
/// plugin from the one the presets were written for.
static_assert( kDeclCount == static_cast< int >( PT_COUNT ),
               "the OFX declaration table and ParamId disagree" );

//---------------------------------------------------------------------------
// The parameters a preset covers, in the order presets::Param declares them.
//
// The same list Vectrix.cpp keeps, for the same reason: the static_assert is
// what stops the two drifting apart, which would otherwise show up as a preset
// quietly writing its Fold value into the Ring Depth slider.
//---------------------------------------------------------------------------
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

} // namespace

//===========================================================================
// The plugin instance.
//===========================================================================
namespace
{
class VectrixOFXPlugin : public OFX::ImageEffect
{
public:
	VectrixOFXPlugin( OfxImageEffectHandle handle, bool overInput ) :
		OFX::ImageEffect( handle ),
		over( overInput )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		if( over )
			srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		for( const Decl& d : kDecls )
		{
			params[ d.id ] = d.def;
			switch( d.kind )
			{
			case Kind::Slider: handles[ d.id ] = fetchDoubleParam( d.name ); break;
			case Kind::Count:  handles[ d.id ] = fetchIntParam( d.name ); break;
			case Kind::Toggle: handles[ d.id ] = fetchBooleanParam( d.name ); break;
			case Kind::Option: handles[ d.id ] = fetchChoiceParam( d.name ); break;
			case Kind::File:   handles[ d.id ] = fetchStringParam( d.name ); break;
			case Kind::Button:
			case Kind::Absent:
			default: handles[ d.id ] = nullptr; break;
			}
		}
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > src;
		if( over && srcClip != nullptr && srcClip->isConnected() )
			src.reset( srcClip->fetchImage( args.time ) );

		if( dst == nullptr )
			OFX::throwSuiteStatusException( kOfxStatFailed );

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();
		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		const OfxRectI bounds = dst->getBounds();
		const int outW        = std::max( 1, bounds.x2 - bounds.x1 );
		const int outH        = std::max( 1, bounds.y2 - bounds.y1 );

		GlassSetup setup;
		simulate( args, outW, outH, setup );

		setup.hasClip = over && src != nullptr;

		switch( depth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA
				? run< VectrixProcessor< unsigned char, 4, 255 > >( args, dst.get(), src.get(), setup )
				: run< VectrixProcessor< unsigned char, 3, 255 > >( args, dst.get(), src.get(), setup );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA
				? run< VectrixProcessor< unsigned short, 4, 65535 > >( args, dst.get(), src.get(), setup )
				: run< VectrixProcessor< unsigned short, 3, 65535 > >( args, dst.get(), src.get(), setup );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA
				? run< VectrixProcessor< float, 4, 1 > >( args, dst.get(), src.get(), setup )
				: run< VectrixProcessor< float, 3, 1 > >( args, dst.get(), src.get(), setup );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		if( applyingPreset )
			return;

		const Decl* decl = find( paramName );
		if( decl == nullptr )
			return;

		// ANY edit throws the replay away, not just the obviously structural ones.
		//
		// The picture at a frame is meant to be a function of (frame, parameters)
		// evaluated by replaying a warm-up window into it, and a simulation already
		// in flight was run under the old values. Continuing it would leave the
		// delay carrying a tail the current settings could never have produced --
		// which is the sort of difference that only shows up when somebody renders
		// the same timeline twice and gets two files.
		simulatedFrame = kNoFrame;

		if( decl->id == PT_RESET )
			return;

		if( decl->id == PT_PRESET )
		{
			applyPreset( args );
			return;
		}

		// Editing anything a preset covers falls back to Custom. Judged by
		// comparing values rather than by the change reason, so a host echoing our
		// own writes cannot un-set the preset.
		OFX::ChoiceParam* preset = static_cast< OFX::ChoiceParam* >( handles[ PT_PRESET ] );
		int active = 0;
		preset->getValue( active );
		if( active <= 0 || active > presets::kCount )
			return;

		const presets::Preset& p = presets::kPresets[ active - 1 ];
		for( int j = 0; j < presets::kParamCount; ++j )
		{
			if( kPresetParamIDs[ j ] != decl->id )
				continue;
			if( std::fabs( readParam( decl->id, args.time ) - p.v[ j ] ) > 1e-6f )
			{
				applyingPreset = true;
				preset->setValue( 0 );
				applyingPreset = false;
			}
			break;
		}
	}

private:
	static constexpr long long kNoFrame = -1000000000LL;

	static const Decl* find( const std::string& name )
	{
		for( const Decl& d : kDecls )
			if( d.kind != Kind::Absent && name == d.name )
				return &d;
		return nullptr;
	}

	template< class PROC >
	void run( const OFX::RenderArguments& args, OFX::Image* dst, OFX::Image* src, const GlassSetup& setup )
	{
		PROC processor( *this );
		processor.setDstImg( dst );
		processor.setSetup( &setup, src );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	//-----------------------------------------------------------------------
	// Reading the host's controls back into the 0..1 array Controls.cpp wants.
	//
	// The FFGL build holds exactly this array and hands it to `Resolve`; doing
	// the same here is what makes the mapping -- every hertz, every millisecond,
	// every dB -- shared rather than mirrored.
	//-----------------------------------------------------------------------
	float readParam( unsigned int id, double time ) const
	{
		const Decl& d = kDecls[ id ];
		switch( d.kind )
		{
		case Kind::Slider:
			return static_cast< float >(
				static_cast< OFX::DoubleParam* >( handles[ id ] )->getValueAtTime( time ) );
		case Kind::Count:
			return static_cast< float >(
				static_cast< OFX::IntParam* >( handles[ id ] )->getValueAtTime( time ) );
		case Kind::Toggle:
			return static_cast< OFX::BooleanParam* >( handles[ id ] )->getValueAtTime( time ) ? 1.0f : 0.0f;
		case Kind::Option:
		{
			int chosen = 0;
			static_cast< OFX::ChoiceParam* >( handles[ id ] )->getValueAtTime( time, chosen );
			//An option parameter holds its element VALUE, which for every dropdown
			//here is its index. Getting this backwards gives a control permanently
			//stuck on its first entry.
			return static_cast< float >( chosen );
		}
		case Kind::File:
		case Kind::Button:
		case Kind::Absent:
		default:
			return d.def;
		}
	}

	void readAll( double time )
	{
		for( const Decl& d : kDecls )
			params[ d.id ] = readParam( d.id, time );
	}

	//-----------------------------------------------------------------------
	// The replay. See the header comment.
	//-----------------------------------------------------------------------
	void simulate( const OFX::RenderArguments& args, int outW, int outH, GlassSetup& setup )
	{
		// OFX hands render time in frames. A host that reports no frame rate gets
		// 25 -- wrong somewhere, but never zero, which would make every frame the
		// first one.
		double fps = dstClip != nullptr ? dstClip->getFrameRate() : 0.0;
		if( !( fps > 0.0 ) )
			fps = 25.0;

		// Clamped the way Clock::Update clamps its own delta, and for the same
		// reason: a frame long enough to ask the engine for half a second of audio
		// in one block is a thirty-times energy deposit and a white flash.
		const double frameSeconds = std::clamp( 1.0 / fps, 1.0 / 240.0, 1.0 / 24.0 );

		readAll( args.time );

		std::string wantedFile;
		if( handles[ PT_FILE ] != nullptr )
			static_cast< OFX::StringParam* >( handles[ PT_FILE ] )->getValue( wantedFile );

		const long long wantFrame = static_cast< long long >( std::llround( args.time ) );

		const bool restart = simulatedFrame == kNoFrame
		                  || wantFrame < simulatedFrame
		                  || wantFrame - simulatedFrame > kWarmUpFrames
		                  || wantedFile != loadedFile;

		if( restart )
		{
			engine.Prepare( static_cast< Detail >( Option( params[ PT_DETAIL ], kDetailCount ) ) );
			engine.Reset();
			modulation = Modulation{};
			tube.Clear();

			engine.File().SetPath( wantedFile );
			engine.SwapContent();
			loadedFile = wantedFile;

			simulatedFrame = std::max( 0LL, wantFrame - kWarmUpFrames ) - 1;
		}

		// Step forward to the frame asked for. Every frame but the last is a
		// warm-up: the engine is advanced and the phosphor decays, but nothing is
		// asked of the halation or the glass.
		//
		// Each step reads the controls at its OWN time rather than holding the
		// requested frame's values through the window. That costs a couple of
		// hundred host parameter reads per warmed-up frame, and it buys the thing
		// the plugin is actually about: a keyframed frequency sweep arrives at this
		// frame having genuinely swept, so the delay and the phosphor carry the
		// history the edit describes rather than the history the current knob
		// positions would have produced.
		while( simulatedFrame < wantFrame )
		{
			++simulatedFrame;
			//The last step is the frame the host asked for, which may be fractional.
			const double frameTime = simulatedFrame == wantFrame
			                             ? args.time
			                             : static_cast< double >( simulatedFrame );
			readAll( frameTime );
			step( frameTime / fps, frameSeconds, outH );
		}

		// This frame's halation and glass, from the excitation the last step left.
		const RenderParams rp = renderParams( lastResolved, frameSeconds );

		const float faceAspect   = std::max( rp.tube.faceAspect, 0.05f );
		const float outputAspect = static_cast< float >( outW ) / static_cast< float >( outH );
		const float faceFit      = faceFitScale( outputAspect, faceAspect );
		const float division     = graticuleDivision( rp.tube );

		// One square output unit is half the output height in pixels, so this is
		// how many output pixels a division is worth. Below about three of them the
		// graticule is a moire generator, so it fades -- and it fades on this one
		// number rather than on each pass's own derivative, because the bright pass
		// runs at quarter size and a local fade would take the halo away two rungs
		// before the sharp copy and leave lines casting no light at all.
		const float pixelsPerDivision = division * faceFit * static_cast< float >( outH ) * 0.5f;
		const float graticuleLevel    = std::max( rp.graticule, 0.0f )
		                             * smoothstep( 2.0f, 4.0f, pixelsPerDivision );

		Prelude pre;
		pre.spec           = &vectrix::phosphor( rp.phosphor );
		pre.graticuleLevel = graticuleLevel;
		pre.graticuleDiv   = division;
		for( int k = 0; k < 3; ++k )
			pre.graticuleColour[ k ] = rp.graticuleColour[ k ];

		tube.RenderHalation( rp, pre );

		setup.phosphor = &tube.Phosphor();
		setup.bloom    = &tube.Bloom();
		setup.prelude  = pre;
		setup.outputW  = static_cast< float >( outW );
		setup.outputH  = static_cast< float >( outH );
		setup.aspect   = outputAspect;
		setup.faceHalfX = faceAspect;
		setup.faceHalfY = 1.0f;
		setup.faceFit   = faceFit;
		setup.cornerRadius = std::clamp( rp.tube.cornerRadius, 0.0f, 1.0f );
		setup.curvature    = std::max( rp.tube.curvature, 0.0f );
		setup.vignette     = std::clamp( rp.tube.vignette, 0.0f, 1.0f );
		setup.halation     = tube.HaloReady() ? rp.halation : 0.0f;
		for( int k = 0; k < 3; ++k )
			setup.filterTransmission[ k ] = rp.filterTransmission[ k ];
		setup.faceBlack = std::clamp( rp.faceBlack, 0.0f, 1.0f );
		setup.opacity   = std::clamp( rp.opacity, 0.0f, 1.0f );
	}

	/// One frame of the simulation: modulation, resolve, engine, deposit.
	void step( double nowSeconds, double frameSeconds, int outputHeight )
	{
		// No FFT anywhere in OpenFX, so the bins are zero -- but this call is also
		// what advances the two LFOs, which need nothing else, so it is made rather
		// than skipped.
		float bins[ kAudioBins ] = {};
		modulation.Update( bins, kAudioBins, frameSeconds );

		modulation.SetLfo( 0, Exponential( params[ PT_LFO1_RATE ], 0.01f, 20.0f ), params[ PT_LFO1_DEPTH ] );
		modulation.SetLfo( 1, Exponential( params[ PT_LFO2_RATE ], 0.01f, 20.0f ), params[ PT_LFO2_DEPTH ] );

		const unsigned int target[ 4 ] = { PT_MOD1_TARGET, PT_MOD2_TARGET, PT_MOD3_TARGET, PT_MOD4_TARGET };
		const unsigned int amount[ 4 ] = { PT_MOD1_AMOUNT, PT_MOD2_AMOUNT, PT_MOD3_AMOUNT, PT_MOD4_AMOUNT };
		const unsigned int band[ 4 ]   = { PT_MOD1_BAND, PT_MOD2_BAND, PT_MOD3_BAND, PT_MOD4_BAND };

		for( int slot = 0; slot < 4; ++slot )
		{
			ModSlot s;
			s.target = static_cast< ModTarget >( Option( params[ target[ slot ] ], kModTargetCount ) );
			//Centred: -1..1 so a slot can subtract, and 0.5 is off. A 0..1 amount
			//would make "no modulation" an end stop.
			s.amount = Linear( params[ amount[ slot ] ], -1.0f, 1.0f );
			s.band   = static_cast< Band >( Option( params[ band[ slot ] ], kBandCount ) );
			modulation.SetSlot( slot, s );
		}

		// A fixed 120: OFX has no transport tempo. Only the delay's Sync to Tempo
		// mode reads it.
		lastResolved = Resolve( params, modulation, 120.0 );
		engine.SetParams( lastResolved );
		engine.Advance( frameSeconds, nowSeconds );

		const int n = std::clamp( static_cast< int >( std::lround( frameSeconds * engine.SampleRate() ) ),
		                          2, kMaxBlock );
		const Sample* samples = engine.Render( n, frameSeconds );

		tube.RenderFace( samples, n, renderParams( lastResolved, frameSeconds ), outputHeight );
	}

	/// The FFGL build's `VectrixPlugin::renderParams`, line for line.
	RenderParams renderParams( const Resolved& resolved, double frameSeconds ) const
	{
		RenderParams rp;

		//Exponential around a calibrated 1.0, the same shape resolume-scopes uses
		//for its trace gain and for the same reason: the useful range is wide.
		rp.beamPower = std::pow( 10.0f, -1.0f + 2.0f * std::clamp( params[ PT_BEAM_POWER ]
		                                                           + modulation.For( ModTarget::BeamPower ), 0.0f, 1.0f ) );
		rp.spotSigma   = Exponential( params[ PT_BEAM_FOCUS ], 0.0012f, 0.02f );
		rp.spotDefocus = Linear( params[ PT_BEAM_DEFOCUS ], 0.0f, 2.0f );
		rp.blankFloor  = Linear( params[ PT_BLANK_FLOOR ], 0.0f, 0.1f );

		rp.phosphor    = Option( params[ PT_PHOSPHOR ], kPhosphorCount );
		rp.persistence = Exponential( std::clamp( params[ PT_PERSISTENCE ]
		                                          + modulation.For( ModTarget::Persistence ), 0.0f, 1.0f ),
		                              0.1f, 1000.0f );

		rp.halation       = std::clamp( params[ PT_HALATION ], 0.0f, 1.0f );
		rp.halationRadius = std::clamp( params[ PT_HALATION_RADIUS ], 0.0f, 1.0f );

		rp.tube.faceAspect     = Exponential( params[ PT_FACE_ASPECT ], 1.0f, 2.0f );
		rp.tube.cornerRadius   = std::clamp( params[ PT_CORNER_RADIUS ], 0.0f, 1.0f );
		rp.tube.deflectionGain = Linear( params[ PT_DEFLECTION_GAIN ], 0.4f, 1.4f );
		rp.tube.curvature      = Linear( params[ PT_CURVATURE ], 0.0f, 0.6f );
		rp.tube.vignette       = std::clamp( params[ PT_VIGNETTE ], 0.0f, 1.0f );

		rp.graticule = std::clamp( params[ PT_GRATICULE ], 0.0f, 1.0f );
		rp.faceBlack = std::clamp( params[ PT_FACE_BLACK ], 0.0f, 1.0f );
		rp.opacity   = resolved.mix;

		//The contrast filter, interpolated between clear glass and a filter the
		//colour of the phosphor.
		const float tint = std::clamp( params[ PT_FILTER_TINT ], 0.0f, 1.0f );
		for( int c = 0; c < 3; ++c )
			rp.filterTransmission[ c ] = 1.0f - tint * ( 1.0f - rp.filterTransmission[ c ] );

		rp.frameSeconds = static_cast< float >( frameSeconds );

		return rp;
	}

	//-----------------------------------------------------------------------
	// Presets. The same table the FFGL build reads, applied inside one edit
	// block so undo takes the whole preset back at once.
	//-----------------------------------------------------------------------
	void applyPreset( const OFX::InstanceChangedArgs& args )
	{
		OFX::ChoiceParam* preset = static_cast< OFX::ChoiceParam* >( handles[ PT_PRESET ] );
		int chosen = 0;
		preset->getValue( chosen );
		if( chosen <= 0 || chosen > presets::kCount )
			return;//Custom: the sliders keep whatever they said

		const presets::Preset& p = presets::kPresets[ chosen - 1 ];

		applyingPreset = true;
		beginEditBlock( "preset" );

		for( int j = 0; j < presets::kParamCount; ++j )
			writeParam( kPresetParamIDs[ j ], p.v[ j ] );

		endEditBlock();
		applyingPreset = false;

		// The DSP state a preset lands on is not the state it would have reached
		// had it always been set, so the replay starts again.
		simulatedFrame = kNoFrame;
		( void )args;
	}

	/// Write one preset value into the host's control, in that control's own
	/// type. The table stores option parameters as their element value and
	/// everything else as the raw number `Resolve` reads, which is exactly what
	/// the FFGL build's `params[]` array holds -- so the two builds land on the
	/// same resolved value even where the table's own number is odd.
	void writeParam( unsigned int id, float value )
	{
		const Decl& d = kDecls[ id ];
		switch( d.kind )
		{
		case Kind::Slider:
			static_cast< OFX::DoubleParam* >( handles[ id ] )->setValue( value );
			break;
		case Kind::Count:
			static_cast< OFX::IntParam* >( handles[ id ] )->setValue(
				static_cast< int >( std::lround( value ) ) );
			break;
		case Kind::Toggle:
			static_cast< OFX::BooleanParam* >( handles[ id ] )->setValue( value > 0.5f );
			break;
		case Kind::Option:
			static_cast< OFX::ChoiceParam* >( handles[ id ] )->setValue(
				std::max( 0, static_cast< int >( std::lround( value ) ) ) );
			break;
		default:
			break;
		}
	}

	const bool over;
	bool applyingPreset = false;

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::Param* handles[ PT_COUNT ] = {};
	float params[ PT_COUNT ]        = {};

	Engine engine;
	Modulation modulation;
	TubeRenderer tube;
	Resolved lastResolved;

	long long simulatedFrame = kNoFrame;
	std::string loadedFile;
};

//---------------------------------------------------------------------------
// Description
//---------------------------------------------------------------------------
void describeCommon( OFX::ImageEffectDescriptor& desc, const char* name )
{
	desc.setLabels( name, name, name );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// Tiles are declined because the beam is placed in the face's own
	// coordinates and a tile does not know the whole frame's geometry -- and
	// because the trace pass is a whole-face accumulation before any pixel of the
	// output exists.
	desc.setSupportsTiles( false );
	desc.setSupportsMultiResolution( true );

	// The replay carries state from frame to frame and is therefore not temporal
	// clip access: the plugin never reads another frame's image, it re-runs its
	// own simulation. Saying otherwise would make a host fetch images nothing
	// looks at.
	desc.setTemporalClipAccess( false );

	// Instance safe, not fully safe. The engine, the modulation and the phosphor
	// history are per-instance mutable state, and two concurrent renders of the
	// same instance would be two threads stepping the same delay line. The pixel
	// pass underneath is still multi-threaded, which is where the pixels are.
	desc.setRenderThreadSafety( OFX::eRenderInstanceSafe );
}

void describeParams( OFX::ImageEffectDescriptor& desc, bool overVariant )
{
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );
	OFX::GroupParamDescriptor* group = nullptr;

	// The preset dropdown's entries are built from the shared table, so the
	// element values a preset selects mean the same thing in both hosts.
	for( const Decl& d : kDecls )
	{
		if( d.kind == Kind::Absent )
			continue;

		if( d.group != nullptr )
		{
			group = desc.defineGroupParam( d.group );
			group->setLabels( d.group, d.group, d.group );
			page->addChild( *group );
		}

		OFX::ParamDescriptor* param = nullptr;

		switch( d.kind )
		{
		case Kind::Slider:
		{
			OFX::DoubleParamDescriptor* p = desc.defineDoubleParam( d.name );
			p->setRange( 0.0, 1.0 );
			p->setDisplayRange( 0.0, 1.0 );
			p->setDefault( d.def );
			param = p;
			break;
		}
		case Kind::Count:
		{
			OFX::IntParamDescriptor* p = desc.defineIntParam( d.name );
			p->setRange( static_cast< int >( d.lo ), static_cast< int >( d.hi ) );
			p->setDisplayRange( static_cast< int >( d.lo ), static_cast< int >( d.hi ) );
			p->setDefault( static_cast< int >( std::lround( d.def ) ) );
			param = p;
			break;
		}
		case Kind::Toggle:
		{
			OFX::BooleanParamDescriptor* p = desc.defineBooleanParam( d.name );
			p->setDefault( d.def > 0.5f );
			param = p;
			break;
		}
		case Kind::Option:
		{
			OFX::ChoiceParamDescriptor* p = desc.defineChoiceParam( d.name );
			if( d.id == PT_PRESET )
			{
				//Element 0 is Custom and is not in the table: it means the sliders
				//are the truth.
				p->appendOption( "Custom" );
				for( int i = 0; i < presets::kCount; ++i )
					p->appendOption( presets::kPresets[ i ].name );
				//The copied values re-render; the label itself does not.
				p->setEvaluateOnChange( false );
			}
			else
			{
				for( int i = 0; i < d.optionCount; ++i )
					p->appendOption( d.options[ i ] );
			}
			p->setDefault( static_cast< int >( std::lround( d.def ) ) );
			param = p;
			break;
		}
		case Kind::File:
		{
			OFX::StringParamDescriptor* p = desc.defineStringParam( d.name );
			p->setStringType( OFX::eStringTypeFilePath );
			//A path that no longer exists must still load the composition.
			p->setFilePathExists( false );
			p->setAnimates( false );
			param = p;
			break;
		}
		case Kind::Button:
		{
			OFX::PushButtonParamDescriptor* p = desc.definePushButtonParam( d.name );
			param = p;
			break;
		}
		default:
			break;
		}

		if( param == nullptr )
			continue;

		param->setLabels( d.label, d.label, d.label );
		if( d.hint != nullptr && d.hint[ 0 ] != '\0' )
			param->setHint( d.hint );
		if( group != nullptr )
			param->setParent( *group );
		page->addChild( *param );
	}

	( void )overVariant;
}

} // namespace

//---------------------------------------------------------------------------
// "Vectrix": the generator.
//---------------------------------------------------------------------------
mDeclarePluginFactory( VectrixSourceFactory, {}, {} );

void VectrixSourceFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	describeCommon( desc, "Vectrix" );
	desc.addSupportedContext( OFX::eContextGenerator );
	desc.addSupportedContext( OFX::eContextGeneral );
}

void VectrixSourceFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	describeParams( desc, false );
}

OFX::ImageEffect* VectrixSourceFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new VectrixOFXPlugin( handle, false );
}

//---------------------------------------------------------------------------
// "Vectrix Trace": the effect. The clip is painted on the tube's face, behind
// the glass, so it curves with it, vignettes with it, sits under the graticule
// and shares the halation.
//---------------------------------------------------------------------------
mDeclarePluginFactory( VectrixTraceFactory, {}, {} );

void VectrixTraceFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	describeCommon( desc, "Vectrix Trace" );
	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );
}

void VectrixTraceFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	describeParams( desc, true );
}

OFX::ImageEffect* VectrixTraceFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new VectrixOFXPlugin( handle, true );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static VectrixSourceFactory* sourceFactory =
		new VectrixSourceFactory( kSourceIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	static VectrixTraceFactory* traceFactory =
		new VectrixTraceFactory( kTraceIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( sourceFactory );
	ids.push_back( traceFactory );
}
