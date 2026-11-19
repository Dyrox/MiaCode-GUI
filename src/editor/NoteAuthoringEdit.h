#pragma once

#include <QPoint>
#include <QString>
#include <QVector>

namespace miacode::editor {

// Graphical charting writes one each-note item into the comma token at the
// editor cursor. Targets are lanes "1".."8" or touch pads "A1".."E8" and "C".
enum class NoteAuthoringAction {
    // Remove the item of the same kind on the target, or add `item`.
    Toggle,
    // Always add `item`.
    Add,
    // Remove the first item on the target, whatever its kind.
    Remove,
    // Two-click holds: remember the cursor token, then write the hold into it
    // with the length up to the token the cursor is in at the second click.
    HoldBegin,
    HoldFinish,
    HoldCancel,
    // Continue the last slide on `target` in the token with another segment
    // (`item` is the shape and end lane, e.g. "q3").
    ExtendSlide,
    // Move the note at (line, column) to the token at (targetLine,
    // targetColumn), rotated by `laneDelta` lanes.
    Move,
    // Remember the notes at `notes` for the selection commands below.
    SelectNotes,
    CopySelection,
    CutSelection,
    DeleteSelection,
    // Overlay the copied notes from the caret token on, keeping their beats.
    Paste,
    // Move the caret `step` tokens forward or back.
    StepToken,
    // Write `item` as the token's `(BPM)` or `{N}` control.
    SetBpm,
    SetDivision,
};

struct NoteAuthoringRequest {
    NoteAuthoringAction action = NoteAuthoringAction::Toggle;
    QString target;
    // Complete item text; for HoldFinish the item without its `[N:M]` length.
    QString item;
    // Length written when a hold starts and ends in the same token.
    QString defaultDuration;
    // Timeline requests name their token instead of using the caret
    // (1-based; 0 = caret). They are only valid at `sourceRevision`.
    int line = 0;
    int column = 0;
    quint64 sourceRevision = 0;
    // A hold whose length runs from its token to this one (1-based; 0 = none).
    int lengthLine = 0;
    int lengthColumn = 0;
    // Move: destination token and lane rotation.
    int targetLine = 0;
    int targetColumn = 0;
    int laneDelta = 0;
    // SelectNotes: note positions as (column, line), 1-based.
    QVector<QPoint> notes;
    int step = 0;
};

enum class NoteItemKind {
    Unknown,
    Tap,
    Hold,
    Slide,
    Touch,
    TouchHold,
};

// The target and kind of one each-note item, e.g. "3bh[4:1]" -> ("3", Hold).
// Lane shorthand such as "15" reports its first lane; use noteItemHitsTarget.
struct NoteItemInfo {
    QString target;
    NoteItemKind kind = NoteItemKind::Unknown;
    bool laneShorthand = false;
};

NoteItemInfo classifyNoteItem(const QString& item);
bool noteItemHitsTarget(const QString& item, const QString& target);

struct NoteAuthoringEditPlan {
    int tokenStart = 0;
    int replaceStart = 0;
    int removalLength = 0;
    QString insertionText;
    bool valid = false;
};

// Plans Toggle, Add or Remove; the hold actions are resolved by the caller
// into an Add with a complete item first.
NoteAuthoringEditPlan planNoteAuthoringEdit(
    const QString& text,
    int cursorPosition,
    NoteAuthoringAction action,
    const QString& target,
    const QString& item);

// Start of the comma token holding a cursor position.
int chartTokenStartAt(const QString& text, int cursorPosition);

// Note length "[N:M]" covering the commas between two token starts, using
// the `{N}` subdivisions in effect; empty when no comma lies between them.
QString beatDurationBetween(const QString& text, int fromTokenStart, int toTokenStart);

// The same span as an exact fraction of a bar, reduced; false when no comma
// lies between the two token starts.
bool beatFractionBetween(const QString& text, int fromTokenStart, int toTokenStart,
                         qint64* numerator, qint64* denominator);

} // namespace miacode::editor
