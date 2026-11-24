// Launcher do Digimon World 4 - Recomp. Toda a logica esta em dw4_launcher.py
// (objeto `app`); aqui so ha telas. Textos vem de app.t (pt/en).
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: win
    width: 980; height: 620
    minimumWidth: 860; minimumHeight: 560
    visible: true
    title: "Digimon World 4 — Recomp"
    color: bg

    property int page: 0
    property bool debugOpenCombo: false      // so para --screenshot --open-combo
    property var t: app.t
    readonly property color bg: "#0e1320"
    readonly property color panel: "#161d30"
    readonly property color line: "#263150"
    readonly property color ink: "#e8ecf6"
    readonly property color dim: "#93a0bd"
    readonly property color accent: "#ff8a1f"
    readonly property color blue: "#3aa0ff"

    // Cores dos controles prontos (ComboBox, Switch, menus) no estilo Basic.
    palette { window: bg; windowText: ink; base: "#212b48"; alternateBase: panel; text: ink
              button: "#212b48"; buttonText: ink; highlight: accent; highlightedText: "#10131c"
              placeholderText: dim }

    Connections {
        target: app
        function onMessage(text) { toast.text = text; toast.visible = true; toastTimer.restart() }
    }

    FolderDialog { id: folderDlg; onAccepted: app.setGameDir(selectedFolder) }
    FileDialog { id: isoDlg; fileMode: FileDialog.OpenFile; nameFilters: ["Xbox disc image (*.iso *.xiso)", "*"]
                 onAccepted: app.importIso(selectedFile) }
    FileDialog { id: exportDlg; fileMode: FileDialog.SaveFile; nameFilters: ["Zip (*.zip)"]
                 defaultSuffix: "zip"; onAccepted: app.exportSaves(selectedFile) }
    FileDialog { id: importDlg; fileMode: FileDialog.OpenFile; nameFilters: ["Zip (*.zip)"]
                 onAccepted: app.importSaves(selectedFile) }

    // ── pecas reutilizadas ───────────────────────────────────────────────────
    component Card: Rectangle {
        default property alias content: inner.data
        property string heading: ""
        Layout.fillWidth: true
        implicitHeight: inner.implicitHeight + 36
        color: win.panel; radius: 10; border.color: win.line
        ColumnLayout {
            id: inner
            anchors { fill: parent; margins: 18 }
            spacing: 12
            Label { visible: heading !== ""; text: heading; color: win.dim
                    font { pixelSize: 12; bold: true; capitalization: Font.AllUppercase; letterSpacing: 1 } }
        }
    }
    component Btn: Button {
        id: b
        property bool primary: false
        font.pixelSize: 14
        contentItem: Label { text: b.text; color: b.enabled ? win.ink : win.dim; font: b.font
                             horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { radius: 7; implicitHeight: 36; implicitWidth: 120
                                color: b.primary ? (b.down ? "#d96f10" : win.accent) : (b.down ? "#2c3960" : "#212b48")
                                opacity: b.enabled ? 1 : 0.45; border.color: b.primary ? "transparent" : win.line }
    }
    // Select com cores proprias: no estilo padrao o item sob o cursor ficava
    // com fundo claro e texto claro, ilegivel.
    component Combo: ComboBox {
        id: cb
        implicitHeight: 38
        font.pixelSize: 14
        contentItem: Label { leftPadding: 12; rightPadding: 30; text: cb.displayText; color: win.ink; font: cb.font
                             verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
        indicator: Label { text: "▾"; color: win.dim; font.pixelSize: 15
                           x: cb.width - width - 12; y: (cb.height - height) / 2 }
        background: Rectangle { radius: 7; color: cb.pressed || cb.popup.visible ? "#2c3960" : "#212b48"
                                border.color: cb.popup.visible ? win.accent : win.line }
        delegate: ItemDelegate {
            id: it
            required property int index
            required property var modelData
            width: cb.width - 8
            height: 36
            highlighted: cb.highlightedIndex === index
            contentItem: Label { leftPadding: 8; text: it.modelData; font.pixelSize: 14
                                 color: it.highlighted ? "#10131c" : win.ink
                                 font.bold: cb.currentIndex === it.index
                                 verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
            background: Rectangle { radius: 5; color: it.highlighted ? win.accent : "transparent" }
        }
        popup: Popup {
            y: cb.height + 4
            width: cb.width
            padding: 4
            implicitHeight: contentItem.implicitHeight + 8
            contentItem: ListView { clip: true; implicitHeight: contentHeight
                                    model: cb.popup.visible ? cb.delegateModel : null
                                    currentIndex: cb.highlightedIndex }
            background: Rectangle { radius: 8; color: "#1a2238"; border.color: win.line }
        }
    }
    component Toggle: RowLayout {
        property string optKey
        property string label
        property string hint
        Layout.fillWidth: true
        spacing: 14
        ColumnLayout {
            Layout.fillWidth: true; spacing: 2
            Label { text: label; color: win.ink; font.pixelSize: 15 }
            Label { text: hint; color: win.dim; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        }
        Switch {
            id: sw
            checked: app.opt(optKey) === true
            onToggled: app.setOpt(optKey, checked)
            indicator: Rectangle {
                implicitWidth: 46; implicitHeight: 24; radius: 12
                x: sw.leftPadding; y: (sw.height - height) / 2
                color: sw.checked ? win.accent : "#2a3554"
                Rectangle { width: 18; height: 18; radius: 9; y: 3; x: sw.checked ? 25 : 3
                            color: sw.checked ? "#ffffff" : win.dim
                            Behavior on x { NumberAnimation { duration: 90 } } }
            }
        }
    }
    component Info: RowLayout {
        property string k
        property string v
        Layout.fillWidth: true
        Label { text: k; color: win.dim; font.pixelSize: 13; Layout.preferredWidth: 130 }
        Label { text: v; color: win.ink; font.pixelSize: 14; elide: Text.ElideMiddle; Layout.fillWidth: true }
    }
    component Page: Flickable {
        default property alias content: col.data
        contentHeight: col.implicitHeight + 48
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ColumnLayout { id: col; x: 28; y: 24; width: parent.width - 56; spacing: 16 }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ── barra lateral ────────────────────────────────────────────────────
        Rectangle {
            Layout.fillHeight: true; Layout.preferredWidth: 220
            color: "#0a0e19"
            ColumnLayout {
                anchors { fill: parent; margins: 16 }
                spacing: 6
                Label { text: "DIGIMON WORLD 4"; color: win.accent; font { pixelSize: 19; bold: true; letterSpacing: 1 } }
                Label { text: "Recomp · " + app.version; color: win.dim; font.pixelSize: 12; Layout.bottomMargin: 18 }
                Repeater {
                    model: [t.play, t.graphics, t.controls, t.saves, t.support]
                    delegate: Rectangle {
                        required property int index
                        required property string modelData
                        Layout.fillWidth: true; implicitHeight: 40; radius: 7
                        color: win.page === index ? "#1d2742" : (hover.hovered ? "#141b2e" : "transparent")
                        Rectangle { visible: win.page === index; width: 3; height: 20; radius: 2; color: win.accent
                                    anchors { left: parent.left; verticalCenter: parent.verticalCenter } }
                        Label { text: modelData; anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
                                color: win.page === index ? win.ink : win.dim; font.pixelSize: 15 }
                        HoverHandler { id: hover }
                        TapHandler { onTapped: win.page = index }
                    }
                }
                Item { Layout.fillHeight: true }
                Label { text: t.language; color: win.dim; font.pixelSize: 11 }
                Combo {
                    Layout.fillWidth: true
                    model: [t.l_auto, "Português (Brasil)", "English (US)"]
                    currentIndex: ["auto", "pt", "en"].indexOf(app.opt("lang"))
                    onActivated: i => app.setOpt("lang", ["auto", "pt", "en"][i])
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            currentIndex: win.page

            // ── Jogar ────────────────────────────────────────────────────────
            Page {
                Card {
                    heading: t.game_folder
                    Label { text: app.gameDir; color: win.ink; font.pixelSize: 14; elide: Text.ElideMiddle; Layout.fillWidth: true }
                    Label { text: t.need_files; color: win.dim; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    RowLayout {
                        spacing: 10
                        visible: !app.importing
                        Btn { text: t.choose_iso; primary: !app.canPlay; enabled: !app.running; implicitWidth: 270; onClicked: isoDlg.open() }
                        Btn { text: t.choose; enabled: !app.running; implicitWidth: 150; onClicked: folderDlg.open() }
                        Btn { text: t.use_default; enabled: !app.running; implicitWidth: 190; onClicked: app.resetGameDir() }
                    }
                    ColumnLayout {
                        visible: app.importing
                        Layout.fillWidth: true; spacing: 6
                        Label { text: t.iso_working + "  " + Math.round(app.importProgress * 100) + "%"; color: win.ink; font.pixelSize: 14 }
                        Rectangle { Layout.fillWidth: true; height: 8; radius: 4; color: "#2a3554"
                                    Rectangle { width: parent.width * app.importProgress; height: 8; radius: 4; color: win.accent } }
                    }
                }
                Card {
                    Info { k: t.gpu; v: app.gpu }
                    Info { k: t.version; v: app.version }
                    Info { k: t.renderer; v: app.opt("renderer") === "cpu" ? t.r_cpu : (app.opt("renderer") === "vulkan" ? t.r_vk : t.r_gl); property var dep: app.gameDir }
                }
                Card {
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16
                        Rectangle { width: 10; height: 10; radius: 5
                                    color: app.running ? win.blue : (app.canPlay ? "#39d98a" : "#ff5d5d") }
                        Label { text: app.running ? t.running : app.statusText; color: win.ink; font.pixelSize: 14
                                wrapMode: Text.WordWrap; Layout.fillWidth: true }
                        Btn { primary: true; text: app.running ? t.btn_running : t.btn_play
                              enabled: app.canPlay && !app.running && !app.importing
                              font.pixelSize: 17; font.bold: true; font.letterSpacing: 1
                              implicitWidth: 190; implicitHeight: 50
                              onClicked: app.play() }
                    }
                }
            }

            // ── Graficos ─────────────────────────────────────────────────────
            Page {
                Card {
                    heading: t.window
                    Toggle { optKey: "fullscreen"; label: t.fullscreen; hint: t.fullscreen_d }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 14
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 2
                            Label { text: t.scale; color: win.ink; font.pixelSize: 15 }
                            Label { text: t.scale_d; color: win.dim; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                        }
                        Combo {
                            implicitWidth: 170
                            id: scaleCombo
                            Connections { target: win; function onDebugOpenComboChanged() { scaleCombo.popup.open(); scaleCombo.incrementCurrentIndex() } }
                            model: ["1× (640×480)", "2× (1280×960)", "3× (1920×1440)", "4× (2560×1920)"]
                            currentIndex: app.opt("scale") - 1
                            onActivated: i => app.setOpt("scale", i + 1)
                        }
                    }
                    Toggle { optKey: "vsync"; label: t.vsync; hint: t.vsync_d }
                }
                Card {
                    heading: t.renderer
                    Combo {
                        Layout.fillWidth: true
                        id: apiCombo
                        readonly property var apis: ["opengl", "vulkan", "cpu"]
                        model: [t.r_gl, t.r_vk, t.r_cpu]
                        currentIndex: Math.max(0, apis.indexOf(app.opt("renderer")))
                        onActivated: i => app.setOpt("renderer", apis[i])
                    }
                    Label { visible: !app.vulkanAvailable && apiCombo.currentIndex === 1
                            text: t.vk_missing; color: "#ffb454"; font.pixelSize: 12
                            wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Label { text: t.renderer_d; color: win.dim; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                }
            }

            // ── Controles ────────────────────────────────────────────────────
            Page {
                Card {
                    Label { text: t.pad_note; color: win.ink; font.pixelSize: 14; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Toggle { optKey: "keyboard"; label: t.keyboard; hint: t.keyboard_d }
                }
                Card {
                    heading: t.keyboard
                    GridLayout {
                        columns: 2; columnSpacing: 40; rowSpacing: 8
                        Label { text: t.key; color: win.dim; font { pixelSize: 12; bold: true } }
                        Label { text: t.xbox; color: win.dim; font { pixelSize: 12; bold: true } }
                        Repeater {
                            model: app ? app.keymap.length * 2 : 0
                            Label {
                                required property int index
                                text: index % 2 ? app.keymap[(index - 1) / 2].xbox : app.keymap[index / 2].key
                                color: index % 2 ? win.ink : win.blue; font.pixelSize: 14
                            }
                        }
                    }
                }
            }

            // ── Saves e dados ────────────────────────────────────────────────
            Page {
                Card {
                    heading: t.save_folder
                    Label { text: app.saveDir; color: win.ink; font.pixelSize: 14; elide: Text.ElideMiddle; Layout.fillWidth: true }
                    RowLayout {
                        spacing: 10
                        Btn { text: t.open_folder; onClicked: app.openFolder("saves") }
                        Btn { text: t.backup; onClicked: app.backupSaves() }
                        Btn { text: t.export; onClicked: exportDlg.open() }
                        Btn { text: t.import; enabled: !app.running; onClicked: importDlg.open() }
                    }
                }
                Card {
                    heading: t.save_list
                    Label { visible: app.saves.length === 0; text: t.no_saves; color: win.dim; font.pixelSize: 14 }
                    Repeater {
                        model: app.saves
                        RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            Label { text: modelData.name; color: win.ink; font.pixelSize: 14; font.family: "monospace"; Layout.fillWidth: true }
                            Label { text: modelData.size; color: win.dim; font.pixelSize: 13; Layout.preferredWidth: 80 }
                            Label { text: modelData.when; color: win.dim; font.pixelSize: 13 }
                        }
                    }
                }
            }

            // ── Suporte ──────────────────────────────────────────────────────
            Page {
                Card {
                    heading: t.known
                    Repeater {
                        model: [t.known_1, t.known_2, t.known_3]
                        Label { required property string modelData
                                text: "•  " + modelData; color: win.ink; font.pixelSize: 14; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    }
                }
                Card {
                    heading: t.logs
                    Label { text: t.freeze_tip; color: win.ink; font.pixelSize: 14; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Label { text: t.report_d; color: win.dim; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    RowLayout {
                        spacing: 10
                        Btn { text: t.report; onClicked: app.makeReport() }
                        Btn { text: t.open_logs; onClicked: app.openFolder("logs") }
                    }
                }
            }
        }
    }

    // ── aviso no rodape ──────────────────────────────────────────────────────
    Rectangle {
        id: toast
        property alias text: toastLabel.text
        visible: false
        anchors { bottom: parent.bottom; bottomMargin: 18; right: parent.right; rightMargin: 28; left: parent.left; leftMargin: 248 }
        height: toastLabel.implicitHeight + 22; radius: 8
        color: "#22304f"; border.color: win.blue
        Label { id: toastLabel; anchors { fill: parent; margins: 11 }
                color: win.ink; font.pixelSize: 13; wrapMode: Text.WrapAnywhere }
        Timer { id: toastTimer; interval: 9000; onTriggered: toast.visible = false }
        TapHandler { onTapped: toast.visible = false }
    }
}
