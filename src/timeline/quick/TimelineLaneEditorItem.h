#pragma once

#include <QHash>
#include <QPixmap>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>
#include <QVector>

#include "timeline/TimelineNoteAssets.h"

class TimelineQuickStateBridge;

// Vertical lane editor for graphical charting, laid out like a classic
// maimai track editor: columns 8..1, a touch column and a BPM column, time
// running upward through a judgement line at the playhead. It reads the same
// render snapshot as the horizontal timeline and reports the same authoring
// gestures, so both views edit the chart text through one path.
class TimelineLaneEditorItem : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject* stateBridge READ stateBridgeObject WRITE setStateBridgeObject NOTIFY stateBridgeChanged)
    Q_PROPERTY(bool noteAuthoringEnabled MEMBER noteAuthoringEnabled_ NOTIFY appearanceChanged)
    // Drags along one lane draw holds; any other drag on empty space selects.
    Q_PROPERTY(bool authoringSpanEnabled MEMBER authoringSpanEnabled_ NOTIFY appearanceChanged)
    // Vertical zoom.
    Q_PROPERTY(double pixelsPerSecond READ pixelsPerSecond WRITE setPixelsPerSecond NOTIFY appearanceChanged)
    // 分音: grid lines and snapping on a 1/N bar grid; 0 shows the chart's
    // commas only.
    Q_PROPERTY(int gridDivision MEMBER gridDivision_ NOTIFY appearanceChanged)

public:
    explicit TimelineLaneEditorItem(QQuickItem* parent = nullptr);

    QObject* stateBridgeObject() const;
    void setStateBridgeObject(QObject* bridge);
    double pixelsPerSecond() const { return pixelsPerSecond_; }
    void setPixelsPerSecond(double value);

    Q_INVOKABLE void clearAuthoringSelection();

    void paint(QPainter* painter) override;

signals:
    void stateBridgeChanged();
    void appearanceChanged();
    // Same contract as TimelineQuickItem::authoringGesture: kind 0 click,
    // 1 span, 2 move, 3 box; lanes 1..8 and 9 for touches.
    void authoringGesture(int kind, bool secondary, double second, double endSecond,
                          int lane, int endLane, bool onNote);
    void authoringDeleteRequested();
    // Seek to a beat: wheel steps, and clicks while no tool is active.
    void navigateRequested(double second);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct VisibleNote {
        QPointF head;
        QPointF tail;
        double second = 0.0;
        int lane = 0;
        int endLane = 0;
        int kind = 0;
        quint32 flags = 0;
    };

    // BPM in effect from a measure line on.
    struct TempoMark {
        double second = 0.0;
        double bpm = 0.0;
    };

    enum class Drag {
        None,
        Place,
        MoveNote,
        Box,
    };

    qreal gutterWidth() const;
    qreal columnWidth() const;
    qreal lanesRight() const;
    qreal lanesBottom() const;
    qreal judgeY() const;
    qreal yForSecond(double second) const;
    double secondForY(qreal y) const;
    qreal xForLane(int lane) const;
    int laneAtX(qreal x) const;
    double playheadSecond() const;
    QVector<VisibleNote> visibleNotes() const;
    QVector<double> beatSeconds(double from, double to) const;
    QVector<TempoMark> tempoMarks() const;
    bool hitNote(const QPointF& point, double* second, int* lane) const;
    void finishPress(const QPointF& point);
    void paintNote(QPainter* painter, const VisibleNote& note, qreal radius) const;
    // Skin icons, the same set the horizontal timeline draws with.
    bool ensureSkinAssets();
    QPixmap skinIcon(const QString& type, qreal size) const;
    bool paintSkinNote(QPainter* painter, const VisibleNote& note, qreal radius) const;
    bool paintSkinHold(QPainter* painter, const VisibleNote& note, qreal radius) const;

    QPointer<TimelineQuickStateBridge> stateBridge_;
    bool noteAuthoringEnabled_ = false;
    bool authoringSpanEnabled_ = false;
    double pixelsPerSecond_ = 150.0;
    int gridDivision_ = 16;
    int hoveredLane_ = 0;
    miacode::timeline::TimelineNoteAssetSet skinAssets_;
    QString skinAssetsDirectory_;
    bool skinAssetsLoaded_ = false;
    mutable QHash<QString, QPixmap> scaledIcons_;

    Drag drag_ = Drag::None;
    Qt::MouseButton button_ = Qt::NoButton;
    QPointF pressPoint_;
    QPointF currentPoint_;
    double pressSecond_ = 0.0;
    double pressRawSecond_ = 0.0;
    int pressLane_ = 0;
    bool pressOnNote_ = false;
    bool selectionValid_ = false;
    double selectionStartSecond_ = 0.0;
    double selectionEndSecond_ = 0.0;
    int selectionFirstLane_ = 0;
    int selectionLastLane_ = 0;
};
