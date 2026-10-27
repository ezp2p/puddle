pragma Singleton
import QtQuick

// Every colour, size and gap the window is built from, in one place to tweak.
//
// Colours that follow the system's light or dark palette are not here: they come from `palette` where they are used. Theme.tint() lays a
// see-through shade of one of them over the background (for hover and selection), and the two shades most things use are named below.
//
// The owner can restyle all of it with a theme.yaml beside puddle.yaml (docs/theming.md): `key: value` lines, where the key is the name of a
// property below that goes through pick(). The Shell hands the file's lines to `overrides`; a missing or unreadable value keeps the default.
QtObject {
    id: theme

    property var overrides: ({})
    function pick(key, fallback) {
        const v = overrides[key]
        if (v === undefined) return fallback
        try { return typeof fallback === "number" ? (isFinite(Number(v)) ? Number(v) : fallback) : Qt.color(v) } catch (e) { return fallback }
    }
    readonly property SystemPalette system: SystemPalette { colorGroup: SystemPalette.Active }
    readonly property bool dark: system.window.hslLightness < 0.5

    // ---- the surfaces: the window is a canvas (the open conversation sits straight on it), with the side panels floating on it as cards
    readonly property color canvas: pick("canvas", dark ? Qt.darker(system.window, 1.35) : Qt.darker(system.window, 1.05))
    readonly property color card: pick("card", dark ? system.window : Qt.lighter(system.window, 1.12))
    readonly property color text: pick("text", system.windowText)
    readonly property color accent: pick("accent", system.highlight)
    readonly property color hairline: pick("hairline", tint(text, 0.12))   // the thin lines between and around things

    // ---- colours that mean something, the same in light and dark
    readonly property color danger: pick("danger", "#d9534f")          // a failure, and the destructive item of a menu
    readonly property color badge: pick("badge", "#d9453d")            // unread counts
    readonly property color warning: pick("warning", "#d9a000")        // held back, certificate changed
    readonly property color banner: pick("banner", "#b8860b")          // "can't reach your Puddle", and cautions on the setup screens
    readonly property color alert: "#c0392b"           // a warning that must not be missed (setup)
    readonly property color toast: "#323232"           // the short message at the bottom
    readonly property color primary: pick("primary", "#2d7d46")        // the green action button
    readonly property color primaryPressed: Qt.darker(primary, 1.2)

    // ---- shades of the text colour laid over the background
    readonly property real hover: 0.07
    readonly property real selected: 0.14
    function tint(base, alpha) { return Qt.rgba(base.r, base.g, base.b, alpha) }

    // ---- layout
    readonly property int wideBreakpoint: 720          // narrower than this the window shows one pane at a time
    readonly property int gap: pick("gap", 10)                         // between the cards, and between them and the window's edge
    readonly property int cardRadius: pick("cardRadius", 16)
    readonly property int railWidth: 64
    readonly property int sidebarWidth: 260
    readonly property int contextBreakpoint: 1000      // from this wide the side panel sits beside the conversation; narrower it opens over it
    readonly property int contextWidth: 280
    readonly property int headerHeight: 48
    readonly property int rowHeight: 52                // a conversation in the sidebar
    readonly property int radius: pick("radius", 6)
    readonly property int radiusLarge: pick("radiusLarge", 8)
    readonly property int buttonRadius: radiusLarge + 4   // buttons, tabs and fields: softer than a row, harder than the round icon buttons
    readonly property int toggleSize: 28               // the little circles that fold the side panels away

    // ---- icons (Tabler, in assets/icons/tabler) and scrolling
    readonly property int iconSize: 20
    function icon(name) { return Qt.resolvedUrl("../../assets/icons/tabler/" + name + ".svg") }
    readonly property int wheelStep: 60                // pixels for one notch of a mouse wheel
}
