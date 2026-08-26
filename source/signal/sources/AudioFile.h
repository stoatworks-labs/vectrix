#pragma once

#include "signal/sources/Source.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace vectrix
{
/// Extensions offered by the file parameter, in the shape `SetFileParamInfo`
/// wants them: bare, lower case, no leading dot. `.aif` is listed next to
/// `.aiff` because both are in circulation and a file dialogue that filters one
/// out is indistinguishable, from the operator's side, from a plugin that
/// cannot read AIFF at all.
///
/// **A complete `std::array` in the header, deliberately.** It was an `extern
/// const char* const[]` with a separate count, and the incomplete type meant a
/// caller could not size it -- so the one caller walked it for a null
/// terminator the array has never had. That runs off the end into whatever the
/// linker put next: zero on macOS, and on MSVC the count itself, read back as a
/// `const char*` of 0x7. Range-for this and the mistake is unavailable.
inline constexpr const char* const kAudioExtensions[] = {
	"wav", "aiff", "aif", "mp3", "flac", "ogg",
#if VECTRIX_WITH_OPUS
	"opus",
#endif
};

inline constexpr int kAudioExtensionCount = static_cast< int >( std::size( kAudioExtensions ) );

/// The most audio the source will hold. Eight minutes covers every piece of
/// oscilloscope music anybody has actually written and costs 184 MB at 96 kHz
/// stereo, which is the number that set the figure. A longer file is
/// **truncated and played**, never refused -- the operator dropped in an album
/// side to see what the first movement looks like, and an error message that
/// draws nothing answers a question nobody asked.
constexpr double kMaxAudioSeconds = 8.0 * 60.0;

/**
	What to put on Y when the file has only one channel.

	The order is load-bearing twice over. `Delayed` is first so that the
	zero-initialised value, the first entry of the dropdown and the documented
	default are the same thing -- an option parameter whose default is declared
	somewhere else is a default that drifts the first time somebody reorders the
	names. And `Controls.h` hard-codes `kMonoModeCount = 3` rather than including
	this header, so adding a mode here means changing that line too.
*/
enum class MonoMode
{
	Delayed,    ///< Y is X, delayed. A self-delay plot: a genuine attractor.
	XOnly,      ///< Y is 0. A horizontal line. Correct, honest, and dull.
	Derivative, ///< Y is dx/dt. The phase plane of the signal.
	Count
};

/// Where the read position comes from.
enum class AudioSync
{
	Free,   ///< Free-running from Reset. The default.
	Locked, ///< Pinned to the host's transport, so scrubbing the composition scrubs the file.
	Count
};

struct AudioFileParams
{
	float rate  = 1.0f;  ///< 0.05 .. 4.0 of the file's own rate
	bool loop   = true;
	float start = 0.0f;  ///< 0..1 of the file
	float end   = 1.0f;  ///< 0..1 of the file
	AudioSync sync = AudioSync::Free;

	MonoMode monoMode = MonoMode::Delayed;
	float monoDelayMs = 5.0f; ///< clamped to 1..20 ms; not exposed as a control

	bool swapXY  = false;

	/// **On by default, and this is the single most consequential default in the
	/// file.** See the long note in `AudioFile.cpp` before changing it -- and in
	/// particular, do not "fix" an upside-down figure by touching the tube pass,
	/// because that flip belongs to every source at once and this one does not.
	bool invertY = true;
};

/**
	A decoded audio file, driving the beam directly: left to X, right to Y.

	This is the source that makes oscilloscope music work, and it does almost
	nothing -- which is the point. The figure is whatever the composer drew in
	the stereo field, and every step between the file and the deflection
	amplifier is a chance to draw something they did not.

	## Decoded once, in full, as interleaved int16 at the file's own rate

	Three decisions, each of which has an obvious-looking alternative:

	**int16, not float.** Float doubles the memory to buy 96 dB of headroom that
	nothing here can show. At int16 the beam quantises to 1/32768 of full
	deflection, which is 0.07 px on a 4K face -- a fifteenth of a pixel, under
	the width of the beam itself, and well under the phosphor pass's own spread.

	**Native rate, not resampled at load.** The Rate control means the
	interpolator runs on every sample regardless, so a load-time resample buys no
	work back; it merely adds a second interpolation, and it would have to be
	redone from the file every time Rate moved.

	**All of it, up front.** Streaming from disk inside a render callback is how
	a plugin acquires a dropout that only happens on the machine at the venue.

	## Loading never happens inside Render

	`SetPath` is called from whatever thread the host feels like -- it stores the
	string behind a mutex and sets a flag. `ReloadIfDirty` does the decode and
	must be called at a block boundary, before `Render`. Nothing else in the
	class locks, because the decoded data is only ever touched by those two, and
	both run on the render thread.
*/
class AudioFile : public Source
{
public:
	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Render( Sample* out, int n, double dtPerSample ) override;

	void SetParams( const AudioFileParams& p );

	//-- Any thread ----------------------------------------------------------

	/// Store the path and mark it for reload. Does no work: `value` arrives from
	/// the host's UI thread and a decode there would block the interface for as
	/// long as an eight minute FLAC takes.
	void SetPath( std::string value );

	/// What `SetPath` was last given.
	std::string Path() const;

	//-- Render thread, at a block boundary only ------------------------------

	/// True if the path has changed since the last `ReloadIfDirty`.
	bool Dirty() const;

	/// Decode, if the path changed. **Never call this from inside `Render`.**
	void ReloadIfDirty();

	/// The host's transport position, in seconds, for `AudioSync::Locked`.
	/// Handed in rather than read, so `Render` still never touches a clock and
	/// the offline harness stays bit-identical with the host.
	void SetHostSeconds( double seconds );

	//-- What got loaded -----------------------------------------------------

	bool Loaded() const
	{
		return frames > 0 && channels > 0 && fileRate > 0.0;
	}

	/// One line for the log and the harness: what was loaded, what was dropped,
	/// or why nothing was. Always populated, never empty.
	const std::string& Note() const
	{
		return note;
	}

	/// Seconds held in memory -- so, after the eight minute cap, not necessarily
	/// the duration of the file on disk.
	double Duration() const
	{
		return fileRate > 0.0 ? static_cast< double >( frames ) / fileRate : 0.0;
	}

	double FileRate() const
	{
		return fileRate;
	}

	/// Channels *kept*, which is at most three: X, Y and the Z blanking channel.
	int Channels() const
	{
		return channels;
	}

private:
	/// The playable window, in file frames, from the Start and End controls.
	void section( std::size_t& first, std::size_t& span ) const;

	/// One stored sample as +/-1, at `index` frames into the section, wrapped.
	float tap( std::ptrdiff_t index, std::size_t first, std::ptrdiff_t span, int channel ) const;

	/// Catmull-Rom at `relative` frames into the section. When `slopeOut` is
	/// given it receives dx per *file frame*, taken from the same cubic, which
	/// is both free and consistent with the value returned.
	float sampleAt( double relative, std::size_t first, std::ptrdiff_t span, int channel,
	                float* slopeOut ) const;

	AudioFileParams params;

	//-- The decoded file. Render thread only, no lock. ----------------------
	std::vector< std::int16_t > pcm; ///< interleaved, `channels` per frame
	int channels        = 0;
	double fileRate     = 0.0;
	std::size_t frames  = 0;
	std::string note    = "no file chosen";

	//-- Playback state ------------------------------------------------------
	double fs          = 96000.0;
	double readPos     = 0.0; ///< absolute, in file frames
	double hostSeconds = 0.0;
	int blankSamples   = 0;   ///< engine samples of gun-off remaining
	int blankLength    = 96;  ///< 1 ms at the engine rate
	bool finished      = false;

	//-- The path, which the host owns and we do not ------------------------
	mutable std::mutex pathMutex;
	std::string path;
	bool pathDirty = false;
};

} // namespace vectrix
