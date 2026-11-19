#include "core/scene/NoteAuthoringGeometry.h"

#include "core/chart/SlideReferenceData.h"
#include "core/scene/PreviewSceneConstants.h"
#include "core/scene/PreviewSceneMath.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QtMath>

#include <limits>

namespace miacode::preview::scene {

namespace {

constexpr int kSlideMatchSamples = 32;
// Lane sectors start a little inside the A pads and end past the playfield.
constexpr qreal kLaneSectorInnerRadius = kLogicalDistanceEdge * 0.35;
constexpr qreal kLaneSectorOuterRadius = kLogicalDistanceEdge * 1.3;
constexpr qreal kLaneTargetRadius = 34.0;

// Resamples a polyline to `count` points evenly spaced by arc length.
QVector<QPointF> resampled(const QVector<QPointF>& points, int count)
{
    QVector<QPointF> result;
    if (points.isEmpty() || count < 2) {
        return result;
    }
    QVector<qreal> cumulative(points.size(), 0.0);
    for (int index = 1; index < points.size(); ++index) {
        const QPointF delta = points.at(index) - points.at(index - 1);
        cumulative[index] = cumulative.at(index - 1) + qSqrt(QPointF::dotProduct(delta, delta));
    }
    const qreal total = cumulative.constLast();
    result.reserve(count);
    if (total <= 0.0) {
        result.fill(points.constFirst(), count);
        return result;
    }
    int segment = 0;
    for (int sample = 0; sample < count; ++sample) {
        const qreal distance = total * sample / (count - 1);
        while (segment < points.size() - 2 && cumulative.at(segment + 1) < distance) {
            ++segment;
        }
        const qreal from = cumulative.at(segment);
        const qreal to = cumulative.at(segment + 1);
        const qreal t = to > from ? qBound<qreal>(0.0, (distance - from) / (to - from), 1.0) : 0.0;
        result.append(points.at(segment) + (points.at(segment + 1) - points.at(segment)) * t);
    }
    return result;
}

struct SlideShapeCandidate {
    QString key;
    QVector<QPointF> path;
};

// Reference shapes grouped by (start, end), with paths moved from the
// centre-origin reference space onto the logical canvas.
const QHash<int, QVector<SlideShapeCandidate>>& slideShapeCandidates()
{
    static const QHash<int, QVector<SlideShapeCandidate>> candidates = [] {
        QHash<int, QVector<SlideShapeCandidate>> result;
        const QJsonObject slides = miacode::slide_reference::root().value("slides").toObject();
        const QPointF center(kLogicalCanvasCenter, kLogicalCanvasCenter);
        for (auto it = slides.constBegin(); it != slides.constEnd(); ++it) {
            const QJsonObject entry = it.value().toObject();
            const int start = entry.value("start").toInt();
            const int end = entry.value("end").toInt();
            QVector<QPointF> points;
            for (const QJsonValue& sample : entry.value("samples").toArray()) {
                const QJsonObject point = sample.toObject();
                points.append(center + QPointF(point.value("x").toDouble(), point.value("y").toDouble()));
            }
            if (start < 1 || start > 8 || end < 1 || end > 8 || points.size() < 2) {
                continue;
            }
            result[start * 10 + end].append({it.key(), resampled(points, kSlideMatchSamples)});
        }
        return result;
    }();
    return candidates;
}

}  // namespace

NoteAuthoringTool noteAuthoringToolFromInt(int value)
{
    switch (value) {
    case int(NoteAuthoringTool::Tap): return NoteAuthoringTool::Tap;
    case int(NoteAuthoringTool::Hold): return NoteAuthoringTool::Hold;
    case int(NoteAuthoringTool::Slide): return NoteAuthoringTool::Slide;
    case int(NoteAuthoringTool::Touch): return NoteAuthoringTool::Touch;
    case int(NoteAuthoringTool::TouchHold): return NoteAuthoringTool::TouchHold;
    default: return NoteAuthoringTool::None;
    }
}

bool noteAuthoringToolUsesLanes(NoteAuthoringTool tool)
{
    return tool == NoteAuthoringTool::Tap
        || tool == NoteAuthoringTool::Hold
        || tool == NoteAuthoringTool::Slide;
}

QPointF laneButtonLogicalPoint(int lane)
{
    return QPointF(kLogicalCanvasCenter, kLogicalCanvasCenter) + laneUnitVector(lane) * kLogicalDistanceEdge;
}

int laneAtLogicalPoint(const QPointF& logicalPoint)
{
    const QPointF offset = logicalPoint - QPointF(kLogicalCanvasCenter, kLogicalCanvasCenter);
    const qreal radius = qSqrt(QPointF::dotProduct(offset, offset));
    if (radius < kLaneSectorInnerRadius || radius > kLaneSectorOuterRadius) {
        return 0;
    }
    const qreal degrees = qRadiansToDegrees(qAtan2(offset.y(), offset.x()));
    const int step = qRound((degrees - kLaneUnitVectorBaseDegrees) / kLaneAngleStepDegrees);
    return wrappedLane(step + 1);
}

QString noteAuthoringTargetAtLogicalPoint(NoteAuthoringTool tool, const QPointF& logicalPoint)
{
    if (tool == NoteAuthoringTool::None) {
        return QString();
    }
    if (noteAuthoringToolUsesLanes(tool)) {
        const int lane = laneAtLogicalPoint(logicalPoint);
        return lane > 0 ? QString::number(lane) : QString();
    }
    return touchPadTokenAtLogicalPoint(logicalPoint);
}

QPointF noteAuthoringTargetLogicalPoint(const QString& target)
{
    const QString normalized = target.trimmed().toUpper();
    if (normalized.size() == 1 && normalized.at(0).isDigit()) {
        return laneButtonLogicalPoint(normalized.at(0).digitValue());
    }
    return touchPadLogicalPoint(normalized);
}

qreal noteAuthoringTargetRadiusLogical(const QString& target)
{
    const QString normalized = target.trimmed().toUpper();
    if (normalized.size() == 1 && normalized.at(0).isDigit()) {
        return kLaneTargetRadius;
    }
    return normalized == QLatin1String("C") ? kTouchPadCenterHitRadiusLogical : kTouchPadHitRadiusLogical;
}

QString matchSlideShape(int startLane, int endLane, const QVector<QPointF>& logicalPath)
{
    const auto candidates = slideShapeCandidates().value(startLane * 10 + endLane);
    if (candidates.isEmpty()) {
        return QString();
    }
    // The press and release land anywhere in a lane's sector; pin the drawn
    // path to the two buttons so only its course decides the shape.
    QVector<QPointF> drawn;
    drawn.reserve(logicalPath.size() + 2);
    drawn.append(laneButtonLogicalPoint(startLane));
    drawn.append(logicalPath);
    drawn.append(laneButtonLogicalPoint(endLane));
    const QVector<QPointF> samples = resampled(drawn, kSlideMatchSamples);

    QString bestKey;
    qreal bestScore = std::numeric_limits<qreal>::max();
    for (const SlideShapeCandidate& candidate : candidates) {
        qreal score = 0.0;
        for (int index = 0; index < kSlideMatchSamples; ++index) {
            const QPointF delta = samples.at(index) - candidate.path.at(index);
            score += qSqrt(QPointF::dotProduct(delta, delta));
        }
        if (score < bestScore) {
            bestScore = score;
            bestKey = candidate.key;
        }
    }
    return bestKey;
}

}  // namespace miacode::preview::scene
