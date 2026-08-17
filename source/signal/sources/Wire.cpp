#include "signal/sources/Wire.h"

#include <algorithm>
#include <cmath>

namespace vectrix
{
namespace
{
constexpr double kTwoPi = 6.283185307179586476925286766559;

/// The near plane, in view space. Anything closer than this is behind the lens
/// as far as the projection is concerned.
constexpr float kNearPlane = 0.15f;

struct View
{
	float x, y, z;
};

/// Rotate a model-space point into view space and push it away from the camera.
View toView( const Vec3& v, float sx, float cx, float sy, float cy, float sz, float cz, float distance )
{
	//Z, then Y, then X. The order is arbitrary but it must be *fixed*: changing
	//it changes what the three Spin controls mean, and a saved composition would
	//come back rotated differently.
	float x = v.x * cz - v.y * sz;
	float y = v.x * sz + v.y * cz;
	float z = v.z;

	const float x2 = x * cy + z * sy;
	const float z2 = -x * sy + z * cy;
	x              = x2;
	z              = z2;

	const float y2 = y * cx - z * sx;
	const float z3 = y * sx + z * cx;
	y              = y2;
	z              = z3;

	return View{ x, y, z + distance };
}

/// Perspective divide. Only ever called on points already known to be in front
/// of the near plane.
void projectPoint( const View& v, float distance, float& outX, float& outY )
{
	const float s = distance / std::max( v.z, kNearPlane );
	outX          = v.x * s / distance;
	outY          = v.y * s / distance;
}
} // namespace

void WireSource::Prepare( double sampleRate )
{
	walker.Prepare( sampleRate );
	dirty = true;
	Reset();
}

void WireSource::Reset()
{
	angleX = angleY = angleZ = 0.0;
	travel                   = 0.0;
	walker.Reset();
}

void WireSource::SetParams( const WireParams& p )
{
	//The mesh is rebuilt only when its *shape* changes. Spin and camera move
	//every frame and must not trigger a rebuild -- rebuilding the mountains
	//re-runs three octaves of noise over 3000 points, which is not a per-frame
	//cost worth paying for nothing.
	const bool structural = p.mesh != params.mesh || p.detail != params.detail || p.seed != params.seed;
	params                = p;
	if( structural )
		dirty = true;
}

void WireSource::rebuild()
{
	switch( params.mesh )
	{
		case MeshKind::Cube: wire = buildCube(); break;
		case MeshKind::Sphere: wire = buildSphere( params.detail, params.detail + 2 ); break;
		case MeshKind::Tunnel: wire = buildTunnel( params.detail, params.detail ); break;
		case MeshKind::Mountains:
		default:
			//Budget: 24 ridges of 128 columns is 3072 points, which is the cap
			//named in the design and comfortably inside a frame's sample count.
			wire = buildMountains( params.detail, 96, params.seed, static_cast< float >( travel ) );
			break;
	}
	dirty = false;
}

void WireSource::Advance( double frameSeconds )
{
	angleX += params.spinX * frameSeconds;
	angleY += params.spinY * frameSeconds;
	angleZ += params.spinZ * frameSeconds;
	angleX -= std::floor( angleX );
	angleY -= std::floor( angleY );
	angleZ -= std::floor( angleZ );

	travel += params.scroll * frameSeconds * 0.25;

	//The mountains scroll by resampling the noise field, so they are rebuilt
	//every frame; everything else is static geometry and is not.
	if( params.mesh == MeshKind::Mountains )
		dirty = true;

	if( dirty )
		rebuild();

	project();
	walker.SetStrokes( projected );
}

void WireSource::project()
{
	projected.clear();

	const float sx = static_cast< float >( std::sin( kTwoPi * angleX ) );
	const float cx = static_cast< float >( std::cos( kTwoPi * angleX ) );
	const float sy = static_cast< float >( std::sin( kTwoPi * angleY ) );
	const float cy = static_cast< float >( std::cos( kTwoPi * angleY ) );
	const float sz = static_cast< float >( std::sin( kTwoPi * angleZ ) );
	const float cz = static_cast< float >( std::cos( kTwoPi * angleZ ) );

	const float distance = std::clamp( params.camera, 2.0f, 8.0f );

	//Hidden-line removal for the landscape: a running per-column maximum of
	//screen Y. Ten lines, and it is the entire reason a stack of wiggly lines
	//reads as terrain rather than as a stack of wiggly lines.
	//
	//The strokes are built near-to-far, so a point that falls below everything
	//already drawn in its column is behind a nearer ridge and gets cut. The beam
	//still travels the hidden part -- it is simply blanked -- which is both
	//correct and what keeps the timing right.
	const bool hiddenLine = params.mesh == MeshKind::Mountains;
	constexpr int kHorizonBuckets = 512;
	std::vector< float > horizon;
	if( hiddenLine )
		horizon.assign( kHorizonBuckets, -1.0e9f );

	for( const std::vector< int >& strokeIndices : wire.strokes )
	{
		Stroke current;

		auto flushStroke = [ & ]() {
			if( current.size() >= 2 )
				projected.push_back( current );
			current.clear();
		};

		for( std::size_t i = 0; i < strokeIndices.size(); ++i )
		{
			const int index = strokeIndices[ i ];
			if( index < 0 || index >= static_cast< int >( wire.verts.size() ) )
				continue;

			const View v = toView( wire.verts[ index ], sx, cx, sy, cy, sz, cz, distance );

			//Behind the near plane: break the stroke rather than projecting it.
			//A vertex behind the camera projects through the singularity to a
			//wild coordinate and slams the beam across the entire picture as a
			//lightning bolt, once per frame, on an object that otherwise looks
			//perfectly fine.
			if( v.z <= kNearPlane )
			{
				flushStroke();
				continue;
			}

			float px = 0.0f;
			float py = 0.0f;
			projectPoint( v, distance, px, py );

			if( hiddenLine )
			{
				const int bucket = std::clamp(
					static_cast< int >( ( px * 0.5f + 0.5f ) * ( kHorizonBuckets - 1 ) ),
					0, kHorizonBuckets - 1 );
				if( py <= horizon[ bucket ] )
				{
					//Hidden. End the visible run here; the beam will fly over it.
					flushStroke();
					continue;
				}
				horizon[ bucket ] = py;
			}

			current.push( px, py );
		}

		flushStroke();
	}
}

void WireSource::Render( Sample* out, int n, double dtPerSample )
{
	walker.Render( out, n, dtPerSample );
}

} // namespace vectrix
