#include "app/ui/editor/ScintillaEditorBridge.h"
#include "editor/NoteAuthoringBatch.h"
#include "core/scene/NoteAuthoringGeometry.h"
#include "editor/SimaiCompletionCatalog.h"
#include "common/DebugLog.h"
#include "common/DebugOptions.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QQuickWindow>
#include <QScopedValueRollback>
#include <QStyleHints>
#include <algorithm>
#include <functional>
#include <cmath>
#include <utility>

namespace miacode::ui {
using miacode::editor::normalizeSimaiInput;
ScintillaEditorBridge::ScintillaEditorBridge(QQuickItem* parent)
    : ScintillaQuick_item(parent), document_(*this), styler_(*this, document_)
{
    send(SCI_SETCODEPAGE, SC_CP_UTF8);
    send(SCI_USEPOPUP, SC_POPUP_NEVER);
    send(SCI_SETHSCROLLBAR, false);
    for (int key : {SCK_ADD, SCK_SUBTRACT, SCK_DIVIDE})
        send(SCI_CLEARCMDKEY, key | (SCMOD_CTRL << 16));
    connect(this, &ScintillaQuick_item::textChanged, this, &ScintillaEditorBridge::textMutated);
    connect(this, &ScintillaQuick_item::cursorPositionChanged, this, [this] {
        refreshSelection(!programmatic_);
    });
    connect(this, &ScintillaQuick_item::updateUi, this, [this](Scintilla::Update update) {
        refreshSelection(!programmatic_);
        emit availabilityChanged();
        const int flags = int(update);
        // 选区使用选择高亮；插入光标所在行使用当前行背景。
        const bool showLine = send(SCI_GETSELECTIONEMPTY);
        if (bool(send(SCI_GETCARETLINEVISIBLE)) != showLine) {
            if (showLine) {
                const QColor fill = palette_.value(QStringLiteral("currentLine")).value<QColor>();
                send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK,
                     quint32(scintillaquick::rgb_from_color(fill)) | (quint32(fill.alpha()) << 24));
            } else send(SCI_RESETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK);
        }
        if (flags & SC_UPDATE_CONTENT)
            scheduleOverviewProjectionRefresh(OverviewMarkersDirty | OverviewCurrentLineDirty);
        else if (flags & SC_UPDATE_V_SCROLL)
            scheduleOverviewProjectionRefresh(OverviewCurrentLineDirty);
    });
    connect(this, &ScintillaQuick_item::displayLayoutChanged, this, [this] {
        // Wrapping can shift markers even when the total display-row count stays equal.
        scheduleOverviewProjectionRefresh(OverviewAllDirty);
    });
    connect(this, &QQuickItem::activeFocusChanged, this, [this] {
        publishContext(false);
        emit followVisualChanged();
        scheduleOverviewProjectionRefresh(OverviewCurrentLineDirty);
    });
    connect(this, &ScintillaQuick_item::fontChanged, this, [this] {
        preserveViewport();
        styler_.setAppearance(property("font").value<QFont>(), palette_);
        scheduleOverviewProjectionRefresh(OverviewAllDirty);
    });
    connect(this, &ScintillaQuick_item::notificationReceived, this, [this](const ScintillaQuick_notification& notification) {
        const int flags = int(notification.modificationType);
        if (notification.code == Scintilla::Notification::Modified && (flags & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) {
            const int position = document_.utf16Position(notification.position);
            const int line = document_.lineAt(position);
            const int removed = flags & SC_MOD_DELETETEXT
                ? QString::fromUtf8(notification.text).size() : 0;
            const int added = flags & SC_MOD_INSERTTEXT ? QString::fromUtf8(notification.text).size() : 0;
            if (styler_.following() && followCaretPosition_ >= position)
                followCaretPosition_ = qMax(position, followCaretPosition_ - removed) + added;
            document_.applyChange(notification.position, removed,
                                  flags & SC_MOD_INSERTTEXT ? notification.text : QByteArray{});
            styler_.invalidate(line, notification.linesAdded);
        }
        if ((int(notification.modificationType) & SC_MOD_CONTAINER) && touchUndoAnchors_.contains(notification.token))
            pendingTouchAnchor_ = touchUndoAnchors_.value(notification.token).position;
    });
    connect(this, &ScintillaQuick_item::styleNeeded, this, [this](Scintilla::Position) { if (ready_) styler_.style(); });
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &ScintillaEditorBridge::availabilityChanged);
    connect(this, &ScintillaQuick_item::marginClicked, this, [this](Scintilla::Position position, Scintilla::KeyMod, int margin) {
        if (margin != 1) return;
        beginUserInteraction();
        const int line = send(SCI_LINEFROMPOSITION, position) + 1;
        const QRectF rect = positionToRectangle(document_.utf16Position(position));
        emit bookmarkMenuRequested(line, 16, rect.y());
    });
    trackScenePosition();
}
ScintillaEditorBridge::~ScintillaEditorBridge()
{
    if (syncController_) syncController_->setEditorReadiness(-1, 0, false);
}
void ScintillaEditorBridge::componentComplete()
{
    ScintillaQuick_item::componentComplete();
    ready_ = true;
    refreshSettings();
    synchronizeDocument();
    scheduleOverviewProjectionRefresh(OverviewAllDirty);
}
void ScintillaEditorBridge::updatePolish()
{
    overviewRefreshPending_ = true;
    prepareLayout();
    if (document_.restoreViewport()) {
        viewportToRestore_.reset();
    } else if (viewportToRestore_) {
        const auto anchor = *viewportToRestore_;
        viewportToRestore_.reset();
        const int position = qBound(0, anchor.position, int(send(SCI_GETLENGTH)));
        send(SCI_ENSUREVISIBLE, send(SCI_LINEFROMPOSITION, position));
        const int lineHeight = send(SCI_TEXTHEIGHT, 0);
        const qreal targetY = qBound(qreal(0), anchor.y, qMax(qreal(0), height() - lineHeight));
        const qreal positionY = send(SCI_POINTYFROMPOSITION, 0, position);
        scrollVertical(int(send(SCI_GETFIRSTVISIBLELINE)) + qRound((positionY - targetY) / lineHeight));
    }

    const auto navigation = pendingNavigation_;
    pendingNavigation_.reset();
    bool applied = false;
    if (navigation) {
        const auto& request = *navigation;
        applied = navigationVisible_ && documentSession_ && !imeComposing_
            && request.difficulty == documentSession_->currentDifficultyId()
            && request.revision == documentSession_->documentRevision()
            && request.generation == documentSession_->documentOpenGeneration()
            && request.start >= 0 && request.end >= request.start && request.end <= document_.text().size();
        if (applied) {
            QScopedValueRollback guard(programmatic_, true);
            select(request.start, request.end);
            if (request.focus) takeFocus();
            if (request.reveal) revealPosition(request.end, true);
            publishContext(false);
        }
    }
    captureFrame();
    publishLayout();
    if (cursorRectangle_.top() >= 0 && cursorRectangle_.bottom() <= height()) {
        viewportAnchor_ = {int(send(SCI_GETCURRENTPOS)), cursorRectangle_.y()};
    } else {
        int textLeft = send(SCI_GETMARGINLEFT);
        for (int margin = 0; margin < send(SCI_GETMARGINS); ++margin)
            textLeft += send(SCI_GETMARGINWIDTHN, margin);
        const int position = send(SCI_POSITIONFROMPOINT, textLeft, 0);
        viewportAnchor_ = {position, qreal(send(SCI_POINTYFROMPOSITION, 0, position))};
    }
    document_.captureViewport();
    if (navigation && syncController_)
        syncController_->acknowledgeNavigation(navigation->sequence, applied);
    overviewRefreshPending_ = false;
    refreshOverviewProjection();
}

void ScintillaEditorBridge::preserveViewport()
{
    if (ready_ && !viewportToRestore_) viewportToRestore_ = viewportAnchor_;
}

void ScintillaEditorBridge::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    if (newGeometry.size() != oldGeometry.size()) {
        preserveViewport();
        scheduleOverviewProjectionRefresh(OverviewAllDirty);
    }
    ScintillaQuick_item::geometryChange(newGeometry, oldGeometry);
}

void ScintillaEditorBridge::trackScenePosition()
{
    for (const auto& connection : sceneConnections_) disconnect(connection);
    sceneConnections_.clear();
    for (QQuickItem* item = this; item; item = item->parentItem()) {
        sceneConnections_.append(connect(item, &QQuickItem::xChanged, this, &ScintillaEditorBridge::scenePositionChanged));
        sceneConnections_.append(connect(item, &QQuickItem::yChanged, this, &ScintillaEditorBridge::scenePositionChanged));
        sceneConnections_.append(connect(item, &QQuickItem::rotationChanged, this, &ScintillaEditorBridge::scenePositionChanged));
        sceneConnections_.append(connect(item, &QQuickItem::scaleChanged, this, &ScintillaEditorBridge::scenePositionChanged));
        sceneConnections_.append(connect(item, &QQuickItem::transformOriginChanged, this, &ScintillaEditorBridge::scenePositionChanged));
        sceneConnections_.append(connect(item, &QQuickItem::parentChanged, this, &ScintillaEditorBridge::trackScenePosition));
    }
    emit scenePositionChanged();
}
void ScintillaEditorBridge::publishLayout()
{
    const QFont font = property("font").value<QFont>();
    const int height = send(SCI_TEXTHEIGHT, 0);
    const QSizeF size(width(), this->height());
    const bool metricsChanged = effectiveFont_ != font || lineHeight_ != height || layoutSize_ != size;
    effectiveFont_ = font;
    lineHeight_ = height;
    layoutSize_ = size;
    if (metricsChanged && miacode::debug_options::runtimeDebugOutputEnabled()) {
        const auto* rail = parentItem() ? parentItem()->findChild<QQuickItem*>(QStringLiteral("sourceVerticalScrollBar")) : nullptr;
        miacode::debug_log::appendLine(miacode::debug_log::Channel::Runtime,
            QStringLiteral("editor/layout"),
            QStringLiteral("scope=%1 width=%2 height=%3 line_height=%4 native_bytes=%5 visible=%6 enabled=%7 font=%8 point_size=%9 parent_width=%10 parent_height=%11 rail_x=%12 rail_width=%13 rail_parent_width=%14 graphics_api=%15")
                .arg(document_.scope()).arg(width()).arg(this->height()).arg(height)
                .arg(send(SCI_GETLENGTH)).arg(isVisible()).arg(isEnabled())
                .arg(font.family()).arg(font.pointSizeF())
                .arg(parentItem() ? parentItem()->width() : 0).arg(parentItem() ? parentItem()->height() : 0)
                .arg(rail ? rail->x() : -1).arg(rail ? rail->width() : -1)
                .arg(rail && rail->parentItem() ? rail->parentItem()->width() : -1)
                .arg(window() ? int(window()->rendererInterface()->graphicsApi()) : -1));
    }

    const QRectF cursor = positionToRectangle(cursorPosition());
    const int currentLine = send(SCI_LINEFROMPOSITION, send(SCI_GETCURRENTPOS));
    QRectF currentLineRect = positionToRectangle(
        document_.utf16Position(send(SCI_POSITIONFROMLINE, currentLine)));
    currentLineRect.setHeight(qMax(1, int(send(SCI_WRAPCOUNT, currentLine))) * height);
    const QRectF anchor = positionToRectangle(document_.utf16Position(send(SCI_GETANCHOR)));
    const QRectF follow = styler_.following() ? positionToRectangle(followCaretPosition_) : QRectF{};
    const bool cursorChanged = cursorRectangle_ != cursor;
    const bool currentLineChanged = currentLineRectangle_ != currentLineRect;
    const bool anchorChanged = anchorRectangle_ != anchor;
    const bool followChanged = followCursorRectangle_ != follow;
    cursorRectangle_ = cursor;
    currentLineRectangle_ = currentLineRect;
    anchorRectangle_ = anchor;
    followCursorRectangle_ = follow;
    if (metricsChanged) emit layoutChanged();
    if (metricsChanged) scheduleOverviewProjectionRefresh(OverviewAllDirty);
    if (cursorChanged) emit cursorRectangleChanged();
    if (currentLineChanged) emit currentLineRectangleChanged();
    if (followChanged) emit followVisualChanged();
    if (hasActiveFocus() && (metricsChanged || cursorChanged || anchorChanged))
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle | Qt::ImAnchorRectangle);
}
void ScintillaEditorBridge::setDocumentSession(DocumentModel* value)
{
    if (documentSession_ == value) return;
    if (documentSession_) disconnect(documentSession_, nullptr, this, nullptr);
    documentSession_ = value;
    if (value) {
        connect(value, &DocumentModel::chartTextChanged, this, &ScintillaEditorBridge::synchronizeDocument);
        connect(value, &DocumentModel::documentStateChanged, this, &ScintillaEditorBridge::synchronizeDocument);
        connect(value, &DocumentModel::documentReplaced, this, &ScintillaEditorBridge::synchronizeDocument);
        connect(value, &DocumentModel::syntaxIssuesChanged, this, &ScintillaEditorBridge::refreshDiagnostics);
    }
    synchronizeDocument();
    if (ready_) refreshDiagnostics();
    scheduleOverviewProjectionRefresh(OverviewAllDirty);
    emit bindingsChanged();
}
void ScintillaEditorBridge::setController(EditorController* value)
{
    if (controller_ == value) return;
    if (controller_) disconnect(controller_, nullptr, this, nullptr);
    controller_ = value;
    if (value) connect(value, &EditorController::settingsChanged, this, &ScintillaEditorBridge::refreshSettings);
    refreshSettings();
    emit bindingsChanged();
}
void ScintillaEditorBridge::setSyncController(miacode::EditorSyncController* value)
{
    if (syncController_ == value) return;
    if (syncController_) {
        syncController_->setEditorReadiness(-1, 0, false);
        disconnect(syncController_, nullptr, this, nullptr);
    }
    syncController_ = value;
    if (value) {
        connect(value, &miacode::EditorSyncController::navigationRequested, this, &ScintillaEditorBridge::navigate);
        connect(value, &miacode::EditorSyncController::followChanged, this, [this] { applyFollow(true); });
        connect(value, &miacode::EditorSyncController::touchPadAuthoringRequested, this, &ScintillaEditorBridge::touchAuthoring);
        connect(value, &miacode::EditorSyncController::noteAuthoringRequested, this, &ScintillaEditorBridge::noteAuthoring);
    }
    publishContext(false);
    emit bindingsChanged();
}
void ScintillaEditorBridge::setAnalysisSession(AnalysisModel* value)
{
    if (analysisSession_ == value) return;
    if (analysisSession_) disconnect(analysisSession_, nullptr, this, nullptr);
    analysisSession_ = value;
    if (value) connect(value, &AnalysisModel::changed, this, &ScintillaEditorBridge::refreshDiagnostics);
    refreshDiagnostics();
    emit bindingsChanged();
}
void ScintillaEditorBridge::setNavigationVisible(bool value)
{
    if (navigationVisible_ == value) return;
    preserveViewport();
    navigationVisible_ = value;
    if (value) synchronizeDocument();
    if (!value) {
        pendingNavigation_.reset();
        if (controller_) controller_->closeCompletion();
    }
    publishContext(false);
    applyFollow();
    request_scene_graph_update(true, true, false);
    emit bindingsChanged();
}
void ScintillaEditorBridge::setEditorColors(const QVariantMap& value)
{
    if (palette_ == value) return;
    palette_ = value;
    styler_.setAppearance(property("font").value<QFont>(), value);
    emit editorColorsChanged();
}
void ScintillaEditorBridge::setBlockSpacing(int value)
{
    const int spacing = qMax(0, value);
    if (blockSpacing_ == spacing) return;
    preserveViewport();
    blockSpacing_ = spacing;
    send(SCI_SETEXTRAASCENT, blockSpacing_ / 2);
    send(SCI_SETEXTRADESCENT, blockSpacing_ - blockSpacing_ / 2);
    emit appearanceChanged();
    scheduleOverviewProjectionRefresh(OverviewAllDirty);
}
void ScintillaEditorBridge::setAutoWrap(bool value)
{
    if (autoWrap_ == value) return;
    preserveViewport();
    autoWrap_ = value;
    send(SCI_SETWRAPMODE, value ? SC_WRAP_WORD : SC_WRAP_NONE);
    send(SCI_SETHSCROLLBAR, !value);
    send(SCI_SETSCROLLWIDTHTRACKING, !value);
    if (value) send(SCI_SETXOFFSET, 0);
    emit appearanceChanged();
    scheduleOverviewProjectionRefresh(OverviewAllDirty);
}
void ScintillaEditorBridge::setScrollPastEnd(bool value)
{
    preserveViewport();
    scrollPastEnd_ = value;
    send(SCI_SETENDATLASTLINE, !value);
    emit appearanceChanged();
    scheduleOverviewProjectionRefresh(OverviewCurrentLineDirty);
}
int ScintillaEditorBridge::cursorPosition() const { return document_.utf16Position(send(SCI_GETCURRENTPOS)); }
void ScintillaEditorBridge::setCursorPosition(int value) { select(value, value); }
int ScintillaEditorBridge::selectionStart() const { return document_.utf16Position(send(SCI_GETSELECTIONSTART)); }
int ScintillaEditorBridge::selectionEnd() const { return document_.utf16Position(send(SCI_GETSELECTIONEND)); }
QString ScintillaEditorBridge::selectedText() const { return document_.text().mid(selectionStart(), selectionEnd() - selectionStart()); }
QRectF ScintillaEditorBridge::positionToRectangle(int position) const
{
    const int byte = document_.bytePosition(position);
    return QRectF(send(SCI_POINTXFROMPOSITION, 0, byte), send(SCI_POINTYFROMPOSITION, 0, byte), 2,
                  send(SCI_TEXTHEIGHT, send(SCI_LINEFROMPOSITION, byte)));
}
QRectF ScintillaEditorBridge::cursorRectangle() const { return cursorRectangle_; }
int ScintillaEditorBridge::positionAt(qreal x, qreal y) const
{
    return document_.utf16Position(send(SCI_POSITIONFROMPOINT, qRound(x), qRound(y)));
}
QRectF ScintillaEditorBridge::textPositionRectangle(int position) const
{
    return positionToRectangle(position);
}
QRectF ScintillaEditorBridge::lineRectangle(int line) const
{
    return positionToRectangle(document_.lineStart(qBound(0, line - 1, document_.lineCount() - 1)));
}
int ScintillaEditorBridge::lineAtPosition(int position) const
{
    return document_.lineAt(qBound(0, position, int(document_.text().size()))) + 1;
}
QRectF ScintillaEditorBridge::followCursorRectangle() const
{
    return followCursorRectangle_;
}
bool ScintillaEditorBridge::followCaretVisible() const
{
    return documentSession_ && syncController_ && syncController_->followActive()
        && syncController_->followDifficultyId() == documentSession_->currentDifficultyId()
        && styler_.following()
        && (syncController_->followPlaybackActive() || !hasActiveFocus());
}
int ScintillaEditorBridge::cursorLine() const { return send(SCI_LINEFROMPOSITION, send(SCI_GETCURRENTPOS)) + 1; }
int ScintillaEditorBridge::cursorColumn() const
{
    return cursorPosition() - document_.utf16Position(send(SCI_POSITIONFROMLINE, cursorLine() - 1)) + 1;
}
void ScintillaEditorBridge::synchronizeDocument()
{
    if (!ready_ || !documentSession_ || synchronizing_ || handlingIme_) return;
    QScopedValueRollback guard(synchronizing_, true);
    const auto generation = documentSession_->documentOpenGeneration();
    const QString scope = QStringLiteral("difficulty:%1").arg(documentSession_->currentDifficultyId());
    const bool identityChanged = generation_ != generation || scope != document_.scope();
    if (!identityChanged && synchronizedRevision_ == documentSession_->documentRevision()) return;
    if (controller_ && (identityChanged || document_.text() != documentSession_->chartText()))
        controller_->closeCompletion();
    if (imeComposing_ && !identityChanged) { refreshDiagnostics(); return; }
    if (identityChanged && imeComposing_) {
        QScopedValueRollback imeGuard(handlingIme_, true);
        QGuiApplication::inputMethod()->reset();
        imeComposing_ = false;
        emit imeComposingChanged();
    }
    if (generation_ != generation) {
        QGuiApplication::inputMethod()->reset();
        imeComposing_ = false;
        emit imeComposingChanged();
        document_.clear();
        touchUndoAnchors_.clear();
        generation_ = generation;
    }
    if (identityChanged) {
        reportedAnchor_ = reportedCaret_ = -1;
        wheelRemainder_ = 0;
    }
    document_.activate(scope, documentSession_->chartText());
    if (identityChanged && miacode::debug_options::runtimeDebugOutputEnabled()) {
        miacode::debug_log::appendLine(miacode::debug_log::Channel::Runtime,
            QStringLiteral("editor/document_bound"),
            QStringLiteral("scope=%1 model_chars=%2 adapter_chars=%3 native_bytes=%4 readonly=%5 width=%6 height=%7")
                .arg(scope).arg(documentSession_->chartText().size()).arg(document_.text().size())
                .arg(send(SCI_GETLENGTH)).arg(send(SCI_GETREADONLY)).arg(width()).arg(height()));
    }
    synchronizedRevision_ = documentSession_->documentRevision();
    if (identityChanged) styler_.reset();
    if (controller_) controller_->setDifficulty(documentSession_->currentDifficultyId());
    refreshDecorations();
    publishContext(false);
    emit selectionChanged();
    emit availabilityChanged();
    scheduleOverviewProjectionRefresh(OverviewAllDirty);
}
void ScintillaEditorBridge::textMutated()
{
    if (!ready_ || synchronizing_ || handlingIme_) return;
    // Note selections are positions in the text they were made on.
    if (!noteSelection_.notes.isEmpty()) {
        noteSelection_ = {};
        publishAuthoringStatus(QString());
    }
    if (documentSession_) {
        QScopedValueRollback guard(synchronizing_, true);
        documentSession_->setChartText(document_.text());
        synchronizedRevision_ = documentSession_->documentRevision();
        if (controller_) controller_->setDifficulty(documentSession_->currentDifficultyId());
    }
    refreshDecorations();
    publishContext(!programmatic_);
    if (controller_) {
        controller_->updateCompletion(document_.text(), cursorPosition());
    }
    emit selectionChanged();
    emit availabilityChanged();
    scheduleOverviewProjectionRefresh(OverviewAllDirty);
}
void ScintillaEditorBridge::refreshDecorations()
{
    styler_.style();
    const auto bookmarks = styler_.bookmarks();
    if (documentSession_) documentSession_->setEditorBookmarks(documentSession_->currentDifficultyId(), bookmarks);
    if (bookmarks_ != bookmarks) {
        bookmarks_ = bookmarks;
        emit bookmarksChanged();
    }
    refreshDiagnostics();
    applyFollow();
    scheduleOverviewProjectionRefresh(OverviewMarkersDirty);
}
void ScintillaEditorBridge::refreshDiagnostics()
{
    const qulonglong generation = documentSession_
        ? documentSession_->documentOpenGeneration() : ~qulonglong(0);
    const int difficulty = documentSession_ ? documentSession_->currentDifficultyId() : -1;
    if (overviewDiagnosticGeneration_ != generation || overviewDiagnosticDifficulty_ != difficulty) {
        overviewDiagnosticGeneration_ = generation;
        overviewDiagnosticDifficulty_ = difficulty;
        overviewSyntaxRows_.clear();
        overviewMuriRows_.clear();
        scheduleOverviewProjectionRefresh(OverviewMarkersDirty);
    }

    bool overviewRowsChanged = false;
    if (documentSession_ && !documentSession_->validationPending()
        && documentSession_->validationRevision() == documentSession_->documentRevision()) {
        const QVariantList rows = documentSession_->syntaxIssues();
        if (overviewSyntaxRows_ != rows) {
            overviewSyntaxRows_ = rows;
            overviewRowsChanged = true;
        }
        styler_.diagnostics(rows, false);
    }
    if (!analysisSession_) {
        styler_.diagnostics({}, true);
        if (!overviewMuriRows_.isEmpty()) {
            overviewMuriRows_.clear();
            overviewRowsChanged = true;
        }
    } else if (!analysisSession_->pending()) {
        if (documentSession_ && analysisSession_->available()
            && analysisSession_->difficultyId() == documentSession_->currentDifficultyId()
            && analysisSession_->revision() == documentSession_->documentRevision()) {
            const QVariantList rows = analysisSession_->muriRows();
            if (overviewMuriRows_ != rows) {
                overviewMuriRows_ = rows;
                overviewRowsChanged = true;
            }
            styler_.diagnostics(rows, true);
        } else if (!analysisSession_->available()) {
            styler_.diagnostics({}, true);
            if (!overviewMuriRows_.isEmpty()) {
                overviewMuriRows_.clear();
                overviewRowsChanged = true;
            }
        }
    }
    if (overviewRowsChanged)
        scheduleOverviewProjectionRefresh(OverviewMarkersDirty);
}

int ScintillaEditorBridge::overviewDisplayLineCount() const
{
    return overviewDisplayLineCount_;
}

int ScintillaEditorBridge::overviewDisplayLineForPosition(int utf16Position) const
{
    if (!ready_ || !documentSession_ || documentSession_->currentDifficultyId() <= 0 || utf16Position < 0
        || utf16Position > document_.text().size()) return -1;
    const int bytePosition = document_.bytePosition(utf16Position);
    const int documentLine = send(SCI_LINEFROMPOSITION, bytePosition);
    const int documentLineCount = send(SCI_GETLINECOUNT);
    if (documentLine < 0 || documentLine >= documentLineCount
        || !send(SCI_GETLINEVISIBLE, documentLine)) return -1;

    const int firstDisplayLine = send(SCI_VISIBLEFROMDOCLINE, documentLine);
    const int wrappedLineCount = qMax(1, int(send(SCI_WRAPCOUNT, documentLine)));
    const int lineHeight = send(SCI_TEXTHEIGHT, 0);
    if (firstDisplayLine < 0 || lineHeight <= 0) return -1;

    // Scintilla returns a viewport-relative y coordinate; topLine restores the absolute display row.
    const int y = send(SCI_POINTYFROMPOSITION, 0, bytePosition);
    const int displayLine = int(send(SCI_GETFIRSTVISIBLELINE))
        + int(std::floor(qreal(y) / lineHeight));
    if (displayLine < firstDisplayLine || displayLine >= firstDisplayLine + wrappedLineCount
        || send(SCI_DOCLINEFROMVISIBLE, displayLine) != documentLine) return -1;
    return displayLine;
}

void ScintillaEditorBridge::scheduleOverviewProjectionRefresh(int parts)
{
    if (!parts) return;
    overviewDirtyParts_ |= parts;
    if (overviewRefreshPending_) return;
    overviewRefreshPending_ = true;
    polish();
}

void ScintillaEditorBridge::refreshOverviewProjection()
{
    if (!ready_ || !overviewDirtyParts_) return;
    const int dirtyParts = overviewDirtyParts_;
    overviewDirtyParts_ = 0;
    bool changed = false;

    if (dirtyParts & OverviewDisplayLineCountDirty) {
        int displayLineCount = 0;
        if (documentSession_ && documentSession_->currentDifficultyId() > 0) {
            const int documentLineCount = send(SCI_GETLINECOUNT);
            // ContractionState keeps the total display-row count at the document's end boundary.
            displayLineCount = send(SCI_VISIBLEFROMDOCLINE, documentLineCount);
        }
        if (overviewDisplayLineCount_ != displayLineCount) {
            overviewDisplayLineCount_ = displayLineCount;
            changed = true;
        }
    }

    if (dirtyParts & OverviewMarkersDirty) {
        struct MarkerInterval {
            int lane;
            int startDisplayLine;
            int endDisplayLine;
        };
        QVector<MarkerInterval> intervals;
        const int documentLineCount = documentSession_ && documentSession_->currentDifficultyId() > 0
            ? int(send(SCI_GETLINECOUNT)) : 0;
        const auto appendDiagnosticRows = [this, &intervals, documentLineCount](const QVariantList& rows) {
            for (const QVariant& value : rows) {
                const QVariantMap row = value.toMap();
                const int line = row.value(QStringLiteral("line")).toInt();
                if (line <= 0 || line > documentLineCount) continue;
                const int logicalLine = line - 1;
                const int start = document_.lineStart(logicalLine);
                const int end = document_.utf16Position(send(SCI_GETLINEENDPOSITION, logicalLine));
                const int column = qMax(1, row.value(QStringLiteral("column")).toInt());
                const int position = start + qMin(column - 1, qMax(0, end - start));
                const int displayLine = overviewDisplayLineForPosition(position);
                if (displayLine < 0 || displayLine >= overviewDisplayLineCount_) continue;
                const int lane = row.value(QStringLiteral("severity")) == QStringLiteral("warning") ? 0 : 2;
                intervals.append({lane, displayLine, displayLine});
            }
        };
        if (overviewDisplayLineCount_ > 0) {
            appendDiagnosticRows(overviewSyntaxRows_);
            appendDiagnosticRows(overviewMuriRows_);
            for (const QVariant& value : bookmarks_) {
                const int line = value.toMap().value(QStringLiteral("line")).toInt();
                if (line <= 0 || line > documentLineCount) continue;
                const int displayLine = overviewDisplayLineForPosition(document_.lineStart(line - 1));
                if (displayLine >= 0 && displayLine < overviewDisplayLineCount_)
                    intervals.append({1, displayLine, displayLine});
            }
        }

        std::sort(intervals.begin(), intervals.end(), [](const MarkerInterval& left, const MarkerInterval& right) {
            if (left.lane != right.lane) return left.lane < right.lane;
            if (left.startDisplayLine != right.startDisplayLine)
                return left.startDisplayLine < right.startDisplayLine;
            return left.endDisplayLine < right.endDisplayLine;
        });
        QVariantList markers;
        for (const MarkerInterval& interval : intervals) {
            if (!markers.isEmpty()) {
                QVariantMap previous = markers.last().toMap();
                if (previous.value(QStringLiteral("lane")).toInt() == interval.lane
                    && interval.startDisplayLine <= previous.value(QStringLiteral("endDisplayLine")).toInt() + 1) {
                    previous.insert(QStringLiteral("endDisplayLine"),
                        qMax(previous.value(QStringLiteral("endDisplayLine")).toInt(), interval.endDisplayLine));
                    markers.last() = previous;
                    continue;
                }
            }
            markers.append(QVariantMap{
                {QStringLiteral("lane"), interval.lane},
                {QStringLiteral("startDisplayLine"), interval.startDisplayLine},
                {QStringLiteral("endDisplayLine"), interval.endDisplayLine},
            });
        }
        if (overviewMarkers_ != markers) {
            overviewMarkers_ = std::move(markers);
            changed = true;
        }
    }

    if (dirtyParts & OverviewCurrentLineDirty) {
        int currentDisplayLine = -1;
        bool useFollowPosition = false;
        if (ready_ && documentSession_ && documentSession_->currentDifficultyId() > 0) {
            useFollowPosition = !hasActiveFocus() && styler_.following();
            const int position = useFollowPosition ? followCaretPosition_ : cursorPosition();
            currentDisplayLine = overviewDisplayLineForPosition(position);
        }
        if (overviewCurrentDisplayLine_ != currentDisplayLine
            || overviewFollowing_ != useFollowPosition) {
            overviewCurrentDisplayLine_ = currentDisplayLine;
            overviewFollowing_ = useFollowPosition;
            changed = true;
        }
    }
    if (changed) emit overviewChanged();
}
void ScintillaEditorBridge::refreshSettings()
{
    if (!controller_) return;
    send(SCI_SETOVERTYPE, controller_->overwriteMode());
    setFlag(ItemAcceptsInputMethod, !controller_->imeInputDisabled());
}
void ScintillaEditorBridge::publishContext(bool userCaret)
{
    if (!ready_ || !documentSession_ || !syncController_) return;
    const int difficulty = documentSession_->currentDifficultyId();
    const auto revision = documentSession_->documentRevision();
    syncController_->setEditorReadiness(difficulty, revision, navigationVisible_);
    syncController_->setEditorContext(difficulty, revision, document_.utf16Position(send(SCI_GETANCHOR)), cursorPosition(),
                                      hasActiveFocus(), imeComposing_, cursorLine(), cursorColumn(),
                                      userCaret && !imeComposing_ && !programmatic_ && !synchronizing_);
}
void ScintillaEditorBridge::refreshSelection(bool userCaret)
{
    const int anchor = send(SCI_GETANCHOR);
    const int caret = send(SCI_GETCURRENTPOS);
    if (anchor == reportedAnchor_ && caret == reportedCaret_) return;
    reportedAnchor_ = anchor;
    reportedCaret_ = caret;
    scheduleOverviewProjectionRefresh(OverviewCurrentLineDirty);
    emit selectionChanged();
    if (!synchronizing_ && !handlingIme_) {
        publishContext(userCaret);
        if (controller_ && userCaret)
            controller_->updateCompletion(document_.text(), cursorPosition());
    }
}
void ScintillaEditorBridge::revealPosition(int utf16, bool center)
{
    send(SCI_ENSUREVISIBLE, send(SCI_LINEFROMPOSITION, document_.bytePosition(utf16)));
    // Scintilla 的文本坐标包含换行后的子行，用该坐标定位目标显示行。
    const QRectF rect = positionToRectangle(utf16);
    if (!center && rect.top() >= 0 && rect.bottom() <= height()) return;
    const int first = send(SCI_GETFIRSTVISIBLELINE);
    const qreal displacement = rect.center().y() - height() / 2;
    scrollVertical(first + qRound(displacement / qMax(1.0, rect.height())));
}
void ScintillaEditorBridge::applyFollow(bool reveal)
{
    if (styler_.following() && documentSession_ && syncController_ && syncController_->followActive()
        && syncController_->followDifficultyId() == documentSession_->currentDifficultyId()
        && syncController_->followRevision() < documentSession_->documentRevision()) return;
    const bool active = documentSession_ && syncController_ && syncController_->followActive()
        && syncController_->followDifficultyId() == documentSession_->currentDifficultyId()
        && syncController_->followRevision() == documentSession_->documentRevision();
    if (active) followCaretPosition_ = syncController_->followCaret();
    styler_.follow(active, active ? syncController_->followStart() : 0,
                  active ? syncController_->followEnd() : 0);
    const int caretStyle = active && syncController_->followPlaybackActive() ? CARETSTYLE_INVISIBLE : CARETSTYLE_LINE;
    if (send(SCI_GETCARETSTYLE) != caretStyle) send(SCI_SETCARETSTYLE, caretStyle);
    emit followVisualChanged();
    if (reveal && active && navigationVisible_ && syncController_->followReveal())
        revealPosition(syncController_->followCaret(), syncController_->followPlaybackActive());
    scheduleOverviewProjectionRefresh(OverviewCurrentLineDirty);
}
void ScintillaEditorBridge::navigate(qulonglong sequence, int difficulty, qulonglong revision, int start, int end, bool focus, bool reveal)
{
    pendingNavigation_ = NavigationRequest{sequence, difficulty, revision,
        documentSession_->documentOpenGeneration(), start, end, focus, reveal};
    request_scene_graph_update(true, true, false);
}
void ScintillaEditorBridge::touchAuthoring(const QString& pad, QChar separator, int difficulty, qulonglong revision, int anchor, int position)
{
    if (!documentSession_ || !controller_ || imeComposing_ || difficulty != documentSession_->currentDifficultyId()
        || revision != documentSession_->documentRevision()) return;
    QScopedValueRollback guard(programmatic_, true);
    const auto tx = controller_->touchPadAuthoring(document_.text(), anchor, position, pad, separator);
    if (applyEditorTransaction(tx)) {
        takeFocus();
        publishTouchAnchor(tx.transaction.touchTokenStart);
    }
}
void ScintillaEditorBridge::noteAuthoring(const miacode::editor::NoteAuthoringRequest& request, int difficulty,
                                          qulonglong, int, int)
{
    namespace edit = miacode::editor;
    using Action = edit::NoteAuthoringAction;
    if (!documentSession_ || !controller_ || imeComposing_ || difficulty != documentSession_->currentDifficultyId()) return;
    const QString text = document_.text();
    const qulonglong revision = documentSession_->documentRevision();
    QScopedValueRollback tidyGuard(tidyAuthoring_, request.tidy);
    // Caret requests act on the live caret: a burst of clicks or keys is
    // queued before the first one lands, and each must see the one before.
    const int anchor = document_.utf16Position(send(SCI_GETANCHOR));
    const int position = cursorPosition();
    // Timeline requests name positions in the text they were resolved
    // against, so they only apply to that revision.
    const bool explicitPositions = request.line > 0 || !request.notes.isEmpty();
    if (explicitPositions && request.sourceRevision != revision) return;
    if (request.action == Action::StepToken) {
        const int target = edit::stepTokenPosition(text, position, request.step);
        if (target < 0) return;
        QScopedValueRollback guard(programmatic_, true);
        select(target, target);
        send(SCI_SCROLLCARET);
        publishContext(true);
        publishTouchAnchor(target);
        return;
    }
    int tokenPosition = position;
    if (request.line > 0) {
        tokenPosition = edit::textPositionAt(text, request.line, request.column);
        if (tokenPosition < 0) return;
    }
    // Edits placed by position on the timeline or lanes, and edits to a
    // selection, leave the playhead where the user put it.
    const bool followEdit = !explicitPositions
        && request.action != Action::CutSelection && request.action != Action::DeleteSelection
        && request.action != Action::TransformSelection && request.action != Action::Move
        && request.action != Action::EditSelection;
    const auto applyPlan = [&](const edit::NoteAuthoringEditPlan& plan) {
        if (!plan.valid) return;
        applyAuthoring(controller_->authoringReplacement(text, anchor, position, plan.replaceStart,
            plan.replaceStart + plan.removalLength, plan.insertionText, plan.tokenStart), followEdit);
    };
    const auto applyText = [&](const QString& edited) {
        const edit::TextReplacement replacement = edit::minimalReplacement(text, edited);
        if (!replacement.changed) return;
        applyAuthoring(controller_->authoringReplacement(text, anchor, position, replacement.start,
            replacement.end, replacement.text, edit::chartTokenStartAt(edited, replacement.start)), followEdit);
    };
    // Grid placement: past the chart's end the commas are added first;
    // between two commas the beat is split. `*target` is where to write.
    const auto placementTextIn = [&](const QString& source, int position, int* target) {
        *target = position;
        if (request.line <= 0) return source;
        if (request.extendSteps >= 0) {
            return edit::extendChartTo(source, request.extendDivision, request.extendSteps, target);
        }
        if (request.gridDivision > 0) {
            return edit::refineTokenAt(source, position, request.gridDivision, request.tokenFraction, target);
        }
        return source;
    };
    const auto placementText = [&](int* target) { return placementTextIn(text, tokenPosition, target); };
    const auto selectionPositions = [&]() {
        QVector<int> positions;
        if (noteSelection_.scope != document_.scope() || noteSelection_.revision != revision) return positions;
        for (const QPoint& note : noteSelection_.notes) {
            const int notePosition = edit::textPositionAt(text, note.y(), note.x());
            if (notePosition >= 0) positions.append(notePosition);
        }
        return positions;
    };

    edit::NoteAuthoringRequest resolved = request;
    switch (request.action) {
    case Action::HoldBegin:
        holdAnchor_ = {document_.scope(), revision, edit::chartTokenStartAt(text, tokenPosition)};
        takeFocus();
        return;
    case Action::HoldCancel:
        holdAnchor_ = {};
        return;
    case Action::HoldFinish: {
        const HoldAnchor start = std::exchange(holdAnchor_, HoldAnchor{});
        // Any edit between the two clicks moves the remembered token.
        if (start.tokenStart < 0 || start.scope != document_.scope() || start.revision != revision) return;
        const int endToken = edit::chartTokenStartAt(text, tokenPosition);
        QString length = edit::beatDurationBetween(text, start.tokenStart, endToken);
        if (length.isEmpty()) length = request.defaultDuration;
        resolved.action = Action::Add;
        resolved.item = request.item + length;
        // A hold clicked backwards starts at the later click's token.
        tokenPosition = qMin(start.tokenStart, endToken);
        break;
    }
    case Action::ExtendSlide:
        applyPlan(edit::planSlideExtension(text, tokenPosition, request.target, request.item));
        return;
    case Action::Move: {
        const int target = edit::textPositionAt(text, request.targetLine, request.targetColumn);
        if (target >= 0) {
            applyText(edit::moveNote(text, tokenPosition, target, request.laneDelta, nullptr,
                                     request.gridDivision, request.tokenFraction));
        }
        return;
    }
    case Action::TransformSelection: {
        const QVector<int> positions = selectionPositions();
        if (positions.isEmpty()) {
            publishAuthoringStatus(qtTrId("qml.charting.status_no_selection"));
            return;
        }
        noteSelection_ = {};
        applyText(edit::transformNotes(text, positions, request.transform));
        return;
    }
    case Action::SelectNotes:
        noteSelection_ = NoteSelection{document_.scope(), revision, request.notes};
        publishAuthoringStatus(noteSelection_.notes.isEmpty() ? QString()
            : qtTrId("qml.charting.status_selected").arg(noteSelection_.notes.size()));
        return;
    case Action::CopySelection:
    case Action::CutSelection: {
        const QVector<int> positions = selectionPositions();
        if (positions.isEmpty()) {
            publishAuthoringStatus(qtTrId("qml.charting.status_no_selection"));
            return;
        }
        noteClip_ = edit::copyNotes(text, positions);
        if (request.action == Action::CopySelection) {
            publishAuthoringStatus(qtTrId("qml.charting.status_copied").arg(noteClip_.size()));
            return;
        }
        noteSelection_ = {};
        applyText(edit::removeNotes(text, positions));
        publishAuthoringStatus(qtTrId("qml.charting.status_cut").arg(noteClip_.size()));
        return;
    }
    case Action::DeleteSelection: {
        const QVector<int> positions = selectionPositions();
        if (positions.isEmpty()) {
            publishAuthoringStatus(qtTrId("qml.charting.status_no_selection"));
            return;
        }
        int removed = 0;
        const QString edited = edit::removeNotes(text, positions, &removed);
        noteSelection_ = {};
        applyText(edited);
        publishAuthoringStatus(qtTrId("qml.charting.status_deleted").arg(removed));
        return;
    }
    case Action::Paste: {
        if (noteClip_.isEmpty()) return;
        int placed = 0;
        int skipped = 0;
        int target = tokenPosition;
        const QString base = placementText(&target);
        applyText(edit::pasteNotes(base, target, noteClip_, &placed, &skipped));
        publishAuthoringStatus(skipped > 0
            ? qtTrId("qml.charting.status_paste_skipped").arg(placed).arg(skipped)
            : qtTrId("qml.charting.status_pasted").arg(placed));
        return;
    }
    case Action::StepToken:
        return;
    case Action::MoveSelection: {
        QVector<int> positions = selectionPositions();
        if (positions.isEmpty()) {
            publishAuthoringStatus(qtTrId("qml.charting.status_no_selection"));
            return;
        }
        edit::NoteClip clip = edit::copyNotes(text, positions);
        for (edit::NoteClipEntry& entry : clip) {
            if (request.laneDelta != 0) entry.item = edit::rotateNoteItem(entry.item, request.laneDelta);
        }
        // Take the notes out last to first, keeping the destination token
        // in step with the text before it.
        int destination = edit::chartTokenStartAt(text, tokenPosition);
        std::sort(positions.begin(), positions.end(), std::greater<int>());
        QString remaining = text;
        for (const int notePosition : std::as_const(positions)) {
            const QString next = edit::removeNotes(remaining, {notePosition});
            const edit::TextReplacement change = edit::minimalReplacement(remaining, next);
            if (change.changed && change.end <= destination) {
                destination += int(change.text.size()) - (change.end - change.start);
            }
            remaining = next;
        }
        int target = destination;
        const QString base = placementTextIn(remaining, destination, &target);
        int placed = 0;
        const QString moved = edit::pasteNotes(base, target, clip, &placed);
        noteSelection_ = {};
        applyText(moved);
        publishAuthoringStatus(qtTrId("qml.charting.status_moved").arg(placed));
        return;
    }
    case Action::SetLength:
        applyText(edit::setNoteLength(text, tokenPosition, request.item));
        return;
    case Action::EditSlide: {
        const edit::SlideParts old = edit::parseSingleSlide(edit::noteItemAt(text, tokenPosition));
        edit::SlideParts updated = edit::parseSingleSlide(request.item);
        if (!old.valid || !updated.valid) {
            publishAuthoringStatus(qtTrId("qml.charting.status_slide_not_single"));
            return;
        }
        // The new course and timing, the old head and track marks.
        updated.head = old.head;
        updated.marks = old.marks;
        applyText(edit::replaceNoteItem(text, tokenPosition, edit::composeSlide(updated)));
        return;
    }
    case Action::EditSelection: {
        const QVector<int> positions = selectionPositions();
        if (positions.isEmpty()) {
            publishAuthoringStatus(qtTrId("qml.charting.status_no_selection"));
            return;
        }
        const QString operation = request.item;
        int changed = 0;
        QString edited = text;
        // In-place edits keep their notes selected, so a mark toggles back.
        QVector<int> kept;
        if (operation.size() == 1) {
            edited = edit::toggleNoteMarks(text, positions, operation.at(0), &changed, &kept);
        } else if (operation == QLatin1String("taps")) {
            edited = edit::holdsToTaps(text, positions, request.gridDivision > 0 ? request.gridDivision : 16, &changed);
        } else if (operation == QLatin1String("swap")) {
            edited = edit::swapSlides(text, positions, &miacode::preview::scene::reversedSlideShape, &changed, &kept);
        } else if (operation.startsWith(QLatin1String("scale:"))) {
            const QStringList ratio = operation.mid(6).split(QLatin1Char('/'));
            if (ratio.size() == 2) {
                edited = edit::scaleNotes(text, positions, ratio.at(0).toInt(), ratio.at(1).toInt(), &changed);
            }
        }
        if (edited == text) {
            publishAuthoringStatus(qtTrId("qml.charting.status_nothing_changed"));
            return;
        }
        noteSelection_ = {};
        applyText(edited);
        if (!kept.isEmpty() && document_.text() == edited) {
            QVector<QPoint> notes;
            for (const int start : std::as_const(kept)) {
                const int lineStart = start > 0 ? int(edited.lastIndexOf(QLatin1Char('\n'), start - 1)) + 1 : 0;
                notes.append(QPoint(start - lineStart + 1, int(edited.left(start).count(QLatin1Char('\n'))) + 1));
            }
            noteSelection_ = NoteSelection{document_.scope(), documentSession_->documentRevision(), notes};
        }
        publishAuthoringStatus(qtTrId("qml.charting.status_edited").arg(changed));
        return;
    }
    case Action::SetBpm:
    case Action::SetDivision: {
        int target = tokenPosition;
        QString base = placementText(&target);
        const edit::NoteAuthoringEditPlan plan = edit::planTokenControl(base, target,
            request.action == Action::SetBpm ? QLatin1Char('(') : QLatin1Char('{'), request.item);
        if (!plan.valid) return;
        base.replace(plan.replaceStart, plan.removalLength, plan.insertionText);
        applyText(base);
        return;
    }
    case Action::Remove:
        if (!request.notes.isEmpty()) {
            QVector<int> positions;
            for (const QPoint& note : request.notes) {
                const int notePosition = edit::textPositionAt(text, note.y(), note.x());
                if (notePosition >= 0) positions.append(notePosition);
            }
            applyText(edit::removeNotes(text, positions));
            return;
        }
        break;
    case Action::Add:
        if (request.lengthLine > 0) {
            // A hold dragged along the timeline: its length runs to the token
            // under the release point.
            const int endToken = edit::textPositionAt(text, request.lengthLine, request.lengthColumn);
            if (endToken < 0) return;
            const int from = edit::chartTokenStartAt(text, tokenPosition);
            const int to = edit::chartTokenStartAt(text, endToken);
            QString length = edit::beatDurationBetween(text, from, to);
            if (length.isEmpty()) length = request.defaultDuration;
            resolved.item = request.item + length;
            tokenPosition = qMin(from, to);
        }
        break;
    case Action::Toggle:
        break;
    }
    if (request.line > 0 && (request.extendSteps >= 0 || request.gridDivision > 0)) {
        // A grid point past the end or between two commas: make the token,
        // then write there.
        int target = tokenPosition;
        QString refined = placementText(&target);
        const auto plan = edit::planNoteAuthoringEdit(refined, target, resolved.action, resolved.target,
                                                      resolved.item, resolved.separator);
        if (!plan.valid) return;
        refined.replace(plan.replaceStart, plan.removalLength, plan.insertionText);
        applyText(refined);
        return;
    }
    applyAuthoring(controller_->noteAuthoring(text, anchor, position, tokenPosition, resolved), followEdit);
}
void ScintillaEditorBridge::applyAuthoring(miacode::editor::SimaiTextEditResult result, bool followEdit)
{
    QScopedValueRollback guard(programmatic_, true);
    // Without a token start the edit records no undo seek either.
    if (!followEdit) result.transaction.touchTokenStart = -1;
    if (tidyAuthoring_ && result.transaction.hasEdit && documentSession_) {
        // GUI charting keeps the text readable: the edited chart is laid out
        // one measure a line, and the whole change stays one undo step.
        const QString before = document_.text();
        QString edited = before;
        edited.replace(result.transaction.replacementStart,
                       result.transaction.replacementEnd - result.transaction.replacementStart,
                       result.transaction.replacementText);
        // The layout needs a closing comma after the last note to time it;
        // GUI charts always end with one.
        int contentEnd = int(edited.size());
        while (contentEnd > 0 && edited.at(contentEnd - 1).isSpace()) --contentEnd;
        if (contentEnd > 0 && edited.at(contentEnd - 1) != QLatin1Char(',')
            && edited.at(contentEnd - 1) != QLatin1Char('E')) {
            edited.insert(contentEnd, QLatin1Char(','));
        }
        const auto tidy = documentSession_->normalizeChartSelection(edited, 0, 0, documentSession_->normalizeOptions());
        if (tidy.consumed && tidy.error.isEmpty()) {
            if (tidy.transaction.hasEdit) {
                edited.replace(tidy.transaction.replacementStart,
                               tidy.transaction.replacementEnd - tidy.transaction.replacementStart,
                               tidy.transaction.replacementText);
            }
            const miacode::editor::TextReplacement change = miacode::editor::minimalReplacement(before, edited);
            result.transaction.replacementStart = change.start;
            result.transaction.replacementEnd = change.end;
            result.transaction.replacementText = change.text;
            result.transaction.anchor = change.start;
            result.transaction.position = change.start;
            result.transaction.touchTokenStart = -1;
        }
    }
    if (applyEditorTransaction(result) && result.transaction.hasEdit) {
        takeFocus();
        if (followEdit) publishTouchAnchor(result.transaction.touchTokenStart);
    }
}
void ScintillaEditorBridge::publishAuthoringStatus(const QString& message)
{
    if (!syncController_) return;
    // Where each copied note lands relative to the paste point, for the
    // lane editor's paste preview.
    QVariantList layout;
    for (const miacode::editor::NoteClipEntry& entry : std::as_const(noteClip_)) {
        const auto info = miacode::editor::classifyNoteItem(entry.item);
        const double bars = double(entry.numerator) / double(qMax<qint64>(1, entry.denominator));
        QList<int> rows;
        if (info.laneShorthand) {
            for (const QChar ch : entry.item) rows.append(ch.digitValue());
        } else if (info.target.size() == 1 && info.target.at(0).isDigit()) {
            rows.append(info.target.at(0).digitValue());
        } else if (!info.target.isEmpty()) {
            rows.append(9);
        }
        for (const int row : std::as_const(rows)) {
            layout.append(QVariantMap{{QStringLiteral("row"), row}, {QStringLiteral("bars"), bars}});
        }
    }
    syncController_->publishNoteAuthoringStatus(noteSelection_.notes.size(), noteClip_.size(), message, layout);
}
void ScintillaEditorBridge::undo()
{
    QGuiApplication::inputMethod()->commit();
    if (controller_) controller_->closeCompletion();
    send(SCI_UNDO);
    publishTouchUndoAnchor();
}
void ScintillaEditorBridge::redo()
{
    QGuiApplication::inputMethod()->commit();
    if (controller_) controller_->closeCompletion();
    send(SCI_REDO);
    publishTouchUndoAnchor();
}
void ScintillaEditorBridge::publishTouchAnchor(int position)
{
    const int bounded = qBound(0, position, int(document_.text().size()));
    const int line = document_.lineAt(bounded);
    syncController_->setTouchPadPreviewAnchor(documentSession_->currentDifficultyId(),
        documentSession_->documentRevision(), line + 1, bounded - document_.lineStart(line) + 1);
}
void ScintillaEditorBridge::publishTouchUndoAnchor()
{
    if (pendingTouchAnchor_ >= 0 && documentSession_ && syncController_)
        publishTouchAnchor(pendingTouchAnchor_);
    pendingTouchAnchor_ = -1;
}
void ScintillaEditorBridge::cut() { send(SCI_CUT); }
void ScintillaEditorBridge::copy() { send(SCI_COPY); }
void ScintillaEditorBridge::paste() { send(SCI_PASTE); }
void ScintillaEditorBridge::selectAll() { send(SCI_SELECTALL); refreshSelection(true); }
void ScintillaEditorBridge::select(int anchor, int position)
{
    send(SCI_SETSELECTION, document_.bytePosition(position), document_.bytePosition(anchor));
    refreshSelection(!programmatic_);
}
void ScintillaEditorBridge::selectCurrentLine()
{
    const int line = cursorLine() - 1;
    select(document_.utf16Position(send(SCI_POSITIONFROMLINE, line)), document_.utf16Position(send(SCI_GETLINEENDPOSITION, line)));
}
void ScintillaEditorBridge::jumpToLine(int line)
{
    const int position = document_.utf16Position(send(SCI_POSITIONFROMLINE, qBound(0, line - 1, int(send(SCI_GETLINECOUNT)) - 1)));
    setCursorPosition(position);
    centerCursorInView();
    takeFocus();
}
void ScintillaEditorBridge::centerCursorInView() { revealPosition(cursorPosition(), true); }
bool ScintillaEditorBridge::applyEditorTransaction(const miacode::editor::SimaiTextEditResult& result)
{
    if (!result.error.isEmpty() && documentSession_) {
        qWarning().noquote() << "MiaCode editor:" << result.error;
    }
    if (!result.consumed) return false;
    const auto& tx = result.transaction;
    const bool caretChanged = tx.anchor != document_.utf16Position(send(SCI_GETANCHOR))
        || tx.position != cursorPosition();
    if (tx.hasEdit || caretChanged) {
        QScopedValueRollback guard(synchronizing_, true);
        if (tx.hasEdit) {
            send(SCI_BEGINUNDOACTION);
            send(SCI_SETTARGETSTART, document_.bytePosition(tx.replacementStart));
            send(SCI_SETTARGETEND, document_.bytePosition(tx.replacementEnd));
            const QByteArray replacement = tx.replacementText.toUtf8();
            sends(SCI_REPLACETARGET, replacement.size(), replacement.constData());
            if (tx.touchTokenStart >= 0) {
                const int token = nextTouchToken_++;
                touchUndoAnchors_.insert(token, {document_.scope(), tx.touchTokenStart});
                send(SCI_ADDUNDOACTION, token, 0);
            }
            send(SCI_ENDUNDOACTION);
        }
        select(tx.anchor, tx.position);
        send(SCI_SCROLLCARET);
    }
    if (tx.hasEdit) textMutated();
    else if (caretChanged) publishContext(!programmatic_);
    return true;
}
void ScintillaEditorBridge::acceptCompletionFromPopup()
{
    if (!controller_) return;
    applyEditorTransaction(controller_->acceptCompletion(document_.text(), document_.utf16Position(send(SCI_GETANCHOR)), cursorPosition()));
    takeFocus();
}
void ScintillaEditorBridge::dropDocument(const QString& key)
{
    QScopedValueRollback guard(synchronizing_, true);
    const bool current = document_.scope() == key;
    document_.drop(key);
    touchUndoAnchors_.removeIf([&key](auto it) { return it.value().scope == key; });
    if (current) {
        styler_.reset();
        bookmarks_.clear();
        overviewSyntaxRows_.clear();
        overviewMuriRows_.clear();
        emit bookmarksChanged();
        emit selectionChanged();
        emit availabilityChanged();
        scheduleOverviewProjectionRefresh(OverviewAllDirty);
    }
}
void ScintillaEditorBridge::configureSearch(const QString& query, const QString& replacement, bool matchCase, bool wholeWord)
{
    setFindText(query); setReplacementText(replacement);
    setFindOptions((matchCase ? SCFIND_MATCHCASE : 0) | (wholeWord ? SCFIND_WHOLEWORD : 0));
}
void ScintillaEditorBridge::seekPreviewToCaret()
{
    if (syncController_ && documentSession_ && selectionStart() == selectionEnd())
        syncController_->seekPreviewToEditorLocation(documentSession_->currentDifficultyId(), documentSession_->documentRevision(), cursorLine(), cursorColumn());
}
void ScintillaEditorBridge::exportSelectionRange()
{
    if (syncController_ && documentSession_ && selectionStart() != selectionEnd())
        syncController_->requestSelectionRangeExport(documentSession_->currentDifficultyId(), documentSession_->documentRevision(), selectionStart(), selectionEnd());
}
bool ScintillaEditorBridge::applyNormalization(const QVariantMap& options)
{
    return documentSession_ && applyEditorTransaction(documentSession_->normalizeChartSelection(document_.text(), selectionStart(), selectionEnd(), options));
}
bool ScintillaEditorBridge::applyChartTransform(const QString& operation)
{
    return documentSession_ && applyEditorTransaction(documentSession_->transformChartSelection(document_.text(), selectionStart(), selectionEnd(), operation));
}
bool ScintillaEditorBridge::createBookmarkAtLine(int line, const QString& title)
{
    if (!controller_ || line < 1 || line > document_.lineCount()) return false;
    const int start = document_.lineStart(line - 1);
    const int end = document_.utf16Position(send(SCI_GETLINEENDPOSITION, line - 1));
    return applyEditorTransaction(controller_->createBookmark(document_.text().mid(start, end - start), start, title));
}
bool ScintillaEditorBridge::renameBookmarkAtLine(int line, const QString& title)
{
    if (!controller_ || line < 1 || line > document_.lineCount()) return false;
    const int start = document_.lineStart(line - 1);
    const int end = document_.utf16Position(send(SCI_GETLINEENDPOSITION, line - 1));
    return applyEditorTransaction(controller_->renameBookmark(document_.text().mid(start, end - start), start, title));
}
bool ScintillaEditorBridge::deleteBookmarkAtLine(int line)
{
    if (!controller_ || line < 1 || line > document_.lineCount()) return false;
    const int start = document_.lineStart(line - 1);
    const int end = document_.utf16Position(send(SCI_GETLINEENDPOSITION, line - 1));
    return applyEditorTransaction(controller_->deleteBookmark(document_.text().mid(start, end - start), start));
}
void ScintillaEditorBridge::beginUserInteraction()
{
    viewportToRestore_.reset();
    if (pendingNavigation_) {
        const auto sequence = pendingNavigation_->sequence;
        pendingNavigation_.reset();
        syncController_->acknowledgeNavigation(sequence, false);
    }
    if (syncController_ && documentSession_)
        syncController_->beginPointerInteraction(documentSession_->currentDifficultyId(), documentSession_->documentRevision());
}
void ScintillaEditorBridge::setKeyboardSuspended(bool suspended)
{
    if (keyboardSuspended_ == suspended) return;
    keyboardSuspended_ = suspended;
    emit bindingsChanged();
}
void ScintillaEditorBridge::takeFocus()
{
    if (!keyboardSuspended_) forceActiveFocus();
}
void ScintillaEditorBridge::keyPressEvent(QKeyEvent* event)
{
    if (keyboardSuspended_) { event->ignore(); return; }
    if (event->matches(QKeySequence::Find)) { emit findRequested(); event->accept(); return; }
    if (event->key() == Qt::Key_H && event->modifiers().testFlag(Qt::ControlModifier)
        && !event->modifiers().testFlag(Qt::AltModifier)) {
        // Suppress ScintillaQuick's built-in find/replace shortcut.
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Menu || (event->key() == Qt::Key_F10 && event->modifiers().testFlag(Qt::ShiftModifier))) {
        updatePolish();
        const auto rect = cursorRectangle(); emit contextMenuRequested(rect.x(), rect.bottom()); event->accept(); return;
    }
    if (event->matches(QKeySequence::Undo)) { undo(); event->accept(); return; }
    if (event->matches(QKeySequence::Redo)) { redo(); event->accept(); return; }
    if (event->matches(QKeySequence::Cut)) { cut(); event->accept(); return; }
    if (event->matches(QKeySequence::Copy)) { copy(); event->accept(); return; }
    if (event->matches(QKeySequence::Paste)) { paste(); event->accept(); return; }
    if (event->matches(QKeySequence::SelectAll)) { selectAll(); event->accept(); return; }
    if (event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown) beginUserInteraction();
    if (controller_) {
        const QString input = event->text();
        // Only DSL pairing, completion and half-width conversion intercept
        // typing. Scintilla executes ordinary editing and clipboard commands.
        const bool smartGlyph = controller_->autoCompletionEnabled() &&
            (QStringLiteral("(){}[]h").contains(input) && input.size() == 1);
        const bool convert = controller_->halfWidthInputEnabled() && normalizeSimaiInput(input) != input;
        const bool completionKey = controller_->completionActive() &&
            (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down || event->key() == Qt::Key_Escape
             || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Tab);
        if (completionKey || smartGlyph || convert || event->key() == Qt::Key_Backspace) {
            const auto tx = controller_->processKey(document_.text(), document_.utf16Position(send(SCI_GETANCHOR)), cursorPosition(), input, event->key(), int(event->modifiers()));
            if (applyEditorTransaction(tx) || tx.suppressFallbackInsert) { event->accept(); return; }
        }
    }
    ScintillaQuick_item::keyPressEvent(event);
    refreshSelection(true);
}
void ScintillaEditorBridge::mousePressEvent(QMouseEvent* event)
{
    beginUserInteraction();
    if (event->button() == Qt::RightButton) {
        int gutterWidth = 0;
        for (int margin = 0; margin < send(SCI_GETMARGINS); ++margin)
            gutterWidth += send(SCI_GETMARGINWIDTHN, margin);
        if (event->position().x() >= 0 && event->position().x() < gutterWidth) {
            const int position = send(SCI_POSITIONFROMPOINT, gutterWidth + send(SCI_GETMARGINLEFT), qRound(event->position().y()));
            const int line = send(SCI_LINEFROMPOSITION, position) + 1;
            takeFocus();
            emit bookmarkMenuRequested(line, event->position().x(), event->position().y());
            event->accept();
            return;
        }
    }
    ScintillaQuick_item::mousePressEvent(event);
    if (event->button() == Qt::RightButton) {
        emit selectionChanged();
        updatePolish();
        emit contextMenuRequested(event->position().x(), event->position().y());
    }
    refreshSelection(!programmatic_);
}
void ScintillaEditorBridge::mouseReleaseEvent(QMouseEvent* event)
{
    ScintillaQuick_item::mouseReleaseEvent(event);
    refreshSelection(!programmatic_);
    if (event->button() == Qt::LeftButton && (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) seekPreviewToCaret();
}
void ScintillaEditorBridge::wheelEvent(QWheelEvent* event)
{
    beginUserInteraction();
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    if (!autoWrap_ && (shift || std::abs(event->pixelDelta().x()) > std::abs(event->pixelDelta().y())
        || std::abs(event->angleDelta().x()) > std::abs(event->angleDelta().y()))) {
        const int pixels = shift ? event->pixelDelta().y() : event->pixelDelta().x();
        const int angle = shift ? event->angleDelta().y() : event->angleDelta().x();
        const qreal delta = pixels ? pixels : qreal(angle) / 120
            * send(SCI_TEXTWIDTH, STYLE_DEFAULT, reinterpret_cast<sptr_t>("M"))
            * QGuiApplication::styleHints()->wheelScrollLines();
        scrollHorizontal(int(send(SCI_GETXOFFSET)) - qRound(delta));
        event->accept();
        return;
    }
    if (event->phase() == Qt::ScrollBegin) wheelRemainder_ = 0;
    if (!event->pixelDelta().isNull()
        && (event->phase() != Qt::NoScrollPhase || event->angleDelta().isNull())) {
        wheelRemainder_ -= qreal(event->pixelDelta().y()) / qMax(1, int(send(SCI_TEXTHEIGHT, 0)));
    } else {
        wheelRemainder_ -= qreal(event->angleDelta().y()) / 120 * QGuiApplication::styleHints()->wheelScrollLines();
    }
    const int rows = int(std::trunc(wheelRemainder_));
    wheelRemainder_ -= rows;
    if (rows != 0) scrollVertical(int(send(SCI_GETFIRSTVISIBLELINE)) + rows);
    if (event->phase() == Qt::ScrollEnd) wheelRemainder_ = 0;
    event->accept();
}
void ScintillaEditorBridge::inputMethodEvent(QInputMethodEvent* event)
{
    if (keyboardSuspended_) { event->ignore(); return; }
    if (controller_ && controller_->imeInputDisabled()) { event->accept(); return; }
    {
        QScopedValueRollback guard(handlingIme_, true);
        imeComposing_ = !event->preeditString().isEmpty() && event->commitString().isEmpty();
        QInputMethodEvent normalized(event->preeditString(), event->attributes());
        const QString commit = controller_ && controller_->halfWidthInputEnabled() ? normalizeSimaiInput(event->commitString()) : event->commitString();
        normalized.setCommitString(commit, event->replacementStart(), event->replacementLength());
        const bool committed = !commit.isEmpty();
        if (committed) send(SCI_BEGINUNDOACTION);
        ScintillaQuick_item::inputMethodEvent(&normalized);
        if (controller_ && committed && commit.size() == 1 && controller_->autoCompletionEnabled()
            && !controller_->overwriteMode() && !controller_->completionActive()) {
            const QChar glyph = commit.front();
            const QChar closing = miacode::editor::closingBracketFor(glyph);
            bool closingPresent = false;
            if (!closing.isNull()) {
                const int caret = cursorPosition();
                if (caret >= document_.text().size() || document_.text().at(caret) != closing) {
                    const QByteArray closingText = QString(closing).toUtf8();
                    sends(SCI_INSERTTEXT, send(SCI_GETCURRENTPOS), closingText.constData());
                }
                closingPresent = true;
            }
            controller_->triggerCompletion(glyph, document_.text(), cursorPosition(), closingPresent);
        }
        if (committed) send(SCI_ENDUNDOACTION);
        event->setAccepted(normalized.isAccepted());
    }
    emit imeComposingChanged();
    if (imeComposing_) {
        if (syncController_) syncController_->setTouchPadControlHold(false);
        publishContext(false);
    } else textMutated();
}
}
