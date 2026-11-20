#include "app/services/NoteAuthoringController.h"

#include "app/services/EditorSyncController.h"
#include "core/scene/PreviewSceneMath.h"

#include <QtMath>

namespace miacode {

namespace {

using miacode::editor::NoteAuthoringAction;
using miacode::editor::NoteAuthoringRequest;
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

bool isTouchTarget(const QString& target)
{
    return !target.isEmpty() && !target.at(0).isDigit();
}

// The target `offset` lanes around the ring; the centre pad stays put.
QString rotatedTarget(const QString& target, int offset)
{
    const int lane = laneOfTarget(target);
    if (lane > 0) {
        return QString::number(miacode::preview::scene::wrappedLane(lane + offset));
    }
    if (target.size() == 2 && target.at(1).isDigit()) {
        return miacode::preview::scene::padTokenForRing(target.at(0), target.at(1).digitValue() + offset);
    }
    return target;
}

} // namespace

NoteAuthoringController::NoteAuthoringController(EditorSyncController& editorSync, QObject* parent)
    : QObject(parent)
    , editorSync_(editorSync)
{
    connect(&editorSync_, &EditorSyncController::noteAuthoringStatusPublished,
            this, &NoteAuthoringController::applyStatus);
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

void NoteAuthoringController::setGuiMode(bool enabled)
{
    if (guiMode_ == enabled) {
        return;
    }
    guiMode_ = enabled;
    // GUI charting starts with a tool in hand; code charting puts it down.
    setTool(enabled ? int(NoteAuthoringTool::Tap) : int(NoteAuthoringTool::None));
    emit guiModeChanged();
}

void NoteAuthoringController::setSlideChain(bool enabled)
{
    if (!enabled) {
        cancelPending();
    }
    assign(&slideChain_, enabled);
}

void NoteAuthoringController::setHoldLength(const QString& length)
{
    assign(&holdLength_, normalizedLength(length));
}

void NoteAuthoringController::setSlideLength(const QString& length)
{
    assign(&slideLength_, normalizedLength(length));
}

void NoteAuthoringController::handleGesture(const miacode::preview::scene::NoteAuthoringGesture& gesture)
{
    if (gesture.tool != tool_ || tool_ == NoteAuthoringTool::None) {
        return;
    }
    const bool click = !gesture.startTarget.isEmpty() && gesture.startTarget == gesture.endTarget;
    if (gesture.secondary) {
        // Right click: abandon a pending hold or chain first, otherwise erase.
        if (!pendingTarget_.isEmpty()) {
            cancelPending();
        } else if (click) {
            sendSimple(NoteAuthoringAction::Remove, gesture.startTarget);
        }
        return;
    }

    switch (tool_) {
    case NoteAuthoringTool::Tap:
    case NoteAuthoringTool::Touch:
        if (click) {
            sendSimple(NoteAuthoringAction::Toggle, gesture.startTarget, itemForTarget(gesture.startTarget));
        }
        break;
    case NoteAuthoringTool::Hold:
    case NoteAuthoringTool::TouchHold:
        if (!click) {
            break;
        }
        if (pendingTarget_.isEmpty()) {
            if (sendSimple(NoteAuthoringAction::HoldBegin, gesture.startTarget)) {
                setPendingTarget(gesture.startTarget);
            }
        } else {
            // The second click may land anywhere: the hold stays on its first
            // target and only takes its length from the cursor's new token.
            const QString target = pendingTarget_;
            setPendingTarget(QString());
            sendSimple(NoteAuthoringAction::HoldFinish, target, holdItemWithoutLength(target));
        }
        break;
    case NoteAuthoringTool::Slide:
        handleSlideGesture(gesture);
        break;
    case NoteAuthoringTool::None:
        break;
    }
}

void NoteAuthoringController::handleSlideGesture(const miacode::preview::scene::NoteAuthoringGesture& gesture)
{
    const int endLane = laneOfTarget(gesture.endTarget);
    if (endLane == 0 || strokeLength(gesture.path) < kMinimumSlideStrokeLogical) {
        return;
    }
    // A running chain continues from its last end, wherever the drag starts.
    if (slideChain_ && !pendingTarget_.isEmpty()) {
        const int fromLane = laneOfTarget(pendingTarget_);
        const QString shape = miacode::preview::scene::matchSlideShape(fromLane, endLane, gesture.path);
        if (!shape.isEmpty() && sendSimple(NoteAuthoringAction::ExtendSlide, chainHead_, shape.mid(1))) {
            setPendingTarget(QString::number(endLane));
        }
        return;
    }
    const int startLane = laneOfTarget(gesture.startTarget);
    if (startLane == 0) {
        return;
    }
    const QString item = slideItem(startLane, endLane, gesture.path);
    if (!item.isEmpty() && sendSimple(NoteAuthoringAction::Add, gesture.startTarget, item) && slideChain_) {
        chainHead_ = gesture.startTarget;
        setPendingTarget(QString::number(endLane));
    }
}

QString NoteAuthoringController::itemForTarget(const QString& target) const
{
    const bool touch = isTouchTarget(target);
    switch (tool_) {
    case NoteAuthoringTool::Tap:
        return touch ? QString() : target + headModifiers() + (starTap_ ? QStringLiteral("$") : QString());
    case NoteAuthoringTool::Hold:
        return touch ? QString() : holdItemWithoutLength(target) + holdLength_;
    case NoteAuthoringTool::Touch:
        return touch ? target + (firework_ ? QStringLiteral("f") : QString()) : QString();
    case NoteAuthoringTool::TouchHold:
        return touch ? holdItemWithoutLength(target) + holdLength_ : QString();
    case NoteAuthoringTool::Slide:
    case NoteAuthoringTool::None:
        break;
    }
    return QString();
}

QString NoteAuthoringController::holdItemWithoutLength(const QString& target) const
{
    // Modifiers stay ahead of the length, the parser's canonical placement.
    if (isTouchTarget(target)) {
        return target + QStringLiteral("h") + (firework_ ? QStringLiteral("f") : QString());
    }
    return target + headModifiers() + QStringLiteral("h");
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

QString NoteAuthoringController::slideItem(int startLane, int endLane, const QVector<QPointF>& path) const
{
    const bool opposite = miacode::preview::scene::wrappedLane(startLane + 4) == endLane;
    const QString shape = wifi_ && opposite
        ? QStringLiteral("%1w%2").arg(startLane).arg(endLane)
        : miacode::preview::scene::matchSlideShape(startLane, endLane, path);
    if (shape.isEmpty()) {
        return QString();
    }
    // "1bx-5[4:1]": head marks right after the start lane; a headless slide
    // (`?`, `!`) has no head to mark.
    static const QString kHeads[] = {QString(), QStringLiteral("@"), QStringLiteral("?"), QStringLiteral("!")};
    const QString head = slideHead_ >= 2 ? kHeads[slideHead_] : kHeads[slideHead_] + headModifiers();
    return shape.left(1) + head + shape.mid(1) + (slideBreak_ ? QStringLiteral("b") : QString()) + slideLength_;
}

QString NoteAuthoringController::keyboardStep(int key, const QString& hovered)
{
    if (tool_ == NoteAuthoringTool::None || hovered.isEmpty()) {
        return QString();
    }
    if (key == Qt::Key_Up || key == Qt::Key_Down) {
        stepBeat(key == Qt::Key_Down ? 1 : -1);
        return hovered;
    }
    if (key != Qt::Key_Left && key != Qt::Key_Right) {
        return QString();
    }
    // Quick placement writes single notes only; holds and slides need a span.
    if (tool_ == NoteAuthoringTool::Tap || tool_ == NoteAuthoringTool::Touch) {
        const QString item = itemForTarget(hovered);
        if (!item.isEmpty()) {
            sendSimple(NoteAuthoringAction::Add, hovered, item);
        }
    }
    stepBeat(1);
    return rotatedTarget(hovered, key == Qt::Key_Right ? 1 : -1);
}

bool NoteAuthoringController::send(NoteAuthoringRequest request)
{
    request.defaultDuration = holdLength_;
    request.separator = pseudoEach_ ? QLatin1Char('`') : QLatin1Char('/');
    return editorSync_.requestNoteAuthoring(request);
}

bool NoteAuthoringController::sendSimple(NoteAuthoringAction action, const QString& target, const QString& item)
{
    NoteAuthoringRequest request;
    request.action = action;
    request.target = target;
    request.item = item;
    return send(request);
}

void NoteAuthoringController::cancelPending()
{
    if (pendingTarget_.isEmpty()) {
        return;
    }
    const QString target = pendingTarget_;
    setPendingTarget(QString());
    chainHead_.clear();
    sendSimple(NoteAuthoringAction::HoldCancel, target);
}

void NoteAuthoringController::copySelection()
{
    sendSimple(NoteAuthoringAction::CopySelection, QStringLiteral("-"));
}

void NoteAuthoringController::cutSelection()
{
    sendSimple(NoteAuthoringAction::CutSelection, QStringLiteral("-"));
}

void NoteAuthoringController::deleteSelection()
{
    sendSimple(NoteAuthoringAction::DeleteSelection, QStringLiteral("-"));
}

void NoteAuthoringController::paste()
{
    sendSimple(NoteAuthoringAction::Paste, QStringLiteral("-"));
}

void NoteAuthoringController::stepBeat(int step)
{
    NoteAuthoringRequest request;
    request.action = NoteAuthoringAction::StepToken;
    request.target = QStringLiteral("-");
    request.step = step;
    send(request);
}

void NoteAuthoringController::setBpm(const QString& bpm)
{
    sendSimple(NoteAuthoringAction::SetBpm, QStringLiteral("-"), bpm.trimmed());
}

void NoteAuthoringController::setDivision(const QString& division)
{
    sendSimple(NoteAuthoringAction::SetDivision, QStringLiteral("-"), division.trimmed());
}

void NoteAuthoringController::transformSelection(int transform)
{
    NoteAuthoringRequest request;
    request.action = NoteAuthoringAction::TransformSelection;
    request.target = QStringLiteral("-");
    request.transform = transform;
    send(request);
}

void NoteAuthoringController::setPendingTarget(const QString& target)
{
    if (pendingTarget_ != target) {
        pendingTarget_ = target;
        emit pendingTargetChanged();
    }
}

void NoteAuthoringController::applyStatus(int selectionCount, int clipCount, const QString& message)
{
    if (selectionCount_ == selectionCount && clipCount_ == clipCount && statusMessage_ == message) {
        return;
    }
    selectionCount_ = selectionCount;
    clipCount_ = clipCount;
    statusMessage_ = message;
    emit statusChanged();
}

} // namespace miacode
