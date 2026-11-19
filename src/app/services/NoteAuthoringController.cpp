#include "app/services/NoteAuthoringController.h"

#include "app/services/EditorSyncController.h"
#include "core/scene/PreviewSceneMath.h"

#include <QtMath>

namespace miacode {

namespace {

using miacode::editor::NoteAuthoringAction;
using miacode::preview::scene::NoteAuthoringTool;

// A click inside one lane sector must not be read as a looping slide.
constexpr qreal kMinimumSlideStrokeLogical = 40.0;

// Accepts "4:1", "[4:1]" or "#1.5"; anything empty falls back to one beat.
QString normalizedLength(const QString& length)
{
    QString text = length.trimmed();
    if (text.startsWith(QLatin1Char('['))) {
        text.remove(0, 1);
    }
    if (text.endsWith(QLatin1Char(']'))) {
        text.chop(1);
    }
    text = text.trimmed();
    return text.isEmpty() ? QStringLiteral("[4:1]") : QStringLiteral("[%1]").arg(text);
}

qreal strokeLength(const QVector<QPointF>& path)
{
    qreal length = 0.0;
    for (int index = 1; index < path.size(); ++index) {
        const QPointF delta = path.at(index) - path.at(index - 1);
        length += qSqrt(QPointF::dotProduct(delta, delta));
    }
    return length;
}

int laneOfTarget(const QString& target)
{
    return target.size() == 1 && target.at(0).isDigit() ? target.at(0).digitValue() : 0;
}

} // namespace

NoteAuthoringController::NoteAuthoringController(EditorSyncController& editorSync, QObject* parent)
    : QObject(parent)
    , editorSync_(editorSync)
{
}

void NoteAuthoringController::setTool(int tool)
{
    const NoteAuthoringTool normalized = miacode::preview::scene::noteAuthoringToolFromInt(tool);
    if (tool_ == normalized) {
        return;
    }
    cancelPending();
    tool_ = normalized;
    emit toolChanged();
}

void NoteAuthoringController::setBreakNote(bool enabled)
{
    if (breakNote_ != enabled) {
        breakNote_ = enabled;
        emit optionsChanged();
    }
}

void NoteAuthoringController::setExNote(bool enabled)
{
    if (exNote_ != enabled) {
        exNote_ = enabled;
        emit optionsChanged();
    }
}

void NoteAuthoringController::setStarTap(bool enabled)
{
    if (starTap_ != enabled) {
        starTap_ = enabled;
        emit optionsChanged();
    }
}

void NoteAuthoringController::setFirework(bool enabled)
{
    if (firework_ != enabled) {
        firework_ = enabled;
        emit optionsChanged();
    }
}

void NoteAuthoringController::setWifi(bool enabled)
{
    if (wifi_ != enabled) {
        wifi_ = enabled;
        emit optionsChanged();
    }
}

void NoteAuthoringController::setHoldLength(const QString& length)
{
    const QString normalized = normalizedLength(length);
    if (holdLength_ != normalized) {
        holdLength_ = normalized;
        emit optionsChanged();
    }
}

void NoteAuthoringController::setSlideLength(const QString& length)
{
    const QString normalized = normalizedLength(length);
    if (slideLength_ != normalized) {
        slideLength_ = normalized;
        emit optionsChanged();
    }
}

void NoteAuthoringController::handleGesture(const miacode::preview::scene::NoteAuthoringGesture& gesture)
{
    if (gesture.tool != tool_ || tool_ == NoteAuthoringTool::None) {
        return;
    }
    const bool click = !gesture.startTarget.isEmpty() && gesture.startTarget == gesture.endTarget;
    if (gesture.secondary) {
        // Right click: abandon a pending hold first, otherwise erase.
        if (!pendingTarget_.isEmpty()) {
            cancelPending();
        } else if (click) {
            send(NoteAuthoringAction::Remove, gesture.startTarget);
        }
        return;
    }

    switch (tool_) {
    case NoteAuthoringTool::Tap:
        if (click) {
            send(NoteAuthoringAction::Toggle, gesture.startTarget, tapItem(gesture.startTarget));
        }
        break;
    case NoteAuthoringTool::Touch:
        if (click) {
            send(NoteAuthoringAction::Toggle, gesture.startTarget, touchItem(gesture.startTarget));
        }
        break;
    case NoteAuthoringTool::Hold:
    case NoteAuthoringTool::TouchHold:
        if (!click) {
            break;
        }
        if (pendingTarget_.isEmpty()) {
            if (send(NoteAuthoringAction::HoldBegin, gesture.startTarget)) {
                setPendingTarget(gesture.startTarget);
            }
        } else {
            // The second click may land anywhere: the hold stays on its first
            // target and only takes its length from the cursor's new token.
            const QString target = pendingTarget_;
            setPendingTarget(QString());
            send(NoteAuthoringAction::HoldFinish, target, holdItemWithoutLength(target));
        }
        break;
    case NoteAuthoringTool::Slide: {
        const int startLane = laneOfTarget(gesture.startTarget);
        const int endLane = laneOfTarget(gesture.endTarget);
        if (startLane == 0 || endLane == 0 || strokeLength(gesture.path) < kMinimumSlideStrokeLogical) {
            break;
        }
        const QString item = slideItem(startLane, endLane, gesture.path);
        if (!item.isEmpty()) {
            send(NoteAuthoringAction::Add, gesture.startTarget, item);
        }
        break;
    }
    case NoteAuthoringTool::None:
        break;
    }
}

void NoteAuthoringController::cancelPending()
{
    if (pendingTarget_.isEmpty()) {
        return;
    }
    const QString target = pendingTarget_;
    setPendingTarget(QString());
    send(NoteAuthoringAction::HoldCancel, target);
}

QString NoteAuthoringController::headModifiers() const
{
    QString modifiers;
    if (breakNote_) {
        modifiers += QLatin1Char('b');
    }
    if (exNote_) {
        modifiers += QLatin1Char('x');
    }
    return modifiers;
}

QString NoteAuthoringController::tapItem(const QString& lane) const
{
    return lane + headModifiers() + (starTap_ ? QStringLiteral("$") : QString());
}

QString NoteAuthoringController::holdItemWithoutLength(const QString& target) const
{
    // Modifiers stay ahead of the length, the parser's canonical placement.
    if (tool_ == NoteAuthoringTool::TouchHold) {
        return target + QStringLiteral("h") + (firework_ ? QStringLiteral("f") : QString());
    }
    return target + headModifiers() + QStringLiteral("h");
}

QString NoteAuthoringController::slideItem(int startLane, int endLane, const QVector<QPointF>& path) const
{
    const bool opposite = miacode::preview::scene::wrappedLane(startLane + 4) == endLane;
    const QString shape = wifi_ && opposite
        ? QStringLiteral("%1w%2").arg(startLane).arg(endLane)
        : miacode::preview::scene::matchSlideShape(startLane, endLane, path);
    if (shape.isEmpty()) {
        return QString();
    }
    // The head modifiers belong right after the start lane: "1bx-5[4:1]".
    return shape.left(1) + headModifiers() + shape.mid(1) + slideLength_;
}

QString NoteAuthoringController::touchItem(const QString& pad) const
{
    return pad + (firework_ ? QStringLiteral("f") : QString());
}

bool NoteAuthoringController::send(NoteAuthoringAction action, const QString& target, const QString& item)
{
    miacode::editor::NoteAuthoringRequest request;
    request.action = action;
    request.target = target;
    request.item = item;
    request.defaultDuration = holdLength_;
    return editorSync_.requestNoteAuthoring(request);
}

void NoteAuthoringController::setPendingTarget(const QString& target)
{
    if (pendingTarget_ != target) {
        pendingTarget_ = target;
        emit pendingTargetChanged();
    }
}

} // namespace miacode
