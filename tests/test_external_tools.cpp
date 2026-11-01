// SPDX-License-Identifier: LGPL-3.0-only
// The archive plugin's reserve: lsar's and 7z's listings parsed, and the
// plugin going over to 7z for what libarchive cannot do (an encrypted 7z).

#include "ExternalTools.h"
#include "siplasplas/plugin/ArchivePlugin.h"

#include <gtest/gtest.h>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QPluginLoader>
#include <QTemporaryDir>

using namespace spp;

namespace {

ArchivePlugin* archivePlugin()
{
    static QPluginLoader loader(QStringLiteral(SPP_ARCHIVE_PLUGIN_FILE));
    return qobject_cast<ArchivePlugin*>(loader.instance());
}

void write(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(data);
}

} // namespace

TEST(ExternalToolsTest, LsarDates)
{
    // As lsar writes them: a space, an offset without a colon.
    EXPECT_EQ(tools::lsarDate("1996-04-02 16:53:32 +0200").toUTC().toString(Qt::ISODate), "1996-04-02T14:53:32Z");
    EXPECT_EQ(tools::lsarDate("1996-04-02 16:53:32 -0130").toUTC().toString(Qt::ISODate), "1996-04-02T18:23:32Z");
    EXPECT_EQ(tools::lsarDate("2024-05-06T07:08:09+02:00").toUTC().toString(Qt::ISODate), "2024-05-06T05:08:09Z");
    EXPECT_FALSE(tools::lsarDate("yesterday").isValid());
}

TEST(ExternalToolsTest, ParsesLsar)
{
    const QByteArray json = R"({
        "lsarFormatName": "RAR",
        "lsarContents": [
            {"XADFileName": "Fen", "XADIsDirectory": 1, "XADFileSize": 0,
             "XADLastModificationDate": "1999-08-13 15:12:26 +0200"},
            {"XADFileName": "Fen/a.fen", "XADFileSize": 44, "XADIsEncrypted": 1,
             "XADPosixPermissions": 33188},
            {"XADFileName": "dir/", "XADIsDirectory": true},
            {"XADFileName": "link", "XADIsLink": 1, "XADLinkDestination": "Fen/a.fen"},
            {"XADFileName": ""}
        ]
    })";
    const QList<ArchiveItem> items = tools::parseLsar(json);
    ASSERT_EQ(items.size(), 4);
    EXPECT_EQ(items[0].path, "Fen");
    EXPECT_TRUE(items[0].isDirectory);  // 1, not true
    EXPECT_EQ(items[0].modTime.toUTC().toString(Qt::ISODate), "1999-08-13T13:12:26Z");
    EXPECT_FALSE(items[1].isDirectory);
    EXPECT_EQ(items[1].size, 44);
    EXPECT_TRUE(items[1].encrypted);
    EXPECT_EQ(items[1].permissions, 0644u);
    EXPECT_EQ(items[2].path, "dir");  // no trailing '/'
    EXPECT_TRUE(items[3].isSymlink);
    EXPECT_EQ(items[3].linkTarget, "Fen/a.fen");
    EXPECT_TRUE(tools::parseLsar("not json").isEmpty());
}

TEST(ExternalToolsTest, Parses7zListing)
{
    const QByteArray slt =
        "Path = d\n"
        "Size = 0\n"
        "Modified = 2026-11-01 20:59:53.4651782\n"
        "Attributes = D drwxrwxr-x\n"
        "Encrypted = -\n"
        "\n"
        "Path = d/a.txt\n"
        "Size = 3\n"
        "Modified = 2026-11-01 20:59:53.4644397\n"
        "Attributes = A -rw-r--r--\n"
        "Encrypted = +\n"
        "\n"
        "Path = d/link\n"
        "Size = 5\n"
        "Attributes = A lrwxrwxrwx\n"
        "\n"
        "Path = Fen\n"
        "Folder = +\n"
        "Size = 0\n"
        "Attributes = DC\n";
    const QList<ArchiveItem> items = tools::parse7zList(slt);
    ASSERT_EQ(items.size(), 4);
    EXPECT_TRUE(items[0].isDirectory);
    EXPECT_EQ(items[0].permissions, 0775u);
    EXPECT_EQ(items[0].modTime.toString("yyyy-MM-dd HH:mm:ss"), "2026-11-01 20:59:53");
    EXPECT_EQ(items[1].path, "d/a.txt");
    EXPECT_EQ(items[1].size, 3);
    EXPECT_TRUE(items[1].encrypted);
    EXPECT_EQ(items[1].permissions, 0644u);
    EXPECT_TRUE(items[2].isSymlink);
    EXPECT_TRUE(items[3].isDirectory);  // "Folder = +", DOS attributes only
}

TEST(ExternalToolsTest, EncryptedSevenZipGoesThrough7z)
{
    if (!tools::installed("7z"))
        GTEST_SKIP() << "7z is not installed";
    ASSERT_NE(archivePlugin(), nullptr);
    QTemporaryDir dir;
    write(dir.filePath("src/d/a.txt"), "hello world");
    write(dir.filePath("src/d/sub/b.txt"), "b");

    // Packed with a password by 7z (libarchive writes no encrypted 7z), the
    // names encrypted too.
    const QString path = dir.filePath("secret.7z");
    PackOptions options;
    options.format = "7z";
    options.password = "tajne";
    QString error;
    ASSERT_TRUE(archivePlugin()->pack(path, dir.filePath("src"), {"d"}, options, error)) << error.toStdString();
    ASSERT_TRUE(QFileInfo::exists(path));

    // libarchive cannot read it: 7z does, asking for the password.
    QStringList answers = {"wrong", "tajne"};
    int asked = 0;
    const ArchivePassword password = [&](const QString&) {
        return asked < answers.size() ? answers[asked++] : QString();
    };
    QList<ArchiveItem> items;
    ASSERT_TRUE(archivePlugin()->list(path, items, error, {}, password)) << error.toStdString();
    EXPECT_EQ(asked, 2);  // again after the wrong one
    QStringList paths;
    for (const ArchiveItem& item : items)
        paths << item.path;
    paths.sort();
    EXPECT_EQ(paths, QStringList({"d", "d/a.txt", "d/sub", "d/sub/b.txt"}));

    const ArchivePassword right = [](const QString&) { return QString("tajne"); };
    QBuffer out;
    out.open(QIODevice::WriteOnly);
    EXPECT_EQ(archivePlugin()->readEntry(path, "d/a.txt", &out, 5, error, right), 5) << error.toStdString();
    EXPECT_EQ(out.data(), "hello");

    QTemporaryDir dest;
    ASSERT_TRUE(archivePlugin()->extract(path, dest.path(), {"d/sub"}, error, {}, right)) << error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(dest.filePath("d/sub/b.txt")));
    EXPECT_FALSE(QFileInfo::exists(dest.filePath("d/a.txt")));

    // No password given: it fails, it does not wait.
    items.clear();
    EXPECT_FALSE(archivePlugin()->list(path, items, error, {}, [](const QString&) { return QString(); }));
}

TEST(ExternalToolsTest, ExistingFilesAskedBefore7zExtracts)
{
    if (!tools::installed("7z"))
        GTEST_SKIP() << "7z is not installed";
    ASSERT_NE(archivePlugin(), nullptr);
    QTemporaryDir dir;
    write(dir.filePath("src/a.txt"), "new a");
    write(dir.filePath("src/b.txt"), "new b");
    const QString path = dir.filePath("s.7z");
    PackOptions options;
    options.format = "7z";
    options.password = "x";
    QString error;
    ASSERT_TRUE(archivePlugin()->pack(path, dir.filePath("src"), {"a.txt", "b.txt"}, options, error));

    QTemporaryDir dest;
    write(dest.filePath("a.txt"), "old a");
    write(dest.filePath("b.txt"), "old b");
    QStringList asked;
    const ExtractConflictHandler conflict = [&](const ArchiveItem& item, const QString&) {
        asked << item.path;
        return item.path == "a.txt" ? ExtractConflict::Skip : ExtractConflict::Overwrite;
    };
    ASSERT_TRUE(archivePlugin()->extract(path, dest.path(), {}, error, {}, [](const QString&) { return QString("x"); },
                                         conflict)) << error.toStdString();
    asked.sort();
    EXPECT_EQ(asked, QStringList({"a.txt", "b.txt"}));
    QFile a(dest.filePath("a.txt")), b(dest.filePath("b.txt"));
    ASSERT_TRUE(a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly));
    EXPECT_EQ(a.readAll(), "old a");  // kept
    EXPECT_EQ(b.readAll(), "new b");  // overwritten
}

TEST(ExternalToolsTest, SolidBlocksBy7z)
{
    if (!tools::installed("7z"))
        GTEST_SKIP() << "7z is not installed";
    ASSERT_NE(archivePlugin(), nullptr);
    QTemporaryDir dir;
    for (int i = 0; i < 4; ++i)
        write(dir.filePath(QString("src/f%1.bin").arg(i)), QByteArray(300 * 1000, char('a' + i)));
    const QString path = dir.filePath("blocks.7z");
    PackOptions options;
    options.format = "7z";
    options.solidBlockSize = 400 * 1000;  // two files a block at most
    QString error;
    ASSERT_TRUE(archivePlugin()->pack(path, dir.filePath("src"), {"f0.bin", "f1.bin", "f2.bin", "f3.bin"}, options,
                                      error)) << error.toStdString();
    // Without a password libarchive reads it.
    QList<ArchiveItem> items;
    ASSERT_TRUE(archivePlugin()->list(path, items, error)) << error.toStdString();
    EXPECT_EQ(items.size(), 4);
}
