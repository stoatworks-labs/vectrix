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
uniform vec2 Texel;
uniform float Threshold;
uniform float Stability;
in vec2 uv;
out vec4 fragColour;

float lumaAt( vec2 p )
{
	//Half a texel inside, always: GL_LINEAR at the picture's edge takes half its
	//weight from padding that was never drawn.
	vec2 c = clamp( p, Texel * 0.5, MaxUV - Texel * 0.5 );
	vec4 s = texture( ClipTexture, c );
	//Unpremultiply: the host hands us a premultiplied texture, and an edge in
	//alpha would otherwise read as an edge in luma.
	vec3 rgb = s.a > 0.001 ? s.rgb / s.a : s.rgb;
	return dot( rgb, vec3( 0.299, 0.587, 0.114 ) );
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

	glGenTextures( 1, &maskTexture );
	glBindTexture( GL_TEXTURE_2D, maskTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R8, kWidth, kHeight, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( previousTexture ) );

	glGenFramebuffers( 1, &fbo );
	GLint previousFBO = 0;
	glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &previousFBO );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, maskTexture, 0 );
	const bool complete = glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
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
	if( maskTexture != 0 )
		glDeleteTextures( 1, &maskTexture );
	if( fbo != 0 )
		glDeleteFramebuffers( 1, &fbo );
	if( pbo[ 0 ] != 0 )
		glDeleteBuffers( 2, pbo );
	maskTexture = 0;
	fbo         = 0;
	pbo[ 0 ] = pbo[ 1 ] = 0;
	quad.Release();
	edgeShader.FreeGLResources();
	ready     = false;
	pboPrimed = false;
}

void TraceSource::Update( GLuint clipTexture, float maxU, float maxV )
{
	if( !ready || clipTexture == 0 )
		return;

	GLint previousViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, previousViewport );

	{
		ffglex::ScopedFBOBinding fboBinding( fbo, ffglex::ScopedFBOBinding::RB_REVERT );
		//ScopedFBOBinding restores the framebuffer binding and NOT the viewport,
		//so this is set here every time and put back by hand below.
		glViewport( 0, 0, kWidth, kHeight );

		ffglex::ScopedShaderBinding shaderBinding( edgeShader.GetGLID() );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, clipTexture );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, maskTexture );

		edgeShader.Set( "ClipTexture", 0 );
		edgeShader.Set( "HistoryTexture", 1 );
		edgeShader.Set( "MaxUV", maxU, maxV );
		edgeShader.Set( "Texel", 1.0f / kWidth, 1.0f / kHeight );
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

	auto degree = [ & ]( int x, int y ) {
		int count = 0;
		for( int k = 0; k < 8; ++k )
		{
			const int nx = x + kNeighbourX[ k ];
			const int ny = y + kNeighbourY[ k ];
			if( nx < 0 || ny < 0 || nx >= kWidth || ny >= kHeight )
				continue;
			count += skeleton[ indexOf( nx, ny ) ];
		}
		return count;
	};

	std::vector< std::pair< float, float > > raw;
	std::vector< std::pair< float, float > > reduced;
	Strokes found;

	const float epsilon = 0.25f + std::clamp( params.simplify, 0.0f, 1.0f ) * 1.5f;

	auto walkFrom = [ & ]( int startX, int startY ) {
		raw.clear();
		int x = startX;
		int y = startY;

		while( true )
		{
			visited[ indexOf( x, y ) ] = 1;
			raw.emplace_back( static_cast< float >( x ), static_cast< float >( y ) );

			int nextX = -1;
			int nextY = -1;
			for( int k = 0; k < 8; ++k )
			{
				const int nx = x + kNeighbourX[ k ];
				const int ny = y + kNeighbourY[ k ];
				if( nx < 0 || ny < 0 || nx >= kWidth || ny >= kHeight )
					continue;
				if( skeleton[ indexOf( nx, ny ) ] == 0 || visited[ indexOf( nx, ny ) ] )
					continue;
				nextX = nx;
				nextY = ny;
				break;
			}

			if( nextX < 0 )
				break;
			x = nextX;
			y = nextY;
		}

		//Two-pixel specks are noise, not drawing.
		if( raw.size() < 4 )
			return;

		simplify( raw, epsilon, reduced );

		Stroke stroke;
		for( const auto& [ px, py ] : reduced )
		{
			//Into deflection volts. +Y is up in the signal domain and the mask
			//is stored bottom-up by glReadPixels, so no flip is needed here --
			//the one flip into screen space happens in the tube pass.
			stroke.push( px / ( kWidth - 1 ) * 2.0f - 1.0f,
			             py / ( kHeight - 1 ) * 2.0f - 1.0f );
		}
		if( stroke.size() >= 2 )
			found.push_back( std::move( stroke ) );
	};

	for( int y = 1; y < kHeight - 1; ++y )
		for( int x = 1; x < kWidth - 1; ++x )
			if( skeleton[ indexOf( x, y ) ] && !visited[ indexOf( x, y ) ] && degree( x, y ) == 1 )
				walkFrom( x, y );

	for( int y = 1; y < kHeight - 1; ++y )
		for( int x = 1; x < kWidth - 1; ++x )
			if( skeleton[ indexOf( x, y ) ] && !visited[ indexOf( x, y ) ] && degree( x, y ) >= 3 )
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

void TraceSource::Render( Sample* out, int n, double dtPerSample )
{
	walker.Render( out, n, dtPerSample );
}

} // namespace vectrix
