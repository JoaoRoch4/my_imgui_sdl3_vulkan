# Icons

Empty on purpose. Every icon in the scaffold is a Unicode glyph rendered as
text (see the `glyph:` properties on `IconButton`, `LauncherPanel`,
`PlacesList`, `PanelMenu`), so the UI has no binary icon dependency.

To move to real icons:

1. Drop SVGs here.
2. Add them to `qt_add_qml_module(... RESOURCES assets/icons/*.svg)`.
3. Swap `IconButton`'s glyph `Text` for an `Image`/`ColorOverlay` pair —
   `glyphColor` already carries the tint the design system expects.
