#include "timeline/TimelineGrid.h"

#include <algorithm>
#include <cmath>

namespace miacode::timeline {

namespace {

// The last measure line at or before `second`, or the first one.
int measureIndexAt(const TimelineRenderSnapshot& snapshot, double second)
{
    const QVector<double>& lines = snapshot.measureLineSeconds;
    if (lines.isEmpty()) {
        return -1;
    }
    const auto after = std::upper_bound(lines.cbegin(), lines.cend(), second + 1e-9);
    return qMax(0, int(after - lines.cbegin()) - 1);
}

} // namespace

double gridStepSecondsAt(const TimelineRenderSnapshot& snapshot, double second, int division)
{
    const int index = measureIndexAt(snapshot, second);
    if (division <= 0 || index < 0 || index >= snapshot.measureLineBeatStepSeconds.size()
        || index >= snapshot.measureLineMeterDenominators.size()) {
        return 0.0;
    }
    // A beat step is 240 / (BPM * denominator), so a whole bar of 4/4 is
    // step * denominator seconds and one grid step a division-th of that.
    const double beatStep = snapshot.measureLineBeatStepSeconds.at(index);
    const int denominator = qMax(1, snapshot.measureLineMeterDenominators.at(index));
    return beatStep > 0.0 ? beatStep * denominator / division : 0.0;
}

double snapSecondToGrid(const TimelineRenderSnapshot& snapshot, double second, int division)
{
    const double step = gridStepSecondsAt(snapshot, second, division);
    const int index = measureIndexAt(snapshot, second);
    if (step <= 0.0 || index < 0) {
        return second;
    }
    const double origin = snapshot.measureLineSeconds.at(index);
    return origin + std::round((second - origin) / step) * step;
}

} // namespace miacode::timeline
