// Launcher do Digimon World 4 - Recomp. Toda a logica esta em dw4_launcher.py
// (objeto `app`); aqui so ha telas. Textos vem de app.t (pt/en).
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: win
    // Abre alta o bastante para a pagina Graficos (a mais longa) caber quase
    // inteira; o que nao couber rola, com a barra sempre a vista.
    width: Math.min(1120, Screen.desktopAvailableWidth - 40)
    height: Math.min(940, Screen.desktopAvailableHeight - 60)
    minimumWidth: 860; minimumHeight: 560
    visible: true
    title: "Digimon World 4 — Recomp"
    color: bg

    property int page: 0
    property bool debugOpenCombo: false      // so para --screenshot --open-combo
    property var t: app.t
    // Visual tirado das telas do jogo: fundo azul-marinho com grade, paineis
    // escuros com filete ciano e aba inclinada (a caixa de dialogo do Digi-Elf),
    // laranja e amarelo do logo para o que e acao.
    readonly property color bg: "#040a1c"
    readonly property color panel: "#0a1638"
    readonly property color line: "#1d3a78"
    readonly property color ink: "#eef4ff"
    readonly property color dim: "#8fa6d6"
    readonly property color accent: "#ff8a1f"
    readonly property color gold: "#ffd23f"
    readonly property color cyan: "#38dcff"
    readonly property color blue: "#3aa0ff"
    readonly property color field: "#0f2152"

    // Cores dos controles prontos (ComboBox, Switch, menus) no estilo Basic.
    palette { window: bg; windowText: ink; base: field; alternateBase: panel; text: ink
              button: field; buttonText: ink; highlight: accent; highlightedText: "#10131c"
              placeholderText: dim }

    // Fundo: o "tunel digital" dos menus do jogo -- degrade azul, grade em
    // perspectiva e um clarao embaixo. Pintado uma vez por tamanho de janela.
    Canvas {
        anchors.fill: parent
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            var c = getContext("2d"), w = width, h = height, cx = w * 0.62, cy = h * 1.05
            var g = c.createLinearGradient(0, 0, 0, h)
            g.addColorStop(0, "#030818"); g.addColorStop(0.55, "#06143a"); g.addColorStop(1, "#0a2468")
            c.fillStyle = g; c.fillRect(0, 0, w, h)
            c.lineWidth = 1
            for (var i = 1; i <= 9; i++) {              // arcos da grade
                c.strokeStyle = "rgba(90,170,255," + (0.035 + i * 0.006) + ")"
                c.beginPath(); c.ellipse(cx - i * w * 0.16, cy - i * h * 0.13, i * w * 0.32, i * h * 0.26); c.stroke()
            }
            for (var a = 0; a < 24; a++) {              // raios
                var t = Math.PI * (1 + a / 23)
                c.strokeStyle = "rgba(90,170,255,0.05)"
                c.beginPath(); c.moveTo(cx, cy); c.lineTo(cx + Math.cos(t) * w * 1.6, cy + Math.sin(t) * h * 1.6); c.stroke()
            }
            var r = c.createRadialGradient(cx, h, 0, cx, h, h * 0.75)
            r.addColorStop(0, "rgba(120,200,255,0.28)"); r.addColorStop(1, "rgba(120,200,255,0)")
            c.fillStyle = r; c.fillRect(0, 0, w, h)
        }
    }

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
    component Card: Item {
        id: card
        default property alias content: inner.data
        property string heading: ""
        readonly property int tabH: heading !== "" ? 26 : 0
        Layout.fillWidth: true
        Layout.topMargin: heading !== "" ? 4 : 0
        implicitHeight: inner.implicitHeight + 34 + tabH
        Canvas {
            id: frame
            anchors.fill: parent
            property real tabW: Math.min(width - 60, tabLabel.implicitWidth + 44)
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onTabWChanged: requestPaint()
            onPaint: {
                var c = getContext("2d"), w = width, h = height, y0 = card.tabH, cut = 14
                c.clearRect(0, 0, w, h)
                // corpo: canto inferior direito cortado
                c.beginPath()
                c.moveTo(0.5, y0 + 0.5); c.lineTo(w - 0.5, y0 + 0.5); c.lineTo(w - 0.5, h - cut)
                c.lineTo(w - cut, h - 0.5); c.lineTo(0.5, h - 0.5); c.closePath()
                var g = c.createLinearGradient(0, y0, 0, h)
                g.addColorStop(0, "rgba(12,28,74,0.94)"); g.addColorStop(1, "rgba(7,16,46,0.94)")
                c.fillStyle = g; c.fill()
                c.strokeStyle = "#1d3a78"; c.lineWidth = 1; c.stroke()
                // filete ciano em cima, como nas caixas de dialogo do jogo
                var l = c.createLinearGradient(0, 0, w, 0)
                l.addColorStop(0, "#38dcff"); l.addColorStop(0.7, "rgba(56,220,255,0.35)"); l.addColorStop(1, "rgba(56,220,255,0)")
                c.fillStyle = l; c.fillRect(0, y0, w, 2)
                if (card.tabH) {                    // aba inclinada com o titulo
                    c.beginPath()
                    c.moveTo(0.5, y0 + 1); c.lineTo(10, 0.5); c.lineTo(tabW, 0.5); c.lineTo(tabW - 12, y0 + 1); c.closePath()
                    c.fillStyle = "#0c1c4a"; c.fill()
                    c.strokeStyle = "#38dcff"; c.stroke()
                }
            }
        }
        Label { id: tabLabel; visible: heading !== ""; text: heading; x: 22; height: card.tabH
                verticalAlignment: Text.AlignVCenter; color: win.cyan
                font { pixelSize: 12; bold: true; italic: true; capitalization: Font.AllUppercase; letterSpacing: 1.5 } }
        ColumnLayout {
            id: inner
            anchors { fill: parent; margins: 18; topMargin: 16 + card.tabH }
            spacing: 12
        }
    }
    // Paralelogramo: os botoes e destaques do jogo sao inclinados.
    component Slant: Canvas {
        property color fill: "#000"
        property color fill2: fill
        property color stroke: "transparent"
        property real lean: 10
        onFillChanged: requestPaint()
        onFill2Changed: requestPaint()
        onStrokeChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            var c = getContext("2d"), w = width, h = height
            c.clearRect(0, 0, w, h)
            c.beginPath(); c.moveTo(lean, 0.5); c.lineTo(w - 0.5, 0.5); c.lineTo(w - lean, h - 0.5); c.lineTo(0.5, h - 0.5); c.closePath()
            var g = c.createLinearGradient(0, 0, 0, h)
            g.addColorStop(0, fill); g.addColorStop(1, fill2)
            c.fillStyle = g; c.fill()
            c.strokeStyle = stroke; c.lineWidth = 1; c.stroke()
        }
    }
    component Btn: Button {
        id: b
        property bool primary: false
        font.pixelSize: 14
        contentItem: Label { text: b.text; font: b.font
                             color: !b.enabled ? win.dim : (b.primary ? "#1a0e00" : win.ink)
                             horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Slant { implicitHeight: 36; implicitWidth: 120
                            opacity: b.enabled ? 1 : 0.45
                            fill: b.primary ? (b.down ? "#ffb21f" : win.gold) : (b.down ? "#1b3a8a" : "#12296a")
                            fill2: b.primary ? (b.down ? "#e06a00" : win.accent) : (b.down ? "#12296a" : "#0c1c4a")
                            stroke: b.primary ? "#fff1b8" : (b.hovered ? win.cyan : "#2b55a8") }
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
        background: Rectangle { radius: 3; color: cb.pressed || cb.popup.visible ? "#17337c" : win.field
                                border.color: cb.popup.visible ? win.cyan : (cb.hovered ? "#3a6fd0" : "#2b55a8") }
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
            background: Rectangle { radius: 2; color: it.highlighted ? win.gold : "transparent" }
        }
        popup: Popup {
            y: cb.height + 4
            width: cb.width
            padding: 4
            implicitHeight: contentItem.implicitHeight + 8
            contentItem: ListView { clip: true; implicitHeight: contentHeight
                                    model: cb.popup.visible ? cb.delegateModel : null
                                    currentIndex: cb.highlightedIndex }
            background: Rectangle { radius: 3; color: "#0a1638"; border.color: win.cyan }
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
                color: sw.checked ? "#1586b8" : "#12224f"
                border.color: sw.checked ? win.cyan : "#2b55a8"
                Rectangle { width: 18; height: 18; radius: 9; y: 3; x: sw.checked ? 25 : 3
                            color: sw.checked ? "#ffffff" : win.dim
                            Behavior on x { NumberAnimation { duration: 90 } } }
            }
        }
    }
    // Atalho de teclado de uma opcao (quit_keys, osd_keys): mostra o atual e
    // captura um novo -- as teclas apertadas juntas valem ao soltar.
    component KeyShortcut: RowLayout {
        id: ks
        property string optKey
        property string label
        property string hint
        Layout.fillWidth: true; spacing: 14
        ColumnLayout {
            Layout.fillWidth: true; spacing: 2
            Label { text: ks.label; color: win.ink; font.pixelSize: 15 }
            Label { text: keyCap.active ? t.quit_press : ks.hint
                    color: keyCap.active ? win.gold : win.dim; font.pixelSize: 12
                    wrapMode: Text.WordWrap; Layout.fillWidth: true }
        }
        Rectangle {
            id: keyCap
            property bool active: false
            property var held: []
            function begin() { held = []; active = true; forceActiveFocus() }
            function end(save) { if (save && held.length) app.setComboKeys(ks.optKey, held); active = false; held = [] }
            implicitWidth: 215; implicitHeight: 38; radius: 3
            color: active ? "#17337c" : win.field
            border.color: active ? win.gold : "#2b55a8"
            onActiveFocusChanged: if (!activeFocus && active) end(false)
            Keys.onPressed: e => {
                if (!active) return
                e.accepted = true
                if (e.isAutoRepeat) return
                var n = app.keyName(e.key)
                if (n !== "" && held.indexOf(n) < 0 && held.length < 4) held = held.concat([n])
            }
            Keys.onReleased: e => {
                if (!active) return
                e.accepted = true
                if (!e.isAutoRepeat) end(true)
            }
            Label { anchors.centerIn: parent; font.pixelSize: 14; font.bold: true
                    text: keyCap.active ? (keyCap.held.length ? app.keysLabel(keyCap.held) : "…")
                                        : (app.rev, t, app.comboLabel(ks.optKey))
                    color: keyCap.active ? win.gold : win.ink }
            TapHandler { onTapped: keyCap.active ? keyCap.end(false) : keyCap.begin() }
        }
        Btn { text: t.quit_set; implicitWidth: 110; onClicked: keyCap.begin() }
        Btn { text: t.quit_clear; implicitWidth: 110; enabled: (app.rev, app.opt(ks.optKey) !== "")
              onClicked: { keyCap.end(false); app.setComboKeys(ks.optKey, []) } }
    }
    // Atalho de controle de uma opcao (quit_pad, osd_pad): de 2 a 4 botoes marcados.
    component PadShortcut: ColumnLayout {
        id: ps
        property string optKey
        property string label
        property string hint
        readonly property int count: (app.rev, app.padCount(optKey))
        Layout.fillWidth: true; spacing: 6
        Label { text: ps.label; color: win.ink; font.pixelSize: 15 }
        Label { text: ps.hint; color: win.dim; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        Flow {
            Layout.fillWidth: true; spacing: 8
            Repeater {
                model: (app.rev, app.padButtons(ps.optKey))
                Rectangle {
                    required property var modelData
                    width: 62; height: 34; radius: 3
                    color: modelData.on ? "#1586b8" : win.field
                    border.color: modelData.on ? win.cyan : (chipHover.hovered ? "#3a6fd0" : "#2b55a8")
                    Label { anchors.centerIn: parent; text: modelData.label; font.pixelSize: 13
                            font.bold: modelData.on; color: modelData.on ? "#ffffff" : win.dim }
                    HoverHandler { id: chipHover }
                    TapHandler { onTapped: app.togglePad(ps.optKey, modelData.name) }
                }
            }
        }
        Label { visible: ps.count === 1; text: t.quit_pad_one; color: "#ffb454"; font.pixelSize: 12
                wrapMode: Text.WordWrap; Layout.fillWidth: true }
        Label { visible: ps.count === 0; text: t.quit_off; color: win.dim; font.pixelSize: 12 }
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
        id: page
        // Barra de rolagem sempre visivel quando ha mais conteudo abaixo.
        ScrollBar.vertical: ScrollBar {
            id: vbar
            policy: page.contentHeight > page.height + 1 ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            width: 14
            contentItem: Rectangle { implicitWidth: 8; radius: 4
                                     color: vbar.pressed ? win.gold : (vbar.hovered ? win.accent : win.blue) }
            background: Rectangle { color: "#40050d2a"; radius: 4 }
        }
        ColumnLayout { id: col; x: 28; y: 24; width: parent.width - 70; spacing: 16 }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ── barra lateral ────────────────────────────────────────────────────
        Rectangle {
            Layout.fillHeight: true; Layout.preferredWidth: 220
            color: "#b0030818"
            Rectangle { anchors { right: parent.right; top: parent.top; bottom: parent.bottom } width: 1
                        gradient: Gradient { GradientStop { position: 0; color: "#0038dcff" }
                                             GradientStop { position: 0.5; color: "#8038dcff" }
                                             GradientStop { position: 1; color: "#0038dcff" } } }
            ColumnLayout {
                anchors { fill: parent; margins: 16 }
                spacing: 6
                // O logo do jogo em tipografia: laranja com contorno amarelo, inclinado.
                Label { text: "DIGIMON"; color: win.accent; style: Text.Outline; styleColor: "#5a1a00"
                        font { pixelSize: 30; bold: true; italic: true; letterSpacing: 2 } }
                RowLayout {
                    spacing: 8; Layout.topMargin: -8
                    Label { text: "WORLD"; color: "#7fd0ff"; style: Text.Outline; styleColor: "#0a2a66"
                            font { pixelSize: 19; bold: true; italic: true; letterSpacing: 3 } }
                    Label { text: "4"; color: win.gold; style: Text.Outline; styleColor: "#7a4a00"
                            font { pixelSize: 34; bold: true; italic: true } }
                }
                Label { text: "RECOMP · " + app.version; color: win.dim; Layout.bottomMargin: 16
                        font { pixelSize: 11; letterSpacing: 2 } }
                Repeater {
                    model: [t.play, t.graphics, t.controls, t.saves, t.support]
                    delegate: Rectangle {
                        required property int index
                        required property string modelData
                        Layout.fillWidth: true; implicitHeight: 40; color: "transparent"
                        Slant { anchors.fill: parent; lean: 8
                                visible: win.page === index || hover.hovered
                                fill: win.page === index ? "#17337c" : "#0f2152"
                                fill2: win.page === index ? "#0c1c4a" : "#0a1638"
                                stroke: win.page === index ? win.cyan : "#2b55a8" }
                        Label { text: (win.page === index ? "▸ " : "") + modelData
                                anchors { left: parent.left; leftMargin: 18; verticalCenter: parent.verticalCenter }
                                color: win.page === index ? win.gold : win.dim
                                font { pixelSize: 15; bold: win.page === index; italic: win.page === index } }
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
                        Rectangle { Layout.fillWidth: true; height: 8; radius: 4; color: "#12224f"
                                    Rectangle { width: parent.width * app.importProgress; height: 8; radius: 4; color: win.accent } }
                    }
                }
                Card {
                    Info { k: t.gpu; v: app.gpu }
                    Info { k: t.version; v: app.version }
                    Info { k: t.renderer; v: (app.rev, app.opt("renderer")) === "vulkan" ? t.r_vk : t.r_gl }
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
                Card {
                    heading: t.updates
                    readonly property bool busy: ["checking", "downloading", "installing"].indexOf(app.updateState) >= 0
                    property var dep: t          // o texto muda com o idioma
                    Label { visible: text !== ""; text: (parent.dep, app.updateText); font.pixelSize: 14
                            color: app.updateState === "available" ? win.gold
                                   : (app.updateState === "error" ? "#ffb454" : win.ink)
                            font.bold: app.updateState === "available"
                            wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Rectangle { visible: app.updateState === "downloading"
                                Layout.fillWidth: true; height: 8; radius: 4; color: "#12224f"
                                Rectangle { width: parent.width * app.updateProgress; height: 8; radius: 4; color: win.accent } }
                    RowLayout {
                        spacing: 10
                        Btn { visible: app.updateState === "available" && app.updateSelf; primary: true
                              text: t.upd_install; enabled: !app.running; implicitWidth: 190
                              onClicked: app.installUpdate() }
                        Btn { visible: (app.updateState === "available" && !app.updateSelf) || app.updateState === "error"
                              primary: true
                              text: t.upd_page; implicitWidth: 270; onClicked: app.openUpdatePage() }
                        Btn { visible: app.updateState === "available" && app.updateSelf
                              text: t.upd_notes; implicitWidth: 190; onClicked: app.openUpdatePage() }
                        Btn { visible: app.updateState === "downloading"
                              text: t.upd_cancel; onClicked: app.cancelUpdate() }
                        Btn { visible: ["idle", "current", "error", "available"].indexOf(app.updateState) >= 0
                              text: t.upd_check; implicitWidth: 210; onClicked: app.checkUpdate() }
                    }
                    Label { text: app.updateSelf ? t.upd_self_d : t.upd_manual_d; color: win.dim; font.pixelSize: 12
                            wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Toggle { optKey: "check_updates"; label: t.upd_auto; hint: t.upd_auto_d }
                }
            }

            // ── Graficos ─────────────────────────────────────────────────────
            Page {
                Card {
                    heading: t.renderer
                    Combo {
                        Layout.fillWidth: true
                        id: apiCombo
                        readonly property var apis: ["vulkan", "opengl"]
                        model: [t.r_vk, t.r_gl]
                        currentIndex: Math.max(0, apis.indexOf(app.opt("renderer")))
                        onActivated: i => app.setOpt("renderer", apis[i])
                    }
                    Label { visible: !app.vulkanAvailable && apiCombo.currentIndex === 0
                            text: t.vk_missing; color: "#ffb454"; font.pixelSize: 12
                            wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Label { text: t.renderer_d; color: win.ink; font.pixelSize: 13; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Repeater {
                        model: [t.api_vk, t.api_gl]
                        RowLayout {
                            required property int index
                            required property string modelData
                            // so a descricao da API escolhida: a pagina fica mais curta
                            visible: apiCombo.currentIndex === index
                            Layout.fillWidth: true; spacing: 8
                            Label { text: "▸"; color: apiCombo.currentIndex === index ? win.gold : win.line
                                    font.pixelSize: 13; Layout.alignment: Qt.AlignTop }
                            Label { text: modelData; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true
                                    color: apiCombo.currentIndex === index ? win.ink : win.dim }
                        }
                    }
                    Label { text: t.api_note; color: win.dim; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                }
                Card {
                    heading: t.image
                    RowLayout {
                        Layout.fillWidth: true; spacing: 14
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 2
                            Label { text: t.res; color: win.ink; font.pixelSize: 15 }
                            Label { text: t.res_d; color: win.dim; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                        }
                        Combo {
                            implicitWidth: 215
                            id: resCombo
                            Connections { target: win; function onDebugOpenComboChanged() { resCombo.popup.open(); resCombo.incrementCurrentIndex() } }
                            readonly property var heights: [480, 720, 1080, 1440, 2160]
                            model: [t.res_orig, "720p", "1080p", "1440p", "2160p (4K)"]
                            currentIndex: Math.max(0, heights.indexOf(app.opt("res")))
                            onActivated: i => app.setOpt("res", heights[i])
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 14
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 2
                            Label { text: t.aspect; color: win.ink; font.pixelSize: 15 }
                            Label { text: t.aspect_d; color: win.dim; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                        }
                        Combo {
                            implicitWidth: 215
                            readonly property var ratios: ["4:3", "16:9", "21:9"]
                            model: [t.aspect_orig, "16:9", "21:9"]
                            currentIndex: Math.max(0, ratios.indexOf(app.opt("aspect")))
                            onActivated: i => app.setOpt("aspect", ratios[i])
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 14
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 2
                            Label { text: t.fps; color: win.ink; font.pixelSize: 15 }
                            Label { text: t.fps_d; color: win.dim; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                        }
                        Combo {
                            implicitWidth: 215
                            readonly property var rates: [30, 60]
                            model: [t.fps_30, t.fps_60]
                            currentIndex: Math.max(0, rates.indexOf(app.opt("fps")))
                            onActivated: i => app.setOpt("fps", rates[i])
                        }
                    }
                }
                Card {
                    heading: t.window
                    RowLayout {
                        Layout.fillWidth: true; spacing: 14
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 2
                            Label { text: t.display; color: win.ink; font.pixelSize: 15 }
                            Label { text: t.display_d; color: win.dim; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                        }
                        Combo {
                            id: dispCombo
                            implicitWidth: 330
                            // Monitores ligados agora; o escolhido continua na lista mesmo desligado.
                            readonly property var list: app.displays()
                            readonly property string saved: (app.rev, app.opt("display"))
                            readonly property var names: {
                                var n = [""].concat(list.map(d => d.name))
                                if (saved !== "" && n.indexOf(saved) < 0) n.push(saved)
                                return n
                            }
                            model: {
                                var m = [t.display_auto].concat(list.map(d => d.label))
                                if (names.length > m.length) m.push(saved + " " + t.display_gone)
                                return m
                            }
                            currentIndex: Math.max(0, names.indexOf(saved))
                            onActivated: i => app.setOpt("display", names[i])
                        }
                    }
                    Toggle { optKey: "fullscreen"; label: t.fullscreen; hint: t.fullscreen_d }
                    Label { text: t.window_d; color: win.dim; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Toggle { optKey: "vsync"; label: t.vsync; hint: t.vsync_d }
                }
                Card {
                    heading: t.perf
                    Toggle { optKey: "parallel"; label: t.parallel; hint: t.parallel_d }
                }
            }

            // ── Controles ────────────────────────────────────────────────────
            Page {
                Card {
                    Label { text: t.pad_note; color: win.ink; font.pixelSize: 14; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Toggle { optKey: "keyboard"; label: t.keyboard; hint: t.keyboard_d }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 14
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 2
                            Label { text: t.pads; color: win.ink; font.pixelSize: 15 }
                            Label { text: t.pads_d; color: win.dim; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                        }
                        Combo {
                            implicitWidth: 110
                            model: ["1", "2", "3", "4"]
                            currentIndex: Math.max(0, app.opt("pads") - 1)
                            onActivated: i => app.setOpt("pads", i + 1)
                        }
                    }
                }
                Card {
                    heading: t.quit
                    Label { text: t.quit_d; color: win.ink; font.pixelSize: 13; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    KeyShortcut { optKey: "quit_keys"; label: t.quit_keys; hint: t.quit_keys_d }
                    PadShortcut { optKey: "quit_pad"; label: t.quit_pad; hint: t.quit_pad_d }
                }
                Card {
                    heading: t.osd
                    Label { text: t.osd_d; color: win.ink; font.pixelSize: 13; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Toggle { optKey: "osd"; label: t.osd_start; hint: t.osd_start_d }
                    KeyShortcut { optKey: "osd_keys"; label: t.osd_keys; hint: t.osd_keys_d }
                    PadShortcut { optKey: "osd_pad"; label: t.osd_pad; hint: t.osd_pad_d }
                }
                Card {
                    heading: t.keyboard
                    Label { text: t.keymap_d; color: win.dim; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    GridLayout {
                        columns: 3; columnSpacing: 26; rowSpacing: 8
                        Layout.fillWidth: true
                        Repeater {
                            model: (app.rev, app.keymapRows())
                            RowLayout {
                                id: kb
                                required property var modelData
                                property bool active: false
                                spacing: 10
                                Layout.fillWidth: true
                                Label { text: kb.modelData.label; color: win.ink; font.pixelSize: 14; Layout.preferredWidth: 92 }
                                Rectangle {
                                    id: kbCap
                                    implicitWidth: 120; implicitHeight: 32; radius: 3
                                    color: kb.active ? "#17337c" : win.field
                                    border.color: kb.active ? win.gold : (kb.modelData.changed ? win.cyan : "#2b55a8")
                                    onActiveFocusChanged: if (!activeFocus) kb.active = false
                                    Keys.onPressed: e => {
                                        if (!kb.active) return
                                        e.accepted = true
                                        if (e.isAutoRepeat) return
                                        var n = app.keyName(e.key)
                                        if (n === "") return
                                        kb.active = false
                                        app.setKeymap(kb.modelData.control, n)
                                    }
                                    Label { anchors.centerIn: parent; font.pixelSize: 13; font.bold: kb.modelData.changed
                                            text: kb.active ? "…" : kb.modelData.key
                                            color: kb.active ? win.gold : (kb.modelData.changed ? win.cyan : win.blue) }
                                    TapHandler { onTapped: { kb.active = !kb.active; if (kb.active) kbCap.forceActiveFocus() } }
                                }
                            }
                        }
                    }
                    Btn { text: t.keymap_reset; implicitWidth: 210; onClicked: app.resetKeymap() }
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
                    heading: t.slow
                    Repeater {
                        model: [t.slow_1, t.slow_2, t.slow_3, t.slow_4, t.slow_5]
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
