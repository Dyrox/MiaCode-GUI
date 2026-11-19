#include "preview/quick_scene/PreviewQuickTouchHoverLayer.h"

#include "preview/quick_scene/PreviewQuickCircleNodes.h"
#include "core/scene/NoteAuthoringGeometry.h"
#include "core/scene/PreviewCircleDescriptor.h"
#include "core/scene/PreviewSceneConstants.h"
#include "core/scene/PreviewSceneGeometry.h"
#include "core/scene/PreviewSceneMath.h"
#include "core/scene/TouchPadAuthoringState.h"

QSGNode* PreviewQuickTouchHoverLayer::updateNode(
    QSGNode* oldNode,
    const miacode::preview::scene::PreviewFrameState& state,
    const QSize& renderSize,
    QQuickWindow* window,
    PreviewTextureRepository* textures
) const
{
    Q_UNUSED(window);
    Q_UNUSED(textures);

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

    if (circles.isEmpty()) {
        return nullptr;
    }
    return buildPreviewCircleNodeTree(oldNode, circles);
}
