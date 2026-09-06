#ifndef KENSHICOOP_SYNC_TUNING_H
#define KENSHICOOP_SYNC_TUNING_H

// SyncTuning.h - Phase 6d: the one owned home for the change-gated SAMPLED
// channels' send cadence. Before this, every channel in ReplicatorChannels.cpp
// carried its own function-local SAMPLE_MS / MIN_SEND_MS / RESEND_MS literals;
// they now live here as named fields so the whole channel cadence surface reads
// from a single struct the Replicator owns (SyncTuning tuning_). The channel
// bodies still bind their old local names to these fields, so runtime values are
// byte-identical to the literals they replaced - this is a consolidation, not a
// behavior change. Pure POD (a default ctor sets today's tuned values); no
// mutable file-scope state, C++03/v100-safe, header-only.
//
// SCOPE: only the ChangeGate-policy channels (money + the seven registry
// channels: faction, doors, placed buildings, placed-building doors,
// production, research, deeds). Medical/stats cadence and the drive/combat/interp tunables in
// ReplicatorUtil.h are a different subsystem (Phase 7's drive state machine
// owns those) and intentionally stay where they are.

namespace coop {

struct SyncTuning {
    // Money pool (protocol 52): purchases come in bursts, so a ~1 Hz floor on
    // the host's TOTAL is plenty (a fold publishes immediately regardless - the
    // join is waiting on that answer), and a lost total self-heals on the safety
    // resend. Join DELTAS are never throttled: each one is a conservation event.
    unsigned long moneyMinSendMs;   // sampling floor between pool-total sends
    unsigned long moneyResendMs;    // unchanged-total safety resend
    // Overdraft contention window (protocol 60, CONS-01, MoneyFold.h): how
    // long the host holds a would-overdraw window open before finalizing the
    // deterministic timestamp-ordered verdict. Same one-RTT physics as the
    // claim-contention window (CLAIM_WINDOW_MS default 250 ms,
    // ReplicatorItems.cpp), so the default matches it - a single owned field
    // (like every other SyncTuning knob) rather than a second getenv() site;
    // WAN A/B tuning is a future config-wiring pass, deferred like the rest
    // of this struct's fields (none of which read an env var today either).
    unsigned long moneyWindowMs;

    // Faction relations (protocol 24): relations move in bursts; 1 Hz sample,
    // long safety resend for rows we ever sent.
    unsigned long factionSampleMs;
    unsigned long factionResendMs;

    // Baked doors (protocol 26): doors move in clicks; 1 Hz sample.
    unsigned long doorSampleMs;
    unsigned long doorResendMs;

    // Production machines (protocol 33): machines tick slowly; 1 Hz sample,
    // resend doubles as the join drift corrector.
    unsigned long prodSampleMs;
    unsigned long prodResendMs;

    // Research (protocol 38): unlocks are rare; 1 Hz sample, long lost-row /
    // late-prereq corrector resend.
    unsigned long researchSampleMs;
    unsigned long researchResendMs;

    // Property deeds (protocol 54): purchases are rare; 1 Hz sample. The resend
    // is the lost-row corrector AND the not-yet-resolvable corrector - the peer
    // may have the building's zone unloaded when the row first lands - so it
    // matches the research cadence rather than the shorter door one.
    unsigned long deedSampleMs;
    unsigned long deedResendMs;

    // Placed buildings - construction progress (protocol 27).
    unsigned long buildSampleMs;
    unsigned long buildResendMs;

    // Placed-building doors (protocol 28): mirrors the protocol-26 door cadence.
    unsigned long bdoorSampleMs;
    unsigned long bdoorResendMs;

    SyncTuning()
        : moneyMinSendMs(1000),   moneyResendMs(5000), moneyWindowMs(250),
          factionSampleMs(1000),  factionResendMs(10000),
          doorSampleMs(1000),     doorResendMs(10000),
          prodSampleMs(1000),     prodResendMs(10000),
          researchSampleMs(1000), researchResendMs(15000),
          deedSampleMs(1000),     deedResendMs(15000),
          buildSampleMs(1000),    buildResendMs(10000),
          bdoorSampleMs(1000),    bdoorResendMs(10000) {}
};

} // namespace coop

#endif // KENSHICOOP_SYNC_TUNING_H
