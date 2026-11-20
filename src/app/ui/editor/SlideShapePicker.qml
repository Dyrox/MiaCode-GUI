import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

// GUI charting's slide shape list, as in classic editors: hovering a shape
// draws its course on the preview ring and a click takes it. On the ring, a click on a key changes the end and a stroke from the
// star picks the shape it draws.
Rectangle {
    id: root

    required property var noteAuthoring
    readonly property bool chaining: root.noteAuthoring.slideChain

    width: 200
    height: column.implicitHeight + 12
    radius: 6
    color: Theme.surfaceColor(Theme.colors.background.elevated)
    border.color: Theme.colors.accent.focus
    border.width: 1

    // Keeps presses and hover from reaching the panel underneath.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
    }

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 6
        spacing: 2

        Text {
            Layout.fillWidth: true
            text: root.noteAuthoring.slidePickChain !== ""
                  ? root.noteAuthoring.slidePickChain + (root.noteAuthoring.slidePickEnd > 0 ? " …" : "")
                  : qsTrId("qml.charting.slide_pick").arg(root.noteAuthoring.slidePickFrom)
                        .arg(root.noteAuthoring.slidePickEnd)
            color: Theme.colors.text.primary
            font.family: Theme.uiFont
            font.pixelSize: Theme.uiFontSize
            elide: Text.ElideMiddle
        }
        Text {
            Layout.fillWidth: true
            Layout.bottomMargin: 2
            text: root.noteAuthoring.slidePickEnd > 0 ? qsTrId("qml.charting.slide_pick_hint")
                                                      : qsTrId("qml.charting.slide_pick_next")
            color: Theme.colors.text.secondary
            font.family: Theme.uiFont
            font.pixelSize: Theme.uiFontSize - 1
            wrapMode: Text.Wrap
        }

        Repeater {
            model: root.noteAuthoring.slidePickShapes
            delegate: AbstractButton {
                id: shapeButton
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredHeight: 26
                hoverEnabled: true
                Accessible.name: shapeButton.modelData.key
                onHoveredChanged: root.noteAuthoring.previewSlideShape(shapeButton.hovered ? shapeButton.modelData.key : "")
                onClicked: root.noteAuthoring.pickSlideShape(shapeButton.modelData.key)
                background: Rectangle {
                    radius: 4
                    color: shapeButton.hovered ? Theme.colors.state.hover : "transparent"
                }
                contentItem: RowLayout {
                    spacing: 8
                    Text {
                        Layout.preferredWidth: 64
                        leftPadding: 6
                        text: shapeButton.modelData.key
                        color: Theme.colors.text.active
                        font.family: Theme.codeFont.family
                        font.pixelSize: Theme.uiFontSize
                    }
                    Text {
                        Layout.fillWidth: true
                        text: shapeButton.modelData.label
                        color: Theme.colors.text.secondary
                        font.family: Theme.uiFont
                        font.pixelSize: Theme.uiFontSize
                        elide: Text.ElideRight
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            spacing: 4

            AppButton {
                Layout.fillWidth: true
                visible: root.chaining
                text: qsTrId("qml.charting.slide_pick_undo")
                onClicked: root.noteAuthoring.undoSlidePick()
            }
            AppButton {
                Layout.fillWidth: true
                visible: root.chaining
                enabled: root.noteAuthoring.slidePickChain !== ""
                text: qsTrId("qml.charting.slide_pick_confirm")
                onClicked: root.noteAuthoring.confirmSlidePick()
            }
            AppButton {
                Layout.fillWidth: true
                text: qsTrId("qml.charting.slide_pick_cancel")
                onClicked: root.noteAuthoring.cancelSlidePick()
            }
        }
    }
}
