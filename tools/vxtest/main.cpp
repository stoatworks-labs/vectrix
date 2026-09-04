/**
    vxtest -- the offline harness.

    It drives **the real code that ships** in a headless core-profile context:
    `VectrixPlugin` for anything that goes through the parameter list, and
    `BeamGeometry` directly for the invariant tests, which need a sample block
    the harness controls to the last `dt` rather than one an oscillator happened
    to produce. Nothing below is a reimplementation and nothing below is a
    preview -- every number printed comes out of a frame that was actually
    rendered.

        --energy    one sweep at ten speeds spanning 100:1 deposits the same light
        --dwell     line density tracks 1/v along a decelerating segment
        --rate      the same figure at Draft / Normal / Fine renders the same
        --point     a stationary beam reaches the analytic N.E/(2.pi.sigma^2)
        --blank     z = 0 deposits nothing and does not leak past its endpoints
        --identity  the effect build at Beam 0 is bit-identical to its input
        --fx        Householder is orthogonal; a knob move keeps the tails
        --drift     ten minutes of phase accumulation stays exact
        --all       every one of the above, with a summary

        --out PATH  render a frame     --size WxH   --frames N   --preset N
        --list      every parameter    --set ID=V (or "Name=V")  --effect
        --pipe      raw RGBA frames on stdout, for the project video
        --script    a cue sheet driving it            --fps N

    ## Determinism

    **Time comes from the frame counter and never from a wall clock.** `Clock`
    falls back to `steady_clock` when the host has never called `SetTime`, so
    every render below calls `SetTime( frame * 1/60 )` first -- which also pins
    `FrameSeconds` to exactly 1/60 through the clamp, and therefore pins the
    sample count. Two runs of the same command produce byte-identical PNGs.

    A synthetic transport goes with it: 120 BPM in 4/4 from time zero, so a bar
    is exactly two seconds and `SetBeatInfo` gives the tempo-synced delay
    something real to divide. Left at the SDK's defaults, `bpm` is whatever the
    base class was constructed with and the Sync switch would look dead.

    ## Why every frame goes through `ProcessOpenGL`

    There is no back door and there should not be one. `ProcessOpenGL` is the
    call a host makes, and it is the only path that advances the clock, updates
    the modulation and reads the transport -- so it is the only path on which
    the LFOs, the audio-driven slots and the tempo-synced delay are alive at
    all. An earlier `RenderFrame` shortcut did none of those and has since been
    deleted; a harness built on it would have reported thirteen live controls as
    dead. `InitGL` is called once per instance rather than per frame, because
    `BeamGeometry::InitGL` compiles five shaders and generates a VAO and a VBO
    without deleting the previous set.

    ## What each test can and cannot catch

    `--energy` is the test the plugin exists to pass, and it is deliberately not
    run through the oscillator: a frequency sweep changes the sampling of the
    figure as well as its speed, so a failure would have two candidate causes.
    A hand-built triangle at ten speeds keeps the *path* identical -- the same
    line, walked between one and a hundred times inside one frame -- so the
    only thing varying is how far the beam travelled per sample interval, which
    is precisely the quantity the brightness model is supposed to divide out.
    It cannot say whether the picture looks right, and it cannot say anything
    about speeds fast enough for `densityFloor` to cull a segment; the margin
    to that floor is printed so the limit of the claim is visible.

    `--dwell` measures the rendered density and regresses it against `1/v`. The
    beam's own spot blurs that density, and the blur is not an artefact -- it is
    the spot -- so the correction term `sigma^2 * lambda'' / 2` is real and a
    steep enough ramp measures the spot rather than the dwell law. The segment
    below is chosen so that term stays around a thousandth of the density; a
    ten-to-one ramp over the same face would put it near five percent and no
    correct renderer would pass.

    `--point` compares against the closed form the fragment shader is supposed
    to tend to, with the beam placed on a texel centre so that the peak texel is
    the peak and not a neighbour of it. It reads the excitation back *through*
    the phosphor's saturation curve and inverts it, which is why it can quote a
    figure "before saturation" at all.

    `--rate` measures two things and only one of them is an identity. The light
    *total* is invariant under the sample count and comes out so; *where* the
    light lands is not quite, because Detail is the rate the whole signal chain
    is modelled at and not merely a sample count -- so the check prints the
    same comparison with the deflection amplifier held inside every rate's
    Nyquist, which is how much of the difference belongs to the renderer.

    `--identity` is the only test here that is about arithmetic rather than
    physics: at Beam 0 with the faceplate out of the way the effect build must
    hand its input back untouched, bit for bit. It fails, and the second
    measurement it prints says which half is at fault.

    `--fx` and `--drift` need no GL at all.

    None of them catches a uniform whose name does not match the GLSL, because
    `glUniform` with location -1 is a documented no-op. See `tools/sweep.py`.

    ## `--pipe` is not a check

    It renders the project video, and the long comment above `runPipe` says how
    and why. The one thing to know before reading anything else: **the source
    build's pipe reads nothing from stdin**, because the source plugin has no
    input. See there.
*/

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Controls.h"
#include "Presets.h"
#include "Vectrix.h"
#include "render/BeamGeometry.h"
#include "render/Phosphor.h"
#include "render/Tube.h"
#include "signal/Signal.h"
#include "signal/fx/Chain.h"
#include "signal/fx/Delay.h"
#include "signal/fx/Reverb.h"
#include "signal/sources/Oscillator.h"

using namespace vectrix;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );// filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

	std::vector< unsigned char > header;
	putU32( header, static_cast< uint32_t >( width ) );
	putU32( header, static_cast< uint32_t >( height ) );
	header.push_back( 8 );// bit depth
	header.push_back( 6 );// colour type: RGBA
	header.push_back( 0 );
	header.push_back( 0 );
	header.push_back( 0 );
	putChunk( png, "IHDR", header );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	std::FILE* file = std::fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

struct Target
{
	GLuint texture = 0;
	GLuint fbo     = 0;
	int width      = 0;
	int height     = 0;
	bool floating  = false;
};

/// `floating` asks for RGBA32F rather than RGBA8.
///
/// Not a luxury: the invariant tests below sum a whole frame and then quote a
/// spread of half a percent, and eight bits per channel quantises every one of
/// those pixels by up to 0.2% of full scale. An RGBA8 target would put the
/// measurement's own noise floor uncomfortably close to the tolerance being
/// asserted. `--identity` is the one test that *wants* eight bits, because
/// "bit-identical" is a claim about the buffer a host would receive.
Target makeTarget( int width, int height, bool floating = false )
{
	Target target;
	target.width    = width;
	target.height   = height;
	target.floating = floating;

	glGenTextures( 1, &target.texture );
	glBindTexture( GL_TEXTURE_2D, target.texture );
	glTexImage2D( GL_TEXTURE_2D, 0, floating ? GL_RGBA32F : GL_RGBA8, width, height, 0,
	              GL_RGBA, floating ? GL_FLOAT : GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenFramebuffers( 1, &target.fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0 );

	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
		std::fprintf( stderr, "the %dx%d %s target is not framebuffer-complete\n",
		              width, height, floating ? "float" : "8-bit" );
	return target;
}

void releaseTarget( Target& target )
{
	if( target.fbo != 0 )
		glDeleteFramebuffers( 1, &target.fbo );
	if( target.texture != 0 )
		glDeleteTextures( 1, &target.texture );
	target = Target();
}

/// Straight out of GL, **bottom row first**. Every sampler below takes frame
/// coordinates with y down and flips here, in one place.
std::vector< unsigned char > readBytes( const Target& target )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( target.width ) * target.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	return pixels;
}

/// The same, unquantised. Also bottom-up.
std::vector< float > readFloats( const Target& target )
{
	std::vector< float > pixels( static_cast< size_t >( target.width ) * target.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.width, target.height, GL_RGBA, GL_FLOAT, pixels.data() );
	return pixels;
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

/// One pixel, in frame coordinates (0..1, y down).
void samplePixel( const std::vector< float >& bottomUp, int width, int height,
                  float fx, float fy, float& r, float& g, float& b, float& a )
{
	const int x     = std::clamp( static_cast< int >( fx * static_cast< float >( width ) ), 0, width - 1 );
	const int yDown = std::clamp( static_cast< int >( fy * static_cast< float >( height ) ), 0, height - 1 );
	const int y     = height - 1 - yDown;

	const float* p = bottomUp.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
	r              = p[ 0 ];
	g              = p[ 1 ];
	b              = p[ 2 ];
	a              = p[ 3 ];
}

//---------------------------------------------------------------------------
// The harness's clock.
//---------------------------------------------------------------------------
//
// 1/60 exactly. `Clock` clamps a frame to [1/240, 1/24] and this sits in the
// middle of that, so the clamp never fires and `SamplesForThisFrame` is a
// round number at all three Detail settings: 800, 1600, 3200.
constexpr double kFrameSeconds = 1.0 / 60.0;

//---------------------------------------------------------------------------
// Driving the plugin.
//---------------------------------------------------------------------------
bool startPlugin( VectrixPlugin& plugin, const Target& target )
{
	FFGLViewportStruct viewport {};
	viewport.width  = static_cast< FFUInt32 >( target.width );
	viewport.height = static_cast< FFUInt32 >( target.height );

	// Once per instance, never per frame. `FlipbookPlugin::InitGL` is
	// idempotent and the sibling harness leans on that; this one is not --
	// `BeamGeometry::InitGL` compiles five shaders and generates a VAO and a VBO
	// without deleting the previous pair, so calling it every frame leaks a
	// program and two objects per frame.
	return plugin.InitGL( &viewport ) == FF_SUCCESS;
}

/// `secondsPerFrame` is only ever anything but the harness's own 1/60 for
/// `--pipe`, where the render has to advance at the rate the finished video will
/// be played back at. Every check in this file leaves it alone.
bool renderFrame( VectrixPlugin& plugin, const Target& target, int frameIndex, GLuint clip = 0,
                  double secondsPerFrame = kFrameSeconds )
{
	// The whole of the harness's determinism is these two lines. `Clock::Update`
	// takes the wall clock only when the host has never called SetTime, so
	// driving it from the frame counter is what stops the picture depending on
	// how long the process has been alive.
	const double seconds = static_cast< double >( frameIndex ) * secondsPerFrame;
	plugin.SetClockScaleForTest( 1.0 );//seconds, said out loud rather than inferred
	plugin.SetTime( seconds );

	// 120 BPM in 4/4 from time zero, so bar N starts at exactly 2N seconds and
	// the tempo-synced delay divides something real.
	constexpr double kBarSeconds = 2.0;
	plugin.SetBeatInfo( 120.0f, static_cast< float >( std::fmod( seconds, kBarSeconds ) / kBarSeconds ) );

	FFGLTextureStruct inputStruct {};
	inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( target.width );
	inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( target.height );
	inputStruct.Handle                              = clip;
	FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

	ProcessOpenGLStruct process {};
	process.numInputTextures = clip != 0 ? 1 : 0;
	process.inputTextures    = clip != 0 ? inputs : nullptr;
	process.HostFBO          = target.fbo;

	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glViewport( 0, 0, target.width, target.height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
}

/// Every parameter's host-facing name, read out of the plugin itself.
///
/// Built at runtime rather than kept as a table beside Controls.h, and that is
/// not tidiness: a hand-written table is a second place for a name to live, and
/// the failure it produces is a `--set` that silently addresses nothing while
/// everything else about the run looks correct.
std::map< std::string, unsigned int > parameterIndex( VectrixPlugin& plugin )
{
	std::map< std::string, unsigned int > byName;
	for( unsigned int id = 0; id < PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name != nullptr && name[ 0 ] != '\0' )
			byName[ name ] = id;
	}
	return byName;
}

/// `--set`, resolved the way `--out` has always resolved it: a bare number is an
/// id, which is what `tools/sweep.py` drives, and anything else is a display
/// name, which is what a person types. Returns how many did not resolve.
int applySets( VectrixPlugin& plugin, const std::vector< std::pair< std::string, float > >& sets )
{
	const std::map< std::string, unsigned int > byName = parameterIndex( plugin );
	int unresolved = 0;

	for( const auto& set : sets )
	{
		char* end           = nullptr;
		const long asNumber = std::strtol( set.first.c_str(), &end, 10 );
		unsigned int id     = PT_COUNT;

		if( end != nullptr && *end == '\0' && !set.first.empty() && asNumber >= 0 && asNumber < PT_COUNT )
		{
			id = static_cast< unsigned int >( asNumber );
		}
		else
		{
			const auto found = byName.find( set.first );
			if( found != byName.end() )
				id = found->second;
		}

		if( id >= PT_COUNT )
		{
			std::fprintf( stderr, "vxtest: no parameter called \"%s\"\n", set.first.c_str() );
			++unresolved;
			continue;
		}
		plugin.SetFloatParameter( id, set.second );
	}

	return unresolved;
}

//---------------------------------------------------------------------------
// Parameter automation for --pipe.
//
// A plain text file of `frame  Parameter Name  value` lines. Values are held
// before the first key and after the last, and linearly interpolated between,
// so the piece is edited by editing the cue sheet rather than by editing code.
// Identical in grammar to `tiltest`'s, deliberately: the cue sheets and the
// `render.py` beside them in stoatworks-backend are shared machinery.
//
// **An option or a boolean steps; it does not ramp.** The interpolation here is
// linear and `Option()` reads a dropdown by rounding, so a key that changes
// Source at frame 100 and the next key for Source at frame 400 does not hold the
// old value until 100 -- it walks through every entry in between and crosses a
// rounding boundary somewhere nobody chose. Vectrix is full of these: Source,
// Wave X, Wave Y, Shape, Mesh, Phosphor, Detail, Ratio, every Routing, and every
// boolean. The fix is a **hold key at the END of every section the value must
// not move in**, not merely a key where it changes. resolume-scopes lost a whole
// re-render to exactly this. `warnAboutRamps` below shouts about it.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;//blank or comment

		//The name is everything up to the last token, because parameters have
		//spaces in them and the value never does.
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame <= track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

/// The trap in the block comment above, caught rather than only documented.
///
/// A stepping parameter that ramps is not an error -- the cue sheet is allowed
/// to say it, and a two-frame step between adjacent entries is a legitimate way
/// to write a cut -- so this warns and renders. What it will not do is let a
/// forty-second drift through six dropdown entries go out unremarked, which is
/// the shape the failure actually takes.
void warnAboutRamps( VectrixPlugin& plugin, const std::string& name, unsigned int id, const Track& track )
{
	const unsigned int type = plugin.GetParamType( id );
	if( type != FF_TYPE_OPTION && type != FF_TYPE_BOOLEAN && type != FF_TYPE_INTEGER )
		return;

	for( size_t i = 1; i < track.size(); ++i )
	{
		const int frames = track[ i ].first - track[ i - 1 ].first;
		if( frames <= 1 || track[ i ].second == track[ i - 1 ].second )
			continue;

		std::fprintf( stderr,
		              "vxtest: \"%s\" steps rather than ramps, but the script slides it from %g to %g\n"
		              "        over frames %d..%d -- it will pass through every value between. Add a\n"
		              "        hold key at frame %d to make the change a cut.\n",
		              name.c_str(), static_cast< double >( track[ i - 1 ].second ),
		              static_cast< double >( track[ i ].second ),
		              track[ i - 1 ].first, track[ i ].first, track[ i ].first - 1 );
	}
}

/// Raw frames off stdin, in whole frames only. A short read at the end is a
/// truncated frame, which is not something to render half of.
bool readExactly( void* into, size_t bytes )
{
	unsigned char* p = static_cast< unsigned char* >( into );
	size_t got = 0;
	while( got < bytes )
	{
		const size_t n = fread( p + got, 1, bytes - got, stdin );
		if( n == 0 )
			return false;//clean EOF, or a short final frame we cannot use
		got += n;
	}
	return true;
}

/// A coloured gradient with a couple of hard edges, for the effect build to sit
/// on top of. The edges matter for `--identity`: a smooth ramp would hide a
/// half-texel sampling error, which is exactly the sort of thing "bit-identical"
/// is supposed to rule out.
GLuint makeClipTexture( int width, int height )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			unsigned char* p = image.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			const bool block = ( ( x / 37 ) + ( y / 29 ) ) % 2 == 0;
			p[ 0 ]           = static_cast< unsigned char >( 255 * x / std::max( 1, width - 1 ) );
			p[ 1 ]           = block ? 200 : 40;
			p[ 2 ]           = static_cast< unsigned char >( 255 * y / std::max( 1, height - 1 ) );
			p[ 3 ]           = 255;
		}
	}

	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.data() );
	// NEAREST, so a passthrough that reads the right texel reads it exactly.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// The bench: BeamGeometry with a sample block the harness wrote by hand.
//---------------------------------------------------------------------------
//
// Everything the invariant tests need is a physical quantity -- a sigma, an
// energy per second, a dt -- and `BeamGeometry::RenderParams` is already stated
// in exactly those units. Going through the plugin instead would mean asking
// for "slider position 0.3" and then working out what sigma that was, which
// puts Controls.cpp's mapping inside a test that is not about it.
BeamGeometry::RenderParams benchParams( float sigma, float beamPower )
{
	BeamGeometry::RenderParams rp;
	rp.beamPower    = beamPower;
	rp.spotSigma    = sigma;
	rp.spotDefocus  = 0.0f;// so the sigma drawn is the sigma the analytic uses
	rp.blankFloor   = 0.0f;
	rp.densityFloor = 1.0e-4f;// the shipped value, deliberately not relaxed

	rp.phosphor    = 0;   // P31: efficiency 1.0, fast colour green 1.0, saturation 12
	rp.persistence = 1.0f;// irrelevant on a cleared buffer, set for the record

	rp.halation  = 0.0f;
	rp.graticule = 0.0f;
	rp.faceBlack = 0.0f;
	rp.opacity   = 1.0f;

	rp.tube.faceAspect     = 1.0f;
	rp.tube.cornerRadius   = 0.0f;// a square face, so nothing is masked off
	rp.tube.deflectionGain = 1.0f;// one volt is one beam unit; no scale to carry
	rp.tube.curvature      = 0.0f;
	rp.tube.vignette       = 0.0f;

	rp.frameSeconds = static_cast< float >( kFrameSeconds );
	rp.clearHistory = true;// every bench shot is frame one

	return rp;
}

/// The face buffer's size for this sigma, uncapped, and therefore the output
/// size to render at.
///
/// Rendering the face at exactly its own resolution is what makes the samplers
/// below exact rather than approximate: with a square face, a square output and
/// no curvature the glass pass's `faceUV` comes out identical to `uv`, so an
/// output pixel centre lands on a face texel centre and the bilinear fetch
/// returns that texel unchanged. Any other size and every measurement here
/// would be reading a resampled copy of the thing it is measuring.
int benchSize( float sigma )
{
	return faceSizeFor( sigma * 0.5f, 2160 );
}

/// Undo the phosphor's saturation, `l = e / (1 + e/S)`.
///
/// The renderer applies it at readout on purpose -- see the prelude -- so the
/// only way to recover what was actually deposited is to invert it. Which is
/// worth doing rather than working around: the deposit is the quantity every
/// invariant here is stated about, and a test that measured emitted light
/// instead would report the saturation curve as a failure of the dwell law.
double excitationFrom( double light, double saturation )
{
	if( !( light > 0.0 ) )
		return 0.0;
	if( light >= saturation )
		return std::numeric_limits< double >::infinity();
	return light / ( 1.0 - light / saturation );
}

/// Excitation summed down every column of the face, recovered from the green
/// channel -- P31's fast colour is exactly 1.0 there and its efficiency is 1.0,
/// so green *is* the saturated excitation with no constant to divide out.
std::vector< double > columnExcitation( const std::vector< float >& pixels, int width, int height )
{
	const double saturation = phosphor( 0 ).saturation;

	std::vector< double > columns( static_cast< size_t >( width ), 0.0 );
	for( int y = 0; y < height; ++y )
	{
		const float* row = pixels.data() + static_cast< size_t >( y ) * width * 4;
		for( int x = 0; x < width; ++x )
			columns[ static_cast< size_t >( x ) ] += excitationFrom( row[ x * 4 + 1 ], saturation );
	}
	return columns;
}

/// Beam units to a column index on a square face rendered at its own size.
int columnFor( double x, int width )
{
	const double u = ( x + 1.0 ) * 0.5 * static_cast< double >( width ) - 0.5;
	return std::clamp( static_cast< int >( std::lround( u ) ), 0, width - 1 );
}

double rSquared( const std::vector< double >& x, const std::vector< double >& y )
{
	const size_t n = std::min( x.size(), y.size() );
	if( n < 3 )
		return 0.0;

	double sx = 0.0, sy = 0.0;
	for( size_t i = 0; i < n; ++i )
	{
		sx += x[ i ];
		sy += y[ i ];
	}
	const double mx = sx / static_cast< double >( n );
	const double my = sy / static_cast< double >( n );

	double sxx = 0.0, sxy = 0.0, syy = 0.0;
	for( size_t i = 0; i < n; ++i )
	{
		const double dx = x[ i ] - mx;
		const double dy = y[ i ] - my;
		sxx += dx * dx;
		sxy += dx * dy;
		syy += dy * dy;
	}

	if( sxx <= 0.0 || syy <= 0.0 )
		return 0.0;
	return ( sxy * sxy ) / ( sxx * syy );
}

//---------------------------------------------------------------------------
// --energy
//---------------------------------------------------------------------------
//
// The whole claim of the renderer, in one number.
//
// One straight line across the face, walked between once and a hundred times
// inside a single frame. The path is identical in all ten cases and so is the
// total beam-on time, so the total energy must be identical too -- `sum(E) =
// BeamPower * frameSeconds * mean(z)`, in which neither the speed nor the
// sample count appears. A renderer that computed 1/v and clamped it, or that
// deposited per *sample* rather than per sample *interval*, fails this by a
// factor of a hundred rather than by half a percent.
int checkEnergy()
{
	constexpr float kSigma  = 0.006f;
	constexpr float kPower  = 0.10f;
	constexpr double kSpan  = 0.8; // the line runs -0.8 .. +0.8 beam units
	constexpr int kSamples  = 4096;
	constexpr int kSpeeds   = 10;

	const int size = benchSize( kSigma );

	BeamGeometry beam;
	if( !beam.InitGL() )
	{
		std::fprintf( stderr, "energy: the beam renderer would not initialise\n" );
		return 1;
	}

	Target target = makeTarget( size, size, true );
	const GLint viewport[ 4 ] = { 0, 0, size, size };

	std::vector< Sample > block( kSamples );
	std::vector< double > totals( kSpeeds, 0.0 );
	std::vector< double > traversals( kSpeeds, 0.0 );

	double worstDensity = 1.0e30;
	int failures        = 0;

	for( int k = 0; k < kSpeeds; ++k )
	{
		// Ten values, log-spaced, exactly 100:1 end to end.
		const double s = std::pow( 100.0, static_cast< double >( k ) / static_cast< double >( kSpeeds - 1 ) );
		traversals[ static_cast< size_t >( k ) ] = s;

		const double dt = kFrameSeconds / static_cast< double >( kSamples - 1 );
		for( int i = 0; i < kSamples; ++i )
		{
			// A triangle: constant speed everywhere, a reversal at each end, and
			// no flyback segment to account for separately.
			const double u    = static_cast< double >( i ) / static_cast< double >( kSamples - 1 );
			const double turn = u * s;
			const double frac = turn - std::floor( turn );

			block[ static_cast< size_t >( i ) ].x  = static_cast< float >( kSpan * ( 4.0 * std::fabs( frac - 0.5 ) - 1.0 ) );
			block[ static_cast< size_t >( i ) ].y  = 0.0f;
			block[ static_cast< size_t >( i ) ].z  = 1.0f;
			block[ static_cast< size_t >( i ) ].dt = static_cast< float >( dt );
		}

		if( !beam.Render( block.data(), kSamples, benchParams( kSigma, kPower ),
		                  target.fbo, viewport, 0, 1.0f, 1.0f ) )
		{
			std::fprintf( stderr, "  %.1f traversals: render failed\n", s );
			++failures;
			continue;
		}

		const std::vector< float > pixels = readFloats( target );

		double sum = 0.0;
		for( size_t i = 0; i < pixels.size(); i += 4 )
			sum += static_cast< double >( pixels[ i ] ) + pixels[ i + 1 ] + pixels[ i + 2 ];
		totals[ static_cast< size_t >( k ) ] = sum;

		// How close the fastest sweep came to being culled outright. Printed
		// because it is the boundary of what this test can claim.
		const double segment = 4.0 * kSpan * s / static_cast< double >( kSamples - 1 );
		const double density = ( kPower * dt ) / ( segment * kSigma * 2.5066282746310002 );
		worstDensity         = std::min( worstDensity, density );
	}

	releaseTarget( target );
	beam.DeInitGL();

	double lowest = 1.0e300, highest = 0.0, mean = 0.0;
	for( double total : totals )
	{
		lowest  = std::min( lowest, total );
		highest = std::max( highest, total );
		mean += total / static_cast< double >( kSpeeds );
	}

	if( !( mean > 0.0 ) )
	{
		std::fprintf( stderr, "energy: nothing was drawn at any speed\n" );
		return failures + 1;
	}

	const double spread = ( highest - lowest ) / mean * 100.0;

	std::printf( "  a %.3f-unit line, %d samples, %g s of beam-on time, sigma %g\n",
	             2.0 * kSpan, kSamples, kFrameSeconds, static_cast< double >( kSigma ) );
	for( int k = 0; k < kSpeeds; ++k )
	{
		const double s = traversals[ static_cast< size_t >( k ) ];
		std::printf( "  %7.2f traversals/frame  %11.2f units/s   total light %.6e  %+.3f%%\n",
		             s, 4.0 * kSpan * s / kFrameSeconds, totals[ static_cast< size_t >( k ) ],
		             ( totals[ static_cast< size_t >( k ) ] - mean ) / mean * 100.0 );
	}
	std::printf( "  spread %.4f%% of the mean (tolerance 0.5%%)\n", spread );
	std::printf( "  the fastest sweep's peak areal density is %.3gx the %g cull floor;\n"
	             "  above that ratio this test would be measuring the cull, not the model\n",
	             worstDensity / 1.0e-4, 1.0e-4 );

	if( spread > 0.5 )
	{
		std::fprintf( stderr, "energy: %.4f%% spread across 100:1 of speed, tolerance 0.5%%\n", spread );
		++failures;
	}

	std::printf( failures == 0 ? "energy: ok\n" : "energy: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --dwell
//---------------------------------------------------------------------------
//
// Brightness proportional to dwell time, measured rather than asserted.
//
// A linearly decelerating segment, the rendered line density read off column by
// column, and a straight-line regression against 1/v. The renderer never
// computes a reciprocal anywhere -- it deposits a fixed quantum per interval and
// spreads it over the distance covered -- so this is a test that the identity
// falls out, not that a formula was typed in correctly.
//
// The ramp is 4:1 and not 10:1 for a reason that is physics rather than
// tolerance-fitting. The measured density is the ideal 1/v convolved with the
// beam's own spot, and that convolution adds sigma^2 * lambda'' / 2, which is
// real light in a real place. At 4:1 over this face that term is about a
// thousandth of the density; at 10:1 it is five percent, and the regression
// would then be measuring the spot size.
int checkDwell()
{
	constexpr float kSigma = 0.006f;
	constexpr float kPower = 0.30f;
	constexpr double kSpan = 0.8;
	constexpr int kSamples = 4096;
	constexpr double kRatio = 4.0;// v0 / v1
	constexpr int kProbes  = 48;

	const int size = benchSize( kSigma );

	BeamGeometry beam;
	if( !beam.InitGL() )
	{
		std::fprintf( stderr, "dwell: the beam renderer would not initialise\n" );
		return 1;
	}

	Target target = makeTarget( size, size, true );
	const GLint viewport[ 4 ] = { 0, 0, size, size };

	// x(t) = x0 + v0 t - a t^2 / 2, chosen so the beam covers exactly the span
	// in exactly one frame and ends at v0 / kRatio.
	const double length = 2.0 * kSpan;
	const double v0     = 2.0 * length / ( kFrameSeconds * ( 1.0 + 1.0 / kRatio ) );
	const double v1     = v0 / kRatio;
	const double accel  = ( v0 - v1 ) / kFrameSeconds;

	auto positionAt = [ & ]( double t ) { return -kSpan + v0 * t - 0.5 * accel * t * t; };
	auto speedAt    = [ & ]( double t ) { return v0 - accel * t; };

	std::vector< Sample > block( kSamples );
	const double dt = kFrameSeconds / static_cast< double >( kSamples - 1 );
	for( int i = 0; i < kSamples; ++i )
	{
		const double t = static_cast< double >( i ) * dt;
		block[ static_cast< size_t >( i ) ].x  = static_cast< float >( positionAt( t ) );
		block[ static_cast< size_t >( i ) ].y  = 0.0f;
		block[ static_cast< size_t >( i ) ].z  = 1.0f;
		block[ static_cast< size_t >( i ) ].dt = static_cast< float >( dt );
	}

	int failures = 0;
	if( !beam.Render( block.data(), kSamples, benchParams( kSigma, kPower ),
	                  target.fbo, viewport, 0, 1.0f, 1.0f ) )
	{
		std::fprintf( stderr, "dwell: render failed\n" );
		releaseTarget( target );
		beam.DeInitGL();
		return 1;
	}

	const std::vector< float > pixels  = readFloats( target );
	const std::vector< double > column = columnExcitation( pixels, size, size );

	releaseTarget( target );
	beam.DeInitGL();

	// The middle 84% of the sweep. The ends are excluded because the beam is
	// still arriving and already leaving there, and a half-populated column is
	// not a measurement of anything.
	std::vector< double > reciprocal, density;
	for( int p = 0; p < kProbes; ++p )
	{
		const double t = ( 0.08 + 0.84 * static_cast< double >( p ) / static_cast< double >( kProbes - 1 ) )
		                 * kFrameSeconds;
		const int index = columnFor( positionAt( t ), size );
		reciprocal.push_back( 1.0 / speedAt( t ) );
		density.push_back( column[ static_cast< size_t >( index ) ] );
	}

	const double r2 = rSquared( reciprocal, density );

	std::printf( "  a %.2f-unit segment decelerating %.0f:1, from %.1f to %.1f units/s\n",
	             length, kRatio, v0, v1 );
	std::printf( "  %d probes; density %.4g at the fast end, %.4g at the slow end (%.2f:1)\n",
	             kProbes, density.front(), density.back(), density.back() / density.front() );
	std::printf( "  density against 1/v: r2 = %.9f (tolerance 0.999)\n", r2 );

	if( !( r2 > 0.999 ) )
	{
		std::fprintf( stderr, "dwell: r2 = %.9f, expected > 0.999\n", r2 );
		++failures;
	}

	std::printf( failures == 0 ? "dwell: ok\n" : "dwell: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --rate
//---------------------------------------------------------------------------
//
// The same figure at 48, 96 and 192 kHz -- 800, 1600 and 3200 samples in a
// frame -- has to render the same picture. `dt` lives in the sample rather than
// in a block-wide sample rate precisely so that it does, and this is the test
// that the arithmetic actually works out: four times the samples, each carrying
// a quarter of the dt, is the same total light in the same places.
//
// Two numbers, because they answer different questions and only one of them is
// an identity.
//
// **How much light** is the identity, and it holds exactly: the total is
// BeamPower * frameSeconds * mean(z), in which the sample count does not
// appear at all.
//
// **Where the light is** cannot be quite as exact, and the third line printed
// below says why. Detail is not only a sample count -- it is the rate the whole
// signal chain is modelled at, and `Svf::Set` clamps a cutoff to 0.49 fs. The
// deflection amplifier's Bandwidth X defaults to about 39.7 kHz, which is above
// Draft's Nyquist and at 83% of Normal's, so the three settings render three
// slightly different amplifiers. What is left after that is per-sample
// structure: the deposit spacing, and the one-interval gap at every block
// boundary that is inherent to the last sample having no interval after it. It
// is a fifth of a sigma wide at Draft and a twentieth at Fine, it shrinks with
// Focus and grows with the figure's speed, and it is what "Draft: figures above
// a few hundred Hz visibly polygonalise" means measured rather than described.
//
// Through the real plugin, because Detail is a real control and switching it
// re-Prepares the whole engine. A fresh instance per rate, so all three start
// from the same phase, and every other control left at its shipped default.
int checkRate()
{
	constexpr int kSize   = 1024;// so the face buffer is 1024 and the spot is resolved
	constexpr int kFrames = 4;

	struct Case
	{
		Detail detail;
		const char* name;
	};
	const Case cases[] = {
		{ Detail::Draft, "Draft" },
		{ Detail::Normal, "Normal" },
		{ Detail::Fine, "Fine" },
	};

	Target target = makeTarget( kSize, kSize, true );
	int failures  = 0;

	auto totalLight = []( const std::vector< float >& a ) {
		double sum = 0.0;
		for( size_t i = 0; i < a.size(); i += 4 )
			sum += static_cast< double >( a[ i ] ) + a[ i + 1 ] + a[ i + 2 ];
		return sum;
	};

	// Normal is the calibration point, so it is what the other two are measured
	// against rather than against each other.
	auto rms = []( const std::vector< float >& a, const std::vector< float >& b ) {
		double sumSq = 0.0, refSq = 0.0;
		for( size_t i = 0; i < a.size(); i += 4 )
		{
			for( int c = 0; c < 3; ++c )
			{
				const double d = static_cast< double >( a[ i + c ] ) - b[ i + c ];
				sumSq += d * d;
				refSq += static_cast< double >( b[ i + c ] ) * b[ i + c ];
			}
		}
		return refSq > 0.0 ? std::sqrt( sumSq / refSq ) * 100.0 : 100.0;
	};

	// The same three rates twice. `sameAmplifier` brings Bandwidth X and Y
	// inside every rate's Nyquist so that the second run varies the sample count
	// and nothing else; the difference between the two runs is how much of the
	// first one was the amplifier rather than the renderer.
	auto run = [ & ]( bool sameAmplifier, double& draftRms, double& fineRms,
	                  double& draftLight, double& fineLight ) {
		std::vector< std::vector< float > > images;

		for( const Case& c : cases )
		{
			VectrixPlugin plugin( false );

			// A Lissajous: two sines at 3:2, at the plugin's own default frequency.
			plugin.SetFloatParameter( PT_DETAIL, static_cast< float >( c.detail ) );
			plugin.SetFloatParameter( PT_WAVE_X, static_cast< float >( Wave::Sine ) );
			plugin.SetFloatParameter( PT_WAVE_Y, static_cast< float >( Wave::Sine ) );
			plugin.SetFloatParameter( PT_FREQ_X, 0.55f );
			plugin.SetFloatParameter( PT_RATIO, 3.0f );

			// The tube's decorations off, so what is compared is the trace. They
			// would be identical in all three anyway, and identical pixels in the
			// denominator only flatter the RMS.
			plugin.SetFloatParameter( PT_GRATICULE, 0.0f );
			plugin.SetFloatParameter( PT_FACE_BLACK, 0.0f );
			plugin.SetFloatParameter( PT_HALATION, 0.0f );
			plugin.SetFloatParameter( PT_VIGNETTE, 0.0f );
			plugin.SetFloatParameter( PT_CURVATURE, 0.0f );

			if( sameAmplifier )
			{
				// 0.5 on the control is about 8.9 kHz -- comfortably inside 48
				// kHz's Nyquist, so all three model the same amplifier.
				plugin.SetFloatParameter( PT_OUT_BANDWIDTH_X, 0.5f );
				plugin.SetFloatParameter( PT_OUT_BANDWIDTH_Y, 0.5f );
			}

			if( !startPlugin( plugin, target ) )
			{
				std::fprintf( stderr, "  %s: InitGL failed\n", c.name );
				++failures;
				continue;
			}

			bool drew = true;
			for( int frame = 0; frame < kFrames && drew; ++frame )
				drew = renderFrame( plugin, target, frame );

			if( !drew )
			{
				std::fprintf( stderr, "  %s: render failed\n", c.name );
				++failures;
				plugin.DeInitGL();
				continue;
			}

			images.push_back( readFloats( target ) );
			if( !sameAmplifier )
				std::printf( "  %-6s %4.0f kHz, %d samples a frame\n", c.name,
				             sampleRateFor( c.detail ) / 1000.0,
				             static_cast< int >( std::lround( sampleRateFor( c.detail ) * kFrameSeconds ) ) );
			plugin.DeInitGL();
		}

		if( images.size() != 3 )
			return false;

		const double reference = totalLight( images[ 1 ] );
		draftLight = ( totalLight( images[ 0 ] ) / reference - 1.0 ) * 100.0;
		fineLight  = ( totalLight( images[ 2 ] ) / reference - 1.0 ) * 100.0;
		draftRms   = rms( images[ 0 ], images[ 1 ] );
		fineRms    = rms( images[ 2 ], images[ 1 ] );
		return true;
	};

	double draftRms = 0.0, fineRms = 0.0, draftLight = 0.0, fineLight = 0.0;
	double sameDraftRms = 0.0, sameFineRms = 0.0, sameDraftLight = 0.0, sameFineLight = 0.0;

	const bool ranShipped = run( false, draftRms, fineRms, draftLight, fineLight );
	const bool ranSame    = run( true, sameDraftRms, sameFineRms, sameDraftLight, sameFineLight );

	releaseTarget( target );

	if( !ranShipped || !ranSame )
	{
		std::printf( "rate: %d FAILED\n", failures + 1 );
		return failures + 1;
	}

	std::printf( "  total light   Draft %+.5f%%   Fine %+.5f%%   against Normal (tolerance 0.1%%)\n",
	             draftLight, fineLight );
	std::printf( "  image RMS     Draft  %.4f%%   Fine  %.4f%%   against Normal (tolerance 1%%)\n",
	             draftRms, fineRms );
	std::printf( "  with the deflection amplifier inside every rate's Nyquist the RMS is\n"
	             "  %.4f%% / %.4f%%, so %.0f%% of the Fine figure above is the amplifier being\n"
	             "  modelled at three different rates rather than anything the renderer did\n",
	             sameDraftRms, sameFineRms,
	             fineRms > 0.0 ? ( 1.0 - sameFineRms / fineRms ) * 100.0 : 0.0 );

	// The one that is an identity gets the tight bound.
	if( std::fabs( draftLight ) > 0.1 || std::fabs( fineLight ) > 0.1 )
	{
		std::fprintf( stderr, "rate: the light totals differ by %+.5f%% / %+.5f%% across a 4:1 sample count\n",
		              draftLight, fineLight );
		++failures;
	}
	if( draftRms > 1.0 || fineRms > 1.0 )
	{
		std::fprintf( stderr, "rate: %.4f%% / %.4f%% RMS at the shipped defaults, tolerance 1%%\n",
		              draftRms, fineRms );
		++failures;
	}

	std::printf( failures == 0 ? "rate: ok\n" : "rate: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --point
//---------------------------------------------------------------------------
//
// The case the closed form exists for.
//
// A beam that does not move has no length to spread its energy over, so a
// renderer that divides by speed has to clamp something here and the clamp is
// then the thing deciding how bright a stationary dot is. This one takes the
// point limit explicitly, and the limit is a Gaussian spot of exactly
// N.E / (2.pi.sigma^2) at its peak -- so that number can be computed on paper
// and compared with the texel.
//
// What actually bounds it is the phosphor, which is why the emitted value and
// the recovered excitation are both printed: the gap between them is the
// saturation doing its job.
int checkPoint()
{
	constexpr float kSigma = 0.006f;
	constexpr int kSamples = 4001;// 4000 intervals -- "a few thousand"

	const int size    = benchSize( kSigma );
	const double dt   = kFrameSeconds / static_cast< double >( kSamples - 1 );
	const int steps   = kSamples - 1;

	// Aim the analytic peak at about 1.0 excitation: high enough that the
	// phosphor's saturation is unmistakably in play, low enough to stay a long
	// way under the 1e6 excitation ceiling.
	const double twoPiSigmaSq = 2.0 * 3.14159265358979323846 * static_cast< double >( kSigma ) * kSigma;
	const float power = static_cast< float >( 1.0 * twoPiSigmaSq / kFrameSeconds );

	// On a texel centre. `faceUV` equals `uv` here, so texel (size/2, size/2)
	// is at beam coordinate (2*(size/2) + 1)/size - 1 on both axes.
	const double onCentre = ( 2.0 * static_cast< double >( size / 2 ) + 1.0 ) / static_cast< double >( size ) - 1.0;

	BeamGeometry beam;
	if( !beam.InitGL() )
	{
		std::fprintf( stderr, "point: the beam renderer would not initialise\n" );
		return 1;
	}

	Target target = makeTarget( size, size, true );
	const GLint viewport[ 4 ] = { 0, 0, size, size };

	std::vector< Sample > block( kSamples );
	for( Sample& s : block )
	{
		s.x  = static_cast< float >( onCentre );
		s.y  = static_cast< float >( onCentre );
		s.z  = 1.0f;
		s.dt = static_cast< float >( dt );
	}

	int failures = 0;
	if( !beam.Render( block.data(), kSamples, benchParams( kSigma, power ),
	                  target.fbo, viewport, 0, 1.0f, 1.0f ) )
	{
		std::fprintf( stderr, "point: render failed\n" );
		releaseTarget( target );
		beam.DeInitGL();
		return 1;
	}

	const std::vector< float > pixels = readFloats( target );
	releaseTarget( target );
	beam.DeInitGL();

	// Found rather than assumed: a peak in the wrong texel is a coordinate bug
	// that reading the expected texel would never see.
	double peak = 0.0;
	int peakX = -1, peakY = -1;
	for( int y = 0; y < size; ++y )
	{
		for( int x = 0; x < size; ++x )
		{
			const double green = pixels[ ( static_cast< size_t >( y ) * size + x ) * 4 + 1 ];
			if( green > peak )
			{
				peak  = green;
				peakX = x;
				peakY = y;
			}
		}
	}

	const double saturation = phosphor( 0 ).saturation;
	const double measured   = excitationFrom( peak, saturation );

	// The pedestal the trace fragment subtracts, so that the truncation of the
	// quad at 4.5 sigma is smooth rather than a step, costs the peak this much.
	const double pedestal = 1.0 - std::exp( -0.5 * 4.5 * 4.5 );
	const double analytic = static_cast< double >( steps ) * ( power * dt ) / twoPiSigmaSq;

	const bool finite  = std::isfinite( measured ) && std::isfinite( peak );
	const bool nonZero = measured > 0.0;
	const double error = analytic > 0.0 ? ( measured - analytic * pedestal ) / ( analytic * pedestal ) * 100.0 : 100.0;

	std::printf( "  %d intervals of %.4g s at sigma %g, %.4g energy each\n",
	             steps, dt, static_cast< double >( kSigma ), power * dt );
	std::printf( "  analytic  N.E/(2.pi.sigma^2) = %.6f excitation  (%.6f after the 4.5-sigma pedestal)\n",
	             analytic, analytic * pedestal );
	std::printf( "  measured  %.6f excitation at texel (%d, %d) of %d, %+.4f%%\n",
	             measured, peakX, peakY, size, error );
	std::printf( "  emitted   %.6f after the phosphor's saturation, which is what bounds it\n", peak );

	if( !finite || !nonZero )
	{
		std::fprintf( stderr, "point: the peak is %s\n", finite ? "zero" : "not finite" );
		++failures;
	}
	else if( std::fabs( error ) > 1.0 )
	{
		std::fprintf( stderr, "point: %+.4f%% off the analytic peak, tolerance 1%%\n", error );
		++failures;
	}

	std::printf( failures == 0 ? "point: ok\n" : "point: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --blank
//---------------------------------------------------------------------------
//
// `z` is a grid voltage and not an alpha, and the cheapest demonstration that
// it is being treated as one is a sweep with its middle third cut off. The beam
// still travels -- the samples are there and the positions are right -- it is
// simply not emitting, so the blanked span has to be empty and the lit spans
// have to reach full density right up to the boundary.
//
// Both halves matter. Light inside the blank means the gun is not really being
// cut; a dip in the lit spans means the blanking is eating into them, which on
// a raster shows up as a dark band at the edge of the picture that no control
// explains.
int checkBlank()
{
	constexpr float kSigma = 0.006f;
	constexpr float kPower = 0.30f;
	constexpr double kSpan = 0.8;
	constexpr int kSamples = 4096;

	const int size = benchSize( kSigma );

	BeamGeometry beam;
	if( !beam.InitGL() )
	{
		std::fprintf( stderr, "blank: the beam renderer would not initialise\n" );
		return 1;
	}

	Target target = makeTarget( size, size, true );
	const GLint viewport[ 4 ] = { 0, 0, size, size };

	const double dt = kFrameSeconds / static_cast< double >( kSamples - 1 );
	std::vector< Sample > block( kSamples );
	for( int i = 0; i < kSamples; ++i )
	{
		const double u = static_cast< double >( i ) / static_cast< double >( kSamples - 1 );
		block[ static_cast< size_t >( i ) ].x  = static_cast< float >( -kSpan + 2.0 * kSpan * u );
		block[ static_cast< size_t >( i ) ].y  = 0.0f;
		block[ static_cast< size_t >( i ) ].z  = ( u > 1.0 / 3.0 && u < 2.0 / 3.0 ) ? 0.0f : 1.0f;
		block[ static_cast< size_t >( i ) ].dt = static_cast< float >( dt );
	}

	int failures = 0;
	if( !beam.Render( block.data(), kSamples, benchParams( kSigma, kPower ),
	                  target.fbo, viewport, 0, 1.0f, 1.0f ) )
	{
		std::fprintf( stderr, "blank: render failed\n" );
		releaseTarget( target );
		beam.DeInitGL();
		return 1;
	}

	const std::vector< float > pixels  = readFloats( target );
	const std::vector< double > column = columnExcitation( pixels, size, size );

	releaseTarget( target );
	beam.DeInitGL();

	const double blankFrom = -kSpan + 2.0 * kSpan / 3.0;
	const double blankTo   = -kSpan + 4.0 * kSpan / 3.0;
	const double guard     = 5.0 * static_cast< double >( kSigma );

	// The lit level, measured well inside the first and last thirds.
	double litSum   = 0.0;
	int litCount    = 0;
	double litFloor = 1.0e300;
	for( int x = 0; x < size; ++x )
	{
		const double at = ( 2.0 * x + 1.0 ) / static_cast< double >( size ) - 1.0;
		const bool lit  = ( at > -kSpan + guard && at < blankFrom - guard )
		                 || ( at > blankTo + guard && at < kSpan - guard );
		if( !lit )
			continue;
		litSum += column[ static_cast< size_t >( x ) ];
		litFloor = std::min( litFloor, column[ static_cast< size_t >( x ) ] );
		++litCount;
	}
	const double litMean = litCount > 0 ? litSum / static_cast< double >( litCount ) : 0.0;

	// The deepest part of the blank: everything more than five sigma inside it.
	// The trace quad is only drawn out to 4.5 sigma, so there is not merely
	// little light out here -- there is provably none, and anything at all is a
	// segment that was drawn when it should not have been.
	double blankPeak = 0.0;
	for( int x = 0; x < size; ++x )
	{
		const double at = ( 2.0 * x + 1.0 ) / static_cast< double >( size ) - 1.0;
		if( at > blankFrom + guard && at < blankTo - guard )
			blankPeak = std::max( blankPeak, column[ static_cast< size_t >( x ) ] );
	}

	const double leak = litMean > 0.0 ? blankPeak / litMean : 1.0;
	const double dip  = litMean > 0.0 ? litFloor / litMean : 0.0;

	std::printf( "  a %.2f-unit sweep with z = 0 across %.3f .. %.3f\n", 2.0 * kSpan, blankFrom, blankTo );
	std::printf( "  lit density  mean %.6g, lowest %.6g (%.2f%% of the mean)\n", litMean, litFloor, dip * 100.0 );
	std::printf( "  blanked span, beyond 5 sigma from either endpoint: peak %.6g (%.3g of the lit level)\n",
	             blankPeak, leak );

	if( !( litMean > 0.0 ) )
	{
		std::fprintf( stderr, "blank: nothing was drawn at all\n" );
		++failures;
	}
	else
	{
		if( leak > 1.0e-9 )
		{
			std::fprintf( stderr, "blank: %.3g of the lit level leaks into the blanked span\n", leak );
			++failures;
		}
		// The lit spans must not be eaten into. A renderer that faded the beam
		// out over a span rather than cutting it would pass the leak check above
		// and fail this one.
		if( dip < 0.98 )
		{
			std::fprintf( stderr, "blank: the lit spans dip to %.2f%% of their mean\n", dip * 100.0 );
			++failures;
		}
	}

	std::printf( failures == 0 ? "blank: ok\n" : "blank: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --identity
//---------------------------------------------------------------------------
//
// The effect build with the beam turned down and the faceplate out of the way
// has to hand the clip back untouched. Not "close": the glass pass claims exact
// passthrough by construction -- every operation is an addition of a literal
// zero or a multiplication by a literal one -- so the assertion is on the bits.
//
// A difference of one part in 255 is worth printing rather than rounding away.
// It is invisible in isolation and perfectly visible in an A/B against the
// bypassed layer, which is where anybody would actually notice it.
struct IdentityResult
{
	int worst   = 0;
	long differ = 0;
	long total  = 0;
	bool drew   = false;
};

IdentityResult identityPass( bool cutTheBeam, int width, int height, int frames )
{
	IdentityResult result;

	Target target = makeTarget( width, height, false );
	const GLuint clip = makeClipTexture( width, height );

	VectrixPlugin plugin( true );

	plugin.SetFloatParameter( PT_BEAM_POWER, 0.0f );
	plugin.SetFloatParameter( PT_FACE_BLACK, 0.0f );
	plugin.SetFloatParameter( PT_CURVATURE, 0.0f );
	plugin.SetFloatParameter( PT_VIGNETTE, 0.0f );
	plugin.SetFloatParameter( PT_HALATION, 0.0f );
	plugin.SetFloatParameter( PT_GRATICULE, 0.0f );

	if( cutTheBeam )
	{
		// The VCA pulls the deflection to nothing, so the gate's radial detector
		// never reaches its threshold, so the gate closes -- and a closed gate in
		// Blank mode multiplies z by an envelope that reaches exactly zero. That
		// is the only route through the parameter list to a genuinely cut-off
		// beam, and it takes a few frames because both the bypass crossfade and
		// the gate's release are deliberately not instant.
		plugin.SetFloatParameter( PT_VCA_ON, 1.0f );
		plugin.SetFloatParameter( PT_VCA_LEVEL, 0.0f );
		plugin.SetFloatParameter( PT_GATE_ON, 1.0f );
		plugin.SetFloatParameter( PT_GATE_THRESHOLD, 1.0f );
		plugin.SetFloatParameter( PT_GATE_MODE, 0.0f );
		plugin.SetFloatParameter( PT_GATE_RELEASE, 0.0f );
	}

	if( startPlugin( plugin, target ) )
	{
		result.drew = true;
		for( int frame = 0; frame < frames && result.drew; ++frame )
			result.drew = renderFrame( plugin, target, frame, clip );
	}

	if( result.drew )
	{
		// The clip as uploaded, read back out of GL rather than kept beside it,
		// so the comparison cannot accidentally be against the harness's idea of
		// what it wrote.
		std::vector< unsigned char > wanted( static_cast< size_t >( width ) * height * 4 );
		glBindTexture( GL_TEXTURE_2D, clip );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, wanted.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );

		const std::vector< unsigned char > got = readBytes( target );

		result.total = static_cast< long >( width ) * height;
		for( size_t i = 0; i < got.size(); i += 4 )
		{
			int worstHere = 0;
			for( int c = 0; c < 4; ++c )
				worstHere = std::max( worstHere, std::abs( static_cast< int >( got[ i + c ] )
				                                           - static_cast< int >( wanted[ i + c ] ) ) );
			if( worstHere > 0 )
				++result.differ;
			result.worst = std::max( result.worst, worstHere );
		}
	}

	glDeleteTextures( 1, &clip );
	plugin.DeInitGL();
	releaseTarget( target );
	return result;
}

int checkIdentity()
{
	constexpr int kWidth  = 640;
	constexpr int kHeight = 360;

	int failures = 0;

	// One frame, because the phosphor accumulates: this is the plugin's best
	// possible case and the assertion is made against it.
	const IdentityResult asAsked = identityPass( false, kWidth, kHeight, 1 );
	if( !asAsked.drew )
	{
		std::fprintf( stderr, "identity: render failed\n" );
		return 1;
	}

	std::printf( "  Beam 0, Face Black 0, Curvature 0, Vignette 0, Halation 0, Graticule 0\n" );
	if( asAsked.worst == 0 )
	{
		std::printf( "  %ld pixels, bit-identical to the input\n", asAsked.total );
	}
	else
	{
		std::printf( "  %ld of %ld pixels differ, worst per-channel difference %d/255\n",
		             asAsked.differ, asAsked.total, asAsked.worst );
		++failures;
	}

	// The second measurement exists because the first one has two possible
	// causes and no way to tell them apart. Beam is an exponential control from
	// x0.1 to x10 -- `pow(10, -1 + 2v)` in `renderParams` -- so its bottom end
	// is a tenth of full power, not none, and a faceplate that was exact would
	// still fail above simply because there is a trace on it. Cutting the gun
	// through the gate removes the trace and leaves only the glass.
	const IdentityResult cut = identityPass( true, kWidth, kHeight, 12 );
	if( !cut.drew )
	{
		std::fprintf( stderr, "identity: the beam-cut pass would not render\n" );
		return failures + 1;
	}

	std::printf( "  the same, with the gun genuinely cut off (VCA 0 into a closed gate):\n" );
	if( cut.worst == 0 )
		std::printf( "    bit-identical -- so the faceplate itself is an exact passthrough\n" );
	else
		std::printf( "    %ld of %ld pixels differ, worst %d/255 -- the faceplate is NOT exact\n",
		             cut.differ, cut.total, cut.worst );

	if( failures > 0 && cut.worst == 0 )
		std::fprintf( stderr,
		              "identity: the faceplate is exact, so what breaks the passthrough is the\n"
		              "          beam: Beam 0 maps to beamPower 0.1, not 0\n" );

	std::printf( failures == 0 ? "identity: ok\n" : "identity: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --fx
//---------------------------------------------------------------------------
//
// Two claims, both pure CPU, both of which fail silently in the picture.
//
// **The mixing matrix is orthogonal.** An FDN whose matrix is not energy-
// preserving decays at a rate that is not the one you asked for, and if the
// error goes the other way it grows. `H = I - (2/N)J` is exactly orthogonal on
// paper; this checks the arithmetic that implements it.
//
// **A parameter change does not clear the tails.** This is the opposite of the
// habit every GPU plugin in this fleet has, where rebuilding on a parameter
// change is normal and correct, and it is therefore the thing most likely to be
// "fixed" into a bug by somebody working across both halves. A real pedal's
// bucket brigade keeps whatever is in it when the time knob moves.
int checkFx()
{
	int failures = 0;

	//-- (a) the Householder matrix ------------------------------------------
	{
		Rng rng( 0xC0FFEEull );
		double worstAbsolute = 0.0;
		double worstRelative = 0.0;

		for( int trial = 0; trial < 20000; ++trial )
		{
			std::array< float, 8 > v {};
			// Across a wide range of magnitudes: an orthogonality bug that only
			// shows on denormal-ish or very large inputs is still a bug.
			const float scale = std::pow( 10.0f, rng.Bipolar() * 4.0f );
			for( float& value : v )
				value = rng.Bipolar() * scale;

			double before = 0.0;
			for( float value : v )
				before += static_cast< double >( value ) * value;
			before = std::sqrt( before );

			Reverb::Householder( v );

			double after = 0.0;
			for( float value : v )
				after += static_cast< double >( value ) * value;
			after = std::sqrt( after );

			const double absolute = std::fabs( after - before );
			worstAbsolute         = std::max( worstAbsolute, absolute );
			if( before > 0.0 )
				worstRelative = std::max( worstRelative, absolute / before );
		}

		// Eight adds, one multiply and eight subtracts in single precision, so a
		// few ulps of the norm is the floor. A wrong matrix misses by percent.
		constexpr double kTolerance = 1.0e-5;
		std::printf( "  Householder over 20000 random vectors: worst | ||Hx|| - ||x|| | / ||x|| = %.3e\n",
		             worstRelative );
		std::printf( "    (worst absolute %.3e; float epsilon is %.3e)\n",
		             worstAbsolute, static_cast< double >( std::numeric_limits< float >::epsilon() ) );

		if( !( worstRelative <= kTolerance ) )
		{
			std::fprintf( stderr, "fx: the Householder matrix is not norm-preserving (%.3e)\n", worstRelative );
			++failures;
		}
	}

	//-- (b) the tails survive a knob move -----------------------------------
	{
		constexpr double kRate  = 96000.0;
		constexpr int kBlock    = 1024;

		auto excite = []( std::vector< Sample >& buffer, bool loud, int phase ) {
			for( size_t i = 0; i < buffer.size(); ++i )
			{
				const double t = static_cast< double >( phase + static_cast< int >( i ) ) / kRate;
				buffer[ i ].x  = loud ? static_cast< float >( 0.8 * std::sin( 6.283185307179586 * 220.0 * t ) ) : 0.0f;
				buffer[ i ].y  = loud ? static_cast< float >( 0.8 * std::cos( 6.283185307179586 * 330.0 * t ) ) : 0.0f;
				buffer[ i ].z  = 1.0f;
				buffer[ i ].dt = static_cast< float >( 1.0 / kRate );
			}
		};

		std::vector< Sample > buffer( kBlock );

		//---- the delay, on its own, because Chain does not expose it --------
		Delay delay;
		delay.Prepare( kRate );
		DelayParams dp;
		dp.enabled  = true;
		dp.timeMs   = 250.0f;
		dp.feedback = 0.75f;
		dp.mix      = 0.5f;
		delay.SetParams( dp );
		delay.SnapEnabled( true );// skip the 20 ms bypass fade; it is not what is under test

		int phase = 0;
		for( int i = 0; i < 40; ++i, phase += kBlock )// ~0.43 s of signal
		{
			excite( buffer, true, phase );
			delay.Process( buffer.data(), kBlock );
		}
		for( int i = 0; i < 110; ++i, phase += kBlock )// ~1.2 s of silence after it
		{
			excite( buffer, false, phase );
			delay.Process( buffer.data(), kBlock );
		}

		const double delayBefore = delay.TailEnergy();

		dp.timeMs = 500.0f;// the knob move
		delay.SetParams( dp );
		const double delayAfterSet = delay.TailEnergy();

		for( int i = 0; i < 20; ++i, phase += kBlock )
		{
			excite( buffer, false, phase );
			delay.Process( buffer.data(), kBlock );
		}
		const double delayAfterRun = delay.TailEnergy();

		// And the converse, so the measurement is known to be capable of
		// reporting zero: Reset() is the call that IS allowed to drop the tail.
		delay.Reset();
		excite( buffer, false, phase );
		delay.Process( buffer.data(), kBlock );
		const double delayAfterReset = delay.TailEnergy();

		std::printf( "  delay tail  %.6g -> %.6g after Time 250 to 500 ms -> %.6g after 0.2 s more\n",
		             delayBefore, delayAfterSet, delayAfterRun );
		std::printf( "              %.6g after an explicit Reset, which is the call that may clear it\n",
		             delayAfterReset );

		if( !( delayBefore > 0.0 ) )
		{
			std::fprintf( stderr, "fx: the delay never built a tail to test\n" );
			++failures;
		}
		else if( !( delayAfterSet > 0.0 ) || !( delayAfterRun > 0.0 ) )
		{
			std::fprintf( stderr, "fx: changing the delay time dropped the tail\n" );
			++failures;
		}
		if( delayAfterReset != 0.0 )
		{
			std::fprintf( stderr, "fx: Reset did not clear the delay, so the check above proves nothing\n" );
			++failures;
		}

		//---- the reverb, through the real Chain ----------------------------
		Chain chain;
		chain.Prepare( kRate );

		ChainParams cp;
		cp.reverb.enabled  = true;
		cp.reverb.decaySec = 6.0f;
		cp.reverb.size     = 1.0f;
		cp.reverb.mix      = 0.5f;
		cp.reverb.diffusion = 0.7f;
		chain.SetParams( cp );

		phase = 0;
		for( int i = 0; i < 40; ++i, phase += kBlock )
		{
			excite( buffer, true, phase );
			chain.Process( buffer.data(), kBlock );
		}
		for( int i = 0; i < 60; ++i, phase += kBlock )
		{
			excite( buffer, false, phase );
			chain.Process( buffer.data(), kBlock );
		}

		const double verbBefore = chain.ReverbBlock().TailEnergy();

		cp.reverb.size = 1.7f;// the knob move
		chain.SetParams( cp );
		const double verbAfterSet = chain.ReverbBlock().TailEnergy();

		for( int i = 0; i < 40; ++i, phase += kBlock )
		{
			excite( buffer, false, phase );
			chain.Process( buffer.data(), kBlock );
		}
		const double verbAfterRun = chain.ReverbBlock().TailEnergy();

		std::printf( "  reverb tail %.6g -> %.6g after Size 1.0 to 1.7 -> %.6g after 0.4 s more\n",
		             verbBefore, verbAfterSet, verbAfterRun );

		if( !( verbBefore > 0.0 ) )
		{
			std::fprintf( stderr, "fx: the reverb never built a tail to test\n" );
			++failures;
		}
		else if( !( verbAfterSet > 0.0 ) || !( verbAfterRun > 0.0 ) )
		{
			std::fprintf( stderr, "fx: changing the reverb size silenced the tank\n" );
			++failures;
		}
	}

	std::printf( failures == 0 ? "fx: ok\n" : "fx: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --drift
//---------------------------------------------------------------------------
//
// Ten minutes at 96 kHz is 57.6 million additions into the phase accumulator.
// In `double` that is nothing; in `float` the accumulator's fractional bits are
// gone within minutes and the figure visibly stalls -- and the bug takes ten
// minutes of running to appear, which is why it needs a test rather than an
// eye. Anyone "optimising" `Axis::phase` to float trips this.
//
// The reference is exact, not merely more precise. `inc` is a double, and a
// double is a dyadic rational m/2^s, so k*inc mod 1 is ((k*m) mod 2^s)/2^s --
// integer arithmetic in __int128, with no rounding anywhere in it.
int checkDrift()
{
	constexpr double kRate    = 96000.0;
	constexpr double kMinutes = 10.0;
	constexpr float kFreq     = 440.0f;
	constexpr int kBlock      = 16384;

	const long long total = static_cast< long long >( kRate * 60.0 * kMinutes );

	Oscillator osc;
	osc.Prepare( kRate );

	OscillatorParams params;
	// Saw, because it is a linear read of the phase: value = 2p - 1 away from
	// the discontinuity, so the accumulator can be recovered from the output
	// without inverting a sine. The polyBLEP correction is only applied within
	// one increment of the wrap, and those samples are skipped below.
	params.waveX  = Wave::Saw;
	params.waveY  = Wave::Sine;
	params.freqX  = kFreq;
	params.hardSync = false;
	osc.SetParams( params );

	const double inc = static_cast< double >( kFreq ) / kRate;

	// inc = mantissa * 2^-shift, exactly.
	int exponent      = 0;
	const double frac = std::frexp( inc, &exponent );
	const long long mantissa = static_cast< long long >( std::ldexp( frac, 53 ) );
	const int shift          = 53 - exponent;

	auto exactPhase = [ & ]( long long index ) {
		const __int128 product = static_cast< __int128 >( index ) * mantissa;
		const __int128 modulus = static_cast< __int128 >( 1 ) << shift;
		return static_cast< double >( static_cast< long long >( product % modulus ) ) / std::ldexp( 1.0, shift );
	};

	// What the same run would look like with a float accumulator, so the test's
	// own discrimination is on the record rather than assumed.
	float floatPhase = 0.0f;
	const float floatInc = static_cast< float >( inc );

	std::vector< Sample > block( kBlock );
	long long done = 0;
	int lastBlock  = 0;
	while( done < total )
	{
		const int n = static_cast< int >( std::min< long long >( kBlock, total - done ) );
		osc.Render( block.data(), n, 1.0 / kRate );

		for( int i = 0; i < n; ++i )
		{
			floatPhase += floatInc;
			if( floatPhase >= 1.0f )
				floatPhase -= std::floor( floatPhase );
		}

		done += n;
		lastBlock = n;
	}

	// Read the phase back out of the last block, skipping anything the polyBLEP
	// touched.
	//
	// `lastBlock` and not kBlock: the run does not divide evenly, so the final
	// call fills only part of the buffer and everything past it is the previous
	// block's data at the wrong sample indices. Reading it anyway is a beautiful
	// way to make this test fail against a perfectly good accumulator.
	const int tail             = lastBlock;
	const long long firstIndex = done - tail;

	double worst = 0.0;
	long long worstAt = -1;
	int checked      = 0;
	for( int i = 0; i < tail; ++i )
	{
		const long long index = firstIndex + i;
		const double expected = exactPhase( index );
		if( expected < 3.0 * inc || expected > 1.0 - 3.0 * inc )
			continue;

		const double measured = ( static_cast< double >( block[ static_cast< size_t >( i ) ].x ) + 1.0 ) * 0.5;
		const double error    = std::fabs( measured - expected );
		if( error > worst )
		{
			worst   = error;
			worstAt = index;
		}
		++checked;
	}

	const double floatError = std::fabs( static_cast< double >( floatPhase ) - exactPhase( total ) );

	// The floor is the readback, not the accumulator: `Sample::x` is a float, so
	// recovering the phase from it costs about 2^-24 whatever the accumulator
	// did. Anything at that level means the accumulator contributed nothing
	// measurable.
	constexpr double kTolerance = 1.0e-5;

	std::printf( "  %lld samples at %.0f kHz -- %.0f minutes -- of %.1f Hz saw, inc = %.17g\n",
	             total, kRate / 1000.0, kMinutes, static_cast< double >( kFreq ), inc );
	std::printf( "  worst phase error over %d samples of the final block: %.3e turns (at sample %lld)\n",
	             checked, worst, worstAt );
	std::printf( "  the readback itself costs %.3e turns, because Sample::x is a float\n",
	             std::ldexp( 1.0, -24 ) );
	std::printf( "  a float accumulator over the same run would be out by %.4f turns\n", floatError );

	int failures = 0;
	if( checked < 16 )
	{
		std::fprintf( stderr, "drift: only %d usable samples in the final block\n", checked );
		++failures;
	}
	if( !( worst < kTolerance ) )
	{
		std::fprintf( stderr, "drift: %.3e turns of error after %.0f minutes, tolerance %.0e\n",
		              worst, kMinutes, kTolerance );
		++failures;
	}
	if( !( floatError > 1.0e-3 ) )
		std::printf( "  NOTE: a float accumulator would pass this too, so the test proves less\n"
		             "        than it looks like it does at this frequency\n" );

	std::printf( failures == 0 ? "drift: ok\n" : "drift: %d FAILED\n", failures );
	return failures;
}

//---------------------------------------------------------------------------
// --names
//---------------------------------------------------------------------------
/// Names and display strings fit the sixteen characters FFGL allows.
///
/// FFGL truncates a parameter NAME at 16 characters, and the same limit applies
/// to the DISPLAY string: FF_GET_PARAMETER_DISPLAY hands the host a 16-byte
/// buffer, and the SDK's own default writes into `static char
/// s_DisplayValue[ 16 ]`. Both truncations happen in the HOST, silently.
/// Nothing plugin-side ever notices -- this plugin's buffer is 64, snprintf
/// succeeds, and `--list` happily prints the whole string. So it ships, and an
/// operator sees half a word. That is cogwheel #5, where nine display strings
/// were over the limit and only the one that cut mid-word got reported.
///
/// v0.1.8 gave this plugin twenty-one real-unit displays (Hz, dB, ms, ratios)
/// and no check that any of them fit. This is that check.
///
/// A display string is a function of the VALUES, not of the parameter, so it
/// has to be swept rather than read once at the defaults -- and because
/// Resolve() reads the whole params[] array, every parameter's display is read
/// again at every step of every other parameter's sweep.
int checkNames( bool overInput )
{
	VectrixPlugin plugin( overInput );
	const char* which = overInput ? "effect" : "source";

	int over = 0;

	for( unsigned int id = 0; id < PT_COUNT; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name == nullptr )
			continue;
		const size_t length = std::strlen( name );
		if( length > 16 )
		{
			std::printf( "  %-8s name     %-3u  %-28s %zu\n", which, id, name, length );
			++over;
		}
	}

	std::vector< float > defaults( PT_COUNT );
	for( unsigned int id = 0; id < PT_COUNT; ++id )
		defaults[ id ] = plugin.GetFloatParameter( id );

	// Keyed by PARAMETER, holding its widest offender. Keying by the rendered
	// string instead reports one row per numeric value, which is a screenful of
	// rows for what is really one broken format.
	std::map< unsigned int, std::string > tooLong;

	// Strictly below PT_ABOUT_TEXT. The About buttons OPEN A WEB BROWSER when
	// their value is set -- a sweep would do it a few hundred times.
	for( unsigned int swept = 0; swept < PT_ABOUT_TEXT; ++swept )
	{
		for( int step = 0; step <= 100; ++step )
		{
			plugin.SetFloatParameter( swept, static_cast< float >( step ) / 100.0f );

			for( unsigned int id = 0; id < PT_ABOUT_TEXT; ++id )
			{
				const char* display = plugin.GetParameterDisplay( id );
				if( display == nullptr )
					continue;

				const size_t length = std::strlen( display );
				if( length > 16 && length > tooLong[ id ].size() )
					tooLong[ id ] = display;
			}
		}
		plugin.SetFloatParameter( swept, defaults[ swept ] );
	}

	for( const auto& entry : tooLong )
	{
		const char* name = plugin.GetParamName( entry.first );
		std::printf( "  %-8s display  %-3u  %-18s %-24s %zu\n", which, entry.first,
		             name ? name : "", entry.second.c_str(), entry.second.size() );
	}

	over += static_cast< int >( tooLong.size() );
	return over;
}

int checkNames()
{
	std::printf( "names and displays longer than FFGL's 16 characters:\n\n" );

	// Both variants: they do not declare the same controls, so checking one
	// says nothing about the other.
	const int over = checkNames( false ) + checkNames( true );

	std::printf( "\n  %d over the limit\n", over );
	return over == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --list
//---------------------------------------------------------------------------
int listParameters()
{
	VectrixPlugin plugin( false );

	const char* types[ 256 ] = {};
	types[ FF_TYPE_BOOLEAN ]  = "boolean";
	types[ FF_TYPE_EVENT ]    = "event";
	types[ FF_TYPE_RED ]      = "red";
	types[ FF_TYPE_GREEN ]    = "green";
	types[ FF_TYPE_BLUE ]     = "blue";
	types[ FF_TYPE_XPOS ]     = "x";
	types[ FF_TYPE_YPOS ]     = "y";
	types[ FF_TYPE_STANDARD ] = "standard";
	types[ FF_TYPE_OPTION ]   = "option";
	types[ FF_TYPE_BUFFER ]   = "buffer";
	types[ FF_TYPE_INTEGER ]  = "integer";
	types[ FF_TYPE_FILE ]     = "file";
	types[ FF_TYPE_TEXT ]     = "text";

	for( unsigned int id = 0; id < PT_COUNT; ++id )
	{
		const char* name        = plugin.GetParamName( id );
		const unsigned int type = plugin.GetParamType( id );
		const char* typeName    = ( type < 256 && types[ type ] != nullptr ) ? types[ type ] : "other";

		std::printf( "%3u  %-24s %-9s %10.4f", id, name != nullptr ? name : "?", typeName,
		             plugin.GetFloatParameter( id ) );

		const RangeStruct range = plugin.GetParamRange( id );
		if( range.min != range.max )
			std::printf( "   [%g .. %g]", range.min, range.max );

		// What the host is told the value MEANS. Printing it here is the only
		// way to see the display strings without a host, and it keeps this
		// listing honest: it showed 0.7700 for controls the plugin knows the
		// real units of.
		const char* shown = plugin.GetParameterDisplay( id );
		if( shown != nullptr && *shown != '\0' )
			std::printf( "   %s", shown );
		std::printf( "\n" );
	}

	return 0;
}

/**
    --pipe: the project video, rendered rather than filmed.

    Frames leave as raw RGBA on stdout, so ffmpeg does the encoding and this does
    the tube. That is how the project video is made, and it is a render rather
    than a screen recording for a reason worth stating: an FFGL plugin has no
    window and no UI of its own -- its control surface IS Resolume's inspector --
    so "filming the app" would mean filming Arena, whose clip grid and effects
    browser are custom-drawn with nothing in the accessibility tree to address.

    What is on screen is genuinely this plugin's output, from the same class
    Resolume loads. It is just not a photograph of Resolume, and the end card
    says so.

    ## The half of this that is not like the rest of the family

    **By default the pipe reads nothing from stdin.** Every other harness in the
    fleet is an effect: a frame goes in, a frame comes out, and the run ends when
    the input stream does. Vectrix ships two plugins and the default one --
    Vectrix, the source -- takes *no input at all*. It is an oscillator and a
    tube; there is nothing to put through it. So the default here generates
    `--frames N` frames and stops on the count, because there is no stream to run
    out of, and stdin is never read.

    `--effect` selects the other build, Vectrix Trace, which paints the incoming
    clip on the tube face and can derive its path from it. That one behaves
    exactly like `tiltest`: one frame in, one frame out, stopping at EOF, and
    `--frames` is ignored because the stream decides.

    Both write one frame of `width * height * 4` bytes per input or per count,
    top-down, which is the order ffmpeg hands over and expects back.
*/
int runPipe( int width, int height, int frames, double fps, bool effect,
             const std::string& scriptPath,
             const std::vector< std::pair< std::string, float > >& sets )
{
	std::map< std::string, Track > tracks;
	if( !scriptPath.empty() )
	{
		std::string error;
		tracks = loadScript( scriptPath, error );
		if( !error.empty() )
		{
			std::fprintf( stderr, "vxtest: %s\n", error.c_str() );
			return 1;
		}
	}

	Target target = makeTarget( width, height, false );

	VectrixPlugin plugin( effect );
	if( applySets( plugin, sets ) > 0 )
	{
		releaseTarget( target );
		return 1;
	}

	//Resolve the cue sheet's names once, against the plugin itself. A cue for a
	//parameter that does not exist is a silent no-op otherwise, and the first
	//sign of it is a beat in the finished video where nothing happens.
	const std::map< std::string, unsigned int > byName = parameterIndex( plugin );
	std::vector< std::pair< unsigned int, const Track* > > bound;
	for( const auto& entry : tracks )
	{
		const auto found = byName.find( entry.first );
		if( found == byName.end() )
		{
			std::fprintf( stderr, "vxtest: no parameter named \"%s\" in the script\n",
			              entry.first.c_str() );
			releaseTarget( target );
			return 1;
		}
		warnAboutRamps( plugin, entry.first, found->second, entry.second );
		bound.emplace_back( found->second, &entry.second );
	}

	//The clip, for the effect build only. The source build has no input and this
	//stays zero, which is what tells ProcessOpenGL there is no texture to bind.
	GLuint input = 0;
	if( effect )
	{
		glGenTextures( 1, &input );
		glBindTexture( GL_TEXTURE_2D, input );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	if( !startPlugin( plugin, target ) )
	{
		std::fprintf( stderr, "vxtest: InitGL failed\n" );
		if( input != 0 )
			glDeleteTextures( 1, &input );
		releaseTarget( target );
		return 1;
	}

	const size_t frameBytes = static_cast< size_t >( width ) * height * 4;
	std::vector< unsigned char > incoming( frameBytes );

	//Seconds per frame for SetTime. It has to be the rate the finished video will
	//be played at or the picture runs slow: this plugin's whole output is a
	//function of elapsed time, unlike the pure-function effects the rest of the
	//family films. `Clock` clamps a frame to [1/240, 1/24], so anything under 24
	//fps is silently rendered as 24.
	const double secondsPerFrame = fps > 0.0 ? 1.0 / fps : kFrameSeconds;

	int frame  = 0;
	int status = 0;
	for( ;; )
	{
		if( effect )
		{
			if( !readExactly( incoming.data(), frameBytes ) )
				break;

			//ffmpeg hands over rows top-down and GL wants them bottom-up.
			//Flipping on the way in and again on the way out keeps every
			//coordinate in this file meaning what it says everywhere else.
			const std::vector< unsigned char > flipped = flipRows( incoming, width, height );

			glBindTexture( GL_TEXTURE_2D, input );
			glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
			                 flipped.data() );
			glBindTexture( GL_TEXTURE_2D, 0 );
		}
		else if( frame >= std::max( 1, frames ) )
		{
			break;
		}

		for( const auto& track : bound )
			plugin.SetFloatParameter( track.first, valueAt( *track.second, frame ) );

		if( !renderFrame( plugin, target, frame, input, secondsPerFrame ) )
		{
			std::fprintf( stderr, "vxtest: render failed at frame %d\n", frame );
			status = 1;
			break;
		}

		const std::vector< unsigned char > out = flipRows( readBytes( target ), width, height );
		if( fwrite( out.data(), 1, frameBytes, stdout ) != frameBytes )
		{
			std::fprintf( stderr, "vxtest: short write at frame %d\n", frame );
			status = 1;
			break;
		}

		++frame;
	}

	fflush( stdout );
	std::fprintf( stderr, "vxtest: %d frames (%s build, %g fps)\n", frame,
	              effect ? "effect" : "source", fps );

	plugin.DeInitGL();
	if( input != 0 )
		glDeleteTextures( 1, &input );
	releaseTarget( target );
	return status;
}

void usage()
{
	std::printf(
		"vxtest -- the vectrix offline harness\n"
		"\n"
		"  --energy            one sweep at ten speeds spanning 100:1 deposits the same light\n"
		"  --dwell             line density tracks 1/v along a decelerating segment\n"
		"  --rate              the same figure at Draft / Normal / Fine renders the same\n"
		"  --point             a stationary beam reaches the analytic N.E/(2.pi.sigma^2)\n"
		"  --blank             z = 0 deposits nothing and does not leak past its endpoints\n"
		"  --identity          the effect build at Beam 0 is bit-identical to its input\n"
		"  --fx                Householder is orthogonal; a knob move keeps the tails (no GL)\n"
		"  --drift             ten minutes of phase accumulation stays exact (no GL)\n"
		"  --all               every check above, with a summary\n"
		"\n"
		"  --out PATH          render one frame to a PNG\n"
		"  --size WxH          the raster (default 1920x1080)\n"
		"  --frames N          frames to render (default 8). For --out, how many to\n"
		"                      render before capturing the last; for --pipe on the\n"
		"                      source build, how many to emit\n"
		"  --preset N          apply factory preset N (1 .. %d)\n"
		"  --effect            the effect build, Vectrix Trace, over an input\n"
		"  --set ID=VALUE      set a parameter, by id or by name (repeatable)\n"
		"  --list              every parameter: id, name, type, current value, range\n"
		"  --names             names and displays fit FFGL's 16 characters\n"
		"\n"
		"  --pipe              raw RGBA frames out on stdout, for the project video\n"
		"                      SOURCE BUILD (the default): reads NOTHING from stdin.\n"
		"                      The source plugin takes no input, so there is no stream\n"
		"                      to run out of -- it generates --frames N frames and stops.\n"
		"                      WITH --effect: one raw RGBA frame in on stdin, one out,\n"
		"                      stopping at EOF, and --frames is ignored.\n"
		"  --script PATH       parameter automation: `frame Parameter Name value`\n"
		"                      Keys are held before the first and after the last, and\n"
		"                      LINEARLY INTERPOLATED between. An option or a boolean is\n"
		"                      read by rounding, so it steps -- a slide from one entry\n"
		"                      to another passes through everything in between. Give\n"
		"                      Source, Wave X/Y, Shape, Mesh, Phosphor, Detail, Ratio\n"
		"                      and every Routing a HOLD KEY AT THE END of each section\n"
		"                      they must not move in, not merely one where they change.\n"
		"  --fps N             the rate the pipe's clock advances at (default 60).\n"
		"                      Must match the rate the footage is encoded at, or the\n"
		"                      picture runs slow -- this plugin's output is a function\n"
		"                      of elapsed time. Clock clamps a frame to 24..240 fps.\n",
		presets::kCount );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath;
	std::string scriptPath;
	std::vector< std::pair< std::string, float > > sets;

	int width   = 1920;
	int height  = 1080;
	int frames  = 8;
	int preset  = 0;
	double fps  = 60.0;
	bool effect = false;
	bool pipeMode = false;

	bool doList     = false;

	bool doNames   = false;
	bool doEnergy   = false;
	bool doDwell    = false;
	bool doRate     = false;
	bool doPoint    = false;
	bool doBlank    = false;
	bool doIdentity = false;
	bool doFx       = false;
	bool doDrift    = false;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		auto next             = [ & ]() -> std::string { return ( i + 1 < argc ) ? argv[ ++i ] : std::string(); };

		if( arg == "--out" ) outPath = next();
		else if( arg == "--frames" ) frames = std::atoi( next().c_str() );
		else if( arg == "--preset" ) preset = std::atoi( next().c_str() );
		else if( arg == "--effect" ) effect = true;
		else if( arg == "--pipe" ) pipeMode = true;
		else if( arg == "--script" ) scriptPath = next();
		else if( arg == "--fps" ) fps = std::atof( next().c_str() );
		else if( arg == "--list" ) doList = true;
		else if( arg == "--names" ) doNames = true;
		else if( arg == "--energy" ) doEnergy = true;
		else if( arg == "--dwell" ) doDwell = true;
		else if( arg == "--rate" ) doRate = true;
		else if( arg == "--point" ) doPoint = true;
		else if( arg == "--blank" ) doBlank = true;
		else if( arg == "--identity" ) doIdentity = true;
		else if( arg == "--fx" ) doFx = true;
		else if( arg == "--drift" ) doDrift = true;
		else if( arg == "--all" )
			doEnergy = doDwell = doRate = doPoint = doBlank = doIdentity = doFx = doDrift = true;
		else if( arg == "--size" )
		{
			const std::string value = next();
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
		}
		else if( arg == "--set" )
		{
			const std::string value = next();
			const size_t equals     = value.find( '=' );
			if( equals != std::string::npos )
				sets.emplace_back( value.substr( 0, equals ),
				                   static_cast< float >( std::atof( value.substr( equals + 1 ).c_str() ) ) );
		}
		else
		{
			usage();
			return arg == "--help" || arg == "-h" ? 0 : 1;
		}
	}

	const bool anyGL = doEnergy || doDwell || doRate || doPoint || doBlank || doIdentity
	                   || !outPath.empty() || pipeMode;
	const bool any   = anyGL || doFx || doDrift || doList || doNames;
	if( !any )
	{
		usage();
		return 1;
	}

	// Line-buffered, so that a failure written to stderr appears where it
	// happened rather than ahead of the whole run's stdout. Every check here
	// prints its measurements before deciding whether they are a failure, and
	// the two streams interleaving wrongly makes the report unreadable.
	//
	// Not in --pipe: there stdout is a stream of raw frames, and it wants the
	// biggest block writes it can get rather than a flush per newline that
	// happens to appear in the pixels.
	if( !pipeMode )
		std::setvbuf( stdout, nullptr, _IOLBF, 0 );

	int failures = 0;

	// --list, --fx and --drift touch no GL at all, so they run before the
	// context exists and work on a machine with no display.
	if( doList )
		failures += listParameters();
	if( doNames )
		failures += checkNames();
	if( doFx )
		failures += checkFx();
	if( doDrift )
		failures += checkDrift();

	CGLContextObj context = nullptr;
	if( anyGL )
	{
		context = createContext();
		if( context == nullptr )
		{
			std::fprintf( stderr, "could not create a GL 4.1 core context\n" );
			return 1;
		}
	}

	// The pipe owns stdout, so it is the whole run when it is asked for: mixing a
	// PNG's progress line into a frame stream would corrupt the video and the
	// symptom would be a torn picture rather than an error.
	if( pipeMode )
	{
		// At the front, so an explicit --set still wins over the preset it sits
		// on top of -- which is the order --out has always applied them in.
		if( preset > 0 )
			sets.insert( sets.begin(),
			             std::make_pair( std::string( "Preset" ), static_cast< float >( preset ) ) );

		const int status = runPipe( width, height, frames, fps, effect, scriptPath, sets );
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return status;
	}

	if( doEnergy )
		failures += checkEnergy();
	if( doDwell )
		failures += checkDwell();
	if( doRate )
		failures += checkRate();
	if( doPoint )
		failures += checkPoint();
	if( doBlank )
		failures += checkBlank();
	if( doIdentity )
		failures += checkIdentity();

	if( !outPath.empty() )
	{
		Target target = makeTarget( width, height, false );

		VectrixPlugin plugin( effect );
		if( preset > 0 )
			plugin.SetFloatParameter( PT_PRESET, static_cast< float >( preset ) );

		failures += applySets( plugin, sets );

		GLuint clip = effect ? makeClipTexture( width, height ) : 0;

		if( !startPlugin( plugin, target ) )
		{
			std::fprintf( stderr, "InitGL failed\n" );
			++failures;
		}
		else
		{
			bool drew = true;
			for( int frame = 0; frame < std::max( 1, frames ) && drew; ++frame )
				drew = renderFrame( plugin, target, frame, clip );

			if( !drew )
			{
				std::fprintf( stderr, "render failed\n" );
				++failures;
			}
			else
			{
				const std::vector< unsigned char > image = flipRows( readBytes( target ), width, height );
				if( !writePng( outPath, width, height, image ) )
				{
					std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
					++failures;
				}
				else
				{
					float r = 0, g = 0, b = 0, a = 0;
					const std::vector< float > centre = readFloats( target );
					samplePixel( centre, width, height, 0.5f, 0.5f, r, g, b, a );
					std::printf( "wrote %s (%d x %d, %d frames at 1/60 s, centre %.4f %.4f %.4f a %.4f)\n",
					             outPath.c_str(), width, height, std::max( 1, frames ), r, g, b, a );
				}
			}
			plugin.DeInitGL();
		}

		if( clip != 0 )
			glDeleteTextures( 1, &clip );
		releaseTarget( target );
	}

	if( doEnergy && doDwell && doRate && doPoint && doBlank && doIdentity && doFx && doDrift )
		std::printf( "\n%s\n", failures == 0 ? "all checks passed" : "SOME CHECKS FAILED" );

	if( context != nullptr )
	{
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
	}
	return failures == 0 ? 0 : 1;
}
