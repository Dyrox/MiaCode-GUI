#pragma once

#include <QString>

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
};

struct NoteAuthoringRequest {
    NoteAuthoringAction action = NoteAuthoringAction::Toggle;
    QString target;
    // Complete item text; for HoldFinish the item without its `[N:M]` length.
    QString item;
    // Length written when a hold starts and ends in the same token.
    QString defaultDuration;
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

} // namespace miacode::editor
