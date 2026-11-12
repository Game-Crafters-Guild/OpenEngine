#pragma once

// CBTUpdateRestGate — whether the CBT update can be skipped because the tree is at rest.
//
// An update whose inputs (camera, terrain, classify) equal the previous update's, and which split
// nothing, merged nothing and left the pool-pressure and off-frustum keep steps unchanged, leaves
// every CBT buffer as it found them; the next update with the same inputs would too. The work-queue
// counters that say so reach the CPU a few frames late (CBTActivityReadback), tagged with the
// sequence number of the update they were copied from. CPU only: the caller owns the inputs, the
// readback and the decision to declare the update.

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

namespace GameEngine::CBTTerrain
{

// What one update did, from its work-queue counters (CBTLayout.h kWQ* slots).
struct CBTUpdateActivity
{
    int32_t SplitServed = 0;        // kWQAllocateCounter: allocations Split satisfied
    int32_t MergeServed = 0;        // kWQSimplifyCounter: merge groups PrepareSimplify built
    int32_t PressureStep = 0;       // kWQPressureStep: planar pool-pressure scale step
    int32_t OffFrustumKeepStep = 0; // kWQOffFrustumKeepStep
    int32_t OffFrustumKeepFloor = 0; // kWQOffFrustumKeepFloor: the rest's keep floor (>= 0) or its phase
    int32_t VertexEvalCount = 0;    // kWQVertexEvalCounter: bisectors a gated VertexEval re-evaluated
    int32_t UnresolvedPageSamples = 0; // kWQUnresolvedPageSamples: page samples no resident page answered
};

// True while the planar keep step's rest search has a step left to take: the last update moved and a probe
// can start, a probe has no measured tree, or the keep step sits above its floor. Such a tree is not at rest
// even when one update split and merged nothing.
bool CBTKeepSearchOpen(int32_t offFrustumKeepStep, int32_t offFrustumKeepFloor);

class CBTUpdateRestGate
{
  public:
    // Quiet readings, from updates recorded after the inputs last changed, before the update is
    // skipped. Two, because a reading proves its steps unchanged only against the one before it.
    static constexpr uint32_t kQuietReadingsToRest = 2u;

    // How long the inputs hold before the keep step's rest search probes one step down. The wait spans the
    // pauses inside one camera move (an input stroke, a frame hitch) and matches the DDGI camera-rest window.
    static constexpr std::chrono::milliseconds kKeepProbeSettle{200};

    // True when this frame's update, with `inputs` at time `now`, can be skipped. Inputs that differ
    // from the previous call's restart the count; their first update is the first one counted.
    bool CanSkip(std::span<const uint8_t> inputs, std::chrono::steady_clock::time_point now);

    // CBTClassifyDesc::Settle for the update the last CanSkip saw: kSettleMoved when its inputs changed,
    // kSettleHeld when they equal the previous call's, kSettleLasted once they have held for kKeepProbeSettle.
    uint32_t Settle() const { return m_Settle; }

    // An update was declared; returns the sequence number its reading carries. A forced update (a
    // full vertex re-evaluation after a re-seed, a retired heightmap or a planet change) moves
    // corners without a split or merge, so it restarts the count.
    uint64_t OnUpdateRecorded(bool forced);

    // A reading landed. Readings of updates from before the last restart are ignored.
    void OnActivityRead(uint64_t sequence, const CBTUpdateActivity& activity);

    // Forgets everything, as at construction (device rebuild).
    void Reset();

  private:
    void Restart();

    std::vector<uint8_t> m_Inputs;
    bool m_HasInputs = false;
    std::chrono::steady_clock::time_point m_InputsSince{};
    uint32_t m_Settle = 0u;
    uint64_t m_NextSequence = 0;
    uint64_t m_FirstCountedSequence = 0;
    uint32_t m_QuietReadings = 0;
    bool m_HasSteps = false;
    int32_t m_PressureStep = 0;
    int32_t m_OffFrustumKeepStep = 0;
};

} // namespace GameEngine::CBTTerrain
