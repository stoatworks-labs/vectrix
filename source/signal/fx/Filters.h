#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

/**
	Every coefficient formula in the plugin lives in this file.

	Not a style preference. A coefficient written out a second time in another
	block is a coefficient that will be *corrected* in one place and not the
	other, and the resulting disagreement between, say, the delay's damping
	filter and the reverb's shows up as "the reverb sounds different from the
	delay", which nobody traces back to a duplicated `tan`.
*/
namespace vectrix
{

/// Denormals cost hundreds of cycles on x86 and arrive precisely when a reverb
/// tail decays below ~1e-38 -- so the symptom is a frame-rate collapse that
/// starts *after* the signal stops, which reads as a leak rather than as an FPU
/// stall. Every feedback path flushes through this.
inline float flush( float x )
{
	return std::fabs( x ) < 1.0e-25f ? 0.0f : x;
}

/// One-pole low pass. `Set` takes a cutoff in Hz.
class OnePole
{
public:
	void Prepare( double sampleRate )
	{
		fs = sampleRate;
		Reset();
	}
	void Reset()
	{
		state = 0.0f;
	}

	void SetCutoff( double hz )
	{
		const double clamped = std::clamp( hz, 1.0, fs * 0.49 );
		coeff = 1.0f - static_cast< float >( std::exp( -6.283185307179586 * clamped / fs ) );
	}

	float Low( float x )
	{
		state = flush( state + ( x - state ) * coeff );
		return state;
	}
	float High( float x )
	{
		return x - Low( x );
	}

private:
	double fs   = 96000.0;
	float coeff = 1.0f;
	float state = 0.0f;
};

/**
	A topology-preserving-transform state variable filter (Zavalishin).

	TPT rather than the classic Chamberlin form because the Chamberlin one goes
	unstable as the cutoff approaches a quarter of the sample rate, and the
	deflection amplifier's bandwidth control wants to run right up there.
*/
class Svf
{
public:
	void Prepare( double sampleRate )
	{
		fs = sampleRate;
		Reset();
	}
	void Reset()
	{
		ic1 = ic2 = 0.0f;
	}

	void Set( double cutoffHz, double q )
	{
		const double clamped = std::clamp( cutoffHz, 1.0, fs * 0.49 );
		g                    = static_cast< float >( std::tan( 3.141592653589793 * clamped / fs ) );
		k                    = static_cast< float >( 1.0 / std::max( 0.05, q ) );
		a1                   = 1.0f / ( 1.0f + g * ( g + k ) );
	}

	/// Low-pass output. The band and high outputs are available from the same
	/// state if a block ever needs them; nothing does yet, so they are not here.
	float Low( float x )
	{
		const float v3 = x - ic2;
		const float v1 = a1 * ( ic1 + g * v3 );
		const float v2 = ic2 + g * v1;
		ic1            = flush( 2.0f * v1 - ic1 );
		ic2            = flush( 2.0f * v2 - ic2 );
		return v2;
	}

private:
	double fs = 96000.0;
	float g = 0.0f, k = 1.0f, a1 = 1.0f;
	float ic1 = 0.0f, ic2 = 0.0f;
};

/// A first-order allpass, the phaser's building block. Unity magnitude at every
/// frequency; all it does is delay phase, which on a deflection axis is a
/// rotation of the figure rather than a colour of the sound.
class Allpass1
{
public:
	void Reset()
	{
		x1 = y1 = 0.0f;
	}

	/// `a` is the coefficient, computed by `allpassCoeff` below.
	float Process( float x, float a )
	{
		const float y = a * x + x1 - a * y1;
		x1            = x;
		y1            = flush( y );
		return y1;
	}

private:
	float x1 = 0.0f, y1 = 0.0f;
};

/// The coefficient placing a first-order allpass's 90-degree point at `hz`.
inline float allpassCoeff( double hz, double fs )
{
	const double t = std::tan( 3.141592653589793 * std::clamp( hz, 1.0, fs * 0.49 ) / fs );
	return static_cast< float >( ( 1.0 - t ) / ( 1.0 + t ) );
}

/// A fixed-gain Schroeder allpass, used as the reverb's input diffuser.
class AllpassDelay
{
public:
	void Prepare( std::size_t length )
	{
		buffer.assign( std::max< std::size_t >( length, 1 ), 0.0f );
		index = 0;
	}
	void Reset()
	{
		std::fill( buffer.begin(), buffer.end(), 0.0f );
	}

	float Process( float x, float g )
	{
		const float delayed = buffer[ index ];
		const float v       = x + g * delayed;
		buffer[ index ]     = flush( v );
		index               = ( index + 1 ) % buffer.size();
		return delayed - g * v;
	}

private:
	std::vector< float > buffer;
	std::size_t index = 0;
};

/**
	A power-of-two delay line with a fractional cubic read.

	Mask-indexed rather than modulo: the wrap is a bitwise AND, which matters
	when it happens twice per sample per axis.

	The read is **cubic Hermite** and not linear on purpose. Linear interpolation
	of a fractional delay is a low-pass whose cutoff depends on the fractional
	part, so a modulated delay -- which is every chorus and flanger here -- gets a
	brightness that wobbles in step with the modulation. On a picture that is a
	figure whose sharpness breathes, which looks like a fault in the focus.
	Thiran allpass interpolation is the other good answer and is rejected because
	it carries state and rings when the delay time moves fast.
*/
class DelayLine
{
public:
	void Prepare( std::size_t powerOfTwoLength )
	{
		std::size_t size = 2;
		while( size < powerOfTwoLength )
			size <<= 1;
		buffer.assign( size, 0.0f );
		mask  = size - 1;
		write = 0;
	}

	void Reset()
	{
		std::fill( buffer.begin(), buffer.end(), 0.0f );
	}

	void Write( float x )
	{
		buffer[ write ] = flush( x );
		write           = ( write + 1 ) & mask;
	}

	/// Read `delay` samples back. Fractional, cubic.
	float Read( double delay ) const
	{
		const double clamped = std::clamp( delay, 1.0, static_cast< double >( mask ) - 2.0 );
		const double pos     = static_cast< double >( write ) - clamped;
		const std::size_t i  = static_cast< std::size_t >( static_cast< long long >( std::floor( pos ) ) + static_cast< long long >( buffer.size() ) * 4 ) & mask;
		const float frac     = static_cast< float >( pos - std::floor( pos ) );

		const float xm1 = buffer[ ( i - 1 ) & mask ];
		const float x0  = buffer[ i ];
		const float x1  = buffer[ ( i + 1 ) & mask ];
		const float x2  = buffer[ ( i + 2 ) & mask ];

		const float c0 = x0;
		const float c1 = 0.5f * ( x1 - xm1 );
		const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
		const float c3 = 0.5f * ( x2 - xm1 ) + 1.5f * ( x0 - x1 );

		return ( ( c3 * frac + c2 ) * frac + c1 ) * frac + c0;
	}

	std::size_t Capacity() const
	{
		return buffer.size();
	}

	/// Total absolute energy held in the line.
	///
	/// Diagnostic only -- it walks the whole buffer, so it is for the harness and
	/// never for the audio path. It exists because the obvious measure, "what did
	/// the line last read", is wrong in exactly the case that matters: a tank
	/// with a pre-delay and long lines has plenty of signal *written* into it
	/// while its read pointer is still in the silence before the strike, so the
	/// last read is zero and the tank is not empty at all.
	double Energy() const
	{
		double sum = 0.0;
		for( float v : buffer )
			sum += std::fabs( static_cast< double >( v ) );
		return sum;
	}

private:
	std::vector< float > buffer;
	std::size_t mask  = 0;
	std::size_t write = 0;
};

/**
	A blocking DC remover.

	Genuinely load-bearing and genuinely last in the chain. The rectifier and the
	wavefolder both produce a DC offset as a matter of course, and DC on a
	deflection axis is not a tonal problem -- it is a permanent shift of the whole
	figure off the centre of the screen, which the operator will try to fix with
	the Offset control and then find they cannot get back.
*/
class DcBlock
{
public:
	void Prepare( double sampleRate )
	{
		//A 5 Hz corner: low enough not to touch the slowest figure anyone will
		//draw, high enough to settle within a second of a fold being engaged.
		coeff = static_cast< float >( 1.0 - 6.283185307179586 * 5.0 / sampleRate );
		Reset();
	}
	void Reset()
	{
		x1 = y1 = 0.0f;
	}

	float Process( float x )
	{
		const float y = x - x1 + coeff * y1;
		x1            = x;
		y1            = flush( y );
		return y1;
	}

private:
	float coeff = 0.999f;
	float x1 = 0.0f, y1 = 0.0f;
};

/**
	A 2x half-band FIR, used in cascade for the wavefolder's 4x oversampling.

	**Linear phase, and that choice is specific to this plugin.** A polyphase IIR
	allpass half-band is cheaper and steeper and is what an audio effect would
	use -- but its phase response is not flat, and a frequency-dependent delay
	between the two deflection axes *is a skew of the figure*. It would add a
	phaser nobody asked for, in a plugin that has a phaser three slots later.
	Phase matters visually here in a way it does not audibly.

	15 taps, of which 7 are zero by construction in a half-band, so it costs
	about 4 multiply-accumulates per output.
*/
class HalfBand
{
public:
	void Reset()
	{
		history.fill( 0.0f );
		index = 0;
	}

	/// Push one input sample; returns the filtered value.
	float Process( float x )
	{
		history[ index ] = x;
		float sum        = 0.0f;
		for( int t = 0; t < kTaps; ++t )
		{
			if( kCoeff[ t ] == 0.0f )
				continue;
			const int h = ( index - t + kTaps * 2 ) % kTaps;
			sum += kCoeff[ t ] * history[ h ];
		}
		index = ( index + 1 ) % kTaps;
		return sum;
	}

private:
	static constexpr int kTaps = 15;
	//A windowed-sinc half-band: every even tap either side of the centre is
	//zero, the centre is 0.5, and the sum is 1.0 so a DC input passes unchanged.
	static constexpr float kCoeff[ kTaps ] = {
		-0.0060f, 0.0f, 0.0293f, 0.0f, -0.1030f, 0.0f, 0.3172f, 0.5f,
		0.3172f, 0.0f, -0.1030f, 0.0f, 0.0293f, 0.0f, -0.0060f
	};

	std::array< float, kTaps > history{};
	int index = 0;
};

} // namespace vectrix
