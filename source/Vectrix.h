#pragma once

#include "Controls.h"
#include "render/BeamGeometry.h"
#include "signal/Clock.h"
#include "signal/Engine.h"
#include "signal/Modulation.h"

#include <FFGLSDK.h>

#include <mutex>
#include <string>

namespace vectrix
{
/**
	An oscillator, a pedalboard and a cathode ray tube in X/Y mode.

	## The one idea

	**This models a route, not a look.** Everything on the screen is where a
	single electron beam was and how long it lingered there. The bright
	turnarounds, the ghost repeats, the halo, the rounded corners of a square are
	all **consequences** of what the signal does on its way to the yoke. If you
	are ever tempted to draw one of them directly, stop: either it already falls
	out of the chain, or the chain is wrong somewhere and that is the bug.

	The rule is made arithmetic in two places, and both are worth knowing before
	changing anything:

	- In the renderer, `1/v` brightness is never computed. A fixed quantum of
	  energy is deposited per *sample interval* and spread over whatever screen
	  distance the beam covered in that interval, so brightness proportional to
	  dwell time is what "equal energy per unit time" *means* rather than a term
	  applied on top.
	- In the signal chain, sources are geometry and brightness is physics. A
	  source never modulates `z` to make something look bright. Corner
	  brightening comes from the slew limiter and the deflection amplifier's
	  bandwidth, which is where a real machine puts them.

	The cheapest demonstration that the rule is being obeyed is Blank on Retrace:
	saw against saw becomes a clean raster because the beam is genuinely *cut
	off* during flyback, not because anything decided not to draw the flyback
	line.

	## Both plugins are this class

	The source generates its own signal over its own tube; the effect paints the
	incoming clip on the tube face and can also derive its path from that clip.
	They differ by a constructor flag, an input count, and one forced parameter
	-- little enough that keeping them as one class is what stops them drifting
	apart. Both declare an identical parameter list so that a composition can be
	moved between them without the numbering shifting underneath it.
*/
class VectrixPlugin : public CFFGLPlugin
{
public:
	explicit VectrixPlugin( bool overInput );
	~VectrixPlugin() override = default;

	FFResult InitGL( const FFGLViewportStruct* viewPort ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	//---------------------------------------------------------------------
	// There is deliberately no separate "render one frame offline" entry
	// point.
	//
	// There was one, and `tools/vxtest` did not use it: it never advanced the
	// clock and never read the spectrum, so through it the transport was
	// frozen, both LFOs stood still and all four modulation slots read zero.
	// A harness built on it would have reported thirteen live controls as
	// dead. The harness drives `ProcessOpenGL` instead -- the real host
	// sequence -- and is deterministic because it drives `SetTime` from its
	// own frame counter. A second entry point would only ever be a second
	// thing to keep in step with this one.
	//---------------------------------------------------------------------

private:
	void declareParameters();
	void applyPreset( int presetIndex );
	void updateAudio();
	BeamGeometry::RenderParams renderParams( const Resolved& resolved, double frameSeconds ) const;

	/// A source takes no input; an effect takes exactly one. Getting this wrong
	/// is not a compile error -- the host simply files the plugin under the wrong
	/// tab and hands it a texture it was not expecting.
	const bool overInput;

	float params[ PT_COUNT ] = {};

	Clock clock;
	Engine engine;
	Modulation modulation;
	BeamGeometry beam;

	/// Guards the file path, which the host sets from its own thread.
	mutable std::mutex textMutex;
	std::string filePath;
	char textReturn[ 4096 ] = {};

	/// The host is handed a bare pointer for the About block, so the string has
	/// to outlive the call that built it.
	std::string aboutText;

	bool glReady      = false;
	bool contentDirty = false;
	long frameCounter = 0;
	double lastHostTime = -1.0;
};

} // namespace vectrix
