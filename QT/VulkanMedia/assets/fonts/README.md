# Fonts

Drop `Inter` and `JetBrains Mono` here if you want the app to ship its own
copies instead of relying on system installs.

The scaffold does **not** bundle any font files — no third-party assets were
added without asking. `Theme.qml` names `Inter` / `JetBrains Mono` and falls
back through `fontUiFamilies` / `fontMonoFamilies` to whatever is installed
(Noto Sans, DejaVu Sans, …), resolved once via `Qt.fontFamilies()`.

To bundle them instead:

1. Copy the `.ttf` files into this directory.
2. Add them to `qt_add_qml_module(... RESOURCES assets/fonts/Inter-Regular.ttf …)`.
3. Load each one at startup with `QFontDatabase::addApplicationFont(":/…")`
   (or a `FontLoader` in QML).
