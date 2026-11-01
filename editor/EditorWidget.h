// SPDX-License-Identifier: LGPL-3.0-only
#ifndef SPP_EDITOR_EDITORWIDGET_H
#define SPP_EDITOR_EDITORWIDGET_H

#include "siplasplas/plugin/EditorPlugin.h"

#include <QWidget>

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

// The editor of the plugin: qcodeedit's CodeEdit with line numbers and
// folding, the file read and saved by qce::encoding::TextFile and coloured by
// qce::kate::EditorSyntax.
class EditorWidget : public QWidget, public EditorView {
    Q_OBJECT
public:
    explicit EditorWidget(QWidget* parent = nullptr);
    ~EditorWidget() override;

    QWidget* widget() override { return this; }

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
    void hideLink();

    qce::SimpleTextDocument* m_document = nullptr;
    qce::CodeEdit* m_edit = nullptr;
    qce::encoding::TextFile* m_file = nullptr;
    std::unique_ptr<qce::LineNumberGutter> m_lineNumbers;
    std::unique_ptr<qce::FoldingGutter> m_foldGutter;
    // Goes before m_edit, which it detaches from.
    std::unique_ptr<qce::kate::EditorSyntax> m_syntax;
    EditorViewListener* m_listener = nullptr;
    bool m_wordWrap = true;
    // The marks underlined now (an edit clears them), for their tooltips.
    QList<TextMark> m_marks;
    bool m_linkShown = false;
};

} // namespace spp

#endif // SPP_EDITOR_EDITORWIDGET_H
