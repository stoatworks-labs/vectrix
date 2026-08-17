#pragma once

namespace vectrix
{
/**
    The face of the tube: its shape, its size in texels, and where the graticule
    sits on it.

    ---------------------------------------------------------------------------
    One renderer, two instruments
    ---------------------------------------------------------------------------

    A round lab scope and a rectangular vector monitor are the same three
    parameters with different numbers, not two code paths:

    - **Face Aspect.** 1:1 for a scope, 4:3 for a television. It is the only
      thing that knows the face is not square, and it enters the trace shader as
      a single divide -- so the spot stays round on a 4:3 face rather than
      turning into an ellipse, which is what happens the moment somebody works in
      normalised coordinates instead of isotropic ones.
    - **Corner Radius.** Radius 1 on a square face already *is* a circle in the
      rounded-rectangle distance field: the inset collapses to zero and the field
      reduces exactly to `length(p) - 1`. There is no special case for a round
      tube because a round tube is not a special case.
    - **Deflection Gain.** Beam units per volt. 0.78 underscans -- full
      deflection lands well inside the glass, which is how a scope is set up so
      that an overdriven signal is visibly overdriven. 1.05 overscans, which is
      how a television is set up so the blanking edges stay behind the bezel.

    Beam units are isotropic and 1 unit is half the face height, in both axes.
    So the face occupies `(faceAspect, 1)` and a sigma of 0.006 is 0.6% of the
    face height whichever axis it is measured along.

    ---------------------------------------------------------------------------
    The face buffer is sized by the spot, not by the output
    ---------------------------------------------------------------------------

    What has to be resolved is the beam, and the beam does not get bigger when
    the composition does. Three texels per sigma is the point where the Gaussian
    stops being visibly polygonal; below that, sharpening Focus stops sharpening
    the trace and starts sharpening the texels it is drawn into.

    The result is quantised onto a short ladder rather than used directly,
    because reallocating the face buffer throws away the phosphor history --
    the trail vanishes and the picture flashes. An operator dragging the Focus
    slider would otherwise cross a boundary on almost every mouse move. On the
    ladder they cross four in the whole travel.
*/
struct TubeSpec
{
	float faceAspect     = 1.0f; ///< Face width / face height.
	float cornerRadius   = 1.0f; ///< 0..1 of the shorter half-extent. 1 on a square face is a circle.
	float deflectionGain = 0.78f;///< Beam units per volt of deflection.
	float curvature      = 0.0f; ///< How much the glass bulges toward the viewer.
	float vignette       = 0.0f; ///< Fall-off toward the edge of the face.
};

/// The default, and what the plugin ships with: a round lab scope, underscanned,
/// flat glass. Everything is calibrated against this.
TubeSpec labScope();

/// A rectangular vector monitor: 4:3, softened corners, overscanned.
TubeSpec television();

/// Height in texels for the face buffer. `spotFraction` is the spot's sigma as a
/// fraction of the *full* face height, and it should be the undefocused sigma --
/// sizing for the widest spot would under-resolve the sharpest one, which is the
/// one that needs the texels.
int faceSizeFor( float spotFraction, int outputHeight );

/// Width to go with that height. Rounded, never zero.
int faceWidthFor( int faceHeight, float faceAspect );

/// Beam units to square output units: the scale that fits the whole face inside
/// the output rectangle. Computed here rather than in the glass shader so that
/// the graticule's resolution fade -- which needs the same number on the CPU --
/// cannot disagree with the geometry.
float faceFitScale( float outputAspect, float faceAspect );

/// The size of one graticule division in beam units, for a graticule of 10 by 8
/// divisions inscribed in this face.
///
/// On a round face this comes out at `10/sqrt(164) = 0.781` of the face radius
/// for the half-width, which is the classic inscribed 10x8 rectangle. That is
/// not written down as a constant anywhere: it falls out of solving the same
/// rounded-rectangle field the faceplate uses, so a change to the face shape
/// takes the graticule with it instead of leaving it hanging off the glass.
float graticuleDivision( const TubeSpec& tube );

} // namespace vectrix
