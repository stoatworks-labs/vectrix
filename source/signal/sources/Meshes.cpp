#include "signal/Rng.h"
#include "signal/sources/Wire.h"

#include <algorithm>
#include <cmath>

namespace vectrix
{
namespace
{
constexpr double kTwoPi = 6.283185307179586476925286766559;

/// Three octaves of value noise. Hash-based rather than table-based so that
/// scrolling by moving the sample origin is exactly repeatable -- a ridge that
/// leaves the far end and comes back is bit-identical, which is what stops the
/// landscape popping as it recycles.
float valueNoise( float x, float y, std::uint32_t seed )
{
	float sum       = 0.0f;
	float amplitude = 1.0f;
	float frequency = 1.0f;
	float norm      = 0.0f;

	for( int octave = 0; octave < 3; ++octave )
	{
		const float sx = x * frequency;
		const float sy = y * frequency;
		const int ix   = static_cast< int >( std::floor( sx ) );
		const int iy   = static_cast< int >( std::floor( sy ) );
		const float fx = sx - static_cast< float >( ix );
		const float fy = sy - static_cast< float >( iy );

		//Smoothstep rather than linear: linear interpolation of value noise
		//leaves visible creases on the lattice lines, and on a ridgeline that
		//reads as a repeating kink rather than as terrain.
		const float ux = fx * fx * ( 3.0f - 2.0f * fx );
		const float uy = fy * fy * ( 3.0f - 2.0f * fy );

		const float n00 = hashNoise( ix, iy, seed + octave );
		const float n10 = hashNoise( ix + 1, iy, seed + octave );
		const float n01 = hashNoise( ix, iy + 1, seed + octave );
		const float n11 = hashNoise( ix + 1, iy + 1, seed + octave );

		const float a = n00 + ( n10 - n00 ) * ux;
		const float b = n01 + ( n11 - n01 ) * ux;

		sum += ( a + ( b - a ) * uy ) * amplitude;
		norm += amplitude;

		amplitude *= 0.5f; //gain
		frequency *= 2.0f; //lacunarity
	}

	return norm > 0.0f ? sum / norm : 0.0f;
}
} // namespace

Wire buildCube()
{
	Wire w;
	w.verts = {
		{ -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
		{ -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 },
	};
	w.edges = {
		{ 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
		{ 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
		{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
	};
	solveStrokes( w );
	return w;
}

Wire buildSphere( int rings, int meridians )
{
	rings     = std::clamp( rings, 3, 24 );
	meridians = std::clamp( meridians, 3, 24 );

	Wire w;

	//Hand-ordered rather than run through Hierholzer, and that is a deliberate
	//special case: the hand order draws each latitude ring as one continuous
	//sweep and each meridian as one continuous sweep, which is simply a prettier
	//globe than any Euler trail through the same graph. Euler minimises lifts;
	//it does not know that a ring drawn in one go looks like a ring.
	for( int r = 1; r < rings; ++r )
	{
		const double lat = kTwoPi * 0.5 * static_cast< double >( r ) / static_cast< double >( rings ) - kTwoPi * 0.25;
		std::vector< int > stroke;
		for( int m = 0; m <= meridians; ++m )
		{
			const double lon = kTwoPi * static_cast< double >( m ) / static_cast< double >( meridians );
			w.verts.push_back( Vec3{ static_cast< float >( std::cos( lat ) * std::cos( lon ) ),
			                         static_cast< float >( std::sin( lat ) ),
			                         static_cast< float >( std::cos( lat ) * std::sin( lon ) ) } );
			stroke.push_back( static_cast< int >( w.verts.size() ) - 1 );
		}
		w.strokes.push_back( std::move( stroke ) );
	}

	for( int m = 0; m < meridians; ++m )
	{
		const double lon = kTwoPi * static_cast< double >( m ) / static_cast< double >( meridians );
		std::vector< int > stroke;
		for( int r = 0; r <= rings; ++r )
		{
			const double lat = kTwoPi * 0.5 * static_cast< double >( r ) / static_cast< double >( rings ) - kTwoPi * 0.25;
			w.verts.push_back( Vec3{ static_cast< float >( std::cos( lat ) * std::cos( lon ) ),
			                         static_cast< float >( std::sin( lat ) ),
			                         static_cast< float >( std::cos( lat ) * std::sin( lon ) ) } );
			stroke.push_back( static_cast< int >( w.verts.size() ) - 1 );
		}
		w.strokes.push_back( std::move( stroke ) );
	}

	return w;
}

Wire buildTunnel( int rings, int sides )
{
	rings = std::clamp( rings, 3, 24 );
	sides = std::clamp( sides, 3, 24 );

	Wire w;

	//Rings are cycles, so the natural order is one ring at a time, receding.
	//Near to far, so the closest -- brightest, largest -- is drawn first.
	for( int r = 0; r < rings; ++r )
	{
		const float z = -1.0f + 2.0f * static_cast< float >( r ) / static_cast< float >( rings );
		std::vector< int > stroke;
		for( int s = 0; s <= sides; ++s )
		{
			const double a = kTwoPi * static_cast< double >( s ) / static_cast< double >( sides );
			w.verts.push_back( Vec3{ static_cast< float >( std::cos( a ) ),
			                         static_cast< float >( std::sin( a ) ), z } );
			stroke.push_back( static_cast< int >( w.verts.size() ) - 1 );
		}
		w.strokes.push_back( std::move( stroke ) );
	}

	//Four longitudinal rails, so the tunnel reads as a tube rather than as a
	//stack of unrelated rings.
	for( int rail = 0; rail < 4; ++rail )
	{
		const double a = kTwoPi * static_cast< double >( rail ) / 4.0;
		std::vector< int > stroke;
		for( int r = 0; r < rings; ++r )
		{
			const float z = -1.0f + 2.0f * static_cast< float >( r ) / static_cast< float >( rings );
			w.verts.push_back( Vec3{ static_cast< float >( std::cos( a ) ),
			                         static_cast< float >( std::sin( a ) ), z } );
			stroke.push_back( static_cast< int >( w.verts.size() ) - 1 );
		}
		w.strokes.push_back( std::move( stroke ) );
	}

	return w;
}

Wire buildMountains( int ridges, int columns, std::uint32_t seed, float offset )
{
	ridges  = std::clamp( ridges, 3, 24 );
	columns = std::clamp( columns, 8, 128 );

	Wire w;

	//The Unknown Pleasures construction: a stack of ridgelines at increasing
	//depth, each a polyline across the frame.
	//
	//Drawn NEAR TO FAR, which is what makes the hidden-line removal below a
	//running maximum rather than a depth sort.
	for( int r = 0; r < ridges; ++r )
	{
		const float depth = static_cast< float >( r ) / static_cast< float >( ridges - 1 );
		const float z     = -0.9f + 1.8f * depth;

		std::vector< int > stroke;
		for( int c = 0; c < columns; ++c )
		{
			const float u = static_cast< float >( c ) / static_cast< float >( columns - 1 );
			const float x = -1.0f + 2.0f * u;

			//Scroll by moving the sample origin through the noise field, never
			//by translating the geometry: translating pops the moment a ridge
			//recycles, because the new ridge is not the continuation of the one
			//that left.
			const float h = valueNoise( x * 2.0f, ( z + offset ) * 2.0f, seed );

			//Taper the extremes so the range does not clip flat against the
			//frame edge, and lift the far ridges slightly for separation.
			const float height = ( h - 0.5f ) * 1.1f;

			w.verts.push_back( Vec3{ x, height, z } );
			stroke.push_back( static_cast< int >( w.verts.size() ) - 1 );
		}
		w.strokes.push_back( std::move( stroke ) );
	}

	return w;
}

void solveStrokes( Wire& wire )
{
	wire.strokes.clear();

	const int vertexCount = static_cast< int >( wire.verts.size() );
	if( vertexCount == 0 || wire.edges.empty() )
		return;

	//Adjacency as (neighbour, edgeIndex), so an edge can be consumed once.
	std::vector< std::vector< std::pair< int, int > > > adjacency( vertexCount );
	std::vector< bool > used( wire.edges.size(), false );

	for( std::size_t e = 0; e < wire.edges.size(); ++e )
	{
		const auto& edge = wire.edges[ e ];
		if( edge.first < 0 || edge.second < 0 || edge.first >= vertexCount || edge.second >= vertexCount )
			continue;
		adjacency[ edge.first ].emplace_back( edge.second, static_cast< int >( e ) );
		adjacency[ edge.second ].emplace_back( edge.first, static_cast< int >( e ) );
	}

	//Hierholzer, run from every vertex that still has an unused edge. Repeated
	//application over the components gives the trail decomposition directly:
	//each run consumes a maximal closed or open walk, and what is left is
	//another one.
	std::vector< int > cursor( vertexCount, 0 );

	auto walkFrom = [ & ]( int start ) {
		std::vector< int > stack{ start };
		std::vector< int > circuit;

		while( !stack.empty() )
		{
			const int v = stack.back();

			//Skip edges already consumed. `cursor` makes this amortised linear
			//rather than quadratic.
			while( cursor[ v ] < static_cast< int >( adjacency[ v ].size() )
			       && used[ adjacency[ v ][ cursor[ v ] ].second ] )
				++cursor[ v ];

			if( cursor[ v ] < static_cast< int >( adjacency[ v ].size() ) )
			{
				const auto [ next, edgeIndex ] = adjacency[ v ][ cursor[ v ] ];
				used[ edgeIndex ]              = true;
				stack.push_back( next );
			}
			else
			{
				circuit.push_back( v );
				stack.pop_back();
			}
		}

		return circuit;
	};

	//Odd-degree vertices first: an Euler trail must start at one, and starting
	//anywhere else on a graph that has them strands edges and produces more
	//strokes than necessary.
	std::vector< int > order;
	order.reserve( vertexCount );
	for( int v = 0; v < vertexCount; ++v )
		if( adjacency[ v ].size() % 2 == 1 )
			order.push_back( v );
	for( int v = 0; v < vertexCount; ++v )
		if( adjacency[ v ].size() % 2 == 0 )
			order.push_back( v );

	for( int v : order )
	{
		while( cursor[ v ] < static_cast< int >( adjacency[ v ].size() ) )
		{
			//Re-check: the cursor may be sitting on a consumed edge.
			while( cursor[ v ] < static_cast< int >( adjacency[ v ].size() )
			       && used[ adjacency[ v ][ cursor[ v ] ].second ] )
				++cursor[ v ];
			if( cursor[ v ] >= static_cast< int >( adjacency[ v ].size() ) )
				break;

			std::vector< int > circuit = walkFrom( v );
			if( circuit.size() >= 2 )
				wire.strokes.push_back( std::move( circuit ) );
		}
	}

	//Degenerate graph, or something the walk could not consume: fall back to
	//edge-by-edge, which is correct and merely dim.
	if( wire.strokes.empty() )
	{
		for( const auto& edge : wire.edges )
			wire.strokes.push_back( { edge.first, edge.second } );
	}
}

} // namespace vectrix
