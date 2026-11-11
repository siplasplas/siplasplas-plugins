// SPDX-License-Identifier: LGPL-3.0-only
#ifndef SIPLASPLAS_PLUGIN_FINDPLUGIN_H
#define SIPLASPLAS_PLUGIN_FINDPLUGIN_H

// Interface of the find plugin: a bar searching (and replacing) in a
// FindTarget through its interface only — plain text, whole words, regular
// expressions. The program puts the bar into its window and tells it which
// target it works on: an editor of the editor plugin (EditorView::
// findTarget()) or anything else implementing FindTarget, e.g. a viewer of
// files of any size. Like the editor, it has state and is used on the GUI
// thread.
//
// A target has two traits, independent of each other:
// - canReplace(): false for a read-only one; the bar has no Replace then.
// - isHuge(): its text is too large to hold as a QString (a file of
//   gigabytes). Then the target seeks itself, in the background with
//   progress, one match at a time, and highlights the matches it shows;
//   the bar knows no count of them. Otherwise the bar seeks in text(),
//   highlights every match and says "3 of 17".

#include "siplasplas/plugin/EditorPlugin.h"

#include <QString>
#include <QtPlugin>

class QWidget;

namespace spp {

// What is sought, as set in the bar.
struct FindQuery {
    QString text;  // empty: nothing
    bool matchCase = false;
    bool wholeWords = false;
    bool regex = false;  // Perl-like, as QRegularExpression
};

// How a huge target's search ended.
enum class FindResult {
    Found,      // the match is selected and shown
    Wrapped,    // found as well, after going around the end (or start)
    NotFound,
    Invalid,    // the target cannot seek this pattern
    Cancelled,  // by cancelFind()
};

// Told about a huge target's search: the find bar.
class FindTargetListener {
public:
    virtual ~FindTargetListener() = default;
    // `done` of `total` (e.g. bytes) sought so far.
    virtual void findProgress(qint64 done, qint64 total) { (void)done; (void)total; }
    // The search of FindTarget::find() ended (also at once, from find()).
    virtual void findFinished(FindResult result) = 0;
};

// What the find bar searches in.
class FindTarget {
public:
    virtual ~FindTarget() = default;

    // The target's widget: it gets the focus back when the bar closes.
    virtual QWidget* widget() = 0;
    // The text can be replaced (not read-only).
    virtual bool canReplace() const = 0;
    // Too large for text(): the target seeks itself (find()).
    virtual bool isHuge() const = 0;

    // The selected text within one line, empty without one: what is sought
    // when the bar opens.
    virtual QString selectedText() const = 0;

    // --- Targets that are not huge: the bar seeks in their text. ---

    // The whole text, lines joined by "\n".
    virtual QString text() const { return {}; }
    // The selected range, start before end; false (and the cursor's empty
    // range) without a selection.
    virtual bool selection(TextRange* range) const
    {
        *range = {};
        return false;
    }
    // Selects the range and scrolls to it.
    virtual void setSelection(TextRange range) { (void)range; }
    // Highlights the matches, replacing those set before.
    virtual void setHighlights(const QList<TextRange>& ranges) { (void)ranges; }
    // Replaces the ranges (in the order of the text, not overlapping) as one
    // undo step named `undoText`; false, doing nothing, when it cannot.
    virtual bool replace(const QList<TextReplacement>& replacements, const QString& undoText)
    {
        (void)replacements;
        (void)undoText;
        return false;
    }

    // --- Huge targets: they seek themselves. ---

    // Highlights the matches of `query` in the part shown, also as it
    // scrolls; an empty text removes them.
    virtual void setQuery(const FindQuery& query) { (void)query; }
    // Seeks the next (or previous) match after (before) the selected one, or
    // from the top of what is shown, around the end, and tells the listener
    // of the progress and of the end: a match found is selected and shown.
    // A search still running is cancelled first.
    virtual void find(const FindQuery& query, bool forward)
    {
        (void)query;
        (void)forward;
    }
    virtual void cancelFind() {}
    // The listener is not owned; nullptr removes it.
    virtual void setFindListener(FindTargetListener* listener) { (void)listener; }
};

class FindBar {
public:
    virtual ~FindBar() = default;

    // The bar's widget, hidden at first. Deleting it deletes the FindBar.
    virtual QWidget* widget() = 0;

    // The target searched (not owned; nullptr: none). A program with tabs
    // gives the current tab's target, and nullptr before that target goes.
    virtual void setTarget(FindTarget* target) = 0;
    // The same for an editor: setTarget(editor->findTarget()).
    virtual void setEditor(EditorView* editor) = 0;

    // Shows the bar for finding, or also for replacing (finding alone for a
    // target that cannot replace), with the target's selected text as the
    // text sought, and focuses it.
    virtual void showFind() = 0;
    virtual void showReplace() = 0;
    // The next or previous match from the cursor, around the end.
    virtual void findNext() = 0;
    virtual void findPrevious() = 0;

    // The target's text changed (for an editor
    // EditorViewListener::textChanged()): the matches are sought again.
    virtual void editorTextChanged() = 0;
};

class FindPlugin {
public:
    virtual ~FindPlugin() = default;

    // A short name for diagnostics.
    virtual QString name() const = 0;

    // A new bar, a child of `parent` when given.
    virtual FindBar* createFindBar(QWidget* parent = nullptr) = 0;
};

} // namespace spp

#define SPP_FIND_PLUGIN_IID "io.github.siplasplas.plugins.FindPlugin/2"
Q_DECLARE_INTERFACE(spp::FindPlugin, SPP_FIND_PLUGIN_IID)

#endif // SIPLASPLAS_PLUGIN_FINDPLUGIN_H
