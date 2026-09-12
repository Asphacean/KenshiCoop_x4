// ExistenceVerdict - engine-free existence/cull decision for a local body the
// host's census does not name (Phase 12 Plan 03, CENSUS-02/CENSUS-04).
//
// THE RULE. A join enumerates its own world every tick and asks, per body:
// does the other side corroborate that this body EXISTS? Three things can
// corroborate it - this tick's fresh entity stream, an active drive, or a row
// in a FRESH census slice. When nothing does, the body is a local-only ghost
// and must be hidden, but only after a sustained absence (the debounce), and
// only where census silence actually MEANS something.
//
// WHY THIS HEADER EXISTS (the defect it closes). The "where silence means
// something" question used to be asked against the ATTENTION radius
// (attentionRadius_, 1000 u), on the premise - stated in the branch's own
// comment - that "the host deliberately leaves an unattended region out of the
// census". That premise went stale: publishNpcCensus (ReplicatorPublish.cpp)
// deliberately REMOVED its attention gate ("a census row is a statement that a
// body EXISTS, and existence cannot depend on who happens to be looking") and
// now publishes a complete existence claim out to censusRadius_ * 1.25
// (2500 u), enumerated from the same interest centers the receiver anchors on
// (engine::interestAnchors IS engine::interestCenters). Two comments, each
// correct about its own side, never re-read together - so the receiver kept
// declining to act on a claim the publisher was making.
//
// The measured consequence: the annulus 1000 u < d <= 2500 u was a PERMANENT
// cull-free zone. Not a slow cull - an unreachable one, because the escape
// also reset the debounce every tick (unstreak=0/75 on all 316 samples of the
// violating key). Plan 12-02 observed it directly: DECIDE (cull skipped, 528
// samples, dAttn 1004-2031 u) and DROP (cull taken, 5 samples, dAttn 72-990 u)
// are disjoint at exactly attnR = 1000, and the sharpest pair is two members of
// one Garru herd 14 u apart - 990 u culled and converged, 1004 u diverged
// forever. The census_convergence oracle judges out to censusRadius_ * 0.8 =
// 1600 u, which is inside the zone. Full evidence: docs/PHASE_12_GATE.md.
//
// This is NOT an arrival race and NOT a timing defect. Plan 12-02 measured the
// joins arriving 10.2-13.3 s apart in every run, PASS and FAIL alike; the two
// known masking levers (a 20 s launch deferral, 60 ms of receive-side netsim
// delay) work by shortening the losing join's comparable window, not by
// separating the joins in time. The predicate names no join identity, so either
// join can be the loser - which is exactly the observed victim flip (join2,
// join1, join1, join1). The decision below is therefore a pure, local,
// order-independent function, and prototest's testExistenceVerdict encodes the
// flip as a symmetry group: a fix that rescues only one join fails there.
//
// THE FIX. The dormancy escape now requires BOTH that nobody attends the body
// AND that the body lies outside the reach the publisher's claim actually
// covers - distance to the nearest raw interest anchor against censusRadius_.
// censusRadius_ (2000 u), not censusRadius_ * 1.25 (2500 u), is deliberate: the
// publisher's own comment says the 25% margin exists so "a real NPC wandering
// near the boundary (inside the join's scan, outside the host's) would not be
// false-culled". Asking at 2000 u spends that margin exactly as designed, on
// anchor and position disagreement between the two sides. No threshold was
// widened: attentionRadius_ and censusRadius_ keep their values, and the
// attention gate keeps its own job (it still decides what to STREAM).
//
// NO WAIT WAS ADDED. The debounce this decision consults
// (suppressAfter = SUPPRESS_AFTER_FRAMES = 75 frames) is the PRE-EXISTING
// hysteresis, unchanged, and it counts an observable condition - consecutive
// ticks of corroborated absence - never elapsed time. Nothing here sleeps,
// defers, backs off or staggers; Phase 12 forbids all four as fixes.
//
// DISCONNECT / REJOIN. An owner that leaves stops refreshing its census slice;
// censusHasAny then goes false (Replicator::censusHasAny skips any slice older
// than CENSUS_OWNER_STALE_MS) and, once every slice lapses, censusFresh goes
// false too. claimCovers() requires censusFresh, so a departed owner's silence
// can never be read as "these bodies do not exist": the dormancy escape comes
// back and the world is left alone. That is the same fail-open the wide pass
// already implements by running only while censusFresh. Purging a departed
// owner's per-key state (authCount_, suppressed_, attnObs_) is the CALLER's
// job - Replicator::clearPeerReplicationState - not this header's; this
// function holds no state at all.
//
// SHAPE. Header-only, namespace coop, CRT-only, zero engine/Wire/ENet coupling,
// the same precedent FoldDedup.h / PinOwner.h / ClaimArbiter.h / CellMap.h /
// SpeedVote.h set: a PURE function of its inputs, so prototest can exercise it
// exhaustively with no GameWorld. The production call site
// (Replicator::enforceHostAuthority, ReplicatorAuthority.cpp - both the near
// and the wide existence pass) CALLS this and keeps no copy of the rule; a
// shadowed second copy would make every check in prototest vacuous.
//
// THREADING. Pure and stateless: no globals, no statics, no allocation, no
// I/O. Safe to call from the game thread (which is the only place it is
// called) at up to NPC_CENSUS_MAX bodies per tick; cost is O(1) per body, and
// the caller computes dClaimAnchor over at most 4 anchors and only on the
// non-corroborated path.

#ifndef KENSHICOOP_EXISTENCE_VERDICT_H
#define KENSHICOOP_EXISTENCE_VERDICT_H

namespace coop {

// What the caller must do with this body on this tick. Exactly one of these is
// returned for EVERY reachable input (prototest's bounds group asserts the
// totality), so the caller needs no fallback branch.
enum ExistenceOutcome {
    // Corroborated. Reset the cull debounce, advance the restore dwell, and run
    // the park / restore policy the caller owns.
    EXIST_KEEP    = 0,
    // Nobody speaks for this place - neither the local attention gate nor the
    // publisher's census claim reaches it. Hold the debounce AT ZERO and leave
    // the body entirely alone, so that when attention or the claim does arrive
    // the body gets a full debounce to be corroborated instead of being hidden
    // on the first frame someone looks at it.
    EXIST_DORMANT = 1,
    // Census-absent inside the covered region, but the absence has not yet
    // persisted long enough. Advance the debounce; touch nothing else.
    EXIST_COUNT   = 2,
    // Census-absent, covered, and the debounce is satisfied on this tick: hide
    // and freeze the local copy. Only ever returned for a body that is not
    // already suppressed.
    EXIST_CULL    = 3
};

// Everything the decision is allowed to look at. Deliberately reduced to plain
// scalars: no Key, no Character*, no GameWorld, no census container - the
// per-owner slice walk and its staleness rule stay in
// Replicator::censusHasAny, which is keyed by the ownerId the TRANSPORT
// authenticated for that peer and is byte-unchanged by this plan. A census
// bool that arrives here is therefore already attributed; this decision's own
// contribution to that boundary is that it refuses to let an unfresh claim
// vouch for anything (see censusHasAny's use below and the T-12-07 checks in
// prototest).
struct ExistenceInputs {
    // Is ANY census slice current enough to speak at all (the aggregate
    // censusFresh gate, <= 5000 ms)? False disables every claim-derived
    // conclusion below.
    bool censusFresh;
    // Does a fresh slice name this body? The caller passes the reduced result
    // of Replicator::censusHasAny.
    bool censusHasAny;
    // Is the body in this tick's fresh entity-stream set (near pass: this also
    // covers a body applyTargets drove this tick via drivenChars_)?
    bool streamed;
    // Is the body under an active drive, including the drivenSeen_ grace
    // window (wide pass only; the near pass folds this into streamed).
    bool driven;
    // Replicator::observedAt's verdict - is any ATTENTION anchor within
    // attentionRadius_ (with its leave-hysteresis) of the body? The caller must
    // evaluate this exactly once and ONLY when existenceHolds() is false,
    // because observedAt latches per-key hysteresis and counts attach flips.
    bool observedAttn;
    // Distance from the body to the NEAREST raw interest anchor
    // (engine::interestAnchors - the same centers the publisher enumerates its
    // census from). Negative means "no anchors resolved", which fails open.
    float dClaimAnchor;
    // Replicator::censusRadius_ (2000 u default). Zero or negative means the
    // census is disabled, which fails open.
    float censusRadius;
    // The body's consecutive-absence streak BEFORE this tick. The decision
    // accounts for this tick's own increment, so the caller passes the counter
    // as it stands and increments it afterwards exactly as before.
    unsigned int unstreamed;
    // Replicator::enforceHostAuthority's SUPPRESS_AFTER_FRAMES (75).
    unsigned int suppressAfter;
    // Is this body already hidden? A suppressed body never re-culls.
    bool suppressed;
};

// Is the body corroborated by something other than the attention gate? Split
// out because the caller needs this answer BEFORE it may call observedAt - the
// latch must only be touched on the non-corroborated path, which is the
// ordering the pre-Phase-12 code had and this preserves.
//
// A stale claim vouches for nothing: censusHasAny is conjoined with
// censusFresh here, never trusted alone. That is what stops a departed or
// silent owner's minutes-old slice from holding a body alive forever.
inline bool existenceHolds(const ExistenceInputs& in) {
    return in.streamed || in.driven || (in.censusFresh && in.censusHasAny);
}

// Does the publisher's existence claim actually cover the ground this body is
// standing on? The publisher enumerates from the interest centers out to
// censusRadius * 1.25 and omits nothing it authors, so anything within
// censusRadius of one of those same centers is ground the publisher has
// spoken for - and its silence about a body there is a statement, not a gap.
// The remaining 25% is the publisher's documented margin for the two sides
// disagreeing about anchor and body positions; asking at censusRadius spends
// it as designed.
//
// Fails OPEN (returns false, i.e. "assume the claim does not reach here") on
// every degenerate input: no fresh claim, census disabled, or no anchors
// resolved. Failing open costs an uncured ghost; failing closed would cull a
// real body against silence nobody authored.
inline bool claimCovers(const ExistenceInputs& in) {
    if (!in.censusFresh) return false;
    if (in.censusRadius <= 0.0f) return false;
    if (in.dClaimAnchor < 0.0f) return false;
    return in.dClaimAnchor <= in.censusRadius;
}

// The decision. Pure, total, and independent of the order in which bodies,
// owners or ticks are processed.
inline ExistenceOutcome existenceVerdict(const ExistenceInputs& in) {
    if (existenceHolds(in)) return EXIST_KEEP;

    // THE DORMANCY ESCAPE, FIXED. Silence only means "this body does not
    // exist" on ground somebody is speaking for. Two independent ways to be
    // spoken for, and it takes BOTH being absent to make a body dormant:
    // somebody local is ATTENDING it (observedAttn, attentionRadius_), or the
    // publisher's census CLAIM reaches it (claimCovers, censusRadius_).
    //
    // The pre-Phase-12 line was `if (!in.observedAttn) return EXIST_DORMANT;`
    // - the claim-reach conjunct is the entire fix, and reverting exactly this
    // line is the mutation recorded in docs/PHASE_12_GATE.md. It flips the
    // 1000 u < d <= 2000 u annulus from permanently cull-free (debounce reset
    // every tick, cull unreachable) to ordinarily judged.
    if (!in.observedAttn && !claimCovers(in)) return EXIST_DORMANT;

    // Already hidden: keep counting the absence (so the restore dwell has a
    // symmetric streak to work against) but never re-issue the cull.
    if (in.suppressed) return EXIST_COUNT;

    // The caller increments the streak on this tick, so the threshold is tested
    // against the value it is ABOUT to hold. Written without computing
    // unstreamed + 1 so a saturated streak cannot wrap past the threshold and
    // read as "not due yet"; the zero-threshold case is answered first so the
    // decrement below can never underflow.
    if (in.suppressAfter == 0u) return EXIST_CULL;
    if (in.unstreamed >= in.suppressAfter - 1u) return EXIST_CULL;
    return EXIST_COUNT;
}

} // namespace coop

#endif // KENSHICOOP_EXISTENCE_VERDICT_H
