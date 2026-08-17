#include "signal/sources/PathWalker.h"

#include <algorithm>
#include <cmath>

namespace vectrix
{

void PathWalker::Prepare( double sampleRate )
{
	fs = sampleRate > 0.0 ? sampleRate : 96000.0;
	Reset();
}

void PathWalker::Reset()
{
	position = 0.0;
}

void PathWalker::flatten( const Strokes& strokes )
{
	points.clear();
	cumulative.clear();

	bool first = true;
	float lastX = 0.0f;
	float lastY = 0.0f;

	for( const Stroke& stroke : strokes )
	{
		if( stroke.size() < 2 )
			continue;

		for( std::size_t i = 0; i < stroke.size(); ++i )
		{
			const float px = stroke.x[ i ];
			const float py = stroke.y[ i ];

			//The first point of a stroke is reached with the gun off -- that is
			//the flyback. The first point of the *first* stroke has nothing
			//before it, so it costs nothing.
			const bool jump = ( i == 0 );
			const float dx  = first ? 0.0f : px - lastX;
			const float dy  = first ? 0.0f : py - lastY;
			const float d   = first ? 0.0f : std::sqrt( dx * dx + dy * dy );

			points.push_back( Point{ px, py, jump ? 0.0f : 1.0f, d } );

			lastX = px;
			lastY = py;
			first = false;
		}
	}

	//Close the loop: after the last stroke the beam flies back to the start.
	if( points.size() >= 2 )
	{
		const float dx = points.front().x - lastX;
		const float dy = points.front().y - lastY;
		points.push_back( Point{ points.front().x, points.front().y, 0.0f,
		                         std::sqrt( dx * dx + dy * dy ) } );
	}

	//Cumulative *cost* rather than distance: a blanked jump covers its distance
	//at `retraceSpeed` times the drawing speed, so it costs proportionally less
	//of the frame. Building the table in cost means the walk below is a plain
	//search with no special cases in it.
	const float retrace = std::max( config.retraceSpeed, 1.0f );
	cumulative.resize( points.size() );
	double running = 0.0;
	for( std::size_t i = 0; i < points.size(); ++i )
	{
		const float cost = points[ i ].lit > 0.5f ? points[ i ].distance
		                                          : points[ i ].distance / retrace;
		running += cost;
		cumulative[ i ] = static_cast< float >( running );
	}
	totalCost = running;
}

void PathWalker::SetStrokes( const Strokes& strokes )
{
	//Keep where we are as a fraction, not as a distance. A rotating wireframe's
	//projected length changes a little every frame; holding the absolute
	//distance would make the beam jump backwards and forwards along the figure
	//as the total grew and shrank.
	const double fraction = totalCost > 0.0 ? position / totalCost : 0.0;

	flatten( strokes );

	position = totalCost * std::clamp( fraction, 0.0, 1.0 );
}

void PathWalker::Render( Sample* out, int n, double dtPerSample )
{
	if( points.size() < 2 || totalCost <= 0.0 )
	{
		//Nothing to draw. Blank the beam rather than parking it lit at the
		//origin, which would put a bright dot in the middle of the screen.
		for( int i = 0; i < n; ++i )
			out[ i ] = Sample{ 0.0f, 0.0f, 0.0f, static_cast< float >( dtPerSample ) };
		return;
	}

	const double refresh = std::clamp( static_cast< double >( config.refreshHz ), 5.0, 120.0 );
	//One whole traversal per refresh period, whatever the figure's length -- so
	//a longer or more fragmented figure is drawn more slowly per unit length and
	//is correspondingly dimmer. That is the physical behaviour and it is free.
	const double advance = totalCost * refresh / fs;

	std::size_t cursor = 0;

	for( int i = 0; i < n; ++i )
	{
		if( position >= totalCost )
			position -= totalCost * std::floor( position / totalCost );

		//Walk forward from wherever we were. Monotonic within a block, so this
		//is amortised O(1) rather than a binary search per sample.
		while( cursor + 1 < cumulative.size() && cumulative[ cursor ] < position )
			++cursor;
		if( cumulative[ cursor ] < position )
			cursor = cumulative.size() - 1;
		while( cursor > 0 && cumulative[ cursor - 1 ] >= position )
			--cursor;

		const Point& to = points[ cursor ];
		const double segmentStart = cursor == 0 ? 0.0 : cumulative[ cursor - 1 ];
		const double segmentCost  = cumulative[ cursor ] - segmentStart;

		float x = to.x;
		float y = to.y;

		if( segmentCost > 1e-9 && cursor > 0 )
		{
			const Point& from = points[ cursor - 1 ];
			const float t     = static_cast< float >( ( position - segmentStart ) / segmentCost );
			x                 = from.x + ( to.x - from.x ) * t;
			y                 = from.y + ( to.y - from.y ) * t;
		}

		out[ i ].x  = x;
		out[ i ].y  = y;
		out[ i ].z  = to.lit;
		out[ i ].dt = static_cast< float >( dtPerSample );

		position += advance;
	}
}

} // namespace vectrix
