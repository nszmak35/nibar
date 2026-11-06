# nibar — Neuswc Ipc Bar

A small status bar for ofwc and other neuswc window managers. It is an
ordinary `wlr-layer-shell` client: the compositor draws nothing for it. What the bar
shows (tags, the focused window, the layout) comes from the compositor over IPC, the
same way waybar's `dwl/*` modules work with dwl and solux, and clicks go back the same
way.

* **Layer shell, one bar per output.** The bar keeps room for itself with an exclusive
  zone, so the tiles of the window manager stay clear of it.
* **Python configuration, live reload.** `~/.config/nibar/config.py` is read like
  ofwc reads its own `config.py`. It is reloaded as soon as the file is saved — only if
  it has no errors; otherwise the running configuration stays and the error is printed
  on stderr. `pkill -HUP nibar` reloads by hand.
* **Light.** One small process, one wake-up per second (for the clock), nothing while
  the file is untouched (inotify, no polling).
* **Does not crash on its surroundings.** A missing optional protocol, an output that
  goes away, a surface the compositor closes, a bad config or a command that dies only
  make the bar skip that part.

## IPC

| protocol | needed | used for |
| --- | --- | --- |
| `wlr-layer-shell-unstable-v1` | yes | the bar itself |
| `dwl-ipc-unstable-v2` | for tags and window title | tags, focused window (title, app id), layout, fullscreen, `toggle_visibility`; view tags / move the window to tags |
| `ofwc-ipc-unstable-v1` | optional | keyboard layouts and switching, `ofwc:` actions |

Without `ofwc-ipc` the bar still works; only the `keyboard` element (it hides itself)
and `ofwc:` clicks are missing. Any compositor that serves `dwl-ipc-unstable-v2` and
`wlr-layer-shell` can run nibar — a neuswc window manager such as hevel only needs to
implement `dwl-ipc-unstable-v2` (the protocol solux serves for waybar) to get the tags
and the title.

While a window is fullscreen on an output, the bar of that output drops to the
`bottom` layer (below the windows) and keeps its room, so the tiles do not move. The
compositor's `togglebar` action hides or shows the bars (`toggle_visibility`).

## Build and install

Needs `wayland-client` (with `wayland-scanner`), neuwld (`wld.pc`, it comes with
neuswc; fontconfig and pixman come through it), Python 3 (development files) and GNU
make. For the icons of the default configuration install a Nerd Font (any:
`Symbols Nerd Font Mono`, `JetBrainsMono Nerd Font`, ...).

    make
    sudo make install      # or doas make install
    make uninstall
    make clean

It installs `nibar` into `$(PREFIX)/bin` (`/usr/local` by default) and the template
configuration into `/etc/nibar/config.py`.

## Run

    nibar [-c config.py] [-d] [-v]

`-c` uses another configuration file, `-d` prints debug messages, `-v` the version.
Under ofwc add `"nibar"` to `autostart`. On the first start
`~/.config/nibar/config.py` is created from the built-in template (`default/config.py`,
it is compiled into the binary). nibar quits when the compositor does.

## Configuration

`~/.config/nibar/config.py` is Python. A setting you leave out keeps the value of the
template; the lists you give (`tags`, `sequence_elements`) replace the template's lists
as a whole. A reload with a bad value (`layer = "sideways"`) is refused like a syntax
error.

    class Bar:
        enabled = True           # False: no bar
        layer = "top"            # "top": over the windows, "bottom": under them, "overlay"
        position_bar = "bottom"  # "top" / "bottom"
        height = 30              # pixels
        width = 100              # percent of the width left after the padding
        padding = [0, 420, 10, 420]   # gap around the bar: [top, right, bottom, left]
        bg = "0x0B0B0Aff"        # background (RRGGBBAA); fg: default text colour
        font = "Libertinus Mono"; font_size_px = 14
        fallback_fonts = ["Symbols Nerd Font Mono", ...]   # for glyphs the font lacks
        element_margin = 3       # between elements
        element_padding = 4      # inside an element, by default
        class Elements:
            class text_Logo:     # any class name
                element_type = "text"
                align = "left"   # left / center / right
                text = "logo_void_nerdfont"
        sequence_elements = [Elements.text_Logo, ...]   # left to right inside its group

    tags = ["1", "2", "3", "4", "5", "6", "7", "8", "9"]   # names of the tags

`sequence_elements` may also stand at module level; the one inside `Bar` wins. The
room the bar takes from the windows is `top + height + bottom` of the padding (the
exclusive zone). There are at most 24 elements.

**Attributes of every element:** `element_type`, `align`, `fg`, `bg`, `font`,
`font_size_px`, `bold`, `padding = [left, right]` (or a number), `margin`, `on_click`,
`on_right_click`, `on_middle_click`. A command in `on_click` is run with `sh -c`;
`"ofwc:action argument"` runs one of the compositor's actions of its `keys` list
(`"ofwc:togglefloating"`, `"ofwc:view 4"`; through `ofwc-ipc`, `spawn` and `moveresize`
are not available that way).

**Element types (`element_type`):**

* `text` — a label or a glyph: `text`. Glyph names: `logo_void_nerdfont`,
  `logo_arch_nerdfont`, `logo_debian_nerdfont`, `logo_fedora_nerdfont`,
  `logo_gentoo_nerdfont`, `logo_nixos_nerdfont`, `logo_ubuntu_nerdfont`,
  `logo_linux_nerdfont`, `icon_power_nerdfont`, `icon_volume_nerdfont`,
  `icon_wifi_nerdfont`; or the symbol itself (`"\uf32e"`).
* `tags` — the tags of the output: `tag_active_marker` (`"tagpod"` a stripe, `"color"`
  only the colour, `"symbol"` the `tag_symbol` before the name), `num_tags`,
  `hide_vacant`, `fg` (empty tag), `fg_occupied`, `fg_active`, `hover_bg`, `hover_fg`,
  `button_padding`; the nested class `TagPod`: `bg`, `position` (`"bottom"`/`"top"`),
  `thickness`. Click: left — view the tag, right — toggle it in the view, middle —
  move the focused window to it.
* `title` — the focused window: `format` (`{title}`, `{appid}`, `{layout}`),
  `max_length`.
* `clock` — `text` or `format` (`"{:L%A %H:%M}"` as in waybar, or a plain strftime);
  days and months are in the language of the environment (`LC_TIME`).
* `battery` — `/sys/class/power_supply`: `format`, `format_charging`, `format_plugged`,
  `format_full`, `format_alt` (a click switches to it and back), `format_icons`,
  `states = {"good": 95, "warning": 30, "critical": 15}` (+ `fg_good`, `fg_warning`,
  `fg_critical`), `interval`, `battery` (the name, if there are several). Variables:
  `{capacity}`, `{icon}`, `{time}`, `{status}`, `{power}`. Without a battery the
  element disappears.
* `network` — `format_wifi`, `format_ethernet`, `format_disconnected` (variable
  `{ifname}`), `interface`, `interval`. The interface is the one of the default route.
* `keyboard` — the active keyboard layout (`ofwc-ipc`): `format` (`{layout}` us,
  `{LAYOUT}` US, `{name}`), `names` (your own labels). A click without `on_click`
  switches to the next layout.
* `exec` — the output of a command: `exec`, `format` (`{}` is the line), `interval`
  (run it again every N seconds; without it the command keeps running and prints a
  line whenever something changes, like waybar's `custom` with `exec`),
  `restart_interval`, `max_length`.

There is no system tray (`tray` in the list is skipped with a log message). An
element with an unknown `element_type`, or none that can be guessed from the name of
its class, is skipped with a log message.

## Layout of the source

    src/nibar.c        main loop: signals, config watch and reload
    src/wl.h           Wayland: layer surfaces, shm buffers, pointer, dwl-ipc, ofwc-ipc
    src/bar.h          elements, their content, layout and painting
    src/gfx.h          text (wld, with fallback fonts) and fills
    src/data.h         formats, clock, battery, network, commands
    src/config.h       the configuration and its loading
    src/parser.h       the embedded Python parser of config.py
    src/util.h         helpers
    src/protocols/     XML of the protocols (wayland-scanner makes the code)
    default/config.py  template configuration

## License

MIT, like ofwc (the code of the bar comes from it, and ofwc comes from dwc).
