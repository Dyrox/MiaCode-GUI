#pragma once

#include "editor/NoteAuthoringEdit.h"

#include <QString>
#include <QVector>

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
// Writes each entry into the token at its distance from the caret token.
// Entries that fall between two commas there are skipped.
QString pasteNotes(const QString& text, int caretPosition, const NoteClip& clip,
                   int* placed = nullptr, int* skipped = nullptr);
QString moveNote(const QString& text, int notePosition, int targetTokenPosition, int laneDelta,
                 bool* moved = nullptr);
// Rotates every lane and pad of one item by `laneDelta` lanes clockwise.
QString rotateNoteItem(const QString& item, int laneDelta);

// Adds `fragment` (shape and end lane, e.g. "q3") to the last slide from
// `headTarget` in the caret token, ahead of its length.
NoteAuthoringEditPlan planSlideExtension(const QString& text, int caretPosition,
                                         const QString& headTarget, const QString& fragment);
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
