# siplasplas-plugins

Shared Qt 6 plugins and their interfaces for
[gemini-commander](https://github.com/siplasplas/gemini-commander),
[ducat](https://github.com/siplasplas/ducat) and
[eudaimonia](https://github.com/eudahub/eudaimonia). A plugin keeps its
dependencies (e.g. libarchive) out of the programs, which work without it.

## Plugins

| Plugin | Interface | Needs | Does |
|--------|-----------|-------|------|
| `spp-archive` | `spp::ArchivePlugin` (`siplasplas/plugin/ArchivePlugin.h`) | libarchive (libarchive-dev); optionally `lsar`/`unar` (unar) and `7z` (p7zip-full or 7zip) at run time | lists archives (with progress, cancelling and passwords), extracts all or chosen entries (asking about existing targets: overwrite, skip or cancel) and reads the start of an entry without decompressing the rest, for previews — also of every file on the way to it (`readEntries`), which a solid archive decompresses anyway; packs files and directories (with progress, cancelling, compression level, a password for zip, volumes) |
| `spp-editor` | `spp::EditorPlugin`, `spp::EditorView` (`siplasplas/plugin/EditorPlugin.h`) | Qt Widgets; [qcodeedit](https://github.com/siplasplas/qcodeedit) 1.70 with [cpg](https://github.com/siplasplas/cpg), fetched and built in (ICU, libicu-dev) | a code editor widget for the program's own window and tabs: opens and saves files in their encoding, colours them with Kate syntax definitions and the Kate theme matching the desktop, reloads files changed on disk as one undo step; see below |
| `spp-findbar` | `spp::FindPlugin`, `spp::FindBar`, `spp::FindTarget` (`siplasplas/plugin/FindPlugin.h`) | Qt Widgets | a find and replace bar for the editor plugin's editors and other find targets (e.g. a viewer of huge files), through their interface only; see below |

Extracting keeps the entries' times and permissions (also of read-only
directories, set at the end), makes symbolic links, and never writes outside
the destination: entries with `..` or absolute paths, and entries below a
symbolic link, are left out.

The archive plugin reads zip (also with a password), 7z, tar with gzip,
bzip2, xz, Zstandard, LZ4, lzip or compress, RAR without encryption, CAB,
LHA/LZH, ISO 9660, XAR, cpio and ar. An archive split into volumes is read
as one from whichever volume is given: `name.001`, `name.002`, ... (7-Zip),
RAR's `name.part1.rar`, `name.part2.rar`, ... and the old `name.rar`,
`name.r00`, `name.r01`, ... (RAR 4 and 5, files spanning volumes too). Old
zips name their files in a DOS code page without saying which: such names
are read as UTF-8 when they are valid UTF-8 (newer tools), else in the DOS
code page of the system's language, as Windows does — CP852 for Polish,
Czech, Hungarian and the like, CP866 for Russian and the like, CP850 for
Western European languages, otherwise CP437, the zip format's own.

What libarchive cannot read, or reads only in part — ARJ, ACE, encrypted RAR
and 7z, solid RAR 1.5 from DOS times — the plugin reads by external programs
when they are installed, through the same calls: it lists by `lsar` (or
`7z`), extracts by `7z` (or `unar`) and reads the start of a file by `7z`.
Passwords are asked through the same callback, once a call, and given to
the programs on their command lines (visible in the process list while they
run: `7z` reads a password it asks for only from a terminal). Existing files
are asked about before the programs extract. Their progress is coarser (7z's
percentages; listing: none), and `readEntries` gives only the file asked
for.

It writes zip (with a password: AES-256), 7z, and tar with gzip, bzip2, xz,
Zstandard, LZ4, lzip or compress, with names in UTF-8. Symbolic links are
stored as links (not followed), hidden files are included, and devices,
pipes and sockets are left out. With a volume size the archive is written as
`name.001`, `name.002`, ... (plain parts, as 7-Zip splits). A cancelled or
failed packing leaves no files behind. A 7z with a password (the file names
encrypted too) or a solid block size is written by the `7z` program, as
libarchive writes neither; RAR is not written.

### The editor plugin

`EditorPlugin::createEditor()` gives an `EditorView`: a widget with line
numbers and folding, for the program to put into its own tabs and menus
(the plugin has none). It opens a file in its encoding — detected, or given
(cpg names such as `utf8`, `cp1250`) — and saves it back the same way with
its BOM and line breaks, atomically; a file that does not exist yet opens as
an empty UTF-8 document saved there. When detection was not confident,
`uncertainEncodings()` lists the plausible encodings and `reopen()` reads
the file in the one chosen. `hasExternalChanges()` tells that the file
changed on disk; `reload()` reads it again, changing only the lines that
differ, as one undo step (Ctrl+Z brings the text before back). Trailing
spaces and tabs are removed from the lines changed since the last save, but
not from the cursor's line.

The text is coloured by the Kate syntax definition matching the file's name,
from the Kate data shared by the programs using qcodeedit
(`~/.local/share/kate-data`, downloaded by such programs), with the Kate
theme matching the desktop (Breeze Light or Dark); without the data, C and
C++ files get a small built-in highlighter. An `EditorViewListener` set on
the view is told when the text becomes modified or saved, when it changes,
when the cursor moves, when the file changes (opened, saved under another
name) and when the encoding or line breaks change. Spell checking, search and
replace and encrypted files are not part of it.

`setMarks()` underlines ranges of the text with a wavy line — errors red,
warnings amber, information blue, hints grey — and shows their messages when
the mouse rests on them, e.g. a language server's diagnostics. The marks
replace those set before, and an edit removes them until they are set again
for the new text.

With Ctrl held over a word, the editor asks the listener whether it is a
link (`linkQueried()`, e.g. whether a language server knows its
definition); the answer (`answerLink()`, at once or later) makes it look
like one — blue, with a straight blue line under it, under a pointing hand —
while Ctrl is held over it (also when Ctrl is pressed or let go while the
mouse rests). Each word is asked about once until the mouse leaves it or the
text changes; unanswered, a word is no link, so comments and the like stay
as they are. A Ctrl+click on a link puts the cursor there and tells the
listener (`linkActivated()`), e.g. for Go to Definition; on anything else it
is an ordinary click.

Encoding detection reads cpg's language models, installed next to the plugin
in `spp-editor/cpg`.

The view also selects and gives its selection, highlights ranges (e.g. a
search's matches; an edit removes them) and replaces ranges as one undo step.

For a program with a debugger, `setBreakpointMargin()` adds a column beside
the line numbers, hidden by default. A click there toggles a breakpoint on the
line: a red dot, the line pale red. The listener hears of the lines
(`breakpointsChanged()`), also when an edit moves them with their code;
`setBreakpoints()` sets them. `setExecutionLine()` shows where the debugger
stopped, with an arrow and a blue line. Removing a line removes its breakpoint,
and opening a file removes them all.

`showCompletions()` shows a list of completions under the word at the cursor,
e.g. a language server's. Typing narrows it down: those starting with what was
typed first, then those having its letters in order, without regard to case.
Up, Down, Page Up and Page Down choose; Enter, Tab or a click write the choice
as one undo step, together with any edits it brings elsewhere (an `#include`);
Escape, a click in the text or leaving the word closes it. With `incomplete`,
the listener hears as the word grows (`completionsNeeded()`), to give more.

When the mouse rests on a word, the listener is asked about it
(`hoverQueried()`); its answer in Markdown (`answerHover()`, at once or later)
shows under the mouse, after the messages of the marks there, until the mouse
moves away.

### The find bar plugin

`FindPlugin::createFindBar()` gives a bar for the program to put into its
window, under the editors; `setTarget()` tells it which `FindTarget` it
works on (the current tab's), `setEditor()` the same for an `EditorView`
(`EditorView::findTarget()`). `showFind()` and `showReplace()` show it with
the target's selection, within a line, as the text sought.

`setPlacement()` chooses where the bar is:

- `FindBarPlacement::Bottom` (the default): the program puts the bar's
  widget below the editors, as any widget; the text never moves.
- `FindBarPlacement::Top`, as in qceditor: the program puts the bar's widget
  just above the editors. The bar covers the first rows shown, beside the
  scroll bar; a match that would be under it is scrolled below it, and a
  match among the first rows of the text (which cannot scroll further)
  moves the text down, the bar's widget taking the room above it. A target
  without rows (`FindTarget::rowHeight()` 0, e.g. a huge one) is always
  moved down. The target tells its rows through `rowHeight()`,
  `coverableRect()`, `textTop()`, `firstVisibleRow()`,
  `setFirstVisibleRow()` and `rowOf()`; the editor does.

A target has two traits of its own:

- `canReplace()`: false for a read-only target (a read-only editor, a
  viewer); the bar shows no Replace row then, also for `showReplace()`.
- `isHuge()`: the text is too large to hold in memory (a file of
  gigabytes). Typing then only gives the target the query (`setQuery()`),
  and it highlights the matches in the part it shows; Enter, ↓ and ↑ ask it
  for the next or previous match (`find()`), which it seeks in the
  background, the bar showing the progress instead of a count; Escape
  cancels the search (a second one closes the bar). The bar then says
  "Wrapped", "No matches" or "Cancelled", never "3/17".

An editor is never huge and replaces unless it is read-only. For a target
that is not huge, the bar seeks in its text itself: it seeks as one types
and highlights every match, selecting the first from where the search began;
it says "3/17" ("0/17" with no match selected, "No matches", or "Invalid"
for a wrong regular expression). Options: wrap around (↩, on by default),
match case (Cc, Alt+C),
whole words (Alt+W), regular expression (Alt+X). Enter and the ↓ button go
to the next match, Shift+Enter and ↑ to the previous one, around the end
while wrapping around (otherwise the last and the first match stay
selected; a huge target gets `FindQuery::wrap`); `findNext()` and `findPrevious()`
do it for the program's keys (F3, Shift+F3). The ▸ button at the left shows
or hides the Replace row (`showReplace()` shows it and focuses the
replacement; `showFind()` leaves it as it was; a read-only target has
neither). For replacing: Replace replaces the selected match and selects the
next one (Enter in the replacement does it too), Replace All replaces them
all as one undo step, Exclude leaves the selected match out of Replace All
until the text sought, an option or the text changes. Preserve case (Aa):
the replacement follows the case of what it replaces — all capitals, all
small, or a capital first. In the replacement of a regular expression `$1`
or `\1` is its group (`$0` the whole match, `$12` the twelfth; a group that
does not exist stays as written), `${name}` a named group, `\n` a line
break, `\t` a tab, `\r` a carriage return, `\\` and `$$` themselves. Escape closes the bar
and returns to the editor. The program tells the bar of edits
(`editorTextChanged()`), after which the matches are sought again.

Search in selection (▽, not for a huge target): opened with several lines
or more than 64 characters selected, the bar seeks and replaces only within
that selection, shaded beneath the matches (`FindTarget::
setScopeHighlights()`), and does not take it as the text sought; a shorter
selection in a line is what is sought, as before. ▽ turns it off, or on by
hand for the selection then. The selection searched in follows edits before
and inside it.

## Interfaces

The interfaces are headers in `api/` (CMake target
`siplasplas::plugin-api`). Only Qt and standard types cross the boundary,
and plugins keep no state between calls — except the editor: each
`EditorView` is a widget with its document, used on the GUI thread only.
Each interface has an IID with a
version number, e.g. `io.github.siplasplas.plugins.ArchivePlugin/3`; it
changes with the interface, so a plugin built for another version is not
loaded instead of failing.

A plugin loads in a program on the same major version of Qt and the same or
a newer minor version than the plugin was built with.

## Building

```sh
cmake -S . -B build
cmake --build build
```

Plugins are built into `build/plugins` and installed to
`lib/siplasplas/qt6/plugins`. A program fetching this repository with
FetchContent may set `SPP_PLUGIN_BUILD_DIR` and `SPP_PLUGIN_INSTALL_DIR`
before, to put the plugins into its own plugin directory, and links
`siplasplas::plugin-api`; only the plugins are installed then (the headers
and CMake package with `-DSPP_INSTALL_API=ON`). `-DSPP_BUILD_ARCHIVE=OFF`
leaves the archive plugin out, `-DSPP_BUILD_EDITOR=OFF` the editor plugin,
`-DSPP_BUILD_FINDBAR=OFF` the find bar.
A program fetching the repository gets the editor plugin only when it sets
`SPP_BUILD_EDITOR` to `ON`, which it does only when it does not link
qcodeedit itself (two copies of it in one process do not go together), and
the find bar when it sets `SPP_BUILD_FINDBAR`.
The editor plugin fetches qcodeedit (and through it cpg and DiffMerge's
diff library) from GitHub and builds the parts it uses into the plugin, with
position-independent code; `-DFETCHCONTENT_SOURCE_DIR_QCODEEDIT=<path>`
uses a local qcodeedit checkout instead.

The tests (GoogleTest, libgtest-dev) are built when the repository is built
on its own (`SPP_BUILD_TESTS`); they take a fraction of a second:

```sh
ctest --test-dir build --output-on-failure
```

## License

GNU Lesser General Public License, version 3 only (LGPL-3.0-only) — see
[LICENSE](LICENSE), and [COPYING](COPYING) for the GNU General Public License,
version 3, which it builds on. Later versions of the LGPL apply only when
accepted as described in [CONTRIBUTING.md](CONTRIBUTING.md).
