#pragma once

#include "timeline/TimelineRenderData.h"

namespace miacode::timeline {

// Seconds per grid step of a 1/division bar grid at `second`, from the BPM of
// the measure in effect there; 0 when the snapshot has no measures.
double gridStepSecondsAt(const TimelineRenderSnapshot& snapshot, double second, int division);

// `second` snapped to the 1/division grid counted from its measure line.
// A division of 0 or less leaves the second unchanged.
double snapSecondToGrid(const TimelineRenderSnapshot& snapshot, double second, int division);

} // namespace miacode::timeline
