# AntShell - a Unix-style shell for Windows

AntShell (Anthropoid Shell) is a command-line shell for Windows, written in plain C. It
brings the `sh` syntax (pipes, `&&`, `||`, redirections, background jobs),
[fish](https://fishshell.com)-style autosuggestions and TAB completion, the familiar
Windows console editing keys, a built-in calculator, folder shortcuts and Windows Terminal
integration, with no dependency beyond the C runtime and the Windows API.

```
>make 2>&1 | grep error || antshell
>cd src\sh<TAB>              → cd src\shell\
>..                           (implicit cd: goes up one level)
>ping -n 30 server > ping.txt &
[1] 8124
```

The prompt is just `>`; the current folder is shown with `/`, and `history`
shows the folder of each command (in gray).

<details>
  <summary><strong>Table of Contents</strong></summary>

- [Quick Start](#quick-start)
- [Building from source](#building-from-source)
- [Syntax](#syntax)
- [Commands](#commands)
- [Line editor](#line-editor)
- [TAB](#tab)
- [Configuration (`data\config.ant`)](#configuration-dataconfigant)
- [Windows Terminal](#windows-terminal)
- [Programs in `bin\`](#programs-in-bin)
- [Code](#code)
- [Tests](#tests)
- [Contributing](#contributing)
- [Credits](#credits)
- [License](#license)

</details>

This repository contains:

* `antshell.exe`, the shell itself (`src\`)
* the Unix-style programs it relies on (`ls`, `cat`, `grep`, `cp`...) in `bin\`
* `note`, a small text editor for the console
* a few more tools and toys in `bin\`: `vplay`, `dump`, `ascii`, `color`, `core` and the
  games `cube`, `pong` and `brick`
* the test suites (`tests\`)

---

## Quick Start

There are no binary releases yet, so AntShell is built from source (see
[Building from source](#building-from-source) for the requirements):

```bat
make            REM builds antshell.exe
make bin        REM builds the Unix-style programs (ls, cat, grep...) into bin\
antshell.exe    REM the first run creates data\config.ant, with bin\ already in [path]
```

AntShell works like `sh` or `bash` for everyday use. A few things worth trying right away:

- type `help` for the list of commands and the syntax (`help name` shows just one entry);
- type `2+3`, `sqrt(16)` or `2*pi`: a line that is only an expression is a calculation;
- press TAB after `cd src\` and keep pressing it: a menu with the options opens, like in fish;
- press ← on an empty line for a menu of the folders you visited in this session;
- run `ping -n 30 server > ping.txt &` and use `jobs`, `wait` and `kill %1`.

`link -f`, `path -f` and `alias -f` open the configuration file in `$EDITOR`, in `note` or in
the Windows text editor, whichever is found first.

---

## Building from source

Requirements: Windows 10 or newer, [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
(`clang`) and `make` (GnuWin32 `make` 3.81 or llvm-mingw's `mingw32-make`).

```bat
make          builds src\ into obj\ (icon included) and produces antshell.exe
make run      builds and opens the shell
make test     runs all the tests
make clean    deletes the objects, recicon.res and the executables
make icon     only builds the icon and version info (icon\recicon.rc → obj\recicon.res)
make dist     builds everything and packs antshell-win64.zip: antshell.exe, the programs of bin\ and data\help.ant, mk.ant
make bin      builds the programs in bin\ through bin\Makefile (inside bin\: `make -j4`, `make ls.exe`, `make note-test`, `make clean`)
```

`antshell.exe` looks for the `data\` folder next to itself. To open it already running
a command (the shell stays open afterwards): `antshell.exe cd src`. The arguments become
one command line of at most 1023 bytes; an argument with spaces goes in quotes.

The icon (`icon\shell_icon.ico`) and the version information that Explorer shows under
Properties → Details live in `icon\recicon.rc`; `make` rebuilds the `.res` when either
of them changes.

Optional: `ant desktop context menu.reg` adds an "AntShell" entry to the Explorer context
menu of folders. Edit the example path inside it first (the file explains how, and how
to remove the entries).

---

## Syntax

The line follows the `sh`/`bash` model:

| Line | What it does |
|---|---|
| `a \| b` | **pipe**: the output of `a` becomes the input of `b` |
| `a ; b` | runs `a` and then `b` |
| `a && b` | runs `b` only if `a` exited with code 0 |
| `a \|\| b` | runs `b` only if `a` failed |
| `a &` | runs `a` in the background (see `jobs`) |
| `< file` | input from the file |
| `> file` / `>> file` | output to the file (overwrite / append) |
| `2> file` / `2>> file` | errors to the file |
| `2>&1` | errors together with the output (`make > log.txt 2>&1`) |
| `/dev/null` | discards (becomes the Windows `NUL`) |

- **Quotes** `"..."` and `'...'` keep spaces and operators: `echo "a | b"` prints `a | b`.
  Adjacent pieces become one word: `a"b c"d` → `ab cd`.
- **The backslash is a path separator, not an escape.** To escape, use quotes.
- **Expansions**, done by the shell on each command right before running it (so
  `false; echo $?` and `export A=1 && echo $A` see the current state):

  | Written | Becomes |
  |---|---|
  | `$NAME`, `${NAME}` | the value of the environment variable (empty if it does not exist) |
  | `$?` | the exit code of the last command |
  | `~`, `~\folder`, `~/folder` | the user's folder (`HOME`, or `USERPROFILE`); only at the start of a word |
  | `*`, `?` | the matching file names, in order (`ls src\*.c`, `rm *.tmp`); with no match it stays as typed; names starting with `.` only match a `.` in the pattern |

  Single quotes expand nothing; double quotes expand only `$`. The value of a variable is
  **a single word** (it is not split on spaces, so `cd $DIR` works with a space in the path)
  and is not globbed. An empty, unquoted `$NAME` disappears from the line. `2 * 3` is still a
  calculation, not a glob.
- **Exit code** of the last command (the `$?` of sh): the program's own; `9009` for
  command not found (as in cmd); `2` for a syntax error. In a pipe, the last command's code counts.
- Errors go to the error output, so `2>` also catches the errors of the built-in commands.
- Programs see the console's original code page, with or without redirection
  (`dir > f.txt` writes as in cmd); the built-in commands write UTF-8.

---

## Commands

| Command | What it does |
|---|---|
| `cd` | with no argument goes to the home folder (`$HOME`; without it, `%USERPROFILE%`), as in POSIX |
| `cd <folder>` | changes folder (quotes optional: `cd my folder`) |
| `cd..` / `cd\` | goes up one level / goes to the root |
| `cd -` | goes back to the previous folder (`$OLDPWD`); every `cd` keeps `PWD` and `OLDPWD` in the environment |
| `..`, `src\`, `.\build`, `~`, `D:` | **implicit cd** (as in fish): a folder path alone on the line enters it |
| `/` | shows the current folder |
| `link` | lists the folder shortcuts (`link -f` opens `config.ant`) |
| `name` | opens the shortcut: a program runs in the console; a folder or file opens in the default program |
| `name\` | enters the shortcut's folder |
| `name.` | opens the shortcut and closes the shell |
| `@prog args` | opens through Windows, in its own window, without waiting; a bare `@` opens the current folder |
| `bg [-o] [-d] program [args...]` | runs the program in the background, in its own hidden console: the terminal stays free and closing it does not end the program. Whatever it writes (a progress bar, for example) stays in that console, to be seen later with `show pid`. It becomes a job (`jobs`, `wait %n`, `kill %n`) and prints `[n] pid`. `-o` sends stdout and stderr to `<program>_<pid>.txt` in the current folder (one file per run: `bg -o ping -n 30 server` writes `ping_11804.txt`; the input becomes `NUL`). With `-o` nothing is written to the console, so `show` opens an empty window: use `bg` without `-o` to see the prints (progress bars) with `show`, and `-o` only when you want the record in a file. `-d` disables the close button of the hidden console, so the shown window cannot end the program by accident. Programs only: built-ins, aliases and `[link]` run inside the shell itself and give an error |
| `show [-h] pid ...` | shows the hidden console of a program started with `bg` (`jobs` lists the pids); `-h` hides it again |
| `jobs` | lists the background commands |
| `wait [%n \| pid ...]` | waits for the jobs (all of them, with no argument) and returns in `$?` the code of the last one waited for; 127 if the job does not exist; Ctrl+C interrupts the wait (130) and the job keeps running |
| `kill [-SIG] %n ...` | ends a job (`%%` or `%+` is the current one). Without `%`, `kill` is the program in `bin\` |
| `echo [-n] text` | writes the words separated by a space (`-n`: no line break) |
| `export NAME=value` | sets an environment variable for the following programs (lasts until the shell closes; to keep it, use `[export]` in `config.ant`); a bare `export` lists them |
| `NAME=value` | the same as `export` (there are no shell-only variables) |
| `unset NAME` | removes the variable |
| `which [-a] name` | what the name runs, in the shell's order: alias, built-in, `[link]` shortcut, `[path]`, and the rest of PATH; `-a` shows all of them |
| `mk [-f] file...` | creates files from the templates in `data\mk.ant`: `mk main.c` already brings the hello world, `mk main` (no template) creates an empty file; the exact name wins (`Makefile`, `LICENSE`, `.gitignore`) and then the extension; creates the folders of the path (`mk src\a\x.h`); does not overwrite (`-f` overwrites); `mk -l` lists the templates. In `mk.ant`, `[.c .h]` or `[=Makefile]` starts a template and `{name}`, `{NAME}` and `{year}` are replaced |
| `true` / `false` | only set the exit code (0 / 1) |
| `history` | log of every command (`42 2026-09-30 14:05:33 folder> command`, in `data\history.ant`; `history N` the last N lines, `history all` everything, `history clear` erases) |
| `path` | folders where programs are searched (`path -f` opens `config.ant`) |
| `alias` | lists the aliases (`alias name` shows one; `alias -f` opens `config.ant`) |
| `2+3`, `(1+2)*3`, `10%3`, `sqrt(16)`, `2*pi`, `2 3 +` | calculator (infix or RPN; `+ - * / % ^ =` and parentheses; functions `sqrt abs round log ln` (with the `(` attached to the name; `log` is base 10) and constants `pi` and `e` (only inside an expression: a bare `pi` or `e` is still a command); `^` right to left, `-2^2` gives -4). In the console the result comes out in big letters; redirected (`2+3 | cat`, `> file`) it comes out as plain text. Error: `ant: calc: division by zero` (status 1) or a syntax error (status 2) |
| `.` | clears the screen |
| `ver` | logo and version |
| `help` | list of commands and syntax; `help name` shows only the line of that command (`help history`, `help tail`); without an exact name, the ones that start with it (`help his`). Every built-in accepts `--help` as its only argument (`cd --help`, `which --help`) and shows the same text; the programs in `bin\` have their own `--help` |
| `exit [n]` / `nop [n]` | closes the shell with code `n` (0 to 255, as in POSIX); a bare `exit` uses the code of the last command (`$?`), a bare `nop` uses 0; text that is not a number closes with 2 |

The **calculator** only kicks in when the whole line is an expression. The rules that
tell it apart from commands:

- **Characters:** only digits, spaces and `. + - * / ^ % ( ) =`, plus the names below. Any
  other character (`7z`, `C:\x`, `a=1`, `echo 5`) makes the line a command.
- **Functions** (`sqrt abs round log ln`): the `(` is attached to the name. `sqrt(9)` is a
  calculation, `sqrt 9` is a command; a name that is neither a function nor a constant
  (`foo(2)`) is also a command.
- **Constants** (`pi`, `e`): only count inside an expression, with a digit or an operator on
  the line (`2*pi`, `e^2`, `pi 2 *`). A bare `pi` and `e` are still commands (or `[link]`
  shortcuts); to see the value, `pi+0`.
- **Precedence**, from lowest to highest: `=`, `+ -`, `* / %`, sign `- +`, `^` (right to
  left: `2^3^2` is 512 and `-2^2` is -4).
- **RPN** (`2 3 +`): applies when the infix reading fails and there are spaces; accepts
  `+ - * / % ^ =` and the constants. The shell does not expand the `*` of a calculation (`2 * 3`).
- **Output:** in the console, big letters (only for what fits on the line); redirected
  (`2+3 | cat`, `> file`), plain text. Errors: `ant: calc: division by zero` (status 1) or a
  syntax error (status 2).

The **implicit cd** only applies to a word that looks like a path, alone on the line:
`..`, something ending in `\` or `/`, starting with `.\`, `..\`, `\` or `~`, or a
drive (`D:`, `D:\games`). That way `make` is still a command even if a folder called
`make` exists, and `make\` enters it. A `name\` shortcut from `[link]` takes priority over
a folder of the same name in the current folder. Like `cd`, the implicit cd adds to the
session's folder list used by left-cd.

Any other word is run as a program. The search follows this order: the `[path]` folders
of `data\config.ant` and then the normal Windows search (the folder of `antshell.exe`,
the current folder, the system folders and `PATH`). Without an extension, it looks for `name.exe`.

Background commands (`&`) print `[n] pid`, do not receive a Ctrl+C given to another
program, read their input from `NUL` (they do not steal the keyboard) and, before the next
prompt, the shell reports `[n] done  <command>` (or `[n] exit <code>`, if it failed).

---

## Line editor

The keys follow the Windows console line editor (the same as `cmd.exe`):

| Key | What it does |
|---|---|
| ← → / Ctrl+← → | moves by character / by word |
| ← with an empty line | **left-cd**: a menu with the session's folders (↑ ↓ Home End PgUp PgDn choose, Enter goes straight to the folder, ← or Esc close it) |
| Home / End | start / end of the line |
| Ctrl+Home / Ctrl+End | deletes to the start / to the end |
| Backspace / Ctrl+Backspace / Delete | deletes a character / a word / the character ahead |
| Insert | toggles insert and overwrite |
| Esc | clears the line (with the TAB menu open, it only closes the menu) |
| Ctrl+C | cancels the line; in a running program, interrupts the program |
| ↑ ↓ / F5 | previous / next command |
| PgUp / PgDn | oldest / newest command |
| → / End at the end of the line | accepts the autosuggestion |
| Ctrl+→ at the end of the line | accepts one word of the autosuggestion |
| → or F1 at the end of the line | with no autosuggestion (or always, with F1): copies one character from the previous command |
| F2 / F3 / F4 | copies from the previous one up to a character / copies the rest of the previous one / deletes up to a character |
| Ctrl+V / Shift+Insert | pastes the clipboard at the cursor (only the 1st line, tab becomes a space) |
| Ctrl+Insert | copies the whole line to the clipboard (to copy text from the screen: the terminal's selection) |
| F7 / Alt+F7 | history list (Enter runs, ←→ copies, Delete removes) / erases the history |
| F8 | searches the history for commands that start with what is before the cursor |
| F9 | looks up a history command by number |

**Autosuggestion**, as in fish: while you type, the newest history command that starts
with the text of the line (case-insensitive) shows in gray after the cursor. Enter runs
only what was typed.

The shell's messages are bash-style (`ant: x: command not found`, `syntax error near '|'`);
the F2, F4 and F9 popups use conhost's texts (`Enter char to copy up to:`).

Accents, CJK and emoji work, and the cursor moves by visible character (grapheme),
with the same widths as Windows Terminal (Unicode 16).

---

## TAB

- Completes the word before the cursor, relative to the typed path:
  `cd src\mo<TAB>` looks for `mo…` inside `src\`.
- **One option:** goes in directly. A folder ends in `\`, so the next TAB already
  lists its contents (`cd s<TAB><TAB>` with no need for `dir`); a file gets a space at the end.
- **Several options:** opens a menu below the line with the first one already selected.
  TAB/Shift+TAB and the arrows switch; Enter accepts without running; Esc goes back to what was typed.
- **TAB on an empty line** lists the current folder.
- **In command position** (1st word and after `|`, `;`, `&&`, `||`, `&`, including
  with `@` in front), it completes built-ins and `.exe` programs; after `<` and `>`,
  files.
- Matching follows fish: prefix before substring, before subsequence;
  typing in lower case ignores case (`mak` finds `Makefile`). Hidden and system files
  only show up after something was typed; names with spaces come out in quotes, and
  names with `$` in single quotes (double quotes would still expand the `$`).

---

## Configuration (`data\config.ant`)

A single file, in sections, next to `antshell.exe`. The shell reads the file again whenever
it changes (it takes effect at the next prompt), and `link -f`, `path -f` and `alias -f`
open it, in this order: in the program named by the `EDITOR` variable (an `.exe`, optionally
with arguments, e.g. `EDITOR = subl -w`), in `note` (`bin\note.exe`, which runs in the
console itself while the shell waits for it to close) and, last, in the Windows `.txt`
editor (its own window, without waiting). If the `EDITOR` program does not start, it falls
through to the next one.

```ini
# comment (only at the start of a line)

[path]
# folders where programs are searched, before PATH
C:\llvm-mingw\bin
$USERPROFILE\bin

[export]
# environment variables (programs opened by the shell inherit them)
EDITOR = subl
MY_BIN = $USERPROFILE\bin

[alias]
# the 1st word of a command becomes the alias text, as in sh
ll = ls -l
up = cd.. && ll

[link]
# name opens, name\ enters the folder, name. opens and closes the shell
doc = C:\Users\user\Documents
vid = D:\Videos
```

- `$VAR` and `${VAR}` become the value of the variable (in `[path]`, `[export]` and `[link]`);
  inside `'single quotes'` the value stays literal. `[alias]` stays as it is.
- `"quotes"` keep spaces at the start and end of the value.
- A repeated name: the first one wins. Lines with errors are reported when the shell opens,
  with the line number (`config.ant, line 5: missing '=' in "..."`).
- The file is UTF-8 (ANSI is also accepted).

The first time `antshell.exe` opens without a `config.ant`, it creates one from a template,
with the `bin\` folder next to `antshell.exe` already in `[path]`, so the programs of `bin\`
work right away. `data\config.ant` is personal and stays out of git (`.gitignore`): each
person has their own.

Other files in `data\`:

| File | Contents |
|---|---|
| `help.ant` | text of the `help` command |
| `mk.ant` | templates of the `mk` command |

---

## Windows Terminal

The shell marks prompt, command and output with Windows Terminal's integration
sequences (OSC 133, the same ones as VS Code) and reports the current folder (OSC 9;9).
In plain conhost they do not show.

With that, in Windows Terminal:

- **duplicate tab** and **split pane** open in the folder where the shell is;
- with `"showMarksOnScrollbar": true` in the profile, each command shows up as a mark
  on the scrollbar, and the `scrollToMark` action (`"direction": "previous"` or `"next"`)
  jumps from command to command;
- each end-of-command mark carries the exit code (`0`, `9009`, …).

---

## Programs in `bin\`

The shell only brings what has to be built in (`cd`, `export`, `echo`, `which`, `history`...).
The rest of the Unix commands are small programs in `bin\` (already in its `[path]`), one
per folder, built with `make bin`:

| Program | What it does |
|---|---|
| `ls [-a] [-l] [-h] [-1]` | lists; in the console it comes out in columns with colors (folder blue, program green, link cyan); in a pipe, one name per line |
| `cat [-n]` | writes the files (or standard input) |
| `mkdir [-p]` | creates folders (`-p`: with the parents, no error if it already exists) |
| `pwd` | the current folder (by convention; the built-in `/` does the same) |
| `rm [-r] [-f]` | deletes (no recycle bin); refuses `.`, `..` and the root of a drive |
| `cp [-r] [-f]` | copies files and, with `-r`, folders; several into a folder |
| `mv` | moves or renames, across drives too |
| `touch [-c]` | creates the file or updates its date |
| `head`, `tail` | `-n N`, `-N`, `-c N`; `tail -n +N` and `tail -f` |
| `wc [-l -w -m -c]` | lines, words, characters (UTF-8) and bytes |
| `sort [-r -n -u -f]` | sorts lines |
| `uniq [-c -d -u -i]` | merges repeated consecutive lines |
| `tee [-a]` | copies the input to the output and to files |
| `find [path] [tests]` | `-name`/`-iname` (`* ? [a-z] [!a]`), `-type f\|d\|l`, `-size [+-]N[ckMGb]`, `-empty`, `-mindepth`/`-maxdepth`, `!`; actions `-print` (default), `-print0` and `-delete`. The tests are all mandatory (no `-o`); protect the patterns with quotes |
| `du [-a -s -h -c -d N]` | space used by files in KiB (sum of the sizes), per folder |
| `sleep time` | waits; accepts decimals and the suffixes `s`, `m`, `h`, `d` |
| `ps [-f -h -m] [-p pid] [-C name]` | lists the running processes: pid, parent, threads, memory (KiB) and name; `-f` shows the path, `-m` sorts by memory; to filter, `ps \| grep name` |
| `pkill [-SIG] [-f] [-l] name` | ends processes by name (exact, with or without `.exe`, case-insensitive; accepts `* ? [a-z]` in quotes); `-f` matches part of the full path, `-l` only lists (like `pgrep`). It never touches itself or whoever started it (the shell). Exits with 1 if nothing matched |
| `xargs [-0] [-n N] [-I str] [-P N] [-r] [-t] [command]` | builds command lines from the words of the input and runs them (`find -name "*.tmp" -print0 | xargs -0 rm`). Words separated by space/newline, `'..'` and `"..."` group; `-0` separates by NUL; `-n N` limits the words per command; `-I str` runs one command per line replacing `str`; `-P N` runs up to N at the same time; `-r` does not run with empty input; `-t` shows the line before running. Without a command it writes the words (like `echo`). The command is a program (the folder of `xargs.exe`, the current one and PATH), not a built-in or an alias; the command's input is `NUL`. Exits with 123 if a command failed, 127 if it was not found |
| `kill [-s SIG \| -SIG \| -0] pid` | ends processes (on Windows every signal ends them, with code 128 + the signal number); `kill -l` lists the names. For shell jobs use `kill %1` |
| `grep [-i -n -v -c -l -q -r -F -h -H -e]` | searches lines; small regex: `. [a-z] [^0-9] [[:alpha:]] * + ? ^ $ a\|b \d \w \s` |

Conventions: combinable short flags (`rm -rf`), `--` ends the options, `--help` in each
one, Unix-style errors (`rm: x: No such file or directory`, on the error output) and exit
code 0 (ok), 1 (error) or 2 (wrong usage; in `grep`, 1 means "nothing matched"). Names arrive
as UTF-16 and the output is UTF-8 (in the console, `WriteConsoleW`). The programs do not expand
`*` or `?`: the shell does. `sort` and `find` also exist in `system32`; inside the shell the
ones in `bin\` win, and `which -a sort` shows both.

Also in `bin\`, outside the Unix-style set:

| Program | What it does |
|---|---|
| `note [file]` | a small, modeless text editor for the console: menu bar (`Alt`), mouse, syntax highlighting and UTF-8; see `bin\note\README.md`. It reads `note.cfg` from its own folder |
| `vplay [file]` | a borderless, always-on-top video overlay (MCI): drag to move, mouse wheel to zoom; `vplay --help` lists the keys |
| `dump [-c columns] [-s jump] [-n range] file` | hex dump of a file |
| `ascii` | prints the ASCII table |
| `color` | the 256 text attributes of the console, 16 by 16 |
| `core` | memory and disk usage, one bar per item |
| `cube`, `pong`, `brick` | a spinning 3D cube and two small games; `brick` is a falling-blocks game on an 11x20 field, and its high score is kept in `brick.dat` |

---

## Code

One folder per topic inside `src\`, as in the Windows Terminal repository; each `.h`
sits next to its `.c`. Code, comments and messages are in English.

| Folder | Contents |
|---|---|
| `src\main.c` | entry point |
| `src\shell\` | main loop, line parser, expansions (`$VAR`, `~`, glob), execution (pipes, redirections, jobs), arguments of `antshell.exe` |
| `src\builtins\` | built-in commands (`cd`, `link`, `path`, `help`, `echo`, `export`, `which`, `mk`, `bg`, calculator, `.`) and their dispatch, with `alias` and `history` |
| `src\editor\` | line editor, command history and TAB completion |
| `src\config\` | reading of `data\config.ant` |
| `src\console\` | console output: colors, cursor, errors, logo |
| `src\types\` | Unicode widths and graphemes, string utilities |
| `bin\` | `ls`, `cat`, `cp`, `grep`... (one per folder, sharing `bin\common.h`) |

---

## Tests

`make test` runs nine sets:

| Test | What it covers |
|---|---|
| `tests\grapheme_test.c` | graphemes and widths against the official Unicode 16 test |
| `tests\parse_test.c` | the line grammar (quotes, operators, redirections, aliases, errors) |
| `tests\expand_test.c` | `$VAR`, `$?`, `~` and glob, with quotes and in a temporary folder |
| `tests\calc_test.c` | the calculator engine: precedence, RPN, functions, errors, and what counts as an expression |
| `tests\bin_test.c` | the programs in `bin\` (output, errors and exit codes) |
| `tests\config_test.c` | `config.ant`: sections, warnings, `$VAR` and the template of a new file |
| `tests\complete_test.c` | TAB completion, on a temporary folder tree |
| `tests\input_test.c` | the real shell in a hidden console: injected keys and the screen read back |
| `tests\osc_test.c` | the shell inside a ConPTY: OSC 133 marks, exit codes and Ctrl+C |

`osc_test` also runs on Windows Terminal's `OpenConsole.exe`, if it exists
(`make test OPENCONSOLE="C:\...\OpenConsole.exe"`); without it, that part is skipped.
`note` has its own suite: `make -C bin note-test`.

---

## Contributing

- Code, comments, messages and tests are in English. Follow the style of the file you are
  touching (tabs, `/* */` comments, the `ant_` prefix for public functions).
- A new built-in command: a function `ant_cmd_<name>` in `src\builtins\`, an entry in
  `ant_builtin` and `ant_is_builtin` (and in `ant_builtin_names` if TAB should complete it),
  a line in `data\help.ant` and a row in this README.
- A new program in `bin\`: `bin\<name>\<name>.c` and its name in `UNIX_PROGS` or `TOOL_PROGS` in
  `bin\Makefile` (the only Makefile of the tree).
- Run `make test` before sending a change. The tests check the text of the messages, so a
  changed message means a changed test.
- The `release` workflow (`.github\workflows\release.yml`) builds with `WERROR=1` (warnings are
  errors; try `make WERROR=1` locally) and runs `make test` on every push and pull request. Pushing
  a tag such as `v1.0` also packs the package with `make dist` and attaches it to a draft release.
- `data\config.ant`, `data\history.ant` and `bin\brick.dat` are personal and ignored by
  git: do not add them.

---

## Credits

- `src\types\unicode.c` and `tests\grapheme_break_data.inc` are ports of Windows
  Terminal's `CodepointWidthDetector` and tests
  ([Windows Terminal](https://github.com/microsoft/terminal); MIT, © Microsoft Corporation;
  the license notice is in the file itself).
- The line editor follows conhost's `COOKED_READ_DATA`, the history follows
  `CommandHistory` and TAB follows the behavior of the [fish shell](https://github.com/fish-shell/fish-shell);
  new C code was written from them, with no copying.

## License

MIT, see the LICENSE file (it includes the Windows Terminal code notice).
