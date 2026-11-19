#include "editor/NoteAuthoringBatch.h"

#include "core/chart/parser/SimaiCommentScan.h"
#include "core/chart/selection/ChartSelectionBeatSummary.h"
#include "core/chart/transform/ChartBatchTransform.h"
#include "editor/ChartTokenScan.h"

#include <algorithm>
#include <numeric>

namespace miacode::editor {

namespace {

// One note found in the text. A lane in "15" shorthand is a single digit.
struct LocatedNote {
    int start = 0;
    int end = 0;
    int tokenStart = 0;
    QString text;
    bool shorthandDigit = false;
    ChartTokenItem item;
    bool valid = false;
};

LocatedNote locateNote(const QString& text, int position)
{
    LocatedNote note;
    if (position < 0 || position > text.size()) {
        return note;
    }
    const ChartTokenScan token = scanChartToken(text, position);
    const ChartTokenItem* found = nullptr;
    for (const ChartTokenItem& item : token.items) {
        if (position >= item.noteStart && position < item.noteEnd) {
            found = &item;
            break;
        }
    }
    if (found == nullptr) {
        for (const ChartTokenItem& item : token.items) {
            if (position >= item.itemStart && position <= item.itemEnd && item.noteEnd > item.noteStart) {
                found = &item;
                break;
            }
        }
    }
    if (found == nullptr) {
        return note;
    }
    note.item = *found;
    note.tokenStart = token.tokenStart;
    note.text = text.mid(found->noteStart, found->noteEnd - found->noteStart);
    note.start = found->noteStart;
    note.end = found->noteEnd;
    if (note.text == QLatin1String("E")) {
        return note;
    }
    if (classifyNoteItem(note.text).laneShorthand) {
        const int digit = qBound(found->noteStart, position, found->noteEnd - 1);
        note.shorthandDigit = true;
        note.start = digit;
        note.end = digit + 1;
        note.text = text.mid(digit, 1);
    }
    note.valid = true;
    return note;
}

// The edit that takes a located note out of its token.
ChartTextRange removalRange(const LocatedNote& note)
{
    if (note.shorthandDigit) {
        return {note.start, 1};
    }
    return chartTokenItemRemovalRange(note.item);
}

QVector<LocatedNote> locateDistinctNotes(const QString& text, const QVector<int>& positions)
{
    QVector<LocatedNote> notes;
    for (const int position : positions) {
        const LocatedNote note = locateNote(text, position);
        const bool duplicate = std::any_of(notes.cbegin(), notes.cend(), [&note](const LocatedNote& other) {
            return other.start == note.start;
        });
        if (note.valid && !duplicate) {
            notes.append(note);
        }
    }
    std::sort(notes.begin(), notes.end(), [](const LocatedNote& left, const LocatedNote& right) {
        return left.start < right.start;
    });
    return notes;
}

struct Fraction {
    qint64 numerator = 0;
    qint64 denominator = 1;
};

Fraction reduced(qint64 numerator, qint64 denominator)
{
    const qint64 divisor = qMax<qint64>(1, std::gcd(numerator, denominator));
    return {numerator / divisor, denominator / divisor};
}

Fraction add(const Fraction& left, const Fraction& right)
{
    const qint64 common = std::lcm(left.denominator, right.denominator);
    return reduced(left.numerator * (common / left.denominator) + right.numerator * (common / right.denominator),
                   common);
}

int compare(const Fraction& left, const Fraction& right)
{
    const qint64 a = left.numerator * right.denominator;
    const qint64 b = right.numerator * left.denominator;
    return a < b ? -1 : (a > b ? 1 : 0);
}

// Bar fraction covered by the commas between two token starts.
Fraction spanBetween(const miacode::chart_selection::ChartSelectionBeatIndex& index, int from, int to)
{
    Fraction total;
    if (from >= to) {
        return total;
    }
    for (const auto& part : index.summarize(from, to).parts) {
        total = add(total, reduced(part.count, qMax(1, part.denominator)));
    }
    return total;
}

void applyPlan(QString* text, const NoteAuthoringEditPlan& plan)
{
    text->replace(plan.replaceStart, plan.removalLength, plan.insertionText);
}

constexpr int kPasteTokenLimit = 4096;

} // namespace

int textPositionAt(const QString& text, int line, int column)
{
    if (line < 1 || column < 1) {
        return -1;
    }
    int position = 0;
    for (int current = 1; current < line; ++current) {
        position = text.indexOf(QLatin1Char('\n'), position);
        if (position < 0) {
            return -1;
        }
        ++position;
    }
    const int lineEnd = text.indexOf(QLatin1Char('\n'), position);
    const int target = position + column - 1;
    return target <= (lineEnd < 0 ? text.size() : lineEnd) ? target : -1;
}

NoteClip copyNotes(const QString& text, const QVector<int>& notePositions)
{
    NoteClip clip;
    const QVector<LocatedNote> notes = locateDistinctNotes(text, notePositions);
    if (notes.isEmpty()) {
        return clip;
    }
    miacode::chart_selection::ChartSelectionBeatIndex index;
    index.setText(text);
    const int baseToken = notes.constFirst().tokenStart;
    for (const LocatedNote& note : notes) {
        const Fraction offset = spanBetween(index, baseToken, note.tokenStart);
        clip.append({note.text, offset.numerator, offset.denominator});
    }
    return clip;
}

QString removeNotes(const QString& text, const QVector<int>& notePositions, int* removed)
{
    QString result = text;
    QVector<LocatedNote> notes = locateDistinctNotes(text, notePositions);
    // Back to front, so each removal leaves the earlier positions in place.
    std::reverse(notes.begin(), notes.end());
    for (const LocatedNote& note : notes) {
        const ChartTextRange range = removalRange(note);
        result.remove(range.start, range.length);
    }
    if (removed != nullptr) {
        *removed = notes.size();
    }
    return result;
}

QString pasteNotes(const QString& text, int caretPosition, const NoteClip& clip, int* placed, int* skipped)
{
    int placedCount = 0;
    int skippedCount = 0;
    miacode::chart_selection::ChartSelectionBeatIndex index;
    index.setText(text);

    // Resolve every entry's token on the untouched text first.
    QVector<std::pair<int, const NoteClipEntry*>> targets;
    const int baseToken = chartTokenStartAt(text, caretPosition);
    int token = baseToken;
    Fraction tokenOffset;
    QVector<const NoteClipEntry*> pending;
    for (const NoteClipEntry& entry : clip) {
        pending.append(&entry);
    }
    std::sort(pending.begin(), pending.end(), [](const NoteClipEntry* left, const NoteClipEntry* right) {
        return compare({left->numerator, left->denominator}, {right->numerator, right->denominator}) < 0;
    });
    int walked = 0;
    for (const NoteClipEntry* entry : pending) {
        const Fraction wanted{entry->numerator, entry->denominator};
        while (token >= 0 && compare(tokenOffset, wanted) < 0 && walked < kPasteTokenLimit) {
            const int comma = miacode::simai::nextChartComma(text, token);
            if (comma < 0) {
                token = -1;
                break;
            }
            tokenOffset = add(tokenOffset, spanBetween(index, token, comma + 1));
            token = comma + 1;
            ++walked;
        }
        if (token >= 0 && compare(tokenOffset, wanted) == 0) {
            targets.append({token, entry});
        } else {
            ++skippedCount;
        }
    }

    // Then write from the back, so earlier token starts stay valid.
    QString result = text;
    std::stable_sort(targets.begin(), targets.end(), [](const auto& left, const auto& right) {
        return left.first > right.first;
    });
    for (const auto& [tokenStart, entry] : targets) {
        const NoteItemInfo info = classifyNoteItem(entry->item);
        const NoteAuthoringEditPlan plan = planNoteAuthoringEdit(
            result, tokenStart, NoteAuthoringAction::Add, info.target, entry->item);
        if (!plan.valid) {
            ++skippedCount;
            continue;
        }
        applyPlan(&result, plan);
        ++placedCount;
    }
    if (placed != nullptr) {
        *placed = placedCount;
    }
    if (skipped != nullptr) {
        *skipped = skippedCount;
    }
    return result;
}

QString rotateNoteItem(const QString& item, int laneDelta)
{
    const int steps = ((laneDelta % 8) + 8) % 8;
    QString rotated = item;
    for (int step = 0; step < steps; ++step) {
        rotated = miacode::chart_transform::transformChartSelectionText(
            rotated, miacode::chart_transform::ChartTransformOp::Rotate45Clockwise);
    }
    return rotated;
}

QString moveNote(const QString& text, int notePosition, int targetTokenPosition, int laneDelta, bool* moved)
{
    if (moved != nullptr) {
        *moved = false;
    }
    const LocatedNote note = locateNote(text, notePosition);
    if (!note.valid) {
        return text;
    }
    const QString item = rotateNoteItem(note.text, laneDelta);
    QString result = text;
    const ChartTextRange range = removalRange(note);
    result.remove(range.start, range.length);
    int target = targetTokenPosition;
    if (target > range.start) {
        target = qMax(range.start, target - range.length);
    }
    const NoteItemInfo info = classifyNoteItem(item);
    const NoteAuthoringEditPlan plan = planNoteAuthoringEdit(
        result, target, NoteAuthoringAction::Add, info.target, item);
    if (!plan.valid) {
        return text;
    }
    applyPlan(&result, plan);
    if (moved != nullptr) {
        *moved = true;
    }
    return result;
}

NoteAuthoringEditPlan planSlideExtension(const QString& text, int caretPosition,
                                         const QString& headTarget, const QString& fragment)
{
    NoteAuthoringEditPlan plan;
    const ChartTokenScan token = scanChartToken(text, caretPosition);
    plan.tokenStart = token.tokenStart;
    const ChartTokenItem* last = nullptr;
    for (const ChartTokenItem& item : token.items) {
        const NoteItemInfo info = classifyNoteItem(text.mid(item.noteStart, item.noteEnd - item.noteStart));
        if (info.kind == NoteItemKind::Slide && info.target == headTarget) {
            last = &item;
        }
    }
    if (last == nullptr || fragment.trimmed().isEmpty()) {
        return plan;
    }
    // The new segment goes ahead of the last part's length and its break /
    // mine marks, which belong to the whole chain: "1-5b[4:1]" -> "1-5q3b[4:1]".
    const QString item = text.mid(last->noteStart, last->noteEnd - last->noteStart);
    int insertAt = item.lastIndexOf(QLatin1Char('['));
    if (insertAt < 0) {
        insertAt = item.size();
    }
    while (insertAt > 1 && QStringLiteral("bxm").contains(item.at(insertAt - 1))) {
        --insertAt;
    }
    plan.replaceStart = last->noteStart + insertAt;
    plan.insertionText = fragment.trimmed();
    plan.valid = true;
    return plan;
}

NoteAuthoringEditPlan planTokenControl(const QString& text, int caretPosition, QChar open, const QString& value)
{
    NoteAuthoringEditPlan plan;
    const QString trimmed = value.trimmed();
    const QChar close = open == QLatin1Char('(') ? QLatin1Char(')') : QLatin1Char('}');
    if (trimmed.isEmpty() || (open != QLatin1Char('(') && open != QLatin1Char('{'))) {
        return plan;
    }
    plan.tokenStart = chartTokenStartAt(text, caretPosition);
    const int rightComma = miacode::simai::nextChartComma(text, plan.tokenStart);
    const int tokenEnd = rightComma >= 0 ? rightComma : text.size();
    const auto spans = miacode::simai::chartContentSpans(text, plan.tokenStart, tokenEnd);
    int contentStart = spans.isEmpty() ? plan.tokenStart : spans.first().start;
    while (contentStart < tokenEnd && text.at(contentStart).isSpace()) {
        ++contentStart;
    }
    // Walk the leading controls: replace one of the same kind; otherwise the
    // BPM goes first and the subdivision after any BPM.
    int afterBpm = contentStart;
    int position = contentStart;
    while (position < tokenEnd) {
        const QChar ch = text.at(position);
        const QChar closing = ch == QLatin1Char('(') ? QLatin1Char(')')
            : ch == QLatin1Char('{') ? QLatin1Char('}') : QChar();
        if (closing.isNull()) {
            break;
        }
        const int closePosition = text.indexOf(closing, position + 1);
        if (closePosition < 0 || closePosition >= tokenEnd) {
            break;
        }
        if (ch == open) {
            plan.replaceStart = position;
            plan.removalLength = closePosition + 1 - position;
            plan.insertionText = open + trimmed + close;
            plan.valid = true;
            return plan;
        }
        if (ch == QLatin1Char('(')) {
            afterBpm = closePosition + 1;
        }
        position = closePosition + 1;
    }
    plan.replaceStart = open == QLatin1Char('(') ? contentStart : afterBpm;
    plan.insertionText = open + trimmed + close;
    plan.valid = true;
    return plan;
}

int stepTokenPosition(const QString& text, int caretPosition, int step)
{
    int token = chartTokenStartAt(text, caretPosition);
    for (int moved = 0; moved < qAbs(step); ++moved) {
        if (step > 0) {
            const int comma = miacode::simai::nextChartComma(text, token);
            if (comma < 0) {
                return -1;
            }
            token = comma + 1;
        } else {
            if (token == 0) {
                return -1;
            }
            token = chartTokenStartAt(text, token - 1);
        }
    }
    // Land on the token's first content so the caret sits on its line.
    int position = token;
    while (position < text.size() && text.at(position).isSpace()) {
        ++position;
    }
    const int comma = miacode::simai::nextChartComma(text, token);
    return comma >= 0 && position > comma ? token : position;
}

TextReplacement minimalReplacement(const QString& before, const QString& after)
{
    TextReplacement replacement;
    if (before == after) {
        return replacement;
    }
    int prefix = 0;
    const int shorter = qMin(before.size(), after.size());
    while (prefix < shorter && before.at(prefix) == after.at(prefix)) {
        ++prefix;
    }
    int suffix = 0;
    while (suffix < shorter - prefix
           && before.at(before.size() - 1 - suffix) == after.at(after.size() - 1 - suffix)) {
        ++suffix;
    }
    replacement.start = prefix;
    replacement.end = before.size() - suffix;
    replacement.text = after.mid(prefix, after.size() - suffix - prefix);
    replacement.changed = true;
    return replacement;
}

} // namespace miacode::editor
