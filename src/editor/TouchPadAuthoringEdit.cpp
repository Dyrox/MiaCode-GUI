#include "editor/TouchPadAuthoringEdit.h"

#include "core/chart/parser/SimaiCommentScan.h"
#include "editor/ChartTokenScan.h"

#include <QTextCursor>
#include <QTextDocument>

namespace miacode::editor {

namespace {

bool isSelectedOrdinaryTouch(const QString& item, const QString& normalizedPad)
{
    if (item.compare(normalizedPad, Qt::CaseInsensitive) == 0) {
        return true;
    }
    return item.size() == normalizedPad.size() + 1
        && item.endsWith(QLatin1Char('f'), Qt::CaseInsensitive)
        && item.left(normalizedPad.size()).compare(normalizedPad, Qt::CaseInsensitive) == 0;
}

} // namespace

TouchPadAuthoringEditPlan planTouchPadAuthoringEdit(
    const QString& text,
    int cursorPosition,
    const QString& pad,
    QChar separator)
{
    TouchPadAuthoringEditPlan plan;
    const QString normalizedPad = pad.trimmed().toUpper();
    if (normalizedPad.isEmpty()) {
        return plan;
    }
    const int position = qBound(0, cursorPosition, int(text.size()));
    if (separator == QLatin1Char(',')) {
        plan.tokenStart = miacode::simai::previousChartComma(text, position) + 1;
        plan.insertionPosition = position;
        plan.insertionText = normalizedPad + separator;
        plan.valid = true;
        return plan;
    }
    const ChartTokenScan token = scanChartToken(text, position);
    plan.tokenStart = token.tokenStart;

    // Left and Ctrl+Shift clicks toggle the first matching pad in the beat.
    for (const ChartTokenItem& item : token.items) {
        if (isSelectedOrdinaryTouch(text.mid(item.noteStart, item.noteEnd - item.noteStart), normalizedPad)) {
            const ChartTextRange removal = chartTokenItemRemovalRange(item);
            plan.insertionPosition = removal.start;
            plan.removalLength = removal.length;
            plan.valid = true;
            return plan;
        }
    }

    if (!token.empty) {
        plan.insertionPosition = token.lastContentEnd;
        const QChar validatedSeparator = separator == QLatin1Char('`')
            ? separator
            : QLatin1Char('/');
        plan.insertionText = QString(validatedSeparator) + normalizedPad;
        plan.valid = true;
        return plan;
    }

    // The pad is written flush against the controls: authoring never inserts
    // whitespace of its own.
    plan.insertionPosition = token.emptyInsertPosition;
    plan.insertionText = normalizedPad;
    plan.valid = true;
    return plan;
}

bool applyTouchPadAuthoringEdit(
    QTextDocument* document,
    QTextCursor* cursor,
    const TouchPadAuthoringEditPlan& plan)
{
    if (document == nullptr || cursor == nullptr || !plan.valid) {
        return false;
    }
    QTextCursor editCursor(document);
    editCursor.beginEditBlock();
    const int documentLength = document->characterCount() - 1;
    const int editStart = qBound(0, plan.insertionPosition, documentLength);
    editCursor.setPosition(editStart);
    if (plan.removalLength > 0) {
        editCursor.setPosition(
            qBound(editStart, editStart + plan.removalLength, documentLength),
            QTextCursor::KeepAnchor);
    }
    editCursor.insertText(plan.insertionText);
    editCursor.endEditBlock();
    *cursor = editCursor;
    return true;
}

} // namespace miacode::editor
