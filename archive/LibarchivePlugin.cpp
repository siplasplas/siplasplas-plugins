// SPDX-License-Identifier: LGPL-3.0-only
// The archive plugin on libarchive.

#include "siplasplas/plugin/ArchivePlugin.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QObject>

#include <archive.h>
#include <archive_entry.h>

#include <memory>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#endif

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

QString cancelled()
{
    return QObject::tr("Cancelled");
}

// Whether `path` (from entryPath) is `selected` or below it; all when empty.
bool isSelected(const QString& path, const QStringList& selected)
{
    if (selected.isEmpty())
        return true;
    for (const QString& s : selected) {
        if (path == s || (path.startsWith(s) && path.at(s.size()) == QLatin1Char('/')))
            return true;
    }
    return false;
}

// An entry path that stays below the destination: relative, no "..".
bool isSafePath(const QString& path)
{
    if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || path.contains(QLatin1Char('\\')))
        return false;
    const QStringList parts = path.split(QLatin1Char('/'));
    return !parts.contains(QStringLiteral(".."));
}

// No directory between `destDir` and `target` is a symbolic link: an
// archive must not write outside the destination through a link it made.
bool parentsAreRealDirs(const QString& destDir, const QString& path)
{
    QString current = destDir;
    const QStringList parts = path.split(QLatin1Char('/'));
    for (int i = 0; i + 1 < parts.size(); ++i) {
        current += QLatin1Char('/') + parts[i];
        const QFileInfo info(current);
        if (info.isSymLink() || (info.exists() && !info.isDir()))
            return false;
    }
    return true;
}

QFileDevice::Permissions permissionsOf(quint32 mode)
{
    QFileDevice::Permissions p;
    if (mode & 0400) p |= QFileDevice::ReadOwner | QFileDevice::ReadUser;
    if (mode & 0200) p |= QFileDevice::WriteOwner | QFileDevice::WriteUser;
    if (mode & 0100) p |= QFileDevice::ExeOwner | QFileDevice::ExeUser;
    if (mode & 0040) p |= QFileDevice::ReadGroup;
    if (mode & 0020) p |= QFileDevice::WriteGroup;
    if (mode & 0010) p |= QFileDevice::ExeGroup;
    if (mode & 0004) p |= QFileDevice::ReadOther;
    if (mode & 0002) p |= QFileDevice::WriteOther;
    if (mode & 0001) p |= QFileDevice::ExeOther;
    return p;
}

// QFile cannot open a directory to set its time.
void setDirectoryTime(const QString& path, const QDateTime& time)
{
#ifdef Q_OS_UNIX
    struct timespec times[2];
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT;  // access time unchanged
    times[1].tv_sec = time_t(time.toSecsSinceEpoch());
    times[1].tv_nsec = 0;
    ::utimensat(AT_FDCWD, QFile::encodeName(path).constData(), times, 0);
#else
    Q_UNUSED(path);
    Q_UNUSED(time);
#endif
}

// A path taken, also by a broken symbolic link.
bool targetExists(const QString& path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
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
                error = cancelled();
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

    bool extract(const QString& archivePath, const QString& destDir, const QStringList& entries,
                 QString& error, const spp::ArchiveProgress& progress,
                 const spp::ArchivePassword& password,
                 const spp::ExtractConflictHandler& conflict) override
    {
        PasswordState passwordState{&password, archivePath, {}};
        ArchivePtr a = openForReading(archivePath, passwordState, error);
        if (!a)
            return false;
        const QString root = QDir::cleanPath(QDir(destDir).absolutePath());
        if (!QDir().mkpath(root)) {
            error = QObject::tr("Cannot create directory '%1'").arg(root);
            return false;
        }
        // Progress by the bytes of the archive file read so far.
        const qint64 total = QFileInfo(archivePath).size();
        const auto report = [&](const QString& name) {
            return !progress || progress(archive_filter_bytes(a.get(), -1), total, name);
        };
        // Directory times and permissions are set at the end: writing into
        // a directory changes its time, and a read-only one would refuse it.
        QList<std::pair<QString, spp::ArchiveItem>> dirs;

        struct archive_entry* entry;
        int status;
        while ((status = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK
               || status == ARCHIVE_WARN) {
            const spp::ArchiveItem item = itemOf(entry);
            if (!isSelected(item.path, entries) || !isSafePath(item.path)
                || !parentsAreRealDirs(root, item.path)) {
                archive_read_data_skip(a.get());
                continue;
            }
            if (!report(item.path)) {
                error = cancelled();
                return false;
            }
            const QString target = root + QLatin1Char('/') + item.path;

            if (item.isDirectory) {
                const QFileInfo info(target);
                if (info.isSymLink() || (info.exists() && !info.isDir()) || !QDir().mkpath(target)) {
                    error = QObject::tr("Cannot create directory '%1'").arg(target);
                    return false;
                }
                dirs.append({target, item});
                continue;
            }

            QDir().mkpath(QFileInfo(target).absolutePath());
            if (targetExists(target)) {
                const spp::ExtractConflict choice = conflict ? conflict(item, target)
                                                             : spp::ExtractConflict::Overwrite;
                if (choice == spp::ExtractConflict::Cancel) {
                    error = cancelled();
                    return false;
                }
                if (choice == spp::ExtractConflict::Skip) {
                    archive_read_data_skip(a.get());
                    continue;
                }
                if (QFileInfo(target).isDir() && !QFileInfo(target).isSymLink()) {
                    error = QObject::tr("Cannot overwrite directory '%1' with a file").arg(target);
                    return false;
                }
                QFile::remove(target);
            }

            if (item.isSymlink) {
                if (!QFile::link(item.linkTarget, target)) {
                    error = QObject::tr("Cannot create symbolic link '%1'").arg(target);
                    return false;
                }
                continue;
            }

            // A hard link to an entry extracted before: a copy of it.
            if (const char* hard = archive_entry_hardlink(entry)) {
                const QString source = root + QLatin1Char('/') + QString::fromUtf8(hard);
                if (isSafePath(QString::fromUtf8(hard)) && QFile::exists(source))
                    QFile::copy(source, target);
                continue;
            }

            QFile file(target);
            if (!file.open(QIODevice::WriteOnly)) {
                error = QObject::tr("Cannot write '%1': %2").arg(target, file.errorString());
                return false;
            }
            char buffer[64 * 1024];
            for (;;) {
                const la_ssize_t n = archive_read_data(a.get(), buffer, sizeof buffer);
                if (n == 0)
                    break;
                if (n < 0 || file.write(buffer, n) != n) {
                    error = n < 0 ? errorOf(a.get()) : file.errorString();
                    file.remove();
                    return false;
                }
                if (!report(item.path)) {
                    file.remove();
                    error = cancelled();
                    return false;
                }
            }
            file.flush();
            if (item.modTime.isValid())
                file.setFileTime(item.modTime, QFileDevice::FileModificationTime);
            file.close();
            if (item.permissions)
                file.setPermissions(permissionsOf(item.permissions));
        }
        if (status != ARCHIVE_EOF) {
            error = errorOf(a.get());
            return false;
        }
        // Children before their parents (archives list parents first).
        for (auto it = dirs.crbegin(); it != dirs.crend(); ++it) {
            if (it->second.modTime.isValid())
                setDirectoryTime(it->first, it->second.modTime);
            if (it->second.permissions)
                QFile::setPermissions(it->first, permissionsOf(it->second.permissions));
        }
        return true;
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
