# PS3DK desktop tools: shared look

Every PS3DK tool with a window (today the PARAM.SFO editor, `sfo-gui`; the profiler and others later) uses the `tools/ps3dk-ui` crate. It holds the palette, type scale, spacing and widgets, so a new tool looks like part of the same kit without copying styling code. This page is the contract a new tool follows.

## Platform

- **egui / eframe 0.29** with the `glow` backend, the same stack `sfo-gui` already shipped. `ps3dk-ui` depends on `egui` only, so it adds nothing to the Linux build or to the Windows cross-build (`x86_64-pc-windows-gnu`).
- Linux and Windows hosts are both first-class. Use `rfd` for file dialogs and egui's viewport commands for window behaviour, never platform APIs. Shortcuts use `Modifiers::COMMAND` (Ctrl on Linux and Windows).
- egui follows the system light/dark preference by default. The settings popover's `theme_picker` lets the user override it for the session.

## Getting started

```rust
eframe::run_native(APP_NAME, options, Box::new(|cc| {
    ps3dk_ui::install(&cc.egui_ctx);   // both themes, type scale, spacing
    Ok(Box::new(app))
}))
```

Read colours through `Palette::of(ui)` (or `Palette::of_ctx(ctx)`), never from literals, so a widget stays right in both themes.

## Window layout

```
┌──────────────────────────────────────────────────────────────┐
│ [SF] Tool name                 ⚙  New  Open…  Save As…  Save │  app_bar
│      document.sfo • unsaved                                  │
├────────────┬─────────────────────────────────────────────────┤
│ EDIT       │  Page title                                     │
│ ⌂ Overview │  One-line explanation                           │
│ … Titles 3 │  ┌───────────── card ──────────────┐            │
│            │  │ Card title            [badge]   │            │  page
│            │  │ Label          [ field       ]  │            │
│ SCHEMA     │  └─────────────────────────────────┘            │
│ [auto ▾] ⓘ │                                                 │
├────────────┴─────────────────────────────────────────────────┤
│ ● Saved PARAM.SFO                    12 entries · game schema │  status_bar
└──────────────────────────────────────────────────────────────┘
```

| Region | Widget | Rules |
|---|---|---|
| App bar | `app_bar`, `app_title`, `brand_mark` | Two-letter monogram on the accent tile (`SF` for the SFO editor; pick a new pair per tool). Document name and state under the tool name. Actions sit on the right; the one `primary_button` is added first, so it lands at the far right. Settings live in a `popover` behind the `⚙` `icon_button`. |
| Sidebar | `sidebar`, `nav_section`, `nav_item` | Only when the tool has more than one view. Counts on nav items are optional. Global, rarely changed settings (schema, target) go in the bottom slot. |
| Page | `page`, `page_header`, `card`, `card_header` | One topic per card. Content width is capped at 1080 px. |
| Status bar | `status_bar` | The tone dot carries the meaning: `Success` after a save or an applied edit, `Danger` for a failure, `Neutral` otherwise. The right side holds document facts. |
| Empty / first run | `empty_state`, `brand_mark` at 56 px | With no document open, show a welcome page with the open and create actions and their shortcuts, not an empty table. |

## Interaction rules

- **Edits apply when the user leaves the field.** Enter or a focus change commits, and Esc reverts to the stored value. Do not add per-row Apply buttons.
- **Errors appear where they happen.** Draw a red outline on the field with a `field_message(Tone::Danger, …)` under it, and repeat the error in the status bar.
- **Explain jargon in place.** Show a plain name with the machine key in small monospace under it (`field_label`). Longer background goes behind a `help_marker`, not in the layout.
- **Never lose work silently.** A dirty document marks the window title with `●`. Opening, creating or closing over unsaved changes goes through a `modal` with a `danger_button` for the discarding action.
- **Shortcuts:** Ctrl+N new, Ctrl+O open, Ctrl+S save (Save As when the document has no path yet), Ctrl+Shift+S save as. List them in the settings popover with `kbd`.
- **Drag and drop.** A tool that opens files accepts a dropped file and draws a drop overlay while one hovers.
- Use a `toggle` for settings that take effect at once, a `segmented` control for 2 to 4 exclusive choices, a `ComboBox` for longer lists, and a `badge` for state. A badge is never a button.

## Tokens

| Token | Dark | Light | Use |
|---|---|---|---|
| `bg` | `#0e1116` | `#f4f6f9` | behind pages |
| `panel` | `#13171e` | `#ffffff` | app bar, sidebar, status bar |
| `surface` | `#191e27` | `#ffffff` | cards, dialogs |
| `surface_alt` | `#10141a` | `#f1f3f7` | inputs, table stripes |
| `border` / `border_strong` | `#262c38` / `#3a4354` | `#dde2ea` / `#bfc7d3` | outlines, rules |
| `text` / `text_muted` / `text_faint` | `#e7eaf0` / `#a0a8b8` / `#6c7486` | `#171b22` / `#535c6e` / `#8a93a5` | body / secondary / captions |
| `accent` | `#4f8cff` | `#2f6be0` | primary action, selection, focus |
| `success` / `warning` / `danger` | `#3dbe84` / `#e5a93c` / `#ef5b66` | `#1f9461` / `#b7791f` / `#d1384a` | `Tone` colours |

Type scale: heading 22, title 16 (`title_style()`), body and buttons 14, monospace 13, small 12, caption 11.5 upper-case (`caption_style()`). Spacing steps are `SPACE_XS..SPACE_XL` (4, 8, 12, 16, 24). Radii: `RADIUS_MD` (6) for controls, `RADIUS_LG` (10) for cards and dialogs.

## Adding a widget

If a second tool needs a control the crate lacks, add it to `ps3dk-ui` rather than styling it locally. The widget takes its colours from `Palette` and its sizes from the spacing constants, and gets a one-line doc comment that says when to use it.
