/**
	The FF_EFFECT registration.

	See the long note in SourcePlugin.cpp: this file is listed directly in the
	VectrixEffect target and never in vectrix_core, because a registration in the
	shared library would put both plugins into both bundles.

	The effect paints the incoming clip **on the tube face** rather than
	compositing the scope over it -- so the clip curves with the glass, sits
	behind the graticule, and shares the halation with the trace. It also unlocks
	the Trace source, which derives the beam's path from that clip.
*/
#include "Vectrix.h"

namespace
{
class VectrixEffect : public vectrix::VectrixPlugin
{
public:
	VectrixEffect() : VectrixPlugin( true )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< VectrixEffect >,                       // Create method
	"VX02",                                               // Plugin unique ID of maximum length 4
	"Vectrix Trace",                                      // Plugin name
	2,                                                    // API major version number
	1,                                                    // API minor version number
	0,                                                    // Plugin major version number
	1,                                                    // Plugin minor version number
	FF_EFFECT,                                            // Plugin type
	"Vector CRT: draw the clip, or trace it with the beam",// Plugin description
	"Vectrix FFGL effect"                                 // About
);

extern "C" const char* VectrixEffectBuildStamp()
{
	return "vectrix " VECTRIX_VERSION " effect, built " __DATE__ " " __TIME__;
}
