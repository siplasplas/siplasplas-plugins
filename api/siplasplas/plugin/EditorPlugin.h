// SPDX-License-Identifier: LGPL-3.0-only
#ifndef SIPLASPLAS_PLUGIN_EDITORPLUGIN_H
#define SIPLASPLAS_PLUGIN_EDITORPLUGIN_H

// Interface of the editor plugin: a code editor widget (qcodeedit) that a
// program puts into its own window, tabs and menus, so that programs
// (eudaimonia, ducat, gemini-commander) do not link the editor libraries.
//
// Unlike the other plugins, an editor has state: each EditorView is a widget
// with its document, used on the GUI thread only. Only Qt and standard types
// cross the boundary.
//
// The editor reads a file in its encoding (detected, or given: legacy code
// pages and UTF), saves it back the same way with its BOM and line breaks,
// colours it with the Kate syntax definitions shared by the programs using
// qcodeedit (~/.local/share/kate-data) and the Kate theme matching the
// desktop. A reload changes only the lines that differ, as one undo step.

#include <QList>
#include <QString>
#include <QStringList>
#include <QtPlugin>

class QWidget;

namespace spp {

class FindTarget;

// A place in the text: line and column from 0; the column counts UTF-16
// code units (QString indexes) in the line.
struct TextPosition {
    int line = 0;
    int column = 0;
};

// A range of the text: from `start` to just before `end`.
struct TextRange {
    TextPosition start;
    TextPosition end;
};

// A range of the text and what replaces it.
struct TextReplacement {
    TextRange range;
    QString text;
};

// A range of the text marked with a message, e.g. a language server's
// diagnostic: underlined, its message shown when the mouse rests on it.
struct TextMark {
    enum class Kind { Error, Warning, Information, Hint };
    TextPosition start;
    TextPosition end;  // past the range; equal to start marks one character
    Kind kind = Kind::Error;
    QString message;
};

// What can be written at the cursor, e.g. from a language server.
struct CompletionItem {
    QString label;       // shown
    QString detail;      // shown beside it, e.g. a type
    QString filterText;  // what typing is matched with; empty: the label
    QString insertText;  // written instead of the word; empty: the label
    // Elsewhere in the text, e.g. the #include it needs: not overlapping the
    // word nor each other.
    QList<TextReplacement> additionalEdits;
};

// Result of reading or saving a file.
enum class EditorResult {
    Ok,
    Failed,    // errorString() tells why
    Cancelled  // by the user, e.g. in the question about characters the
               // file's code page cannot store; nothing to report
};

// Told about an EditorView's changes; the program implements the methods it
// needs. Called on the GUI thread, after the change.
class EditorViewListener {
public:
    virtual ~EditorViewListener() = default;
    // The text differs from the file now, or no longer.
    virtual void modificationChanged(bool modified) { (void)modified; }
    virtual void cursorMoved(TextPosition position) { (void)position; }
    // Another file: opened, or saved under another name.
    virtual void filePathChanged(const QString& path) { (void)path; }
    // Encoding or line breaks changed, e.g. for a status bar.
    virtual void formatChanged() {}
    // The text changed: an edit, undo or redo, or a file read.
    virtual void textChanged() {}
    // Ctrl is held with the mouse over the word at `position`: is it a link
    // (e.g. a symbol whose definition a language server knows)? Answered by
    // EditorView::answerLink(), now or later; unanswered, it is no link.
    virtual void linkQueried(TextPosition position) { (void)position; }
    // Ctrl+click on a word shown as a link: the cursor is at `position` now,
    // e.g. for Go to Definition.
    virtual void linkActivated(TextPosition position) { (void)position; }
    // The breakpoints are these lines now, in ascending order: a click in
    // their margin, or an edit that moved them with their code. Not called
    // for EditorView::setBreakpoints().
    virtual void breakpointsChanged(const QList<int>& lines) { (void)lines; }
    // The word typed further, with completions shown as incomplete: more of
    // them may fit it now, for showCompletions() again.
    virtual void completionsNeeded(TextPosition position) { (void)position; }
    // The mouse rests on the word at `position`: is there something to say
    // of it (e.g. a language server's description)? Answered by
    // EditorView::answerHover(), now or later; unanswered, nothing is shown.
    virtual void hoverQueried(TextPosition position) { (void)position; }
};

// One editor: a widget with a document and the file behind it.
class EditorView {
public:
    virtual ~EditorView() = default;

    // The editor's widget, to put into a layout or tab. Deleting it deletes
    // the EditorView as well.
    virtual QWidget* widget() = 0;

    // Opens a file with a new undo history; an empty `encoding` detects it
    // (cpg names: "utf8", "cp1250", ...). A path that does not exist yet
    // gives an empty UTF-8 document to be saved there.
    virtual EditorResult open(const QString& path, const QString& encoding = {}) = 0;
    // Reads the file again in another encoding, with a new undo history.
    virtual EditorResult reopen(const QString& encoding) = 0;
    // Reads the file again in its encoding; the change is one undo step.
    virtual EditorResult reload() = 0;
    virtual EditorResult save() = 0;
    // Saves to `path` (atomically where possible); the file becomes `path`.
    virtual EditorResult saveAs(const QString& path) = 0;
    // Why the last call failed.
    virtual QString errorString() const = 0;

    // Empty for a document never opened or saved.
    virtual QString filePath() const = 0;
    // The text differs from the file (undoing back to it is no change).
    virtual bool isModified() const = 0;
    // The file changed on disk since it was read or saved.
    virtual bool hasExternalChanges() const = 0;
    // Takes the file as it is on disk now as the known version.
    virtual void ignoreExternalChanges() = 0;
    // The encoding the file is saved in (cpg name).
    virtual QString encoding() const = 0;
    // When the encoding was detected without confidence on opening: the
    // plausible encodings, best first (the file is shown in the first);
    // empty otherwise.
    virtual QStringList uncertainEncodings() const = 0;

    virtual void setReadOnly(bool readOnly) = 0;
    virtual bool isReadOnly() const = 0;
    // Long lines wrap at the window's width; on by default.
    virtual void setWordWrap(bool wrap) = 0;
    virtual bool wordWrap() const = 0;

    virtual TextPosition cursorPosition() const = 0;
    // Moves the cursor (clamped to the text) and scrolls to it.
    virtual void setCursorPosition(TextPosition position) = 0;
    // The whole text, lines joined by "\n".
    virtual QString text() const = 0;

    // The selected range, start before end; false (and the cursor's empty
    // range) without a selection.
    virtual bool selection(TextRange* range) const = 0;
    // Selects the range, the cursor at its end, and scrolls to it.
    virtual void setSelection(TextRange range) = 0;
    // Highlights the ranges, e.g. a search's matches, replacing those set
    // before; an edit removes them until they are set again.
    virtual void setHighlights(const QList<TextRange>& ranges) = 0;
    // Replaces the ranges (in the order of the text, not overlapping) as one
    // undo step named `undoText`; false, doing nothing, in a read-only view
    // or with nothing to replace.
    virtual bool replace(const QList<TextReplacement>& replacements, const QString& undoText) = 0;
    // The editor as the find plugin's target (siplasplas/plugin/
    // FindPlugin.h): never huge, replacing unless read-only. Lives as long as
    // the view.
    virtual FindTarget* findTarget() = 0;

    // Underlines the ranges (errors red, warnings amber, information blue,
    // hints grey) and shows their messages when the mouse rests on them;
    // replaces the marks set before. An edit removes them all, until they
    // are set again for the new text.
    virtual void setMarks(const QList<TextMark>& marks) = 0;

    // The answer to EditorViewListener::linkQueried(position): the word
    // there is a link, shown as one (blue, underlined, a pointing hand) while
    // Ctrl is held over it, or it is none. An answer for a word the mouse has
    // left, or after an edit, is let go.
    virtual void answerLink(TextPosition position, bool isLink) = 0;

    // A column beside the line numbers for a program with a debugger, hidden
    // by default: a click toggles a breakpoint on the line. While it is
    // hidden neither breakpoints nor the execution line are drawn.
    virtual void setBreakpointMargin(bool shown) = 0;
    virtual bool hasBreakpointMargin() const = 0;
    // Breakpoints on these lines (from 0): a red dot and a pale red line;
    // replaces those set before. They stay on their code through edits and
    // reloads, and opening a file removes them.
    virtual void setBreakpoints(const QList<int>& lines) = 0;
    virtual QList<int> breakpoints() const = 0;
    // The line where the debugger stopped, -1 for none: an arrow and a blue
    // line. It does not scroll there: setCursorPosition() does. It stays on
    // its code through edits, and opening a file removes it.
    virtual void setExecutionLine(int line) = 0;
    virtual int executionLine() const = 0;

    // A list of completions at the cursor, for the word from `start` to the
    // cursor; let go when the cursor left that word in the meantime or the
    // view is read-only. Typing narrows the list down, leaving the word closes
    // it. Up, Down, Page Up and Page Down choose; Enter, Tab or a click write
    // the choice as one undo step; Escape closes it. `incomplete`: the program
    // knows more than these, and completionsNeeded() asks as the word grows.
    virtual void showCompletions(const QList<CompletionItem>& items, TextPosition start, bool incomplete = false) = 0;
    virtual void hideCompletions() = 0;
    virtual bool isCompleting() const = 0;

    // The answer to EditorViewListener::hoverQueried(position): Markdown shown
    // under the mouse, after the messages of the marks there. Empty, or for a
    // word the mouse has left, or after an edit, it is let go.
    virtual void answerHover(TextPosition position, const QString& markdown) = 0;

    // The listener is not owned and must outlive the view or be removed
    // (nullptr); one listener at a time.
    virtual void setListener(EditorViewListener* listener) = 0;
};

class EditorPlugin {
public:
    virtual ~EditorPlugin() = default;

    // A short name for diagnostics.
    virtual QString name() const = 0;

    // A new editor with an empty document. Its widget is a child of
    // `parent` when given; otherwise the caller owns it.
    virtual EditorView* createEditor(QWidget* parent = nullptr) = 0;
};

} // namespace spp

#define SPP_EDITOR_PLUGIN_IID "io.github.siplasplas.plugins.EditorPlugin/9"
Q_DECLARE_INTERFACE(spp::EditorPlugin, SPP_EDITOR_PLUGIN_IID)

#endif // SIPLASPLAS_PLUGIN_EDITORPLUGIN_H
