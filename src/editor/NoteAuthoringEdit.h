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
    // Mirror or rotate the selected notes in place (`transform`: a
    // chart_transform::ChartTransformOp value).
    TransformSelection,
    // Write `item` ("[16:3]") as the length of the note at (line, column).
    SetLength,
    // Give the one-segment slide at (line, column) the shape, end lane and
    // length of `item` ("1>5[4:1]"), keeping its marks.
    EditSlide,
    // Edit the selected notes: `item` is "b", "x", "f" or "B" to toggle a
    // mark, "taps" for holds to taps on a 1/gridDivision grid, "swap" to turn
    // slides around, or "scale:N/D" to stretch them in time.
    EditSelection,
    // Move the selected notes together: the earliest to the token at
    // (line, column), all rotated by `laneDelta` lanes. With `item` "copy"
    // the originals stay.
    MoveSelection,
};

struct NoteAuthoringRequest {
    NoteAuthoringAction action = NoteAuthoringAction::Toggle;
    QString target;
    // Complete item text; for a hold spanning to (lengthLine, lengthColumn)
    // the item without its `[N:M]` length.
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
    // `/` joins notes of one beat; `` ` `` writes a pseudo-each.
    QChar separator = QLatin1Char('/');
    // Grid placement between two commas: split the named token's step on a
    // 1/gridDivision grid and write at `tokenFraction` of it (0 < f < 1).
    int gridDivision = 0;
    double tokenFraction = 0.0;
    // Placement past the chart's end: the named token is its last one, and
    // `extendSteps` commas of 1/extendDivision bar are added before writing.
    int extendSteps = -1;
    int extendDivision = 0;
    // GUI charting: lay the chart out again after the edit (one measure a
    // line, as 谱面整理 does), in the same undo step.
    bool tidy = false;
    int transform = 0;
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
    const QString& item,
    QChar separator = QLatin1Char('/'));

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
