#pragma once

#include "signal/sources/PathWalker.h"
#include "signal/sources/Source.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace vectrix
{
enum class MeshKind
{
	Cube,
	Sphere,
	Tunnel,
	Mountains,
	Count
};

struct WireParams
{
	MeshKind mesh = MeshKind::Mountains;
	int detail    = 8;     ///< rings/sides/ridges, 3..24
	float spinX   = 0.13f; ///< Hz
	float spinY   = 0.21f;
	float spinZ   = 0.0f;
	float camera  = 4.0f;  ///< distance, 2..8
	float scroll  = 1.0f;  ///< tunnel/mountain travel
	std::uint32_t seed = 1;
};

struct Vec3
{
	float x = 0.0f, y = 0.0f, z = 0.0f;
};

/**
	A wireframe object, and the order the beam should walk it in.

	`strokes` is not "the edge list". Drawing edge by edge means a blanked jump
	between very nearly every pair of edges, and blanked jumps still cost time,
	so the figure refreshes more slowly and every stroke gets dimmer. A cube
	drawn naively is 12 edges and 11 jumps; decomposed into Eulerian trails it is
	**4 strokes and 3 jumps**, and it is visibly brighter for it.
*/
struct Wire
{
	std::vector< Vec3 > verts;
	std::vector< std::pair< int, int > > edges;
	std::vector< std::vector< int > > strokes; ///< indices into verts
};

/**
	The 3D sources.

	Per parameter change: build the mesh and solve the stroke ordering, once.
	Per frame: rotate, clip, project, hand the result to the PathWalker.

	**Near-plane clipping is not optional and is the single most likely visible
	bug here.** A vertex behind the camera projects through the singularity to a
	wild coordinate, and the beam slams across the whole picture as a lightning
	bolt -- once per frame, forever, on an object that otherwise looks correct.
	Edges are clipped in view space before projection.
*/
class WireSource : public Source
{
public:
	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Render( Sample* out, int n, double dtPerSample ) override;

	void SetParams( const WireParams& p );
	void SetWalker( const PathWalker::Config& c )
	{
		walker.SetConfig( c );
	}

	/// Advance the rotation and rebuild the projected strokes. Called once per
	/// frame, before Render.
	void Advance( double frameSeconds );

private:
	void rebuild();
	void project();

	WireParams params;
	Wire wire;
	PathWalker walker;
	Strokes projected;

	double angleX = 0.0, angleY = 0.0, angleZ = 0.0;
	double travel = 0.0;
	bool dirty    = true;
};

//---------------------------------------------------------------------------
// Mesh construction, in Meshes.cpp
//---------------------------------------------------------------------------

Wire buildCube();
Wire buildSphere( int rings, int meridians );
Wire buildTunnel( int rings, int sides );
Wire buildMountains( int ridges, int columns, std::uint32_t seed, float offset );

/**
	Decompose a graph into the fewest open trails, by Hierholzer.

	A graph with 2k odd-degree vertices decomposes into exactly k open trails --
	that is Euler's result, and it is the right formulation of "what is the
	fewest number of times the beam has to lift". Pair the odd vertices with
	virtual edges, find one Euler circuit over the augmented graph, then split it
	at the virtual edges.

	Guards the degenerate cases -- no edges, isolated vertices -- and falls back
	to edge-by-edge, which is correct and merely dim.
*/
void solveStrokes( Wire& wire );

} // namespace vectrix
