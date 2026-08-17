#include "Tube.h"

#include <algorithm>
#include <cmath>

namespace vectrix
{
namespace
{
/// The rungs the face buffer is allowed to sit on. Wide enough apart that
/// dragging Focus across the whole control crosses four of them, close enough
/// together that the worst case wastes less than half the texels it asked for.
constexpr int kLadder[]  = { 512, 768, 1024, 1440, 2160 };
constexpr int kLadderLen = static_cast< int >( sizeof( kLadder ) / sizeof( kLadder[ 0 ] ) );

/// Texels per sigma below which the spot is visibly a polygon.
constexpr float kTexelsPerSigma = 3.0f;

/// The same rounded-rectangle signed distance the glass shader evaluates, on the
/// CPU. Kept as one small function so the graticule is solved against the shape
/// that is actually drawn rather than against an idealisation of it.
float faceDistance( float px, float py, float halfX, float halfY, float radius )
{
	const float qx = std::fabs( px ) - ( halfX - radius );
	const float qy = std::fabs( py ) - ( halfY - radius );

	const float mx = std::max( qx, 0.0f );
	const float my = std::max( qy, 0.0f );

	return std::sqrt( mx * mx + my * my ) + std::min( std::max( qx, qy ), 0.0f ) - radius;
}
} // namespace

TubeSpec labScope()
{
	TubeSpec tube;
	tube.faceAspect     = 1.0f;
	tube.cornerRadius   = 1.0f; //a circle
	tube.deflectionGain = 0.78f;//underscanned, so full deflection is visibly inside the glass
	tube.curvature      = 0.0f;
	tube.vignette       = 0.0f;
	return tube;
}

TubeSpec television()
{
	TubeSpec tube;
	tube.faceAspect     = 4.0f / 3.0f;
	tube.cornerRadius   = 0.18f;
	tube.deflectionGain = 1.05f;//overscanned, the way a set is
	tube.curvature      = 0.35f;
	tube.vignette       = 0.25f;
	return tube;
}

int faceSizeFor( float spotFraction, int outputHeight )
{
	const float fraction = std::max( spotFraction, 1e-4f );
	const int wanted     = static_cast< int >( std::ceil( kTexelsPerSigma / fraction ) );

	//Up to the first rung that is big enough, so the buffer is never smaller than
	//the spot asked for; the top rung if nothing is.
	int chosen = kLadder[ kLadderLen - 1 ];
	for( int i = 0; i < kLadderLen; ++i )
	{
		if( kLadder[ i ] >= wanted )
		{
			chosen = kLadder[ i ];
			break;
		}
	}

	//And down to the largest rung the output can actually show. Capping to the
	//output height itself would take the result off the ladder -- a 720-line
	//composition would pin the face at 720, and every Focus change near that
	//boundary would then flash. Capping to a rung keeps every value on it.
	int cap = kLadder[ 0 ];
	for( int i = 0; i < kLadderLen; ++i )
	{
		if( kLadder[ i ] <= outputHeight )
			cap = kLadder[ i ];
	}

	return std::min( chosen, cap );
}

int faceWidthFor( int faceHeight, float faceAspect )
{
	const float aspect = std::clamp( faceAspect, 0.05f, 20.0f );
	const int width    = static_cast< int >( std::lround( static_cast< float >( faceHeight ) * aspect ) );
	return std::max( width, 1 );
}

float faceFitScale( float outputAspect, float faceAspect )
{
	const float aspect = std::max( faceAspect, 1e-4f );

	//Beam units run to (faceAspect, 1); square output units run to
	//(outputAspect, 1). The face has to fit both ways, so the scale is whichever
	//constraint binds -- width on a wide face in a narrow frame, height
	//otherwise.
	return std::min( std::max( outputAspect, 1e-4f ) / aspect, 1.0f );
}

float graticuleDivision( const TubeSpec& tube )
{
	const float halfX  = std::max( tube.faceAspect, 1e-4f );
	const float halfY  = 1.0f;
	const float radius = std::clamp( tube.cornerRadius, 0.0f, 1.0f ) * std::min( halfX, halfY );

	//The corner of a 10x8 graticule sits at (5d, 4d). Find the d that puts it
	//exactly on the face's edge.
	//
	//Bisected rather than solved. The distance field is piecewise -- flat side,
	//corner arc, interior -- so a closed form needs three cases and a test for
	//which one the corner lands in, and getting that test wrong on one face shape
	//would put the graticule slightly off the glass in a way that looks like a
	//rounding error rather than like a bug. This runs once per frame, on the CPU,
	//and thirty halvings of an interval of two beam units is exact to 2e-9.
	float lo = 0.0f;
	float hi = 2.0f * std::max( halfX, halfY );
	for( int i = 0; i < 30; ++i )
	{
		const float mid = 0.5f * ( lo + hi );
		if( faceDistance( 5.0f * mid, 4.0f * mid, halfX, halfY, radius ) < 0.0f )
			lo = mid;
		else
			hi = mid;
	}

	//On a square face at radius 1 the field is length(p) - 1, so this converges on
	//1/sqrt(41) = 0.156174 and the graticule's half-width is 5d = 0.780869, which
	//is 10/sqrt(164) -- the inscribed rectangle every scope face has ever used.
	return 0.5f * ( lo + hi );
}

} // namespace vectrix
