// SPDX-License-Identifier: LGPL-3.0-only
#ifndef SPP_ARCHIVE_EXTERNALTOOLS_H
#define SPP_ARCHIVE_EXTERNALTOOLS_H

// The archive plugin's reserve: external programs for what libarchive cannot
// do — lsar (unar) and 7z for formats it does not read or reads only in part
// (ARJ, ACE, encrypted RAR and 7z, solid RAR 1.5), and 7z to write 7z with a
// password or a solid block size. Passwords go on the programs' command
// lines (visible in the process list while they run): 7z reads a password
// it asks for from the terminal only, lsar has no other way.

#include "siplasplas/plugin/ArchivePlugin.h"

#include <QSet>

class QIODevice;

namespace spp::tools {

// Whether a program is installed ("lsar", "7z", "unar").
bool installed(const QString& program);

// Lists the archive by lsar, else by 7z.
bool list(const QString& archivePath, QList<ArchiveItem>& items, QString& error,
          const ArchiveProgress& progress, const ArchivePassword& password);

// Extracts by 7z, else unar. `written`: the files the caller wrote before
// in this extraction (libarchive, which broke off): overwritten without
// asking `conflict`.
bool extract(const QString& archivePath, const QString& destDir, const QStringList& entries, QString& error,
             const ArchiveProgress& progress, const ArchivePassword& password,
             const ExtractConflictHandler& conflict, const QSet<QString>& written);

// The first `maxBytes` (all when -1) of the file `entry`, by 7z.
qint64 readEntry(const QString& archivePath, const QString& entry, QIODevice* out, qint64 maxBytes,
                 QString& error, const ArchivePassword& password);

// Packs a 7z archive by the 7z program (a password, a solid block size).
bool pack7z(const QString& archivePath, const QString& baseDir, const QStringList& files,
            const PackOptions& options, QString& error, const ArchiveProgress& progress);

// Parsing, for the tests.
QList<ArchiveItem> parseLsar(const QByteArray& json);
QList<ArchiveItem> parse7zList(const QByteArray& slt);
// A date as lsar writes it, "1996-04-02 16:53:32 +0200", or ISO 8601.
QDateTime lsarDate(const QString& text);

} // namespace spp::tools

#endif // SPP_ARCHIVE_EXTERNALTOOLS_H
