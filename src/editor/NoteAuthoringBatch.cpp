#include "editor/NoteAuthoringBatch.h"

#include "core/chart/parser/SimaiCommentScan.h"
#include "core/chart/selection/ChartSelectionBeatSummary.h"
#include "core/chart/transform/ChartBatchTransform.h"
#include "editor/ChartTokenScan.h"

#include <QRegularExpression>

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
    QString result = text;
    const int baseToken = chartTokenStartAt(text, caretPosition);
    QVector<const NoteClipEntry*> pending;
    for (const NoteClipEntry& entry : clip) {
        pending.append(&entry);
    }
    // Latest first: each write then only touches text after the earlier
    // entries' beats and the base token.
    std::stable_sort(pending.begin(), pending.end(), [](const NoteClipEntry* left, const NoteClipEntry* right) {
        return compare({left->numerator, left->denominator}, {right->numerator, right->denominator}) > 0;
    });
    for (const NoteClipEntry* entry : pending) {
        const Fraction wanted = reduced(entry->numerator, entry->denominator);
        miacode::chart_selection::ChartSelectionBeatIndex index;
        index.setText(result);
        int token = baseToken;
        Fraction tokenOffset;
        int target = -1;
        for (int walked = 0; walked < kPasteTokenLimit; ++walked) {
            if (compare(tokenOffset, wanted) == 0) {
                target = token;
                break;
            }
            const int comma = miacode::simai::nextChartComma(result, token);
            if (comma < 0) {
                // Past the chart's last comma: grow it to the entry's beat.
                const Fraction rest = reduced(wanted.numerator * tokenOffset.denominator
                                                  - tokenOffset.numerator * wanted.denominator,
                                              wanted.denominator * tokenOffset.denominator);
                result = extendChartTo(result, int(rest.denominator), int(rest.numerator), &target);
                break;
            }
            const Fraction step = spanBetween(index, token, comma + 1);
            const Fraction next = add(tokenOffset, step);
            if (compare(next, wanted) > 0) {
                // Between two commas: split this beat so one lands there.
                const Fraction inside = reduced(wanted.numerator * tokenOffset.denominator
                                                    - tokenOffset.numerator * wanted.denominator,
                                                wanted.denominator * tokenOffset.denominator);
                const double fraction = double(inside.numerator) * step.denominator
                    / (double(inside.denominator) * qMax<qint64>(1, step.numerator));
                result = refineTokenAt(result, token, int(inside.denominator), fraction, &target);
                break;
            }
            tokenOffset = next;
            token = comma + 1;
        }
        if (target >= 0 && miacode::simai::nextChartComma(result, target) < 0) {
            // The last token may be the `E` marker's; the note goes before it.
            result = extendChartTo(result, 0, 0, &target);
        }
        const NoteItemInfo info = classifyNoteItem(entry->item);
        const NoteAuthoringEditPlan plan = target >= 0
            ? planNoteAuthoringEdit(result, target, NoteAuthoringAction::Add, info.target, entry->item)
            : NoteAuthoringEditPlan();
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

QString noteItemAt(const QString& text, int notePosition)
{
    const LocatedNote note = locateNote(text, notePosition);
    return note.valid ? note.text : QString();
}

QString replaceNoteItem(const QString& text, int notePosition, const QString& item)
{
    const LocatedNote note = locateNote(text, notePosition);
    if (!note.valid || item.isEmpty()) {
        return text;
    }
    QString result = text;
    result.replace(note.start, note.end - note.start, item);
    return result;
}

QString setNoteLength(const QString& text, int notePosition, const QString& length)
{
    QString item = noteItemAt(text, notePosition);
    if (item.isEmpty() || !length.startsWith(QLatin1Char('['))) {
        return text;
    }
    const int open = item.lastIndexOf(QLatin1Char('['));
    const int close = open >= 0 ? item.indexOf(QLatin1Char(']'), open) : -1;
    if (open >= 0 && close > open) {
        item.replace(open, close + 1 - open, length);
    } else {
        item += length;
    }
    return replaceNoteItem(text, notePosition, item);
}

SlideParts parseSingleSlide(const QString& item)
{
    // lane, head marks, one shape, end lane, track marks, length.
    static const QRegularExpression pattern(QStringLiteral(
        "^([1-8])([^\\-\\^<>vpqszVw\\[\\]*]*)(-|\\^|<|>|v|pp|qq|p|q|s|z|w|V[1-8])([1-8])([^\\[\\]*]*)(\\[[^\\]]*\\])?$"));
    SlideParts parts;
    const QRegularExpressionMatch match = pattern.match(item.trimmed());
    if (!match.hasMatch()) {
        return parts;
    }
    parts.lane = match.captured(1).toInt();
    parts.head = match.captured(2);
    parts.symbol = match.captured(3);
    parts.endLane = match.captured(4).toInt();
    parts.marks = match.captured(5);
    parts.length = match.captured(6);
    parts.valid = true;
    return parts;
}

QString composeSlide(const SlideParts& parts)
{
    return QString::number(parts.lane) + parts.head + parts.symbol + QString::number(parts.endLane)
        + parts.marks + parts.length;
}

namespace {

// Where in-place item rewrites leave each item's start, in text order.
QVector<int> shiftedStarts(const QVector<LocatedNote>& notes, const QVector<int>& newLengths)
{
    QVector<int> starts;
    int shift = 0;
    for (int index = 0; index < notes.size(); ++index) {
        const LocatedNote& note = notes.at(index);
        starts.append(note.start + shift);
        shift += newLengths.at(index) - (note.end - note.start);
    }
    return starts;
}

} // namespace

QString toggleNoteMarks(const QString& text, const QVector<int>& notePositions, QChar mark, int* changed,
                        QVector<int>* editedPositions)
{
    QString result = text;
    const QVector<LocatedNote> ordered = locateDistinctNotes(text, notePositions);
    QVector<int> newLengths(ordered.size());
    int count = 0;
    for (int index = ordered.size() - 1; index >= 0; --index) {
        const LocatedNote& note = ordered.at(index);
        QString item = note.text;
        newLengths[index] = item.size();
        if (mark == QLatin1Char('b')) {
            item = miacode::chart_transform::toggleBreakForSelection(item);
        } else if (mark == QLatin1Char('x')) {
            item = miacode::chart_transform::toggleExForSelection(item);
        } else if (mark == QLatin1Char('f')) {
            item = miacode::chart_transform::toggleFireworkForSelection(item);
        } else if (mark == QLatin1Char('B')) {
            // A slide track's break mark sits ahead of its length.
            SlideParts slide = parseSingleSlide(item);
            if (!slide.valid) {
                continue;
            }
            slide.marks = slide.marks.contains(QLatin1Char('b'))
                ? QString(slide.marks).remove(QLatin1Char('b')) : slide.marks + QLatin1Char('b');
            item = composeSlide(slide);
        }
        if (item != note.text) {
            result.replace(note.start, note.end - note.start, item);
            newLengths[index] = item.size();
            ++count;
        }
    }
    if (changed != nullptr) {
        *changed = count;
    }
    if (editedPositions != nullptr) {
        *editedPositions = shiftedStarts(ordered, newLengths);
    }
    return result;
}

namespace {

// "[N:M]" as M/N of a bar; false for lengths written in seconds or BPM.
bool lengthFraction(const QString& item, Fraction* fraction)
{
    static const QRegularExpression pattern(QStringLiteral("\\[(\\d+):(\\d+)\\]"));
    const QRegularExpressionMatch match = pattern.match(item);
    if (!match.hasMatch() || match.captured(1).toLongLong() <= 0) {
        return false;
    }
    *fraction = reduced(match.captured(2).toLongLong(), match.captured(1).toLongLong());
    return true;
}

QString scaledLengths(const QString& item, qint64 numerator, qint64 denominator)
{
    static const QRegularExpression pattern(QStringLiteral("\\[(\\d+):(\\d+)\\]"));
    QString result;
    qsizetype last = 0;
    auto matches = pattern.globalMatch(item);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const Fraction scaled = reduced(match.captured(2).toLongLong() * numerator,
                                        match.captured(1).toLongLong() * denominator);
        result += item.mid(last, match.capturedStart() - last)
            + QStringLiteral("[%1:%2]").arg(scaled.denominator).arg(scaled.numerator);
        last = match.capturedEnd();
    }
    return result + item.mid(last);
}

} // namespace

QString holdsToTaps(const QString& text, const QVector<int>& notePositions, int division, int* changed)
{
    QString result = text;
    QVector<LocatedNote> notes = locateDistinctNotes(text, notePositions);
    std::reverse(notes.begin(), notes.end());
    int count = 0;
    for (const LocatedNote& note : notes) {
        const NoteItemInfo info = classifyNoteItem(note.text);
        Fraction length;
        if (info.kind != NoteItemKind::Hold || !lengthFraction(note.text, &length) || division <= 0) {
            continue;
        }
        // The hold's head and its marks, without the hold and its length.
        QString tap = note.text.left(note.text.indexOf(QLatin1Char('[')) >= 0
                                         ? note.text.indexOf(QLatin1Char('[')) : note.text.size());
        tap.remove(QLatin1Char('h'));
        NoteClip clip;
        const qint64 steps = length.numerator * division / length.denominator;
        for (qint64 step = 0; step <= steps; ++step) {
            const Fraction offset = reduced(step, division);
            clip.append({tap, offset.numerator, offset.denominator});
        }
        const ChartTextRange range = removalRange(note);
        result.remove(range.start, range.length);
        result = pasteNotes(result, note.tokenStart, clip);
        ++count;
    }
    if (changed != nullptr) {
        *changed = count;
    }
    return result;
}

QString swapSlides(const QString& text, const QVector<int>& notePositions,
                   const std::function<QString(const QString&)>& reverseShape, int* changed,
                   QVector<int>* editedPositions)
{
    QString result = text;
    const QVector<LocatedNote> ordered = locateDistinctNotes(text, notePositions);
    QVector<int> newLengths(ordered.size());
    int count = 0;
    for (int index = ordered.size() - 1; index >= 0; --index) {
        const LocatedNote& note = ordered.at(index);
        newLengths[index] = note.text.size();
        const SlideParts slide = parseSingleSlide(note.text);
        if (!slide.valid) {
            continue;
        }
        const QString reversed = reverseShape(QString::number(slide.lane) + slide.symbol
                                              + QString::number(slide.endLane));
        if (reversed.size() < 3) {
            continue;
        }
        SlideParts back = slide;
        back.lane = slide.endLane;
        back.endLane = slide.lane;
        back.symbol = reversed.mid(1, reversed.size() - 2);
        back.head = QStringLiteral("?");
        QString item = composeSlide(back);
        if (!slide.head.contains(QLatin1Char('?')) && !slide.head.contains(QLatin1Char('!'))) {
            // The star stays where it was, now as a star-shaped tap.
            QString star = QString::number(slide.lane) + slide.head;
            if (!star.contains(QLatin1Char('$'))) {
                star += QLatin1Char('$');
            }
            item = star + QLatin1Char('/') + item;
        }
        result.replace(note.start, note.end - note.start, item);
        newLengths[index] = item.size();
        ++count;
    }
    if (changed != nullptr) {
        *changed = count;
    }
    if (editedPositions != nullptr) {
        *editedPositions = shiftedStarts(ordered, newLengths);
    }
    return result;
}

QString scaleNotes(const QString& text, const QVector<int>& notePositions, int numerator, int denominator,
                   int* changed)
{
    const QVector<LocatedNote> notes = locateDistinctNotes(text, notePositions);
    if (notes.isEmpty() || numerator <= 0 || denominator <= 0) {
        return text;
    }
    // Offsets and lengths both stretch from the first selected beat.
    NoteClip clip = copyNotes(text, notePositions);
    for (NoteClipEntry& entry : clip) {
        const Fraction offset = reduced(entry.numerator * numerator, entry.denominator * denominator);
        entry.numerator = offset.numerator;
        entry.denominator = offset.denominator;
        entry.item = scaledLengths(entry.item, numerator, denominator);
    }
    const int baseToken = notes.constFirst().tokenStart;
    QString result = removeNotes(text, notePositions);
    if (changed != nullptr) {
        *changed = clip.size();
    }
    return pasteNotes(result, baseToken, clip);
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
            // A following "{finer}" now repeats the step just written.
            const int close = result.indexOf(QLatin1Char('}'), position);
            if (close > position && close < followingEnd
                && result.mid(position + 1, close - position - 1).trimmed() == QString::number(finer)) {
                result.remove(position, close + 1 - position);
            }
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

QString extendChartTo(const QString& text, int division, int steps, int* targetToken)
{
    const int lastToken = chartTokenStartAt(text, text.size());
    *targetToken = lastToken;
    if (steps < 0 || (division <= 0 && steps > 0)) {
        return text;
    }
    QString result = text;
    // The last `{N}` of the text is the step in effect at its end.
    static const QRegularExpression divisionPattern(QStringLiteral("\\{\\s*(\\d+)\\s*\\}"));
    QRegularExpressionMatch last;
    for (auto it = divisionPattern.globalMatch(result); it.hasNext();) {
        last = it.next();
    }
    if (division > 0 && (!last.hasMatch() || last.captured(1).toInt() != division)) {
        const NoteAuthoringEditPlan control = planTokenControl(result, lastToken, QLatin1Char('{'),
                                                               QString::number(division));
        if (control.valid) {
            result.replace(control.replaceStart, control.removalLength, control.insertionText);
        }
    }
    // The end marker moves on past the new commas: "…,E" becomes "…,,,E",
    // and the token before it takes the notes.
    const ChartTokenScan token = scanChartToken(result, lastToken);
    for (const ChartTokenItem& item : token.items) {
        if (result.mid(item.noteStart, item.noteEnd - item.noteStart) == QLatin1String("E")) {
            result.insert(item.noteStart, QString(steps + 1, QLatin1Char(',')));
            *targetToken = steps == 0 ? lastToken : item.noteStart + steps;
            return result;
        }
    }
    if (steps > 0) {
        result.append(QString(steps, QLatin1Char(',')));
        *targetToken = result.size();
    }
    return result;
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
