// SPDX-License-Identifier: LGPL-3.0-only
// The find bar on an editor of the editor plugin (offscreen): matches and
// their count, the options, stepping around the end, Replace and Replace All
// (one undo step), and the expansion of a regular expression's replacement.

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
