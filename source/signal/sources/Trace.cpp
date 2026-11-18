#include "signal/sources/Trace.h"

#include "Diag.h"

#include <algorithm>
#include <cmath>

#include <ffglex/FFGLScopedFBOBinding.h>

namespace vectrix
{
namespace
{
const char* const kVertex = R"(#version 410 core
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;
out vec2 uv;
void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

/// Luma, Sobel, then a two-level threshold. Deliberately just a mask -- no
/// difference-of-Gaussians and no flow field, because those are what make a
/// *look*, and `nib` already owns that. What is wanted here is a clean binary
/// map that a contour walk can chase.
const char* const kFragment = R"(#version 410 core
uniform sampler2D ClipTexture;
uniform sampler2D HistoryTexture;
uniform vec2 MaxUV;
uniform vec2 Texel;//one mask texel, in the clip's texture coordinates
uniform float Threshold;
uniform float Stability;
in vec2 uv;
out vec4 fragColour;

float lumaTap( vec2 p )
{
	//Half a *clip* texel inside, always: GL_LINEAR at the picture's edge takes
	//half its weight from padding that was never drawn.
	//(Not called `half`: that is a reserved word, like `sample` and `input`.)
	vec2 inset = 0.5 / vec2( textureSize( ClipTexture, 0 ) );
	vec2 c = clamp( p, inset, MaxUV - inset );
	vec4 s = texture( ClipTexture, c );
	//Unpremultiply: the host hands us a premultiplied texture, and an edge in
	//alpha would otherwise read as an edge in luma.
	vec3 rgb = s.a > 0.001 ? s.rgb / s.a : s.rgb;
	return dot( rgb, vec3( 0.299, 0.587, 0.114 ) );
}

/// The clip's luma averaged over one mask texel, not read at a point. A mask
/// texel spans several clip pixels, and a point sample falls between the
/// strokes of thin line art as often as it lands on one -- which breaks the
/// line into dashes. Four bilinear taps a quarter texel out each read a 2x2
/// block, so together they cover most of the footprint.
float lumaAt( vec2 p )
{
	vec2 q = Texel * 0.25;
	return 0.25 * ( lumaTap( p + vec2( -q.x, -q.y ) ) + lumaTap( p + vec2( q.x, -q.y ) )
	              + lumaTap( p + vec2( -q.x, q.y ) ) + lumaTap( p + vec2( q.x, q.y ) ) );
}

void main()
{
	vec2 p = uv * MaxUV;
	float tl = lumaAt( p + Texel * vec2( -1.0,  1.0 ) );
	float t  = lumaAt( p + Texel * vec2(  0.0,  1.0 ) );
	float tr = lumaAt( p + Texel * vec2(  1.0,  1.0 ) );
	float l  = lumaAt( p + Texel * vec2( -1.0,  0.0 ) );
	float r  = lumaAt( p + Texel * vec2(  1.0,  0.0 ) );
	float bl = lumaAt( p + Texel * vec2( -1.0, -1.0 ) );
	float b  = lumaAt( p + Texel * vec2(  0.0, -1.0 ) );
	float br = lumaAt( p + Texel * vec2(  1.0, -1.0 ) );

	float gx = ( tr + 2.0 * r + br ) - ( tl + 2.0 * l + bl );
	float gy = ( tl + 2.0 * t + tr ) - ( bl + 2.0 * b + br );
	float mag = clamp( length( vec2( gx, gy ) ) * 0.25, 0.0, 1.0 );

	//Temporal smoothing before the threshold, not after. Strokes flickering in
	//and out between frames read as the whole figure twitching, which is far
	//more distracting than a slightly late contour. Smoothing the magnitude
	//lets a marginal edge settle instead of dithering across the threshold.
	float history = texture( HistoryTexture, uv ).r;
	float smoothed = mix( mag, history, clamp( Stability, 0.0, 0.95 ) );

	fragColour = vec4( smoothed, smoothed, smoothed, 1.0 );
}
)";

/// The 8-neighbourhood, in order, starting east and going anticlockwise.
constexpr int kNeighbourX[ 8 ] = { 1, 1, 0, -1, -1, -1, 0, 1 };
constexpr int kNeighbourY[ 8 ] = { 0, 1, 1, 1, 0, -1, -1, -1 };

/// The order the walk tries them in: the four edge neighbours before the four
/// corners. A thinned line still has staircase corners -- three pixels in an L
/// -- and taking the diagonal there skips the corner pixel, which is then left
/// behind as an orphan the later passes start from.
constexpr int kWalkOrder[ 8 ] = { 0, 2, 4, 6, 1, 3, 5, 7 };

inline int indexOf( int x, int y )
{
	return y * TraceSource::kWidth + x;
}

/// Ramer-Douglas-Peucker, iterative. Removes the staircase a thresholded mask
/// always has, which otherwise shows up as visible dither along every line, and
/// cuts the point count by roughly ten times.
void simplify( const std::vector< std::pair< float, float > >& in, float epsilon,
               std::vector< std::pair< float, float > >& out )
{
	if( in.size() < 3 )
	{
		out = in;
		return;
	}

	std::vector< bool > keep( in.size(), false );
	keep.front() = keep.back() = true;

	std::vector< std::pair< std::size_t, std::size_t > > stack{ { 0, in.size() - 1 } };

	while( !stack.empty() )
	{
		const auto [ first, last ] = stack.back();
		stack.pop_back();
		if( last <= first + 1 )
			continue;

		const float ax = in[ first ].first;
		const float ay = in[ first ].second;
		const float bx = in[ last ].first;
		const float by = in[ last ].second;
		const float dx = bx - ax;
		const float dy = by - ay;
		const float len = std::sqrt( dx * dx + dy * dy );

		float worst      = 0.0f;
		std::size_t pick = first;

		for( std::size_t i = first + 1; i < last; ++i )
		{
			const float px = in[ i ].first;
			const float py = in[ i ].second;
			const float d  = len > 1e-6f
			                     ? std::fabs( dy * px - dx * py + bx * ay - by * ax ) / len
			                     : std::sqrt( ( px - ax ) * ( px - ax ) + ( py - ay ) * ( py - ay ) );
			if( d > worst )
			{
				worst = d;
				pick  = i;
			}
		}

		if( worst > epsilon )
		{
			keep[ pick ] = true;
			stack.push_back( { first, pick } );
			stack.push_back( { pick, last } );
		}
	}

	out.clear();
	for( std::size_t i = 0; i < in.size(); ++i )
		if( keep[ i ] )
			out.push_back( in[ i ] );
}
} // namespace

void TraceSource::Prepare( double sampleRate )
{
	walker.Prepare( sampleRate );
	skeleton.assign( kWidth * kHeight, 0 );
	visited.assign( kWidth * kHeight, 0 );
	Reset();
}

void TraceSource::Reset()
{
	strokes.clear();
	walker.Reset();
	pboPrimed = false;
}

bool TraceSource::InitGL()
{
	if( ready )
		return true;

	if( !edgeShader.Compile( kVertex, kFragment ) )
	{
		diag::error( "trace: the edge shader would not compile" );
		return false;
	}
	if( !quad.Initialise() )
	{
		diag::error( "trace: could not create the screen quad" );
		return false;
	}

	GLint previousTexture = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &previousTexture );

	glGenTextures( 2, maskTexture );
	for( GLuint texture : maskTexture )
	{
		glBindTexture( GL_TEXTURE_2D, texture );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_R8, kWidth, kHeight, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	}
	glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( previousTexture ) );

	glGenFramebuffers( 2, fbo );
	GLint previousFBO = 0;
	glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &previousFBO );
	GLfloat previousClear[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
	glGetFloatv( GL_COLOR_CLEAR_VALUE, previousClear );
	bool complete = true;
	for( int i = 0; i < 2; ++i )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, fbo[ i ] );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, maskTexture[ i ], 0 );
		complete = complete && glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
		//glTexImage2D with no data leaves the texels undefined, and the first
		//frame reads one of these as its history.
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
	}
	glClearColor( previousClear[ 0 ], previousClear[ 1 ], previousClear[ 2 ], previousClear[ 3 ] );
	glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( previousFBO ) );

	if( !complete )
	{
		diag::error( "trace: the mask framebuffer is incomplete" );
		return false;
	}

	glGenBuffers( 2, pbo );
	for( int i = 0; i < 2; ++i )
	{
		glBindBuffer( GL_PIXEL_PACK_BUFFER, pbo[ i ] );
		glBufferData( GL_PIXEL_PACK_BUFFER, kWidth * kHeight, nullptr, GL_STREAM_READ );
	}
	glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );

	ready = true;
	return true;
}

void TraceSource::DeInitGL()
{
	if( maskTexture[ 0 ] != 0 )
		glDeleteTextures( 2, maskTexture );
	if( fbo[ 0 ] != 0 )
		glDeleteFramebuffers( 2, fbo );
	if( pbo[ 0 ] != 0 )
		glDeleteBuffers( 2, pbo );
	maskTexture[ 0 ] = maskTexture[ 1 ] = 0;
	fbo[ 0 ] = fbo[ 1 ] = 0;
	pbo[ 0 ] = pbo[ 1 ] = 0;
	quad.Release();
	edgeShader.FreeGLResources();
	ready     = false;
	pboPrimed = false;
}

void TraceSource::Update( GLuint clipTexture, float maxU, float maxV, float voltsX, float voltsY )
{
	if( !ready || clipTexture == 0 )
		return;

	frameX = voltsX;
	frameY = voltsY;

	const int target  = maskIndex;
	const int history = 1 - maskIndex;
	maskIndex         = history;

	GLint previousViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, previousViewport );

	{
		ffglex::ScopedFBOBinding fboBinding( fbo[ target ], ffglex::ScopedFBOBinding::RB_REVERT );
		//ScopedFBOBinding restores the framebuffer binding and NOT the viewport,
		//so this is set here every time and put back by hand below.
		glViewport( 0, 0, kWidth, kHeight );

		ffglex::ScopedShaderBinding shaderBinding( edgeShader.GetGLID() );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, clipTexture );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, maskTexture[ history ] );

		edgeShader.Set( "ClipTexture", 0 );
		edgeShader.Set( "HistoryTexture", 1 );
		edgeShader.Set( "MaxUV", maxU, maxV );
		//The taps step one *mask* texel, measured in the clip's coordinates --
		//which, when the host pads its texture, is MaxUV of the way across.
		edgeShader.Set( "Texel", maxU / kWidth, maxV / kHeight );
		edgeShader.Set( "Threshold", params.threshold );
		edgeShader.Set( "Stability", params.stability );

		glDisable( GL_BLEND );
		quad.Draw();

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );

		//Issue this frame's readback into one buffer while mapping the one
		//issued last frame. glReadPixels into a bound PIXEL_PACK_BUFFER returns
		//immediately -- it is a DMA request, not a stall.
		glBindBuffer( GL_PIXEL_PACK_BUFFER, pbo[ pboIndex ] );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, kWidth, kHeight, GL_RED, GL_UNSIGNED_BYTE, nullptr );
		glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
	}

	glViewport( previousViewport[ 0 ], previousViewport[ 1 ], previousViewport[ 2 ], previousViewport[ 3 ] );

	const int other = 1 - pboIndex;
	if( pboPrimed )
	{
		glBindBuffer( GL_PIXEL_PACK_BUFFER, pbo[ other ] );
		const void* mapped = glMapBuffer( GL_PIXEL_PACK_BUFFER, GL_READ_ONLY );
		if( mapped != nullptr )
		{
			extract( static_cast< const unsigned char* >( mapped ) );
			glUnmapBuffer( GL_PIXEL_PACK_BUFFER );
		}
		glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
	}

	pboIndex  = other;
	pboPrimed = true;

	walker.SetStrokes( strokes );
}

void TraceSource::extract( const unsigned char* mask )
{
	const unsigned char high = static_cast< unsigned char >(
		std::clamp( params.threshold, 0.02f, 0.95f ) * 255.0f );

	//Threshold to a binary image.
	for( int i = 0; i < kWidth * kHeight; ++i )
		skeleton[ i ] = mask[ i ] >= high ? 1 : 0;

	//Zhang-Suen thinning to a one-pixel skeleton. Without it a thick edge is
	//walked as a wide blob and the contour wanders back and forth across it,
	//which draws a scribble on top of every line.
	bool changed = true;
	for( int pass = 0; pass < 12 && changed; ++pass )
	{
		changed = false;
		for( int subIteration = 0; subIteration < 2; ++subIteration )
		{
			std::vector< int > doomed;
			for( int y = 1; y < kHeight - 1; ++y )
			{
				for( int x = 1; x < kWidth - 1; ++x )
				{
					const int index = indexOf( x, y );
					if( skeleton[ index ] == 0 )
						continue;

					int neighbours   = 0;
					int transitions  = 0;
					unsigned char previous = skeleton[ indexOf( x + kNeighbourX[ 7 ], y + kNeighbourY[ 7 ] ) ];
					for( int k = 0; k < 8; ++k )
					{
						const unsigned char value = skeleton[ indexOf( x + kNeighbourX[ k ], y + kNeighbourY[ k ] ) ];
						neighbours += value;
						if( previous == 0 && value == 1 )
							++transitions;
						previous = value;
					}

					if( neighbours < 2 || neighbours > 6 || transitions != 1 )
						continue;

					const unsigned char n = skeleton[ indexOf( x, y + 1 ) ];
					const unsigned char e = skeleton[ indexOf( x + 1, y ) ];
					const unsigned char s = skeleton[ indexOf( x, y - 1 ) ];
					const unsigned char w = skeleton[ indexOf( x - 1, y ) ];

					const bool first  = subIteration == 0 ? ( n * e * s ) == 0 : ( n * e * w ) == 0;
					const bool second = subIteration == 0 ? ( e * s * w ) == 0 : ( n * s * w ) == 0;

					if( first && second )
						doomed.push_back( index );
				}
			}

			for( int index : doomed )
				skeleton[ index ] = 0;
			if( !doomed.empty() )
				changed = true;
		}
	}

	//Walk. Endpoints first, then junctions, then anything left -- which is the
	//closed loops. Walking from endpoints first matters: starting in the middle
	//of an open line produces two half-strokes instead of one whole one.
	std::fill( visited.begin(), visited.end(), 0 );

	auto on = [ & ]( int x, int y ) {
		return x >= 0 && y >= 0 && x < kWidth && y < kHeight && skeleton[ indexOf( x, y ) ] != 0;
	};

	//How many separate branches leave a pixel: the 0 -> 1 transitions round its
	//ring. 1 is an end, 2 a line, 3 or more a junction. Not the neighbour
	//*count*, which reads every staircase corner of a thinned line as a
	//junction -- and then the junction pass starts walking in the middle of
	//every curve, cutting a clean circle into arcs with gaps between them.
	auto branches = [ & ]( int x, int y ) {
		int transitions = 0;
		bool previous   = on( x + kNeighbourX[ 7 ], y + kNeighbourY[ 7 ] );
		for( int k = 0; k < 8; ++k )
		{
			const bool value = on( x + kNeighbourX[ k ], y + kNeighbourY[ k ] );
			if( !previous && value )
				++transitions;
			previous = value;
		}
		return transitions;
	};

	std::vector< std::pair< float, float > > raw;
	std::vector< std::pair< float, float > > back;
	std::vector< std::pair< float, float > > reduced;
	Strokes found;

	const float epsilon = 0.25f + std::clamp( params.simplify, 0.0f, 1.0f ) * 1.5f;

	//Follow unvisited pixels from (x, y), which is already visited, appending
	//each step to `into`.
	auto follow = [ & ]( int x, int y, std::vector< std::pair< float, float > >& into ) {
		while( true )
		{
			int nextX = -1;
			int nextY = -1;
			for( int k : kWalkOrder )
			{
				const int nx = x + kNeighbourX[ k ];
				const int ny = y + kNeighbourY[ k ];
				if( !on( nx, ny ) || visited[ indexOf( nx, ny ) ] )
					continue;
				nextX = nx;
				nextY = ny;
				break;
			}

			if( nextX < 0 )
				return;
			x = nextX;
			y = nextY;
			visited[ indexOf( x, y ) ] = 1;
			into.emplace_back( static_cast< float >( x ), static_cast< float >( y ) );
		}
	};

	auto walkFrom = [ & ]( int startX, int startY ) {
		raw.clear();
		back.clear();
		visited[ indexOf( startX, startY ) ] = 1;
		raw.emplace_back( static_cast< float >( startX ), static_cast< float >( startY ) );
		follow( startX, startY, raw );

		//Then the other way from the start, if there is one. A start in the
		//middle of a line -- a junction, or anywhere on a loop that a junction
		//touches -- otherwise yields half a stroke now and the other half later,
		//with a blanked jump between two pieces that should have been one.
		follow( startX, startY, back );
		if( !back.empty() )
		{
			std::reverse( back.begin(), back.end() );
			raw.insert( raw.begin(), back.begin(), back.end() );
		}

		//Two-pixel specks are noise, not drawing.
		if( raw.size() < 4 )
			return;

		//A closed contour ends one pixel short of where it began, because the
		//start was already visited. Close it, or every loop is drawn with a gap.
		const float gapX = raw.back().first - raw.front().first;
		const float gapY = raw.back().second - raw.front().second;
		if( raw.size() >= 8 && std::fabs( gapX ) <= 1.0f && std::fabs( gapY ) <= 1.0f )
			raw.push_back( raw.front() );

		simplify( raw, epsilon, reduced );

		Stroke stroke;
		for( const auto& [ px, py ] : reduced )
		{
			//Into deflection volts, through pixel *centres*, at the volts the
			//clip's edges sit at on the glass. +Y is up in the signal domain and
			//the mask is stored bottom-up by glReadPixels, so no flip is needed
			//here -- the one flip into screen space happens in the tube pass.
			stroke.push( ( ( px + 0.5f ) / kWidth * 2.0f - 1.0f ) * frameX,
			             ( ( py + 0.5f ) / kHeight * 2.0f - 1.0f ) * frameY );
		}
		if( stroke.size() >= 2 )
			found.push_back( std::move( stroke ) );
	};

	for( int y = 1; y < kHeight - 1; ++y )
		for( int x = 1; x < kWidth - 1; ++x )
			if( skeleton[ indexOf( x, y ) ] && !visited[ indexOf( x, y ) ] && branches( x, y ) == 1 )
				walkFrom( x, y );

	for( int y = 1; y < kHeight - 1; ++y )
		for( int x = 1; x < kWidth - 1; ++x )
			if( skeleton[ indexOf( x, y ) ] && !visited[ indexOf( x, y ) ] && branches( x, y ) >= 3 )
				walkFrom( x, y );

	for( int y = 1; y < kHeight - 1; ++y )
		for( int x = 1; x < kWidth - 1; ++x )
			if( skeleton[ indexOf( x, y ) ] && !visited[ indexOf( x, y ) ] )
				walkFrom( x, y );

	//Over budget: keep the LONGEST strokes. That is also the denoiser -- a short
	//stroke on real footage is almost always a speck.
	const std::size_t budget = static_cast< std::size_t >( std::clamp( params.maxStrokes, 4, 64 ) );
	if( found.size() > budget )
	{
		std::partial_sort( found.begin(), found.begin() + budget, found.end(),
		                   []( const Stroke& a, const Stroke& b ) { return a.size() > b.size(); } );
		found.resize( budget );
	}

	//Order greedily by nearest endpoint, reversal allowed. Cuts the total
	//blanked-jump distance by roughly three times, and every unit of jump
	//distance is time the beam spends not drawing.
	strokes.clear();
	std::vector< bool > taken( found.size(), false );
	float cursorX = 0.0f;
	float cursorY = 0.0f;

	for( std::size_t placed = 0; placed < found.size(); ++placed )
	{
		std::size_t best = found.size();
		float bestCost   = 1e30f;
		bool bestReverse = false;

		for( std::size_t i = 0; i < found.size(); ++i )
		{
			if( taken[ i ] )
				continue;
			const Stroke& s = found[ i ];
			const float df  = ( s.x.front() - cursorX ) * ( s.x.front() - cursorX )
			                 + ( s.y.front() - cursorY ) * ( s.y.front() - cursorY );
			const float db = ( s.x.back() - cursorX ) * ( s.x.back() - cursorX )
			                 + ( s.y.back() - cursorY ) * ( s.y.back() - cursorY );
			if( df < bestCost )
			{
				bestCost    = df;
				best        = i;
				bestReverse = false;
			}
			if( db < bestCost )
			{
				bestCost    = db;
				best        = i;
				bestReverse = true;
			}
		}

		if( best >= found.size() )
			break;

		taken[ best ] = true;
		Stroke s      = found[ best ];
		if( bestReverse )
		{
			std::reverse( s.x.begin(), s.x.end() );
			std::reverse( s.y.begin(), s.y.end() );
		}
		cursorX = s.x.back();
		cursorY = s.y.back();
		strokes.push_back( std::move( s ) );
	}
}

void TraceSource::ExtractForTest( const unsigned char* mask, float voltsX, float voltsY )
{
	if( skeleton.size() != static_cast< std::size_t >( kWidth * kHeight ) )
	{
		skeleton.assign( kWidth * kHeight, 0 );
		visited.assign( kWidth * kHeight, 0 );
	}
	frameX = voltsX;
	frameY = voltsY;
	extract( mask );
	walker.SetStrokes( strokes );
}

void TraceSource::Render( Sample* out, int n, double dtPerSample )
{
	walker.Render( out, n, dtPerSample );
}

} // namespace vectrix
