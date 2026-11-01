// SPDX-License-Identifier: LGPL-3.0-only
#include "EditorWidget.h"

#include <qce/CodeEdit.h>
#include <qce/CodeEditArea.h>
#include <qce/SimpleTextDocument.h>
#include <qce/diff/LineDiff.h>
#include <qce/encoding/TextFile.h>
#include <qce/kate/EditorSyntax.h>
#include <qce/margins/FoldingGutter.h>
#include <qce/margins/LineNumberGutter.h>

#include <QVBoxLayout>

#include <algorithm>

namespace spp {
namespace {

EditorResult toResult(qce::encoding::TextFile::Result result)
{
    switch (result) {
    case qce::encoding::TextFile::Result::Ok: return EditorResult::Ok;
    case qce::encoding::TextFile::Result::Cancelled: return EditorResult::Cancelled;
    case qce::encoding::TextFile::Result::Failed: break;
    }
    return EditorResult::Failed;
}

} // namespace

EditorWidget::EditorWidget(QWidget* parent) : QWidget(parent)
{
    // A reload changes only the lines that differ, as one undo step.
    qce::diff::install();

    m_document = new qce::SimpleTextDocument(this);
    m_edit = new qce::CodeEdit(this);
    m_edit->setDocument(m_document);

    m_lineNumbers = std::make_unique<qce::LineNumberGutter>(m_document);
    m_lineNumbers->setFont(m_edit->area()->font());
    m_edit->addLeftMargin(m_lineNumbers.get());
    m_foldGutter = std::make_unique<qce::FoldingGutter>(
        &m_edit->area()->foldState(), [this](int line) { m_edit->area()->toggleFoldAt(line); });
    m_edit->addLeftMargin(m_foldGutter.get());

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_edit);
    setFocusProxy(m_edit->area());

    m_file = new qce::encoding::TextFile(m_edit->area(), m_document, this);
    m_file->setTrimWhitespace(qce::encoding::TextFile::TrimWhitespace::ChangedLines);
    connect(m_file, &qce::encoding::TextFile::modificationChanged, this, [this](bool modified) {
        if (m_listener) m_listener->modificationChanged(modified);
    });
    connect(m_file, &qce::encoding::TextFile::pathChanged, this, [this](const QString& path) {
        applySyntax();
        if (m_listener) m_listener->filePathChanged(path);
    });
    connect(m_file, &qce::encoding::TextFile::formatChanged, this, [this] {
        if (m_listener) m_listener->formatChanged();
    });
    connect(m_edit->area(), &qce::CodeEditArea::cursorPositionChanged, this, [this](qce::TextCursor cursor) {
        if (m_listener) m_listener->cursorMoved({cursor.line, cursor.column});
    });

    // The default theme for the desktop until a file chooses a syntax.
    m_syntax = std::make_unique<qce::kate::EditorSyntax>(m_edit);
    m_edit->area()->setWordWrap(m_wordWrap);
}

// The text file and the syntax let go of the editor while it is whole.
EditorWidget::~EditorWidget()
{
    disconnect(m_file, nullptr, this, nullptr);
    disconnect(m_edit->area(), nullptr, this, nullptr);
    m_syntax.reset();
}

void EditorWidget::applySyntax()
{
    m_syntax->setFileName(m_file->path());
    m_syntax->apply();
}

EditorResult EditorWidget::open(const QString& path, const QString& encoding)
{
    return toResult(m_file->open(path, encoding));
}

EditorResult EditorWidget::reopen(const QString& encoding)
{
    return toResult(m_file->reopen(encoding));
}

EditorResult EditorWidget::reload()
{
    return toResult(m_file->reload());
}

EditorResult EditorWidget::save()
{
    return toResult(m_file->save());
}

EditorResult EditorWidget::saveAs(const QString& path)
{
    return toResult(m_file->saveAs(path));
}

QString EditorWidget::errorString() const
{
    return m_file->errorString();
}

QString EditorWidget::filePath() const
{
    return m_file->path();
}

bool EditorWidget::isModified() const
{
    return m_file->isModified();
}

bool EditorWidget::hasExternalChanges() const
{
    return m_file->hasExternalChanges();
}

void EditorWidget::ignoreExternalChanges()
{
    m_file->ignoreExternalChanges();
}

QString EditorWidget::encoding() const
{
    return m_file->encoding();
}

QStringList EditorWidget::uncertainEncodings() const
{
    QStringList encodings;
    for (const auto& option : m_file->uncertainDetection().options) encodings << option.encoding;
    return encodings;
}

void EditorWidget::setReadOnly(bool readOnly)
{
    m_edit->area()->setReadOnly(readOnly);
}

bool EditorWidget::isReadOnly() const
{
    return m_edit->area()->readOnly();
}

void EditorWidget::setWordWrap(bool wrap)
{
    m_wordWrap = wrap;
    m_edit->area()->setWordWrap(wrap);
}

bool EditorWidget::wordWrap() const
{
    return m_wordWrap;
}

TextPosition EditorWidget::cursorPosition() const
{
    const qce::TextCursor cursor = m_edit->area()->cursorPosition();
    return {cursor.line, cursor.column};
}

void EditorWidget::setCursorPosition(TextPosition position)
{
    const int line = std::clamp(position.line, 0, std::max(0, m_document->lineCount() - 1));
    const int column = std::clamp(position.column, 0, int(m_document->lineAt(line).size()));
    m_edit->area()->setCursorPosition({line, column});
}

QString EditorWidget::text() const
{
    return m_document->toPlainText();
}

} // namespace spp
