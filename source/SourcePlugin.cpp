/**
	The FF_SOURCE registration.

	**This file is listed directly in the VectrixSource target, not in
	vectrix_core.** Both plugins share the class; what they do not share is the
	`CFFGLPluginInfo` below, and putting either registration in the shared
	library would register both plugins into both bundles.

	It is also why the shared library is an OBJECT library rather than a STATIC
	one. `CFFGLPluginInfo` registers itself from a file-scope constructor and
	nothing ever references it by name, so in an archive the linker is entitled
	to drop the whole translation unit -- giving a bundle that loads, exports
	`plugMain`, and reports that it contains no plugins.

	    nm -gU Vectrix.bundle/Contents/MacOS/Vectrix | grep plugMain
*/
#include "Vectrix.h"

namespace
{
class VectrixSource : public vectrix::VectrixPlugin
{
public:
	VectrixSource() : VectrixPlugin( false )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< VectrixSource >,                          // Create method
	"VX01",                                                  // Plugin unique ID of maximum length 4
	"Vectrix",                                               // Plugin name
	2,                                                       // API major version number
	1,                                                       // API minor version number
	0,                                                       // Plugin major version number
	1,                                                       // Plugin minor version number
	FF_SOURCE,                                               // Plugin type
	"Vector CRT synth: oscillator, pedals, oscilloscope",     // Plugin description
	"Vectrix FFGL source"                                    // About
);

extern "C" const char* VectrixSourceBuildStamp()
{
	return "vectrix " VECTRIX_VERSION " source, built " __DATE__ " " __TIME__;
}
