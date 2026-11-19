import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

// Graphical charting tools. Clicks on the preview write into the comma token
// at the editor cursor; the controller owns the tool state.
Item {
    id: root

    required property var noteAuthoring
    readonly property int tool: root.noteAuthoring ? root.noteAuthoring.tool : 0
    readonly property bool active: root.tool !== 0
    readonly property bool laneTool: root.tool >= 1 && root.tool <= 3
    readonly property bool touchTool: root.tool === 4 || root.tool === 5

    implicitHeight: flow.implicitHeight + 2 * Theme.chromePadding

    Flow {
        id: flow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: Theme.panelPadding
        anchors.rightMargin: Theme.panelPadding
        spacing: 2

        Repeater {
            model: [
                { tool: 0, label: qsTrId("qml.charting.off") },
                { tool: 1, label: qsTrId("qml.charting.tap") },
                { tool: 2, label: qsTrId("qml.charting.hold") },
                { tool: 3, label: qsTrId("qml.charting.slide") },
                { tool: 4, label: qsTrId("qml.charting.touch") },
                { tool: 5, label: qsTrId("qml.charting.touch_hold") }
            ]
            delegate: AppChoiceButton {
                id: toolButton
                required property var modelData
                text: modelData.label
                checked: root.tool === modelData.tool
                // Mouse, keyboard and accessibility presses all flip `checked`
                // first; select the tool and restore the binding from there.
                onCheckedChanged: {
                    if (toolButton.checked === (root.tool === toolButton.modelData.tool))
                        return
                    if (toolButton.checked)
                        root.noteAuthoring.tool = toolButton.modelData.tool
                    toolButton.checked = Qt.binding(() => root.tool === toolButton.modelData.tool)
                }

                Tooltip {
                    visible: parent.hovered && modelData.tool !== 0
                    text: qsTrId("qml.charting.hint")
                }
            }
        }

        AppChoiceButton {
            visible: root.laneTool
            text: qsTrId("qml.charting.break")
            checked: root.noteAuthoring.breakNote
            onCheckedChanged: root.noteAuthoring.breakNote = checked
        }
        AppChoiceButton {
            visible: root.laneTool
            text: qsTrId("qml.charting.ex")
            checked: root.noteAuthoring.exNote
            onCheckedChanged: root.noteAuthoring.exNote = checked
        }
        AppChoiceButton {
            visible: root.tool === 1
            text: qsTrId("qml.charting.star")
            checked: root.noteAuthoring.starTap
            onCheckedChanged: root.noteAuthoring.starTap = checked
        }
        AppChoiceButton {
            visible: root.tool === 3
            text: qsTrId("qml.charting.wifi")
            checked: root.noteAuthoring.wifi
            onCheckedChanged: root.noteAuthoring.wifi = checked
        }
        AppChoiceButton {
            visible: root.touchTool
            text: qsTrId("qml.charting.firework")
            checked: root.noteAuthoring.firework
            onCheckedChanged: root.noteAuthoring.firework = checked
        }

        // Hold length is used when both clicks land on the same beat.
        AppTextField {
            visible: root.tool === 2 || root.tool === 5 || root.tool === 3
            width: 72
            text: root.tool === 3 ? root.noteAuthoring.slideLength : root.noteAuthoring.holdLength
            horizontalAlignment: TextInput.AlignHCenter
            onEditingFinished: {
                if (root.tool === 3)
                    root.noteAuthoring.slideLength = text
                else
                    root.noteAuthoring.holdLength = text
                text = Qt.binding(() => root.tool === 3 ? root.noteAuthoring.slideLength
                                                        : root.noteAuthoring.holdLength)
            }

            Tooltip {
                visible: parent.hovered
                text: root.tool === 3 ? qsTrId("qml.charting.slide_length")
                                      : qsTrId("qml.charting.hold_length")
            }
        }

        Text {
            visible: root.noteAuthoring.pendingTarget !== ""
            height: Theme.controlMinHeight
            verticalAlignment: Text.AlignVCenter
            leftPadding: 6
            text: qsTrId("qml.charting.hold_pending").arg(root.noteAuthoring.pendingTarget)
            color: Theme.colors.text.secondary
            font.family: Theme.uiFont
            font.pixelSize: Theme.uiFontSize
        }
    }
}
