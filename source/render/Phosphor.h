#pragma once

namespace vectrix
{
/**
    The phosphor table.

    ---------------------------------------------------------------------------
    This is a measured table, not a set of tints
    ---------------------------------------------------------------------------

    Every screen phosphor in this list is a real JEDEC type with published
    figures, and the numbers here are those figures rather than whatever looked
    nice. Two consequences follow, and both of them will read as bugs to somebody
    who has not read this header:

    **P31 at persistence x1 shows no trail at 60 fps.** Its decay constant is
    sixteen microseconds. A frame is sixteen *milliseconds*, a thousand times
    longer, so `exp(-frame/tau)` underflows to zero and every frame starts from
    black. That is not the model failing. That is what a P31 is, and it is why
    every oscilloscope photograph ever taken of a fast transient was taken with
    the shutter open. The Persistence control is there to make it something else.

    **Changing phosphor changes the brightness by up to three times.** P11 is a
    blue photographic phosphor with about a third of P31's luminous efficiency,
    because a third of its light lands where the eye is not very interested.
    Normalising that away would make the control a colour picker, and would throw
    away the single most useful thing the table knows.

    ---------------------------------------------------------------------------
    Two layers
    ---------------------------------------------------------------------------

    Published decay figures are quoted as the time to fall to 10% of peak, so
    `tau = t10 / ln(10)`. The fields below are taus, already converted.

    A long-persistence screen is a cascade, not a mixture: a fast layer that the
    beam excites directly, and a slow layer underneath that is pumped by the
    light the fast layer emits. `transfer` is how much of what the fast layer
    sheds the slow one catches. That is why a P7's trail is yellow-green when its
    strike is blue, and why the trail builds *behind* the beam rather than
    starting at the same instant a mixture would.

    One artefact of doing that once per frame rather than continuously, written
    down here because it looks like a bug and is not: excitation transferred into
    the slow layer during a frame does not decay during that frame, so it always
    survives at least one frame however short `tauSlow` is. On P7 that is half a
    frame of error against a 174 ms decay and invisible. On P31, whose slow layer
    drains in well under a millisecond, it means a residue of `transfer` -- two
    percent of the strike, in the slow layer's colour, for exactly one frame. Two
    percent for one frame at 60 fps is not a trail, which is why the claim above
    stands, but it is the reason a measurement of P31 does not come back at
    exactly zero.

    ---------------------------------------------------------------------------
    Persistence is a multiplier on tau
    ---------------------------------------------------------------------------

    Not a per-frame decay factor. A decay factor means something different at
    every frame rate: an operator who set a pleasing trail on a 60 fps
    composition would find it twice as long on a 30 fps one, and the plugin would
    look like it had a frame-rate bug. A multiplier on tau is a statement about
    the phosphor, and the frame rate cancels out of it.
*/
struct PhosphorSpec
{
	const char* name;

	float tauFast;        ///< Seconds. The layer the beam excites.
	float fastColour[ 3 ];///< Linear RGB, normalised to a peak component of 1.

	float tauSlow;        ///< Seconds. Zero means there is no second layer.
	float slowColour[ 3 ];

	/// How much of the fast layer's shed excitation pumps the slow one, 0..1.
	float transfer;

	/// Luminous efficiency relative to P31, which is the reference because it is
	/// the standard bright scope phosphor everything else was compared against.
	float efficiency;

	/// The excitation at which a layer emits half of what a linear response
	/// would give, in the renderer's deposit units. Long-persistence screens
	/// saturate early -- that is the same property as burning easily.
	float saturation;
};

int phosphorCount();

/// Clamped: an out-of-range index returns P31 rather than reading off the end.
const PhosphorSpec& phosphor( int index );

/// The per-frame factors the decay pass wants, for this phosphor at this
/// persistence and this frame length.
struct PhosphorDecay
{
	float fast;    ///< exp(-frame/tauFast), the fraction the fast layer keeps.
	float slow;    ///< The same for the slow layer; 0 when there is not one.
	float transfer;///< 0 when there is no slow layer.
};

/// `persistenceMultiplier` scales both taus. `frameSeconds` is the length of the
/// frame about to be drawn, from Clock::FrameSeconds().
PhosphorDecay decayFor( const PhosphorSpec& spec, float persistenceMultiplier, float frameSeconds );

/// The Persistence control, as a multiplier on tau. Logarithmic from x0.1 to
/// x1000 -- four decades, because that is the range between "shorter than a
/// frame" and "a smear that lasts most of a second", and a linear control would
/// spend nine tenths of its travel in territory nobody wants.
float persistenceMultiplier( float normalised );

/// Where x1 sits on that control, which is where its default belongs.
constexpr float kPersistenceUnityPoint = 0.25f;

} // namespace vectrix
