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
#include <qce/WordBoundary.h>

#include <QCursor>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>

namespace spp {
namespace {

// The area's layers of the marks and of the link under the mouse, beside
// qcodeedit-spell's (100, 101).
constexpr int MarksLayer = 110;
constexpr int LinkLayer = 111;
constexpr int HighlightsLayer = 112;

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
    // The area clears its decorations on every change of the text; the
    // marks' messages go with them.
    connect(m_edit->area(), &qce::CodeEditArea::textChanged, this, [this] {
        m_marks.clear();
        m_linkShown = false;
        m_query = {};  // an answer was for the text before
        if (m_listener) m_listener->textChanged();
    });
    // The messages of the marks under the mouse, and the word under it as a
    // link while Ctrl is held (Ctrl pressed and released over it too).
    m_edit->area()->viewport()->installEventFilter(this);
    m_edit->area()->viewport()->setMouseTracking(true);
    m_edit->area()->installEventFilter(this);

    // The default theme for the desktop until a file chooses a syntax.
    m_syntax = std::make_unique<qce::kate::EditorSyntax>(m_edit);
    m_edit->area()->setWordWrap(m_wordWrap);
}

// The text file and the syntax let go of the editor while it is whole.
EditorWidget::~EditorWidget()
{
    disconnect(m_file, nullptr, this, nullptr);
    disconnect(m_edit->area(), nullptr, this, nullptr);
    m_edit->area()->viewport()->removeEventFilter(this);
    m_edit->area()->removeEventFilter(this);
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

bool EditorWidget::selection(TextRange* range) const
{
    qce::CodeEditArea* area = m_edit->area();
    const qce::TextCursor start = area->selectionStart();
    const qce::TextCursor end = area->selectionEnd();
    *range = {{start.line, start.column}, {end.line, end.column}};
    return area->hasSelection();
}

void EditorWidget::setSelection(TextRange range)
{
    m_edit->area()->setSelection({range.start.line, range.start.column}, {range.end.line, range.end.column});
}

void EditorWidget::setHighlights(const QList<TextRange>& ranges)
{
    // Matches in amber, as search matches are elsewhere.
    QVector<qce::ExtraSelection> selections;
    selections.reserve(ranges.size());
    for (const TextRange& range : ranges)
        selections.append({{range.start.line, range.start.column}, {range.end.line, range.end.column},
                           QColor(250, 200, 60, 140), {}, {}});
    m_edit->area()->setExtraSelections(HighlightsLayer, selections);
}

bool EditorWidget::replace(const QList<TextReplacement>& replacements, const QString& undoText)
{
    QList<qce::CodeEditArea::TextReplacement> edits;
    edits.reserve(replacements.size());
    for (const TextReplacement& replacement : replacements)
        edits.append({{replacement.range.start.line, replacement.range.start.column},
                      {replacement.range.end.line, replacement.range.end.column},
                      replacement.text});
    return m_edit->area()->replaceRanges(edits, undoText);
}

void EditorWidget::setMarks(const QList<TextMark>& marks)
{
    m_marks = marks;
    QVector<qce::ExtraSelection> selections;
    selections.reserve(marks.size());
    for (const TextMark& mark : marks) {
        qce::ExtraSelection selection;
        selection.start = {mark.start.line, mark.start.column};
        selection.end = {mark.end.line, mark.end.column};
        // An empty range marks the character at its start.
        if (mark.end.line < mark.start.line
            || (mark.end.line == mark.start.line && mark.end.column <= mark.start.column))
            selection.end = {mark.start.line, mark.start.column + 1};
        switch (mark.kind) {
        case TextMark::Kind::Error: selection.underline = QColor(0xe0, 0x20, 0x20); break;
        case TextMark::Kind::Warning: selection.underline = QColor(0xd0, 0x90, 0x00); break;
        case TextMark::Kind::Information: selection.underline = QColor(0x30, 0x70, 0xd0); break;
        case TextMark::Kind::Hint: selection.underline = QColor(0x90, 0x90, 0x90); break;
        }
        selections << selection;
    }
    m_edit->area()->setExtraSelections(MarksLayer, selections);
}

bool EditorWidget::linkAt(const QPoint& viewportPoint, TextPosition* start, TextPosition* end) const
{
    using qce::words::isWordCharAt;
    const qce::TextCursor at = m_edit->area()->cursorAt(viewportPoint);
    const QString line = m_document->lineAt(at.line);
    // The cursor goes to the nearer edge of a character: over the right half
    // of a word's last one it is after it.
    int column = at.column;
    if ((column >= line.size() || !isWordCharAt(line, column)) && column > 0 && isWordCharAt(line, column - 1))
        --column;
    if (column >= line.size() || !isWordCharAt(line, column)) return false;
    int first = column;
    int last = column + 1;
    while (first > 0 && isWordCharAt(line, first - 1)) --first;
    while (last < line.size() && isWordCharAt(line, last)) ++last;
    *start = {at.line, first};
    *end = {at.line, last};
    return true;
}

void EditorWidget::showLink(const QPoint& viewportPoint, bool ctrl)
{
    if (!ctrl) {
        hideLink();  // the answer for the word stays, for Ctrl pressed again
        return;
    }
    TextPosition start;
    TextPosition end;
    if (!linkAt(viewportPoint, &start, &end)) {
        hideLink();
        m_query = {};
        return;
    }
    if (m_query.asked && m_query.start.line == start.line && m_query.start.column == start.column) {
        if (m_query.isLink) displayLink();
        return;
    }
    // Another word: the listener tells whether it is a link.
    hideLink();
    m_query = {true, false, start, end};
    const qce::TextCursor at = m_edit->area()->cursorAt(viewportPoint);
    const int column = std::clamp(at.column, start.column, end.column - 1);
    m_querying = true;
    if (m_listener) m_listener->linkQueried({at.line, column});
    m_querying = false;
    // Answered at once: Ctrl is held over the word right now.
    if (m_query.isLink) displayLink();
}

void EditorWidget::answerLink(TextPosition position, bool isLink)
{
    // For the word still asked about only.
    if (!m_query.asked || position.line != m_query.start.line || position.column < m_query.start.column
        || position.column >= m_query.end.column)
        return;
    m_query.isLink = isLink;
    if (m_querying) return;  // showLink() shows it
    // Answered later: if Ctrl is still held over the word.
    QWidget* viewport = m_edit->area()->viewport();
    if (isLink && (QGuiApplication::queryKeyboardModifiers() & Qt::ControlModifier) && viewport->underMouse())
        displayLink();
}

void EditorWidget::displayLink()
{
    // Blue, with a straight line under it, as a link.
    const QColor blue(0x1a, 0x5f, 0xd6);
    qce::ExtraSelection link{{m_query.start.line, m_query.start.column}, {m_query.end.line, m_query.end.column},
                             {}, blue, blue};
    link.straightUnderline = true;
    m_edit->area()->setExtraSelections(LinkLayer, {link});
    m_edit->area()->viewport()->setCursor(Qt::PointingHandCursor);
    m_linkShown = true;
}

void EditorWidget::hideLink()
{
    if (!m_linkShown) return;
    m_linkShown = false;
    m_edit->area()->setExtraSelections(LinkLayer, {});
    m_edit->area()->viewport()->setCursor(Qt::IBeamCursor);
}

bool EditorWidget::eventFilter(QObject* watched, QEvent* event)
{
    QWidget* viewport = m_edit->area()->viewport();
    if (watched == viewport) {
        switch (event->type()) {
        case QEvent::MouseMove: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->buttons() == Qt::NoButton)
                showLink(mouse->position().toPoint(), mouse->modifiers() & Qt::ControlModifier);
            break;
        }
        case QEvent::MouseButtonPress: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            TextPosition start;
            TextPosition end;
            // Ctrl+click on a word shown as a link follows it, the cursor going
            // there first; elsewhere it is an ordinary click.
            if (mouse->button() == Qt::LeftButton && (mouse->modifiers() & Qt::ControlModifier) && m_linkShown
                && linkAt(mouse->position().toPoint(), &start, &end) && start.line == m_query.start.line
                && start.column == m_query.start.column) {
                const qce::TextCursor at = m_edit->area()->cursorAt(mouse->position().toPoint());
                hideLink();
                m_query = {};
                m_edit->area()->setCursorPosition(at);
                if (m_listener) m_listener->linkActivated({at.line, at.column});
                return true;
            }
            break;
        }
        case QEvent::Leave:
            hideLink();
            m_query = {};
            break;
        default: break;
        }
    } else if (watched == m_edit->area()
               && (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease)) {
        // Ctrl pressed or let go with the mouse resting over the text.
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Control && viewport->underMouse())
            showLink(viewport->mapFromGlobal(QCursor::pos()), event->type() == QEvent::KeyPress);
        else if (event->type() == QEvent::KeyPress)
            hideLink();
    } else if (watched == m_edit->area() && event->type() == QEvent::FocusOut) {
        hideLink();
    }
    if (watched == viewport && event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        const qce::TextCursor at = m_edit->area()->cursorAt(help->pos());
        auto before = [](int line, int column, const TextPosition& position) {
            return line < position.line || (line == position.line && column < position.column);
        };
        QStringList messages;
        for (const TextMark& mark : m_marks) {
            const TextPosition end = (mark.end.line > mark.start.line
                                      || (mark.end.line == mark.start.line && mark.end.column > mark.start.column))
                                         ? mark.end
                                         : TextPosition{mark.start.line, mark.start.column + 1};
            if (!before(at.line, at.column, mark.start) && before(at.line, at.column, end))
                messages << mark.message.toHtmlEscaped();
        }
        if (messages.isEmpty()) {
            QToolTip::hideText();
            event->ignore();
        } else {
            QToolTip::showText(help->globalPos(), messages.join(QStringLiteral("<hr>")), m_edit->area()->viewport());
        }
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace spp
