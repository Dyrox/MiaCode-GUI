#include "editor/NoteAuthoringBatch.h"

#include "core/chart/parser/SimaiCommentScan.h"
#include "core/chart/selection/ChartSelectionBeatSummary.h"
#include "core/chart/transform/ChartBatchTransform.h"
#include "editor/ChartTokenScan.h"

#include <algorithm>
#include <cmath>
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

QString moveNote(const QString& text, int notePosition, int targetTokenPosition, int laneDelta, bool* moved,
                 int division, double fraction)
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
    if (division > 0) {
        result = refineTokenAt(result, target, division, fraction, &target);
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

QString transformNotes(const QString& text, const QVector<int>& notePositions, int transform, int* changed)
{
    QString result = text;
    QVector<LocatedNote> notes = locateDistinctNotes(text, notePositions);
    std::reverse(notes.begin(), notes.end());
    int count = 0;
    for (const LocatedNote& note : notes) {
        const QString transformed = miacode::chart_transform::transformChartSelectionText(
            note.text, miacode::chart_transform::ChartTransformOp(transform));
        if (transformed != note.text) {
            result.replace(note.start, note.end - note.start, transformed);
            ++count;
        }
    }
    if (changed != nullptr) {
        *changed = count;
    }
    return result;
}

QString refineTokenAt(const QString& text, int tokenPosition, int division, double fraction, int* targetToken)
{
    const int tokenStart = chartTokenStartAt(text, tokenPosition);
    *targetToken = tokenStart;
    const int comma = miacode::simai::nextChartComma(text, tokenStart);
    if (division <= 0 || comma < 0) {
        return text;
    }
    // The subdivision this one comma step is written in.
    miacode::chart_selection::ChartSelectionBeatIndex index;
    index.setText(text);
    const auto parts = index.summarize(tokenStart, comma + 1).parts;
    if (parts.size() != 1 || parts.constFirst().count != 1) {
        return text;
    }
    const int current = qMax(1, parts.constFirst().denominator);
    const int finer = int(std::lcm(qint64(current), qint64(division)));
    const int steps = finer / current;
    const int offset = int(std::lround(qBound(0.0, fraction, 1.0) * steps));
    if (offset <= 0) {
        return text;
    }
    if (offset >= steps) {
        *targetToken = comma + 1;
        return text;
    }

    // "{4}2,3," at 1/2 on a 1/16 grid becomes "{16}2,,,,{4}3,".
    QString result = text;
    const NoteAuthoringEditPlan finerControl = planTokenControl(result, tokenStart, QLatin1Char('{'),
                                                                QString::number(finer));
    result.replace(finerControl.replaceStart, finerControl.removalLength, finerControl.insertionText);
    const int splitComma = miacode::simai::nextChartComma(result, tokenStart);
    result.insert(splitComma + 1, QString(steps - 1, QLatin1Char(',')));
    const int nextToken = splitComma + steps;
    // Restore the old step unless the next token sets its own.
    const ChartTokenScan following = scanChartToken(result, nextToken);
    const int followingEnd = following.tokenEnd;
    int content = nextToken;
    while (content < followingEnd && result.at(content).isSpace()) {
        ++content;
    }
    bool ownDivision = false;
    for (int position = content; position < followingEnd; ++position) {
        const QChar ch = result.at(position);
        if (ch == QLatin1Char('{')) {
            ownDivision = true;
            break;
        }
        if (ch != QLatin1Char('(')) {
            break;
        }
        position = result.indexOf(QLatin1Char(')'), position);
        if (position < 0 || position >= followingEnd) {
            break;
        }
    }
    if (!ownDivision) {
        const NoteAuthoringEditPlan restore = planTokenControl(result, nextToken, QLatin1Char('{'),
                                                               QString::number(current));
        result.replace(restore.replaceStart, restore.removalLength, restore.insertionText);
    }
    *targetToken = splitComma + offset;
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
