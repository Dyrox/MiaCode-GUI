#pragma once

#include <QString>
#include <QVector>

namespace miacode::editor {

// One each-note item of a comma token, split on `/` and `` ` `` only.
struct ChartTokenItem {
    // First character after the preceding separator (or the span start).
    int itemStart = 0;
    // The next separator, or the span's trimmed content end.
    int itemEnd = 0;
    // The note text itself: leading controls and spaces skipped, trailing
    // spaces trimmed.
    int noteStart = 0;
    int noteEnd = 0;
    int spanEnd = 0;
    // Position within its content span; comments split a token into spans.
    int index = 0;
};

// The comma token that holds a cursor position. Comments are skipped, and
// `{N}`, `(BPM)` and `<HS*x>` controls do not make a token non-empty.
struct ChartTokenScan {
    int tokenStart = 0;
    int tokenEnd = 0;
    bool empty = true;
    // Where an appended each-note item goes when the token is not empty.
    int lastContentEnd = 0;
    // Where a note goes when the token is empty: right after its controls.
    int emptyInsertPosition = 0;
    // Only filled for a non-empty token.
    QVector<ChartTokenItem> items;
};

ChartTokenScan scanChartToken(const QString& text, int cursorPosition);

// Removes an item together with one adjoining separator, so the remaining
// items of the token stay well formed.
struct ChartTextRange {
    int start = 0;
    int length = 0;
};

ChartTextRange chartTokenItemRemovalRange(const ChartTokenItem& item);

} // namespace miacode::editor
