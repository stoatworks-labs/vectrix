#pragma once

#include "signal/Signal.h"

namespace vectrix
{
/**
	Walks a set of strokes with the beam, at constant screen speed.

	Shared by the wireframe and the trace sources, which is the point: both
	produce `Strokes`, and having one walker is what stops them becoming two
	engines with two subtly different ideas about refresh rate.

	**Constant screen-space arc length, not constant time per edge.** The obvious
	implementation gives each edge the same slice of the frame, and it is wrong
	in a way that reads as a rendering bug: a distant, short edge and a near,
	long one get the same dwell, so the distant one is *brighter*. Depth would
	appear inverted. Advancing by arc length instead means a long edge takes
	proportionally longer, which is what a real vector generator does and what
	makes the far side of a wireframe correctly dimmer.

	**Blanked jumps are still travel.** The beam does not teleport between
	strokes; it flies back with the gun off, faster than it draws but not
	instantly. So the jumps are inside the total path length, and a figure made
	of many strokes genuinely refreshes more slowly than one made of few. That is
	why stroke ordering is worth doing well rather than drawing edge by edge.
*/
class PathWalker
{
public:
	struct Config
	{
		float refreshHz   = 40.0f; ///< how many times a second the whole figure is drawn
		float retraceSpeed = 8.0f; ///< blanked jumps travel this many times faster
	};

	void Prepare( double sampleRate );
	void Reset();

	void SetConfig( const Config& c )
	{
		config = c;
	}

	/// Replace the path. Called when the geometry changes -- once per frame for
	/// a rotating wireframe, or when a new trace arrives.
	///
	/// The walk position is kept as a *fraction* of the total length across the
	/// swap rather than as an absolute distance, so a figure whose length
	/// changes slightly from frame to frame -- which is every rotating wireframe
	/// -- does not jump.
	void SetStrokes( const Strokes& strokes );

	/// Fill `n` samples by walking. Emits `z = 0` across the jumps between
	/// strokes.
	void Render( Sample* out, int n, double dtPerSample );

	bool Empty() const
	{
		return points.empty();
	}

private:
	/// One point on the flattened path, with the distance from the previous
	/// point and whether the beam is lit getting there.
	struct Point
	{
		float x;
		float y;
		float lit;      ///< 1 while drawing, 0 across a jump
		float distance; ///< screen distance from the previous point
	};

	void flatten( const Strokes& strokes );

	Config config;
	std::vector< Point > points;
	std::vector< float > cumulative; ///< cumulative *cost*, jumps discounted by retrace speed
	double position = 0.0;           ///< current cost along the path
	double totalCost = 0.0;
	double fs        = 96000.0;
};

} // namespace vectrix
