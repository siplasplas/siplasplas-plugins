# siplasplas-plugins

Shared Qt 6 plugins and their interfaces for
[gemini-commander](https://github.com/siplasplas/gemini-commander) and
[ducat](https://github.com/siplasplas/ducat). A plugin keeps its dependencies
(e.g. libarchive) out of the programs, which work without it.

## Plugins

| Plugin | Interface | Needs | Does |
|--------|-----------|-------|------|
| `spp-archive` | `spp::ArchivePlugin` (`siplasplas/plugin/ArchivePlugin.h`) | libarchive (libarchive-dev) | lists archives (with progress, cancelling and passwords) and reads the start of an entry without decompressing the rest, for previews; extracting and packing follow |

The archive plugin reads zip (also with a password), 7z, tar with gzip,
bzip2, xz, Zstandard, LZ4, lzip or compress, RAR without encryption, CAB,
LHA/LZH, ISO 9660, XAR, cpio and ar. Formats libarchive does not read — ARJ,
ACE, encrypted RAR and 7z — are left to external programs (`unar`, `unrar`,
`7z`) by the program using the plugin.

## Interfaces

The interfaces are headers in `api/` (CMake target
`siplasplas::plugin-api`). Only Qt and standard types cross the boundary,
and plugins keep no state between calls. Each interface has an IID with a
version number, e.g. `io.github.siplasplas.plugins.ArchivePlugin/1`; it
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
`siplasplas::plugin-api`. `-DSPP_BUILD_ARCHIVE=OFF` leaves the archive plugin
out.

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
