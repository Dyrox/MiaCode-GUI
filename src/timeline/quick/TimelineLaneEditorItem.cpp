#include "timeline/quick/TimelineLaneEditorItem.h"

#include "core/video/PreviewSkinConfig.h"
#include "timeline/TimelineGrid.h"
#include "timeline/TimelineRenderData.h"
#include "timeline/quick/TimelineQuickStateBridge.h"

#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QQuickWindow>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int kLaneCount = 9;
constexpr int kTouchLane = 9;
// Eight button lanes and the touch column, then a wider BPM column.
constexpr qreal kColumnUnits = 9.0 + 1.6;
constexpr qreal kLabelStripHeight = 30.0;
constexpr qreal kJudgeFraction = 0.8;
constexpr qreal kDragThreshold = 6.0;
constexpr double kBeatEpsilon = 1e-4;
// Notes whose start lies this far below the view can still reach into it.
constexpr double kLongestBodySeconds = 30.0;

// A dark track-editor palette; the surface stays dark in every UI theme,
// like the preview stage.
const QColor kGutterColor(27, 27, 28);
const QColor kGridColor(42, 42, 43);
const QColor kColumnLineColor(58, 58, 60);
const QColor kBeatLineColor(98, 98, 100);
const QColor kMeasureLineColor(224, 52, 52);
const QColor kNoteRowColor(63, 191, 95);
const QColor kLabelColor(190, 190, 194);
const QColor kAccentColor(76, 157, 255);
const QColor kTapColor(242, 85, 155);
const QColor kEachColor(246, 194, 28);
const QColor kBreakColor(255, 138, 28);
const QColor kSlideColor(102, 212, 255);
const QColor kSlideEdgeColor(43, 98, 255);
const QColor kTouchColor(63, 184, 255);

bool hasFlag(quint32 flags, TimelineRenderNoteFlag flag)
{
    return (flags & quint32(flag)) != 0u;
}

QColor headColor(quint32 flags, const QColor& normal)
{
    if (hasFlag(flags, TimelineRenderFlagIsBreak) || hasFlag(flags, TimelineRenderFlagHeadBreak)) {
        return kBreakColor;
    }
    return hasFlag(flags, TimelineRenderFlagIsEach) || hasFlag(flags, TimelineRenderFlagHeadEach)
        ? kEachColor : normal;
}

QPolygonF starPolygon(const QPointF& center, qreal radius)
{
    QPolygonF star;
    for (int point = 0; point < 10; ++point) {
        const qreal angle = M_PI / 2.0 + point * M_PI / 5.0;
        const qreal r = point % 2 == 0 ? radius : radius * 0.48;
        star << QPointF(center.x() + r * std::cos(angle), center.y() - r * std::sin(angle));
    }
    return star;
}

// A hold body: a vertical bar with pointed ends.
QPolygonF holdPolygon(qreal x, qreal top, qreal bottom, qreal halfWidth, qreal tip)
{
    QPolygonF shape;
    shape << QPointF(x, top - tip) << QPointF(x + halfWidth, top) << QPointF(x + halfWidth, bottom)
          << QPointF(x, bottom + tip) << QPointF(x - halfWidth, bottom) << QPointF(x - halfWidth, top);
    return shape;
}

} // namespace

TimelineLaneEditorItem::TimelineLaneEditorItem(QQuickItem* parent)
    : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    setAntialiasing(true);
    setFlag(ItemIsFocusScope, true);
    setActiveFocusOnTab(true);
    connect(this, &TimelineLaneEditorItem::appearanceChanged, this, [this] { update(); });
}

QObject* TimelineLaneEditorItem::stateBridgeObject() const
{
    return stateBridge_;
}

void TimelineLaneEditorItem::setStateBridgeObject(QObject* bridge)
{
    auto* typed = qobject_cast<TimelineQuickStateBridge*>(bridge);
    if (stateBridge_ == typed) {
        return;
    }
    if (stateBridge_ != nullptr) {
        disconnect(stateBridge_, nullptr, this, nullptr);
    }
    stateBridge_ = typed;
    if (stateBridge_ != nullptr) {
        connect(stateBridge_, &TimelineQuickStateBridge::renderStateChanged, this, [this] { update(); });
        connect(stateBridge_, &TimelineQuickStateBridge::playheadChanged, this, [this] { update(); });
    }
    emit stateBridgeChanged();
    update();
}

void TimelineLaneEditorItem::setPixelsPerSecond(double value)
{
    const double bounded = qBound(40.0, value, 1600.0);
    if (!qFuzzyCompare(pixelsPerSecond_, bounded)) {
        pixelsPerSecond_ = bounded;
        emit appearanceChanged();
    }
}

void TimelineLaneEditorItem::clearAuthoringSelection()
{
    if (selectionValid_) {
        selectionValid_ = false;
        update();
    }
}

qreal TimelineLaneEditorItem::gutterWidth() const
{
    return 40.0;
}

qreal TimelineLaneEditorItem::columnWidth() const
{
    return qMax<qreal>(8.0, (width() - gutterWidth()) / kColumnUnits);
}

qreal TimelineLaneEditorItem::lanesRight() const
{
    return gutterWidth() + kLaneCount * columnWidth();
}

qreal TimelineLaneEditorItem::lanesBottom() const
{
    return height() - kLabelStripHeight;
}

qreal TimelineLaneEditorItem::judgeY() const
{
    return lanesBottom() * kJudgeFraction;
}

double TimelineLaneEditorItem::playheadSecond() const
{
    return stateBridge_ != nullptr ? stateBridge_->playheadSeconds() : 0.0;
}

qreal TimelineLaneEditorItem::yForSecond(double second) const
{
    return judgeY() - (second - playheadSecond()) * pixelsPerSecond_;
}

double TimelineLaneEditorItem::secondForY(qreal y) const
{
    return playheadSecond() + (judgeY() - y) / pixelsPerSecond_;
}

// Columns run 8..1 from the left, then the touch column.
qreal TimelineLaneEditorItem::xForLane(int lane) const
{
    const int column = lane >= 1 && lane <= 8 ? 8 - lane : 8;
    return gutterWidth() + (column + 0.5) * columnWidth();
}

int TimelineLaneEditorItem::laneAtX(qreal x) const
{
    if (x < gutterWidth() || x >= lanesRight()) {
        return 0;
    }
    const int column = static_cast<int>(std::floor((x - gutterWidth()) / columnWidth()));
    return column < 8 ? 8 - column : kTouchLane;
}

QVector<TimelineLaneEditorItem::VisibleNote> TimelineLaneEditorItem::visibleNotes() const
{
    QVector<VisibleNote> notes;
    if (stateBridge_ == nullptr) {
        return notes;
    }
    const double top = secondForY(0.0);
    const double bottom = secondForY(height());
    for (const TimelineRenderLine& line : stateBridge_->renderSnapshot().lines) {
        if (line.endSecond < bottom - kLongestBodySeconds || line.startSecond > top) {
            continue;
        }
        for (const TimelineRenderNote& note : line.notes) {
            const double second = timelineRenderAbsoluteSecond(line, note.secondOffset);
            const double endSecond = note.endSecondOffset >= 0.0
                ? timelineRenderAbsoluteSecond(line, note.endSecondOffset) : second;
            if (endSecond < bottom || second > top) {
                continue;
            }
            VisibleNote visible;
            visible.second = second;
            visible.lane = qBound(1, note.lane, kLaneCount);
            visible.endLane = qBound(1, note.endLane, kLaneCount);
            visible.kind = int(note.kind);
            visible.flags = note.flags;
            visible.head = QPointF(xForLane(visible.lane), yForSecond(second));
            const bool slideLike = note.kind == TimelineRenderNoteKind::Slide
                || note.kind == TimelineRenderNoteKind::Wifi;
            visible.tail = QPointF(xForLane(slideLike ? visible.endLane : visible.lane), yForSecond(endSecond));
            notes.append(visible);
        }
    }
    return notes;
}

QVector<double> TimelineLaneEditorItem::beatSeconds(double from, double to) const
{
    QVector<double> seconds;
    if (stateBridge_ == nullptr) {
        return seconds;
    }
    for (const TimelineRenderLine& line : stateBridge_->renderSnapshot().lines) {
        if (line.endSecond < from - 1.0 || line.startSecond > to + 1.0) {
            continue;
        }
        for (const TimelineRenderBeat& beat : line.beats) {
            const double second = timelineRenderAbsoluteSecond(line, beat.secondOffset);
            if (second >= from && second <= to) {
                seconds.append(second);
            }
        }
    }
    return seconds;
}

QVector<TimelineLaneEditorItem::TempoMark> TimelineLaneEditorItem::tempoMarks() const
{
    QVector<TempoMark> marks;
    if (stateBridge_ == nullptr) {
        return marks;
    }
    // Each measure line knows its beat step under its meter denominator:
    // step = 240 / (BPM * denominator).
    const TimelineRenderSnapshot& snapshot = stateBridge_->renderSnapshot();
    const int count = qMin(snapshot.measureLineSeconds.size(),
                           qMin(snapshot.measureLineBeatStepSeconds.size(), snapshot.measureLineMeterDenominators.size()));
    for (int index = 0; index < count; ++index) {
        const double step = snapshot.measureLineBeatStepSeconds.at(index);
        const int denominator = qMax(1, snapshot.measureLineMeterDenominators.at(index));
        if (step <= 0.0) {
            continue;
        }
        const double bpm = 240.0 / (step * denominator);
        if (marks.isEmpty() || qAbs(marks.constLast().bpm - bpm) > 0.005) {
            marks.append({snapshot.measureLineSeconds.at(index), bpm});
        }
    }
    return marks;
}

namespace {

// The skin icon key for a note head, following the timeline's naming.
QString skinIconType(const TimelineRenderNoteKind kind, quint32 flags)
{
    const bool mine = hasFlag(flags, TimelineRenderFlagIsMine);
    const auto starType = [flags, mine](bool breakHead, bool eachHead) {
        const bool doubled = hasFlag(flags, TimelineRenderFlagTapStarDouble);
        if (mine) {
            return QStringLiteral("star_mine");
        }
        if (breakHead) {
            return doubled ? QStringLiteral("star_break_double") : QStringLiteral("star_break");
        }
        if (eachHead) {
            return doubled ? QStringLiteral("star_each_double") : QStringLiteral("star_each");
        }
        return doubled ? QStringLiteral("star_double") : QStringLiteral("slide");
    };
    const bool isBreak = hasFlag(flags, TimelineRenderFlagIsBreak);
    const bool isEach = hasFlag(flags, TimelineRenderFlagIsEach);
    const auto variant = [&](const QString& base) {
        return mine ? base + QStringLiteral("_mine")
            : isBreak ? base + QStringLiteral("_break")
            : isEach ? base + QStringLiteral("_each") : base;
    };
    switch (kind) {
    case TimelineRenderNoteKind::Tap:
        return hasFlag(flags, TimelineRenderFlagTapUsesStarMaterial) ? starType(isBreak, isEach) : variant(QStringLiteral("tap"));
    case TimelineRenderNoteKind::Hold:
        return variant(QStringLiteral("hold"));
    case TimelineRenderNoteKind::Slide:
    case TimelineRenderNoteKind::Wifi:
        if (hasFlag(flags, TimelineRenderFlagSlideHeadUsesTapMaterial)) {
            return hasFlag(flags, TimelineRenderFlagHeadBreak) ? QStringLiteral("tap_break")
                : hasFlag(flags, TimelineRenderFlagHeadEach) ? QStringLiteral("tap_each") : QStringLiteral("tap");
        }
        return starType(hasFlag(flags, TimelineRenderFlagHeadBreak), hasFlag(flags, TimelineRenderFlagHeadEach));
    case TimelineRenderNoteKind::Touch:
        return variant(QStringLiteral("touch"));
    case TimelineRenderNoteKind::TouchHold:
        return variant(QStringLiteral("touch_hold"));
    case TimelineRenderNoteKind::Unknown:
        break;
    }
    return QString();
}

} // namespace

bool TimelineLaneEditorItem::ensureSkinAssets()
{
    const QString directory = stateBridge_ != nullptr ? stateBridge_->skinDirectory() : QString();
    if (!skinAssetsLoaded_ || directory != skinAssetsDirectory_) {
        skinAssets_ = miacode::timeline::loadTimelineNoteAssets(directory);
        skinAssetsDirectory_ = directory;
        skinAssetsLoaded_ = true;
        scaledIcons_.clear();
    }
    return !skinAssets_.noteIcons.isEmpty();
}

QPixmap TimelineLaneEditorItem::skinIcon(const QString& type, qreal size) const
{
    const qreal ratio = window() != nullptr ? window()->effectiveDevicePixelRatio() : 1.0;
    const int pixels = qMax(1, qRound(size * ratio));
    const QString key = QStringLiteral("%1|%2").arg(type).arg(pixels);
    const auto cached = scaledIcons_.constFind(key);
    if (cached != scaledIcons_.cend()) {
        return cached.value();
    }
    const QPixmap source = skinAssets_.noteIcons.value(type);
    if (source.isNull()) {
        return QPixmap();
    }
    QPixmap scaled = source.scaled(QSize(pixels, pixels), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(ratio);
    scaledIcons_.insert(key, scaled);
    return scaled;
}

bool TimelineLaneEditorItem::paintSkinNote(QPainter* painter, const VisibleNote& note, qreal radius) const
{
    const auto kind = TimelineRenderNoteKind(note.kind);
    if ((kind == TimelineRenderNoteKind::Slide || kind == TimelineRenderNoteKind::Wifi)
        && !hasFlag(note.flags, TimelineRenderFlagHasHeadStar)) {
        return true;
    }
    if (kind == TimelineRenderNoteKind::Hold) {
        return paintSkinHold(painter, note, radius);
    }
    const bool star = kind == TimelineRenderNoteKind::Slide || kind == TimelineRenderNoteKind::Wifi
        || hasFlag(note.flags, TimelineRenderFlagTapUsesStarMaterial);
    const QPixmap icon = skinIcon(skinIconType(kind, note.flags), radius * (star ? 2.9 : 2.5));
    if (icon.isNull()) {
        return false;
    }
    if (hasFlag(note.flags, TimelineRenderFlagIsEx) || hasFlag(note.flags, TimelineRenderFlagHeadEx)) {
        painter->setPen(QPen(QColor(255, 255, 255, 170), 2.0));
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(note.head, radius * 1.4, radius * 1.4);
    }
    const QSizeF size = icon.deviceIndependentSize();
    painter->drawPixmap(QPointF(note.head.x() - size.width() / 2, note.head.y() - size.height() / 2), icon);
    return true;
}

bool TimelineLaneEditorItem::paintSkinHold(QPainter* painter, const VisibleNote& note, qreal radius) const
{
    // The skin's hold is drawn upright: keep its two ends and stretch the
    // middle row between the head (bottom) and the tail (top).
    const QPixmap icon = skinIcon(skinIconType(TimelineRenderNoteKind::Hold, note.flags), radius * 2.5);
    if (icon.isNull()) {
        return false;
    }
    const QSizeF size = icon.deviceIndependentSize();
    const qreal cap = size.height() * miacode::preview_skin::kHoldCapSliceRatioNumerator
        / miacode::preview_skin::kHoldCapSliceRatioDenominator;
    const qreal capPixels = cap * icon.devicePixelRatio();
    const qreal left = note.head.x() - size.width() / 2;
    const qreal top = qMin(note.tail.y(), note.head.y()) - size.height() / 2;
    const qreal bottom = note.head.y() + size.height() / 2;
    const QRectF source(0, 0, icon.width(), icon.height());
    painter->drawPixmap(QRectF(left, top, size.width(), cap),
                        icon, QRectF(0, 0, icon.width(), capPixels));
    painter->drawPixmap(QRectF(left, top + cap, size.width(), qMax<qreal>(0.0, bottom - top - 2 * cap)),
                        icon, QRectF(0, source.height() / 2, icon.width(), 1));
    painter->drawPixmap(QRectF(left, bottom - cap, size.width(), cap),
                        icon, QRectF(0, icon.height() - capPixels, icon.width(), capPixels));
    return true;
}

void TimelineLaneEditorItem::paintNote(QPainter* painter, const VisibleNote& note, qreal radius) const
{
    const auto kind = TimelineRenderNoteKind(note.kind);
    const QPointF center = note.head;
    switch (kind) {
    case TimelineRenderNoteKind::Slide:
    case TimelineRenderNoteKind::Wifi: {
        if (!hasFlag(note.flags, TimelineRenderFlagHasHeadStar)) {
            return;
        }
        const QColor fill = hasFlag(note.flags, TimelineRenderFlagHeadBreak) ? kBreakColor
            : hasFlag(note.flags, TimelineRenderFlagHeadEach) ? kEachColor : kSlideColor;
        painter->setPen(QPen(fill == kSlideColor ? kSlideEdgeColor : fill.darker(150), 2.0));
        painter->setBrush(fill);
        painter->drawPolygon(starPolygon(center, radius * 1.2));
        painter->setPen(QPen(Qt::white, 1.2));
        painter->setBrush(Qt::NoBrush);
        painter->drawPolygon(starPolygon(center, radius * 0.7));
        return;
    }
    case TimelineRenderNoteKind::Touch:
    case TimelineRenderNoteKind::TouchHold: {
        if (kind == TimelineRenderNoteKind::TouchHold) {
            // Four coloured quarters of a diamond.
            const QColor quarters[] = {QColor(255, 82, 82), QColor(255, 210, 63), QColor(76, 205, 110), QColor(63, 150, 255)};
            const QPointF corners[] = {center + QPointF(0, -radius), center + QPointF(radius, 0),
                                       center + QPointF(0, radius), center + QPointF(-radius, 0)};
            painter->setPen(Qt::NoPen);
            for (int index = 0; index < 4; ++index) {
                painter->setBrush(quarters[index]);
                painter->drawPolygon(QPolygonF() << center << corners[index] << corners[(index + 1) % 4]);
            }
            painter->setPen(QPen(Qt::white, 1.5));
            painter->setBrush(Qt::NoBrush);
            painter->drawPolygon(QPolygonF() << corners[0] << corners[1] << corners[2] << corners[3]);
            return;
        }
        const QColor color = hasFlag(note.flags, TimelineRenderFlagIsBreak) ? kBreakColor
            : hasFlag(note.flags, TimelineRenderFlagIsEach) ? kEachColor : kTouchColor;
        const QRectF box(center.x() - radius * 0.85, center.y() - radius * 0.85, radius * 1.7, radius * 1.7);
        painter->setPen(QPen(color, 2.0));
        painter->setBrush(QColor(color.red(), color.green(), color.blue(), 50));
        painter->drawRect(box);
        painter->drawLine(box.topLeft(), box.bottomRight());
        painter->drawLine(box.topRight(), box.bottomLeft());
        if (hasFlag(note.flags, TimelineRenderFlagIsFirework)) {
            painter->setPen(QPen(kEachColor, 1.5, Qt::DotLine));
            painter->setBrush(Qt::NoBrush);
            painter->drawEllipse(center, radius * 1.35, radius * 1.35);
        }
        return;
    }
    default:
        break;
    }
    // Taps and hold heads: a coloured ring around a white core, crossed by a
    // short bar.
    const QColor color = headColor(note.flags, kTapColor);
    if (hasFlag(note.flags, TimelineRenderFlagIsEx)) {
        painter->setPen(QPen(QColor(255, 255, 255, 180), 2.0));
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(center, radius * 1.25, radius * 1.25);
    }
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    painter->drawEllipse(center, radius, radius);
    painter->setBrush(Qt::white);
    painter->drawEllipse(center, radius * 0.56, radius * 0.56);
    painter->setPen(QPen(QColor(60, 60, 64), radius * 0.24, Qt::SolidLine, Qt::RoundCap));
    painter->drawLine(center - QPointF(radius * 0.38, 0), center + QPointF(radius * 0.38, 0));
}

void TimelineLaneEditorItem::paint(QPainter* painter)
{
    const qreal w = width();
    const qreal bottomY = lanesBottom();
    const qreal cw = columnWidth();
    painter->fillRect(QRectF(0, 0, w, height()), kGutterColor);
    painter->fillRect(QRectF(gutterWidth(), 0, w - gutterWidth(), bottomY), kGridColor);

    // Column dividers; the touch and BPM columns stand apart.
    for (int column = 0; column <= kLaneCount; ++column) {
        const qreal x = gutterWidth() + column * cw;
        painter->setPen(QPen(column >= 8 ? kBeatLineColor : kColumnLineColor, 1));
        painter->drawLine(QPointF(x, 0), QPointF(x, bottomY));
    }
    // The hovered lane carries a guide line.
    if (hoveredLane_ > 0) {
        painter->setPen(QPen(kNoteRowColor, 1.5));
        painter->drawLine(QPointF(xForLane(hoveredLane_), 0), QPointF(xForLane(hoveredLane_), bottomY));
    }

    // Grid: the 分音 grid faintly, one line per comma of the chart, and
    // measure lines in red with their number in the gutter.
    const double top = secondForY(0.0);
    const double bottom = secondForY(bottomY);
    if (stateBridge_ != nullptr && gridDivision_ > 0) {
        const TimelineRenderSnapshot& snapshot = stateBridge_->renderSnapshot();
        painter->setPen(QPen(kColumnLineColor, 1));
        double second = miacode::timeline::snapSecondToGrid(snapshot, bottom, gridDivision_);
        for (int guard = 0; second <= top && guard < 2000; ++guard) {
            const double step = miacode::timeline::gridStepSecondsAt(snapshot, second, gridDivision_);
            if (step <= 0.0) {
                break;
            }
            const qreal y = yForSecond(second);
            painter->drawLine(QPointF(gutterWidth(), y), QPointF(lanesRight(), y));
            // Re-snap each step so a BPM change re-anchors the grid.
            second = miacode::timeline::snapSecondToGrid(snapshot, second + step, gridDivision_);
        }
    }
    painter->setPen(QPen(kBeatLineColor, 1));
    for (const double second : beatSeconds(bottom, top)) {
        const qreal y = yForSecond(second);
        painter->drawLine(QPointF(gutterWidth(), y), QPointF(lanesRight(), y));
    }
    const QVector<VisibleNote> notes = visibleNotes();
    // Rows that hold notes light up green, as in the classic editor.
    painter->setPen(QPen(kNoteRowColor, 1.5));
    double lastRow = -1.0;
    for (const VisibleNote& note : notes) {
        if (qAbs(note.second - lastRow) > kBeatEpsilon && note.head.y() >= 0 && note.head.y() <= bottomY) {
            painter->drawLine(QPointF(gutterWidth(), note.head.y()), QPointF(lanesRight(), note.head.y()));
            lastRow = note.second;
        }
    }
    if (stateBridge_ != nullptr) {
        const QVector<double>& measures = stateBridge_->renderSnapshot().measureLineSeconds;
        QFont font = painter->font();
        font.setPixelSize(11);
        painter->setFont(font);
        for (int index = 0; index < measures.size(); ++index) {
            const double second = measures.at(index);
            if (second < bottom || second > top) {
                continue;
            }
            const qreal y = yForSecond(second);
            painter->setPen(QPen(kMeasureLineColor, 1.5));
            painter->drawLine(QPointF(0, y), QPointF(lanesRight(), y));
            painter->setPen(kLabelColor);
            painter->drawText(QRectF(0, y + 2, gutterWidth() - 6, 14), Qt::AlignRight | Qt::AlignTop,
                              QString::number(index + 1));
        }
    }

    ensureSkinAssets();
    // Bodies first, heads on top.
    const qreal radius = qBound<qreal>(6.0, cw * 0.32, 14.0);
    for (const VisibleNote& note : notes) {
        const auto kind = TimelineRenderNoteKind(note.kind);
        if (kind == TimelineRenderNoteKind::Hold && !skinAssets_.noteIcons.isEmpty()) {
            continue;  // drawn whole by paintSkinHold
        }
        if (kind == TimelineRenderNoteKind::Hold || kind == TimelineRenderNoteKind::TouchHold) {
            const QColor color = kind == TimelineRenderNoteKind::TouchHold
                ? kTouchColor : headColor(note.flags, kTapColor);
            QColor fill = color;
            fill.setAlpha(70);
            painter->setPen(QPen(color, 2.5));
            painter->setBrush(fill);
            painter->drawPolygon(holdPolygon(note.head.x(), note.tail.y(), note.head.y(), radius * 0.8, radius * 0.6));
        } else if (kind == TimelineRenderNoteKind::Slide || kind == TimelineRenderNoteKind::Wifi) {
            const QColor color = hasFlag(note.flags, TimelineRenderFlagTrackBreak) ? kBreakColor : kSlideColor;
            painter->setPen(QPen(color, 2.0, Qt::DashLine));
            painter->drawLine(note.head, note.tail);
            painter->setPen(QPen(color, 1.5));
            painter->setBrush(Qt::NoBrush);
            painter->drawPolygon(starPolygon(note.tail, radius * 0.55));
        }
    }
    const bool skinned = !skinAssets_.noteIcons.isEmpty();
    for (const VisibleNote& note : notes) {
        if (!skinned || !paintSkinNote(painter, note, radius)) {
            paintNote(painter, note, radius);
        }
    }

    // The judgement line: white over a red edge, with the BPM in effect.
    const qreal judge = judgeY();
    painter->setPen(QPen(kMeasureLineColor, 4));
    painter->drawLine(QPointF(0, judge), QPointF(w, judge));
    painter->setPen(QPen(Qt::white, 2));
    painter->drawLine(QPointF(gutterWidth(), judge), QPointF(lanesRight(), judge));
    const QVector<TempoMark> marks = tempoMarks();
    const auto badge = [&](double bpm, qreal y, bool current) {
        const QRectF rect(lanesRight() + 4, y - 9, w - lanesRight() - 8, 18);
        if (rect.width() < 20) {
            return;
        }
        painter->setPen(Qt::NoPen);
        painter->setBrush(current ? QColor(63, 191, 111) : QColor(63, 191, 111, 120));
        painter->drawRoundedRect(rect, 9, 9);
        painter->setPen(current ? QColor(16, 40, 22) : Qt::white);
        painter->drawText(rect, Qt::AlignCenter, QString::number(bpm, 'f', 2));
    };
    double currentBpm = marks.isEmpty() ? 0.0 : marks.constFirst().bpm;
    for (const TempoMark& mark : marks) {
        if (mark.second <= playheadSecond() + kBeatEpsilon) {
            currentBpm = mark.bpm;
        } else if (mark.second <= top) {
            badge(mark.bpm, yForSecond(mark.second), false);
        }
    }
    if (currentBpm > 0.0) {
        badge(currentBpm, judge, true);
    }

    // Selection box and the cell a press would land in.
    QRectF box;
    if (drag_ == Drag::Box) {
        box = QRectF(pressPoint_, currentPoint_).normalized();
    } else if (selectionValid_) {
        const qreal left = qMin(xForLane(selectionFirstLane_), xForLane(selectionLastLane_)) - cw / 2;
        const qreal right = qMax(xForLane(selectionFirstLane_), xForLane(selectionLastLane_)) + cw / 2;
        box = QRectF(QPointF(left, yForSecond(selectionEndSecond_)), QPointF(right, yForSecond(selectionStartSecond_)));
    }
    if (!box.isEmpty()) {
        QColor fill = kAccentColor;
        fill.setAlpha(40);
        painter->setPen(QPen(kAccentColor, 1));
        painter->setBrush(fill);
        painter->drawRect(box);
    }
    if (drag_ == Drag::MoveNote || drag_ == Drag::Place) {
        const int lane = laneAtX(currentPoint_.x());
        if (lane > 0) {
            const qreal grab = drag_ == Drag::MoveNote ? yForSecond(pressSecond_) - pressPoint_.y() : 0.0;
            qreal y = currentPoint_.y() + grab;
            if (stateBridge_ != nullptr && gridDivision_ > 0) {
                y = yForSecond(miacode::timeline::snapSecondToGrid(stateBridge_->renderSnapshot(), secondForY(y),
                                                                   gridDivision_));
            }
            painter->setPen(QPen(kAccentColor, 1.5));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(QRectF(xForLane(lane) - cw / 2 + 2, y - radius, cw - 4, radius * 2), 3, 3);
        }
    }

    // Lane buttons along the bottom: 8..1 in dotted rings, then T and BPM.
    painter->fillRect(QRectF(0, bottomY, w, kLabelStripHeight), kGutterColor);
    QFont labelFont = painter->font();
    labelFont.setPixelSize(12);
    labelFont.setBold(true);
    painter->setFont(labelFont);
    const qreal labelY = bottomY + kLabelStripHeight / 2;
    const qreal ring = qMin<qreal>(9.0, cw * 0.36);
    for (int lane = 1; lane <= kLaneCount; ++lane) {
        const QPointF center(xForLane(lane), labelY);
        if (lane == kTouchLane) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(58, 123, 213));
            painter->drawEllipse(center, ring, ring);
            painter->setPen(Qt::white);
            painter->drawText(QRectF(center.x() - ring, center.y() - ring, ring * 2, ring * 2), Qt::AlignCenter,
                              QStringLiteral("T"));
            continue;
        }
        painter->setPen(Qt::NoPen);
        painter->setBrush(lane == hoveredLane_ ? kNoteRowColor : kLabelColor);
        for (int dot = 0; dot < 8; ++dot) {
            const qreal angle = dot * M_PI / 4.0 + M_PI / 8.0;
            painter->drawEllipse(center + QPointF(ring * std::cos(angle), ring * std::sin(angle)), 1.2, 1.2);
        }
        painter->setPen(Qt::white);
        painter->drawText(QRectF(center.x() - ring, center.y() - ring, ring * 2, ring * 2), Qt::AlignCenter,
                          QString::number(lane));
    }
    painter->setPen(kLabelColor);
    painter->drawText(QRectF(lanesRight(), bottomY, w - lanesRight(), kLabelStripHeight), Qt::AlignCenter,
                      QStringLiteral("BPM"));
}

bool TimelineLaneEditorItem::hitNote(const QPointF& point, double* second, int* lane) const
{
    const int pointLane = laneAtX(point.x());
    if (pointLane == 0) {
        return false;
    }
    const qreal reach = qMax<qreal>(8.0, qBound<qreal>(6.0, columnWidth() * 0.32, 14.0) * 1.2);
    qreal best = reach;
    bool found = false;
    for (const VisibleNote& note : visibleNotes()) {
        const qreal distance = qAbs(note.head.y() - point.y());
        if (note.lane == pointLane && distance <= best) {
            best = distance;
            *second = note.second;
            *lane = note.lane;
            found = true;
        }
    }
    return found;
}

void TimelineLaneEditorItem::hoverMoveEvent(QHoverEvent* event)
{
    const int lane = event->position().y() < lanesBottom() ? laneAtX(event->position().x()) : 0;
    if (lane != hoveredLane_) {
        hoveredLane_ = lane;
        update();
    }
}

void TimelineLaneEditorItem::hoverLeaveEvent(QHoverEvent*)
{
    if (hoveredLane_ != 0) {
        hoveredLane_ = 0;
        update();
    }
}

void TimelineLaneEditorItem::mousePressEvent(QMouseEvent* event)
{
    forceActiveFocus(Qt::MouseFocusReason);
    const int lane = laneAtX(event->position().x());
    if (event->position().y() > lanesBottom() || lane == 0) {
        event->ignore();
        return;
    }
    if (!noteAuthoringEnabled_) {
        // Without a tool the view only seeks.
        if (event->button() == Qt::LeftButton) {
            emit navigateRequested(secondForY(event->position().y()));
        }
        event->accept();
        return;
    }
    double noteSecond = 0.0;
    int noteLane = 0;
    pressOnNote_ = hitNote(event->position(), &noteSecond, &noteLane);
    button_ = event->button();
    pressPoint_ = event->position();
    currentPoint_ = event->position();
    pressRawSecond_ = secondForY(event->position().y());
    pressSecond_ = pressOnNote_ ? noteSecond : pressRawSecond_;
    pressLane_ = lane;
    if (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::ShiftModifier)) {
        drag_ = Drag::Box;
    } else if (event->button() == Qt::LeftButton && pressOnNote_) {
        drag_ = Drag::MoveNote;
    } else {
        drag_ = Drag::Place;
    }
    update();
    event->accept();
}

void TimelineLaneEditorItem::mouseMoveEvent(QMouseEvent* event)
{
    if (drag_ == Drag::None) {
        event->ignore();
        return;
    }
    currentPoint_ = event->position();
    const QPointF delta = currentPoint_ - pressPoint_;
    if (drag_ == Drag::Place && button_ == Qt::LeftButton
        && qAbs(delta.x()) + qAbs(delta.y()) > kDragThreshold
        && !(authoringSpanEnabled_ && laneAtX(currentPoint_.x()) == pressLane_)) {
        drag_ = Drag::Box;
    }
    update();
    event->accept();
}

void TimelineLaneEditorItem::mouseReleaseEvent(QMouseEvent* event)
{
    if (drag_ == Drag::None || event->button() != button_) {
        event->ignore();
        return;
    }
    finishPress(event->position());
    event->accept();
}

void TimelineLaneEditorItem::mouseUngrabEvent()
{
    // A press that loses its grab never finishes; drop it instead of leaving
    // the drop cell hanging.
    if (drag_ != Drag::None) {
        drag_ = Drag::None;
        button_ = Qt::NoButton;
        update();
    }
}

void TimelineLaneEditorItem::finishPress(const QPointF& point)
{
    const Drag drag = drag_;
    drag_ = Drag::None;
    const bool secondary = button_ == Qt::RightButton;
    button_ = Qt::NoButton;
    const QPointF delta = point - pressPoint_;
    const bool moved = qAbs(delta.x()) + qAbs(delta.y()) > kDragThreshold;
    const double releaseSecond = secondForY(point.y());
    const int releaseLane = qMax(1, laneAtX(qBound(gutterWidth(), point.x(), lanesRight() - 1.0)));

    switch (drag) {
    case Drag::Box:
        selectionValid_ = moved;
        if (moved) {
            selectionStartSecond_ = qMin(pressRawSecond_, releaseSecond);
            selectionEndSecond_ = qMax(pressRawSecond_, releaseSecond);
            selectionFirstLane_ = pressLane_;
            selectionLastLane_ = releaseLane;
            // Columns run 8..1 then T, so the boxed lanes go out as a bit
            // mask (endLane -1) rather than a range of lane numbers.
            const qreal left = qMin(xForLane(pressLane_), xForLane(releaseLane));
            const qreal right = qMax(xForLane(pressLane_), xForLane(releaseLane));
            int mask = 0;
            for (int lane = 1; lane <= kLaneCount; ++lane) {
                if (xForLane(lane) >= left && xForLane(lane) <= right) {
                    mask |= 1 << lane;
                }
            }
            emit authoringGesture(3, false, selectionStartSecond_, selectionEndSecond_, mask, -1, false);
        }
        break;
    case Drag::MoveNote:
        if (moved) {
            // Keep the grab offset: the note moves by as much as the pointer.
            emit authoringGesture(2, false, pressSecond_, pressSecond_ + (releaseSecond - pressRawSecond_),
                                  pressLane_, releaseLane, true);
        } else {
            emit authoringGesture(0, false, pressSecond_, pressSecond_, pressLane_, pressLane_, true);
        }
        break;
    case Drag::Place:
        if (!secondary) {
            selectionValid_ = false;
        }
        if (!secondary && moved && releaseLane == pressLane_) {
            emit authoringGesture(1, false, pressSecond_, releaseSecond, pressLane_, releaseLane, false);
        } else if (!moved) {
            emit authoringGesture(0, secondary, pressSecond_, pressSecond_, pressLane_, pressLane_, pressOnNote_);
        }
        break;
    case Drag::None:
        break;
    }
    update();
}

void TimelineLaneEditorItem::wheelEvent(QWheelEvent* event)
{
    const QPoint angle = event->angleDelta();
    const int delta = angle.y() != 0 ? angle.y() : event->pixelDelta().y();
    if (delta == 0) {
        event->ignore();
        return;
    }
    if (event->modifiers().testFlag(Qt::ControlModifier) || event->modifiers().testFlag(Qt::MetaModifier)) {
        setPixelsPerSecond(pixelsPerSecond_ * (delta > 0 ? 1.15 : 1.0 / 1.15));
        event->accept();
        return;
    }
    // Scrolling up moves forward in time, one beat of the chart per notch.
    const double now = playheadSecond();
    const QVector<double> beats = beatSeconds(now - 60.0, now + 60.0);
    double target = now;
    if (delta > 0) {
        double best = std::numeric_limits<double>::max();
        for (const double beat : beats) {
            if (beat > now + kBeatEpsilon && beat < best) {
                best = beat;
            }
        }
        target = best < std::numeric_limits<double>::max() ? best : now;
    } else {
        double best = -std::numeric_limits<double>::max();
        for (const double beat : beats) {
            if (beat < now - kBeatEpsilon && beat > best) {
                best = beat;
            }
        }
        target = best > -std::numeric_limits<double>::max() ? best : now;
    }
    if (target != now) {
        emit navigateRequested(target);
    }
    event->accept();
}

void TimelineLaneEditorItem::keyPressEvent(QKeyEvent* event)
{
    if (event->modifiers() != Qt::NoModifier) {
        event->ignore();
        return;
    }
    switch (event->key()) {
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        emit authoringDeleteRequested();
        break;
    case Qt::Key_Escape:
        clearAuthoringSelection();
        break;
    default:
        event->ignore();
        return;
    }
    event->accept();
}
