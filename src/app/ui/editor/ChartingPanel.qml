import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import MiaCode.UI

// GUI charting panel beside the vertical lane editor, laid out like a classic
// maimai editor: editor settings, Tap and Touch tiles drawn with the current
// skin and their marks, then note operations.
Rectangle {
    id: root

    required property var noteAuthoring
    // The lane editor this panel drives, for its zoom.
    property Item laneEditor: null
    readonly property int tool: root.noteAuthoring ? root.noteAuthoring.tool : 0
    // A shortcut as the platform writes it: "⇧⌘F" on macOS, "Ctrl+Shift+F"
    // elsewhere. `mods` uses "Cmd" for the command key (Ctrl off macOS).
    function keys(mods, key) {
        if (Qt.platform.os === "osx") {
            const symbols = { "Ctrl": "⌃", "Option": "⌥", "Shift": "⇧", "Cmd": "⌘" }
            return ["Ctrl", "Option", "Shift", "Cmd"].filter(m => mods.indexOf(m) >= 0)
                       .map(m => symbols[m]).join("") + key
        }
        const names = { "Cmd": "Ctrl", "Option": "Alt" }
        return mods.map(m => names[m] || m).concat([key]).join("+")
    }
    readonly property real labelWidth: 52
    // Zoom is shown relative to 75 px per second.
    readonly property real zoomBase: 75
    // Three columns of marks need room; narrow panels fall back to two.
    readonly property int optionColumns: root.width >= 440 ? 3 : 2

    color: Theme.surfaceColor(Theme.colors.background.panel)

    component Section: ColumnLayout {
        id: section
        required property string title
        default property alias content: body.data
        Layout.fillWidth: true
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: Theme.surfaceColor(Theme.colors.background.surface)
            Text {
                anchors.left: parent.left
                anchors.leftMargin: Theme.panelPadding
                anchors.verticalCenter: parent.verticalCenter
                text: section.title
                color: Theme.colors.text.primary
                font.family: Theme.uiFont
                font.pixelSize: Theme.uiFontSize + 1
            }
        }
        ColumnLayout {
            id: body
            Layout.fillWidth: true
            Layout.margins: Theme.panelPadding
            spacing: 8
        }
    }

    // A label in the left column and its controls.
    component LabeledRow: RowLayout {
        id: labeled
        required property string label
        default property alias content: controls.data
        Layout.fillWidth: true
        spacing: 8
        Text {
            Layout.preferredWidth: root.labelWidth
            Layout.alignment: Qt.AlignTop
            topPadding: 6
            text: labeled.label
            color: Theme.colors.text.secondary
            font.family: Theme.uiFont
            font.pixelSize: Theme.uiFontSize
        }
        RowLayout {
            id: controls
            Layout.fillWidth: true
            spacing: 4
        }
    }

    // One note kind: the skin's icon, its number key and its name.
    component NoteTile: AbstractButton {
        id: tile
        required property int toolValue
        required property string iconType
        required property string label
        readonly property bool selected: root.tool === tile.toolValue

        Layout.fillWidth: true
        Layout.preferredHeight: 64
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        onClicked: root.noteAuthoring.tool = tile.selected ? 0 : tile.toolValue
        Accessible.name: tile.label

        background: Rectangle {
            color: tile.selected ? Theme.colors.state.selected
                 : tile.hovered ? Theme.colors.state.hover
                 : Theme.surfaceColor(Theme.colors.background.control)
            border.width: tile.selected ? 1 : 0
            border.color: Theme.colors.accent.focus
        }
        contentItem: Item {
            Image {
                anchors.centerIn: parent
                anchors.verticalCenterOffset: -4
                width: 36
                height: 36
                sourceSize: Qt.size(72, 72)
                fillMode: Image.PreserveAspectFit
                smooth: true
                source: "image://noteicon/" + tile.iconType
            }
            Text {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 4
                text: "(" + tile.toolValue + ")"
                color: Theme.colors.text.secondary
                font.family: Theme.uiFont
                font.pixelSize: Theme.uiFontSize - 1
            }
            Text {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 4
                width: parent.width - 8
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
                text: tile.label
                color: tile.selected ? Theme.colors.text.active : Theme.colors.text.secondary
                font.family: Theme.uiFont
                font.pixelSize: Theme.uiFontSize - 1
            }
        }
    }

    // "绝赞(B)  [x]": a mark kept in step with one controller property.
    component CheckOption: RowLayout {
        id: option
        required property string key
        required property string label
        Layout.fillWidth: true
        spacing: 4
        Text {
            Layout.fillWidth: true
            text: option.label
            elide: Text.ElideRight
            color: option.enabled ? Theme.colors.text.secondary : Theme.colors.text.disabled
            font.family: Theme.uiFont
            font.pixelSize: Theme.uiFontSize
            TapHandler {
                onTapped: box.toggle()
            }
        }
        AppCheckBox {
            id: box
            checked: root.noteAuthoring[option.key]
            onCheckedChanged: {
                if (root.noteAuthoring[option.key] !== box.checked)
                    root.noteAuthoring[option.key] = box.checked
            }
        }
    }

    // An operation button: its name, and its shortcut underneath.
    component OperationButton: AbstractButton {
        id: operation
        required property string label
        property string shortcut: ""
        Accessible.name: operation.label
        Layout.fillWidth: true
        Layout.preferredHeight: 52
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        background: Rectangle {
            color: operation.down ? Theme.colors.state.pressed
                 : operation.hovered ? Theme.colors.state.hover
                 : Theme.surfaceColor(Theme.colors.background.control)
        }
        contentItem: Item {
            opacity: operation.enabled ? 1.0 : 0.45
            // With a shortcut line the label sits on top; alone, centred.
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                y: operation.shortcut !== "" ? 8 : Math.round((parent.height - height) / 2)
                text: operation.label
                color: Theme.colors.text.primary
                font.family: Theme.uiFont
                font.pixelSize: Theme.uiFontSize
            }
            Text {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 4
                visible: operation.shortcut !== ""
                text: "(" + operation.shortcut + ")"
                color: Theme.colors.text.secondary
                font.family: Theme.uiFont
                font.pixelSize: Theme.uiFontSize - 1
            }
        }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: AppScrollBar {}

        ColumnLayout {
            id: column
            width: root.width
            spacing: 0

            Section {
                title: qsTrId("qml.charting.panel_editor")

                LabeledRow {
                    label: qsTrId("qml.charting.grid")
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        RowLayout {
                            Layout.fillWidth: true
                            AppSlider {
                                Layout.fillWidth: true
                                from: 0
                                to: 64
                                stepSize: 1
                                value: root.noteAuthoring.gridDivision
                                onMoved: root.noteAuthoring.gridDivision = Math.round(value)
                            }
                            Text {
                                Layout.preferredWidth: 36
                                horizontalAlignment: Text.AlignRight
                                text: root.noteAuthoring.gridDivision > 0
                                      ? root.noteAuthoring.gridDivision : qsTrId("qml.charting.grid_chart")
                                color: Theme.colors.accent.focus
                                font.family: Theme.uiFont
                                font.pixelSize: Theme.uiFontSize
                            }
                        }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: 4
                            columnSpacing: 3
                            rowSpacing: 3
                            Repeater {
                                model: [0, 8, 12, 16, 24, 32, 48, 64]
                                delegate: OperationButton {
                                    required property int modelData
                                    Layout.preferredHeight: 26
                                    label: modelData > 0 ? String(modelData) : qsTrId("qml.charting.grid_chart")
                                    onClicked: root.noteAuthoring.gridDivision = modelData
                                }
                            }
                        }
                    }
                }
                LabeledRow {
                    label: qsTrId("qml.charting.zoom")
                    AppSlider {
                        Layout.fillWidth: true
                        from: 40
                        to: 600
                        value: root.laneEditor ? root.laneEditor.pixelsPerSecond : 150
                        onMoved: if (root.laneEditor) root.laneEditor.pixelsPerSecond = value
                    }
                    Text {
                        Layout.preferredWidth: 36
                        horizontalAlignment: Text.AlignRight
                        text: ((root.laneEditor ? root.laneEditor.pixelsPerSecond : 150) / root.zoomBase).toFixed(1) + "×"
                        color: Theme.colors.accent.focus
                        font.family: Theme.uiFont
                        font.pixelSize: Theme.uiFontSize
                    }
                }
            }

            Section {
                title: qsTrId("qml.charting.panel_tap")

                LabeledRow {
                    label: qsTrId("qml.charting.type")
                    NoteTile { toolValue: 1; iconType: "tap"; label: "Tap" }
                    NoteTile { toolValue: 2; iconType: "hold"; label: "Hold" }
                    NoteTile { toolValue: 3; iconType: "slide"; label: "Slide" }
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: root.optionColumns
                    columnSpacing: 12
                    rowSpacing: 2
                    CheckOption { key: "breakNote"; label: qsTrId("qml.charting.break") + "(B)" }
                    CheckOption { key: "exNote"; label: qsTrId("qml.charting.ex") + "(X)" }
                    CheckOption { key: "pseudoEach"; label: qsTrId("qml.charting.pseudo_each") + "(M)" }
                    CheckOption { key: "slideBreak"; label: qsTrId("qml.charting.slide_break") + "(S)" }
                    CheckOption { key: "wifi"; label: qsTrId("qml.charting.wifi") + "(W)" }
                    CheckOption { key: "starTap"; label: qsTrId("qml.charting.star") + "($)" }
                    CheckOption {
                        Layout.columnSpan: root.optionColumns === 3 ? 2 : 1
                        key: "slideChain"
                        label: qsTrId("qml.charting.chain") + "(T)"
                    }
                }
                LabeledRow {
                    label: qsTrId("qml.charting.slide_head")
                    AppComboBox {
                        Layout.fillWidth: true
                        model: [qsTrId("qml.charting.head_star"), qsTrId("qml.charting.head_tap"),
                                qsTrId("qml.charting.head_none"), qsTrId("qml.charting.head_none_now")]
                        currentIndex: root.noteAuthoring.slideHead
                        onActivated: index => root.noteAuthoring.slideHead = index
                    }
                }
            }

            Section {
                title: qsTrId("qml.charting.panel_touch")

                LabeledRow {
                    label: qsTrId("qml.charting.type")
                    NoteTile { toolValue: 4; iconType: "touch"; label: "Touch" }
                    NoteTile { toolValue: 5; iconType: "touch_hold"; label: "Touch Hold" }
                    Item { Layout.fillWidth: true; Layout.preferredHeight: 64 }
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: root.optionColumns
                    columnSpacing: 12
                    CheckOption { key: "firework"; label: qsTrId("qml.charting.firework") + "(F)" }
                    Repeater {
                        model: root.optionColumns - 1
                        delegate: Item { Layout.fillWidth: true }
                    }
                }
            }

            Section {
                title: qsTrId("qml.charting.panel_operations")

                LabeledRow {
                    label: qsTrId("qml.charting.edit")
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 3
                        columnSpacing: 4
                        rowSpacing: 4
                        OperationButton {
                            label: qsTrId("qml.charting.copy")
                            shortcut: root.keys(["Cmd"], "C")
                            enabled: root.noteAuthoring.selectionCount > 0
                            onClicked: root.noteAuthoring.copySelection()
                        }
                        OperationButton {
                            label: qsTrId("qml.charting.cut")
                            shortcut: root.keys(["Cmd"], "X")
                            enabled: root.noteAuthoring.selectionCount > 0
                            onClicked: root.noteAuthoring.cutSelection()
                        }
                        OperationButton {
                            label: qsTrId("qml.charting.paste")
                            shortcut: root.keys(["Cmd"], "V")
                            enabled: root.noteAuthoring.clipCount > 0
                            // In the lane editor the copy follows the pointer
                            // until a click puts it down.
                            onClicked: {
                                if (root.laneEditor && root.laneEditor.visible)
                                    root.laneEditor.pasteMode = true
                                else
                                    root.noteAuthoring.paste()
                            }
                        }
                        OperationButton {
                            label: qsTrId("qml.charting.delete")
                            shortcut: Qt.platform.os === "osx" ? "⌫" : "Del"
                            enabled: root.noteAuthoring.selectionCount > 0
                            onClicked: root.noteAuthoring.deleteSelection()
                        }
                    }
                }
                LabeledRow {
                    label: qsTrId("qml.charting.operate")
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 4
                        rowSpacing: 4
                        // ChartTransformOp values.
                        OperationButton {
                            label: qsTrId("action.transform.mirror_lr")
                            shortcut: root.keys(["Cmd"], "F")
                            enabled: root.noteAuthoring.selectionCount > 0
                            onClicked: root.noteAuthoring.transformSelection(0)
                        }
                        OperationButton {
                            label: qsTrId("action.transform.mirror_ud")
                            shortcut: root.keys(["Cmd", "Shift"], "F")
                            enabled: root.noteAuthoring.selectionCount > 0
                            onClicked: root.noteAuthoring.transformSelection(1)
                        }
                        OperationButton {
                            label: qsTrId("action.transform.rotate_cw_45")
                            shortcut: root.keys(["Cmd"], "R")
                            enabled: root.noteAuthoring.selectionCount > 0
                            onClicked: root.noteAuthoring.transformSelection(4)
                        }
                        OperationButton {
                            label: qsTrId("action.transform.rotate_ccw_45")
                            shortcut: root.keys(["Cmd", "Shift"], "R")
                            enabled: root.noteAuthoring.selectionCount > 0
                            onClicked: root.noteAuthoring.transformSelection(3)
                        }
                        OperationButton {
                            label: qsTrId("action.transform.rotate_180")
                            enabled: root.noteAuthoring.selectionCount > 0
                            onClicked: root.noteAuthoring.transformSelection(2)
                        }
                    }
                }
                LabeledRow {
                    label: qsTrId("qml.charting.selection_edit")
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 4
                        columnSpacing: 4
                        rowSpacing: 4
                        Repeater {
                            model: [
                                { label: qsTrId("qml.charting.break"), operation: "b", key: "B" },
                                { label: qsTrId("qml.charting.ex"), operation: "x", key: "X" },
                                { label: qsTrId("qml.charting.firework"), operation: "f" },
                                { label: qsTrId("qml.charting.slide_break"), operation: "B", key: "S" },
                                { label: qsTrId("qml.charting.hold_to_taps"), operation: "taps" },
                                { label: qsTrId("qml.charting.swap_slide"), operation: "swap" },
                                { label: qsTrId("qml.charting.scale_double"), operation: "scale:2/1" },
                                { label: qsTrId("qml.charting.scale_half"), operation: "scale:1/2" }
                            ]
                            delegate: OperationButton {
                                required property var modelData
                                required property int index
                                Layout.preferredHeight: index < 4 ? 52 : 34
                                label: modelData.label
                                shortcut: modelData.key ? root.keys(["Option"], modelData.key) : ""
                                enabled: root.noteAuthoring.selectionCount > 0
                                onClicked: root.noteAuthoring.editSelection(modelData.operation)
                            }
                        }
                    }
                }
                LabeledRow {
                    label: qsTrId("qml.charting.beat")
                    OperationButton {
                        Layout.preferredHeight: 32
                        label: qsTrId("qml.charting.step_back")
                        onClicked: root.noteAuthoring.stepBeat(-1)
                    }
                    OperationButton {
                        Layout.preferredHeight: 32
                        label: qsTrId("qml.charting.step_forward")
                        onClicked: root.noteAuthoring.stepBeat(1)
                    }
                    // Written at the caret's beat on Enter.
                    AppTextField {
                        Layout.preferredWidth: 56
                        placeholderText: "BPM"
                        onAccepted: {
                            root.noteAuthoring.setBpm(text)
                            text = ""
                        }
                    }
                    AppTextField {
                        Layout.preferredWidth: 48
                        placeholderText: "{N}"
                        onAccepted: {
                            root.noteAuthoring.setDivision(text)
                            text = ""
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: root.laneEditor && root.laneEditor.pasteMode
                          ? qsTrId("qml.charting.paste_mode")
                          : root.laneEditor && root.laneEditor.placingKind === 1
                          ? qsTrId("qml.charting.placing_hold").arg(root.laneEditor.placingLane)
                          : root.laneEditor && root.laneEditor.placingKind === 4
                          ? qsTrId("qml.charting.placing_touch")
                          : root.laneEditor && root.laneEditor.placingKind === 3
                          ? qsTrId("qml.charting.picking_slide")
                          : root.laneEditor && root.laneEditor.placingKind === 2
                          ? qsTrId("qml.charting.placing_slide").arg(root.laneEditor.placingLane)
                          : root.noteAuthoring.statusMessage
                    wrapMode: Text.Wrap
                    color: Theme.colors.text.secondary
                    font.family: Theme.uiFont
                    font.pixelSize: Theme.uiFontSize
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTrId("qml.charting.panel_hint")
                    wrapMode: Text.Wrap
                    color: Theme.colors.text.secondary
                    font.family: Theme.uiFont
                    font.pixelSize: Theme.uiFontSize - 1
                }
            }
        }
    }
}
