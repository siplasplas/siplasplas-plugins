// SPDX-License-Identifier: LGPL-3.0-only
// The editor plugin, loaded as programs load it: opening and saving files in
// their encoding, the modified state, reloading as an undo step, and what
// the listener is told.

#include "siplasplas/plugin/EditorPlugin.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QKeyEvent>
#include <QPluginLoader>
#include <QTemporaryDir>
#include <QWidget>

#include <memory>

namespace {

spp::EditorPlugin* plugin()
{
    static QPluginLoader loader(QStringLiteral(SPP_EDITOR_PLUGIN_FILE));
    return qobject_cast<spp::EditorPlugin*>(loader.instance());
}

// "Zażółć gęślą jaźń." in cp1250.
const QByteArray polishCp1250("Za\xBF\xF3\xB3\xE6 g\xEA\x9Cl\xB9 ja\x9F\xF1.");
const QString polish = QStringLiteral("Zażółć gęślą jaźń.");

QString write(const QTemporaryDir& dir, const QString& name, const QByteArray& bytes)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    file.write(bytes);
    return path;
}

QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Types into the editor as a user does.
void type(spp::EditorView* view, int key, const QString& text = {},
          Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QWidget* target = view->widget()->focusProxy();
    QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
    QCoreApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
    QCoreApplication::sendEvent(target, &release);
}

struct Listener : spp::EditorViewListener {
    QList<bool> modified;
    QStringList paths;
    spp::TextPosition cursor{-1, -1};
    void modificationChanged(bool value) override { modified << value; }
    void cursorMoved(spp::TextPosition position) override { cursor = position; }
    void filePathChanged(const QString& path) override { paths << path; }
};

std::unique_ptr<QWidget> editorWidget(spp::EditorView* view)
{
    return std::unique_ptr<QWidget>(view->widget());
}

} // namespace

TEST(EditorPlugin, Loads)
{
    ASSERT_NE(plugin(), nullptr);
    EXPECT_EQ(plugin()->name(), QStringLiteral("qcodeedit"));
}

TEST(EditorPlugin, OpensAndSavesInTheFilesEncoding)
{
    QTemporaryDir dir;
    const QByteArray bytes = polishCp1250 + "\r\nsecond\r\n";
    const QString path = write(dir, "cp1250.txt", bytes);
    spp::EditorView* view = plugin()->createEditor();
    const auto owner = editorWidget(view);
    ASSERT_EQ(view->open(path, QStringLiteral("cp1250")), spp::EditorResult::Ok);
    EXPECT_EQ(view->text(), polish + QStringLiteral("\nsecond"));
    EXPECT_EQ(view->filePath(), path);
    EXPECT_EQ(view->encoding(), QStringLiteral("cp1250"));
    EXPECT_TRUE(view->uncertainEncodings().isEmpty());
    EXPECT_FALSE(view->isModified());
    ASSERT_EQ(view->save(), spp::EditorResult::Ok);
    EXPECT_EQ(read(path), bytes);
}

TEST(EditorPlugin, ReportsEditsAndSaving)
{
    QTemporaryDir dir;
    const QString path = write(dir, "a.txt", "alpha\n");
    spp::EditorView* view = plugin()->createEditor();
    const auto owner = editorWidget(view);
    Listener listener;
    view->setListener(&listener);
    ASSERT_EQ(view->open(path), spp::EditorResult::Ok);
    EXPECT_EQ(listener.paths, QStringList{path});

    view->setCursorPosition({0, 5});
    EXPECT_EQ(listener.cursor.line, 0);
    EXPECT_EQ(listener.cursor.column, 5);
    type(view, Qt::Key_X, QStringLiteral("x"));
    EXPECT_TRUE(view->isModified());
    EXPECT_EQ(listener.modified, QList<bool>{true});
    EXPECT_EQ(view->cursorPosition().column, 6);

    const QString other = dir.filePath("b.txt");
    ASSERT_EQ(view->saveAs(other), spp::EditorResult::Ok);
    EXPECT_FALSE(view->isModified());
    EXPECT_EQ((listener.modified), (QList<bool>{true, false}));
    EXPECT_EQ(listener.paths, (QStringList{path, other}));
    EXPECT_EQ(read(other), QByteArray("alphax\n"));
    view->setListener(nullptr);
}

TEST(EditorPlugin, ReloadIsAnUndoStep)
{
    QTemporaryDir dir;
    const QString path = write(dir, "reload.txt", "one\ntwo\n");
    spp::EditorView* view = plugin()->createEditor();
    const auto owner = editorWidget(view);
    ASSERT_EQ(view->open(path), spp::EditorResult::Ok);
    write(dir, "reload.txt", "one\n2\nthree\n");
    EXPECT_TRUE(view->hasExternalChanges());
    ASSERT_EQ(view->reload(), spp::EditorResult::Ok);
    EXPECT_EQ(view->text(), QStringLiteral("one\n2\nthree"));
    EXPECT_FALSE(view->isModified());
    EXPECT_FALSE(view->hasExternalChanges());
    type(view, Qt::Key_Z, {}, Qt::ControlModifier);
    EXPECT_EQ(view->text(), QStringLiteral("one\ntwo"));
    EXPECT_TRUE(view->isModified());
}

TEST(EditorPlugin, ReadOnlyAndCursor)
{
    QTemporaryDir dir;
    const QString path = write(dir, "ro.txt", "abc\n");
    spp::EditorView* view = plugin()->createEditor();
    const auto owner = editorWidget(view);
    ASSERT_EQ(view->open(path), spp::EditorResult::Ok);
    view->setReadOnly(true);
    EXPECT_TRUE(view->isReadOnly());
    type(view, Qt::Key_X, QStringLiteral("x"));
    EXPECT_EQ(view->text(), QStringLiteral("abc"));
    EXPECT_FALSE(view->isModified());
    // Clamped to the text.
    view->setCursorPosition({10, 10});
    EXPECT_EQ(view->cursorPosition().line, 0);
    EXPECT_EQ(view->cursorPosition().column, 3);
    EXPECT_TRUE(view->wordWrap());
}

TEST(EditorPlugin, ReportsFailures)
{
    QTemporaryDir dir;
    spp::EditorView* view = plugin()->createEditor();
    const auto owner = editorWidget(view);
    EXPECT_EQ(view->save(), spp::EditorResult::Failed);
    EXPECT_EQ(view->saveAs(dir.filePath("missing/x.txt")), spp::EditorResult::Failed);
    EXPECT_FALSE(view->errorString().isEmpty());
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
