// SPDX-License-Identifier: LGPL-3.0-only
#ifndef SIPLASPLAS_PLUGIN_ARCHIVEPLUGIN_H
#define SIPLASPLAS_PLUGIN_ARCHIVEPLUGIN_H

// Interface of the archive plugin: listing, extracting, reading the start of
// an entry and packing archives, so that programs (gemini-commander, ducat)
// do not depend on the archive library (libarchive).
//
// Only Qt and standard types cross the boundary. The plugin keeps no state
// between calls: they may run on several threads at once. An archive in
// volumes is read from its first volume whichever is given: name.001,
// name.002, ...; RAR's name.part1.rar, name.part2.rar, ... (any width of the
// number); the old RAR naming name.rar, name.r00, name.r01, ... Progress
// totals count all of them.
//
// What libarchive cannot read, or reads only in part (ARJ, ACE, encrypted
// RAR and 7z, solid RAR 1.5), the plugin reads by external programs when
// they are installed: lsar (unar) and 7z; the calls stay the same, passwords
// asked through ArchivePassword as well, once a call (given to the programs
// on their command lines: 7z reads one it asks for only from a terminal).
// Their progress is coarser (7z's percentages; listing: none), and
// readEntries() gives only the file stopped at.

#include <QByteArray>
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
    // Inside the archive, '/' separated, no leading "./" nor trailing '/'.
    // A zip name stored without its encoding is read as UTF-8 when valid,
    // else in the DOS code page of the system's language (CP852 for Polish).
    QString path;
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

// readEntries(): whether the data of a file is wanted, and the start of it.
using EntryWanted = std::function<bool(const ArchiveItem& item)>;
using EntryStart = std::function<void(const ArchiveItem& item, const QByteArray& start)>;

struct PackOptions {
    QString format;           // ArchiveFormat::id
    int level = -1;           // compression level, -1 the format's default
    QString password;         // empty: none (formats with canEncrypt only)
    qint64 volumeSize = 0;    // > 0: split into volumes .001, .002, ... of this size
    // 7z: > 0 limits solid blocks to this many bytes; 0 makes one block of
    // the whole archive. With a password or a block size 7z is written by
    // the 7z program (libarchive writes neither), the file names encrypted
    // too.
    qint64 solidBlockSize = 0;
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
    // encryption, damaged, or read only in part as solid RAR 1.5); the
    // entries read before stay in `items` then.
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

    // Reads the archive in its order up to and including the file `stopAt`
    // (to the end when empty), giving `start` the first `maxBytes` of every
    // file `wanted` accepts (all files when it is empty). For previews of
    // solid archives (tar.gz, solid 7z), where everything before an entry
    // is decompressed anyway: the files passed on the way come at no extra
    // cost. False with `error` set when `stopAt` is not there, the archive
    // cannot be read, or `progress` cancelled ("Cancelled").
    virtual bool readEntries(const QString& archivePath, const QString& stopAt, qint64 maxBytes,
                             const EntryWanted& wanted, const EntryStart& start, QString& error,
                             const ArchiveProgress& progress = {}, const ArchivePassword& password = {}) = 0;

    // Packs `files` (paths relative to `baseDir`; directories with their
    // contents) into a new archive at `archivePath`, replacing a file there.
    // With PackOptions::volumeSize the files are archivePath.001, .002, ...
    // Progress counts the bytes of the files packed. A cancelled or failed
    // call leaves no files behind.
    virtual bool pack(const QString& archivePath, const QString& baseDir, const QStringList& files,
                      const PackOptions& options, QString& error,
                      const ArchiveProgress& progress = {}) = 0;
};

} // namespace spp

// The number changes with the interface: plugins built for another one are
// not loaded.
#define SPP_ARCHIVE_PLUGIN_IID "io.github.siplasplas.plugins.ArchivePlugin/3"
Q_DECLARE_INTERFACE(spp::ArchivePlugin, SPP_ARCHIVE_PLUGIN_IID)

#endif // SIPLASPLAS_PLUGIN_ARCHIVEPLUGIN_H
