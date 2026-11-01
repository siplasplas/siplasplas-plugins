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
| `spp-editor` | `spp::EditorPlugin`, `spp::EditorView` (`siplasplas/plugin/EditorPlugin.h`) | Qt Widgets; [qcodeedit](https://github.com/siplasplas/qcodeedit) 1.69 with [cpg](https://github.com/siplasplas/cpg), fetched and built in (ICU, libicu-dev) | a code editor widget for the program's own window and tabs: opens and saves files in their encoding, colours them with Kate syntax definitions and the Kate theme matching the desktop, reloads files changed on disk as one undo step; see below |

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
the view is told when the text becomes modified or saved, when the cursor
moves, when the file changes (opened, saved under another name) and when the
encoding or line breaks change. Spell checking, search and replace and
encrypted files are not part of it.

Encoding detection reads cpg's language models, installed next to the plugin
in `spp-editor/cpg`.

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
leaves the archive plugin out, `-DSPP_BUILD_EDITOR=OFF` the editor plugin.
A program fetching the repository gets the editor plugin only when it sets
`SPP_BUILD_EDITOR` to `ON`, which it does only when it does not link
qcodeedit itself (two copies of it in one process do not go together).
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
