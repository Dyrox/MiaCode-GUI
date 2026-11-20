#pragma once

#include <QPointF>
#include <QString>
#include <QStringList>
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

// The shape key whose path is `key`'s walked backwards: "1>5" -> "5<1".
QString reversedSlideShape(const QString& key);

// The slide shape from startLane to endLane whose reference path follows a
// drawn logical-canvas path most closely, as simai text such as "1-5" or
// "3pp7". Empty when no shape joins the two lanes.
QString matchSlideShape(int startLane, int endLane, const QVector<QPointF>& logicalPath);

// The reference course of a shape key ("1>5", or a chain such as "1-4q7")
// on the logical canvas; a wifi key runs straight. Empty for unknown keys.
QVector<QPointF> slideShapeLogicalPath(const QString& key);

// One arrow of a slide track as the preview draws it: a logical-canvas
// point, the sprite's rotation, and for wifi slides its fan image index
// (-1 for the plain slide track image).
struct NoteAuthoringGuideArrow {
    QPointF point;
    qreal rotationDegrees = 0.0;
    int wifiImage = -1;

    friend bool operator==(const NoteAuthoringGuideArrow&, const NoteAuthoringGuideArrow&) = default;
};

// The track arrows of a shape key ("1>5", a chain "1-4q7", or "1w5") from
// the slide reference data, segment after segment.
QVector<NoteAuthoringGuideArrow> slideShapeArrows(const QString& key);

// Every touch pad token: "C", then A1..A8, B1..B8, D1..D8, E1..E8.
QStringList touchPadTokens();

// Every slide shape from startLane to endLane as simai text ("1-5", "1>5",
// ...), with the wifi slide when the lanes face each other.
QStringList slideShapeKeys(int startLane, int endLane);

}  // namespace miacode::preview::scene
