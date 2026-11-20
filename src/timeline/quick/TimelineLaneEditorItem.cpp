#include "timeline/quick/TimelineLaneEditorItem.h"

#include "core/video/PreviewSkinConfig.h"
#include "timeline/TimelineGrid.h"
#include "timeline/TimelineRenderData.h"
#include "timeline/quick/TimelineQuickStateBridge.h"

#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QQuickWindow>
#include <QPainterPath>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <array>
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

namespace {

// The lane of each of the eight columns, left to right, for laneOrder.
const std::array<int, 8>& laneColumns(int order)
{
    static const std::array<int, 8> kOrders[] = {
        {1, 2, 3, 4, 5, 6, 7, 8},
        {8, 7, 6, 5, 4, 3, 2, 1},
        {5, 6, 7, 8, 1, 2, 3, 4},
    };
    return kOrders[qBound(0, order, 2)];
}

} // namespace

// Lane columns in the chosen order, then the touch column.
qreal TimelineLaneEditorItem::xForLane(int lane) const
{
    int column = 8;
    if (lane >= 1 && lane <= 8) {
        const auto& columns = laneColumns(laneOrder_);
        column = int(std::find(columns.begin(), columns.end(), lane) - columns.begin());
    }
    return gutterWidth() + (column + 0.5) * columnWidth();
}

int TimelineLaneEditorItem::laneAtX(qreal x) const
{
    if (x < gutterWidth() || x >= lanesRight()) {
        return 0;
    }
    const int column = static_cast<int>(std::floor((x - gutterWidth()) / columnWidth()));
    return column < 8 ? laneColumns(laneOrder_)[column] : kTouchLane;
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
            const double startSecond = slideLike && note.slideTraceSecondOffset >= 0.0
                ? timelineRenderAbsoluteSecond(line, note.slideTraceSecondOffset) : second;
            visible.start = QPointF(visible.head.x(), yForSecond(qBound(second, startSecond, endSecond)));
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

bool TimelineLaneEditorItem::paintSkinSlideTrack(QPainter* painter, const VisibleNote& note, qreal radius) const
{
    // As classic editors draw a slide: the skin's track arrows in a row from
    // the start mark to the end, pointing the way the star goes.
    const auto kind = TimelineRenderNoteKind(note.kind);
    const QString type = kind == TimelineRenderNoteKind::Wifi
        ? (hasFlag(note.flags, TimelineRenderFlagTrackBreak) ? QStringLiteral("wifi_track_break")
           : hasFlag(note.flags, TimelineRenderFlagSlideEach) ? QStringLiteral("wifi_track_each")
                                                                : QStringLiteral("wifi_track"))
        : (hasFlag(note.flags, TimelineRenderFlagTrackBreak) ? QStringLiteral("slide_track_break")
           : hasFlag(note.flags, TimelineRenderFlagSlideEach) ? QStringLiteral("slide_track_each")
                                                                : QStringLiteral("slide_track"));
    const QPixmap arrow = skinIcon(type, radius * 2.1);
    const QPointF delta = note.tail - note.start;
    const qreal length = std::hypot(delta.x(), delta.y());
    if (arrow.isNull() || length < radius) {
        return false;
    }
    const QSizeF size = arrow.deviceIndependentSize();
    // The slide sprite points left and the wifi one down; turn either along
    // the segment.
    const qreal native = kind == TimelineRenderNoteKind::Wifi ? 90.0 : 180.0;
    const qreal angle = qRadiansToDegrees(std::atan2(delta.y(), delta.x())) - native;
    const qreal spacing = qMax<qreal>(4.0, size.width() * 0.85);
    const int count = qMax(1, int(std::floor(length / spacing)));
    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (int index = 0; index < count; ++index) {
        const QPointF center = note.start + delta * ((index + 0.5) / count);
        painter->save();
        painter->translate(center);
        painter->rotate(angle);
        painter->drawPixmap(QPointF(-size.width() / 2.0, -size.height() / 2.0), arrow);
        painter->restore();
    }
    painter->restore();
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
        // Past the chart, bars of its last length run on for charting there.
        const double bar = measures.isEmpty() ? 0.0
            : miacode::timeline::gridStepSecondsAt(stateBridge_->renderSnapshot(), measures.constLast(), 1);
        if (bar > 0.0) {
            const int first = qMax(1, int(std::ceil((bottom - measures.constLast()) / bar)));
            for (int extra = first; measures.constLast() + extra * bar <= top && extra < first + 200; ++extra) {
                const qreal y = yForSecond(measures.constLast() + extra * bar);
                painter->setPen(QPen(kMeasureLineColor, 1.5));
                painter->drawLine(QPointF(0, y), QPointF(lanesRight(), y));
                painter->setPen(kLabelColor);
                painter->drawText(QRectF(0, y + 2, gutterWidth() - 6, 14), Qt::AlignRight | Qt::AlignTop,
                                  QString::number(measures.size() + extra));
            }
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
            // As in classic editors: the star waits in its lane, then its
            // course runs from the start mark to the end.
            if (note.start.y() < note.head.y() - 1.0) {
                QColor wait = color;
                wait.setAlpha(140);
                painter->setPen(QPen(wait, 1.5, Qt::DotLine));
                painter->drawLine(note.head, note.start);
                painter->setPen(QPen(color, 2.0));
                painter->drawLine(QPointF(note.start.x() - radius * 0.7, note.start.y()),
                                  QPointF(note.start.x() + radius * 0.7, note.start.y()));
            }
            if (!paintSkinSlideTrack(painter, note, radius)) {
                painter->setPen(QPen(color, 2.0, Qt::DashLine));
                painter->drawLine(note.start, note.tail);
            }
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
    // Selected notes carry a ring, the selection's only mark once made.
    if (selectionValid_) {
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(QColor(150, 205, 255), 2.5));
        for (const VisibleNote& note : notes) {
            if (inSelection(note.second, note.lane)) {
                painter->drawEllipse(note.head, radius * 1.45, radius * 1.45);
            }
        }
    }

    paintPlacement(painter, radius);
    paintPasteGhost(painter, radius);

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

    // The box while dragging one; once made, the selected notes' rings
    // show the selection.
    QRectF box;
    if (drag_ == Drag::Box) {
        box = QRectF(pressPoint_, currentPoint_).normalized();
    }
    if (!box.isEmpty()) {
        QColor fill = kAccentColor;
        fill.setAlpha(40);
        painter->setPen(QPen(kAccentColor, 1));
        painter->setBrush(fill);
        painter->drawRect(box);
    }
    if (drag_ == Drag::ResizeHold) {
        const double tail = qMax(snappedSecond(secondForY(currentPoint_.y())), pressSecond_);
        const qreal x = xForLane(pressLane_);
        QColor fill = kAccentColor;
        fill.setAlpha(60);
        painter->setPen(QPen(kAccentColor, 2.0, Qt::DashLine));
        painter->setBrush(fill);
        painter->drawRoundedRect(QRectF(x - radius * 0.8, yForSecond(tail) - radius * 0.6, radius * 1.6,
                                        yForSecond(pressSecond_) - yForSecond(tail) + radius * 1.2),
                                 radius * 0.8, radius * 0.8);
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

double TimelineLaneEditorItem::snappedSecond(double second) const
{
    if (stateBridge_ == nullptr) {
        return second;
    }
    if (gridDivision_ > 0) {
        return miacode::timeline::snapSecondToGrid(stateBridge_->renderSnapshot(), second, gridDivision_);
    }
    double best = second;
    double bestDistance = std::numeric_limits<double>::max();
    for (const double beat : beatSeconds(second - 4.0, second + 4.0)) {
        if (qAbs(beat - second) < bestDistance) {
            bestDistance = qAbs(beat - second);
            best = beat;
        }
    }
    // Past the chart's commas, quarters of the bar.
    const TimelineRenderSnapshot& snapshot = stateBridge_->renderSnapshot();
    const double quarter = miacode::timeline::gridStepSecondsAt(snapshot, second, 4);
    if (quarter > 0.0 && bestDistance > quarter * 0.5) {
        return miacode::timeline::snapSecondToGrid(snapshot, second, 4);
    }
    return best;
}

double TimelineLaneEditorItem::adjacentBeat(double second, int direction) const
{
    double best = direction > 0 ? std::numeric_limits<double>::max() : -std::numeric_limits<double>::max();
    const auto consider = [&](double beat) {
        if (direction > 0 ? (beat > second + kBeatEpsilon && beat < best)
                          : (beat < second - kBeatEpsilon && beat > best)) {
            best = beat;
        }
    };
    for (const double beat : beatSeconds(second - 60.0, second + 60.0)) {
        consider(beat);
    }
    if (stateBridge_ != nullptr) {
        const TimelineRenderSnapshot& snapshot = stateBridge_->renderSnapshot();
        const double quarter = miacode::timeline::gridStepSecondsAt(snapshot, second, 4);
        if (quarter > 0.0) {
            consider(miacode::timeline::snapSecondToGrid(snapshot, second + direction * quarter, 4));
        }
    }
    return qAbs(best) < std::numeric_limits<double>::max() ? best : second;
}

double TimelineLaneEditorItem::bpmAt(double second) const
{
    double bpm = 120.0;
    for (const TempoMark& mark : tempoMarks()) {
        if (mark.second <= second + kBeatEpsilon) {
            bpm = mark.bpm;
        }
    }
    return bpm;
}

void TimelineLaneEditorItem::setPlacementMode(int mode)
{
    if (placementMode_ == mode) {
        return;
    }
    placementMode_ = mode;
    cancelPlacement();
    emit appearanceChanged();
}

void TimelineLaneEditorItem::setPasteMode(bool enabled)
{
    if (pasteMode_ == enabled) {
        return;
    }
    pasteMode_ = enabled;
    if (enabled) {
        cancelPlacement();
    }
    emit placementChanged();
    update();
}

void TimelineLaneEditorItem::paintPasteGhost(QPainter* painter, qreal radius) const
{
    if (!pasteMode_ || hoverPoint_.x() < 0.0 || stateBridge_ == nullptr) {
        return;
    }
    // Bars become seconds at the paste point's tempo; enough for a preview.
    const double second = snappedSecond(secondForY(hoverPoint_.y()));
    const double bar = miacode::timeline::gridStepSecondsAt(stateBridge_->renderSnapshot(), second, 1);
    QColor fill = kAccentColor;
    fill.setAlpha(90);
    painter->setPen(QPen(kAccentColor, 1.5, Qt::DashLine));
    painter->setBrush(fill);
    for (const QVariant& value : pasteLayout_) {
        const QVariantMap entry = value.toMap();
        const int row = entry.value(QStringLiteral("row")).toInt();
        if (row < 1 || row > kLaneCount) {
            continue;
        }
        const QPointF center(xForLane(row), yForSecond(second + entry.value(QStringLiteral("bars")).toDouble() * bar));
        if (row == kTouchLane) {
            painter->drawRect(QRectF(center.x() - radius * 0.85, center.y() - radius * 0.85, radius * 1.7, radius * 1.7));
        } else {
            painter->drawEllipse(center, radius, radius);
        }
    }
    painter->setPen(QPen(kAccentColor, 1.0));
    painter->drawLine(QPointF(gutterWidth(), yForSecond(second)), QPointF(lanesRight(), yForSecond(second)));
}

void TimelineLaneEditorItem::cancelPlacement()
{
    placeReplaces_ = false;
    if (placing_ == Placing::None) {
        return;
    }
    placing_ = Placing::None;
    emit placementChanged();
    update();
}

void TimelineLaneEditorItem::commitSlide(const QString& shape)
{
    if (placing_ != Placing::SlideShape) {
        return;
    }
    if (!shape.isEmpty()) {
        emit authoringSlide(placeSecond_, placeEndSecond_, placeLane_, placeEndLane_, shape, placeReplaces_);
    }
    cancelPlacement();
}

bool TimelineLaneEditorItem::pressPlacement(QMouseEvent* event, int lane)
{
    const double second = snappedSecond(secondForY(event->position().y()));
    if (placing_ != Placing::None) {
        if (event->button() == Qt::RightButton) {
            cancelPlacement();
            return true;
        }
        if (placing_ == Placing::Hold) {
            // The tail: any column, the hold stays in its own.
            emit authoringGesture(1, false, placeSecond_, second, placeLane_, placeLane_, false);
            if (placeLane_ == kTouchLane) {
                // A touch hold goes back to its picked time for more touches.
                placing_ = Placing::TouchTime;
                emit placementChanged();
                update();
            } else {
                cancelPlacement();
            }
        } else if (placing_ == Placing::Slide && lane >= 1 && lane <= 8 && second > placeSecond_) {
            placeEndLane_ = lane;
            placeEndSecond_ = second;
            placing_ = Placing::SlideShape;
            emit placementChanged();
            emit slideShapeRequested(placeLane_, placeEndLane_, event->position());
            update();
        }
        return true;
    }
    // The head: a hold in any column, a slide on a button lane.
    if (event->button() != Qt::LeftButton || event->modifiers() != Qt::NoModifier
        || (placementMode_ == 2 && lane > 8)) {
        return false;
    }
    placing_ = placementMode_ == 1 ? Placing::Hold : Placing::Slide;
    placeLane_ = lane;
    placeSecond_ = second;
    emit placementChanged();
    update();
    return true;
}

void TimelineLaneEditorItem::pickTouchTime(double second)
{
    placing_ = Placing::TouchTime;
    placeReplaces_ = false;
    placeLane_ = kTouchLane;
    placeSecond_ = second;
    emit placementChanged();
    emit navigateRequested(second);
    update();
}

void TimelineLaneEditorItem::beginTouchHold()
{
    if (placing_ != Placing::TouchTime) {
        return;
    }
    placing_ = Placing::Hold;
    emit placementChanged();
    update();
}

void TimelineLaneEditorItem::advanceTouchTime()
{
    if (placing_ != Placing::TouchTime || stateBridge_ == nullptr) {
        return;
    }
    double next = placeSecond_;
    if (gridDivision_ > 0) {
        const double step = miacode::timeline::gridStepSecondsAt(stateBridge_->renderSnapshot(), placeSecond_,
                                                                 gridDivision_);
        next = snappedSecond(placeSecond_ + step);
    } else {
        next = adjacentBeat(placeSecond_, 1);
    }
    if (next > placeSecond_) {
        pickTouchTime(next);
    }
}

void TimelineLaneEditorItem::paintPlacement(QPainter* painter, qreal radius) const
{
    if (placing_ == Placing::None) {
        return;
    }
    const QPointF head(xForLane(placeLane_), yForSecond(placeSecond_));
    if (placing_ == Placing::TouchTime) {
        // The picked time: a line across the lanes and a frame in T.
        QColor line = kAccentColor;
        line.setAlpha(120);
        painter->setPen(QPen(line, 1.0, Qt::DashLine));
        painter->drawLine(QPointF(gutterWidth(), head.y()), QPointF(lanesRight(), head.y()));
        QColor fill = kAccentColor;
        fill.setAlpha(60);
        painter->setPen(QPen(kAccentColor, 2.0));
        painter->setBrush(fill);
        const qreal half = columnWidth() * 0.42;
        painter->drawRoundedRect(QRectF(head.x() - half, head.y() - radius, half * 2, radius * 2), 4.0, 4.0);
        return;
    }
    const bool hovering = hoverPoint_.x() >= 0.0;
    const double hoverSecond = hovering ? snappedSecond(secondForY(hoverPoint_.y())) : placeSecond_;
    if (placing_ == Placing::Hold) {
        const qreal top = qMin(head.y(), yForSecond(hoverSecond));
        const qreal bottom = qMax(head.y(), yForSecond(hoverSecond));
        QColor fill = kTapColor;
        fill.setAlpha(90);
        painter->setPen(QPen(kTapColor, 2.0, Qt::DashLine));
        painter->setBrush(fill);
        painter->drawRoundedRect(QRectF(head.x() - radius * 0.8, top - radius * 0.6, radius * 1.6,
                                        bottom - top + radius * 1.2), radius * 0.8, radius * 0.8);
        return;
    }
    // Slide: the star at its head, the beat it waits, and its way to the end.
    const double beat = stateBridge_ != nullptr
        ? miacode::timeline::gridStepSecondsAt(stateBridge_->renderSnapshot(), placeSecond_, 4) : 0.0;
    const qreal startY = yForSecond(placeSecond_ + beat);
    painter->setPen(QPen(kSlideColor, 1.0, Qt::DotLine));
    painter->drawLine(QPointF(head.x() - columnWidth() / 2, startY), QPointF(head.x() + columnWidth() / 2, startY));
    const QPointF end = placing_ == Placing::SlideShape
        ? QPointF(xForLane(placeEndLane_), yForSecond(placeEndSecond_))
        : QPointF(xForLane(qMax(1, qMin(8, laneAtX(hoverPoint_.x())))), yForSecond(hoverSecond));
    if (placing_ == Placing::SlideShape || (hovering && laneAtX(hoverPoint_.x()) >= 1 && laneAtX(hoverPoint_.x()) <= 8)) {
        painter->setPen(QPen(kSlideColor, 2.0, Qt::DashLine));
        painter->drawLine(QPointF(head.x(), startY), end);
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(end, radius * 0.6, radius * 0.6);
    }
    painter->setPen(QPen(kSlideEdgeColor, 2.0));
    painter->setBrush(kSlideColor);
    painter->drawPolygon(starPolygon(head, radius * 1.2));
}

bool TimelineLaneEditorItem::hitHoldTail(const QPointF& point, double* second, int* lane) const
{
    const int pointLane = laneAtX(point.x());
    const qreal reach = qMax<qreal>(6.0, qBound<qreal>(6.0, columnWidth() * 0.32, 14.0) * 0.8);
    for (const VisibleNote& note : visibleNotes()) {
        const auto kind = TimelineRenderNoteKind(note.kind);
        const bool hold = kind == TimelineRenderNoteKind::Hold || kind == TimelineRenderNoteKind::TouchHold;
        // Only a tail clear of its own head, or the press would be ambiguous.
        if (hold && note.lane == pointLane && note.head.y() - note.tail.y() > reach * 2
            && qAbs(note.tail.y() - point.y()) <= reach) {
            *second = note.second;
            *lane = note.lane;
            return true;
        }
    }
    return false;
}

bool TimelineLaneEditorItem::hitNote(const QPointF& point, double* second, int* lane, int* kind) const
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
            if (kind != nullptr) {
                *kind = note.kind;
            }
            found = true;
        }
    }
    return found;
}

void TimelineLaneEditorItem::hoverMoveEvent(QHoverEvent* event)
{
    hoverPoint_ = event->position();
    if (placing_ != Placing::None || pasteMode_) {
        update();
    }
    const int lane = event->position().y() < lanesBottom() ? laneAtX(event->position().x()) : 0;
    if (lane != hoveredLane_) {
        hoveredLane_ = lane;
        emit hoveredLaneChanged();
        update();
    }
}

void TimelineLaneEditorItem::hoverLeaveEvent(QHoverEvent*)
{
    hoverPoint_ = QPointF(-1.0, -1.0);
    if (hoveredLane_ != 0) {
        hoveredLane_ = 0;
        emit hoveredLaneChanged();
        update();
    }
}

void TimelineLaneEditorItem::mousePressEvent(QMouseEvent* event)
{
    forceActiveFocus(Qt::MouseFocusReason);
    const int lane = laneAtX(event->position().x());
    if (noteAuthoringEnabled_ && event->button() == Qt::LeftButton && event->position().x() >= lanesRight()
        && event->position().y() <= lanesBottom()) {
        // The BPM column: a BPM change at the clicked beat.
        const double second = snappedSecond(secondForY(event->position().y()));
        emit bpmEditRequested(second, bpmAt(second), event->position());
        event->accept();
        return;
    }
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
    if (pasteMode_) {
        // A left click pastes at the grid point; anything else leaves.
        if (event->button() == Qt::LeftButton) {
            emit authoringGesture(6, false, snappedSecond(secondForY(event->position().y())), 0.0, lane, lane, false);
        }
        setPasteMode(false);
        event->accept();
        return;
    }
    double noteSecond = 0.0;
    int noteLane = 0;
    if (event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier && placing_ == Placing::None
        && hitHoldTail(event->position(), &noteSecond, &noteLane)) {
        button_ = Qt::LeftButton;
        drag_ = Drag::ResizeHold;
        pressPoint_ = event->position();
        currentPoint_ = event->position();
        pressSecond_ = noteSecond;
        pressLane_ = noteLane;
        update();
        event->accept();
        return;
    }
    pressNoteKind_ = 0;
    pressOnNote_ = hitNote(event->position(), &noteSecond, &noteLane, &pressNoteKind_);
    // The T column picks times for touches; anywhere else ends the pick.
    const bool touchColumn = lane == kTouchLane;
    if (placing_ == Placing::TouchTime && !touchColumn) {
        cancelPlacement();
    }
    const bool placing = placing_ == Placing::Hold || placing_ == Placing::Slide || placing_ == Placing::SlideShape;
    // Holds and slides are placed click by click; a press on a note still
    // picks it up, unless a placement is under way.
    if (placementMode_ != 0 && (placing || (!pressOnNote_ && !touchColumn))
        && pressPlacement(event, lane)) {
        event->accept();
        return;
    }
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

bool TimelineLaneEditorItem::inSelection(double second, int lane) const
{
    if (!selectionValid_ || second < selectionStartSecond_ - kBeatEpsilon || second > selectionEndSecond_ + kBeatEpsilon) {
        return false;
    }
    const qreal left = qMin(xForLane(selectionFirstLane_), xForLane(selectionLastLane_));
    const qreal right = qMax(xForLane(selectionFirstLane_), xForLane(selectionLastLane_));
    return xForLane(lane) >= left - 0.5 && xForLane(lane) <= right + 0.5;
}

double TimelineLaneEditorItem::earliestSelectedSecond() const
{
    double earliest = std::numeric_limits<double>::max();
    if (stateBridge_ == nullptr) {
        return earliest;
    }
    for (const TimelineRenderLine& line : stateBridge_->renderSnapshot().lines) {
        if (line.endSecond < selectionStartSecond_ - 1.0 || line.startSecond > selectionEndSecond_ + 1.0) {
            continue;
        }
        for (const TimelineRenderNote& note : line.notes) {
            const double second = timelineRenderAbsoluteSecond(line, note.secondOffset);
            if (inSelection(second, qBound(1, note.lane, kLaneCount))) {
                earliest = qMin(earliest, second);
            }
        }
    }
    return earliest;
}

void TimelineLaneEditorItem::selectBox(double releaseSecond, int releaseLane)
{
    selectionValid_ = true;
    selectionStartSecond_ = qMin(pressRawSecond_, releaseSecond);
    selectionEndSecond_ = qMax(pressRawSecond_, releaseSecond);
    selectionFirstLane_ = pressLane_;
    selectionLastLane_ = releaseLane;
    // Columns run 8..1 then T, so the boxed lanes go out as a bit mask
    // (endLane -1) rather than a range of lane numbers.
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
    case Drag::ResizeHold:
        if (moved) {
            emit authoringGesture(5, false, pressSecond_, qMax(snappedSecond(releaseSecond), pressSecond_),
                                  pressLane_, pressLane_, true);
        }
        break;
    case Drag::Box:
        selectionValid_ = false;
        if (moved) {
            selectBox(releaseSecond, releaseLane);
        }
        break;
    case Drag::MoveNote:
        if (moved && inSelection(pressSecond_, pressLane_)) {
            // A selected note carries the whole selection: the earliest
            // selected note moves by as much as this one, on the grid.
            const double target = snappedSecond(pressSecond_ + (releaseSecond - pressRawSecond_));
            const double earliest = earliestSelectedSecond();
            if (earliest < std::numeric_limits<double>::max()) {
                emit authoringGesture(9, false, earliest, earliest + (target - pressSecond_), pressLane_,
                                      releaseLane, true);
            }
            selectionValid_ = false;
        } else if (moved) {
            // Keep the grab offset: the note moves by as much as the pointer.
            emit authoringGesture(2, false, pressSecond_, pressSecond_ + (releaseSecond - pressRawSecond_),
                                  pressLane_, releaseLane, true);
        } else if (pressLane_ == kTouchLane) {
            // Touches share the column: a click picks their time to edit on
            // the preview.
            pickTouchTime(pressSecond_);
        } else if (placementMode_ == 2 && (TimelineRenderNoteKind(pressNoteKind_) == TimelineRenderNoteKind::Slide
                                           || TimelineRenderNoteKind(pressNoteKind_) == TimelineRenderNoteKind::Wifi)) {
            // With the slide tool a click on a star re-places its slide: pick
            // a new end and shape, keeping the star where it is.
            placing_ = Placing::Slide;
            placeReplaces_ = true;
            placeLane_ = pressLane_;
            placeSecond_ = pressSecond_;
            emit placementChanged();
        } else {
            emit authoringGesture(0, false, pressSecond_, pressSecond_, pressLane_, pressLane_, true);
        }
        break;
    case Drag::Place:
        if (!secondary) {
            selectionValid_ = false;
        }
        if (!secondary && moved && authoringSpanEnabled_ && releaseLane == pressLane_) {
            emit authoringGesture(1, false, pressSecond_, releaseSecond, pressLane_, releaseLane, false);
        } else if (!secondary && moved) {
            // A drag whose moves never arrived still selects what it spans.
            selectBox(releaseSecond, releaseLane);
        } else if (!moved && pressLane_ == kTouchLane && !secondary) {
            pickTouchTime(snappedSecond(pressSecond_));
        } else if (!moved && pressLane_ == kTouchLane && !pressOnNote_) {
            cancelPlacement();
        } else if (!moved) {
            emit authoringGesture(0, secondary, pressSecond_, pressSecond_, pressLane_, pressLane_, pressOnNote_);
        }
        break;
    case Drag::None:
        break;
    }
    update();
}

void TimelineLaneEditorItem::scrollTo(double second)
{
    scrollTarget_ = qMax(0.0, second);
    scrollClock_.restart();
    emit navigateRequested(scrollTarget_);
}

void TimelineLaneEditorItem::zoomAround(qreal y, double factor)
{
    const double anchor = secondForY(y);
    const double before = pixelsPerSecond_;
    setPixelsPerSecond(pixelsPerSecond_ * factor);
    if (qFuzzyCompare(before, pixelsPerSecond_)) {
        return;
    }
    // y = judge - (t - now) * pps, so the anchor stays at y for this now.
    const bool burst = scrollClock_.isValid() && scrollClock_.elapsed() < 150;
    const double now = burst ? scrollTarget_ : playheadSecond();
    const double target = anchor - (judgeY() - y) / pixelsPerSecond_;
    if (qAbs(target - now) > 1e-4) {
        scrollTo(target);
    }
}

bool TimelineLaneEditorItem::event(QEvent* event)
{
    if (event->type() == QEvent::NativeGesture) {
        const auto* gesture = static_cast<QNativeGestureEvent*>(event);
        switch (gesture->gestureType()) {
        case Qt::ZoomNativeGesture:
            zoomAround(gesture->position().y(), 1.0 + gesture->value());
            break;
        case Qt::SmartZoomNativeGesture:
            // Two-finger double tap: twice as close, then back.
            if (smartZoomRestore_ > 0.0) {
                zoomAround(gesture->position().y(), smartZoomRestore_ / pixelsPerSecond_);
                smartZoomRestore_ = 0.0;
            } else {
                smartZoomRestore_ = pixelsPerSecond_;
                zoomAround(gesture->position().y(), 2.0);
            }
            break;
        default:
            break;
        }
        event->accept();
        return true;
    }
    return QQuickPaintedItem::event(event);
}

void TimelineLaneEditorItem::wheelEvent(QWheelEvent* event)
{
    const bool zoomModifier = event->modifiers().testFlag(Qt::ControlModifier)
        || event->modifiers().testFlag(Qt::MetaModifier);
    const QPoint pixels = event->pixelDelta();
    if (!pixels.isNull() && !zoomModifier) {
        // Trackpads and Magic Mouse scroll smoothly: the chart follows the
        // fingers, momentum included, without stepping to beats.
        if (pixels.y() != 0) {
            const bool burst = scrollClock_.isValid() && scrollClock_.elapsed() < 150;
            const double now = burst ? scrollTarget_ : playheadSecond();
            scrollTo(now + pixels.y() / pixelsPerSecond_);
        }
        event->accept();
        return;
    }
    const QPoint angle = event->angleDelta();
    const int delta = angle.y() != 0 ? angle.y() : event->pixelDelta().y();
    if (delta == 0) {
        event->ignore();
        return;
    }
    if (zoomModifier) {
        zoomAround(event->position().y(), delta > 0 ? 1.15 : 1.0 / 1.15);
        event->accept();
        return;
    }
    // Scrolling up moves forward in time, one beat per notch.
    const double now = playheadSecond();
    const double target = adjacentBeat(now, delta > 0 ? 1 : -1);
    if (target != now) {
        // A picked touch time rides along with the playhead.
        if (placing_ == Placing::TouchTime) {
            placeSecond_ = target;
            emit placementChanged();
        }
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
        setPasteMode(false);
        cancelPlacement();
        clearAuthoringSelection();
        break;
    default:
        event->ignore();
        return;
    }
    event->accept();
}
