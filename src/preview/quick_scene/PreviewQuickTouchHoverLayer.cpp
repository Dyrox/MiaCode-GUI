#include "preview/quick_scene/PreviewQuickTouchHoverLayer.h"

#include "preview/quick_scene/PreviewQuickCircleNodes.h"
#include "preview/quick_scene/PreviewQuickSpriteNodes.h"
#include "core/scene/NoteAuthoringGeometry.h"
#include "core/scene/PreviewCircleDescriptor.h"
#include "core/scene/PreviewSceneConstants.h"
#include "core/scene/PreviewSceneGeometry.h"
#include "core/scene/PreviewSceneMath.h"
#include "core/scene/TouchPadAuthoringState.h"

#include <QSGNode>
#include <QtMath>

QSGNode* PreviewQuickTouchHoverLayer::updateNode(
    QSGNode* oldNode,
    const miacode::preview::scene::PreviewFrameState& state,
    const QSize& renderSize,
    QQuickWindow* window,
    PreviewTextureRepository* textures
) const
{

    namespace scene = miacode::preview::scene;
    const QRectF playfieldRect = scene::playfieldRectForStage(
        scene::stageRectForSize(renderSize),
        state.render.layoutSquareScale
    );
    const qreal strokeWidth = qMax<qreal>(1.0, scene::mapLogicalLengthToRect(1.4, playfieldRect));
    scene::PreviewCircleDescriptors circles;

    const QString hoveredPad = state.hoveredTouchPad.trimmed().toUpper();
    const QPointF padPoint = scene::touchPadLogicalPoint(hoveredPad);
    if (!hoveredPad.isEmpty() && !(qFuzzyIsNull(padPoint.x()) && qFuzzyIsNull(padPoint.y()))) {
        const qreal logicalRadius = hoveredPad == QLatin1String("C")
            ? scene::kTouchPadCenterHitRadiusLogical
            : scene::kTouchPadHitRadiusLogical;
        const qreal radius = scene::mapLogicalLengthToRect(logicalRadius, playfieldRect);
        scene::PreviewCircleDescriptor circle;
        circle.center = scene::mapLogicalPointToRect(padPoint, playfieldRect);
        circle.radiusX = radius;
        circle.radiusY = radius;
        const bool pressed = state.pressedTouchPad.trimmed().toUpper() == hoveredPad;
        const auto style = scene::touchPadAuthoringVisualStyle(pressed);
        circle.fillColor = style.fill;
        circle.strokeColor = style.stroke;
        circle.strokeWidth = strokeWidth;
        circles.append(circle);
    }

    // The lane hovered in the GUI lane editor: its button lit on the ring.
    if (!state.authoringCueTarget.isEmpty()) {
        const qreal radius = scene::mapLogicalLengthToRect(
            scene::noteAuthoringTargetRadiusLogical(state.authoringCueTarget), playfieldRect);
        scene::PreviewCircleDescriptor cue;
        cue.center = scene::mapLogicalPointToRect(
            scene::noteAuthoringTargetLogicalPoint(state.authoringCueTarget), playfieldRect);
        cue.radiusX = radius;
        cue.radiusY = radius;
        cue.fillColor = QColor(120, 220, 255, 70);
        cue.strokeColor = QColor(120, 220, 255, 230);
        cue.strokeWidth = strokeWidth * 2.0;
        circles.append(cue);
    }

    if (state.noteAuthoringTool != 0) {
        const auto appendTarget = [&](const QString& target, const scene::TouchPadAuthoringVisualStyle& style) {
            if (target.isEmpty()) {
                return;
            }
            const qreal radius = scene::mapLogicalLengthToRect(
                scene::noteAuthoringTargetRadiusLogical(target), playfieldRect);
            scene::PreviewCircleDescriptor circle;
            circle.center = scene::mapLogicalPointToRect(
                scene::noteAuthoringTargetLogicalPoint(target), playfieldRect);
            circle.radiusX = radius;
            circle.radiusY = radius;
            circle.fillColor = style.fill;
            circle.strokeColor = style.stroke;
            circle.strokeWidth = strokeWidth;
            circles.append(circle);
        };
        // Pickers show every target they take, faintly, under the rest.
        if (state.authoringShowTargets) {
            const scene::TouchPadAuthoringVisualStyle faint{QColor(255, 255, 255, 18), QColor(255, 255, 255, 70)};
            const auto tool = scene::noteAuthoringToolFromInt(state.noteAuthoringTool);
            if (scene::noteAuthoringToolUsesLanes(tool)) {
                for (int lane = 1; lane <= 8; ++lane) {
                    appendTarget(QString::number(lane), faint);
                }
            } else {
                for (const QString& pad : scene::touchPadTokens()) {
                    appendTarget(pad, faint);
                }
            }
        }
        // The slide shape on offer: dotted only when the skin's arrows are
        // not available for it.
        if (state.authoringGuideArrows.isEmpty() && state.authoringGuidePath.size() >= 2) {
            const qreal guideRadius = qMax<qreal>(1.5, scene::mapLogicalLengthToRect(5.0, playfieldRect));
            const QColor guideColor(120, 220, 255, 220);
            constexpr qreal kGuideSpacing = 14.0;
            qreal carried = 0.0;
            for (int index = 1; index < state.authoringGuidePath.size(); ++index) {
                const QPointF from = state.authoringGuidePath.at(index - 1);
                const QPointF to = state.authoringGuidePath.at(index);
                const QPointF delta = to - from;
                const qreal length = qSqrt(QPointF::dotProduct(delta, delta));
                qreal along = carried;
                while (along < length) {
                    scene::PreviewCircleDescriptor dot;
                    dot.center = scene::mapLogicalPointToRect(from + delta * (along / length), playfieldRect);
                    dot.radiusX = guideRadius;
                    dot.radiusY = guideRadius;
                    dot.fillColor = guideColor;
                    circles.append(dot);
                    along += kGuideSpacing;
                }
                carried = along - length;
            }
        }
        appendTarget(state.pendingAuthoringTarget, scene::noteAuthoringPendingVisualStyle());
        appendTarget(state.pressedAuthoringTarget, scene::touchPadAuthoringVisualStyle(true));
        if (state.hoveredAuthoringTarget != state.pressedAuthoringTarget) {
            appendTarget(state.hoveredAuthoringTarget, scene::touchPadAuthoringVisualStyle(false));
        }
        // The slide being drawn, as a dotted trail.
        const qreal dotRadius = qMax<qreal>(1.5, scene::mapLogicalLengthToRect(4.0, playfieldRect));
        const auto pathStyle = scene::touchPadAuthoringVisualStyle(true);
        for (const QPointF& point : state.authoringPath) {
            scene::PreviewCircleDescriptor dot;
            dot.center = scene::mapLogicalPointToRect(point, playfieldRect);
            dot.radiusX = dotRadius;
            dot.radiusY = dotRadius;
            dot.fillColor = pathStyle.stroke;
            circles.append(dot);
        }
    }

    // The slide shape on offer in the skin's own track arrows.
    scene::PreviewSpriteDescriptors sprites;
    if (state.noteAuthoringTool != 0) {
        const qreal canvasScale = playfieldRect.width() / scene::kLogicalCanvasSize;
        for (const scene::NoteAuthoringGuideArrow& arrow : state.authoringGuideArrows) {
            const QImage* image = &state.skin.slideTrackImage;
            if (arrow.wifiImage >= 0 && !state.skin.wifiImages.isEmpty()) {
                image = &state.skin.wifiImages.at(qBound(0, arrow.wifiImage, int(state.skin.wifiImages.size()) - 1));
            }
            if (image->isNull()) {
                continue;
            }
            scene::PreviewSpriteDescriptor sprite;
            sprite.image = image;
            sprite.center = scene::mapLogicalPointToRect(arrow.point, playfieldRect);
            sprite.width = qMax<qreal>(1.0, qRound(image->width() * canvasScale * scene::kSlideTrackScale));
            sprite.height = qMax<qreal>(1.0, qRound(image->height() * canvasScale * scene::kSlideTrackScale));
            sprite.rotationDegrees = arrow.rotationDegrees;
            sprite.opacity = 0.9;
            sprites.append(sprite);
        }
    }

    if (circles.isEmpty() && sprites.isEmpty()) {
        return nullptr;
    }
    // Two holders under one node: the arrows, then the circles over them.
    QSGNode* container = oldNode != nullptr ? oldNode : new QSGNode;
    while (container->childCount() < 2) {
        container->appendChildNode(new QSGNode);
    }
    const auto updateHolder = [](QSGNode* holder, QSGNode* built) {
        QSGNode* child = holder->firstChild();
        while (child != nullptr) {
            QSGNode* next = child->nextSibling();
            if (child != built) {
                holder->removeChildNode(child);
                delete child;
            }
            child = next;
        }
        if (built != nullptr && built->parent() != holder) {
            holder->appendChildNode(built);
        }
    };
    QSGNode* spriteHolder = container->childAtIndex(0);
    QSGNode* circleHolder = container->childAtIndex(1);
    updateHolder(spriteHolder, sprites.isEmpty() ? nullptr
        : buildPreviewSpriteNodeTree(spriteHolder->firstChild(), sprites, window, textures, state.playheadSeconds,
                                     "touch_hover"));
    updateHolder(circleHolder, circles.isEmpty() ? nullptr
        : buildPreviewCircleNodeTree(circleHolder->firstChild(), circles));
    return container;
}
