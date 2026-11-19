#pragma once

#include "core/scene/NoteAuthoringGeometry.h"
#include "editor/NoteAuthoringEdit.h"

#include <QObject>
#include <QString>

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
    // Slide head: 0 star, 1 tap-shaped `@`, 2 no head `?`, 3 no head with an
    // immediate star `!`.
    Q_PROPERTY(int slideHead READ slideHead WRITE setSlideHead NOTIFY optionsChanged)
    Q_PROPERTY(bool slideBreak READ slideBreak WRITE setSlideBreak NOTIFY optionsChanged)
    // Each slide drag continues the previous slide instead of starting one.
    Q_PROPERTY(bool slideChain READ slideChain WRITE setSlideChain NOTIFY optionsChanged)
    Q_PROPERTY(QString holdLength READ holdLength WRITE setHoldLength NOTIFY optionsChanged)
    Q_PROPERTY(QString slideLength READ slideLength WRITE setSlideLength NOTIFY optionsChanged)
    // A hold's first click, or the lane a slide chain continues from.
    Q_PROPERTY(QString pendingTarget READ pendingTarget NOTIFY pendingTargetChanged)
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY statusChanged)
    Q_PROPERTY(int clipCount READ clipCount NOTIFY statusChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusChanged)

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
    int slideHead() const { return slideHead_; }
    void setSlideHead(int head) { assign(&slideHead_, qBound(0, head, 3)); }
    bool slideBreak() const { return slideBreak_; }
    void setSlideBreak(bool enabled) { assign(&slideBreak_, enabled); }
    bool slideChain() const { return slideChain_; }
    void setSlideChain(bool enabled);
    QString holdLength() const { return holdLength_; }
    void setHoldLength(const QString& length);
    QString slideLength() const { return slideLength_; }
    void setSlideLength(const QString& length);
    QString pendingTarget() const { return pendingTarget_; }
    int selectionCount() const { return selectionCount_; }
    int clipCount() const { return clipCount_; }
    QString statusMessage() const { return statusMessage_; }

    void handleGesture(const miacode::preview::scene::NoteAuthoringGesture& gesture);
    // The current tool's item on a target ("3bx", "Ch[4:1]"); empty when the
    // tool cannot write there.
    QString itemForTarget(const QString& target) const;
    QString holdItemWithoutLength(const QString& target) const;
    // Stamps the request with the hold length and queues it for the editor.
    bool send(miacode::editor::NoteAuthoringRequest request);
    // Arrow keys over the preview: Left/Right write on `hovered` and step one
    // beat on, Up/Down only step. Returns the target the keyboard moves to.
    QString keyboardStep(int key, const QString& hovered);

    Q_INVOKABLE void cancelPending();
    Q_INVOKABLE void copySelection();
    Q_INVOKABLE void cutSelection();
    Q_INVOKABLE void deleteSelection();
    Q_INVOKABLE void paste();
    Q_INVOKABLE void stepBeat(int step);
    Q_INVOKABLE void setBpm(const QString& bpm);
    Q_INVOKABLE void setDivision(const QString& division);

signals:
    void toolChanged();
    void guiModeChanged();
    void optionsChanged();
    void pendingTargetChanged();
    void statusChanged();

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
    QString slideItem(int startLane, int endLane, const QVector<QPointF>& path) const;
    void handleSlideGesture(const miacode::preview::scene::NoteAuthoringGesture& gesture);
    bool sendSimple(miacode::editor::NoteAuthoringAction action, const QString& target,
                    const QString& item = QString());
    void setPendingTarget(const QString& target);
    void applyStatus(int selectionCount, int clipCount, const QString& message);

    EditorSyncController& editorSync_;
    miacode::preview::scene::NoteAuthoringTool tool_ = miacode::preview::scene::NoteAuthoringTool::None;
    bool guiMode_ = false;
    bool breakNote_ = false;
    bool exNote_ = false;
    bool starTap_ = false;
    bool firework_ = false;
    bool wifi_ = false;
    int slideHead_ = 0;
    bool slideBreak_ = false;
    bool slideChain_ = false;
    QString holdLength_ = QStringLiteral("[4:1]");
    QString slideLength_ = QStringLiteral("[4:1]");
    QString pendingTarget_;
    // The head lane of the slide a chain extends.
    QString chainHead_;
    int selectionCount_ = 0;
    int clipCount_ = 0;
    QString statusMessage_;
};

} // namespace miacode
