#include "editor/ChartTokenScan.h"

#include "core/chart/parser/SimaiCommentScan.h"

namespace miacode::editor {

namespace {

int skipSpaces(const QString& text, int position, int end)
{
    while (position < end && text.at(position).isSpace()) {
        ++position;
    }
    return position;
}

int trimmedEnd(const QString& text, int start, int end)
{
    while (end > start && text.at(end - 1).isSpace()) {
        --end;
    }
    return end;
}

// Skips the `{N}`, `(BPM)` and `<HS*x>` controls that may open a token.
int firstNoteCandidateStart(const QString& text, int start, int end)
{
    int position = skipSpaces(text, start, end);
    while (position < end) {
        const QChar opening = text.at(position);
        const QChar closing = opening == QLatin1Char('(')
            ? QLatin1Char(')')
            : opening == QLatin1Char('{') ? QLatin1Char('}') : QChar();
        const bool isHsDirective = text.mid(position, 4) == QStringLiteral("<HS*");
        if (closing.isNull() && !isHsDirective) {
            break;
        }
        const int closePosition = isHsDirective
            ? text.indexOf(QLatin1Char('>'), position + 4)
            : text.indexOf(closing, position + 1);
        if (closePosition < 0 || closePosition >= end) {
            break;
        }
        position = skipSpaces(text, closePosition + 1, end);
    }
    return position;
}

bool isItemSeparator(QChar ch)
{
    return ch == QLatin1Char('/') || ch == QLatin1Char('`');
}

// When an empty token covers a line break the beat belongs to the LAST line it
// reaches: appending at the trimmed content end would strand the note on the
// previous line, behind that line's trailing comment.
int emptyTokenInsertPosition(const QString& text, int start, int end)
{
    const QVector<miacode::simai::ChartContentSpan> spans =
        miacode::simai::chartContentSpans(text, start, end);
    const int lastNewline = end > start
        ? text.lastIndexOf(QLatin1Char('\n'), end - 1)
        : -1;
    int contentStart = start;
    int contentEnd = start;
    if (!spans.isEmpty()) {
        if (lastNewline >= start) {
            const miacode::simai::ChartContentSpan& lastSpan = spans.constLast();
            contentStart = qMax(lastSpan.start, lastNewline + 1);
            contentEnd = trimmedEnd(text, contentStart, qMax(contentStart, lastSpan.end));
        } else {
            contentStart = spans.first().start;
            contentEnd = trimmedEnd(text, contentStart, spans.first().end);
        }
    }
    // A `{16}` opening the line still has to precede the authored note.
    return firstNoteCandidateStart(text, contentStart, contentEnd);
}

} // namespace

ChartTokenScan scanChartToken(const QString& text, int cursorPosition)
{
    ChartTokenScan scan;
    const int position = qBound(0, cursorPosition, int(text.size()));
    // Commas inside a `||` comment are prose, not beat separators.
    scan.tokenStart = miacode::simai::previousChartComma(text, position) + 1;
    const int rightComma = miacode::simai::nextChartComma(text, position);
    scan.tokenEnd = rightComma >= 0 ? rightComma : int(text.size());
    scan.lastContentEnd = scan.tokenStart;

    // A comment ends at ITS newline, not at the token end, so one comma token
    // can hold chart content on both sides of one (or several) comments.
    const QVector<miacode::simai::ChartContentSpan> spans =
        miacode::simai::chartContentSpans(text, scan.tokenStart, scan.tokenEnd);
    for (const miacode::simai::ChartContentSpan& span : spans) {
        const int spanEnd = trimmedEnd(text, span.start, span.end);
        if (spanEnd > span.start) {
            scan.lastContentEnd = spanEnd;
        }
        if (firstNoteCandidateStart(text, span.start, spanEnd) < spanEnd) {
            scan.empty = false;
        }
    }
    if (scan.empty) {
        scan.emptyInsertPosition = emptyTokenInsertPosition(text, scan.tokenStart, scan.tokenEnd);
        return scan;
    }

    for (const miacode::simai::ChartContentSpan& span : spans) {
        const int spanEnd = trimmedEnd(text, span.start, span.end);
        int itemStart = span.start;
        int itemIndex = 0;
        while (itemStart <= spanEnd) {
            int itemEnd = itemStart;
            while (itemEnd < spanEnd && !isItemSeparator(text.at(itemEnd))) {
                ++itemEnd;
            }
            ChartTokenItem item;
            item.itemStart = itemStart;
            item.itemEnd = itemEnd;
            item.noteStart = itemIndex == 0
                ? firstNoteCandidateStart(text, itemStart, itemEnd)
                : skipSpaces(text, itemStart, itemEnd);
            item.noteEnd = trimmedEnd(text, item.noteStart, itemEnd);
            item.spanEnd = spanEnd;
            item.index = itemIndex;
            scan.items.append(item);
            if (itemEnd >= spanEnd) {
                break;
            }
            itemStart = itemEnd + 1;
            ++itemIndex;
        }
    }
    return scan;
}

ChartTextRange chartTokenItemRemovalRange(const ChartTokenItem& item)
{
    ChartTextRange range;
    if (item.index > 0) {
        range.start = item.itemStart - 1;
        range.length = item.itemEnd - range.start;
    } else if (item.itemEnd < item.spanEnd) {
        range.start = item.noteStart;
        range.length = item.itemEnd + 1 - item.noteStart;
    } else {
        range.start = item.noteStart;
        range.length = item.itemEnd - item.noteStart;
    }
    return range;
}

} // namespace miacode::editor
