#include "app/runtime/playback/PlaybackCoordinator.h"

#include "app/services/ApplicationServices.h"
#include "core/scene/NoteAuthoringGeometry.h"
#include "timeline/TimelineGrid.h"

#include <cmath>
#include <numeric>

namespace {

using miacode::TimelineAuthoringGesture;
using miacode::editor::NoteAuthoringAction;
using miacode::editor::NoteAuthoringRequest;
using miacode::preview::scene::NoteAuthoringTool;

// The timeline's ninth row holds every touch; new touches go to the centre.
constexpr int kTouchRow = 9;
// A note glyph's second, read back from its drawn x, is off by under a pixel.
constexpr double kNoteMatchToleranceSeconds = 0.02;
// Grid points closer than this to a beat start land on that beat.
constexpr double kGridMatchSeconds = 0.002;

// "[N:k]" reduced, counted in whole beats where it can be: [4:2], not [2:1].
QString gridLength(int division, int steps)
{
    qint64 denominator = division;
    qint64 numerator = steps;
    const qint64 divisor = std::gcd(denominator, numerator);
    denominator /= divisor;
    numerator /= divisor;
    if (denominator < 4 && 4 % denominator == 0) {
        numerator *= 4 / denominator;
        denominator = 4;
    }
    return QStringLiteral("[%1:%2]").arg(denominator).arg(numerator);
}

QString targetForRow(int lane)
{
    if (lane >= 1 && lane <= 8) {
        return QString::number(lane);
    }
    return lane == kTouchRow ? QStringLiteral("C") : QString();
}

} // namespace

void miacode::runtime::PlaybackCoordinator::handleTimelineAuthoring(
    const TimelineAuthoringGesture& gesture, quint64 documentRevision)
{
    NoteAuthoringController& authoring = services_.noteAuthoring();
    const auto tool = miacode::preview::scene::noteAuthoringToolFromInt(authoring.tool());
    if (tool == NoteAuthoringTool::None || !hasActiveDifficulty()) {
        return;
    }
    const TimelineQuickModel& model = state_.timeline_.timelineQuickModel_;
    // Positions below come from the timeline built for this revision; the
    // editor refuses them once the text has moved on.
    NoteAuthoringRequest request;
    request.sourceRevision = documentRevision;
    const auto tokenAt = [&model](double second, int* line, int* column) {
        return model.resolveNearestCursorAnchorForSecond(second, line, column, nullptr);
    };
    // Where a press at `second` writes: snapped to the 分音 grid, onto a beat
    // that exists or between two (the editor then splits that beat).
    const int division = authoring.gridDivision();
    const auto placeAt = [&](double second, int* line, int* column, int* gridDivision, double* fraction) {
        *gridDivision = 0;
        if (division <= 0) {
            return tokenAt(second, line, column);
        }
        const double snapped = miacode::timeline::snapSecondToGrid(model.snapshot(), second, division);
        double start = 0.0;
        double end = 0.0;
        if (!model.resolveCursorSpanForSecond(snapped, line, column, &start, &end)) {
            return tokenAt(snapped, line, column);
        }
        if (end - snapped <= kGridMatchSeconds) {
            return tokenAt(end, line, column);
        }
        if (snapped - start > kGridMatchSeconds && end - start > kGridMatchSeconds) {
            *gridDivision = division;
            *fraction = (snapped - start) / (end - start);
        }
        return true;
    };
    const auto noteAt = [&model](double second, int lane, int* line, int* column) {
        double noteSecond = 0.0;
        return model.resolveNearestTimelineNote(second, lane, line, column, &noteSecond)
            && qAbs(noteSecond - second) <= kNoteMatchToleranceSeconds;
    };

    switch (gesture.kind) {
    case TimelineAuthoringGesture::Kind::Click: {
        if (gesture.secondary) {
            // Right click erases the note under the pointer.
            int line = 0;
            int column = 0;
            if (gesture.onNote && noteAt(gesture.second, gesture.lane, &line, &column)) {
                request.action = NoteAuthoringAction::Remove;
                request.target = targetForRow(gesture.lane);
                request.notes = {QPoint(column, line)};
                authoring.send(request);
            }
            return;
        }
        const QString target = targetForRow(gesture.lane);
        const QString item = authoring.itemForTarget(target);
        if (item.isEmpty()) {
            return;
        }
        // On a note, toggle within that note's token; elsewhere snap to the
        // nearest beat.
        const bool found = gesture.onNote
            ? noteAt(gesture.second, gesture.lane, &request.line, &request.column)
            : placeAt(gesture.second, &request.line, &request.column, &request.gridDivision, &request.tokenFraction);
        if (!found) {
            return;
        }
        request.action = NoteAuthoringAction::Toggle;
        request.target = target;
        request.item = item;
        authoring.send(request);
        return;
    }
    case TimelineAuthoringGesture::Kind::Span: {
        // A drag along one row writes a hold that spans it.
        const bool holdTool = (tool == NoteAuthoringTool::Hold && gesture.lane <= 8)
            || (tool == NoteAuthoringTool::TouchHold && gesture.lane == kTouchRow);
        const QString target = targetForRow(gesture.lane);
        if (!holdTool || target.isEmpty()) {
            return;
        }
        const double from = qMin(gesture.second, gesture.endSecond);
        const double to = qMax(gesture.second, gesture.endSecond);
        if (!placeAt(from, &request.line, &request.column, &request.gridDivision, &request.tokenFraction)) {
            return;
        }
        request.action = NoteAuthoringAction::Add;
        request.target = target;
        request.item = authoring.holdItemWithoutLength(target);
        if (division > 0) {
            // On a grid the length is a count of grid steps.
            const double step = miacode::timeline::gridStepSecondsAt(model.snapshot(), from, division);
            const int steps = step > 0.0 ? int(std::lround((to - from) / step)) : 0;
            request.item += steps > 0 ? gridLength(division, steps) : authoring.holdLength();
        } else if (!tokenAt(to, &request.lengthLine, &request.lengthColumn)) {
            return;
        }
        authoring.send(request);
        return;
    }
    case TimelineAuthoringGesture::Kind::Move: {
        // Notes keep their row kind: lanes rotate among lanes, touches only
        // move in time.
        const bool laneRows = gesture.lane <= 8 && gesture.endLane <= 8;
        if (!laneRows && gesture.lane != gesture.endLane) {
            return;
        }
        if (!noteAt(gesture.second, gesture.lane, &request.line, &request.column)
            || !placeAt(gesture.endSecond, &request.targetLine, &request.targetColumn,
                        &request.gridDivision, &request.tokenFraction)) {
            return;
        }
        request.action = NoteAuthoringAction::Move;
        request.target = targetForRow(gesture.lane);
        request.laneDelta = laneRows ? gesture.endLane - gesture.lane : 0;
        authoring.send(request);
        return;
    }
    case TimelineAuthoringGesture::Kind::Box:
        request.action = NoteAuthoringAction::SelectNotes;
        request.target = QStringLiteral("-");
        if (gesture.endLane < 0) {
            for (int lane = 1; lane <= kTouchRow; ++lane) {
                if ((gesture.lane & (1 << lane)) != 0) {
                    request.notes += model.notePositionsInRange(gesture.second, gesture.endSecond, lane, lane);
                }
            }
        } else {
            request.notes = model.notePositionsInRange(gesture.second, gesture.endSecond, gesture.lane, gesture.endLane);
        }
        authoring.send(request);
        return;
    }
}
