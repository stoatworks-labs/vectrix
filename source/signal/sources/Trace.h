#pragma once

#include "signal/sources/PathWalker.h"
#include "signal/sources/Source.h"

#include <FFGLSDK.h>

#include <vector>

namespace vectrix
{
struct TraceParams
{
	float threshold = 0.35f;
	float stability = 0.5f;
	float simplify  = 0.4f;
	int maxStrokes  = 24;
};

/**
	Derive a path the beam walks from the incoming clip. Effect build only.

	**Set expectations honestly, because this is the source most likely to
	disappoint.** It works well on graphics, titles, logos and line art, where
	the edge map is a small number of long clean contours. It works badly on real
	footage, where it is a mess of short strokes and the figure becomes a
	scribble. That is a property of the problem, not a bug to be tuned out, and
	the README says so.

	It is also deliberately *not* what the sibling plugin `nib` does. nib produces
	an image of lines -- edge detection as a look. This has to produce an
	**ordered path**, because a beam has to know what to draw next. Contour
	following is inherently sequential pointer-chasing, so:

	**GPU for the pixel-parallel mask, CPU for the ordering.** The split is
	forced rather than chosen: FFGL on macOS is GL 4.1 core, which has no compute
	shaders (4.3), no SSBOs (4.3) and no image load/store (4.2). Jump flooding
	gives distance fields, not ordered contours.

	**The readback is the risk**, and this is the first plugin in the fleet to do
	one inside ProcessOpenGL. Two mitigations, both load-bearing: read back
	*small* (128x72 single-channel is 9 KB), and read back *late* -- a
	double-buffered PBO, mapping the buffer issued last frame. The data is one
	frame old, which on a traced contour is invisible, and it turns a full
	pipeline stall into an asynchronous DMA.
*/
class TraceSource : public Source
{
public:
	static constexpr int kWidth  = 128;
	static constexpr int kHeight = 72;

	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Render( Sample* out, int n, double dtPerSample ) override;

	void SetParams( const TraceParams& p )
	{
		params = p;
	}
	void SetWalker( const PathWalker::Config& c )
	{
		walker.SetConfig( c );
	}

	/// Allocate the GL objects. Safe to call repeatedly.
	bool InitGL();
	void DeInitGL();

	/// Reduce the clip to an edge mask, start a readback, and extract strokes
	/// from the readback issued last frame. Called once per frame, before Render.
	void Update( GLuint clipTexture, float maxU, float maxV );

private:
	void extract( const unsigned char* mask );

	TraceParams params;
	PathWalker walker;
	Strokes strokes;

	ffglex::FFGLShader edgeShader;
	ffglex::FFGLScreenQuad quad;
	GLuint fbo         = 0;
	GLuint maskTexture = 0;
	GLuint pbo[ 2 ]    = { 0, 0 };
	int pboIndex       = 0;
	bool pboPrimed     = false;
	bool ready         = false;

	std::vector< unsigned char > skeleton;
	std::vector< unsigned char > visited;
};

} // namespace vectrix
