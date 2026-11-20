#pragma once

#include "editor/NoteAuthoringEdit.h"

#include <QString>
#include <QVector>

#include <functional>

namespace miacode::editor {

// Whole-text graphical charting operations. Each returns the edited text;
// callers turn the difference into one undoable replacement.

// UTF-16 position of a 1-based (line, column); -1 when outside the text.
int textPositionAt(const QString& text, int line, int column);

// One copied note and its distance from the first copied token, in bars.
struct NoteClipEntry {
    QString item;
    qint64 numerator = 0;
    qint64 denominator = 1;
};
using NoteClip = QVector<NoteClipEntry>;

NoteClip copyNotes(const QString& text, const QVector<int>& notePositions);
QString removeNotes(const QString& text, const QVector<int>& notePositions, int* removed = nullptr);
// Writes each entry into the token at its distance from the caret token,
// splitting a beat where an entry falls between two commas.
QString pasteNotes(const QString& text, int caretPosition, const NoteClip& clip,
                   int* placed = nullptr, int* skipped = nullptr);
// With a grid division the destination is split as refineTokenAt does.
QString moveNote(const QString& text, int notePosition, int targetTokenPosition, int laneDelta,
                 bool* moved = nullptr, int division = 0, double fraction = 0.0);
// The note item at a position, and that item replaced.
QString noteItemAt(const QString& text, int notePosition);
QString replaceNoteItem(const QString& text, int notePosition, const QString& item);
// Writes `length` ("[16:3]") as the note's length.
QString setNoteLength(const QString& text, int notePosition, const QString& length);

// A one-segment slide split into its parts: "1bx>5b[4:3]" is lane 1, head
// "bx", symbol ">", end lane 5, marks "b", length "[4:3]".
struct SlideParts {
    int lane = 0;
    QString head;
    QString symbol;
    int endLane = 0;
    QString marks;
    QString length;
    bool valid = false;
};
SlideParts parseSingleSlide(const QString& item);
QString composeSlide(const SlideParts& parts);

// Selection edits: mark is 'b' break, 'x' EX, 'f' firework, or 'B' for a
// slide track's break.
// `editedPositions` gets where each edited note now starts, for keeping it
// selected.
QString toggleNoteMarks(const QString& text, const QVector<int>& notePositions, QChar mark,
                        int* changed = nullptr, QVector<int>* editedPositions = nullptr);
// Each hold becomes taps on its lane every 1/division bar, head to tail.
QString holdsToTaps(const QString& text, const QVector<int>& notePositions, int division,
                    int* changed = nullptr);
// Stretches the notes' distances from the first one, and their lengths, by
// numerator/denominator.
QString scaleNotes(const QString& text, const QVector<int>& notePositions, int numerator, int denominator,
                   int* changed = nullptr);

// Turns each one-segment slide around: a star tap stays at its head (none
// for a headless slide) and a headless slide runs back from its end with
// the same timing. `reverseShape` maps a shape key ("1>5") to the key of
// the same path walked backwards ("5<1"), or empty when there is none.
QString swapSlides(const QString& text, const QVector<int>& notePositions,
                   const std::function<QString(const QString&)>& reverseShape, int* changed = nullptr,
                   QVector<int>* editedPositions = nullptr);

// Rotates every lane and pad of one item by `laneDelta` lanes clockwise.
QString rotateNoteItem(const QString& item, int laneDelta);
// Applies a chart_transform::ChartTransformOp to each note in place.
QString transformNotes(const QString& text, const QVector<int>& notePositions, int transform,
                       int* changed = nullptr);

// Splits the comma step of the token at `tokenPosition` into a grid fine
// enough for 1/division and returns the text with a comma at `fraction` of
// that step. The token is rewritten under `{m}` and the old `{n}` restored
// after it. `*targetToken` gets the start of the token at that point.
QString refineTokenAt(const QString& text, int tokenPosition, int division, double fraction,
                      int* targetToken);

// Grows the chart past its last token by `steps` commas of 1/division bar,
// keeping any `E` last, and returns the text with `*targetToken` at the
// token `steps` on from the old last one. Writes `{division}` there unless
// it is already in effect; with 0 steps and division 0 it only makes room
// ahead of an `E`.
QString extendChartTo(const QString& text, int division, int steps, int* targetToken);

// Replaces or inserts the token's `(BPM)` (open '(') or `{N}` (open '{').
NoteAuthoringEditPlan planTokenControl(const QString& text, int caretPosition,
                                       QChar open, const QString& value);
// First content position of the token `step` tokens away; -1 when none.
int stepTokenPosition(const QString& text, int caretPosition, int step);

struct TextReplacement {
    int start = 0;
    int end = 0;
    QString text;
    bool changed = false;
};

TextReplacement minimalReplacement(const QString& before, const QString& after);

} // namespace miacode::editor
