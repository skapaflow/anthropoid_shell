# note

A small, modeless text editor for the Windows console, in the spirit of MS-DOS
`EDIT`. You open a file and type; there is no insert/command mode to switch.
`Esc` asks whether to exit, and a menu bar (`Alt`) lists every command.

Written in plain C with the Windows API only: no dependencies, one executable.

```
 File  Edit  Search  View  Help                                     main.c *
   1 #include <stdio.h>
   2
   3 int main (void) {
   4     printf("hello\n");       ┌──────────── note ────────────┐
   5     return 0;                │                              │
   6 }                            │   Save changes to main.c?    │
   7                              │                              │
   8                              │  Save   Don't Save   Cancel  │
   9                              └──────────────────────────────┘
 F1 Help   Alt Menu   Esc Exit          Ln 4/7, Col 5   C   UTF-8   CRLF
```

## Features

- **Editing:** selection with Shift or the mouse, cut/copy/paste through the Windows
  clipboard, undo/redo, auto-indent, indent/unindent of selected lines, word moves.
- **Search:** find (case-insensitive unless the query has uppercase), find next/previous
  with wrap-around, replace with a per-match prompt or all at once, go to line.
- **Menu bar** in the style of EDIT: `Alt` or `F10` activates it, `Alt+letter` opens a
  menu, and the mouse works on it too.
- **Mouse:** click, drag to select, double-click selects a word, the wheel scrolls.
- **View:** word wrap, line numbers, current line highlight, matching bracket highlight.
- **Syntax highlighting** for 25 languages (see below).
- **Files:**
  - UTF-8 (with or without BOM), UTF-16 LE/BE and ANSI (the Windows code page, for old files).
  - Files are saved back in the encoding and line endings (CRLF/LF) they were read with.
  - Saving writes a temporary file and swaps it in, so a failed save never destroys the original.
  - If an ANSI file gets characters its code page lacks, note asks whether to save as UTF-8.
- **Recovery:** if the console window (or terminal tab) is closed with unsaved changes,
  they are written to `<file>.note-recovery`. note offers them back the next time you
  open the file.
- **Tab completion** of file names in the Open and Save As dialogs.
- **Unicode aware:** accents, CJK, emoji and combining marks line up with the terminal,
  both in Windows Terminal and in the classic console.

## Usage

```
note [file[:line[:column]]]
```

- `note` opens an untitled buffer.
- `note main.c` opens (or creates) `main.c`.
- `note main.c:120:5` opens at line 120, column 5, the format compilers print.

## Keys

| Keys | Action |
|---|---|
| `Ctrl+N` `Ctrl+O` `Ctrl+S` | New, open, save |
| `Esc` | Exit (asks first) |
| `Ctrl+Z` `Ctrl+Y` | Undo, redo |
| `Ctrl+X` `Ctrl+C` `Ctrl+V` | Cut, copy, paste (with no selection: the whole line) |
| `Ctrl+A` | Select all |
| `Shift` + arrows / `Home` / `End` / `PgUp` / `PgDn` | Select |
| `Ctrl+←` `Ctrl+→` | Move by word |
| `Ctrl+Backspace` `Ctrl+Delete` | Delete a word |
| `Tab` `Shift+Tab` | Indent / unindent the selected lines |
| `Home` | First non-blank character, then column 1 |
| `Ctrl+Home` `Ctrl+End` | Start / end of the file |
| `Ctrl+F` `F3` `Shift+F3` | Find, next, previous |
| `Ctrl+H` | Replace |
| `Ctrl+G` | Go to line |
| `Alt+Z` | Word wrap |
| `Alt` `F10` | Menu bar (`Alt+F`, `Alt+E`, ... open a menu) |
| `F1` | Keyboard shortcuts |

In dialogs, `Tab` moves between fields and buttons, `Enter` confirms, `Esc` cancels, and
the highlighted letter of a button picks it. In the Open and Save As dialogs, `Tab` and
`Shift+Tab` cycle through the matching files. To list the contents of a completed folder
(`src\`), press `End`, then `Tab`.

## Configuration

note reads `note.cfg` from the folder of `note.exe`. Anything missing keeps its default,
and the defaults are the Monokai theme shipped in `note.cfg`. A bad line is reported in the
status bar at startup (`note.cfg:4: bad setting 'foo'`).

```ini
[editor]
tab_size       = 4
expand_tabs    = false    # Tab inserts spaces
word_wrap      = false
line_numbers   = true
syntax         = true
auto_indent    = true
highlight_line = true
mouse          = true     # false leaves mouse selection to the terminal

[colors]
# key = #RRGGBB [on #RRGGBB] [bold] [italic] [underline]
# "none" leaves a color unset (it falls back to the text style)
text      = #F8F8F2 on #272822
keyword   = #F92672
type      = #66D9EF italic
selection = none on #49483E
```

The `[colors]` keys cover:

- **Text area:** `text`, `current_line`, `selection`, `bracket`, `line_number`,
  `line_number_current`, `control`.
- **Syntax:** `comment`, `keyword`, `type`, `function`, `string`, `number`, `constant`,
  `preprocessor`, `operator`.
- **Menus:** `menu_bar`, `menu_hotkey`, `menu`, `menu_selected`, `menu_shortcut`.
- **Status bar:** `status_bar`, `status_message`, `status_error`.
- **Dialogs:** `dialog`, `dialog_title`, `button`, `button_selected`, `button_hotkey`,
  `field`, `shadow`.

The View menu toggles only last for the session; set them in `note.cfg` to keep them.

## Syntax highlighting

| Group | Languages |
|---|---|
| C family | C, C++, C#, Java, Go, Rust |
| Web | JavaScript, TypeScript, HTML/XML, CSS, SCSS/LESS, JSON |
| Scripts | Python, Lua, Ruby, Shell, PowerShell, Batch, SQL |
| Build and data | Makefile, Dockerfile, YAML, Config (INI/TOML/cfg), Markdown, Diff |

The language comes from the file extension, or from the whole name for files like
`Makefile` and `Dockerfile`. Each language is one row of a table in `src/syntax.c`:
comment and string delimiters, word lists and a few flags, read by one generic lexer.
Adding a similar language means adding a row. HTML/XML, config files and diffs have
small lexers of their own.

## Building

Requires Windows 10 or newer (the console must support VT sequences), `clang` (or `gcc`)
and GNU `make`. note is built by `bin\Makefile` (the only Makefile of the tree), which
uses `cmd.exe` as its shell. Run these from `bin\`, or use `make bin` from the root:

```bat
make note.exe               REM build note.exe (it reads note.cfg from its own folder)
make note-test              REM build and run the test suite
make clean                  REM remove every program built from bin\, note.exe included
make note.exe CC=gcc        REM use another compiler
```

`make note-test` builds `tests/note_test.c`. It starts note in a hidden console, types keys
and mouse events into it, and checks the screen and the bytes written to disk. The
clipboard is saved and restored around the run.

## Source layout

| File | Purpose |
|---|---|
| `src/main.c` | Command line (`file:line:col`) |
| `src/editor.c` | Editor state, commands, keys, mouse, search, files |
| `src/render.c` | Screen layout: menu bar, line numbers, text, word wrap, status bar |
| `src/buffer.c` | Lines of text, loading and saving in each encoding |
| `src/history.c` | Undo/redo steps |
| `src/syntax.c` | Language table and lexers |
| `src/menu.c` | Menu bar and drop-down menus |
| `src/dialog.c` | Question, form and info boxes; file name completion |
| `src/screen.c` | Console setup, input events, output buffer, 24-bit colors |
| `src/config.c` | `note.cfg` and the default theme |
| `src/clipboard.c` | Windows clipboard |
| `src/ucd.c` | Unicode character width and grapheme clusters |

## License

`src/ucd.c` is a C port of the `CodepointWidthDetector` of
[Windows Terminal](https://github.com/microsoft/terminal), © Microsoft Corporation,
under the MIT license (the full notice is at the top of the file).
