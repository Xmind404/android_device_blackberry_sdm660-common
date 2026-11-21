# BlackBerry physical keyboard layout provider

This app publishes selectable language KCM overlays to Android InputManager.
It does not receive key events, process touch input, or replace an IME.

Sibling directories hold the native input stack:
- ../keylayout: Linux scan code -> Android key code (.kl).
- ../keychars: base characters and BlackBerry modifiers (.kcm).
- ../idc: physical device configuration, including which base maps to load.

This directory holds the Android resource/receiver packaging required for
language overlays to appear in Settings. Android.bp installs the same existing
BlackBerryKeyboardLayouts module in system_ext. The package, receiver, and
existing layout names intentionally remain unchanged, preserving saved choices.

All original raw language KCMs remain unchanged. Regional/default mapping is
selected by framework policy; a native fallback resource defines no keys.
New ordinary language layouts need correct family/locale/VID/PID metadata.
Add subtype handling only for legacy IMEs whose transliteration variants share
the same locale; do not infer transliteration from a translated display label.
