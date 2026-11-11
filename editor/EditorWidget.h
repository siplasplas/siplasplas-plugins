// SPDX-License-Identifier: LGPL-3.0-only
#ifndef SPP_EDITOR_EDITORWIDGET_H
#define SPP_EDITOR_EDITORWIDGET_H

#include "siplasplas/plugin/EditorPlugin.h"
#include "siplasplas/plugin/FindPlugin.h"

#include <QWidget>

#include <functional>
#include <memory>

namespace qce {
class CodeEdit;
class FoldingGutter;
class LineNumberGutter;
class SimpleTextDocument;
namespace encoding { class TextFile; }
namespace kate { class EditorSyntax; }
}

namespace spp {

class BreakpointGutter;
class CompletionPopup;

// The editor of the plugin: qcodeedit's CodeEdit with line numbers and
// folding, the file read and saved by qce::encoding::TextFile and coloured by
// qce::kate::EditorSyntax.
// It is also its own find target: text(), selection(), setSelection(),
// setHighlights(), replace() and widget() serve both interfaces.
class EditorWidget : public QWidget, public EditorView, public FindTarget {
    Q_OBJECT
public:
    explicit EditorWidget(QWidget* parent = nullptr);
    ~EditorWidget() override;

    QWidget* widget() override { return this; }

    FindTarget* findTarget() override { return this; }
    bool canReplace() const override { return !isReadOnly(); }
    bool isHuge() const override { return false; }
    QString selectedText() const override;
    int rowHeight() const override;
    QRect coverableRect() const override;
    int textTop() const override;
    int firstVisibleRow() const override;
    void setFirstVisibleRow(int row) override;
    int rowOf(TextPosition position) const override;

    EditorResult open(const QString& path, const QString& encoding) override;
    EditorResult reopen(const QString& encoding) override;
    EditorResult reload() override;
    EditorResult save() override;
    EditorResult saveAs(const QString& path) override;
    QString errorString() const override;

    QString filePath() const override;
    bool isModified() const override;
    bool hasExternalChanges() const override;
    void ignoreExternalChanges() override;
    QString encoding() const override;
    QStringList uncertainEncodings() const override;

    void setReadOnly(bool readOnly) override;
    bool isReadOnly() const override;
    void setWordWrap(bool wrap) override;
    bool wordWrap() const override;

    TextPosition cursorPosition() const override;
    void setCursorPosition(TextPosition position) override;
    QString text() const override;
    void setMarks(const QList<TextMark>& marks) override;
    void answerLink(TextPosition position, bool isLink) override;
    bool selection(TextRange* range) const override;
    void setSelection(TextRange range) override;
    void setHighlights(const QList<TextRange>& ranges) override;
    void setScopeHighlights(const QList<TextRange>& ranges) override;
    bool replace(const QList<TextReplacement>& replacements, const QString& undoText) override;
    void setBreakpointMargin(bool shown) override;
    bool hasBreakpointMargin() const override;
    void setBreakpoints(const QList<int>& lines) override;
    QList<int> breakpoints() const override;
    void setExecutionLine(int line) override;
    int executionLine() const override;
    void showCompletions(const QList<CompletionItem>& items, TextPosition start, bool incomplete) override;
    void hideCompletions() override;
    bool isCompleting() const override { return m_completing; }
    void answerHover(TextPosition position, const QString& markdown) override;

    void setListener(EditorViewListener* listener) override { m_listener = listener; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // Colours the text by the file's name (after it was opened or renamed).
    void applySyntax();
    // The word at `viewportPoint` as a link while Ctrl is held; false when
    // there is no word there. linkAt() only finds it.
    bool linkAt(const QPoint& viewportPoint, TextPosition* start, TextPosition* end) const;
    void showLink(const QPoint& viewportPoint, bool ctrl);
    void displayLink();
    void hideLink();
    // The breakpoints' and the execution line's column and backgrounds again.
    void repaintBreakpoints();
    void toggleBreakpoint(int line);
    // Keeps breakpoints and the execution line on their code through an edit.
    void linesInserted(int start, int count);
    void linesRemoved(int start, int count);
    // Each line through `moved`: its new line, or -1 for one that is gone.
    void moveLines(const std::function<int(int)>& moved);
    // What was typed of the word being completed; false when the cursor is
    // not in it any more.
    bool completionWord(QString* typed) const;
    // The list for the word as it is now, or the end of completing.
    void updateCompletions();
    void placeCompletions();
    void acceptCompletion();
    // The list's keys; true when it took the key.
    bool completionKey(int key);
    // The mouse rests at `point` of the viewport: the marks' messages there
    // now, and the listener asked about the word.
    void showHover(const QPoint& point, const QPoint& globalPoint);
    // Tooltip shown under the mouse until it moves away from there.
    void showHoverText(const QString& html);

    qce::SimpleTextDocument* m_document = nullptr;
    qce::CodeEdit* m_edit = nullptr;
    qce::encoding::TextFile* m_file = nullptr;
    std::unique_ptr<qce::LineNumberGutter> m_lineNumbers;
    std::unique_ptr<BreakpointGutter> m_breakpoints;
    std::unique_ptr<qce::FoldingGutter> m_foldGutter;
    // Goes before m_edit, which it detaches from.
    std::unique_ptr<qce::kate::EditorSyntax> m_syntax;
    EditorViewListener* m_listener = nullptr;
    bool m_wordWrap = true;
    // The marks underlined now (an edit clears them), for their tooltips.
    QList<TextMark> m_marks;
    bool m_linkShown = false;
    // The word under the mouse asked about with Ctrl held, and the answer.
    struct LinkQuery {
        bool asked = false;
        bool isLink = false;
        TextPosition start;
        TextPosition end;
    } m_query;
    bool m_querying = false;  // in the listener's linkQueried()
    // The word the mouse rests on, asked about, and what shows of it.
    struct Hover {
        bool asked = false;
        TextPosition start;
        TextPosition end;
        QPoint point;
        QPoint globalPoint;
        QStringList messages;  // of the marks there, as HTML
    } m_hover;
    CompletionPopup* m_completionPopup = nullptr;
    QList<CompletionItem> m_completions;
    TextPosition m_completionStart;
    QString m_completionTyped;
    bool m_completionIncomplete = false;
    bool m_completing = false;
};

} // namespace spp

#endif // SPP_EDITOR_EDITORWIDGET_H
