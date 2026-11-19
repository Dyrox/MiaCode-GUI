#include "editor/NoteAuthoringEdit.h"

#include "core/chart/parser/SimaiCommentScan.h"
#include "core/chart/selection/ChartSelectionBeatSummary.h"
#include "editor/ChartTokenScan.h"

#include <numeric>

namespace miacode::editor {

namespace {

bool isLaneDigit(QChar ch)
{
    return ch >= QLatin1Char('1') && ch <= QLatin1Char('8');
}

// Item text with every `[...]` length removed, so its digits and `:`/`#`
// cannot be mistaken for shape or modifier characters.
QString withoutBrackets(const QString& text)
{
    QString result;
    result.reserve(text.size());
    int depth = 0;
    for (const QChar ch : text) {
        if (ch == QLatin1Char('[')) {
            ++depth;
        } else if (ch == QLatin1Char(']')) {
            depth = qMax(0, depth - 1);
        } else if (depth == 0) {
            result.append(ch);
        }
    }
    return result;
}

bool hasSlideShape(const QString& body)
{
    static const QString kShapeChars = QStringLiteral("-^<>vpqszVw");
    for (const QChar ch : body) {
        if (kShapeChars.contains(ch)) {
            return true;
        }
    }
    return false;
}

QString normalizedTarget(const QString& target)
{
    const QString upper = target.trimmed().toUpper();
    return upper.startsWith(QLatin1Char('C')) ? QStringLiteral("C") : upper;
}

} // namespace

NoteItemInfo classifyNoteItem(const QString& item)
{
    NoteItemInfo info;
    const QString text = item.trimmed();
    if (text.isEmpty()) {
        return info;
    }
    const QChar first = text.at(0);
    if (isLaneDigit(first)) {
        info.target = QString(first);
        bool allLanes = text.size() >= 2;
        for (const QChar ch : text) {
            allLanes = allLanes && isLaneDigit(ch);
        }
        if (allLanes) {
            // "15" is the each-tap shorthand for 1/5.
            info.kind = NoteItemKind::Tap;
            info.laneShorthand = true;
            return info;
        }
        const QString body = withoutBrackets(text.mid(1));
        info.kind = hasSlideShape(body)
            ? NoteItemKind::Slide
            : body.contains(QLatin1Char('h')) ? NoteItemKind::Hold : NoteItemKind::Tap;
        return info;
    }

    const QChar ring = first.toUpper();
    int bodyStart = 1;
    if (ring == QLatin1Char('C')) {
        info.target = QStringLiteral("C");
        if (text.size() > 1 && (text.at(1) == QLatin1Char('1') || text.at(1) == QLatin1Char('2'))) {
            bodyStart = 2;
        }
    } else if (ring >= QLatin1Char('A') && ring <= QLatin1Char('E') && text.size() >= 2
               && isLaneDigit(text.at(1))) {
        info.target = QString(ring) + text.at(1);
        bodyStart = 2;
    } else {
        return info;
    }
    const QString body = withoutBrackets(text.mid(bodyStart));
    info.kind = body.contains(QLatin1Char('h'), Qt::CaseInsensitive)
        ? NoteItemKind::TouchHold
        : NoteItemKind::Touch;
    return info;
}

bool noteItemHitsTarget(const QString& item, const QString& target)
{
    const NoteItemInfo info = classifyNoteItem(item);
    const QString wanted = normalizedTarget(target);
    if (info.laneShorthand) {
        return wanted.size() == 1 && item.contains(wanted.at(0));
    }
    return !info.target.isEmpty() && info.target == wanted;
}

NoteAuthoringEditPlan planNoteAuthoringEdit(
    const QString& text,
    int cursorPosition,
    NoteAuthoringAction action,
    const QString& target,
    const QString& item)
{
    NoteAuthoringEditPlan plan;
    const QString wanted = normalizedTarget(target);
    if (wanted.isEmpty()) {
        return plan;
    }
    const ChartTokenScan token = scanChartToken(text, cursorPosition);
    plan.tokenStart = token.tokenStart;
    // The token after the last comma holds the chart's `E` end marker; notes
    // written there would land past the end of the chart.
    for (const ChartTokenItem& tokenItem : token.items) {
        if (text.mid(tokenItem.noteStart, tokenItem.noteEnd - tokenItem.noteStart) == QLatin1String("E")) {
            return plan;
        }
    }

    if (action == NoteAuthoringAction::Toggle || action == NoteAuthoringAction::Remove) {
        const NoteItemKind wantedKind = classifyNoteItem(item).kind;
        for (const ChartTokenItem& tokenItem : token.items) {
            const QString existing = text.mid(tokenItem.noteStart, tokenItem.noteEnd - tokenItem.noteStart);
            if (!noteItemHitsTarget(existing, wanted)) {
                continue;
            }
            const NoteItemInfo info = classifyNoteItem(existing);
            if (action == NoteAuthoringAction::Toggle && info.kind != wantedKind) {
                continue;
            }
            if (info.laneShorthand) {
                // Drop one lane from "15"; a single lane left stays a plain tap.
                QString remaining = existing;
                remaining.remove(remaining.indexOf(wanted.at(0)), 1);
                plan.replaceStart = tokenItem.noteStart;
                plan.removalLength = existing.size();
                plan.insertionText = remaining;
            } else {
                const ChartTextRange removal = chartTokenItemRemovalRange(tokenItem);
                plan.replaceStart = removal.start;
                plan.removalLength = removal.length;
            }
            plan.valid = true;
            return plan;
        }
        if (action == NoteAuthoringAction::Remove) {
            return plan;
        }
    }

    const QString addition = item.trimmed();
    if (addition.isEmpty()) {
        return plan;
    }
    // Notes are written flush against controls and joined with `/`; authoring
    // never inserts whitespace of its own.
    if (token.empty) {
        plan.replaceStart = token.emptyInsertPosition;
        plan.insertionText = addition;
    } else {
        plan.replaceStart = token.lastContentEnd;
        plan.insertionText = QLatin1Char('/') + addition;
    }
    plan.valid = true;
    return plan;
}

int chartTokenStartAt(const QString& text, int cursorPosition)
{
    const int position = qBound(0, cursorPosition, int(text.size()));
    return miacode::simai::previousChartComma(text, position) + 1;
}

QString beatDurationBetween(const QString& text, int fromTokenStart, int toTokenStart)
{
    const auto summary = miacode::chart_selection::summarizeChartSelectionBeats(
        text, qMin(fromTokenStart, toTokenStart), qMax(fromTokenStart, toTokenStart));
    if (summary.totalCommaCount <= 0) {
        return QString();
    }
    // Each comma under {N} lasts 1/N of a bar: sum the parts over one common
    // denominator, then reduce, so {8} x2 becomes [4:1].
    qint64 denominator = 1;
    for (const auto& part : summary.parts) {
        denominator = std::lcm(denominator, qint64(qMax(1, part.denominator)));
    }
    qint64 numerator = 0;
    for (const auto& part : summary.parts) {
        numerator += qint64(part.count) * (denominator / qMax(1, part.denominator));
    }
    const qint64 divisor = std::gcd(denominator, numerator);
    denominator /= divisor;
    numerator /= divisor;
    // Count whole beats rather than half or whole bars: [4:2], not [2:1].
    if (denominator < 4 && 4 % denominator == 0) {
        numerator *= 4 / denominator;
        denominator = 4;
    }
    return QStringLiteral("[%1:%2]").arg(denominator).arg(numerator);
}

} // namespace miacode::editor
