// SPDX-License-Identifier: LGPL-3.0-only
// The archive plugin on libarchive.

#include "siplasplas/plugin/ArchivePlugin.h"
#include "ExternalTools.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QLocale>
#include <QObject>
#include <QRegularExpression>
#include <QStringDecoder>

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
    bool declined = false;  // asked, and given none: no other reader tries
};

const char* passphraseCallback(struct archive*, void* data)
{
    auto* state = static_cast<PasswordState*>(data);
    if (!state->ask || !*state->ask)
        return nullptr;
    const QString password = (*state->ask)(state->archivePath);
    if (password.isEmpty()) {
        state->declined = true;
        return nullptr;
    }
    state->current = password.toUtf8();
    return state->current.constData();
}

// The volume file `index` (1, 2, ...) of an archive split as name.001,
// name.002, ... (`base` is the name without the number).
QString volumeName(const QString& base, int index)
{
    return QStringLiteral("%1.%2").arg(base).arg(index, 3, 10, QLatin1Char('0'));
}

// The files of an archive in volumes, from the first one whichever is
// given: name.001, name.002, ... (split by 7-Zip or pack()); RAR's
// name.part1.rar, name.part2.rar, ... (any width of the number); and the old
// RAR naming name.rar, name.r00, name.r01, ... Else just `path`.
QStringList volumesOf(const QString& path)
{
    static const QRegularExpression numbered(QStringLiteral("^(.*)\\.(\\d{3})$"));
    static const QRegularExpression rarPart(QStringLiteral("^(.*\\.part)(\\d+)\\.rar$"),
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression oldRarPart(QStringLiteral("^(.*)\\.([rR])\\d\\d$"));
    // name.r01 given: the archive starts at name.rar.
    if (const auto m = oldRarPart.match(path); m.hasMatch()) {
        for (const char* suffix : {".rar", ".RAR"}) {
            const QString first = m.captured(1) + QLatin1String(suffix);
            if (QFileInfo::exists(first))
                return volumesOf(first);
        }
    }
    QStringList result;
    if (const auto m = numbered.match(path); m.hasMatch()) {
        const QString base = m.captured(1);
        for (int i = 1; QFileInfo::exists(volumeName(base, i)); ++i)
            result << volumeName(base, i);
    } else if (const auto m = rarPart.match(path); m.hasMatch()) {
        const int width = int(m.captured(2).size());
        const QString suffix = path.right(4);  // ".rar", as written
        for (int i = 1;; ++i) {
            const QString part = m.captured(1) + QStringLiteral("%1").arg(i, width, 10, QLatin1Char('0')) + suffix;
            if (!QFileInfo::exists(part))
                break;
            result << part;
        }
    } else if (path.endsWith(QLatin1String(".rar"), Qt::CaseInsensitive)) {
        const QString base = path.chopped(4);
        const QChar r = path.at(path.size() - 3);  // 'r' or 'R'
        for (int i = 0; i < 100; ++i) {
            const QString part = QStringLiteral("%1.%2%3").arg(base).arg(r).arg(i, 2, 10, QLatin1Char('0'));
            if (!QFileInfo::exists(part))
                break;
            if (result.isEmpty())
                result << path;
            result << part;
        }
    }
    return result.isEmpty() ? QStringList{path} : result;
}

// The size of all files of the archive, for progress.
qint64 archiveSize(const QString& path)
{
    qint64 size = 0;
    for (const QString& volume : volumesOf(path))
        size += QFileInfo(volume).size();
    return size;
}

ArchivePtr openForReading(const QString& path, PasswordState& password, QString& error)
{
    ArchivePtr a(archive_read_new(), archive_read_free);
    archive_read_support_filter_all(a.get());
    archive_read_support_format_all(a.get());
    archive_read_set_passphrase_callback(a.get(), &password, passphraseCallback);
    // Volumes are read as one stream.
    QList<QByteArray> names;
    for (const QString& volume : volumesOf(path))
        names << QFile::encodeName(volume);
    std::vector<const char*> pointers;
    for (const QByteArray& name : names)
        pointers.push_back(name.constData());
    pointers.push_back(nullptr);
    if (archive_read_open_filenames(a.get(), pointers.data(), 64 * 1024) != ARCHIVE_OK) {
        error = errorOf(a.get());
        return ArchivePtr(nullptr, archive_read_free);
    }
    return a;
}

// The DOS code page in which old zips (made on DOS or by Windows' own zip)
// name their files: the system's OEM code page, guessed from the language.
const char* dosCodePage()
{
    switch (QLocale().language()) {
        case QLocale::Polish: case QLocale::Czech: case QLocale::Slovak: case QLocale::Hungarian:
        case QLocale::Slovenian: case QLocale::Croatian: case QLocale::Romanian: case QLocale::Bosnian:
            return "IBM852";
        case QLocale::Russian: case QLocale::Ukrainian: case QLocale::Belarusian: case QLocale::Bulgarian:
            return "IBM866";
        case QLocale::German: case QLocale::French: case QLocale::Spanish: case QLocale::Italian:
        case QLocale::Portuguese: case QLocale::Dutch: case QLocale::Danish: case QLocale::Swedish:
        case QLocale::NorwegianBokmal: case QLocale::Finnish: case QLocale::Catalan:
            return "IBM850";
        default:
            return "IBM437";  // what the zip format specifies
    }
}

// Bytes of a name stored without saying its encoding: UTF-8 when they are
// valid UTF-8 (newer tools), else in the DOS code page (old zips).
QString decodeStoredName(const QByteArray& bytes)
{
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = utf8.decode(bytes);
    if (!utf8.hasError())
        return text;
    QStringDecoder dos(dosCodePage());
    return dos.isValid() ? dos.decode(bytes) : QString::fromLatin1(bytes);
}

// The entry's path, without a leading "./" and trailing '/'. A zip names its
// files in UTF-8 when it says so; else in raw bytes of an unknown encoding:
// see decodeStoredName. Other formats: as libarchive converts them.
QString entryPath(struct archive* a, struct archive_entry* entry)
{
    QString path;
    const bool zip = (archive_format(a) & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_ZIP;
    const char* raw = archive_entry_pathname(entry);
    const auto highBytes = [](const char* s) {
        for (; *s; ++s) {
            if (static_cast<unsigned char>(*s) >= 0x80)
                return true;
        }
        return false;
    };
    if (zip && raw && highBytes(raw))
        path = decodeStoredName(QByteArray(raw));
    else if (const char* utf8 = archive_entry_pathname_utf8(entry))
        path = QString::fromUtf8(utf8);
    else if (raw)
        path = QString::fromLocal8Bit(raw);
    while (path.startsWith(QLatin1String("./")))
        path.remove(0, 2);
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path == QLatin1String(".") ? QString() : path;
}

spp::ArchiveItem itemOf(struct archive* a, struct archive_entry* entry)
{
    spp::ArchiveItem item;
    item.path = entryPath(a, entry);
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

// How a format id is written by libarchive.
struct WriteFormat {
    const char* id;
    const char* format;   // archive_write_set_format_by_name
    const char* filter;   // archive_write_add_filter_by_name, null: none
    bool encrypt;
};

const WriteFormat kWriteFormats[] = {
    {"zip", "zip", nullptr, true},
    {"7z", "7zip", nullptr, false},
    // paxr (restricted pax): ustar, with pax headers only where needed
    // (long or non-ASCII names, big files).
    {"tar", "paxr", nullptr, false},
    {"tar.gz", "paxr", "gzip", false},
    {"tar.bz2", "paxr", "bzip2", false},
    {"tar.xz", "paxr", "xz", false},
    {"tar.zst", "paxr", "zstd", false},
    {"tar.lz4", "paxr", "lz4", false},
    {"tar.lz", "paxr", "lzip", false},
    {"tar.Z", "paxr", "compress", false},
};

const WriteFormat* writeFormat(const QString& id)
{
    for (const WriteFormat& format : kWriteFormats) {
        if (id == QLatin1String(format.id))
            return &format;
    }
    return nullptr;
}

// The output of a new archive: one file, or volumes name.001, name.002, ...
// of at most `volumeSize` bytes each.
class VolumeOutput {
public:
    VolumeOutput(const QString& path, qint64 volumeSize) : m_path(path), m_volumeSize(volumeSize) {}

    static la_ssize_t write(struct archive* a, void* data, const void* buffer, size_t length)
    {
        auto* self = static_cast<VolumeOutput*>(data);
        const char* bytes = static_cast<const char*>(buffer);
        qint64 left = qint64(length);
        while (left > 0) {
            if (!self->m_file.isOpen() || (self->m_volumeSize > 0 && self->m_inFile == self->m_volumeSize)) {
                if (!self->openNext()) {
                    archive_set_error(a, EIO, "%s", self->m_error.toLocal8Bit().constData());
                    return -1;
                }
            }
            const qint64 chunk = self->m_volumeSize > 0 ? qMin(left, self->m_volumeSize - self->m_inFile) : left;
            if (self->m_file.write(bytes, chunk) != chunk) {
                archive_set_error(a, EIO, "%s", self->m_file.errorString().toLocal8Bit().constData());
                return -1;
            }
            bytes += chunk;
            left -= chunk;
            self->m_inFile += chunk;
        }
        return la_ssize_t(length);
    }

    static int close(struct archive*, void* data)
    {
        auto* self = static_cast<VolumeOutput*>(data);
        self->m_file.close();
        return ARCHIVE_OK;
    }

    // The files written so far are removed (a failed or cancelled packing).
    void removeAll()
    {
        m_file.close();
        for (const QString& file : m_created)
            QFile::remove(file);
    }

private:
    bool openNext()
    {
        m_file.close();
        m_file.setFileName(m_volumeSize > 0 ? volumeName(m_path, ++m_index) : m_path);
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            m_error = QObject::tr("Cannot write '%1': %2").arg(m_file.fileName(), m_file.errorString());
            return false;
        }
        m_created << m_file.fileName();
        m_inFile = 0;
        return true;
    }

    QString m_path;
    qint64 m_volumeSize;
    int m_index = 0;
    QFile m_file;
    qint64 m_inFile = 0;
    QStringList m_created;
    QString m_error;
};

// A file to pack: on disk, and in the archive.
struct Source {
    QString absolute;
    QString relative;
};

// `relative` and, for a directory (not a link to one), everything below it,
// by name; `bytes` sums the sizes of the regular files.
void collectSources(const QString& baseDir, const QString& relative, QList<Source>& sources, qint64& bytes)
{
    const QString absolute = baseDir + QLatin1Char('/') + relative;
    const QFileInfo info(absolute);
    sources.append({absolute, relative});
    if (info.isSymLink())
        return;
    if (info.isFile()) {
        bytes += info.size();
        return;
    }
    if (!info.isDir())
        return;
    const QStringList children = QDir(absolute).entryList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& child : children)
        collectSources(baseDir, relative + QLatin1Char('/') + child, sources, bytes);
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
        QString text = QStringLiteral("libarchive %1")
                           .arg(QString::fromLatin1(archive_version_details()).section(QLatin1Char(' '), 1, 1));
        QStringList tools;
        for (const char* program : {"lsar", "7z", "unar"}) {
            if (spp::tools::installed(QString::fromLatin1(program)))
                tools << QString::fromLatin1(program);
        }
        if (!tools.isEmpty())
            text += QStringLiteral(" (and %1)").arg(tools.join(QLatin1String(", ")));
        return text;
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
        // Written: see kWriteFormats.
        return {
            format("zip", "ZIP", {"zip"}, true, true),
            // Encrypted by the 7z program, when installed.
            format("7z", "7-Zip", {"7z"}, true, spp::tools::installed(QStringLiteral("7z"))),
            format("tar", "tar", {"tar"}, true),
            format("tar.gz", "tar + gzip", {"tar.gz", "tgz"}, true),
            format("tar.bz2", "tar + bzip2", {"tar.bz2", "tbz2", "tbz"}, true),
            format("tar.xz", "tar + xz", {"tar.xz", "txz"}, true),
            format("tar.zst", "tar + Zstandard", {"tar.zst", "tzst"}, true),
            format("tar.lz4", "tar + LZ4", {"tar.lz4"}, true),
            format("tar.lz", "tar + lzip", {"tar.lz"}, true),
            format("tar.Z", "tar + compress", {"tar.Z", "taz"}, true),
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
        if (listLib(archivePath, items, error, progress, passwordState))
            return true;
        if (!useTools(error, passwordState))
            return false;
        // Another reader; what libarchive read stays when it fails too.
        QList<spp::ArchiveItem> found;
        QString toolError;
        if (spp::tools::list(archivePath, found, toolError, progress, password)) {
            items = found;
            error.clear();
            return true;
        }
        // Cancelled, or libarchive read nothing: the programs' reason says more.
        if (toolError == cancelled() || items.isEmpty())
            error = toolError.isEmpty() ? error : toolError;
        return false;
    }

    bool extract(const QString& archivePath, const QString& destDir, const QStringList& entries,
                 QString& error, const spp::ArchiveProgress& progress,
                 const spp::ArchivePassword& password,
                 const spp::ExtractConflictHandler& conflict) override
    {
        PasswordState passwordState{&password, archivePath, {}};
        QSet<QString> written;
        if (extractLib(archivePath, destDir, entries, error, progress, passwordState, conflict, written))
            return true;
        if (!useTools(error, passwordState))
            return false;
        return spp::tools::extract(archivePath, destDir, entries, error, progress, password, conflict, written);
    }

    qint64 readEntry(const QString& archivePath, const QString& entryName, QIODevice* out,
                     qint64 maxBytes, QString& error, const spp::ArchivePassword& password) override
    {
        PasswordState passwordState{&password, archivePath, {}};
        qint64 written = 0;
        bool answered = false;
        const qint64 n = readEntryLib(archivePath, entryName, out, maxBytes, error, passwordState, written,
                                      answered);
        // Only when nothing was written yet: `out` cannot be taken back.
        if (n >= 0 || answered || written > 0 || !useTools(error, passwordState))
            return n;
        return spp::tools::readEntry(archivePath, entryName, out, maxBytes, error, password);
    }

    bool readEntries(const QString& archivePath, const QString& stopAt, qint64 maxBytes,
                     const spp::EntryWanted& wanted, const spp::EntryStart& start, QString& error,
                     const spp::ArchiveProgress& progress, const spp::ArchivePassword& password) override
    {
        PasswordState passwordState{&password, archivePath, {}};
        QSet<QString> given;
        const auto startOnce = [&](const spp::ArchiveItem& item, const QByteArray& data) {
            given.insert(item.path);
            if (start)
                start(item, data);
        };
        bool answered = false;
        if (readEntriesLib(archivePath, stopAt, maxBytes, wanted, startOnce, error, progress, passwordState,
                           answered))
            return true;
        if (answered || stopAt.isEmpty() || given.contains(stopAt) || !useTools(error, passwordState))
            return false;
        // By a program: only the file stopped at.
        QBuffer buffer;
        buffer.open(QIODevice::WriteOnly);
        if (spp::tools::readEntry(archivePath, stopAt, &buffer, maxBytes, error, password) < 0)
            return false;
        spp::ArchiveItem item;
        item.path = stopAt;
        item.size = buffer.size();
        if (start)
            start(item, buffer.data());
        error.clear();
        return true;
    }

private:
    // After libarchive failed: whether the external programs may try. Not
    // when the user cancelled or gave no password.
    static bool useTools(const QString& error, const PasswordState& passwordState)
    {
        return error != cancelled() && !passwordState.declined;
    }

    bool listLib(const QString& archivePath, QList<spp::ArchiveItem>& items, QString& error,
                 const spp::ArchiveProgress& progress, PasswordState& passwordState)
    {
        ArchivePtr a = openForReading(archivePath, passwordState, error);
        if (!a)
            return false;
        // Progress by the bytes of the archive file read so far.
        const qint64 total = archiveSize(archivePath);
        struct archive_entry* entry;
        int status;
        while ((status = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK
               || status == ARCHIVE_WARN) {
            spp::ArchiveItem item = itemOf(a.get(), entry);
            if (!item.path.isEmpty())
                items.append(item);
            if (progress && !progress(archive_filter_bytes(a.get(), -1), total, item.path)) {
                error = cancelled();
                return false;
            }
            if (archive_read_data_skip(a.get()) < ARCHIVE_WARN) {
                status = ARCHIVE_FATAL;  // the data cannot be passed over
                break;
            }
        }
        if (status != ARCHIVE_EOF) {
            // Not read to the end (damaged, or a format libarchive reads only
            // in part, e.g. solid RAR 1.5): a failure, so that the caller can
            // try another reader; the entries read so far stay in `items`.
            error = errorOf(a.get());
            return false;
        }
        return true;
    }

    // `written`: the files written, for the programs trying after a failure.
    bool extractLib(const QString& archivePath, const QString& destDir, const QStringList& entries,
                    QString& error, const spp::ArchiveProgress& progress, PasswordState& passwordState,
                    const spp::ExtractConflictHandler& conflict, QSet<QString>& written)
    {
        ArchivePtr a = openForReading(archivePath, passwordState, error);
        if (!a)
            return false;
        const QString root = QDir::cleanPath(QDir(destDir).absolutePath());
        if (!QDir().mkpath(root)) {
            error = QObject::tr("Cannot create directory '%1'").arg(root);
            return false;
        }
        // Progress by the bytes of the archive file read so far.
        const qint64 total = archiveSize(archivePath);
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
            const spp::ArchiveItem item = itemOf(a.get(), entry);
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
            written.insert(target);
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

    // `written`: bytes written to `out`, also when it fails. `answered`: the
    // archive was read and the entry is not there or not a file, which no
    // other reader changes.
    qint64 readEntryLib(const QString& archivePath, const QString& entryName, QIODevice* out,
                        qint64 maxBytes, QString& error, PasswordState& passwordState, qint64& written,
                        bool& answered)
    {
        written = 0;
        answered = false;
        ArchivePtr a = openForReading(archivePath, passwordState, error);
        if (!a)
            return -1;
        struct archive_entry* entry;
        int status;
        while ((status = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK
               || status == ARCHIVE_WARN) {
            if (entryPath(a.get(), entry) != entryName) {
                archive_read_data_skip(a.get());
                continue;
            }
            if (archive_entry_filetype(entry) != AE_IFREG) {
                error = QObject::tr("'%1' is not a file").arg(entryName);
                answered = true;
                return -1;
            }
            // Stops after maxBytes: the rest of the entry is not decompressed.
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
        answered = status == ARCHIVE_EOF;
        error = answered ? QObject::tr("'%1' is not in the archive").arg(entryName) : errorOf(a.get());
        return -1;
    }

    // `answered`: as for readEntryLib.
    bool readEntriesLib(const QString& archivePath, const QString& stopAt, qint64 maxBytes,
                        const spp::EntryWanted& wanted, const spp::EntryStart& start, QString& error,
                        const spp::ArchiveProgress& progress, PasswordState& passwordState, bool& answered)
    {
        answered = false;
        ArchivePtr a = openForReading(archivePath, passwordState, error);
        if (!a)
            return false;
        const qint64 total = archiveSize(archivePath);
        struct archive_entry* entry;
        int status;
        char buffer[64 * 1024];
        while ((status = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK
               || status == ARCHIVE_WARN) {
            const spp::ArchiveItem item = itemOf(a.get(), entry);
            if (progress && !progress(archive_filter_bytes(a.get(), -1), total, item.path)) {
                error = cancelled();
                return false;
            }
            const bool last = !stopAt.isEmpty() && item.path == stopAt;
            if (archive_entry_filetype(entry) == AE_IFREG && (last || !wanted || wanted(item))) {
                // The start of the file; the rest is skipped (in a solid
                // archive decompressed all the same, to reach what follows).
                QByteArray data;
                while (maxBytes < 0 || data.size() < maxBytes) {
                    const qint64 want = maxBytes < 0 ? qint64(sizeof buffer)
                                                     : qMin<qint64>(sizeof buffer, maxBytes - data.size());
                    const la_ssize_t n = archive_read_data(a.get(), buffer, size_t(want));
                    if (n < 0) {
                        error = errorOf(a.get());
                        return false;
                    }
                    if (n == 0)
                        break;
                    data.append(buffer, n);
                    if (progress && !progress(archive_filter_bytes(a.get(), -1), total, item.path)) {
                        error = cancelled();
                        return false;
                    }
                }
                if (start)
                    start(item, data);
            } else if (last) {
                error = QObject::tr("'%1' is not a file").arg(stopAt);
                answered = true;
                return false;
            }
            if (last)
                return true;
        }
        if (status != ARCHIVE_EOF) {
            error = errorOf(a.get());
            return false;
        }
        if (!stopAt.isEmpty()) {
            error = QObject::tr("'%1' is not in the archive").arg(stopAt);
            answered = true;
            return false;
        }
        return true;
    }

public:
    bool pack(const QString& archivePath, const QString& baseDir, const QStringList& files,
              const spp::PackOptions& options, QString& error,
              const spp::ArchiveProgress& progress) override
    {
        // libarchive writes neither an encrypted 7z nor solid blocks of a size.
        if (options.format == QLatin1String("7z") && (!options.password.isEmpty() || options.solidBlockSize > 0))
            return spp::tools::pack7z(archivePath, baseDir, files, options, error, progress);
        const WriteFormat* format = writeFormat(options.format);
        if (!format) {
            error = QObject::tr("Cannot write the format '%1'").arg(options.format);
            return false;
        }
        if (!options.password.isEmpty() && !format->encrypt) {
            error = QObject::tr("The format '%1' cannot be encrypted").arg(options.format);
            return false;
        }
        const QString base = QDir(baseDir).absolutePath();
        QList<Source> sources;
        qint64 total = 0;
        for (const QString& file : files) {
            QString relative = QDir::cleanPath(file);
            if (!isSafePath(relative) || !targetExists(base + QLatin1Char('/') + relative)) {
                error = QObject::tr("'%1' is not in '%2'").arg(file, base);
                return false;
            }
            collectSources(base, relative, sources, total);
        }

        ArchivePtr a(archive_write_new(), archive_write_free);
        if (archive_write_set_format_by_name(a.get(), format->format) != ARCHIVE_OK
            || (format->filter && archive_write_add_filter_by_name(a.get(), format->filter) < ARCHIVE_WARN)) {
            error = errorOf(a.get());
            return false;
        }
        // No padding of the output to 10 KiB blocks (for tapes): zstd and
        // others reject zeros after the compressed stream.
        archive_write_set_bytes_in_last_block(a.get(), 1);
        // Names in UTF-8; a module without the option ignores it.
        archive_write_set_options(a.get(), "hdrcharset=UTF-8");
        if (options.level >= 0)
            archive_write_set_options(a.get(), QByteArray("compression-level=")
                                                   .append(QByteArray::number(options.level)).constData());
        if (!options.password.isEmpty()) {
            archive_write_set_options(a.get(), "zip:encryption=aes256");
            archive_write_set_passphrase(a.get(), options.password.toUtf8().constData());
        }

        VolumeOutput output(archivePath, options.volumeSize);
        const auto fail = [&](const QString& message) {
            error = message;
            a.reset();  // closes the output
            output.removeAll();
            return false;
        };
        if (archive_write_open2(a.get(), &output, nullptr, VolumeOutput::write, VolumeOutput::close, nullptr)
            != ARCHIVE_OK)
            return fail(errorOf(a.get()));

        ArchivePtr disk(archive_read_disk_new(), archive_read_free);
        archive_read_disk_set_standard_lookup(disk.get());
        archive_read_disk_set_symlink_physical(disk.get());  // links are stored, not followed

        qint64 done = 0;
        char buffer[64 * 1024];
        for (const Source& source : sources) {
            if (progress && !progress(done, total, source.relative))
                return fail(cancelled());
            std::unique_ptr<struct archive_entry, void (*)(struct archive_entry*)> entry(
                archive_entry_new(), archive_entry_free);
            archive_entry_copy_sourcepath(entry.get(), QFile::encodeName(source.absolute).constData());
            if (archive_read_disk_entry_from_file(disk.get(), entry.get(), -1, nullptr) < ARCHIVE_WARN)
                return fail(errorOf(disk.get()));
            const auto type = archive_entry_filetype(entry.get());
            if (type != AE_IFREG && type != AE_IFDIR && type != AE_IFLNK)
                continue;  // devices, pipes, sockets
            archive_entry_set_pathname_utf8(entry.get(), source.relative.toUtf8().constData());
            if (archive_write_header(a.get(), entry.get()) < ARCHIVE_WARN)
                return fail(errorOf(a.get()));
            if (type != AE_IFREG || archive_entry_size(entry.get()) == 0)
                continue;
            QFile file(source.absolute);
            if (!file.open(QIODevice::ReadOnly))
                return fail(QObject::tr("Cannot read '%1': %2").arg(source.absolute, file.errorString()));
            for (;;) {
                const qint64 n = file.read(buffer, sizeof buffer);
                if (n < 0)
                    return fail(QObject::tr("Cannot read '%1': %2").arg(source.absolute, file.errorString()));
                if (n == 0)
                    break;
                if (archive_write_data(a.get(), buffer, size_t(n)) < 0)
                    return fail(errorOf(a.get()));
                done += n;
                if (progress && !progress(done, total, source.relative))
                    return fail(cancelled());
            }
        }
        if (archive_write_close(a.get()) != ARCHIVE_OK)
            return fail(errorOf(a.get()));
        return true;
    }
};

#include "LibarchivePlugin.moc"
