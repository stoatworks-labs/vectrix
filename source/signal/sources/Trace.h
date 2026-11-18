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
	*small* (one byte a pixel at the mask size below), and read back *late* -- a
	double-buffered PBO, mapping the buffer issued last frame. The data is one
	frame old, which on a traced contour is invisible, and it turns a full
	pipeline stall into an asynchronous DMA.

	**The mask has to resolve a letter, not just a word.** At 128x72 a title's
	letters were five to seven mask pixels tall, Sobel merged each word into
	one band, and the walk drew the band's skeleton -- a zig-zag along every
	word, which is the scribble the guide promises titles will not be. See
	docs/NOTES.md, 2026-11-18.

	**The strokes are registered to the clip as the glass paints it.** The
	effect shows the clip behind the glass across the whole output, so the
	beam has to land on the clip's own edges -- which means the mask's square
	pixels go out at the *output's* aspect, scaled by the face fit and the
	deflection gain. The plugin works those two volts out from the tube and
	the viewport and hands them to Update. Mapping the mask's corners to
	+-1 V instead, as v0.1.12 did, squeezed a 16:9 clip into a square and
	drew a round label as a tall ellipse in the middle of the glass.
*/
class TraceSource : public Source
{
public:
	static constexpr int kWidth  = 512;
	static constexpr int kHeight = 288;

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
	///
	/// `voltsX` / `voltsY` are the deflection, in volts, at which the clip's
	/// right and top edges sit on the glass: the strokes come out at those
	/// volts so that the beam traces the clip where the glass shows it.
	void Update( GLuint clipTexture, float maxU, float maxV, float voltsX, float voltsY );

	/// The strokes the walker was last handed, in volts. For the harness.
	const Strokes& Current() const
	{
		return strokes;
	}

	/// Extract strokes from a mask the caller supplies, bottom row first, as
	/// the GPU pass would have produced it. For the harness: it drives the
	/// whole CPU half -- threshold, thinning, walk, ordering -- with no GL.
	void ExtractForTest( const unsigned char* mask, float voltsX, float voltsY );

private:
	void extract( const unsigned char* mask );

	TraceParams params;
	PathWalker walker;
	Strokes strokes;

	ffglex::FFGLShader edgeShader;
	ffglex::FFGLScreenQuad quad;
	//Two masks, ping-ponged: the pass reads last frame's as its history, and a
	//texture cannot be sampled while it is the framebuffer being drawn into --
	//that is a feedback loop, and GL leaves its result undefined.
	GLuint fbo[ 2 ]         = { 0, 0 };
	GLuint maskTexture[ 2 ] = { 0, 0 };
	int maskIndex           = 0;
	GLuint pbo[ 2 ]    = { 0, 0 };
	int pboIndex       = 0;
	float frameX       = 1.0f;
	float frameY       = 1.0f;
	bool pboPrimed     = false;
	bool ready         = false;

	std::vector< unsigned char > skeleton;
	std::vector< unsigned char > visited;
};

} // namespace vectrix
