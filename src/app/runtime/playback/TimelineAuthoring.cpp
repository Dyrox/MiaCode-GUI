#include "app/runtime/playback/PlaybackCoordinator.h"

#include "app/services/ApplicationServices.h"
#include "core/scene/NoteAuthoringGeometry.h"
#include "timeline/TimelineGrid.h"

#include <cmath>
#include <limits>
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
// A bar at the default 120 BPM, for a chart with no measures yet.
constexpr double kDefaultBarSeconds = 2.0;

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

// Touches go to the pad last picked on the preview.
QString targetForRow(int lane, const QString& touchPad)
{
    if (lane >= 1 && lane <= 8) {
        return QString::number(lane);
    }
    return lane == kTouchRow ? touchPad : QString();
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
    // that exists or between two (the editor then splits that beat). Past
    // the chart's last token the editor first adds commas up to it.
    const int division = authoring.gridDivision();
    const auto placeAt = [&](double second, int* line, int* column, int* gridDivision, double* fraction,
                             bool extend = true) {
        *gridDivision = 0;
        const TimelineRenderSnapshot& snapshot = model.snapshot();
        if (extend) {
            // 谱面 (division 0) grows the chart by beats.
            const int extendDivision = division > 0 ? division : 4;
            double step = miacode::timeline::gridStepSecondsAt(snapshot, second, extendDivision);
            if (step <= 0.0) {
                step = kDefaultBarSeconds / extendDivision;
            }
            int lastLine = 1;
            int lastColumn = 1;
            double lastSecond = snapshot.measureLineSeconds.isEmpty() ? 0.0 : snapshot.measureLineSeconds.constFirst();
            const bool hasToken = model.resolveNearestCursorAnchorForSecond(
                std::numeric_limits<double>::max(), &lastLine, &lastColumn, &lastSecond);
            if (!hasToken || second - lastSecond > step * 0.5) {
                *line = lastLine;
                *column = lastColumn;
                request.extendDivision = extendDivision;
                request.extendSteps = qMax(0, int(std::lround((second - lastSecond) / step)));
                return true;
            }
        }
        if (division <= 0) {
            return tokenAt(second, line, column);
        }
        const double snapped = miacode::timeline::snapSecondToGrid(snapshot, second, division);
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
                request.target = targetForRow(gesture.lane, authoring.touchPad());
                request.notes = {QPoint(column, line)};
                authoring.send(request);
            }
            return;
        }
        if (gesture.onNote && gesture.lane == kTouchRow) {
            // Touches on other pads share the column: a click takes away the
            // one under the pointer rather than toggling the picked pad.
            int line = 0;
            int column = 0;
            if (noteAt(gesture.second, gesture.lane, &line, &column)) {
                request.action = NoteAuthoringAction::Remove;
                request.target = QStringLiteral("-");
                request.notes = {QPoint(column, line)};
                authoring.send(request);
            }
            return;
        }
        const QString target = targetForRow(gesture.lane, authoring.touchPad());
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
        const QString target = targetForRow(gesture.lane, authoring.touchPad());
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
                        &request.gridDivision, &request.tokenFraction, false)) {
            return;
        }
        request.action = NoteAuthoringAction::Move;
        request.target = QStringLiteral("-");
        request.laneDelta = laneRows ? gesture.endLane - gesture.lane : 0;
        authoring.send(request);
        return;
    }
    case TimelineAuthoringGesture::Kind::Slide: {
        if (gesture.lane < 1 || gesture.lane > 8 || gesture.shape.isEmpty()
            || !placeAt(gesture.second, &request.line, &request.column, &request.gridDivision,
                        &request.tokenFraction)) {
            return;
        }
        // The star leaves one beat after its head; the length covers the
        // move from there to the end the user clicked.
        const TimelineRenderSnapshot& snapshot = model.snapshot();
        const double head = gesture.onNote ? gesture.second
            : division > 0 ? miacode::timeline::snapSecondToGrid(snapshot, gesture.second, division) : gesture.second;
        const double beat = miacode::timeline::gridStepSecondsAt(snapshot, head, 4);
        const int lengthDivision = division > 0 ? division : 16;
        const double step = miacode::timeline::gridStepSecondsAt(snapshot, head, lengthDivision);
        const int steps = step > 0.0 ? int(std::lround((gesture.endSecond - head - beat) / step)) : 0;
        request.action = NoteAuthoringAction::Add;
        request.target = QString::number(gesture.lane);
        request.item = authoring.slideItemForShape(gesture.shape, steps > 0 ? gridLength(lengthDivision, steps) : QString());
        if (gesture.onNote) {
            // Re-editing: the slide whose star sits at the head.
            if (!noteAt(gesture.second, gesture.lane, &request.line, &request.column)) {
                return;
            }
            request.action = NoteAuthoringAction::EditSlide;
            request.gridDivision = 0;
        }
        authoring.send(request);
        return;
    }
    case TimelineAuthoringGesture::Kind::Resize: {
        // A hold's tail dragged along its lane: the length becomes the grid
        // steps from head to the new tail, at least one.
        if (!noteAt(gesture.second, gesture.lane, &request.line, &request.column)) {
            return;
        }
        const int lengthDivision = division > 0 ? division : 16;
        const double step = miacode::timeline::gridStepSecondsAt(model.snapshot(), gesture.second, lengthDivision);
        if (step <= 0.0) {
            return;
        }
        const int steps = qMax(1, int(std::lround((gesture.endSecond - gesture.second) / step)));
        request.action = NoteAuthoringAction::SetLength;
        request.target = QStringLiteral("-");
        request.item = gridLength(lengthDivision, steps);
        authoring.send(request);
        return;
    }
    case TimelineAuthoringGesture::Kind::Paste:
        if (!placeAt(gesture.second, &request.line, &request.column, &request.gridDivision,
                     &request.tokenFraction)) {
            return;
        }
        request.action = NoteAuthoringAction::Paste;
        request.target = QStringLiteral("-");
        authoring.send(request);
        return;
    case TimelineAuthoringGesture::Kind::Touch:
        // Picked on the preview for the time chosen in the T column.
        if (gesture.shape.isEmpty()
            || !placeAt(gesture.second, &request.line, &request.column, &request.gridDivision,
                        &request.tokenFraction)) {
            return;
        }
        request.action = gesture.secondary ? NoteAuthoringAction::Add : NoteAuthoringAction::Toggle;
        request.target = gesture.shape;
        request.item = authoring.touchItem(gesture.shape);
        authoring.send(request);
        return;
    case TimelineAuthoringGesture::Kind::MoveSelection:
        if (!placeAt(gesture.endSecond, &request.line, &request.column, &request.gridDivision,
                     &request.tokenFraction)) {
            return;
        }
        request.action = NoteAuthoringAction::MoveSelection;
        request.target = QStringLiteral("-");
        request.laneDelta = gesture.lane <= 8 && gesture.endLane <= 8 ? gesture.endLane - gesture.lane : 0;
        authoring.send(request);
        return;
    case TimelineAuthoringGesture::Kind::Bpm:
        // `(BPM)` written at the grid point, from the BPM column.
        if (gesture.shape.isEmpty()
            || !placeAt(gesture.second, &request.line, &request.column, &request.gridDivision,
                        &request.tokenFraction)) {
            return;
        }
        request.action = NoteAuthoringAction::SetBpm;
        request.target = QStringLiteral("-");
        request.item = gesture.shape;
        authoring.send(request);
        return;
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
