// SPDX-License-Identifier: LGPL-3.0-only
#include "EditorWidget.h"

#include "BreakpointGutter.h"
#include "CompletionPopup.h"

#include <qce/CodeEdit.h>
#include <qce/CodeEditArea.h>
#include <qce/LeftRail.h>
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
#include <QTextDocument>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>

namespace spp {
namespace {

// The area's layers of the marks and of the link under the mouse, beside
// qcodeedit-spell's (100, 101).
constexpr int ScopeLayer = 109;  // the find bar's selection searched in
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
    m_breakpoints = std::make_unique<BreakpointGutter>([this](int line) { toggleBreakpoint(line); });
    m_edit->addLeftMargin(m_breakpoints.get());
    m_edit->area()->setLineBackgroundProvider([this](int line) -> QColor {
        if (!m_breakpoints->isShown()) return {};
        if (line == m_breakpoints->executionLine()) return QColor(60, 130, 240, 80);
        return m_breakpoints->hasLine(line) ? QColor(219, 58, 58, 45) : QColor();
    });
    connect(m_document, &qce::ITextDocument::linesInserted, this, &EditorWidget::linesInserted);
    connect(m_document, &qce::ITextDocument::linesRemoved, this, &EditorWidget::linesRemoved);
    m_foldGutter =std::make_unique<qce::FoldingGutter>(
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
        // Typing moves the cursor too: the word being completed is seen here.
        if (m_completing) updateCompletions();
        if (m_listener) m_listener->cursorMoved({cursor.line, cursor.column});
    });
    m_completionPopup = new CompletionPopup(m_edit);
    m_completionPopup->setFont(m_edit->area()->font());
    connect(m_completionPopup, &CompletionPopup::accepted, this, &EditorWidget::acceptCompletion);
    connect(m_edit->area(), &qce::CodeEditArea::viewportChanged, this, [this] { placeCompletions(); });
    // The area clears its decorations on every change of the text; the
    // marks' messages go with them.
    connect(m_edit->area(), &qce::CodeEditArea::textChanged, this, [this] {
        m_marks.clear();
        m_linkShown = false;
        m_query = {};  // an answer was for the text before
        m_hover = {};
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
    disconnect(m_document, nullptr, this, nullptr);
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
    const EditorResult result = toResult(m_file->open(path, encoding));
    if (result == EditorResult::Ok) {
        // They were on the lines of the text before.
        m_breakpoints->setLines({});
        m_breakpoints->setExecutionLine(-1);
        repaintBreakpoints();
    }
    return result;
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

QString EditorWidget::selectedText() const
{
    TextRange range;
    if (!selection(&range) || range.start.line != range.end.line) return {};
    return m_document->lineAt(range.start.line).mid(range.start.column, range.end.column - range.start.column);
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

void EditorWidget::setScopeHighlights(const QList<TextRange>& ranges)
{
    // Pale blue (dark blue-grey on a dark background), as in qceditor.
    const bool dark = m_edit->area()->palette().color(QPalette::Base).lightness() < 128;
    const QColor shade = dark ? QColor(0x33, 0x40, 0x4f) : QColor(0xe3, 0xec, 0xf7);
    QVector<qce::ExtraSelection> selections;
    selections.reserve(ranges.size());
    for (const TextRange& range : ranges)
        selections.append({{range.start.line, range.start.column}, {range.end.line, range.end.column}, shade, {}, {}});
    m_edit->area()->setExtraSelections(ScopeLayer, selections);
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

void EditorWidget::setBreakpointMargin(bool shown)
{
    m_breakpoints->setShown(shown);
    m_edit->findChild<qce::LeftRail*>()->updateGeometry();
    repaintBreakpoints();
}

bool EditorWidget::hasBreakpointMargin() const
{
    return m_breakpoints->isShown();
}

void EditorWidget::setBreakpoints(const QList<int>& lines)
{
    QList<int> sorted = lines;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    m_breakpoints->setLines(sorted);
    repaintBreakpoints();
}

QList<int> EditorWidget::breakpoints() const
{
    return m_breakpoints->lines();
}

void EditorWidget::setExecutionLine(int line)
{
    m_breakpoints->setExecutionLine(line);
    repaintBreakpoints();
}

int EditorWidget::executionLine() const
{
    return m_breakpoints->executionLine();
}

void EditorWidget::repaintBreakpoints()
{
    m_edit->findChild<qce::LeftRail*>()->update();
    m_edit->area()->viewport()->update();
}

void EditorWidget::toggleBreakpoint(int line)
{
    QList<int> lines = m_breakpoints->lines();
    const auto at = std::lower_bound(lines.begin(), lines.end(), line);
    if (at != lines.end() && *at == line)
        lines.erase(at);
    else
        lines.insert(at, line);
    m_breakpoints->setLines(lines);
    repaintBreakpoints();
    if (m_listener) m_listener->breakpointsChanged(lines);
}

void EditorWidget::linesInserted(int start, int count)
{
    // The document says only which lines came after the split one. Left with
    // nothing but spaces, it gave its code away to the last of them (Enter
    // at the start of a line).
    const int split = start - 1;
    const bool codeLeft = split >= 0 && m_document->lineAt(split).trimmed().isEmpty();
    moveLines([=](int line) { return line >= start || (line == split && codeLeft) ? line + count : line; });
}

void EditorWidget::linesRemoved(int start, int count)
{
    // What followed the removed text on their last line joined the line
    // before them (Backspace at the start of a line, a deleted line).
    const int last = start + count - 1;
    moveLines([=](int line) {
        if (line < start) return line;
        if (line > last) return line - count;
        return line == last ? start - 1 : -1;
    });
}

void EditorWidget::moveLines(const std::function<int(int)>& moved)
{
    const QList<int> before = m_breakpoints->lines();
    QList<int> lines;
    lines.reserve(before.size());
    for (int line : before) {
        const int now = moved(line);
        if (now >= 0 && (lines.isEmpty() || lines.last() != now)) lines << now;
    }
    if (m_breakpoints->executionLine() >= 0) m_breakpoints->setExecutionLine(moved(m_breakpoints->executionLine()));
    if (lines == before) return;
    m_breakpoints->setLines(lines);
    if (m_listener) m_listener->breakpointsChanged(lines);
}

void EditorWidget::showCompletions(const QList<CompletionItem>& items, TextPosition start, bool incomplete)
{
    if (isReadOnly()) return;
    m_completions = items;
    m_completionStart = start;
    m_completionIncomplete = incomplete;
    m_completing = true;
    if (!completionWord(&m_completionTyped)) {
        hideCompletions();
        return;
    }
    updateCompletions();
}

void EditorWidget::hideCompletions()
{
    m_completing = false;
    m_completions.clear();
    m_completionPopup->hide();
}

bool EditorWidget::completionWord(QString* typed) const
{
    const TextPosition cursor = cursorPosition();
    if (cursor.line != m_completionStart.line || cursor.column < m_completionStart.column) return false;
    const QString line = m_document->lineAt(cursor.line);
    for (int column = m_completionStart.column; column < cursor.column; ++column)
        if (!qce::words::isWordCharAt(line, column)) return false;
    *typed = line.mid(m_completionStart.column, cursor.column - m_completionStart.column);
    return true;
}

void EditorWidget::updateCompletions()
{
    QString typed;
    if (!completionWord(&typed)) {
        hideCompletions();
        return;
    }
    const bool grew = typed != m_completionTyped;
    m_completionTyped = typed;
    const bool any = m_completionPopup->fill(m_completions, typed);
    // Incomplete, more may come for this word though none fits now.
    if (!any && !m_completionIncomplete) {
        hideCompletions();
        return;
    }
    m_completionPopup->setVisible(any);
    if (any) {
        m_completionPopup->raise();
        placeCompletions();
    }
    if (grew && m_completionIncomplete && m_listener) m_listener->completionsNeeded(cursorPosition());
}

void EditorWidget::placeCompletions()
{
    if (!m_completionPopup->isVisible()) return;
    qce::CodeEditArea* area = m_edit->area();
    const QRect cursor = area->inputMethodQuery(Qt::ImCursorRectangle).toRect();
    const QPoint top = area->mapTo(m_edit, cursor.topLeft());
    const QPoint bottom = area->mapTo(m_edit, cursor.bottomLeft());
    const QSize size = m_completionPopup->wantedSize().boundedTo(m_edit->size());
    // Under the word's start, or over it when there is no room below.
    int x = top.x() - m_completionTyped.size() * area->fontMetrics().horizontalAdvance(QLatin1Char('x'));
    x = std::clamp(x, 0, std::max(0, m_edit->width() - size.width()));
    const int y = bottom.y() + size.height() < m_edit->height() ? bottom.y() + 1 : std::max(0, top.y() - size.height());
    m_completionPopup->setGeometry(x, y, size.width(), size.height());
}

void EditorWidget::acceptCompletion()
{
    const int chosen = m_completionPopup->chosen();
    if (chosen < 0 || chosen >= m_completions.size()) return;
    const CompletionItem item = m_completions.at(chosen);
    const TextPosition start = m_completionStart;
    const TextPosition end = cursorPosition();
    hideCompletions();
    const QString written = item.insertText.isEmpty() ? item.label : item.insertText;
    QList<qce::CodeEditArea::TextReplacement> edits{{{start.line, start.column}, {end.line, end.column}, written}};
    // The cursor goes after what was written: lines added or removed above
    // it move that down or up.
    int line = start.line + int(written.count(QLatin1Char('\n')));
    for (const TextReplacement& edit : item.additionalEdits) {
        edits.append({{edit.range.start.line, edit.range.start.column}, {edit.range.end.line, edit.range.end.column}, edit.text});
        if (edit.range.start.line < start.line)
            line += int(edit.text.count(QLatin1Char('\n'))) - (edit.range.end.line - edit.range.start.line);
    }
    std::sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) {
        return a.start.line != b.start.line ? a.start.line < b.start.line : a.start.column < b.start.column;
    });
    if (!m_edit->area()->replaceRanges(edits, tr("Complete"))) return;
    const int lastBreak = int(written.lastIndexOf(QLatin1Char('\n')));
    const int column = lastBreak < 0 ? start.column + int(written.size()) : int(written.size()) - lastBreak - 1;
    m_edit->area()->setCursorPosition({line, column});
}

bool EditorWidget::completionKey(int key)
{
    if (!m_completionPopup->isVisible()) return false;
    switch (key) {
    case Qt::Key_Up: m_completionPopup->moveChoice(-1, true); return true;
    case Qt::Key_Down: m_completionPopup->moveChoice(1, true); return true;
    case Qt::Key_PageUp: m_completionPopup->moveChoice(-9, false); return true;
    case Qt::Key_PageDown: m_completionPopup->moveChoice(9, false); return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Tab: acceptCompletion(); return true;
    case Qt::Key_Escape: hideCompletions(); return true;
    default: return false;
    }
}

void EditorWidget::showHover(const QPoint& point, const QPoint& globalPoint)
{
    const qce::TextCursor at = m_edit->area()->cursorAt(point);
    auto before = [](int line, int column, const TextPosition& position) {
        return line < position.line || (line == position.line && column < position.column);
    };
    m_hover = {};
    m_hover.point = point;
    m_hover.globalPoint = globalPoint;
    for (const TextMark& mark : m_marks) {
        const TextPosition end = (mark.end.line > mark.start.line
                                  || (mark.end.line == mark.start.line && mark.end.column > mark.start.column))
                                     ? mark.end
                                     : TextPosition{mark.start.line, mark.start.column + 1};
        if (!before(at.line, at.column, mark.start) && before(at.line, at.column, end))
            m_hover.messages << mark.message.toHtmlEscaped();
    }
    if (m_hover.messages.isEmpty())
        QToolTip::hideText();
    else
        showHoverText(m_hover.messages.join(QStringLiteral("<hr>")));
    // The word: a description may come now or later.
    TextPosition start;
    TextPosition end;
    if (!linkAt(point, &start, &end)) return;
    m_hover.asked = true;
    m_hover.start = start;
    m_hover.end = end;
    const int column = std::clamp(at.column, start.column, end.column - 1);
    if (m_listener) m_listener->hoverQueried({at.line, column});
}

void EditorWidget::answerHover(TextPosition position, const QString& markdown)
{
    // For the word still under the mouse only.
    if (!m_hover.asked || markdown.trimmed().isEmpty() || position.line != m_hover.start.line
        || position.column < m_hover.start.column || position.column >= m_hover.end.column)
        return;
    QTextDocument document;
    document.setMarkdown(markdown);
    // Only its body: it goes after the marks' messages in one tooltip.
    const QString html = document.toHtml();
    const qsizetype body = html.indexOf(QLatin1Char('>'), html.indexOf(QLatin1String("<body"))) + 1;
    QStringList parts = m_hover.messages;
    parts << html.mid(body, html.lastIndexOf(QLatin1String("</body>")) - body);
    showHoverText(parts.join(QStringLiteral("<hr>")));
}

void EditorWidget::showHoverText(const QString& html)
{
    // Gone when the mouse goes on, as a description belongs to one word.
    QWidget* viewport = m_edit->area()->viewport();
    QToolTip::showText(m_hover.globalPoint, html, viewport, QRect(m_hover.point - QPoint(12, 12), QSize(24, 24)));
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
            // A description on its way is of the word the mouse rested on.
            if (m_hover.asked) {
                TextPosition start;
                TextPosition end;
                if (!linkAt(mouse->position().toPoint(), &start, &end) || start.line != m_hover.start.line
                    || start.column != m_hover.start.column)
                    m_hover = {};
            }
            break;
        }
        case QEvent::MouseButtonPress: {
            hideCompletions();
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
            m_hover = {};
            break;
        default: break;
        }
    } else if (watched == m_edit->area()
               && (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease)) {
        // Ctrl pressed or let go with the mouse resting over the text.
        const auto* key = static_cast<QKeyEvent*>(event);
        if (event->type() == QEvent::KeyPress && completionKey(key->key())) return true;
        if (key->key() == Qt::Key_Control && viewport->underMouse())
            showLink(viewport->mapFromGlobal(QCursor::pos()), event->type() == QEvent::KeyPress);
        else if (event->type() == QEvent::KeyPress)
            hideLink();
    } else if (watched == m_edit->area() && event->type() == QEvent::FocusOut) {
        hideLink();
        hideCompletions();
    }
    if (watched == viewport && event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        showHover(help->pos(), help->globalPos());
        if (m_hover.messages.isEmpty()) event->ignore();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace spp
