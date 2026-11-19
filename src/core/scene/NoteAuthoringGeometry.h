#pragma once

#include <QPointF>
#include <QString>
#include <QVector>

namespace miacode::preview::scene {

// Graphical charting tools. The integer values are the QML-facing contract.
enum class NoteAuthoringTool {
    None = 0,
    Tap = 1,
    Hold = 2,
    Slide = 3,
    Touch = 4,
    TouchHold = 5,
};

// A finished press-drag-release on the preview while a tool is active.
struct NoteAuthoringGesture {
    NoteAuthoringTool tool = NoteAuthoringTool::None;
    // Right button: remove instead of author.
    bool secondary = false;
    QString startTarget;
    QString endTarget;
    // Slide tool only: the drawn path in logical canvas coordinates.
    QVector<QPointF> path;
};

NoteAuthoringTool noteAuthoringToolFromInt(int value);
bool noteAuthoringToolUsesLanes(NoteAuthoringTool tool);

// Logical-canvas position of a lane's button on the judgement ring.
QPointF laneButtonLogicalPoint(int lane);
// The lane whose 45-degree sector holds a point near the ring; 0 near the
// centre or outside the playfield.
int laneAtLogicalPoint(const QPointF& logicalPoint);

// "1".."8" for lane tools, a touch pad token for touch tools, empty for none.
QString noteAuthoringTargetAtLogicalPoint(NoteAuthoringTool tool, const QPointF& logicalPoint);
QPointF noteAuthoringTargetLogicalPoint(const QString& target);
qreal noteAuthoringTargetRadiusLogical(const QString& target);

// The slide shape from startLane to endLane whose reference path follows a
// drawn logical-canvas path most closely, as simai text such as "1-5" or
// "3pp7". Empty when no shape joins the two lanes.
QString matchSlideShape(int startLane, int endLane, const QVector<QPointF>& logicalPath);

}  // namespace miacode::preview::scene
