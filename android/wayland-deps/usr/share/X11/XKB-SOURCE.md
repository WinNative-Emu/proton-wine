# xkeyboard-config data

`xkb/` is the `share/X11/xkb` tree of Termux's x11 repo package
`xkeyboard-config_2.48-1_all.deb` (pool/main/x/xkeyboard-config, https://packages.termux.dev/apt/termux-x11),
verbatim: `rules/` (evdev.xml, evdev.extras.xml, evdev.lst, base.*, xorg.*), `symbols/`, `keycodes/`,
`compat/`, `types/`, `geometry/`. It is the same data the bundled libxkbcommon/libxkbregistry
(Termux builds) were built against; their compiled-in default root is Termux's private
`/data/data/com.termux/files/usr/share/X11/xkb`, unreadable from the app, so winewayland points
`XKB_CONFIG_ROOT` at this copy (`<wcp>/share/X11/xkb`) before it talks to the compositor. That is
what lets `rxkb_context_parse_default_ruleset()` succeed and layouts get their real names for the
HKL; the keymap itself still comes from the compositor. Without it winewayland warns
"Xkb registry unavailable, layout names default to us" and carries on.
