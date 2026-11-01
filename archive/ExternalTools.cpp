// SPDX-License-Identifier: LGPL-3.0-only
// The archive plugin's reserve: lsar, 7z and unar (see ExternalTools.h).

#include "ExternalTools.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimeZone>

namespace spp::tools {

namespace {

QString cancelled()
{
    return QObject::tr("Cancelled");
}

// The output of a program run to its end, or stopped.
struct Run {
    bool started = false;
    bool stopped = false;     // by `onOutput` (cancelled, or enough read)
    int exitCode = -1;
    QByteArray out;           // all of it, unless `onOutput` takes it
    QByteArray err;
};

// Runs `program`; `input` goes to its standard input, which is closed then
// (a password prompt gets nothing more). `onOutput` gets the standard
// output as it comes (it is not kept in Run::out then); false stops the
// program.
Run run(const QString& program, const QStringList& args, const QString& workDir, const QByteArray& input,
        const std::function<bool(const QByteArray&)>& onOutput = {})
{
    Run result;
    QProcess process;
    if (!workDir.isEmpty())
        process.setWorkingDirectory(workDir);
    process.start(program, args);
    if (!process.waitForStarted()) {
        result.err = process.errorString().toLocal8Bit();
        return result;
    }
    result.started = true;
    if (!input.isEmpty())
        process.write(input);
    process.closeWriteChannel();
    for (;;) {
        const bool finished = process.waitForFinished(50);
        const QByteArray chunk = process.readAllStandardOutput();
        result.err += process.readAllStandardError();
        if (onOutput) {
            if (!chunk.isEmpty() && !onOutput(chunk)) {
                result.stopped = true;
                break;
            }
            if (!finished && !onOutput({})) {  // a chance to cancel while it is quiet
                result.stopped = true;
                break;
            }
        } else {
            result.out += chunk;
        }
        if (finished)
            break;
    }
    if (result.stopped) {
        process.kill();
        process.waitForFinished();
        return result;
    }
    result.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    return result;
}

// Whether a program's messages say that the password is missing or wrong.
bool wantsPassword(const Run& r)
{
    const QString text = QString::fromLocal8Bit(r.err + r.out).toLower();
    return text.contains(QLatin1String("wrong password")) || text.contains(QLatin1String("encrypted"))
           || text.contains(QLatin1String("password"));
}

QString messageOf(const QString& program, const Run& r)
{
    if (!r.started)
        return QObject::tr("Cannot start %1: %2").arg(program, QString::fromLocal8Bit(r.err));
    const QString text = QString::fromLocal8Bit(r.err).trimmed();
    return text.isEmpty() ? QObject::tr("%1 failed (exit code %2)").arg(program).arg(r.exitCode)
                          : QObject::tr("%1 failed: %2").arg(program, text);
}

// The password of one call: asked once, then kept for every program run in
// it (lsar, then 7z).
class Password {
public:
    Password(const ArchivePassword& ask, const QString& archivePath) : m_ask(ask), m_archivePath(archivePath) {}

    const QString& known() const { return m_known; }
    void set(const QString& password) { m_known = password; }

    // After a missing or wrong one: asks; false when none is given.
    bool askAgain()
    {
        if (!m_ask)
            return false;
        m_known = m_ask(m_archivePath);
        return !m_known.isEmpty();
    }

private:
    const ArchivePassword& m_ask;
    QString m_archivePath;
    QString m_known;
};

// Runs 7z with `args` and a password switch, asking for the password when 7z
// wants one (again after a wrong one). 7z reads a password asked for from
// the terminal, not from its standard input, so it is always given on the
// command line: "-p-" (none, 7z fails instead of asking) until one is known.
Run run7z(const QStringList& args, const QString& workDir, Password& password,
          const std::function<bool(const QByteArray&)>& onOutput = {})
{
    for (;;) {
        QStringList withPassword = args;
        withPassword.insert(1, password.known().isEmpty() ? QStringLiteral("-p-")
                                                          : QStringLiteral("-p") + password.known());
        Run r = run(QStringLiteral("7z"), withPassword, workDir, {}, onOutput);
        if (r.stopped || r.exitCode == 0 || !wantsPassword(r) || !password.askAgain())
            return r;
    }
}

// "1996-04-02 16:53:32.0000000" as 7z writes it, in local time.
QDateTime sevenZipDate(const QString& text)
{
    return QDateTime::fromString(text.left(19), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

// "-rw-r--r--" / "drwxr-xr-x" -> 0644 / 0755; 0 when not such a string.
quint32 unixMode(const QString& text)
{
    if (text.size() != 10)
        return 0;
    quint32 mode = 0;
    for (int i = 1; i < 10; ++i) {
        if (text.at(i) != QLatin1Char('-'))
            mode |= 1u << (9 - i);
    }
    return mode;
}

QString cleanPath(QString path)
{
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.startsWith(QLatin1String("./")))
        path.remove(0, 2);
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

// The total size of the archive's files, for progress.
qint64 sizeOf(const QString& archivePath)
{
    return QFileInfo(archivePath).size();
}

} // namespace

bool installed(const QString& program)
{
    return !QStandardPaths::findExecutable(program).isEmpty();
}

QDateTime lsarDate(const QString& text)
{
    // lsar writes "1996-04-02 16:53:32 +0200" (not ISO: a space, an offset
    // without a colon); ISO is taken too.
    static const QRegularExpression pattern(
        QStringLiteral("^(\\d{4}-\\d\\d-\\d\\d)[ T](\\d\\d:\\d\\d:\\d\\d)\\s*(?:([+-])(\\d\\d):?(\\d\\d)|Z)?$"));
    const QRegularExpressionMatch m = pattern.match(text.trimmed());
    if (!m.hasMatch())
        return QDateTime::fromString(text, Qt::ISODate);
    const QDate date = QDate::fromString(m.captured(1), Qt::ISODate);
    const QTime time = QTime::fromString(m.captured(2), Qt::ISODate);
    int offset = 0;  // UTC without an offset ("Z" or none)
    if (!m.captured(3).isEmpty()) {
        offset = (m.captured(4).toInt() * 60 + m.captured(5).toInt()) * 60;
        if (m.captured(3) == QLatin1String("-"))
            offset = -offset;
    }
    return QDateTime(date, time, QTimeZone::fromSecondsAheadOfUtc(offset));
}

QList<ArchiveItem> parseLsar(const QByteArray& json)
{
    QList<ArchiveItem> items;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    for (const QJsonValue& value : doc.object().value(QStringLiteral("lsarContents")).toArray()) {
        const QJsonObject obj = value.toObject();
        ArchiveItem item;
        item.path = cleanPath(obj.value(QStringLiteral("XADFileName")).toString());
        if (item.path.isEmpty())
            continue;
        // Numbers (1) in lsar's output, not JSON booleans.
        item.isDirectory = obj.value(QStringLiteral("XADIsDirectory")).toVariant().toBool();
        item.isSymlink = obj.value(QStringLiteral("XADIsLink")).toVariant().toBool();
        item.linkTarget = obj.value(QStringLiteral("XADLinkDestination")).toString();
        item.size = item.isDirectory ? 0 : obj.value(QStringLiteral("XADFileSize")).toVariant().toLongLong();
        item.modTime = lsarDate(obj.value(QStringLiteral("XADLastModificationDate")).toString());
        item.permissions = quint32(obj.value(QStringLiteral("XADPosixPermissions")).toVariant().toUInt() & 07777);
        item.encrypted = obj.value(QStringLiteral("XADIsEncrypted")).toVariant().toBool();
        items.append(item);
    }
    return items;
}

QList<ArchiveItem> parse7zList(const QByteArray& slt)
{
    QList<ArchiveItem> items;
    ArchiveItem item;
    bool inItem = false;
    const auto finish = [&] {
        if (inItem && !item.path.isEmpty())
            items.append(item);
        item = ArchiveItem();
        inItem = false;
    };
    for (const QByteArray& rawLine : slt.split('\n')) {
        const QString line = QString::fromUtf8(rawLine).trimmed();
        if (line.isEmpty()) {
            finish();
            continue;
        }
        const int eq = int(line.indexOf(QLatin1String(" = ")));
        const QString key = eq < 0 ? line.chopped(line.endsWith(QLatin1String(" =")) ? 2 : 0) : line.left(eq);
        const QString value = eq < 0 ? QString() : line.mid(eq + 3);
        if (key == QLatin1String("Path")) {
            finish();
            inItem = true;
            item.path = cleanPath(value);
        } else if (!inItem) {
            continue;
        } else if (key == QLatin1String("Folder")) {
            item.isDirectory = value == QLatin1String("+");
        } else if (key == QLatin1String("Size")) {
            item.size = value.toLongLong();
        } else if (key == QLatin1String("Modified")) {
            item.modTime = sevenZipDate(value);
        } else if (key == QLatin1String("Attributes")) {
            // "D drwxr-xr-x", "A -rw-r--r--", "DC" (DOS attributes only)
            const QString mode = value.section(QLatin1Char(' '), 1, 1);
            if (value.startsWith(QLatin1Char('D')) || mode.startsWith(QLatin1Char('d')))
                item.isDirectory = true;
            if (mode.startsWith(QLatin1Char('l')))
                item.isSymlink = true;
            item.permissions = unixMode(mode);
        } else if (key == QLatin1String("Encrypted")) {
            item.encrypted = value == QLatin1String("+");
        }
    }
    finish();
    for (ArchiveItem& i : items) {
        if (i.isDirectory)
            i.size = 0;
    }
    return items;
}

namespace {

bool listWith(const QString& archivePath, QList<ArchiveItem>& items, QString& error,
              const ArchiveProgress& progress, Password& password)
{
    const qint64 total = sizeOf(archivePath);
    // No progress from the programs: a chance to cancel while they work.
    bool cancel = false;
    const auto watch = [&](const QByteArray&) {
        cancel = progress && !progress(0, total, QString());
        return !cancel;
    };
    QByteArray output;
    const auto keep = [&](const QByteArray& chunk) {
        output += chunk;
        return watch(chunk);
    };
    if (installed(QStringLiteral("lsar"))) {
        for (;;) {
            output.clear();
            QStringList args = {"-j"};
            if (!password.known().isEmpty())
                args << "-p" << password.known();  // lsar has no other way
            args << archivePath;
            const Run r = run(QStringLiteral("lsar"), args, {}, {}, keep);
            if (cancel) {
                error = cancelled();
                return false;
            }
            // Without a password lsar writes a line before the listing.
            const QList<ArchiveItem> found = parseLsar(output.mid(output.indexOf('{')));
            const bool needsPassword = wantsPassword(r) || output.startsWith("This archive requires a password");
            if (r.exitCode == 0 && !found.isEmpty() && !needsPassword) {
                items = found;
                return true;
            }
            if (!needsPassword || !password.askAgain()) {
                error = messageOf(QStringLiteral("lsar"), r);
                break;
            }
        }
    }
    if (installed(QStringLiteral("7z"))) {
        output.clear();
        const Run r = run7z({"l", "-slt", "-ba", archivePath}, {}, password, keep);
        if (cancel) {
            error = cancelled();
            return false;
        }
        const QList<ArchiveItem> found = parse7zList(output);
        if (r.exitCode == 0 && !found.isEmpty()) {
            items = found;
            error.clear();
            return true;
        }
        error = messageOf(QStringLiteral("7z"), r);
        return false;
    }
    if (error.isEmpty())
        error = QObject::tr("Neither lsar (unar) nor 7z is installed to read this archive.");
    return false;
}

} // namespace

bool list(const QString& archivePath, QList<ArchiveItem>& items, QString& error,
          const ArchiveProgress& progress, const ArchivePassword& password)
{
    Password session(password, archivePath);
    return listWith(archivePath, items, error, progress, session);
}

bool extract(const QString& archivePath, const QString& destDir, const QStringList& entries, QString& error,
             const ArchiveProgress& progress, const ArchivePassword& password,
             const ExtractConflictHandler& conflict, const QSet<QString>& written)
{
    Password session(password, archivePath);
    const QString root = QDir::cleanPath(QDir(destDir).absolutePath());
    if (!QDir().mkpath(root)) {
        error = QObject::tr("Cannot create directory '%1'").arg(root);
        return false;
    }
    // Existing files: asked about before (the programs overwrite all or none).
    QStringList excluded;
    if (conflict) {
        QList<ArchiveItem> items;
        if (!listWith(archivePath, items, error, progress, session))
            return false;
        for (const ArchiveItem& item : items) {
            if (item.isDirectory)
                continue;
            bool selected = entries.isEmpty();
            for (const QString& e : entries)
                selected = selected || item.path == e || item.path.startsWith(e + QLatin1Char('/'));
            const QString target = root + QLatin1Char('/') + item.path;
            if (!selected || written.contains(target) || !(QFileInfo::exists(target) || QFileInfo(target).isSymLink()))
                continue;
            const ExtractConflict choice = conflict(item, target);
            if (choice == ExtractConflict::Cancel) {
                error = cancelled();
                return false;
            }
            if (choice == ExtractConflict::Skip)
                excluded << item.path;
        }
    }
    const qint64 total = sizeOf(archivePath);
    static const QRegularExpression percent(QStringLiteral("(\\d{1,3})%"));
    bool cancel = false;
    const auto watch = [&](const QByteArray& chunk) {
        qint64 done = 0;
        auto matches = percent.globalMatch(QString::fromLocal8Bit(chunk));
        while (matches.hasNext())
            done = total * matches.next().captured(1).toInt() / 100;
        cancel = progress && !progress(done, total, QString());
        return !cancel;
    };
    if (installed(QStringLiteral("7z"))) {
        QTemporaryDir listDir;
        QStringList args = {"x", "-y", "-aoa", "-bsp1", QStringLiteral("-o%1").arg(root)};
        if (!excluded.isEmpty()) {
            QFile list(listDir.filePath(QStringLiteral("excluded.txt")));
            if (list.open(QIODevice::WriteOnly)) {
                for (const QString& path : excluded)
                    list.write(path.toUtf8() + '\n');
                list.close();
                args << "-scsUTF-8" << QStringLiteral("-x@%1").arg(list.fileName());
            }
        }
        args << archivePath << "--" << entries;
        const Run r = run7z(args, root, session, watch);
        if (cancel) {
            error = cancelled();
            return false;
        }
        if (r.exitCode == 0) {
            error.clear();
            return true;
        }
        error = messageOf(QStringLiteral("7z"), r);
        if (!installed(QStringLiteral("unar")))
            return false;
    }
    if (installed(QStringLiteral("unar"))) {
        // unar: no exclusions; the files asked to be kept are moved aside.
        QStringList args = {"-f", "-q", "-o", root, archivePath};
        for (const QString& e : entries)
            args << e << e + QStringLiteral("/*");
        for (;;) {
            const QStringList withPassword =
                session.known().isEmpty() ? args : QStringList{"-p", session.known()} + args;
            QTemporaryDir kept(root + QStringLiteral("/.spp-kept-XXXXXX"));
            for (const QString& path : excluded) {
                QDir().mkpath(QFileInfo(kept.filePath(path)).absolutePath());
                QFile::rename(root + QLatin1Char('/') + path, kept.filePath(path));
            }
            const Run r = run(QStringLiteral("unar"), withPassword, root, {}, watch);
            for (const QString& path : excluded) {
                QFile::remove(root + QLatin1Char('/') + path);
                QFile::rename(kept.filePath(path), root + QLatin1Char('/') + path);
            }
            if (cancel) {
                error = cancelled();
                return false;
            }
            if (r.exitCode == 0) {
                error.clear();
                return true;
            }
            if (!wantsPassword(r) || !session.askAgain()) {
                error = messageOf(QStringLiteral("unar"), r);
                return false;
            }
        }
    }
    error = QObject::tr("Neither 7z nor unar is installed to extract this archive.");
    return false;
}

qint64 readEntry(const QString& archivePath, const QString& entry, QIODevice* out, qint64 maxBytes,
                 QString& error, const ArchivePassword& password)
{
    if (!installed(QStringLiteral("7z"))) {
        error = QObject::tr("7z is not installed to read this archive.");
        return -1;
    }
    qint64 written = 0;
    bool failed = false;
    const auto take = [&](const QByteArray& chunk) {
        if (chunk.isEmpty())
            return true;
        const qint64 want = maxBytes < 0 ? chunk.size() : qMin<qint64>(chunk.size(), maxBytes - written);
        if (out->write(chunk.constData(), want) != want) {
            failed = true;
            return false;
        }
        written += want;
        return maxBytes < 0 || written < maxBytes;  // enough: stopped
    };
    Password session(password, archivePath);
    const Run r = run7z({"e", "-so", archivePath, "--", entry}, {}, session, take);
    if (failed) {
        error = out->errorString();
        return -1;
    }
    if (r.stopped)
        return written;
    // 7z ends well also when no file matched the name.
    if (r.exitCode == 0 && !QString::fromLocal8Bit(r.err).contains(QLatin1String("No files to process")))
        return written;
    error = r.exitCode == 0 ? QObject::tr("'%1' is not in the archive").arg(entry) : messageOf(QStringLiteral("7z"), r);
    return -1;
}

bool pack7z(const QString& archivePath, const QString& baseDir, const QStringList& files,
            const PackOptions& options, QString& error, const ArchiveProgress& progress)
{
    if (!installed(QStringLiteral("7z"))) {
        error = QObject::tr("Packing 7z with a password or a solid block size needs the 7z program.");
        return false;
    }
    // The names in a list file: thousands of them do not fit the command line.
    QTemporaryDir listDir;
    QFile list(listDir.filePath(QStringLiteral("files.txt")));
    if (!listDir.isValid() || !list.open(QIODevice::WriteOnly)) {
        error = QObject::tr("Cannot write a temporary file list.");
        return false;
    }
    for (const QString& file : files)
        list.write(file.toUtf8() + '\n');
    list.close();

    QStringList args = {"a", "-bsp1", "-scsUTF-8"};
    if (options.volumeSize > 0)
        args << QStringLiteral("-v%1b").arg(options.volumeSize);
    if (options.solidBlockSize > 0)
        args << QStringLiteral("-ms=%1b").arg(options.solidBlockSize) << "-mqs=on";
    if (options.level >= 0)
        args << QStringLiteral("-mx=%1").arg(options.level);
    if (!options.password.isEmpty())
        args << "-mhe=on";  // the names encrypted too
    args << archivePath << QStringLiteral("@%1").arg(list.fileName());

    // Progress in 7z's percentages of all of it.
    static const QRegularExpression percent(QStringLiteral("(\\d{1,3})%"));
    bool cancel = false;
    const auto watch = [&](const QByteArray& chunk) {
        qint64 done = 0;
        auto matches = percent.globalMatch(QString::fromLocal8Bit(chunk));
        while (matches.hasNext())
            done = matches.next().captured(1).toInt();
        cancel = progress && !progress(done, 100, QString());
        return !cancel;
    };
    // The password known: given, never asked ("-p-" when none).
    const ArchivePassword none;
    Password session(none, archivePath);
    session.set(options.password);
    const Run r = run7z(args, baseDir, session, watch);
    if (!cancel && r.exitCode == 0)
        return true;
    // A program stopped halfway leaves its output.
    QFile::remove(archivePath);
    for (int i = 1;; ++i) {
        const QString volume = QStringLiteral("%1.%2").arg(archivePath).arg(i, 3, 10, QLatin1Char('0'));
        if (!QFile::remove(volume))
            break;
    }
    error = cancel ? cancelled() : messageOf(QStringLiteral("7z"), r);
    return false;
}

} // namespace spp::tools
