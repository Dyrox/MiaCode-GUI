#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QPixmap>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>
#include <QVariantList>
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
    // Two-click placement as in classic maimai editors: 0 none, 1 holds
    // (head, then tail), 2 slides (head, then end key and time).
    Q_PROPERTY(int placementMode READ placementMode WRITE setPlacementMode NOTIFY appearanceChanged)
    // 0 idle, 1 hold placed, 2 slide placed, 3 slide waiting for a shape,
    // 4 a time picked in the T column for touches.
    Q_PROPERTY(int placingKind READ placingKind NOTIFY placementChanged)
    Q_PROPERTY(int placingLane READ placingLane NOTIFY placementChanged)
    Q_PROPERTY(double placingSecond READ placingSecond NOTIFY placementChanged)
    // The copied notes as {row, bars} (see NoteAuthoringController), drawn
    // under the pointer while pasteMode is on; a click pastes there.
    Q_PROPERTY(QVariantList pasteLayout MEMBER pasteLayout_ NOTIFY appearanceChanged)
    Q_PROPERTY(bool pasteMode READ pasteMode WRITE setPasteMode NOTIFY placementChanged)
    // Lane columns left to right: 0 is 1..8, 1 is 8..1, 2 is 5678 1234.
    Q_PROPERTY(int laneOrder READ laneOrder WRITE setLaneOrder NOTIFY appearanceChanged)
    // The lane column under the pointer (1..8, 9 for T), 0 for none.
    Q_PROPERTY(int hoveredLane READ hoveredLane NOTIFY hoveredLaneChanged)

public:
    explicit TimelineLaneEditorItem(QQuickItem* parent = nullptr);

    QObject* stateBridgeObject() const;
    void setStateBridgeObject(QObject* bridge);
    double pixelsPerSecond() const { return pixelsPerSecond_; }
    void setPixelsPerSecond(double value);

    Q_INVOKABLE void clearAuthoringSelection();
    // The shape picked for the slide waiting on one; empty cancels.
    Q_INVOKABLE void commitSlide(const QString& shape);
    Q_INVOKABLE void cancelPlacement();
    // With a touch time picked: start a touch hold there, its tail set by
    // the next click; or move the time one grid step on.
    Q_INVOKABLE void beginTouchHold();
    Q_INVOKABLE void advanceTouchTime();
    int placementMode() const { return placementMode_; }
    void setPlacementMode(int mode);
    int placingKind() const { return int(placing_); }
    int placingLane() const { return placeLane_; }
    double placingSecond() const { return placeSecond_; }
    bool pasteMode() const { return pasteMode_; }
    int laneOrder() const { return laneOrder_; }
    void setLaneOrder(int order)
    {
        if (laneOrder_ != order) {
            laneOrder_ = order;
            emit appearanceChanged();
            update();
        }
    }
    int hoveredLane() const { return hoveredLane_; }
    void setPasteMode(bool enabled);

    void paint(QPainter* painter) override;

signals:
    void stateBridgeChanged();
    void appearanceChanged();
    // Same contract as TimelineQuickItem::authoringGesture: kind 0 click,
    // 1 span, 2 move, 3 box, 5 hold tail resize, 6 paste, 9 selection move
    // (the earliest selected note's second, then its target); lanes 1..8 and 9
    // for touches.
    void authoringGesture(int kind, bool secondary, double second, double endSecond,
                          int lane, int endLane, bool onNote);
    void authoringDeleteRequested();
    // Seek to a beat: wheel steps, and clicks while no tool is active.
    void navigateRequested(double second);
    void placementChanged();
    void hoveredLaneChanged();
    // A click in the BPM column: edit the BPM from `second`, now `bpm`.
    void bpmEditRequested(double second, double bpm, QPointF point);
    // A slide's second click: pick a shape from startLane to endLane,
    // offered at `point`.
    void slideShapeRequested(int startLane, int endLane, QPointF point);
    // `replace`: the slide whose star is at (second, lane) gets this course.
    void authoringSlide(double second, double endSecond, int lane, int endLane, const QString& shape,
                        bool replace);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseUngrabEvent() override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    // Trackpad pinch and smart zoom.
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct VisibleNote {
        QPointF head;
        QPointF tail;
        // Slides: where the star starts moving, after its wait.
        QPointF start;
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
        // A hold's tail dragged to a new length.
        ResizeHold,
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
    double bpmAt(double second) const;
    // The next beat after (direction 1) or before (-1) `second`: a comma of
    // the chart or a quarter of the bar, whichever is nearer, so the view
    // also moves on past the chart's end.
    double adjacentBeat(double second, int direction) const;
    bool hitNote(const QPointF& point, double* second, int* lane, int* kind = nullptr) const;
    bool hitHoldTail(const QPointF& point, double* second, int* lane) const;
    void paintPasteGhost(QPainter* painter, qreal radius) const;
    void finishPress(const QPointF& point);
    // Selects the box from the press to the release and reports it.
    void selectBox(double releaseSecond, int releaseLane);
    // A note at (second, lane) inside the current selection box.
    bool inSelection(double second, int lane) const;
    // Seek continuously; a burst of trackpad events builds on its own last
    // target instead of a playhead that has not caught up yet.
    void scrollTo(double second);
    // Zoom by `factor`, keeping the time under `y` where it is.
    void zoomAround(qreal y, double factor);
    double earliestSelectedSecond() const;
    void paintNote(QPainter* painter, const VisibleNote& note, qreal radius) const;
    void paintPlacement(QPainter* painter, qreal radius) const;
    // A press snapped to the 分音 grid, or to the nearest comma without one.
    double snappedSecond(double second) const;
    bool pressPlacement(QMouseEvent* event, int lane);
    // Picks `second` for touches and brings the preview there.
    void pickTouchTime(double second);
    // Skin icons, the same set the horizontal timeline draws with.
    bool ensureSkinAssets();
    QPixmap skinIcon(const QString& type, qreal size) const;
    bool paintSkinNote(QPainter* painter, const VisibleNote& note, qreal radius) const;
    bool paintSkinHold(QPainter* painter, const VisibleNote& note, qreal radius) const;
    bool paintSkinSlideTrack(QPainter* painter, const VisibleNote& note, qreal radius) const;

    QPointer<TimelineQuickStateBridge> stateBridge_;
    bool noteAuthoringEnabled_ = false;
    bool authoringSpanEnabled_ = false;
    double pixelsPerSecond_ = 150.0;
    int gridDivision_ = 16;
    int hoveredLane_ = 0;
    int laneOrder_ = 1;
    QElapsedTimer scrollClock_;
    double scrollTarget_ = 0.0;
    // Smart zoom's way back.
    double smartZoomRestore_ = 0.0;
    QPointF hoverPoint_{-1.0, -1.0};
    enum class Placing {
        None,
        Hold,
        Slide,
        SlideShape,
        TouchTime,
    };
    int placementMode_ = 0;
    Placing placing_ = Placing::None;
    // The slide being placed replaces the course of one already there.
    bool placeReplaces_ = false;
    QVariantList pasteLayout_;
    bool pasteMode_ = false;
    int placeLane_ = 0;
    double placeSecond_ = 0.0;
    int placeEndLane_ = 0;
    double placeEndSecond_ = 0.0;
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
    int pressNoteKind_ = 0;
    bool selectionValid_ = false;
    double selectionStartSecond_ = 0.0;
    double selectionEndSecond_ = 0.0;
    int selectionFirstLane_ = 0;
    int selectionLastLane_ = 0;
};
