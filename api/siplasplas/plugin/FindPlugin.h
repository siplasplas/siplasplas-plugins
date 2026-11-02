// SPDX-License-Identifier: LGPL-3.0-only
#ifndef SIPLASPLAS_PLUGIN_FINDPLUGIN_H
#define SIPLASPLAS_PLUGIN_FINDPLUGIN_H

// Interface of the find plugin: a bar searching and replacing in an
// spp::EditorView (the editor plugin's) through its interface only — plain
// text, whole words, regular expressions, all matches highlighted, Replace
// and Replace All as one undo step. The program puts the bar into its window
// and tells it which editor it works on. Like the editor, it has state and is
// used on the GUI thread.

#include "siplasplas/plugin/EditorPlugin.h"

#include <QString>
#include <QtPlugin>

class QWidget;

namespace spp {

class FindBar {
public:
    virtual ~FindBar() = default;

    // The bar's widget, hidden at first. Deleting it deletes the FindBar.
    virtual QWidget* widget() = 0;

    // The editor searched (not owned; nullptr: none). A program with tabs
    // gives the current tab's editor, and nullptr before that editor goes.
    virtual void setEditor(EditorView* editor) = 0;

    // Shows the bar for finding, or also for replacing, with the editor's
    // selection (within one line) as the text sought, and focuses it.
    virtual void showFind() = 0;
    virtual void showReplace() = 0;
    // The next or previous match from the cursor, around the end.
    virtual void findNext() = 0;
    virtual void findPrevious() = 0;

    // The editor's text changed (EditorViewListener::textChanged()): the
    // matches are sought again.
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

#define SPP_FIND_PLUGIN_IID "io.github.siplasplas.plugins.FindPlugin/1"
Q_DECLARE_INTERFACE(spp::FindPlugin, SPP_FIND_PLUGIN_IID)

#endif // SIPLASPLAS_PLUGIN_FINDPLUGIN_H
