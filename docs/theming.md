# Theming

The desktop app reads `theme.yaml` from the same folder as `puddle.yaml` (`~/.config/puddle/` on macOS and Linux, `%APPDATA%\puddle\` on
Windows). It is optional. Save it while the app runs and the window restyles at once.

The file is `key: value` lines; `#` starts a comment. Put colours in quotes (a bare `#` starts a comment in YAML). A key the app does not
know, or a value that is not a colour or number, is ignored and the default stays.

```yaml
canvas: "#12151a"      # the window, and what the open conversation sits on
card: "#1a1f27"        # the floating side panels
hairline: "#2a313c"    # thin lines
text: "#e6e9ef"
accent: "#7aa2f7"
primary: "#3b8f5c"     # the main action button
cardRadius: 28
gap: 12                # between cards, and between them and the window edge
```

| key | what | default |
|---|---|---|
| `canvas`, `card` | window surface, side panels | follow the system's light or dark |
| `text`, `accent`, `hairline` | text, highlight, thin lines | system text, system highlight, text at 12% |
| `primary` | the main action button | green |
| `danger`, `badge`, `warning`, `banner` | destructive items, unread counts, cautions, "can't reach your Puddle" | red, red, amber, amber |
| `cardRadius`, `radius`, `radiusLarge`, `gap` | sizes in pixels | 16, 6, 8, 10 |

Not themeable yet: fonts, menus and dialogs (the system draws their colours), the message layout.

The keys are the properties of `src/gui/qml/theme/Theme.qml` that go through `pick()`; add a key by routing a new property through it.
