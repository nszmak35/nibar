# nibar config: reloaded when saved (an error keeps the running one); `pkill -HUP nibar` reloads by hand.
# A setting left out keeps the built-in value; a list you give (tags, sequence_elements) replaces it.

class Colors:
    base    = "0x0B0B0Aff"
    text    = "0xCDCDCAff"
    subtext = "0x8A8A86ff"
    mauve   = "0xABAAA2ff"
    border  = "0x4D4E4Cff"
    green   = "0x889482ff"
    warning = "0xB2A38Bff"
    hover   = "0xABAAA21A"

class Bar:
    enabled = True            # False: no bar (a reload brings it back or takes it away)
    layer = "top"             # "top": over the windows, "bottom": under them, "overlay"
    position_bar = "bottom"   # "top" or "bottom" of the screen
    height = 30               # pixels
    width = 100               # percent of the width left after the padding
    padding = [0, 420, 10, 420]   # gap around the bar: [top, right, bottom, left]
    bg = Colors.base          # background of the bar ("rootcolor" works as well)
    fg = Colors.mauve         # text colour of an element that sets none
    font = "Libertinus Mono"
    font_size_px = 14
    # fallback fonts for glyphs the font lacks (icons)
    fallback_fonts = ["Symbols Nerd Font Mono", "Symbols Nerd Font",
                      "JetBrainsMono Nerd Font", "FiraCode Nerd Font",
                      "Noto Sans Symbols 2"]
    element_margin = 3        # space between two elements
    element_padding = 4       # space inside an element, left and right

    class Elements:
        # element_type: text, tags, title, clock, battery, network, keyboard, exec
        # common: align, fg, bg, font, font_size_px, bold, padding, margin, on_*click
        # on_click "ofwc:action arg" runs one of the compositor's key actions

        class text_Logo:
            element_type = "text"
            align = "left"
            text = "logo_void_nerdfont"     # or any text / glyph
            fg = Colors.mauve
            font_size_px = 16
            padding = [14, 14]
            on_click = "rofi -show drun -theme ~/.config/rofi/we.rasi.d"

        class special_Tags:
            element_type = "tags"
            align = "left"
            tag_active_marker = "tagpod"    # tagpod (underline) | color | symbol
            num_tags = 9
            hide_vacant = False
            font_size_px = 11
            fg = Colors.subtext             # empty tag
            fg_occupied = Colors.text       # tag with windows
            fg_active = Colors.mauve        # tag on screen
            hover_bg = Colors.hover
            hover_fg = Colors.mauve
            padding = [4, 4]
            button_padding = [8, 8]
            class TagPod:
                bg = Colors.mauve
                position = "bottom"         # "bottom" or "top"
                thickness = 2

        class special_Title:
            element_type = "title"          # the focused window
            align = "left"
            format = "[ {layout} ] {title} "   # {title} {appid} {layout}
            max_length = 30
            fg = Colors.mauve

        class executive_Time:
            element_type = "clock"
            align = "center"
            text = "{:L%A %H:%M}"           # strftime inside {:...}
            interval = 1
            fg = Colors.text
            bold = True
            padding = [12, 12]

        class executive_Language:
            element_type = "keyboard"       # the active layout of the keyboard
            align = "right"
            format = "{LAYOUT}"             # {layout} us   {LAYOUT} US   {name}
            fg = Colors.text
            padding = [6, 10]               # click: next layout

        class executive_Battery:
            element_type = "battery"
            align = "right"
            states = {"good": 95, "warning": 30, "critical": 15}
            format = "{capacity}% {icon}"
            format_charging = "{capacity}% \U000f0084"
            format_plugged = "{capacity}% \uf1e6"
            format_alt = "{time} {icon}"    # a click switches to it and back
            format_icons = ["\U000f008e", "\U000f007a", "\U000f007b", "\U000f007c",
                            "\U000f007d", "\U000f007e", "\U000f007f", "\U000f0080",
                            "\U000f0081", "\U000f0082", "\U000f0079"]
            fg = Colors.text
            padding = [8, 8]

        class text_Volume:
            element_type = "text"
            align = "right"
            text = "\uf028"
            fg = Colors.mauve
            font_size_px = 16
            padding = [14, 14]
            on_click = "pavucontrol"

        class executive_Network:
            element_type = "network"
            align = "right"
            format_wifi = "\uf1eb"
            format_ethernet = "\U000f0200"
            format_disconnected = "\U000f092e"
            fg = Colors.mauve
            font_size_px = 16
            padding = [11, 11]
            on_click = "nm-connection-editor"

        class text_Power:
            element_type = "text"
            align = "right"
            text = "\uf011"
            fg = Colors.mauve
            font_size_px = 16
            padding = [11, 18]
            on_click = "~/.config/mango/waybar/scripts/power-menu.sh"

        # The system tray is not part of the bar.

    sequence_elements = [
        Elements.text_Logo,
        Elements.special_Tags,
        Elements.special_Title,
        Elements.executive_Time,
        Elements.executive_Language,
        Elements.executive_Battery,
        Elements.text_Volume,
        Elements.executive_Network,
        Elements.text_Power,
    ]

# tag names (a missing one is its number); the count comes from "tags" in ofwc's config
tags = ["1", "2", "3", "4", "5", "6", "7", "8", "9"]
