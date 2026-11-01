// SPDX-License-Identifier: LGPL-3.0-only
// The editor plugin, loaded as programs load it: opening and saving files in
// their encoding, the modified state, reloading as an undo step, and what
// the listener is told.

#include "siplasplas/plugin/EditorPlugin.h"

#include <gtest/gtest.h>

#include <QAbstractScrollArea>
#include <QApplication>
#include <QFile>
#include <QElapsedTimer>
#include <QHelpEvent>
#include <QThread>
#include <QToolTip>
#include <QKeyEvent>
#include <QMouseEvent>
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
    int textChanges = 0;
    QList<spp::TextPosition> links;
    QList<spp::TextPosition> queries;
    // Answers link queries: a link before this column, none after it.
    spp::EditorView* view = nullptr;
    int linksBefore = 0;
    void textChanged() override { ++textChanges; }
    void linkQueried(spp::TextPosition position) override
    {
        queries << position;
        if (view) view->answerLink(position, position.column < linksBefore);
    }
    void linkActivated(spp::TextPosition position) override { links << position; }
    void modificationChanged(bool value) override { modified << value; }
    void cursorMoved(spp::TextPosition position) override { cursor = position; }
    void filePathChanged(const QString& path) override { paths << path; }
};

// QToolTip hides after a moment: waits until it is gone.
void hideToolTip()
{
    QToolTip::hideText();
    QElapsedTimer timer;
    timer.start();
    while (QToolTip::isVisible() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
}

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

// A mark's message shows where the mouse rests on it; typing tells the
// listener and takes the marks away.
TEST(EditorPlugin, MarksShowTheirMessages)
{
    QTemporaryDir dir;
    const QString path = write(dir, "marks.cpp", "int x = y;\nint z;\n");
    spp::EditorView* view = plugin()->createEditor();
    const auto owner = editorWidget(view);
    view->widget()->resize(600, 300);
    view->widget()->show();
    ASSERT_EQ(view->open(path), spp::EditorResult::Ok);
    Listener listener;
    view->setListener(&listener);
    view->setMarks({{{0, 0}, {0, 10}, spp::TextMark::Kind::Error, QStringLiteral("use of undeclared identifier 'y'")}});

    auto* area = view->widget()->findChild<QAbstractScrollArea*>();
    ASSERT_NE(area, nullptr);
    QHelpEvent onMark(QEvent::ToolTip, QPoint(4, 4), area->viewport()->mapToGlobal(QPoint(4, 4)));
    QCoreApplication::sendEvent(area->viewport(), &onMark);
    EXPECT_TRUE(QToolTip::isVisible());
    EXPECT_TRUE(QToolTip::text().contains(QStringLiteral("undeclared identifier")));
    hideToolTip();
    ASSERT_FALSE(QToolTip::isVisible());
    // The second line has no mark.
    const QPoint below(4, area->viewport()->fontMetrics().height() * 3 / 2 + 4);
    QHelpEvent offMark(QEvent::ToolTip, below, area->viewport()->mapToGlobal(below));
    QCoreApplication::sendEvent(area->viewport(), &offMark);
    EXPECT_FALSE(QToolTip::isVisible());

    view->setCursorPosition({1, 0});
    type(view, Qt::Key_A, QStringLiteral("a"));
    EXPECT_GT(listener.textChanges, 0);
    QCoreApplication::sendEvent(area->viewport(), &onMark);
    EXPECT_FALSE(QToolTip::isVisible());
    view->setListener(nullptr);
}

// With Ctrl the word under the mouse is asked about; the one the listener
// calls a link shows as one (a pointing hand), the other does not. Ctrl+click
// on the link tells the listener; on no link, and a plain click, does not.
TEST(EditorPlugin, CtrlClickFollowsALink)
{
    QTemporaryDir dir;
    const QString path = write(dir, "link.cpp", "answer(); // note\n");
    spp::EditorView* view = plugin()->createEditor();
    const auto owner = editorWidget(view);
    view->widget()->resize(600, 300);
    view->widget()->show();
    ASSERT_EQ(view->open(path), spp::EditorResult::Ok);
    Listener listener;
    listener.view = view;
    listener.linksBefore = 6;  // "answer" is a link, "note" none
    view->setListener(&listener);
    auto* area = view->widget()->findChild<QAbstractScrollArea*>();
    QWidget* viewport = area->viewport();
    const QPoint onWord(4, 4);
    const int charWidth = area->fontMetrics().horizontalAdvance(QLatin1Char('x'));
    const QPoint onComment(4 + 14 * charWidth, 4);
    auto move = [&](const QPoint& at, Qt::KeyboardModifiers modifiers) {
        QMouseEvent event(QEvent::MouseMove, at, viewport->mapToGlobal(at), Qt::NoButton, Qt::NoButton, modifiers);
        QCoreApplication::sendEvent(viewport, &event);
    };

    move(onWord, Qt::NoModifier);
    EXPECT_NE(viewport->cursor().shape(), Qt::PointingHandCursor);
    EXPECT_TRUE(listener.queries.isEmpty());
    move(onComment, Qt::ControlModifier);
    ASSERT_EQ(listener.queries.size(), 1);
    EXPECT_GE(listener.queries.last().column, 13);
    EXPECT_NE(viewport->cursor().shape(), Qt::PointingHandCursor);
    QMouseEvent ctrlClickComment(QEvent::MouseButtonPress, onComment, viewport->mapToGlobal(onComment),
                                 Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    QCoreApplication::sendEvent(viewport, &ctrlClickComment);
    EXPECT_TRUE(listener.links.isEmpty());

    move(onWord, Qt::ControlModifier);
    EXPECT_EQ(listener.queries.size(), 2);
    EXPECT_EQ(viewport->cursor().shape(), Qt::PointingHandCursor);
    // The same word again: no new question.
    move(onWord + QPoint(1, 0), Qt::ControlModifier);
    EXPECT_EQ(listener.queries.size(), 2);

    QMouseEvent plainClick(QEvent::MouseButtonPress, onWord, viewport->mapToGlobal(onWord), Qt::LeftButton,
                           Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(viewport, &plainClick);
    EXPECT_TRUE(listener.links.isEmpty());
    QMouseEvent ctrlClick(QEvent::MouseButtonPress, onWord, viewport->mapToGlobal(onWord), Qt::LeftButton,
                          Qt::LeftButton, Qt::ControlModifier);
    QCoreApplication::sendEvent(viewport, &ctrlClick);
    ASSERT_EQ(listener.links.size(), 1);
    EXPECT_EQ(listener.links.first().line, 0);
    EXPECT_EQ(view->cursorPosition().line, 0);
    // The link is gone once followed.
    EXPECT_NE(viewport->cursor().shape(), Qt::PointingHandCursor);
    view->setListener(nullptr);
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
