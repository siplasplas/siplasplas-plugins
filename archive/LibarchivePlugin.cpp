// SPDX-License-Identifier: LGPL-3.0-only
// The archive plugin on libarchive.

#include "siplasplas/plugin/ArchivePlugin.h"

#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QObject>

#include <archive.h>
#include <archive_entry.h>

#include <memory>

namespace {

using ArchivePtr = std::unique_ptr<struct archive, int (*)(struct archive*)>;

QString errorOf(struct archive* a)
{
    const char* text = archive_error_string(a);
    return text ? QString::fromLocal8Bit(text) : QObject::tr("Unknown archive error");
}

// The password callback for libarchive; asked again after a wrong one.
struct PasswordState {
    const spp::ArchivePassword* ask = nullptr;
    QString archivePath;
    QByteArray current;  // kept alive while libarchive uses it
};

const char* passphraseCallback(struct archive*, void* data)
{
    auto* state = static_cast<PasswordState*>(data);
    if (!state->ask || !*state->ask)
        return nullptr;
    const QString password = (*state->ask)(state->archivePath);
    if (password.isEmpty())
        return nullptr;
    state->current = password.toUtf8();
    return state->current.constData();
}

ArchivePtr openForReading(const QString& path, PasswordState& password, QString& error)
{
    ArchivePtr a(archive_read_new(), archive_read_free);
    archive_read_support_filter_all(a.get());
    archive_read_support_format_all(a.get());
    archive_read_set_passphrase_callback(a.get(), &password, passphraseCallback);
    if (archive_read_open_filename(a.get(), QFile::encodeName(path).constData(), 64 * 1024) != ARCHIVE_OK) {
        error = errorOf(a.get());
        return ArchivePtr(nullptr, archive_read_free);
    }
    return a;
}

// The entry's path: UTF-8 when the archive says so, else in the locale's
// encoding (old zips name their files in a DOS code page, unknown here);
// without a leading "./" and trailing '/'.
QString entryPath(struct archive_entry* entry)
{
    QString path;
    if (const char* utf8 = archive_entry_pathname_utf8(entry))
        path = QString::fromUtf8(utf8);
    else if (const char* raw = archive_entry_pathname(entry))
        path = QString::fromLocal8Bit(raw);
    while (path.startsWith(QLatin1String("./")))
        path.remove(0, 2);
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path == QLatin1String(".") ? QString() : path;
}

spp::ArchiveItem itemOf(struct archive_entry* entry)
{
    spp::ArchiveItem item;
    item.path = entryPath(entry);
    const auto type = archive_entry_filetype(entry);
    item.isDirectory = type == AE_IFDIR;
    item.isSymlink = type == AE_IFLNK;
    if (item.isSymlink) {
        if (const char* utf8 = archive_entry_symlink_utf8(entry))
            item.linkTarget = QString::fromUtf8(utf8);
        else if (const char* raw = archive_entry_symlink(entry))
            item.linkTarget = QString::fromLocal8Bit(raw);
    }
    item.size = item.isDirectory ? 0 : archive_entry_size(entry);
    if (archive_entry_mtime_is_set(entry))
        item.modTime = QDateTime::fromSecsSinceEpoch(archive_entry_mtime(entry));
    item.permissions = archive_entry_perm(entry);
    item.encrypted = archive_entry_is_data_encrypted(entry) == 1;
    return item;
}

QString notImplemented()
{
    return QObject::tr("Not implemented yet");
}

} // namespace

class LibarchivePlugin : public QObject, public spp::ArchivePlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID SPP_ARCHIVE_PLUGIN_IID)
    Q_INTERFACES(spp::ArchivePlugin)

public:
    QString name() const override
    {
        return QStringLiteral("libarchive %1")
            .arg(QString::fromLatin1(archive_version_details()).section(QLatin1Char(' '), 1, 1));
    }

    QList<spp::ArchiveFormat> formats() const override
    {
        const auto format = [](const char* id, const char* name, QStringList suffixes, bool write,
                               bool encrypt = false) {
            spp::ArchiveFormat f;
            f.id = QString::fromLatin1(id);
            f.name = QString::fromLatin1(name);
            f.suffixes = std::move(suffixes);
            f.canRead = true;
            f.canWrite = write;
            f.canEncrypt = encrypt;
            return f;
        };
        // Writing comes with the pack() of a later version.
        return {
            format("zip", "ZIP", {"zip"}, false),
            format("7z", "7-Zip", {"7z"}, false),
            format("tar", "tar", {"tar"}, false),
            format("tar.gz", "tar + gzip", {"tar.gz", "tgz"}, false),
            format("tar.bz2", "tar + bzip2", {"tar.bz2", "tbz2", "tbz"}, false),
            format("tar.xz", "tar + xz", {"tar.xz", "txz"}, false),
            format("tar.zst", "tar + Zstandard", {"tar.zst", "tzst"}, false),
            format("tar.lz4", "tar + LZ4", {"tar.lz4"}, false),
            format("tar.lz", "tar + lzip", {"tar.lz"}, false),
            format("tar.Z", "tar + compress", {"tar.Z", "taz"}, false),
            format("rar", "RAR (without encryption)", {"rar"}, false),
            format("cab", "Microsoft Cabinet", {"cab"}, false),
            format("lha", "LHA/LZH", {"lha", "lzh"}, false),
            format("iso", "ISO 9660", {"iso"}, false),
            format("xar", "XAR", {"xar", "pkg"}, false),
            format("cpio", "cpio", {"cpio"}, false),
            format("ar", "ar", {"ar", "a", "deb"}, false),
        };
    }

    bool list(const QString& archivePath, QList<spp::ArchiveItem>& items, QString& error,
              const spp::ArchiveProgress& progress, const spp::ArchivePassword& password) override
    {
        PasswordState passwordState{&password, archivePath, {}};
        ArchivePtr a = openForReading(archivePath, passwordState, error);
        if (!a)
            return false;
        // Progress by the bytes of the archive file read so far.
        const qint64 total = QFileInfo(archivePath).size();
        struct archive_entry* entry;
        int status;
        while ((status = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK
               || status == ARCHIVE_WARN) {
            spp::ArchiveItem item = itemOf(entry);
            if (!item.path.isEmpty())
                items.append(item);
            if (progress && !progress(archive_filter_bytes(a.get(), -1), total, item.path)) {
                error = QObject::tr("Cancelled");
                return false;
            }
            archive_read_data_skip(a.get());
        }
        if (status != ARCHIVE_EOF) {
            error = errorOf(a.get());
            return !items.isEmpty();
        }
        return true;
    }

    bool extract(const QString&, const QString&, const QStringList&, QString& error,
                 const spp::ArchiveProgress&, const spp::ArchivePassword&,
                 const spp::ExtractConflictHandler&) override
    {
        error = notImplemented();
        return false;
    }

    qint64 readEntry(const QString& archivePath, const QString& entryName, QIODevice* out,
                     qint64 maxBytes, QString& error, const spp::ArchivePassword& password) override
    {
        PasswordState passwordState{&password, archivePath, {}};
        ArchivePtr a = openForReading(archivePath, passwordState, error);
        if (!a)
            return -1;
        struct archive_entry* entry;
        int status;
        while ((status = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK
               || status == ARCHIVE_WARN) {
            if (entryPath(entry) != entryName) {
                archive_read_data_skip(a.get());
                continue;
            }
            if (archive_entry_filetype(entry) != AE_IFREG) {
                error = QObject::tr("'%1' is not a file").arg(entryName);
                return -1;
            }
            // Stops after maxBytes: the rest of the entry is not decompressed.
            qint64 written = 0;
            char buffer[64 * 1024];
            while (maxBytes < 0 || written < maxBytes) {
                const qint64 want = maxBytes < 0 ? qint64(sizeof buffer)
                                                 : qMin<qint64>(sizeof buffer, maxBytes - written);
                const la_ssize_t n = archive_read_data(a.get(), buffer, size_t(want));
                if (n < 0) {
                    error = errorOf(a.get());
                    return -1;
                }
                if (n == 0)
                    break;
                if (out->write(buffer, n) != n) {
                    error = out->errorString();
                    return -1;
                }
                written += n;
            }
            return written;
        }
        error = status == ARCHIVE_EOF ? QObject::tr("'%1' is not in the archive").arg(entryName)
                                      : errorOf(a.get());
        return -1;
    }

    bool pack(const QString&, const QString&, const QStringList&, const spp::PackOptions&,
              QString& error, const spp::ArchiveProgress&) override
    {
        error = notImplemented();
        return false;
    }
};

#include "LibarchivePlugin.moc"
