#include "app/ui/timeline/TimelineModel.h"

#include "app/ui/preferences/LocaleService.h"
#include <QCoreApplication>

namespace miacode::ui {

TimelineModel::TimelineModel(miacode::ShellNotifications& notifications,
                                   miacode::TimelineSurface*& surfaceSlot,
                                   QObject* parent)
    : QObject(parent)
    , notifications_(&notifications)
    , surfaceSlot_(&surfaceSlot)
{
    connect(notifications_, &miacode::ShellNotifications::presentationChanged,
            this, &TimelineModel::tabsChanged);
    connect(&miacode::LocaleService::instance(), &miacode::LocaleService::languageChanged,
            this, [this](const QString&) { emit localeLabelsChanged(); });
}

QObject* TimelineModel::stateBridge() const
{
    return surface() != nullptr ? surface()->timelineStateBridge() : nullptr;
}

QString TimelineModel::currentTabId() const
{
    return surface() != nullptr ? surface()->bottomTabsCurrentTabId() : QString();
}

bool TimelineModel::panelVisible() const
{
    return surface() != nullptr && surface()->bottomTabsVisible();
}

bool TimelineModel::timelineTabVisible() const
{
    return surface() != nullptr && surface()->timelineTabVisible();
}

bool TimelineModel::validationTabVisible() const
{
    return surface() != nullptr && surface()->validationTabVisible();
}

bool TimelineModel::muriTabVisible() const
{
    return surface() != nullptr && surface()->muriTabVisible();
}

QString TimelineModel::timelineTabLabel() const
{
    return qtTrId("window.timeline");
}

QString TimelineModel::validationTabLabel() const
{
    return qtTrId("window.syntax");
}

QString TimelineModel::muriTabLabel() const
{
    return qtTrId("window.muri");
}

QString TimelineModel::followCodeLabel() const
{
    return qtTrId("shell.follow_code");
}

void TimelineModel::setCurrentTabId(const QString& tabId)
{
    miacode::TimelineSurface* current = surface();
    if (current == nullptr || tabId.trimmed().isEmpty() || tabId == currentTabId()) {
        return;
    }
    current->setBottomTabsCurrentTabId(current->issueCommandStamp(), tabId);
}

void TimelineModel::headerNavigate(double second)
{
    if (auto* current = surface(); current != nullptr) {
        current->navigateToSecond(current->issueCommandStamp(), second);
    }
}

void TimelineModel::wheelNavigate(double second)
{
    if (auto* current = surface(); current != nullptr) {
        current->wheelNavigateToSecond(current->issueCommandStamp(), second);
    }
}

void TimelineModel::centerNavigate(double second)
{
    if (auto* current = surface(); current != nullptr) {
        current->centerOnSecond(current->issueCommandStamp(), second);
    }
}

void TimelineModel::dragStarted()
{
    if (auto* current = surface(); current != nullptr) {
        current->timelineDragStarted(current->issueCommandStamp());
    }
}

void TimelineModel::dragFinished(double second)
{
    if (auto* current = surface(); current != nullptr) {
        current->timelineDragFinished(current->issueCommandStamp(), second);
    }
}

void TimelineModel::authoringGesture(int kind, bool secondary, double second, double endSecond,
                                     int lane, int endLane, bool onNote)
{
    using Kind = miacode::TimelineAuthoringGesture::Kind;
    // Slides carry a shape and come through authoringSlide.
    // Slides, touches and BPM carry text and come through their own calls.
    const bool plain = (kind >= int(Kind::Click) && kind <= int(Kind::Paste) && kind != int(Kind::Slide))
        || kind == int(Kind::MoveSelection);
    if (!plain) {
        return;
    }
    if (auto* current = surface(); current != nullptr) {
        current->timelineAuthoring(current->issueCommandStamp(),
                                   {Kind(kind), secondary, second, endSecond, lane, endLane, onNote});
    }
}

void TimelineModel::authoringSlide(double second, double endSecond, int lane, int endLane, const QString& shape,
                                   bool replace)
{
    if (auto* current = surface(); current != nullptr) {
        miacode::TimelineAuthoringGesture gesture;
        gesture.kind = miacode::TimelineAuthoringGesture::Kind::Slide;
        gesture.second = second;
        gesture.endSecond = endSecond;
        gesture.lane = lane;
        gesture.endLane = endLane;
        gesture.shape = shape;
        gesture.onNote = replace;
        current->timelineAuthoring(current->issueCommandStamp(), gesture);
    }
}

void TimelineModel::authoringTouch(double second, const QString& pad, bool add)
{
    if (auto* current = surface(); current != nullptr) {
        miacode::TimelineAuthoringGesture gesture;
        gesture.kind = miacode::TimelineAuthoringGesture::Kind::Touch;
        gesture.secondary = add;
        gesture.second = second;
        gesture.endSecond = second;
        gesture.lane = 9;
        gesture.endLane = 9;
        gesture.shape = pad;
        current->timelineAuthoring(current->issueCommandStamp(), gesture);
    }
}

void TimelineModel::authoringBpm(double second, const QString& bpm)
{
    if (auto* current = surface(); current != nullptr) {
        miacode::TimelineAuthoringGesture gesture;
        gesture.kind = miacode::TimelineAuthoringGesture::Kind::Bpm;
        gesture.second = second;
        gesture.endSecond = second;
        gesture.shape = bpm.trimmed();
        current->timelineAuthoring(current->issueCommandStamp(), gesture);
    }
}

void TimelineModel::userInteractionStarted()
{
    if (auto* current = surface(); current != nullptr) {
        current->timelineUserInteractionStarted(current->issueCommandStamp());
    }
}

void TimelineModel::surfaceReady()
{
    if (auto* current = surface(); current != nullptr) {
        current->noteTimelineSurfaceReady(current->issueCommandStamp());
    }
}

void TimelineModel::followPreviewToggled(bool enabled)
{
    if (auto* current = surface(); current != nullptr) {
        current->setFollowPreviewEnabled(current->issueCommandStamp(), enabled);
    }
}

}  // namespace miacode::ui
