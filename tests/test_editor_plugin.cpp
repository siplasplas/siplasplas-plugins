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
#include <QTreeWidget>
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

namespace {

struct BreakpointListener : spp::EditorViewListener {
    QList<QList<int>> told;
    void breakpointsChanged(const QList<int>& lines) override { told << lines; }
};

// An editor shown with a file of six lines, and the column of its margin
// and the height of its lines, found by clicking as the user does.
struct BreakpointEditor {
    QTemporaryDir dir;
    spp::EditorView* view = plugin()->createEditor();
    std::unique_ptr<QWidget> owner = editorWidget(view);
    BreakpointListener listener;
    QWidget* rail = nullptr;
    QWidget* text = view->widget()->focusProxy();

    BreakpointEditor()
    {
        view->widget()->resize(700, 400);
        view->widget()->show();
        view->open(write(dir, "lines.txt", "zero\none\ntwo\nthree\nfour\nfive\n"));
        for (QWidget* child : view->widget()->findChildren<QWidget*>())
            if (QString::fromLatin1(child->metaObject()->className()) == QLatin1String("qce::LeftRail")) rail = child;
        view->setListener(&listener);
    }
    ~BreakpointEditor() { view->setListener(nullptr); }

    void click(QPoint at)
    {
        QMouseEvent press(QEvent::MouseButtonPress, at, rail->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(rail, &press);
    }
    void press(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        type(view, key, key == Qt::Key_Return ? QStringLiteral("\r") : QString(), modifiers);
    }
    // The leftmost x of the margin, -1 without one; a toggled line is toggled back.
    int marginColumn()
    {
        for (int x = 0; x < rail->width(); ++x) {
            const qsizetype before = listener.told.size();
            click({x, 3});
            if (listener.told.size() == before) continue;
            click({x, 3});
            return x;
        }
        return -1;
    }
    QImage textImage() { return text->grab().toImage(); }
};

using Lines = QList<int>;

} // namespace

// Programs without a debugger never see the margin.
TEST(EditorPlugin, BreakpointMarginIsHiddenByDefault)
{
    BreakpointEditor editor;
    ASSERT_NE(editor.rail, nullptr);
    QCoreApplication::processEvents();
    EXPECT_FALSE(editor.view->hasBreakpointMargin());
    EXPECT_EQ(editor.marginColumn(), -1);
    EXPECT_TRUE(editor.listener.told.isEmpty());

    // Nor anything set for it.
    const QImage plain = editor.textImage();
    editor.view->setBreakpoints({0, 1});
    editor.view->setExecutionLine(2);
    EXPECT_EQ(editor.textImage(), plain);
}

TEST(EditorPlugin, ClicksInTheMarginToggleBreakpoints)
{
    BreakpointEditor editor;
    ASSERT_NE(editor.rail, nullptr);
    const int width = editor.rail->sizeHint().width();
    editor.view->setBreakpointMargin(true);
    EXPECT_GT(editor.rail->sizeHint().width(), width);
    QCoreApplication::processEvents();
    const int x = editor.marginColumn();
    ASSERT_GE(x, 0);
    ASSERT_EQ(editor.listener.told.size(), 2);
    EXPECT_EQ(editor.listener.told.at(0), Lines{0});
    EXPECT_EQ(editor.listener.told.at(1), Lines{});
    // Further down, the second line: in order, whichever came first.
    int y = 3;
    while (y < 100 && editor.view->breakpoints() != Lines{1}) {
        editor.click({x + 1, ++y});
        if (editor.view->breakpoints() == Lines{0}) editor.click({x + 1, y});
    }
    ASSERT_EQ(editor.view->breakpoints(), Lines{1});
    editor.click({x + 1, 3});
    EXPECT_EQ(editor.view->breakpoints(), (Lines{0, 1}));
    EXPECT_EQ(editor.listener.told.last(), (Lines{0, 1}));
    // Setting them is not told, and sorts them.
    const qsizetype told = editor.listener.told.size();
    editor.view->setBreakpoints({4, 2, 4});
    EXPECT_EQ(editor.view->breakpoints(), (Lines{2, 4}));
    EXPECT_EQ(editor.listener.told.size(), told);
}

TEST(EditorPlugin, BreakpointsAndTheExecutionLineAreShown)
{
    BreakpointEditor editor;
    editor.view->setBreakpointMargin(true);
    QCoreApplication::processEvents();
    const QImage plain = editor.textImage();
    editor.view->setBreakpoints({1});
    const QImage withBreakpoint = editor.textImage();
    EXPECT_NE(withBreakpoint, plain);
    editor.view->setExecutionLine(1);
    EXPECT_EQ(editor.view->executionLine(), 1);
    const QImage stopped = editor.textImage();
    EXPECT_NE(stopped, withBreakpoint);
    // The execution line shows over a breakpoint's.
    editor.view->setExecutionLine(-1);
    EXPECT_EQ(editor.textImage(), withBreakpoint);
}

// Lines stay on their code through edits, the listener hearing of it.
TEST(EditorPlugin, BreakpointsFollowTheirCode)
{
    BreakpointEditor editor;
    spp::EditorView* view = editor.view;
    auto textAt = [view](int line) { return view->text().split(QLatin1Char('\n')).value(line); };
    view->setBreakpoints({2, 4});

    // Enter above them moves both; the listener hears once.
    view->setCursorPosition({0, 0});
    editor.press(Qt::Key_Return);
    EXPECT_EQ(view->breakpoints(), (Lines{3, 5}));
    EXPECT_EQ(editor.listener.told.size(), 1);
    EXPECT_EQ(textAt(3), QStringLiteral("two"));
    // Enter at the start of its line: it goes with the code.
    view->setCursorPosition({3, 0});
    editor.press(Qt::Key_Return);
    EXPECT_EQ(view->breakpoints(), (Lines{4, 6}));
    EXPECT_EQ(textAt(4), QStringLiteral("two"));
    // Enter at its end: it stays.
    view->setCursorPosition({4, 3});
    editor.press(Qt::Key_Return);
    EXPECT_EQ(view->breakpoints(), (Lines{4, 7}));
    // Backspace joining its line to the one before: it goes along.
    view->setCursorPosition({5, 0});
    editor.press(Qt::Key_Backspace);
    EXPECT_EQ(view->breakpoints(), (Lines{4, 6}));
    view->setCursorPosition({4, 0});
    editor.press(Qt::Key_Backspace);
    EXPECT_EQ(view->breakpoints(), (Lines{3, 5}));
    EXPECT_EQ(textAt(3), QStringLiteral("two"));
    // Below them nothing moves and nothing is told.
    const qsizetype told = editor.listener.told.size();
    view->setCursorPosition({6, 0});
    editor.press(Qt::Key_Return);
    EXPECT_EQ(editor.listener.told.size(), told);
    // Removed with its line it is gone; the last removed line's code stays.
    view->setBreakpoints({1, 3, 5});
    ASSERT_TRUE(view->replace({{{{2, 0}, {4, 0}}, QString()}}, QStringLiteral("cut")));
    EXPECT_EQ(view->breakpoints(), (Lines{1, 3}));

    view->setBreakpoints({});
    view->setExecutionLine(2);
    view->setCursorPosition({0, 0});
    editor.press(Qt::Key_Return);
    EXPECT_EQ(view->executionLine(), 3);
}

TEST(EditorPlugin, BreakpointsAndFiles)
{
    BreakpointEditor editor;
    spp::EditorView* view = editor.view;
    const QString path = write(editor.dir, "file.txt", "a\nb\nc\nd\n");
    view->setBreakpoints({1});
    view->setExecutionLine(1);
    ASSERT_EQ(view->open(path), spp::EditorResult::Ok);
    EXPECT_TRUE(view->breakpoints().isEmpty());
    EXPECT_EQ(view->executionLine(), -1);

    // A reload changes only the lines that differ: they follow.
    view->setBreakpoints({2});
    write(editor.dir, "file.txt", "new\na\nb\nc\nd\n");
    ASSERT_EQ(view->reload(), spp::EditorResult::Ok);
    EXPECT_EQ(view->breakpoints(), Lines{3});
    // Another encoding is the same text.
    ASSERT_EQ(view->reopen(QStringLiteral("utf8")), spp::EditorResult::Ok);
    EXPECT_EQ(view->breakpoints(), Lines{3});
}

namespace {

struct CompletionListener : spp::EditorViewListener {
    QList<spp::TextPosition> needed;
    void completionsNeeded(spp::TextPosition position) override { needed << position; }
};

// An editor shown with a file, the cursor at the place.
struct CompletionEditor {
    QTemporaryDir dir;
    spp::EditorView* view = plugin()->createEditor();
    std::unique_ptr<QWidget> owner = editorWidget(view);
    CompletionListener listener;
    QTreeWidget* list = view->widget()->findChild<QTreeWidget*>();

    CompletionEditor(const QByteArray& text, spp::TextPosition cursor)
    {
        view->widget()->resize(700, 400);
        view->widget()->show();
        view->open(write(dir, "code.cpp", text));
        view->setCursorPosition(cursor);
        view->setListener(&listener);
    }
    ~CompletionEditor() { view->setListener(nullptr); }

    QStringList shown() const
    {
        QStringList labels;
        for (int i = 0; i < list->topLevelItemCount(); ++i) labels << list->topLevelItem(i)->text(0);
        return labels;
    }
    bool listShown() const { return list->isVisible(); }
};

const QList<spp::CompletionItem> someCompletions{{QStringLiteral("alpha(int a)"), QStringLiteral("void"), QStringLiteral("alpha"), QStringLiteral("alpha"), {}},
                                                 {QStringLiteral("alphabet"), QStringLiteral("int"), {}, {}, {}},
                                                 {QStringLiteral("beta"), QStringLiteral("double"), {}, {}, {}},
                                                 {QStringLiteral("nsAlpha"), {}, {}, {}, {}}};

} // namespace

// Typing narrows the list, those starting with the word first; Enter writes
// the choice as one undo step.
TEST(EditorPlugin, CompletionsNarrowDownAndWriteTheChoice)
{
    CompletionEditor editor("int x = al;\nsecond\n", {0, 10});
    ASSERT_NE(editor.list, nullptr);
    spp::EditorView* view = editor.view;
    view->showCompletions(someCompletions, {0, 8});
    EXPECT_TRUE(view->isCompleting());
    EXPECT_TRUE(editor.listShown());
    EXPECT_EQ(editor.shown(), QStringList({QStringLiteral("alpha(int a)"), QStringLiteral("alphabet"), QStringLiteral("nsAlpha")}));
    type(view, Qt::Key_P, QStringLiteral("p"));
    type(view, Qt::Key_H, QStringLiteral("h"));
    type(view, Qt::Key_A, QStringLiteral("a"));
    type(view, Qt::Key_B, QStringLiteral("b"));
    EXPECT_EQ(editor.shown(), QStringList{QStringLiteral("alphabet")});
    type(view, Qt::Key_Backspace);
    EXPECT_EQ(editor.shown().size(), 3);
    type(view, Qt::Key_Down);
    type(view, Qt::Key_Return, QStringLiteral("\r"));
    EXPECT_FALSE(view->isCompleting());
    EXPECT_FALSE(editor.listShown());
    EXPECT_EQ(view->text(), QStringLiteral("int x = alphabet;\nsecond"));
    EXPECT_EQ(view->cursorPosition().column, 16);
    type(view, Qt::Key_Z, {}, Qt::ControlModifier);
    EXPECT_EQ(view->text(), QStringLiteral("int x = alpha;\nsecond"));
}

TEST(EditorPlugin, CompletionsCloseWhenNotWanted)
{
    CompletionEditor editor("ab\nsecond\n", {0, 2});
    spp::EditorView* view = editor.view;
    // Escape leaves the text as it is.
    view->showCompletions(someCompletions, {0, 0});
    EXPECT_TRUE(editor.listShown());
    type(view, Qt::Key_Escape);
    EXPECT_FALSE(view->isCompleting());
    EXPECT_EQ(view->text(), QStringLiteral("ab\nsecond"));
    // Within the word it goes on; leaving the word closes it.
    view->showCompletions(someCompletions, {0, 0});
    type(view, Qt::Key_Left);
    EXPECT_TRUE(view->isCompleting());
    view->setCursorPosition({1, 0});
    EXPECT_FALSE(view->isCompleting());
    // An answer for a word the cursor left is let go.
    view->showCompletions(someCompletions, {0, 0});
    EXPECT_FALSE(view->isCompleting());
    // A character that ends the word closes it.
    view->setCursorPosition({0, 2});
    view->showCompletions(someCompletions, {0, 0});
    EXPECT_TRUE(view->isCompleting());
    type(view, Qt::Key_ParenLeft, QStringLiteral("("));
    EXPECT_FALSE(view->isCompleting());
    // None fits, or no writing here.
    view->setCursorPosition({1, 6});
    view->showCompletions(someCompletions, {1, 0});
    EXPECT_FALSE(view->isCompleting());
    view->setReadOnly(true);
    view->setCursorPosition({0, 0});
    view->showCompletions(someCompletions, {0, 0});
    EXPECT_FALSE(view->isCompleting());
}

TEST(EditorPlugin, CompletionsAreChosenWithTabOrAClick)
{
    CompletionEditor editor("a\n", {0, 1});
    spp::EditorView* view = editor.view;
    view->showCompletions(someCompletions, {0, 0});
    type(view, Qt::Key_Tab);
    EXPECT_EQ(view->text(), QStringLiteral("alpha"));

    view->setCursorPosition({0, 5});
    view->showCompletions(someCompletions, {0, 0});
    ASSERT_TRUE(editor.listShown());
    QWidget* rows = editor.list->viewport();
    const QPoint second = editor.list->visualItemRect(editor.list->topLevelItem(1)).center();
    QMouseEvent press(QEvent::MouseButtonPress, second, rows->mapToGlobal(second), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(rows, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, second, rows->mapToGlobal(second), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(rows, &release);
    EXPECT_FALSE(view->isCompleting());
    EXPECT_EQ(view->text(), QStringLiteral("alphabet"));
}

// What it brings elsewhere goes with it, in one undo step; the cursor stays
// after the word, though lines were added above it.
TEST(EditorPlugin, CompletionsBringTheirEdits)
{
    CompletionEditor editor("line\nfoo\n", {1, 3});
    spp::EditorView* view = editor.view;
    const QList<spp::CompletionItem> withInclude{
        {QStringLiteral("food"), {}, {}, {}, {{{{0, 0}, {0, 0}}, QStringLiteral("#include <a>\n#include <b>\n")}}}};
    view->showCompletions(withInclude, {1, 0});
    type(view, Qt::Key_Return, QStringLiteral("\r"));
    EXPECT_EQ(view->text(), QStringLiteral("#include <a>\n#include <b>\nline\nfood"));
    EXPECT_EQ(view->cursorPosition().line, 3);
    EXPECT_EQ(view->cursorPosition().column, 4);
    type(view, Qt::Key_Z, {}, Qt::ControlModifier);
    EXPECT_EQ(view->text(), QStringLiteral("line\nfoo"));
}

// The program knows more: it is asked as the word grows, even with none left to show.
TEST(EditorPlugin, IncompleteCompletionsAskForMore)
{
    CompletionEditor editor("a\n", {0, 1});
    spp::EditorView* view = editor.view;
    view->showCompletions({{QStringLiteral("alpha"), {}, {}, {}, {}}}, {0, 0}, true);
    EXPECT_TRUE(editor.listener.needed.isEmpty());
    type(view, Qt::Key_Q, QStringLiteral("q"));
    EXPECT_EQ(editor.listener.needed.size(), 1);
    EXPECT_TRUE(view->isCompleting());
    EXPECT_FALSE(editor.listShown());
    view->showCompletions({{QStringLiteral("aq"), {}, {}, {}, {}}}, {0, 0}, true);
    EXPECT_TRUE(editor.listShown());
    view->hideCompletions();
    EXPECT_FALSE(view->isCompleting());
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
