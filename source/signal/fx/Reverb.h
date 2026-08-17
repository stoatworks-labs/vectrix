#pragma once

#include "signal/fx/Filters.h"
#include "signal/fx/Fx.h"

#include <array>

namespace vectrix
{
struct ReverbParams
{
	bool enabled     = false;
	float preDelayMs = 20.0f;
	float decaySec   = 2.0f;
	float damping    = 0.5f;
	float diffusion  = 0.7f;
	float size       = 1.0f;
	float modulation = 0.2f;
	float mix        = 0.3f;
};

/**
	The halo and the smear: an 8x8 feedback delay network.

	**An FDN and not a Schroeder or Freeverb bank**, for a reason that is
	specifically visual. A comb-filter reverb produces a handful of discrete
	echoes, and a handful of discrete echoes of an X/Y figure is a handful of
	visibly separate ghost copies of that figure. An FDN's echo density builds
	quadratically, so the halo *fills in* into something continuous, which is
	what a halo is.

	The mixing matrix is **Householder**, `H = I - (2/N)J`, which costs 8 adds,
	one multiply and 8 subtracts -- `s = (2/N)*sum(x); y_i = x_i - s` -- and is
	cheaper than the eight comb filters it replaces. It is also exactly
	orthogonal, so `||Hx|| == ||Hx||` for every input, which the harness asserts
	directly: an energy-preserving matrix is the difference between a reverb that
	decays at the rate you asked for and one that quietly grows.

	**One tank, not two.** X injects into lines 0-3 and Y into 4-7; X reads lines
	0,2,4,6 and Y reads 1,3,5,7. The shared matrix cross-couples them, which is
	what a real stereo reverb does, and it costs half of what two tanks would.
*/
class Reverb : public FxBlock
{
public:
	void Prepare( double sampleRate ) override;
	void Reset() override;
	void Process( Sample* buffer, int n ) override;
	void SetParams( const ReverbParams& p );

	/// Sum of |state| across the tank. The harness uses this to assert that a
	/// parameter change does not silence the tail.
	float TailEnergy() const;

	/// Exposed for the harness: apply the Householder matrix to eight values.
	/// `||Hx|| == ||x||` must hold to within float precision.
	static void Householder( std::array< float, 8 >& v );

private:
	static constexpr int kLines = 8;
	static constexpr int kDiffusers = 4;

	/// Mutually prime lengths at 48 kHz, scaled by the real rate and by Size.
	/// Prime because any common factor between two lines puts their echoes on
	/// top of each other periodically, which rings.
	static constexpr int kBaseLength[ kLines ] = { 1327, 1523, 1723, 1949, 2141, 2381, 2617, 2851 };
	static constexpr int kDiffuserLength[ kDiffusers ] = { 142, 379, 107, 277 };

	ReverbParams params;

	DelayLine lines[ kLines ];
	OnePole damping[ kLines ];
	double lineDelay[ kLines ] = {};
	float lineGain[ kLines ]   = {};
	double modPhase[ kLines ]  = {};
	double modInc[ kLines ]    = {};

	AllpassDelay diffuserA[ kDiffusers ];
	AllpassDelay diffuserB[ kDiffusers ];

	DelayLine preDelayA, preDelayB;
	double preDelaySamples = 0.0;

	std::array< float, kLines > state{};
};

} // namespace vectrix
