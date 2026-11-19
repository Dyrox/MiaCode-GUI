#pragma once

#include "core/scene/NoteAuthoringGeometry.h"
#include "editor/NoteAuthoringEdit.h"

#include <QObject>
#include <QString>

namespace miacode {

class EditorSyncController;

// Graphical charting tool state. Preview gestures become note items written
// into the comma token at the editor cursor through EditorSyncController, so
// the chart text, its undo history and the document revision stay the single
// authority.
class NoteAuthoringController final : public QObject
{
    Q_OBJECT
    // NoteAuthoringTool value; 0 turns graphical charting off.
    Q_PROPERTY(int tool READ tool WRITE setTool NOTIFY toolChanged)
    Q_PROPERTY(bool breakNote READ breakNote WRITE setBreakNote NOTIFY optionsChanged)
    Q_PROPERTY(bool exNote READ exNote WRITE setExNote NOTIFY optionsChanged)
    Q_PROPERTY(bool starTap READ starTap WRITE setStarTap NOTIFY optionsChanged)
    Q_PROPERTY(bool firework READ firework WRITE setFirework NOTIFY optionsChanged)
    Q_PROPERTY(bool wifi READ wifi WRITE setWifi NOTIFY optionsChanged)
    Q_PROPERTY(QString holdLength READ holdLength WRITE setHoldLength NOTIFY optionsChanged)
    Q_PROPERTY(QString slideLength READ slideLength WRITE setSlideLength NOTIFY optionsChanged)
    Q_PROPERTY(QString pendingTarget READ pendingTarget NOTIFY pendingTargetChanged)

public:
    explicit NoteAuthoringController(EditorSyncController& editorSync, QObject* parent = nullptr);

    int tool() const { return int(tool_); }
    void setTool(int tool);
    bool breakNote() const { return breakNote_; }
    void setBreakNote(bool enabled);
    bool exNote() const { return exNote_; }
    void setExNote(bool enabled);
    bool starTap() const { return starTap_; }
    void setStarTap(bool enabled);
    bool firework() const { return firework_; }
    void setFirework(bool enabled);
    bool wifi() const { return wifi_; }
    void setWifi(bool enabled);
    QString holdLength() const { return holdLength_; }
    void setHoldLength(const QString& length);
    QString slideLength() const { return slideLength_; }
    void setSlideLength(const QString& length);
    QString pendingTarget() const { return pendingTarget_; }

    void handleGesture(const miacode::preview::scene::NoteAuthoringGesture& gesture);
    Q_INVOKABLE void cancelPending();

signals:
    void toolChanged();
    void optionsChanged();
    void pendingTargetChanged();

private:
    QString headModifiers() const;
    QString tapItem(const QString& lane) const;
    QString holdItemWithoutLength(const QString& target) const;
    QString slideItem(int startLane, int endLane, const QVector<QPointF>& path) const;
    QString touchItem(const QString& pad) const;
    bool send(miacode::editor::NoteAuthoringAction action, const QString& target,
              const QString& item = QString());
    void setPendingTarget(const QString& target);

    EditorSyncController& editorSync_;
    miacode::preview::scene::NoteAuthoringTool tool_ = miacode::preview::scene::NoteAuthoringTool::None;
    bool breakNote_ = false;
    bool exNote_ = false;
    bool starTap_ = false;
    bool firework_ = false;
    bool wifi_ = false;
    QString holdLength_ = QStringLiteral("[4:1]");
    QString slideLength_ = QStringLiteral("[4:1]");
    QString pendingTarget_;
};

} // namespace miacode
