#pragma once

#include <cstdint>

namespace vectrix
{
/**
	xorshift128+, seeded.

	Seeded and not `std::random_device` because the offline harness has to be
	able to render the same noise twice: a test that cannot reproduce its input
	cannot tell a regression from the weather. The Seed parameter is exposed for
	the same reason at the other end -- two instances of the plugin side by side
	should be able to differ, and to differ *repeatably*.
*/
class Rng
{
public:
	explicit Rng( std::uint64_t seed = 0x9E3779B97F4A7C15ull )
	{
		Seed( seed );
	}

	void Seed( std::uint64_t seed )
	{
		//SplitMix64 to fill the state: seeding xorshift with a small integer
		//directly gives a first few hundred outputs with visible structure,
		//which on a noise waveform is a faint stationary pattern in the corner.
		state[ 0 ] = splitMix( seed );
		state[ 1 ] = splitMix( seed );
		if( state[ 0 ] == 0 && state[ 1 ] == 0 )
			state[ 0 ] = 0x9E3779B97F4A7C15ull;
	}

	std::uint64_t Next()
	{
		std::uint64_t s1       = state[ 0 ];
		const std::uint64_t s0 = state[ 1 ];
		state[ 0 ]             = s0;
		s1 ^= s1 << 23;
		state[ 1 ] = s1 ^ s0 ^ ( s1 >> 18 ) ^ ( s0 >> 5 );
		return state[ 1 ] + s0;
	}

	/// Uniform in [0, 1).
	float Unit()
	{
		//24 bits into a float's mantissa. Taking the low bits instead would use
		//the weakest bits of the generator.
		return static_cast< float >( Next() >> 40 ) * ( 1.0f / 16777216.0f );
	}

	/// Uniform in [-1, 1]. Uniform and deliberately not Gaussian: uniform noise
	/// on both axes fills a square, which is what noise on a scope looks like.
	/// Gaussian noise makes a round cloud, which looks like a different fault.
	float Bipolar()
	{
		return Unit() * 2.0f - 1.0f;
	}

private:
	static std::uint64_t splitMix( std::uint64_t& x )
	{
		x += 0x9E3779B97F4A7C15ull;
		std::uint64_t z = x;
		z               = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
		z               = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
		return z ^ ( z >> 31 );
	}

	std::uint64_t state[ 2 ] = { 0, 0 };
};

/// A stateless integer hash, for value noise where the same input must give the
/// same output every frame however the camera moved. Scrolling the mountains
/// works by moving the *sample origin* through this field rather than moving the
/// geometry, so a ridge that leaves and comes back is bit-identical.
inline float hashNoise( int x, int y, std::uint32_t seed )
{
	std::uint32_t h = seed;
	h ^= static_cast< std::uint32_t >( x ) * 0x8DA6B343u;
	h ^= static_cast< std::uint32_t >( y ) * 0xD8163841u;
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	h *= 0x297A2D39u;
	h ^= h >> 15;
	return static_cast< float >( h >> 8 ) * ( 1.0f / 16777216.0f );
}

} // namespace vectrix
