#pragma once
#include <cstdint>
#include <algorithm>

// Estimate transport delay from the unshifted frame time, never the predicted
// pose timestamp: subtracting the latter feeds our own lead back into the EWMA.
struct PosePrediction {
    int64_t lead = 40000000;
    int64_t lastSample = 0;
    void reset() { lead = 40000000; lastSample = 0; }
    void observe(int64_t displayedAt, int64_t sourceDisplayTime) {
        if (sourceDisplayTime <= lastSample || displayedAt < sourceDisplayTime) return;
        lastSample = sourceDisplayTime;
        auto delay = displayedAt - sourceDisplayTime;
        if (delay > 250000000) return; // paused/stalled video is not normal latency
        delay = std::min<int64_t>(delay, 100000000);
        lead += (delay - lead) / 10;
    }
};
