#include "signal/sources/AudioFile.h"

#include "Diag.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

//---------------------------------------------------------------------------
// The decoders. Compiled here and in no other translation unit.
//
// dr_wav, dr_flac and dr_mp3 are single-header public-domain decoders whose
// implementations appear only when the DR_..._IMPLEMENTATION macros are
// defined -- so defining one of them in a second .cpp costs a duplicate symbol
// for every function in a 500 kB header, which is a link error nobody enjoys
// reading. flipbook does the same thing with stb_image in Sheet.cpp, and for
// the same reason: one decoder, one home.
//
// dr_wav covers AIFF and AIFC as well as RIFF WAVE, W64 and RF64, which is why
// there is no separate AIFF path here.
//---------------------------------------------------------------------------
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

// Opus is the one decoder that is a real library rather than a header, so it is
// behind a CMake option. Every use of it in this file is guarded, and with the
// option off the .opus extension is not offered either -- an extension in the
// file dialogue that cannot be decoded is worse than one that is missing.
#if VECTRIX_WITH_OPUS
	#include <opusfile.h>
#endif

// stb_vorbis is a .c file with no header/implementation split, so it is
// included **last of all, inside an unnamed namespace, behind a visibility
// push**. All three parts do different work and none of them is decorative:
//
// - **Last**, because its implementation section defines `L`, `C` and `R` as
//   object-like macros and never undefines them. Anything compiled after that
//   which uses one of those as an identifier fails with an error pointing at a
//   line that looks perfectly correct. The undef block below puts the
//   preprocessor back the way it was found.
// - **Unnamed namespace**, which contains the file-scope helpers it declares
//   without `static` -- but *not* its API, because that half of the file is
//   inside its own `extern "C"` block and a C-linkage function keeps external
//   linkage wherever it is written. `nm` on the object is what settles this,
//   not the shape of the source.
// - **Hidden visibility**, which is what actually keeps `stb_vorbis_*` out of
//   the bundle's export table. This matters because the bundle is dlopened
//   into Resolume, which is entitled to have loaded a vorbis decoder of its
//   own. The system headers stb_vorbis wants are included above so that only
//   its own definitions land inside the push -- a libc declaration marked
//   hidden is an undefined hidden symbol, which is a link error on ELF.
//
// STB_VORBIS_NO_PUSHDATA_API drops the streaming half of the library. Nothing
// here streams -- the file is decoded in one go at a block boundary -- and the
// pushdata API is the half that carries stb_vorbis's own reentrancy caveats.
#if defined( __GNUC__ ) || defined( __clang__ )
	#pragma GCC visibility push( hidden )
#endif
namespace
{
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
} // namespace
#if defined( __GNUC__ ) || defined( __clang__ )
	#pragma GCC visibility pop
#endif

#undef C
#undef L
#undef R
#undef CHECK
#undef CODEBOOK_ELEMENT
#undef CODEBOOK_ELEMENT_BASE
#undef CODEBOOK_ELEMENT_FAST
#undef CRC32_POLY
#undef DECODE
#undef DECODE_RAW
#undef DIVTAB_DENOM
#undef DIVTAB_NUMER
#undef EOP
#undef FALSE
#undef FAST_HUFFMAN_TABLE_MASK
#undef INVALID_BITS
#undef LINE_OP
#undef MAX_BLOCKSIZE
#undef MAX_BLOCKSIZE_LOG
#undef NO_CODE
#undef PLAYBACK_LEFT
#undef PLAYBACK_MONO
#undef PLAYBACK_RIGHT
#undef SAMPLE_unknown
#undef TRUE
#undef array_size_required
#undef temp_alloc
#undef temp_alloc_restore
#undef temp_alloc_save
#undef temp_block_array
#undef temp_free

namespace vectrix
{
namespace
{
constexpr double kTwoPi = 6.283185307179586476925286766559;

/// int16 to +/-1. 32768 and not 32767, so that the most negative code maps to
/// exactly -1 and the scale is a power of two -- an exact multiply with no
/// rounding, which matters when the same file has to render identically in the
/// harness and in the host.
constexpr float kInt16Scale = 1.0f / 32768.0f;

/// X, Y and Z. A fourth channel in an audio file is not a fourth thing the beam
/// can do, so it is dropped at load rather than carried around at 33% overhead.
constexpr int kMaxKeptChannels = 3;

/// The cubic needs four taps, so four frames is the shortest thing that can be
/// called a file and the shortest a Start/End section is allowed to become.
constexpr std::size_t kMinFrames = 4;

/// The gun is cut for this long across a loop join. One millisecond is enough
/// for the deflection amplifier's slew limiter downstream to have swung most of
/// the way to the new position before the beam comes back on.
constexpr double kBlankSeconds = 0.001;

constexpr float kMonoDelayMinMs = 1.0f;
constexpr float kMonoDelayMaxMs = 20.0f;

/// Derivative mode is scaled so a full-scale sine at this frequency draws a
/// unit circle: for x = sin(2*pi*f*t), dx/dt = 2*pi*f*cos(2*pi*f*t), so
/// dividing by 2*pi*200 makes 200 Hz come out at full deflection. 200 Hz is
/// where most sung and played material has its fundamental.
constexpr float kDerivativeReferenceHz = 200.0f;

/// ...and it is clamped, because a differentiator has no upper bound. One
/// badly-edited splice in a mono file is a single-sample step whose slope is
/// arbitrarily large, and unclamped it would throw Y far enough off the face
/// that the renderer draws a full-width streak across the tube for one frame.
/// Four is already four times full deflection, so nothing musical is touched.
constexpr float kDerivativeCeiling = 4.0f;

//---------------------------------------------------------------------------
// Decoding
//---------------------------------------------------------------------------

enum class Codec
{
	Wav, ///< also AIFF, AIFC, W64 and RF64 -- dr_wav sniffs the container
	Flac,
	Vorbis,
	Opus,
	Mp3,
	Count
};

struct Decoded
{
	std::vector< std::int16_t > pcm;
	int sourceChannels     = 0; ///< what the file had
	int channels           = 0; ///< what was kept, at most kMaxKeptChannels
	double rate            = 0.0;
	std::uint64_t frames   = 0;
	std::uint64_t sourceFrames = 0; ///< 0 when the decoder would not commit to a length
	bool truncated         = false;
	std::string note;
};

std::string Basename( const std::string& path )
{
	const std::size_t cut = path.find_last_of( "/\\" );
	return cut == std::string::npos ? path : path.substr( cut + 1 );
}

std::string Extension( const std::string& path )
{
	const std::size_t dot   = path.find_last_of( '.' );
	const std::size_t slash = path.find_last_of( "/\\" );
	if( dot == std::string::npos || ( slash != std::string::npos && dot < slash ) )
		return std::string();

	std::string ext = path.substr( dot + 1 );
	for( char& c : ext )
		c = static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
	return ext;
}

/// m:ss. Not h:mm:ss -- the cap is eight minutes, so there is no hours column
/// to get wrong.
std::string TimeText( double seconds )
{
	if( !( seconds > 0.0 ) )
		seconds = 0.0;

	const int whole = static_cast< int >( seconds + 0.5 );
	char text[ 32 ];
	std::snprintf( text, sizeof( text ), "%d:%02d", whole / 60, whole % 60 );
	return text;
}

std::uint64_t CapFramesFor( double rate )
{
	return static_cast< std::uint64_t >( kMaxAudioSeconds * rate );
}

/// The extension is a hint about which decoder to try first, not a decision.
/// A file named .wav that is really a FLAC is a thing that happens, and the
/// fallback sweep in `Decode` handles it.
Codec CodecForExtension( const std::string& ext )
{
	if( ext == "wav" || ext == "wave" || ext == "aiff" || ext == "aif" || ext == "aifc"
	    || ext == "w64" || ext == "rf64" )
		return Codec::Wav;
	if( ext == "flac" )
		return Codec::Flac;
	if( ext == "ogg" || ext == "oga" )
		return Codec::Vorbis;
	if( ext == "opus" )
		return Codec::Opus;
	if( ext == "mp3" )
		return Codec::Mp3;
	return Codec::Count;
}

/**
	Pull the whole stream through `read` into `d.pcm`, keeping at most three
	channels and at most `capFrames` frames.

	`read( destination, frames )` returns how many frames it actually produced,
	0 at the end. **Every call asks for a whole chunk**, never a short tail
	sized to what is left under the cap: `op_read_stereo` only promises to make
	progress when the buffer can hold 120 ms at 48 kHz, so a loop that narrowed
	its request as it approached the cap would stall on the last read of a long
	.opus rather than finishing it. The overshoot is trimmed afterwards.
*/
template< typename ReadChunk >
void Drain( Decoded& d, std::uint64_t capFrames, ReadChunk read )
{
	constexpr std::uint64_t kChunkFrames = 16384; //341 ms at 48 kHz

	if( d.sourceChannels <= 0 || d.channels <= 0 || capFrames == 0 )
		return;

	std::vector< std::int16_t > scratch( static_cast< std::size_t >( kChunkFrames )
	                                     * static_cast< std::size_t >( d.sourceChannels ) );

	if( d.sourceFrames > 0 )
	{
		//One reserve rather than twenty doublings. An eight minute stereo file
		//at 96 kHz is 184 MB, and reallocating that on the way up means holding
		//276 MB at the moment of the copy.
		const std::uint64_t expect = std::min( d.sourceFrames, capFrames );
		d.pcm.reserve( static_cast< std::size_t >( expect ) * static_cast< std::size_t >( d.channels ) );
	}

	while( d.frames < capFrames )
	{
		const std::uint64_t got = read( scratch.data(), kChunkFrames );
		if( got == 0 )
			break;

		const std::size_t base = d.pcm.size();
		d.pcm.resize( base + static_cast< std::size_t >( got ) * static_cast< std::size_t >( d.channels ) );

		if( d.sourceChannels == d.channels )
		{
			std::memcpy( d.pcm.data() + base, scratch.data(),
			             static_cast< std::size_t >( got ) * static_cast< std::size_t >( d.channels )
			                 * sizeof( std::int16_t ) );
		}
		else
		{
			for( std::uint64_t f = 0; f < got; ++f )
				for( int c = 0; c < d.channels; ++c )
					d.pcm[ base + static_cast< std::size_t >( f ) * static_cast< std::size_t >( d.channels )
					       + static_cast< std::size_t >( c ) ] =
						scratch[ static_cast< std::size_t >( f ) * static_cast< std::size_t >( d.sourceChannels )
						         + static_cast< std::size_t >( c ) ];
		}

		d.frames += got;
	}

	//Truncation is established by asking for more, not by trusting a reported
	//length: three of the five decoders report nothing for a stream they cannot
	//seek, and a note that says "truncated" about a file that was not is worse
	//than a note that says nothing.
	if( d.frames > capFrames )
	{
		d.truncated = true;
	}
	else if( d.frames == capFrames && read( scratch.data(), kChunkFrames ) > 0 )
	{
		d.truncated = true;
	}

	if( d.frames > capFrames )
	{
		d.frames = capFrames;
		d.pcm.resize( static_cast< std::size_t >( capFrames ) * static_cast< std::size_t >( d.channels ) );
	}
}

bool DecodeWav( const std::string& path, Decoded& d )
{
	drwav wav;
	if( !drwav_init_file( &wav, path.c_str(), nullptr ) )
		return false;

	d.rate           = static_cast< double >( wav.sampleRate );
	d.sourceChannels = static_cast< int >( wav.channels );
	d.channels       = std::min( d.sourceChannels, kMaxKeptChannels );
	d.sourceFrames   = wav.totalPCMFrameCount;

	if( d.rate > 0.0 )
	{
		Drain( d, CapFramesFor( d.rate ), [ & ]( std::int16_t* dst, std::uint64_t want ) {
			return static_cast< std::uint64_t >( drwav_read_pcm_frames_s16( &wav, want, dst ) );
		} );
	}

	drwav_uninit( &wav );
	return d.frames > 0;
}

bool DecodeFlac( const std::string& path, Decoded& d )
{
	drflac* flac = drflac_open_file( path.c_str(), nullptr );
	if( flac == nullptr )
		return false;

	d.rate           = static_cast< double >( flac->sampleRate );
	d.sourceChannels = static_cast< int >( flac->channels );
	d.channels       = std::min( d.sourceChannels, kMaxKeptChannels );
	d.sourceFrames   = flac->totalPCMFrameCount;

	if( d.rate > 0.0 )
	{
		Drain( d, CapFramesFor( d.rate ), [ & ]( std::int16_t* dst, std::uint64_t want ) {
			return static_cast< std::uint64_t >( drflac_read_pcm_frames_s16( flac, want, dst ) );
		} );
	}

	drflac_close( flac );
	return d.frames > 0;
}

bool DecodeMp3( const std::string& path, Decoded& d )
{
	drmp3 mp3;
	if( !drmp3_init_file( &mp3, path.c_str(), nullptr ) )
		return false;

	d.rate           = static_cast< double >( mp3.sampleRate );
	d.sourceChannels = static_cast< int >( mp3.channels );
	d.channels       = std::min( d.sourceChannels, kMaxKeptChannels );
	//Set at init from the Xing/VBRI header or a frame scan, and left at
	//UINT64_MAX when neither was usable. Never call drmp3_get_pcm_frame_count()
	//to fill the gap -- it decodes the entire file to count, which doubles the
	//load time of the one format most likely to be an hour long.
	d.sourceFrames = mp3.totalPCMFrameCount == DRMP3_UINT64_MAX ? 0 : mp3.totalPCMFrameCount;

	if( d.rate > 0.0 )
	{
		Drain( d, CapFramesFor( d.rate ), [ & ]( std::int16_t* dst, std::uint64_t want ) {
			return static_cast< std::uint64_t >( drmp3_read_pcm_frames_s16( &mp3, want, dst ) );
		} );
	}

	drmp3_uninit( &mp3 );
	return d.frames > 0;
}

bool DecodeVorbis( const std::string& path, Decoded& d )
{
	int error      = 0;
	stb_vorbis* ogg = stb_vorbis_open_filename( path.c_str(), &error, nullptr );
	if( ogg == nullptr )
		return false;

	const stb_vorbis_info info = stb_vorbis_get_info( ogg );

	d.rate           = static_cast< double >( info.sample_rate );
	d.sourceChannels = info.channels;
	d.channels       = std::min( d.sourceChannels, kMaxKeptChannels );
	d.sourceFrames   = stb_vorbis_stream_length_in_samples( ogg );

	if( d.rate > 0.0 && d.sourceChannels > 0 )
	{
		const int stride = d.sourceChannels;
		Drain( d, CapFramesFor( d.rate ), [ & ]( std::int16_t* dst, std::uint64_t want ) {
			const int got = stb_vorbis_get_samples_short_interleaved(
				ogg, stride, dst, static_cast< int >( want ) * stride );
			return got > 0 ? static_cast< std::uint64_t >( got ) : std::uint64_t( 0 );
		} );
	}

	stb_vorbis_close( ogg );
	return d.frames > 0;
}

#if VECTRIX_WITH_OPUS
bool DecodeOpus( const std::string& path, Decoded& d )
{
	int error       = 0;
	OggOpusFile* of = op_open_file( path.c_str(), &error );
	if( of == nullptr )
		return false;

	//Opus has no other rate. The format's internal rate is 48 kHz whatever the
	//encoder was handed, and opusfile decodes to that; `input_sample_rate` in
	//the header is a note about the original and not something to resample to.
	d.rate = 48000.0;

	//op_read_stereo is the downmixing reader, so a multichannel .opus arrives
	//as two channels and the channel-3-drives-Z path below never applies to
	//one. That is deliberate: op_read would need channel-family handling for a
	//combination -- surround Opus as oscilloscope music -- that does not exist.
	d.sourceChannels = 2;
	d.channels       = 2;

	const ogg_int64_t total = op_pcm_total( of, -1 );
	d.sourceFrames          = total > 0 ? static_cast< std::uint64_t >( total ) : 0;

	Drain( d, CapFramesFor( d.rate ), [ & ]( std::int16_t* dst, std::uint64_t want ) {
		//Negative is an error, not a short read: stop rather than loop on it.
		const int got = op_read_stereo( of, dst, static_cast< int >( want ) * 2 );
		return got > 0 ? static_cast< std::uint64_t >( got ) : std::uint64_t( 0 );
	} );

	op_free( of );
	return d.frames > 0;
}
#else
bool DecodeOpus( const std::string&, Decoded& )
{
	return false;
}
#endif

bool DecodeWith( Codec codec, const std::string& path, Decoded& d )
{
	switch( codec )
	{
		case Codec::Wav:
			return DecodeWav( path, d );
		case Codec::Flac:
			return DecodeFlac( path, d );
		case Codec::Vorbis:
			return DecodeVorbis( path, d );
		case Codec::Opus:
			return DecodeOpus( path, d );
		case Codec::Mp3:
			return DecodeMp3( path, d );
		case Codec::Count:
		default:
			return false;
	}
}

Decoded Decode( const std::string& path )
{
	Decoded d;
	const std::string name = Basename( path );

	//Opened and closed before anything else, so that the commonest failure --
	//a composition saved on another machine, whose media is not on this one --
	//says so, instead of coming out the far end of five decoders as "nothing
	//would take it".
	std::FILE* probe = std::fopen( path.c_str(), "rb" );
	if( probe == nullptr )
	{
		d.note = "could not read '" + path + "'";
		return d;
	}
	std::fclose( probe );

	//mp3 is last and is never the first thing tried on an unknown extension.
	//dr_mp3 will find a plausible frame header in almost any byte stream and
	//hand back minutes of noise rather than failing, so as a speculative
	//candidate it does not reject anything -- it just wins.
	static const Codec kOrder[] = { Codec::Wav, Codec::Flac, Codec::Vorbis, Codec::Opus, Codec::Mp3 };

	const Codec preferred = CodecForExtension( Extension( path ) );

	bool ok = false;
	if( preferred != Codec::Count )
	{
		d  = Decoded();
		ok = DecodeWith( preferred, path, d );
	}

	if( !ok )
	{
		for( const Codec codec : kOrder )
		{
			if( codec == preferred )
				continue;

			d = Decoded();
			if( DecodeWith( codec, path, d ) )
			{
				ok = true;
				break;
			}
		}
	}

	if( !ok )
	{
		d      = Decoded();
		d.note = name + ": no decoder would take it (tried wav/aiff, flac, ogg"
#if VECTRIX_WITH_OPUS
		         ", opus"
#endif
		         ", mp3)";
		return d;
	}

	if( d.frames < kMinFrames || d.channels <= 0 || !( d.rate > 0.0 ) )
	{
		const std::uint64_t got = d.frames;
		d                       = Decoded();
		d.note = name + ": " + std::to_string( got ) + " frames is too short to interpolate";
		return d;
	}

	const double seconds = static_cast< double >( d.frames ) / d.rate;

	d.note = name + ": " + TimeText( seconds ) + ", " + std::to_string( d.sourceChannels ) + " ch, "
	         + std::to_string( static_cast< long long >( d.rate + 0.5 ) ) + " Hz";

	if( d.truncated )
	{
		d.note += " (truncated";
		if( d.sourceFrames > static_cast< std::uint64_t >( d.frames ) )
			d.note += " from " + TimeText( static_cast< double >( d.sourceFrames ) / d.rate );
		d.note += " -- the " + std::to_string( static_cast< int >( kMaxAudioSeconds / 60.0 ) )
		          + " minute cap)";
	}

	if( d.sourceChannels == 1 )
		d.note += " -- mono, Y comes from the Mono Mode control";
	else if( d.sourceChannels == 2 )
		d.note += " -- L to X, R to Y";
	else if( d.sourceChannels == kMaxKeptChannels )
		d.note += " -- L to X, R to Y, channel 3 to Z";
	else
		d.note += " -- L to X, R to Y, channel 3 to Z, channels 4+ dropped";

	return d;
}
} // namespace

//---------------------------------------------------------------------------
// The source
//---------------------------------------------------------------------------

void AudioFile::Prepare( double sampleRate )
{
	fs = sampleRate > 0.0 ? sampleRate : 96000.0;

	//At least one sample, so a Draft-rate engine still cuts the gun across a
	//loop join rather than drawing it.
	blankLength = std::max( 1, static_cast< int >( fs * kBlankSeconds + 0.5 ) );

	Reset();
}

void AudioFile::Reset()
{
	std::size_t first = 0;
	std::size_t span  = 0;
	section( first, span );

	readPos      = static_cast< double >( first );
	blankSamples = 0;
	finished     = false;
}

void AudioFile::SetParams( const AudioFileParams& p )
{
	//The one change that is more than a copy. With Loop off the position parks
	//at the end of the section with the gun cut, and turning Loop back on has
	//to mean "go again" -- otherwise the control appears dead, because the only
	//visible effect of enabling it is nothing at all.
	if( p.loop && !params.loop )
		finished = false;

	params = p;
}

void AudioFile::SetPath( std::string value )
{
	std::lock_guard< std::mutex > lock( pathMutex );

	//Compared rather than assigned blind. A host is entitled to re-send the
	//same string every frame, and with a decode on the other end of the flag
	//that is not a wasted memcpy, it is an eight minute FLAC decoded sixty
	//times a second. The cost is that re-choosing the same file in the dialogue
	//does not force a re-read, which is a trade worth making.
	if( value == path )
		return;

	path      = std::move( value );
	pathDirty = true;
}

std::string AudioFile::Path() const
{
	std::lock_guard< std::mutex > lock( pathMutex );
	return path;
}

bool AudioFile::Dirty() const
{
	std::lock_guard< std::mutex > lock( pathMutex );
	return pathDirty;
}

void AudioFile::SetHostSeconds( double seconds )
{
	hostSeconds = seconds;
}

void AudioFile::ReloadIfDirty()
{
	std::string wanted;
	{
		std::lock_guard< std::mutex > lock( pathMutex );
		if( !pathDirty )
			return;

		pathDirty = false;
		wanted    = path;
	}

	//Released before the next decode is started, not after. Two eight minute
	//files held at once is 368 MB, and the old one is of no use from here on.
	pcm.clear();
	pcm.shrink_to_fit();
	channels = 0;
	fileRate = 0.0;
	frames   = 0;

	if( wanted.empty() )
	{
		note = "no file chosen";
		Reset();
		return;
	}

	Decoded decoded = Decode( wanted );
	note            = decoded.note;

	if( decoded.frames >= kMinFrames && decoded.channels > 0 && decoded.rate > 0.0 )
	{
		pcm      = std::move( decoded.pcm );
		channels = decoded.channels;
		fileRate = decoded.rate;
		frames   = static_cast< std::size_t >( decoded.frames );
		diag::info( "audio file loaded: " + note );
	}
	else
	{
		diag::error( "audio file not loaded: " + note );
	}

	Reset();
}

void AudioFile::section( std::size_t& first, std::size_t& span ) const
{
	first = 0;
	span  = 0;
	if( frames < kMinFrames )
		return;

	double a = static_cast< double >( std::clamp( params.start, 0.0f, 1.0f ) );
	double b = static_cast< double >( std::clamp( params.end, 0.0f, 1.0f ) );

	//Dragging End below Start shrinks the section from the other side rather
	//than emptying it. An operator who has crossed the two handles over is
	//still describing a region, and refusing to play anything is a worse
	//answer than playing the region they drew.
	if( b < a )
		std::swap( a, b );

	const double total = static_cast< double >( frames );
	std::size_t from   = static_cast< std::size_t >( a * total );
	std::size_t to     = static_cast< std::size_t >( b * total );
	from               = std::min( from, frames );
	to                 = std::min( to, frames );

	//Four frames is the interpolator's tap count and therefore the floor. A
	//section that short is a stationary buzzing dot, which is exactly what
	//asking for it looks like -- it is not clamped to something more tasteful,
	//because a control that silently disagrees with its own readout is worse.
	if( to < from + kMinFrames )
	{
		if( from + kMinFrames <= frames )
			to = from + kMinFrames;
		else
		{
			from = frames - kMinFrames;
			to   = frames;
		}
	}

	first = from;
	span  = to - from;
}

float AudioFile::tap( std::ptrdiff_t index, std::size_t first, std::ptrdiff_t span, int channel ) const
{
	//Wrapped, not clamped. At the loop join the four taps straddle the seam,
	//and clamping there would repeat the end sample -- a few microseconds of DC
	//at exactly the moment the beam is being asked to jump, which reads as the
	//figure sticking rather than looping.
	index %= span;
	if( index < 0 )
		index += span;

	const std::size_t frame = first + static_cast< std::size_t >( index );
	return static_cast< float >(
	           pcm[ frame * static_cast< std::size_t >( channels ) + static_cast< std::size_t >( channel ) ] )
	       * kInt16Scale;
}

float AudioFile::sampleAt( double relative, std::size_t first, std::ptrdiff_t span, int channel,
                           float* slopeOut ) const
{
	const double length = static_cast< double >( span );

	double r = std::fmod( relative, length );
	if( r < 0.0 )
		r += length;

	const double base    = std::floor( r );
	const float t        = static_cast< float >( r - base );
	const std::ptrdiff_t i = static_cast< std::ptrdiff_t >( base );

	const float xm1 = tap( i - 1, first, span, channel );
	const float x0  = tap( i, first, span, channel );
	const float x1  = tap( i + 1, first, span, channel );
	const float x2  = tap( i + 2, first, span, channel );

	//Cubic Hermite, Catmull-Rom tangents. Linear would be cheaper and would
	//show: between two samples it draws a chord across a curve, so every arc in
	//the figure gains a faint polygonal flat, and because the error depends on
	//where the fractional position falls it behaves as a low-pass whose corner
	//moves with the Rate control. A figure that softens when you slow it down
	//is the single most confusing artefact an interpolator can produce.
	const float c0 = x0;
	const float c1 = 0.5f * ( x1 - xm1 );
	const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
	const float c3 = 0.5f * ( x2 - xm1 ) + 1.5f * ( x0 - x1 );

	//The slope comes from the same polynomial the value came from, so Y in
	//Derivative mode is the exact derivative of the X actually drawn rather
	//than a finite difference of a different curve. It is also free: the
	//coefficients are already here.
	if( slopeOut != nullptr )
		*slopeOut = ( 3.0f * c3 * t + 2.0f * c2 ) * t + c1;

	return ( ( c3 * t + c2 ) * t + c1 ) * t + c0;
}

void AudioFile::Render( Sample* out, int n, double dtPerSample )
{
	const float dt = static_cast< float >( dtPerSample );

	std::size_t first     = 0;
	std::size_t spanCount = 0;
	section( first, spanCount );

	//The span test is not redundant with Loaded(). It is the one thing standing
	//between `tap` and a modulo by zero, and it holds even if some future edit
	//lets a three-frame file through the loader.
	if( !Loaded() || spanCount < kMinFrames )
	{
		//Nothing loaded is the ordinary state before the operator picks a file,
		//and it has to look like a machine that is switched on with nothing
		//patched into it: beam at the centre, gun cut. A lit dot parked at the
		//origin would be a bright point in the middle of the tube that reads as
		//a fault, and leaving the block untouched would draw whatever the last
		//source left in it.
		for( int i = 0; i < n; ++i )
		{
			out[ i ].x  = 0.0f;
			out[ i ].y  = 0.0f;
			out[ i ].z  = 0.0f;
			out[ i ].dt = dt;
		}
		return;
	}

	const std::ptrdiff_t span = static_cast< std::ptrdiff_t >( spanCount );
	const double length       = static_cast< double >( spanCount );

	const double rate = static_cast< double >( std::clamp( params.rate, 0.05f, 4.0f ) );

	//No anti-imaging filter above 1x, deliberately. Reading faster than the
	//file was written folds the images above Nyquist back into the band, and on
	//an X/Y display that arrives as a faint mirror of the figure rather than as
	//hiss -- which is honest, free, and rather good looking. A filter that
	//removed it would also round off precisely the corners the cubic was chosen
	//to keep.
	const double inc = ( fileRate / fs ) * rate;

	double rel = std::fmod( readPos - static_cast< double >( first ), length );
	if( rel < 0.0 )
		rel += length;

	if( params.sync == AudioSync::Locked )
	{
		//Locked implies looping: the host's timeline is modular and the Loop
		//control has nothing to say about it.
		//
		//This is `t * rate`, which is the form the house rule forbids for the
		//oscillator's phase -- and here it is the correct one. Integrating in
		//double is right when the requirement is that the figure not drift over
		//an hour; this requirement is the opposite, that the position be a pure
		//function of the host's transport so that dragging the playhead lands
		//on the same frame of the file every time. An integrator would answer
		//"wherever I have got to", which is not an answer to a scrub.
		//
		//The position is snapped at the block boundary and then free-runs
		//through the block. Recomputing it per sample would be worse, not
		//better: `hostSeconds` only changes once a frame, so a per-sample
		//recompute would hold the beam still for the whole block and then teleport.
		double target = std::fmod( hostSeconds * rate * fileRate, length );
		if( target < 0.0 )
			target += length;

		//Blank only for a real jump. The host's clock and ours agree to within
		//a few samples in normal running, and blanking on every block boundary
		//would strobe the beam at the frame rate -- a visible flicker with no
		//control an operator could trace it to.
		if( std::fabs( target - rel ) > fileRate * kBlankSeconds )
			blankSamples = blankLength;

		rel      = target;
		finished = false;
	}

	const double delayFrames =
		static_cast< double >( std::clamp( params.monoDelayMs, kMonoDelayMinMs, kMonoDelayMaxMs ) ) * 0.001
		* fileRate;

	const float derivativeScale =
		static_cast< float >( 1.0 / ( kTwoPi * static_cast< double >( kDerivativeReferenceHz ) ) );

	for( int i = 0; i < n; ++i )
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 1.0f;

		if( finished )
		{
			//Played out, with Loop off. Centre and cut, not a parked lit dot:
			//the renderer deposits energy for as long as the beam sits there,
			//so a stationary bright sample is a burn.
			z = 0.0f;
		}
		else
		{
			float slope = 0.0f;
			x           = sampleAt( rel, first, span, 0, &slope );

			if( channels >= 2 )
			{
				y = sampleAt( rel, first, span, 1, nullptr );
			}
			else
			{
				switch( params.monoMode )
				{
					case MonoMode::XOnly:
						//A horizontal line. Correct, and the reason it is not
						//the default.
						y = 0.0f;
						break;

					case MonoMode::Derivative:
						//The phase plane, (x, dx/dt). `slope` is dx per file
						//frame, so the file rate converts it to dx/dt.
						y = std::clamp( slope * static_cast< float >( fileRate ) * derivativeScale,
						                -kDerivativeCeiling, kDerivativeCeiling );
						break;

					case MonoMode::Delayed:
					case MonoMode::Count:
					default:
						//(x(t), x(t-tau)): the delay embedding, and the reason
						//it is the default. It is not decoration -- it is what
						//turns a one-dimensional time series into a trajectory,
						//so a mono file draws a real attractor whose shape
						//tracks the material instead of a flat line. Tau
						//matters: much below a millisecond x(t-tau) is nearly
						//x(t) and the figure collapses onto the diagonal, which
						//is why the floor is 1 ms and the default is 5.
						y = sampleAt( rel - delayFrames, first, span, 0, nullptr );
						break;
				}
			}

			if( channels >= kMaxKeptChannels )
			{
				//Channel 3 as the Z axis, mapped bipolar to unipolar. It came
				//out of the same DAW at the same scaling as the other two, so
				//it is treated as the audio signal it is: a hard-clipped square
				//on that channel is a clean on/off gate, a sine is a smooth
				//fade, and -- the part that matters -- a *silent* third channel
				//lands at half brightness rather than black. A file that
				//happens to carry an unused third channel still draws.
				z = std::clamp( 0.5f + 0.5f * sampleAt( rel, first, span, 2, nullptr ), 0.0f, 1.0f );
			}
		}

		//-------------------------------------------------------------------
		// Axis mapping. Swap first, then invert, because the two controls are
		// read in that order: Swap decides which channel is on which axis, and
		// Invert Y then negates whatever is on Y. An operator who ticks both
		// expects "Invert Y" to invert the thing labelled Y, not the right
		// channel.
		//-------------------------------------------------------------------
		if( params.swapXY )
			std::swap( x, y );

		//-------------------------------------------------------------------
		// Invert Y, and why it defaults to on.
		//
		// This negates the Y axis in the **signal** domain, where Signal.h is
		// explicit that +Y is up and that the flip into the renderer's raster
		// happens in exactly one place, the tube pass. So this is emphatically
		// not that flip, and it must never be described as one. A second raster
		// flip here would be precisely the bug Signal.h's last paragraph exists
		// to warn about, and it would be a worse version of it: it would apply
		// to audio files alone, leaving the oscillator and the shapes the other
		// way up, so the two would disagree and neither would look wrong on its
		// own.
		//
		// What it is instead is a statement about the *file*. A stereo pair is
		// two voltages and nothing in any audio format says which way up the
		// right channel is meant to be read. On a real scope in X-Y mode a
		// positive right channel deflects the beam up, so a piece checked on
		// hardware wants this **off**. Almost nothing is checked on hardware.
		// The repertoire is authored and previewed in software scopes that draw
		// into a screen-space raster with +Y down -- so the composer who drew a
		// letter "A" the right way up saw it the right way up there, and the
		// samples they shipped have the top of the "A" at *negative* right
		// channel. Fed into a Y-up signal domain unchanged, those samples come
		// out upside down, having already been flipped exactly once, in a tool
		// that is not ours.
		//
		// Hence: on. It is the setting that draws the majority of the
		// repertoire the way it was drawn, and the failure it prevents is both
		// the likeliest bug report and the hardest to see in a screenshot,
		// because an upside-down Lissajous is still a Lissajous. There is no
		// way to tell the two conventions apart from the bytes, which is why
		// this is a control and not a constant.
		//-------------------------------------------------------------------
		if( params.invertY )
			y = -y;

		//-------------------------------------------------------------------
		// Advance, and decide about the wrap *before* z is written.
		//
		// The renderer deposits energy for the interval that *starts* at this
		// sample, so the sample that must be dark is the last one before the
		// jump, not the first one after it. Setting the blank counter here and
		// applying it below is what puts the cut on the right side of the seam.
		//
		// A cut and not a crossfade. Crossfading two positions of an X/Y path
		// mixes two points into one, and the beam draws the straight line
		// between them -- a bright chord across the middle of the figure, once
		// per loop, which is far more conspicuous than the jump it was meant to
		// hide. A real machine mutes; so does this.
		//-------------------------------------------------------------------
		if( !finished )
		{
			const double next = rel + inc;
			if( next >= length )
			{
				if( params.loop || params.sync == AudioSync::Locked )
				{
					rel          = std::fmod( next, length );
					blankSamples = blankLength;
				}
				else
				{
					//Park just inside the end, so that switching Loop back on
					//resumes by wrapping to the top rather than from wherever
					//the position happened to overshoot to.
					rel          = length - 1.0;
					finished     = true;
					blankSamples = blankLength;
				}
			}
			else
			{
				rel = next;
			}
		}

		if( blankSamples > 0 )
		{
			z = 0.0f;
			--blankSamples;
		}

		out[ i ].x  = x;
		out[ i ].y  = y;
		out[ i ].z  = z;
		out[ i ].dt = dt;
	}

	readPos = static_cast< double >( first ) + rel;
}

} // namespace vectrix
