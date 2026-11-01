// SPDX-License-Identifier: LGPL-3.0-only
#ifndef SIPLASPLAS_PLUGIN_ARCHIVEPLUGIN_H
#define SIPLASPLAS_PLUGIN_ARCHIVEPLUGIN_H

// Interface of the archive plugin: listing, extracting, reading the start of
// an entry and packing archives, so that programs (gemini-commander, ducat)
// do not depend on the archive library (libarchive).
//
// Only Qt and standard types cross the boundary. The plugin keeps no state
// between calls: they may run on several threads at once.

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtPlugin>

#include <functional>

class QIODevice;

namespace spp {

// An entry of an archive.
struct ArchiveItem {
    QString path;             // inside the archive, '/' separated, no leading "./" nor trailing '/'
    bool isDirectory = false;
    bool isSymlink = false;
    QString linkTarget;       // for a symbolic link
    qint64 size = 0;          // uncompressed; 0 for directories
    QDateTime modTime;        // invalid when the archive has none
    quint32 permissions = 0;  // Unix mode bits (0777 and the like), 0 when unknown
    bool encrypted = false;   // its data needs a password
};

// A format the plugin knows.
struct ArchiveFormat {
    QString id;               // e.g. "zip", "7z", "tar.gz"
    QString name;             // for the user, e.g. "ZIP"
    QStringList suffixes;     // e.g. {"tar.gz", "tgz"}, without a dot
    bool canRead = false;
    bool canWrite = false;
    bool canEncrypt = false;  // writes with a password
};

// Progress of a long call: bytes done of `total` (-1 when not known, e.g.
// listing a compressed stream) and the entry being worked on. False cancels
// the call, which then fails with the error "Cancelled".
using ArchiveProgress = std::function<bool(qint64 done, qint64 total, const QString& entry)>;

// Asked for the password of an encrypted archive or entry; an empty string
// cancels. Called again after a wrong password.
using ArchivePassword = std::function<QString(const QString& archivePath)>;

// What to do with an entry whose target exists already when extracting.
enum class ExtractConflict { Overwrite, Skip, Cancel };
using ExtractConflictHandler = std::function<ExtractConflict(const ArchiveItem& item, const QString& target)>;

struct PackOptions {
    QString format;           // ArchiveFormat::id
    int level = -1;           // compression level, -1 the format's default
    QString password;         // empty: none (formats with canEncrypt only)
    qint64 volumeSize = 0;    // > 0: split into volumes .001, .002, ... of this size
};

class ArchivePlugin {
public:
    virtual ~ArchivePlugin() = default;

    // For diagnostics, e.g. "libarchive 3.8.5".
    virtual QString name() const = 0;

    // The formats the plugin reads and writes.
    virtual QList<ArchiveFormat> formats() const = 0;

    // All entries of the archive, in the archive's order. False with `error`
    // set when it cannot be read (not an archive, unsupported format or
    // encryption, damaged); a damaged end still gives the entries read before.
    virtual bool list(const QString& archivePath, QList<ArchiveItem>& items, QString& error,
                      const ArchiveProgress& progress = {}, const ArchivePassword& password = {}) = 0;

    // Extracts `entries` (paths as list() gives them; a directory with its
    // contents; empty: all) into `destDir`, with their paths below it.
    // Entries whose paths leave `destDir` ("..", absolute, below a symbolic
    // link) are not written. Without `conflict` existing files are
    // overwritten; Cancel stops with the error "Cancelled".
    virtual bool extract(const QString& archivePath, const QString& destDir, const QStringList& entries,
                         QString& error, const ArchiveProgress& progress = {},
                         const ArchivePassword& password = {},
                         const ExtractConflictHandler& conflict = {}) = 0;

    // Writes the first `maxBytes` bytes (all when -1) of the file `entry` to
    // `out`, decompressing no more than needed: a preview of a huge entry
    // reads only its start. Returns the bytes written, -1 on error.
    virtual qint64 readEntry(const QString& archivePath, const QString& entry, QIODevice* out,
                             qint64 maxBytes, QString& error, const ArchivePassword& password = {}) = 0;

    // Packs `files` (paths relative to `baseDir`; directories with their
    // contents) into a new archive at `archivePath`.
    virtual bool pack(const QString& archivePath, const QString& baseDir, const QStringList& files,
                      const PackOptions& options, QString& error,
                      const ArchiveProgress& progress = {}) = 0;
};

} // namespace spp

// The number changes with the interface: plugins built for another one are
// not loaded.
#define SPP_ARCHIVE_PLUGIN_IID "io.github.siplasplas.plugins.ArchivePlugin/1"
Q_DECLARE_INTERFACE(spp::ArchivePlugin, SPP_ARCHIVE_PLUGIN_IID)

#endif // SIPLASPLAS_PLUGIN_ARCHIVEPLUGIN_H
