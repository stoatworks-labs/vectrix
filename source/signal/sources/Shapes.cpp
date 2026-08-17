#include "signal/sources/Shapes.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace vectrix
{
namespace
{
constexpr double kTwoPi = 6.283185307179586476925286766559;

/// Walk the perimeter of a regular N-gon at constant speed. `t` is [0,1) around
/// the whole figure. Radii alternate when `inner` is given, which is a star.
void polygonAt( double t, int sides, float innerFraction, bool star, float& x, float& y )
{
	const int vertices = star ? sides * 2 : sides;
	const double scaled = t * static_cast< double >( vertices );
	const int edge      = static_cast< int >( scaled ) % vertices;
	const double frac   = scaled - std::floor( scaled );

	auto vertexAt = [ & ]( int index, double& vx, double& vy ) {
		const double angle = kTwoPi * static_cast< double >( index ) / static_cast< double >( vertices )
		                     + kTwoPi * 0.25;//start at the top, as a compass does
		const double radius = ( star && ( index & 1 ) ) ? static_cast< double >( innerFraction ) : 1.0;
		vx                  = radius * std::cos( angle );
		vy                  = radius * std::sin( angle );
	};

	double ax, ay, bx, by;
	vertexAt( edge, ax, ay );
	vertexAt( ( edge + 1 ) % vertices, bx, by );

	//Linear along the edge, so the beam is at constant speed along a side and
	//pivots at the vertex. The vertex brightening that falls out of that is the
	//point; do not smooth it.
	x = static_cast< float >( ax + ( bx - ax ) * frac );
	y = static_cast< float >( ay + ( by - ay ) * frac );
}
} // namespace

void Shapes::Prepare( double sampleRate )
{
	fs = sampleRate > 0.0 ? sampleRate : 96000.0;
	recompute();
	Reset();
}

void Shapes::Reset()
{
	phase = 0.0;
}

void Shapes::SetParams( const ShapeParams& p )
{
	const bool structural = p.kind != params.kind || p.sides != params.sides
	                        || p.inner != params.inner || p.petalN != params.petalN
	                        || p.petalD != params.petalD || p.trochoid != params.trochoid;
	params = p;
	if( structural )
		recompute();
}

void Shapes::recompute()
{
	const int sides = std::clamp( params.sides, 3, 24 );
	const int n     = std::clamp( params.petalN, 1, 12 );
	const int d     = std::clamp( params.petalD, 1, 12 );

	switch( params.kind )
	{
		case ShapeKind::Rose:
			//r = cos(k*theta) with k = n/d closes after d turns when n*d is odd,
			//and after 2d turns otherwise. Wrapping at 1 turn regardless leaves a
			//remainder that precesses the figure a little further every cycle --
			//slowly enough to look like an intentional drift and to survive review.
			turns = ( ( n * d ) % 2 == 1 ) ? static_cast< double >( d ) : 2.0 * static_cast< double >( d );
			normalise = 1.0f;
			break;

		case ShapeKind::Epitrochoid:
		{
			//R and r are taken from the same two integer knobs, so the closure is
			//r / gcd(R, r) turns of the driving angle.
			const int bigR   = n + d;
			const int littleR = d;
			const int g       = std::gcd( bigR, littleR );
			turns             = static_cast< double >( littleR / std::max( 1, g ) );
			//The extreme radius is (R-r)+pen, computed here rather than per
			//sample: it is a constant for the whole figure.
			normalise = 1.0f / std::max( 1e-4f,
			                             static_cast< float >( bigR - littleR ) + std::fabs( params.trochoid ) );
			break;
		}

		default:
			turns     = 1.0;
			normalise = 1.0f;
			break;
	}

	( void )sides;
}

void Shapes::evaluate( double t, float& x, float& y ) const
{
	const int sides = std::clamp( params.sides, 3, 24 );

	switch( params.kind )
	{
		case ShapeKind::Circle:
			x = static_cast< float >( std::cos( kTwoPi * t ) );
			y = static_cast< float >( std::sin( kTwoPi * t ) );
			break;

		case ShapeKind::Square:
		{
			//Triangle against triangle in quadrature. Deliberately NOT an
			//arc-length walk: this makes the beam slow into each corner, and the
			//bright corners of a square on a scope are a dwell artefact, not a
			//drawn decoration.
			const double a = t;
			const double b = t + 0.25;
			auto tri       = []( double p ) {
				p -= std::floor( p );
				return 1.0 - 4.0 * std::fabs( p - 0.5 );
			};
			x = static_cast< float >( std::clamp( tri( a ) * 2.0, -1.0, 1.0 ) );
			y = static_cast< float >( std::clamp( tri( b ) * 2.0, -1.0, 1.0 ) );
			break;
		}

		case ShapeKind::Diamond:
			//A square rotated 45 degrees is a 4-gon walked at constant speed. It
			//gets its own entry because operators look for the word, not because
			//it is a different construction.
			polygonAt( t, 4, 1.0f, false, x, y );
			break;

		case ShapeKind::Polygon:
			polygonAt( t, sides, 1.0f, false, x, y );
			break;

		case ShapeKind::Star:
			polygonAt( t, sides, std::clamp( params.inner, 0.05f, 0.95f ), true, x, y );
			break;

		case ShapeKind::Rose:
		{
			const int n        = std::clamp( params.petalN, 1, 12 );
			const int d        = std::clamp( params.petalD, 1, 12 );
			const double theta = kTwoPi * t * turns;
			const double k     = static_cast< double >( n ) / static_cast< double >( d );
			const double r     = std::cos( k * theta );
			x                  = static_cast< float >( r * std::cos( theta ) );
			y                  = static_cast< float >( r * std::sin( theta ) );
			break;
		}

		case ShapeKind::Epitrochoid:
		{
			const int n        = std::clamp( params.petalN, 1, 12 );
			const int d        = std::clamp( params.petalD, 1, 12 );
			const double bigR  = static_cast< double >( n + d );
			const double lilR  = static_cast< double >( d );
			const double pen   = static_cast< double >( params.trochoid );
			const double theta = kTwoPi * t * turns;
			const double diff  = bigR - lilR;
			const double inner = diff * theta / std::max( 1e-6, lilR );

			x = static_cast< float >( ( diff * std::cos( theta ) + pen * std::cos( inner ) ) * normalise );
			y = static_cast< float >( ( diff * std::sin( theta ) - pen * std::sin( inner ) ) * normalise );
			break;
		}

		case ShapeKind::Count:
		default:
			x = 0.0f;
			y = 0.0f;
			break;
	}
}

void Shapes::Render( Sample* out, int n, double dtPerSample )
{
	const double rate = std::clamp( static_cast< double >( params.rate ), 0.01, 200.0 );
	const double inc  = rate / fs;

	for( int i = 0; i < n; ++i )
	{
		float x = 0.0f;
		float y = 0.0f;
		evaluate( phase, x, y );

		out[ i ].x  = x;
		out[ i ].y  = y;
		out[ i ].z  = 1.0f;//a closed path needs no blanking
		out[ i ].dt = static_cast< float >( dtPerSample );

		phase += inc;
		if( phase >= 1.0 )
			phase -= std::floor( phase );
	}
}

} // namespace vectrix
