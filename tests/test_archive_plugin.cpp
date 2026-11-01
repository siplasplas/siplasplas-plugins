// SPDX-License-Identifier: LGPL-3.0-only
// The archive plugin, loaded as programs load it: listing, reading the start
// of an entry, cancelling and passwords, on small archives written here.

#include "siplasplas/plugin/ArchivePlugin.h"

#include <gtest/gtest.h>

#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QPluginLoader>
#include <QTemporaryDir>

#include <archive.h>
#include <archive_entry.h>

namespace {

spp::ArchivePlugin* plugin()
{
    static QPluginLoader loader(QStringLiteral(SPP_ARCHIVE_PLUGIN_FILE));
    return qobject_cast<spp::ArchivePlugin*>(loader.instance());
}

struct Entry {
    QString path;
    QByteArray data;            // for a file
    bool isDirectory = false;
    QString linkTarget;         // for a symbolic link
    quint32 perm = 0;           // 0: 0755 for directories, 0644 for files
};

// Writes an archive with libarchive: `format` e.g. "pax" or "zip", `filter`
// e.g. "gzip" or "none".
bool writeArchive(const QString& path, const char* format, const char* filter,
                  const QList<Entry>& entries, const char* password = nullptr)
{
    struct archive* a = archive_write_new();
    archive_write_set_format_by_name(a, format);
    if (qstrcmp(filter, "none") != 0)
        archive_write_add_filter_by_name(a, filter);
    if (password) {
        archive_write_set_options(a, "zip:encryption=aes256");
        archive_write_set_passphrase(a, password);
    }
    if (archive_write_open_filename(a, QFile::encodeName(path).constData()) != ARCHIVE_OK) {
        ADD_FAILURE() << archive_error_string(a);
        archive_write_free(a);
        return false;
    }
    for (const Entry& e : entries) {
        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname_utf8(entry, e.path.toUtf8().constData());
        archive_entry_set_mtime(entry, 1700000000, 0);
        if (e.isDirectory) {
            archive_entry_set_filetype(entry, AE_IFDIR);
            archive_entry_set_perm(entry, e.perm ? e.perm : 0755);
        } else if (!e.linkTarget.isEmpty()) {
            archive_entry_set_filetype(entry, AE_IFLNK);
            archive_entry_set_perm(entry, 0777);
            archive_entry_set_symlink_utf8(entry, e.linkTarget.toUtf8().constData());
        } else {
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, e.perm ? e.perm : 0644);
            archive_entry_set_size(entry, e.data.size());
        }
        archive_write_header(a, entry);
        if (!e.data.isEmpty())
            archive_write_data(a, e.data.constData(), size_t(e.data.size()));
        archive_entry_free(entry);
    }
    const bool ok = archive_write_close(a) == ARCHIVE_OK;
    if (!ok)
        ADD_FAILURE() << archive_error_string(a);
    archive_write_free(a);
    return ok;
}

const QList<Entry> kTree = {
    {"./d", {}, true, {}},
    {"./d/a.txt", "hello world", false, {}},
    {"./d/link", {}, false, "a.txt"},
    {"./d/sub", {}, true, {}},
    {"./d/sub/zażółć.txt", "x", false, {}},
};

} // namespace

TEST(ArchivePluginTest, Loads)
{
    ASSERT_NE(plugin(), nullptr);
    EXPECT_TRUE(plugin()->name().startsWith("libarchive"));
    bool hasZip = false;
    for (const spp::ArchiveFormat& format : plugin()->formats())
        hasZip = hasZip || (format.id == "zip" && format.canRead);
    EXPECT_TRUE(hasZip);
}

TEST(ArchivePluginTest, ListsEntries)
{
    ASSERT_NE(plugin(), nullptr);
    QTemporaryDir dir;
    const QString path = dir.filePath("a.tar.gz");
    ASSERT_TRUE(writeArchive(path, "pax", "gzip", kTree));

    QList<spp::ArchiveItem> items;
    QString error;
    int progressCalls = 0;
    ASSERT_TRUE(plugin()->list(path, items, error, [&](qint64, qint64 total, const QString&) {
        EXPECT_GT(total, 0);
        ++progressCalls;
        return true;
    })) << error.toStdString();
    EXPECT_EQ(progressCalls, 5);
    ASSERT_EQ(items.size(), 5);

    // No leading "./", names in UTF-8.
    EXPECT_EQ(items[0].path, "d");
    EXPECT_TRUE(items[0].isDirectory);
    EXPECT_EQ(items[1].path, "d/a.txt");
    EXPECT_EQ(items[1].size, 11);
    EXPECT_EQ(items[1].permissions, 0644u);
    EXPECT_EQ(items[1].modTime.toSecsSinceEpoch(), 1700000000);
    EXPECT_TRUE(items[2].isSymlink);
    EXPECT_EQ(items[2].linkTarget, "a.txt");
    EXPECT_EQ(items[4].path, QString::fromUtf8("d/sub/zażółć.txt"));
}

TEST(ArchivePluginTest, ReadsTheStartOfAnEntry)
{
    ASSERT_NE(plugin(), nullptr);
    QTemporaryDir dir;
    const QString path = dir.filePath("a.tar.xz");
    const QByteArray big(3 * 1024 * 1024, 'q');
    ASSERT_TRUE(writeArchive(path, "pax", "xz", {{"big.bin", big, false, {}}, {"d/a.txt", "hello world", false, {}}}));

    QBuffer out;
    out.open(QIODevice::WriteOnly);
    QString error;
    EXPECT_EQ(plugin()->readEntry(path, "big.bin", &out, 1000, error), 1000);
    EXPECT_EQ(out.data(), big.left(1000));

    QBuffer whole;
    whole.open(QIODevice::WriteOnly);
    EXPECT_EQ(plugin()->readEntry(path, "d/a.txt", &whole, -1, error), 11);
    EXPECT_EQ(whole.data(), "hello world");

    EXPECT_EQ(plugin()->readEntry(path, "missing", &whole, -1, error), -1);
    EXPECT_FALSE(error.isEmpty());
}

TEST(ArchivePluginTest, CancelsListing)
{
    ASSERT_NE(plugin(), nullptr);
    QTemporaryDir dir;
    const QString path = dir.filePath("a.zip");
    ASSERT_TRUE(writeArchive(path, "zip", "none", kTree));

    QList<spp::ArchiveItem> items;
    QString error;
    EXPECT_FALSE(plugin()->list(path, items, error, [](qint64, qint64, const QString&) { return false; }));
    EXPECT_EQ(error, "Cancelled");
}

TEST(ArchivePluginTest, AsksForThePassword)
{
    ASSERT_NE(plugin(), nullptr);
    QTemporaryDir dir;
    const QString path = dir.filePath("secret.zip");
    ASSERT_TRUE(writeArchive(path, "zip", "none", {{"a.txt", "hello world", false, {}}}, "secret"));

    QList<spp::ArchiveItem> items;
    QString error;
    ASSERT_TRUE(plugin()->list(path, items, error));
    ASSERT_EQ(items.size(), 1);
    EXPECT_TRUE(items[0].encrypted);

    // Asked again after a wrong password; an empty answer gives up.
    QStringList answers = {"wrong", "secret"};
    int asked = 0;
    QBuffer out;
    out.open(QIODevice::WriteOnly);
    EXPECT_EQ(plugin()->readEntry(path, "a.txt", &out, -1, error, [&](const QString&) {
        return asked < answers.size() ? answers[asked++] : QString();
    }), 11) << error.toStdString();
    EXPECT_EQ(out.data(), "hello world");
    EXPECT_EQ(asked, 2);

    QBuffer none;
    none.open(QIODevice::WriteOnly);
    EXPECT_EQ(plugin()->readEntry(path, "a.txt", &none, -1, error, [](const QString&) { return QString(); }), -1);
}

namespace {

QByteArray contents(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<none>");
}

} // namespace

TEST(ArchivePluginTest, ExtractsAll)
{
    ASSERT_NE(plugin(), nullptr);
    QTemporaryDir dir;
    const QString path = dir.filePath("a.tar.gz");
    ASSERT_TRUE(writeArchive(path, "pax", "gzip", kTree));
    const QString dest = dir.filePath("out");

    QString error;
    int progressCalls = 0;
    ASSERT_TRUE(plugin()->extract(path, dest, {}, error, [&](qint64, qint64, const QString&) {
        ++progressCalls;
        return true;
    })) << error.toStdString();
    EXPECT_GT(progressCalls, 0);

    EXPECT_EQ(contents(dest + "/d/a.txt"), "hello world");
    EXPECT_EQ(contents(dest + QString::fromUtf8("/d/sub/zażółć.txt")), "x");
    EXPECT_EQ(QFileInfo(dest + "/d/link").symLinkTarget(), QFileInfo(dest + "/d/a.txt").absoluteFilePath());
    const QFileInfo a(dest + "/d/a.txt");
    EXPECT_EQ(a.lastModified().toSecsSinceEpoch(), 1700000000);
    EXPECT_FALSE(a.permissions() & QFileDevice::WriteOther);
    EXPECT_TRUE(a.permissions() & QFileDevice::ReadOther);
    EXPECT_EQ(QFileInfo(dest + "/d").lastModified().toSecsSinceEpoch(), 1700000000);
}

TEST(ArchivePluginTest, ExtractsSelectedEntries)
{
    ASSERT_NE(plugin(), nullptr);
    QTemporaryDir dir;
    const QString path = dir.filePath("a.zip");
    ASSERT_TRUE(writeArchive(path, "zip", "none", kTree));
    const QString dest = dir.filePath("out");

    QString error;
    ASSERT_TRUE(plugin()->extract(path, dest, {"d/sub"}, error)) << error.toStdString();
    EXPECT_EQ(contents(dest + QString::fromUtf8("/d/sub/zażółć.txt")), "x");
    EXPECT_FALSE(QFileInfo::exists(dest + "/d/a.txt"));
    // "d/s" is not a parent of "d/sub".
    QTemporaryDir other;
    ASSERT_TRUE(plugin()->extract(path, other.path(), {"d/s"}, error));
    EXPECT_TRUE(QDir(other.path()).isEmpty());
}

TEST(ArchivePluginTest, AsksAboutExistingTargets)
{
    ASSERT_NE(plugin(), nullptr);
    QTemporaryDir dir;
    const QString path = dir.filePath("a.tar");
    ASSERT_TRUE(writeArchive(path, "pax", "none", {{"a.txt", "new", false, {}}, {"b.txt", "new", false, {}}}));
    const QString dest = dir.filePath("out");
    const auto reset = [&] {
        QDir(dest).removeRecursively();
        QDir().mkpath(dest);
        QFile a(dest + "/a.txt");
        ASSERT_TRUE(a.open(QIODevice::WriteOnly));
        a.write("old");
    };
    QString error;
    QStringList asked;
    const auto answer = [&](spp::ExtractConflict choice) {
        return [&asked, choice](const spp::ArchiveItem& item, const QString&) {
            asked << item.path;
            return choice;
        };
    };

    reset();
    ASSERT_TRUE(plugin()->extract(path, dest, {}, error, {}, {}, answer(spp::ExtractConflict::Skip)));
    EXPECT_EQ(asked, QStringList{"a.txt"});
    EXPECT_EQ(contents(dest + "/a.txt"), "old");
    EXPECT_EQ(contents(dest + "/b.txt"), "new");

    reset();
    ASSERT_TRUE(plugin()->extract(path, dest, {}, error, {}, {}, answer(spp::ExtractConflict::Overwrite)));
    EXPECT_EQ(contents(dest + "/a.txt"), "new");

    reset();
    EXPECT_FALSE(plugin()->extract(path, dest, {}, error, {}, {}, answer(spp::ExtractConflict::Cancel)));
    EXPECT_EQ(error, "Cancelled");
    EXPECT_EQ(contents(dest + "/a.txt"), "old");
    EXPECT_FALSE(QFileInfo::exists(dest + "/b.txt"));
}

TEST(ArchivePluginTest, StaysInsideTheDestination)
{
    ASSERT_NE(plugin(), nullptr);
    QTemporaryDir dir;
    const QString outside = dir.filePath("outside");
    QDir().mkpath(outside);
    const QString path = dir.filePath("evil.tar");
    ASSERT_TRUE(writeArchive(path, "pax", "none", {
        {"../evil.txt", "evil", false, {}},
        {"d/../../evil2.txt", "evil", false, {}},
        {"d/out", {}, false, outside},       // a link out of the destination...
        {"d/out/x.txt", "evil", false, {}},  // ...and a file written through it
        {"d/ok.txt", "ok", false, {}},
    }));
    const QString dest = dir.filePath("out");

    QString error;
    ASSERT_TRUE(plugin()->extract(path, dest, {}, error)) << error.toStdString();
    EXPECT_EQ(contents(dest + "/d/ok.txt"), "ok");
    EXPECT_FALSE(QFileInfo::exists(dir.filePath("evil.txt")));
    EXPECT_FALSE(QFileInfo::exists(dir.filePath("evil2.txt")));
    EXPECT_FALSE(QFileInfo::exists(outside + "/x.txt"));
}

TEST(ArchivePluginTest, ExtractsIntoReadOnlyDirectories)
{
    ASSERT_NE(plugin(), nullptr);
    QTemporaryDir dir;
    const QString path = dir.filePath("ro.tar");
    ASSERT_TRUE(writeArchive(path, "pax", "none", {{"ro", {}, true, {}, 0555}, {"ro/f.txt", "f", false, {}}}));
    const QString dest = dir.filePath("out");

    QString error;
    ASSERT_TRUE(plugin()->extract(path, dest, {}, error)) << error.toStdString();
    EXPECT_EQ(contents(dest + "/ro/f.txt"), "f");
    EXPECT_FALSE(QFileInfo(dest + "/ro").permissions() & QFileDevice::WriteOwner);
    QFile::setPermissions(dest + "/ro", QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

TEST(ArchivePluginTest, CancelsExtracting)
{
    ASSERT_NE(plugin(), nullptr);
    QTemporaryDir dir;
    const QString path = dir.filePath("a.tar.gz");
    ASSERT_TRUE(writeArchive(path, "pax", "gzip", kTree));

    QString error;
    EXPECT_FALSE(plugin()->extract(path, dir.filePath("out"), {}, error,
                                   [](qint64, qint64, const QString&) { return false; }));
    EXPECT_EQ(error, "Cancelled");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
