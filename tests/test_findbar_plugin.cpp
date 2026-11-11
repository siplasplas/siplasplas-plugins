// SPDX-License-Identifier: LGPL-3.0-only
// The find bar on an editor of the editor plugin (offscreen): matches and
// their count, the options, stepping around the end, Replace and Replace All
// (one undo step), none in a read-only editor, and the expansion of a regular
// expression's replacement; and on a huge target that seeks itself.

#include "FindBarWidget.h"

#include "siplasplas/plugin/EditorPlugin.h"
#include "siplasplas/plugin/FindPlugin.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPluginLoader>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QToolButton>

#include <memory>

namespace {

spp::EditorPlugin* editorPlugin()
{
    static QPluginLoader loader(QStringLiteral(SPP_EDITOR_PLUGIN_FILE));
    return qobject_cast<spp::EditorPlugin*>(loader.instance());
}

// An editor with `text` and a find bar on it.
struct Fixture {
    QTemporaryDir dir;
    spp::EditorView* view = nullptr;
    std::unique_ptr<QWidget> owner;
    spp::FindBarWidget bar;

    explicit Fixture(const QByteArray& text)
    {
        const QString path = dir.filePath(QStringLiteral("text.txt"));
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) file.write(text);
        file.close();
        view = editorPlugin()->createEditor();
        owner.reset(view->widget());
        view->open(path);
        bar.setEditor(view);
    }
    ~Fixture() { bar.setEditor(nullptr); }

    void seek(const QString& text)
    {
        auto* find = bar.findChildren<QLineEdit*>().first();
        find->setText(text);
        emit find->textEdited(text);
    }
    QString count() const { return bar.findChild<QLabel*>()->text(); }
    QToolButton* option(const QString& text)
    {
        for (auto* button : bar.findChildren<QToolButton*>())
            if (button->text() == text) return button;
        return nullptr;
    }
    QString selected() const
    {
        spp::TextRange range;
        if (!view->selection(&range)) return {};
        const QString line = view->text().section(QLatin1Char('\n'), range.start.line, range.start.line);
        return line.mid(range.start.column, range.end.column - range.start.column);
    }
};

} // namespace

TEST(FindBar, CountsAndSelectsMatches)
{
    Fixture f("one Two one\nthree one two\n");
    f.bar.showFind();
    f.seek(QStringLiteral("one"));
    EXPECT_EQ(f.count(), QStringLiteral("1 of 3"));
    EXPECT_EQ(f.selected(), QStringLiteral("one"));
    f.bar.findNext();
    EXPECT_EQ(f.count(), QStringLiteral("2 of 3"));
    f.bar.findNext();
    f.bar.findNext();  // around the end
    EXPECT_EQ(f.count(), QStringLiteral("1 of 3"));
    f.bar.findPrevious();  // around the start
    EXPECT_EQ(f.count(), QStringLiteral("3 of 3"));
    f.seek(QStringLiteral("missing"));
    EXPECT_EQ(f.count(), QStringLiteral("No matches"));
}

TEST(FindBar, StopsAtTheEndWithoutWrapping)
{
    Fixture f("one one one\n");
    f.bar.showFind();
    f.option(QStringLiteral("↩"))->setChecked(false);
    f.seek(QStringLiteral("one"));
    f.bar.findNext();
    f.bar.findNext();
    EXPECT_EQ(f.count(), QStringLiteral("3 of 3"));
    f.bar.findNext();  // stays at the last one
    EXPECT_EQ(f.count(), QStringLiteral("3 of 3"));
    f.bar.findPrevious();
    f.bar.findPrevious();
    f.bar.findPrevious();  // stays at the first one
    EXPECT_EQ(f.count(), QStringLiteral("1 of 3"));
    f.option(QStringLiteral("↩"))->setChecked(true);
    f.bar.findPrevious();  // around the start
    EXPECT_EQ(f.count(), QStringLiteral("3 of 3"));
}

TEST(FindBar, Options)
{
    Fixture f("Two two twofold\n");
    f.bar.showFind();
    f.seek(QStringLiteral("two"));
    EXPECT_EQ(f.count(), QStringLiteral("1 of 3"));
    f.option(QStringLiteral("Aa"))->setChecked(true);
    EXPECT_TRUE(f.count().endsWith(QStringLiteral("of 2")));
    f.option(QStringLiteral("W"))->setChecked(true);
    EXPECT_EQ(f.count(), QStringLiteral("1 of 1"));
    f.option(QStringLiteral("Aa"))->setChecked(false);
    f.option(QStringLiteral("W"))->setChecked(false);
    f.option(QStringLiteral(".*"))->setChecked(true);
    f.seek(QStringLiteral("t\\w+d"));
    EXPECT_EQ(f.selected(), QStringLiteral("twofold"));
    f.seek(QStringLiteral("("));
    EXPECT_EQ(f.count(), QStringLiteral("Invalid"));
}

TEST(FindBar, ReplacesOneAndAll)
{
    Fixture f("a=1; b=2; c=3;\n");
    f.bar.showReplace();
    f.option(QStringLiteral(".*"))->setChecked(true);
    f.seek(QStringLiteral("(\\w)=(\\d)"));
    f.bar.findChildren<QLineEdit*>().at(1)->setText(QStringLiteral("$2=$1"));
    QPushButton* replace = nullptr;
    QPushButton* replaceAll = nullptr;
    for (auto* button : f.bar.findChildren<QPushButton*>())
        (button->text() == QStringLiteral("Replace") ? replace : replaceAll) = button;
    ASSERT_TRUE(replace && replaceAll);
    replace->click();
    EXPECT_EQ(f.view->text(), QStringLiteral("1=a; b=2; c=3;"));
    EXPECT_EQ(f.selected(), QStringLiteral("b=2"));  // the next one, to replace or skip
    replaceAll->click();
    EXPECT_EQ(f.view->text(), QStringLiteral("1=a; 2=b; 3=c;"));
    // Replace All is one undo step.
    QWidget* target = f.view->widget()->focusProxy();
    QKeyEvent undo(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
    QCoreApplication::sendEvent(target, &undo);
    EXPECT_EQ(f.view->text(), QStringLiteral("1=a; b=2; c=3;"));
}

TEST(FindBar, StartsWithTheSelectionAndCloses)
{
    Fixture f("alpha beta alpha\n");
    f.view->setSelection({{0, 6}, {0, 10}});
    f.bar.showFind();
    EXPECT_EQ(f.bar.findChildren<QLineEdit*>().first()->text(), QStringLiteral("beta"));
    EXPECT_FALSE(f.bar.isHidden());
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(f.bar.findChildren<QLineEdit*>().first(), &escape);
    EXPECT_TRUE(f.bar.isHidden());
}

TEST(FindBar, NoReplacingInAReadOnlyEditor)
{
    Fixture f("a a a\n");
    f.view->setReadOnly(true);
    f.bar.showReplace();
    QPushButton* replaceAll = nullptr;
    for (auto* button : f.bar.findChildren<QPushButton*>())
        if (button->text() == QStringLiteral("Replace All")) replaceAll = button;
    ASSERT_NE(replaceAll, nullptr);
    EXPECT_TRUE(replaceAll->parentWidget()->isHidden());
    f.seek(QStringLiteral("a"));
    EXPECT_EQ(f.count(), QStringLiteral("1 of 3"));  // finding still works
    replaceAll->click();
    EXPECT_EQ(f.view->text(), QStringLiteral("a a a"));
    // Writable again: the next showReplace() has the row.
    f.view->setReadOnly(false);
    f.bar.showReplace();
    EXPECT_FALSE(replaceAll->parentWidget()->isHidden());
}

namespace {

// A huge target whose search the test ends by hand.
struct HugeTarget : spp::FindTarget {
    QWidget view;
    spp::FindTargetListener* listener = nullptr;
    spp::FindQuery query;
    int finds = 0;
    bool forward = false;
    int cancels = 0;

    QWidget* widget() override { return &view; }
    bool canReplace() const override { return false; }
    bool isHuge() const override { return true; }
    QString selectedText() const override { return QStringLiteral("seed"); }
    void setQuery(const spp::FindQuery& q) override { query = q; }
    void find(const spp::FindQuery& q, bool fwd) override
    {
        query = q;
        forward = fwd;
        ++finds;
    }
    void cancelFind() override { ++cancels; }
    void setFindListener(spp::FindTargetListener* l) override { listener = l; }
};

} // namespace

TEST(FindBar, HugeTargetSeeksItselfWithProgress)
{
    HugeTarget target;
    spp::FindBarWidget bar;
    bar.setTarget(&target);
    ASSERT_NE(target.listener, nullptr);
    auto* find = bar.findChildren<QLineEdit*>().first();
    auto* label = bar.findChild<QLabel*>();
    auto* progress = bar.findChild<QProgressBar*>();

    // Opening seeds the text and gives it to the target to highlight, but
    // does not seek; no Replace row for a read-only target.
    bar.showReplace();
    EXPECT_EQ(find->text(), QStringLiteral("seed"));
    EXPECT_EQ(target.query.text, QStringLiteral("seed"));
    EXPECT_EQ(target.finds, 0);
    for (auto* button : bar.findChildren<QPushButton*>())
        EXPECT_TRUE(button->parentWidget()->isHidden());

    // Typing only highlights.
    find->setText(QStringLiteral("abc"));
    emit find->textEdited(find->text());
    EXPECT_EQ(target.query.text, QStringLiteral("abc"));
    EXPECT_EQ(target.finds, 0);

    // Seeking: progress instead of the count, then the result.
    bar.findNext();
    EXPECT_EQ(target.finds, 1);
    EXPECT_TRUE(target.forward);
    EXPECT_TRUE(target.query.wrap);
    EXPECT_FALSE(progress->isHidden());
    EXPECT_TRUE(label->isHidden());
    target.listener->findProgress(50, 100);
    EXPECT_EQ(progress->value(), progress->maximum() / 2);
    target.listener->findFinished(spp::FindResult::Wrapped);
    EXPECT_TRUE(progress->isHidden());
    EXPECT_EQ(label->text(), QStringLiteral("Wrapped"));
    for (auto* button : bar.findChildren<QToolButton*>())
        if (button->text() == QStringLiteral("↩")) button->setChecked(false);
    bar.findPrevious();
    EXPECT_FALSE(target.forward);
    EXPECT_FALSE(target.query.wrap);
    target.listener->findFinished(spp::FindResult::NotFound);
    EXPECT_EQ(label->text(), QStringLiteral("No matches"));

    // The first Escape cancels a search, a late end of it is let go; the
    // second closes the bar and removes the highlights.
    bar.findNext();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(find, &escape);
    EXPECT_EQ(target.cancels, 1);
    EXPECT_EQ(label->text(), QStringLiteral("Cancelled"));
    target.listener->findFinished(spp::FindResult::Found);
    EXPECT_EQ(label->text(), QStringLiteral("Cancelled"));
    EXPECT_FALSE(bar.isHidden());
    QCoreApplication::sendEvent(find, &escape);
    EXPECT_TRUE(bar.isHidden());
    EXPECT_TRUE(target.query.text.isEmpty());

    // An invalid pattern is not sought.
    bar.showFind();
    for (auto* button : bar.findChildren<QToolButton*>())
        if (button->text() == QStringLiteral(".*")) button->setChecked(true);
    find->setText(QStringLiteral("("));
    emit find->textEdited(find->text());
    EXPECT_EQ(label->text(), QStringLiteral("Invalid"));
    const int finds = target.finds;
    bar.findNext();
    EXPECT_EQ(target.finds, finds);

    bar.setTarget(nullptr);
    EXPECT_EQ(target.listener, nullptr);
}

TEST(FindBar, ExpandsReplacements)
{
    const QStringList captures{QStringLiteral("a=1"), QStringLiteral("a"), QStringLiteral("1")};
    EXPECT_EQ(spp::FindBarWidget::expand(QStringLiteral("$2=$1"), captures), QStringLiteral("1=a"));
    EXPECT_EQ(spp::FindBarWidget::expand(QStringLiteral("\\1-\\0"), captures), QStringLiteral("a-a=1"));
    EXPECT_EQ(spp::FindBarWidget::expand(QStringLiteral("$$ \\\\ \\n\\t"), captures), QStringLiteral("$ \\ \n\t"));
    EXPECT_EQ(spp::FindBarWidget::expand(QStringLiteral("$9"), captures), QString());
}

TEST(FindBar, PluginLoads)
{
    QPluginLoader loader(QStringLiteral(SPP_FINDBAR_PLUGIN_FILE));
    auto* plugin = qobject_cast<spp::FindPlugin*>(loader.instance());
    ASSERT_NE(plugin, nullptr);
    std::unique_ptr<QWidget> bar(plugin->createFindBar()->widget());
    EXPECT_TRUE(bar->isHidden());
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
