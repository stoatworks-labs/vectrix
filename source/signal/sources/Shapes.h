#pragma once

#include "signal/sources/Source.h"

namespace vectrix
{
enum class ShapeKind
{
	Circle,
	Square,
	Diamond,
	Polygon,
	Star,
	Rose,
	Epitrochoid,
	Count
};

struct ShapeParams
{
	ShapeKind kind = ShapeKind::Circle;
	float rate     = 30.0f; ///< Hz -- how many times a second the figure is drawn
	int sides      = 5;     ///< Polygon / Star vertex count, 3..24
	float inner    = 0.382f;///< Star inner radius fraction. 1/phi^2, the pentagram's own
	int petalN     = 3;     ///< Rose numerator
	int petalD     = 1;     ///< Rose denominator
	float trochoid = 0.6f;  ///< Epitrochoid pen offset
};

/**
	Parametric closed paths.

	One phase, integrated at `rate`. Two disciplines make the difference between
	a figure that stands still and one that slowly precesses for reasons nobody
	can find:

	**The closure period is computed when a parameter changes, never per sample.**
	A rose with k = n/d closes after d turns if n*d is odd and 2d turns
	otherwise; an epitrochoid closes after r/gcd(R,r) turns. Wrap the phase at 1
	regardless and the figure drifts by whatever the remainder is, every cycle.

	**The square is not arc-length parameterised.** It is triangle-X against
	triangle-Y in quadrature, so the beam genuinely slows into each corner and
	the corner genuinely brightens. Re-parameterising to constant speed would be
	tidier and would *remove* the artefact -- which is the whole thing we are
	here to produce.
*/
class Shapes : public Source
{
public:
	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Render( Sample* out, int n, double dtPerSample ) override;

	void SetParams( const ShapeParams& p );

private:
	/// Evaluate at `t` turns through one full closure period, in [0,1).
	void evaluate( double t, float& x, float& y ) const;

	/// Recompute the closure period and the normalising radius. Called only when
	/// the parameters change.
	void recompute();

	ShapeParams params;
	double phase  = 0.0; ///< [0,1) across the whole closure period
	double fs     = 96000.0;
	double turns  = 1.0;  ///< how many turns of the underlying angle one period is
	float normalise = 1.0f;
};

} // namespace vectrix
