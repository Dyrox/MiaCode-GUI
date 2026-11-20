#include "app/services/NoteAuthoringController.h"

#include "app/services/EditorSyncController.h"
#include "core/scene/PreviewSceneMath.h"

#include <QVariantMap>
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
    cancelSlidePick();
    tool_ = normalized;
    emit toolChanged();
    emit pickChanged();
    emit previewGuideChanged();
}

void NoteAuthoringController::setGuiMode(bool enabled)
{
    if (guiMode_ == enabled) {
        return;
    }
    guiMode_ = enabled;
    touchPicking_ = false;
    // GUI charting starts with a tool in hand; code charting puts it down.
    setTool(enabled ? int(NoteAuthoringTool::Tap) : int(NoteAuthoringTool::None));
    emit guiModeChanged();
    emit pickChanged();
    emit previewGuideChanged();
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

int NoteAuthoringController::previewTool() const
{
    if (!guiMode_) {
        return int(tool_);
    }
    if (slidePicking()) {
        return int(NoteAuthoringTool::Slide);
    }
    if (touchPicking_) {
        return int(tool_ == NoteAuthoringTool::TouchHold ? NoteAuthoringTool::TouchHold : NoteAuthoringTool::Touch);
    }
    return int(NoteAuthoringTool::None);
}

void NoteAuthoringController::setTouchPicking(bool enabled)
{
    if (touchPicking_ == enabled) {
        return;
    }
    touchPicking_ = enabled;
    emit pickChanged();
    emit previewGuideChanged();
}

QVariantList NoteAuthoringController::slidePickShapes() const
{
    return slidePicking() && slidePickEnd_ > 0 ? slideShapes(slidePickCurrentLane(), slidePickEnd_) : QVariantList();
}

int NoteAuthoringController::slidePickCurrentLane() const
{
    return slidePickChain_.isEmpty() ? slidePickFrom_ : slidePickChain_.back().digitValue();
}

QString NoteAuthoringController::previewAnchor() const
{
    return slidePicking() ? QString::number(slidePickCurrentLane()) : pendingTarget_;
}

bool NoteAuthoringController::previewShowsTargets() const
{
    return guiMode_ && (touchPicking_ || slidePicking());
}

QVector<QPointF> NoteAuthoringController::previewGuidePath() const
{
    if (!slidePicking()) {
        return {};
    }
    QVector<QPointF> path = miacode::preview::scene::slideShapeLogicalPath(slidePickChain_);
    path += miacode::preview::scene::slideShapeLogicalPath(slidePickHover_);
    return path;
}

QVector<miacode::preview::scene::NoteAuthoringGuideArrow> NoteAuthoringController::previewGuideArrows() const
{
    if (!slidePicking()) {
        return {};
    }
    auto arrows = miacode::preview::scene::slideShapeArrows(slidePickChain_);
    arrows += miacode::preview::scene::slideShapeArrows(slidePickHover_);
    return arrows;
}

void NoteAuthoringController::setLaneCue(int lane)
{
    lane = lane >= 1 && lane <= 8 ? lane : 0;
    if (laneCue_ != lane) {
        laneCue_ = lane;
        emit previewGuideChanged();
    }
}

QString NoteAuthoringController::touchItem(const QString& pad) const
{
    return pad + (firework_ ? QStringLiteral("f") : QString());
}

void NoteAuthoringController::beginSlidePick(int startLane, int endLane)
{
    if (startLane < 1 || startLane > 8) {
        return;
    }
    slidePickFrom_ = startLane;
    slidePickEnd_ = endLane >= 1 && endLane <= 8 ? endLane : 0;
    slidePickChain_.clear();
    slidePickChainLengths_.clear();
    slidePickHover_.clear();
    emit pickChanged();
    emit previewGuideChanged();
}

void NoteAuthoringController::setSlidePickEnd(int lane)
{
    if (!slidePicking() || lane < 1 || lane > 8) {
        return;
    }
    slidePickEnd_ = lane;
    slidePickHover_.clear();
    emit pickChanged();
    emit previewGuideChanged();
}

void NoteAuthoringController::previewSlideShape(const QString& key)
{
    if (!slidePicking() || slidePickHover_ == key) {
        return;
    }
    slidePickHover_ = key;
    emit previewGuideChanged();
}

void NoteAuthoringController::pickSlideShape(const QString& key)
{
    if (!slidePicking() || key.isEmpty() || key.front().digitValue() != slidePickCurrentLane()) {
        return;
    }
    if (!slideChain_) {
        finishSlidePick(key);
        return;
    }
    // 连锁: the segment joins the chain, and the next one starts at its end.
    slidePickChainLengths_.append(slidePickChain_.size());
    slidePickChain_ = slidePickChain_.isEmpty() ? key : slidePickChain_ + key.mid(1);
    slidePickEnd_ = 0;
    slidePickHover_.clear();
    emit pickChanged();
    emit previewGuideChanged();
}

void NoteAuthoringController::undoSlidePick()
{
    if (!slidePicking()) {
        return;
    }
    if (!slidePickHover_.isEmpty()) {
        previewSlideShape(QString());
        return;
    }
    if (slidePickChainLengths_.isEmpty()) {
        cancelSlidePick();
        return;
    }
    slidePickChain_.truncate(slidePickChainLengths_.takeLast());
    slidePickEnd_ = 0;
    emit pickChanged();
    emit previewGuideChanged();
}

void NoteAuthoringController::confirmSlidePick()
{
    if (slidePicking() && !slidePickChain_.isEmpty()) {
        finishSlidePick(slidePickChain_);
    }
}

void NoteAuthoringController::cancelSlidePick()
{
    if (slidePicking()) {
        finishSlidePick(QString());
    }
}

void NoteAuthoringController::finishSlidePick(QString shape)
{
    slidePickFrom_ = 0;
    slidePickEnd_ = 0;
    slidePickChain_.clear();
    slidePickChainLengths_.clear();
    slidePickHover_.clear();
    emit pickChanged();
    emit previewGuideChanged();
    emit slidePickFinished(shape);
}

void NoteAuthoringController::handlePickGesture(const miacode::preview::scene::NoteAuthoringGesture& gesture)
{
    if (slidePicking()) {
        // Right click steps back, as in classic editors.
        if (gesture.secondary) {
            undoSlidePick();
            return;
        }
        const int endLane = laneOfTarget(gesture.endTarget);
        if (endLane == 0) {
            return;
        }
        // A click on a button offers the shapes to it; a stroke is read as
        // the shape it draws.
        if (strokeLength(gesture.path) < kMinimumSlideStrokeLogical) {
            setSlidePickEnd(endLane);
            return;
        }
        const int fromLane = slidePickCurrentLane();
        const bool opposite = miacode::preview::scene::wrappedLane(fromLane + 4) == endLane;
        const QString shape = wifi_ && opposite
            ? QStringLiteral("%1w%2").arg(fromLane).arg(endLane)
            : miacode::preview::scene::matchSlideShape(fromLane, endLane, gesture.path);
        if (!shape.isEmpty()) {
            pickSlideShape(shape);
        }
        return;
    }
    const bool click = !gesture.startTarget.isEmpty() && gesture.startTarget == gesture.endTarget;
    if (touchPicking_ && click && isTouchTarget(gesture.startTarget)) {
        setTouchPad(gesture.startTarget);
        emit touchPadPicked(gesture.startTarget, gesture.secondary);
    }
}

void NoteAuthoringController::handleGesture(const miacode::preview::scene::NoteAuthoringGesture& gesture)
{
    // GUI charting places through the lane editor; the preview only serves
    // its touch and slide pickers.
    if (guiMode_) {
        handlePickGesture(gesture);
        return;
    }
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
    return shape.isEmpty() ? QString() : slideItemForShape(shape, QString());
}

QString NoteAuthoringController::slideItemForShape(const QString& shapeKey, const QString& length) const
{
    if (shapeKey.size() < 3) {
        return QString();
    }
    // "1bx-5[4:1]": head marks right after the start lane; a headless slide
    // (`?`, `!`) has no head to mark.
    static const QString kHeads[] = {QString(), QStringLiteral("@"), QStringLiteral("?"), QStringLiteral("!")};
    const QString head = slideHead_ >= 2 ? kHeads[slideHead_] : kHeads[slideHead_] + headModifiers();
    return shapeKey.left(1) + head + shapeKey.mid(1) + (slideBreak_ ? QStringLiteral("b") : QString())
        + (length.isEmpty() ? slideLength_ : length);
}

QVariantList NoteAuthoringController::slideShapes(int startLane, int endLane) const
{
    // Names for the shape symbols, so the picker reads like the editor's own.
    const auto name = [](const QString& key) {
        const QString symbol = key.mid(1, key.size() - 2);
        if (symbol == QLatin1String("-")) return qtTrId("qml.charting.shape_straight");
        if (symbol == QLatin1String("^")) return qtTrId("qml.charting.shape_short_arc");
        if (symbol == QLatin1String(">") || symbol == QLatin1String("<")) return qtTrId("qml.charting.shape_ring");
        if (symbol == QLatin1String("v")) return qtTrId("qml.charting.shape_center");
        if (symbol == QLatin1String("p") || symbol == QLatin1String("q")) return qtTrId("qml.charting.shape_around");
        if (symbol == QLatin1String("pp") || symbol == QLatin1String("qq")) return qtTrId("qml.charting.shape_around_wide");
        if (symbol == QLatin1String("s") || symbol == QLatin1String("z")) return qtTrId("qml.charting.shape_zigzag");
        if (symbol == QLatin1String("w")) return qtTrId("qml.charting.shape_wifi");
        if (symbol.startsWith(QLatin1Char('V'))) return qtTrId("qml.charting.shape_bend");
        return QString();
    };
    QVariantList shapes;
    for (const QString& key : miacode::preview::scene::slideShapeKeys(startLane, endLane)) {
        shapes.append(QVariantMap{{QStringLiteral("key"), key}, {QStringLiteral("label"), name(key)}});
    }
    return shapes;
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
    request.tidy = guiMode_;
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

void NoteAuthoringController::editSelection(const QString& operation)
{
    NoteAuthoringRequest request;
    request.action = NoteAuthoringAction::EditSelection;
    request.target = QStringLiteral("-");
    request.item = operation;
    request.gridDivision = gridDivision_;
    send(request);
}

void NoteAuthoringController::setTouchPad(const QString& pad)
{
    const QString normalized = pad.trimmed().toUpper();
    const bool valid = normalized == QLatin1String("C")
        || (normalized.size() == 2 && QStringLiteral("ABDE").contains(normalized.at(0))
            && normalized.at(1) >= QLatin1Char('1') && normalized.at(1) <= QLatin1Char('8'));
    if (valid) {
        assign(&touchPad_, normalized);
    }
}

void NoteAuthoringController::setPendingTarget(const QString& target)
{
    if (pendingTarget_ != target) {
        pendingTarget_ = target;
        emit pendingTargetChanged();
    }
}

void NoteAuthoringController::applyStatus(int selectionCount, int clipCount, const QString& message,
                                          const QVariantList& clipLayout)
{
    if (selectionCount_ == selectionCount && clipCount_ == clipCount && statusMessage_ == message
        && clipLayout_ == clipLayout) {
        return;
    }
    selectionCount_ = selectionCount;
    clipCount_ = clipCount;
    statusMessage_ = message;
    clipLayout_ = clipLayout;
    emit statusChanged();
}

} // namespace miacode
