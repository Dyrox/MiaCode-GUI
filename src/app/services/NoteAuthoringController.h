#pragma once

#include "core/scene/NoteAuthoringGeometry.h"
#include "editor/NoteAuthoringEdit.h"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <QPointF>

namespace miacode {

class EditorSyncController;

// Graphical charting tool state. Preview and timeline gestures become note
// items written into the chart text through EditorSyncController, so the
// text, its undo history and the document revision stay the single authority.
class NoteAuthoringController final : public QObject
{
    Q_OBJECT
    // NoteAuthoringTool value; 0 turns graphical charting off.
    Q_PROPERTY(int tool READ tool WRITE setTool NOTIFY toolChanged)
    // GUI charting replaces the code view with the vertical lane editor.
    Q_PROPERTY(bool guiMode READ guiMode WRITE setGuiMode NOTIFY guiModeChanged)
    Q_PROPERTY(bool breakNote READ breakNote WRITE setBreakNote NOTIFY optionsChanged)
    Q_PROPERTY(bool exNote READ exNote WRITE setExNote NOTIFY optionsChanged)
    Q_PROPERTY(bool starTap READ starTap WRITE setStarTap NOTIFY optionsChanged)
    Q_PROPERTY(bool firework READ firework WRITE setFirework NOTIFY optionsChanged)
    Q_PROPERTY(bool wifi READ wifi WRITE setWifi NOTIFY optionsChanged)
    // 伪双押: notes of one beat joined with `` ` `` instead of `/`.
    Q_PROPERTY(bool pseudoEach READ pseudoEach WRITE setPseudoEach NOTIFY optionsChanged)
    // 分音: timeline presses snap to a 1/N bar grid; 0 snaps to the chart's
    // own commas.
    Q_PROPERTY(int gridDivision READ gridDivision WRITE setGridDivision NOTIFY optionsChanged)
    // Slide head: 0 star, 1 tap-shaped `@`, 2 no head `?`, 3 no head with an
    // immediate star `!`.
    Q_PROPERTY(int slideHead READ slideHead WRITE setSlideHead NOTIFY optionsChanged)
    Q_PROPERTY(bool slideBreak READ slideBreak WRITE setSlideBreak NOTIFY optionsChanged)
    // Festival (连锁): slide picks add up into one chain.
    Q_PROPERTY(bool slideChain READ slideChain WRITE setSlideChain NOTIFY optionsChanged)
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY statusChanged)
    Q_PROPERTY(int clipCount READ clipCount NOTIFY statusChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusChanged)
    // The copied notes as {row, bars}, for the paste preview.
    Q_PROPERTY(QVariantList clipLayout READ clipLayout NOTIFY statusChanged)
    // The pad the last touch went to: "C", "A1".."E8".
    Q_PROPERTY(QString touchPad READ touchPad WRITE setTouchPad NOTIFY optionsChanged)
    // The tool the preview offers: the panel's tool in code charting; in GUI
    // charting only the touch and slide pickers below.
    Q_PROPERTY(int previewTool READ previewTool NOTIFY pickChanged)
    // GUI charting: a time is picked in the T column and preview clicks on
    // the judgement area put touches there.
    Q_PROPERTY(bool touchPicking READ touchPicking WRITE setTouchPicking NOTIFY pickChanged)
    // GUI charting: the slide's head and end are clicked and its shape is
    // picked on the preview, from the list or by drawing it.
    Q_PROPERTY(bool slidePicking READ slidePicking NOTIFY pickChanged)
    Q_PROPERTY(int slidePickFrom READ slidePickFrom NOTIFY pickChanged)
    Q_PROPERTY(int slidePickEnd READ slidePickEnd NOTIFY pickChanged)
    // The chain picked so far ("1-4q7"); empty before the first segment.
    Q_PROPERTY(QString slidePickChain READ slidePickChain NOTIFY pickChanged)
    // Shapes from slidePickFrom to slidePickEnd: {key, label}.
    Q_PROPERTY(QVariantList slidePickShapes READ slidePickShapes NOTIFY pickChanged)
    // GUI charting: the lane column under the pointer (1..8; 0 for none),
    // marked on the preview's ring as classic editors do.
    Q_PROPERTY(int laneCue READ laneCue WRITE setLaneCue NOTIFY previewGuideChanged)

public:
    explicit NoteAuthoringController(EditorSyncController& editorSync, QObject* parent = nullptr);

    int tool() const { return int(tool_); }
    void setTool(int tool);
    bool guiMode() const { return guiMode_; }
    void setGuiMode(bool enabled);
    bool breakNote() const { return breakNote_; }
    void setBreakNote(bool enabled) { assign(&breakNote_, enabled); }
    bool exNote() const { return exNote_; }
    void setExNote(bool enabled) { assign(&exNote_, enabled); }
    bool starTap() const { return starTap_; }
    void setStarTap(bool enabled) { assign(&starTap_, enabled); }
    bool firework() const { return firework_; }
    void setFirework(bool enabled) { assign(&firework_, enabled); }
    bool wifi() const { return wifi_; }
    void setWifi(bool enabled) { assign(&wifi_, enabled); }
    bool pseudoEach() const { return pseudoEach_; }
    void setPseudoEach(bool enabled) { assign(&pseudoEach_, enabled); }
    int gridDivision() const { return gridDivision_; }
    void setGridDivision(int division) { assign(&gridDivision_, qBound(0, division, 384)); }
    int slideHead() const { return slideHead_; }
    void setSlideHead(int head) { assign(&slideHead_, qBound(0, head, 3)); }
    bool slideBreak() const { return slideBreak_; }
    void setSlideBreak(bool enabled) { assign(&slideBreak_, enabled); }
    bool slideChain() const { return slideChain_; }
    void setSlideChain(bool enabled) { assign(&slideChain_, enabled); }
    // The length a hold gets when its clicks give it none.
    QString holdLength() const { return holdLength_; }
    int selectionCount() const { return selectionCount_; }
    int clipCount() const { return clipCount_; }
    QString statusMessage() const { return statusMessage_; }
    QVariantList clipLayout() const { return clipLayout_; }
    QString touchPad() const { return touchPad_; }
    void setTouchPad(const QString& pad);

    int previewTool() const;
    bool touchPicking() const { return touchPicking_; }
    void setTouchPicking(bool enabled);
    bool slidePicking() const { return slidePickFrom_ > 0; }
    int slidePickFrom() const { return slidePickFrom_; }
    int slidePickEnd() const { return slidePickEnd_; }
    QString slidePickChain() const { return slidePickChain_; }
    QVariantList slidePickShapes() const;
    // What the preview marks and draws for the pickers.
    QString previewAnchor() const;
    bool previewShowsTargets() const;
    QVector<QPointF> previewGuidePath() const;
    QVector<miacode::preview::scene::NoteAuthoringGuideArrow> previewGuideArrows() const;
    // A touch on `pad` with the panel's marks ("A3f").
    QString touchItem(const QString& pad) const;
    int laneCue() const { return laneCue_; }
    void setLaneCue(int lane);

    // The slide picker, opened once the lane editor has the slide's head
    // and end. Picking a shape finishes it; with 连锁 shapes add up into a
    // chain until confirmSlidePick.
    Q_INVOKABLE void beginSlidePick(int startLane, int endLane);
    Q_INVOKABLE void setSlidePickEnd(int lane);
    // The shape under the pointer, drawn on the preview; empty clears it.
    Q_INVOKABLE void previewSlideShape(const QString& key);
    Q_INVOKABLE void pickSlideShape(const QString& key);
    // Drops the last chained segment; with none, the picker closes.
    Q_INVOKABLE void undoSlidePick();
    Q_INVOKABLE void confirmSlidePick();
    Q_INVOKABLE void cancelSlidePick();

    void handleGesture(const miacode::preview::scene::NoteAuthoringGesture& gesture);
    // The current tool's item on a target ("3bx", "Ch[4:1]"); empty when the
    // tool cannot write there.
    QString itemForTarget(const QString& target) const;
    QString holdItemWithoutLength(const QString& target) const;
    // A slide of shape `shapeKey` ("1>5") with the head and track marks set
    // in the panel, and `length` ("[16:3]"; empty for the default length).
    QString slideItemForShape(const QString& shapeKey, const QString& length) const;
    // Shapes from one lane to another for the slide picker: {key, label}.
    Q_INVOKABLE QVariantList slideShapes(int startLane, int endLane) const;
    // Stamps the request with the hold length and queues it for the editor.
    bool send(miacode::editor::NoteAuthoringRequest request);

    Q_INVOKABLE void copySelection();
    Q_INVOKABLE void cutSelection();
    Q_INVOKABLE void deleteSelection();
    Q_INVOKABLE void paste();
    Q_INVOKABLE void stepBeat(int step);
    Q_INVOKABLE void setBpm(const QString& bpm);
    Q_INVOKABLE void setDivision(const QString& division);
    // A chart_transform::ChartTransformOp applied to the selected notes.
    Q_INVOKABLE void transformSelection(int transform);
    // Selection edits: "b", "x", "f", "B" toggle marks, "taps" turns holds
    // into taps on the 分音 grid, "swap" turns slides around, "scale:N/D"
    // stretches the notes in time.
    Q_INVOKABLE void editSelection(const QString& operation);

signals:
    void toolChanged();
    void guiModeChanged();
    void optionsChanged();
    void statusChanged();
    void pickChanged();
    // What the preview should draw for the pickers changed.
    void previewGuideChanged();
    // A pad clicked while touchPicking; `advance` (right click) also asks
    // for the next grid step.
    void touchPadPicked(const QString& pad, bool advance);
    // The slide's course ("1>5", "1-4q7"); empty when the picker was left.
    void slidePickFinished(const QString& shape);

private:
    template <typename T>
    void assign(T* member, const T& value)
    {
        if (*member != value) {
            *member = value;
            emit optionsChanged();
        }
    }
    QString headModifiers() const;
    void handlePickGesture(const miacode::preview::scene::NoteAuthoringGesture& gesture);
    // By value: callers pass slidePickChain_, which this clears.
    void finishSlidePick(QString shape);
    // The lane the next chained segment starts from.
    int slidePickCurrentLane() const;
    bool sendSimple(miacode::editor::NoteAuthoringAction action, const QString& target,
                    const QString& item = QString());
    void applyStatus(int selectionCount, int clipCount, const QString& message, const QVariantList& clipLayout);

    EditorSyncController& editorSync_;
    miacode::preview::scene::NoteAuthoringTool tool_ = miacode::preview::scene::NoteAuthoringTool::None;
    bool guiMode_ = false;
    bool breakNote_ = false;
    bool exNote_ = false;
    bool starTap_ = false;
    bool firework_ = false;
    bool wifi_ = false;
    bool pseudoEach_ = false;
    int gridDivision_ = 16;
    int slideHead_ = 0;
    bool slideBreak_ = false;
    bool slideChain_ = false;
    // Lengths for holds and slides whose clicks give them none.
    QString holdLength_ = QStringLiteral("[4:1]");
    QString slideLength_ = QStringLiteral("[4:1]");
    int selectionCount_ = 0;
    int clipCount_ = 0;
    QString statusMessage_;
    QVariantList clipLayout_;
    QString touchPad_ = QStringLiteral("C");
    bool touchPicking_ = false;
    int laneCue_ = 0;
    int slidePickFrom_ = 0;
    int slidePickEnd_ = 0;
    QString slidePickChain_;
    // Segment ends of the chain, for undo.
    QVector<int> slidePickChainLengths_;
    QString slidePickHover_;
};

} // namespace miacode
