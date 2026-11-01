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

#include <QString>
#include <QStringList>
#include <QtPlugin>

class QWidget;

namespace spp {

// A place in the text: line and column from 0; the column counts UTF-16
// code units (QString indexes) in the line.
struct TextPosition {
    int line = 0;
    int column = 0;
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

#define SPP_EDITOR_PLUGIN_IID "io.github.siplasplas.plugins.EditorPlugin/1"
Q_DECLARE_INTERFACE(spp::EditorPlugin, SPP_EDITOR_PLUGIN_IID)

#endif // SIPLASPLAS_PLUGIN_EDITORPLUGIN_H
