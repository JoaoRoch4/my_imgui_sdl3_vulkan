pragma Singleton
import QtQuick

QtObject {
    readonly property color bg:        "#161826"
    readonly property color surface:   "#232532"
    readonly property color panel:     "#191b28"
    readonly property color chrome:    "#1b1d2b"
    readonly property color text:      "#e9e9ed"
    readonly property color accent:    "#9184d9"

    // neutral ramp 100→900
    readonly property var neutral: ["#f3f5fe","#e4e7f5","#cfd3e5","#b2b6ca","#9397ab","#75798c","#595d6c","#3f424d","#292b31"]
    // accent ramp 100→900
    readonly property var accentRamp: ["#f5f4ff","#e7e5fe","#d2cefd","#b5abfc","#968ae0","#796cbf","#5d5294","#423a6a","#2b2741"]

    readonly property int radius: 8
    readonly property int radiusSm: 6
    readonly property string fontUi: "Inter"
    readonly property string fontMono: "JetBrains Mono"

    // ---- derived helpers -------------------------------------------------
    // Named ramp steps, so call sites read like the design system talks.
    readonly property color neutral100: neutral[0]
    readonly property color neutral200: neutral[1]
    readonly property color neutral300: neutral[2]
    readonly property color neutral400: neutral[3]
    readonly property color neutral500: neutral[4]
    readonly property color neutral600: neutral[5]
    readonly property color neutral700: neutral[6]
    readonly property color neutral800: neutral[7]
    readonly property color neutral900: neutral[8]

    readonly property color accent100: accentRamp[0]
    readonly property color accent200: accentRamp[1]
    readonly property color accent300: accentRamp[2]
    readonly property color accent400: accentRamp[3]
    readonly property color accent500: accentRamp[4]
    readonly property color accent600: accentRamp[5]
    readonly property color accent700: accentRamp[6]
    readonly property color accent800: accentRamp[7]
    readonly property color accent900: accentRamp[8]

    // Body copy and muted copy, per the design system.
    readonly property color bodyText: neutral[2]   // #cfd3e5
    readonly property color mutedText: neutral[4]  // #9397ab
    readonly property color dimText: neutral[5]    // #75798c

    // Borders.
    readonly property color border: neutral[7]     // #3f424d
    readonly property color borderSoft: neutral[8] // #292b31

    // Tinted fill = accent-900 body with an accent-800 inset ring.
    readonly property color tintFill: accentRamp[8]
    readonly property color tintRing: accentRamp[7]

    // Font family fallback chains. `fontUi`/`fontMono` above are the spec'd
    // names; these chains keep the UI legible on machines where Inter (or
    // JetBrains Mono) is not installed. QML's font value type has no
    // `families` list here, so the first installed candidate is resolved once
    // and every call site binds to `uiFamily` / `monoFamily`.
    readonly property var fontUiFamilies: [fontUi, "Noto Sans", "DejaVu Sans", "Sans Serif"]
    readonly property var fontMonoFamilies: [fontMono, "Noto Sans Mono", "DejaVu Sans Mono", "Monospace"]

    readonly property var installedFamilies: Qt.fontFamilies()
    readonly property string uiFamily: firstInstalled(fontUiFamilies)
    readonly property string monoFamily: firstInstalled(fontMonoFamilies)

    function firstInstalled(candidates) {
        for (let i = 0; i < candidates.length; ++i) {
            if (installedFamilies.indexOf(candidates[i]) >= 0)
                return candidates[i]
        }
        return candidates[candidates.length - 1]
    }

    // Type scale. QML's font.pixelSize is an integer, so the design system's
    // half-pixel steps are rounded here once instead of at every call site.
    readonly property int fsMicro: 10 // 9.5 in the design system
    readonly property int fsSmall: 11 // 10.5
    readonly property int fsBody: 12  // 11.5

    // Motion vocabulary.
    readonly property int durHover: 90
    readonly property int durTab: 120
    readonly property int durPopup: 140
    readonly property int durPanel: 160
    readonly property int durThumb: 180
    readonly property int durDisplace: 200
}
