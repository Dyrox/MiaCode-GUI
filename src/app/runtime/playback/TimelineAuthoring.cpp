#include "app/runtime/playback/PlaybackCoordinator.h"

#include "app/services/ApplicationServices.h"
#include "core/scene/NoteAuthoringGeometry.h"

namespace {

using miacode::TimelineAuthoringGesture;
using miacode::editor::NoteAuthoringAction;
using miacode::editor::NoteAuthoringRequest;
using miacode::preview::scene::NoteAuthoringTool;

// The timeline's ninth row holds every touch; new touches go to the centre.
constexpr int kTouchRow = 9;
// A note glyph's second, read back from its drawn x, is off by under a pixel.
constexpr double kNoteMatchToleranceSeconds = 0.02;

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
            : tokenAt(gesture.second, &request.line, &request.column);
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
        if (!holdTool || target.isEmpty()
            || !tokenAt(gesture.second, &request.line, &request.column)
            || !tokenAt(gesture.endSecond, &request.lengthLine, &request.lengthColumn)) {
            return;
        }
        request.action = NoteAuthoringAction::Add;
        request.target = target;
        request.item = authoring.holdItemWithoutLength(target);
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
            || !tokenAt(gesture.endSecond, &request.targetLine, &request.targetColumn)) {
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
