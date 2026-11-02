// SPDX-License-Identifier: LGPL-3.0-only
#include "FindBarWidget.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace spp {
namespace {

// More matches than this are not sought: the count says so.
constexpr int maxMatches = 100000;

QToolButton* option(const QString& text, const QString& tip, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(tip);
    button->setCheckable(true);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

} // namespace

FindBarWidget::FindBarWidget(QWidget* parent) : QWidget(parent) {
    m_find = new QLineEdit(this);
    m_find->setPlaceholderText(tr("Find"));
    m_find->setClearButtonEnabled(true);
    m_matchCase = option(QStringLiteral("Aa"), tr("Match case (Alt+C)"), this);
    m_matchCase->setShortcut(QKeySequence(Qt::ALT | Qt::Key_C));
    m_wholeWords = option(QStringLiteral("W"), tr("Whole words (Alt+W)"), this);
    m_wholeWords->setShortcut(QKeySequence(Qt::ALT | Qt::Key_W));
    m_regex = option(QStringLiteral(".*"), tr("Regular expression (Alt+X)"), this);
    m_regex->setShortcut(QKeySequence(Qt::ALT | Qt::Key_X));
    m_count = new QLabel(this);
    m_count->setMinimumWidth(m_count->fontMetrics().horizontalAdvance(QStringLiteral("99999 of 99999")));
    auto* previous = new QToolButton(this);
    previous->setText(QStringLiteral("↑"));
    previous->setToolTip(tr("Previous match (Shift+Enter, Shift+F3)"));
    previous->setAutoRaise(true);
    auto* next = new QToolButton(this);
    next->setText(QStringLiteral("↓"));
    next->setToolTip(tr("Next match (Enter, F3)"));
    next->setAutoRaise(true);
    auto* closeButton = new QToolButton(this);
    closeButton->setText(QStringLiteral("×"));
    closeButton->setToolTip(tr("Close (Escape)"));
    closeButton->setAutoRaise(true);

    m_replacement = new QLineEdit(this);
    m_replacement->setPlaceholderText(tr("Replace"));
    m_replaceButton = new QPushButton(tr("Replace"), this);
    m_replaceAllButton = new QPushButton(tr("Replace All"), this);

    auto* findRow = new QHBoxLayout;
    findRow->setContentsMargins(0, 0, 0, 0);
    findRow->addWidget(m_find, 1);
    findRow->addWidget(m_matchCase);
    findRow->addWidget(m_wholeWords);
    findRow->addWidget(m_regex);
    findRow->addWidget(m_count);
    findRow->addWidget(previous);
    findRow->addWidget(next);
    findRow->addWidget(closeButton);
    m_replaceRow = new QWidget(this);
    auto* replaceRow = new QHBoxLayout(m_replaceRow);
    replaceRow->setContentsMargins(0, 0, 0, 0);
    replaceRow->addWidget(m_replacement, 1);
    replaceRow->addWidget(m_replaceButton);
    replaceRow->addWidget(m_replaceAllButton);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(2);
    layout->addLayout(findRow);
    layout->addWidget(m_replaceRow);

    // Typing seeks at once, from where the search began.
    connect(m_find, &QLineEdit::textEdited, this, [this] {
        TextRange range;
        if (!m_editor) return;
        m_editor->selection(&range);
        search();
        selectFrom(offsetOf(range.start), true);
    });
    for (QToolButton* button : {m_matchCase, m_wholeWords, m_regex})
        connect(button, &QToolButton::toggled, this, [this] {
            if (!m_editor) return;
            TextRange range;
            m_editor->selection(&range);
            search();
            selectFrom(offsetOf(range.start), true);
        });
    connect(previous, &QToolButton::clicked, this, [this] { step(false); });
    connect(next, &QToolButton::clicked, this, [this] { step(true); });
    connect(closeButton, &QToolButton::clicked, this, &FindBarWidget::close);
    connect(m_replaceButton, &QPushButton::clicked, this, &FindBarWidget::replaceOne);
    connect(m_replaceAllButton, &QPushButton::clicked, this, &FindBarWidget::replaceAll);
    m_find->installEventFilter(this);
    m_replacement->installEventFilter(this);

    // Edits come one after another: sought again once they pause.
    m_researching.setSingleShot(true);
    m_researching.setInterval(200);
    connect(&m_researching, &QTimer::timeout, this, [this] {
        if (isVisible() && search()) showCount();
    });
    hide();
}

void FindBarWidget::setEditor(EditorView* editor) {
    if (editor == m_editor) return;
    if (m_editor && isVisible()) m_editor->setHighlights({});
    m_editor = editor;
    m_matches.clear();
    setEnabled(m_editor != nullptr);
    if (m_editor && isVisible()) {
        search();
        showCount();
    }
}

void FindBarWidget::showFind() {
    open(false);
}

void FindBarWidget::showReplace() {
    open(true);
}

void FindBarWidget::open(bool replacing) {
    m_replaceRow->setVisible(replacing);
    if (m_editor) {
        // The selection, within one line, is what is sought.
        TextRange range;
        if (m_editor->selection(&range) && range.start.line == range.end.line) {
            const QString line = m_editor->text().section(QLatin1Char('\n'), range.start.line, range.start.line);
            m_find->setText(line.mid(range.start.column, range.end.column - range.start.column));
        }
        m_replaceButton->setEnabled(!m_editor->isReadOnly());
        m_replaceAllButton->setEnabled(!m_editor->isReadOnly());
    }
    show();
    m_find->setFocus();
    m_find->selectAll();
    if (m_editor && search()) {
        TextRange range;
        m_editor->selection(&range);
        selectFrom(offsetOf(range.start), true);
    }
}

void FindBarWidget::close() {
    hide();
    m_researching.stop();
    if (m_editor) {
        m_editor->setHighlights({});
        m_editor->widget()->setFocus();
    }
}

void FindBarWidget::editorTextChanged() {
    if (isVisible()) m_researching.start();
}

bool FindBarWidget::search() {
    m_matches.clear();
    m_error.clear();
    if (!m_editor) return false;
    m_text = m_editor->text();
    m_lineStarts = {0};
    for (int i = 0; i < m_text.size(); ++i)
        if (m_text.at(i) == QLatin1Char('\n')) m_lineStarts << i + 1;
    const QString sought = m_find->text();
    if (sought.isEmpty()) {
        m_editor->setHighlights({});
        showCount();
        return true;
    }
    QString pattern = m_regex->isChecked() ? sought : QRegularExpression::escape(sought);
    if (m_wholeWords->isChecked()) pattern = QStringLiteral("\\b(?:%1)\\b").arg(pattern);
    QRegularExpression expression(pattern, m_matchCase->isChecked() ? QRegularExpression::NoPatternOption
                                                                    : QRegularExpression::CaseInsensitiveOption);
    expression.setPatternOptions(expression.patternOptions() | QRegularExpression::UseUnicodePropertiesOption);
    if (!expression.isValid()) {
        m_error = expression.errorString();
        m_editor->setHighlights({});
        showCount();
        return false;
    }
    QList<TextRange> highlights;
    QRegularExpressionMatchIterator it = expression.globalMatch(m_text);
    while (it.hasNext() && m_matches.size() < maxMatches) {
        const QRegularExpressionMatch found = it.next();
        if (found.capturedLength() == 0) continue;  // nothing to select
        Match match{int(found.capturedStart()), int(found.capturedEnd()), found.capturedTexts()};
        highlights << rangeOf(match.start, match.end);
        m_matches << match;
    }
    m_editor->setHighlights(highlights);
    showCount();
    return true;
}

TextRange FindBarWidget::rangeOf(int start, int end) const {
    auto position = [this](int offset) {
        const auto line = std::upper_bound(m_lineStarts.cbegin(), m_lineStarts.cend(), offset) - m_lineStarts.cbegin() - 1;
        return TextPosition{int(line), offset - m_lineStarts.at(line)};
    };
    return {position(start), position(end)};
}

int FindBarWidget::offsetOf(TextPosition position) const {
    if (m_lineStarts.isEmpty()) return 0;
    const int line = std::clamp(position.line, 0, int(m_lineStarts.size()) - 1);
    return std::min(int(m_text.size()), m_lineStarts.at(line) + position.column);
}

int FindBarWidget::currentMatch() const {
    if (!m_editor) return -1;
    TextRange range;
    if (!m_editor->selection(&range)) return -1;
    const int start = offsetOf(range.start);
    const int end = offsetOf(range.end);
    for (int i = 0; i < m_matches.size(); ++i)
        if (m_matches.at(i).start == start && m_matches.at(i).end == end) return i;
    return -1;
}

void FindBarWidget::selectFrom(int offset, bool forward) {
    if (!m_editor || m_matches.isEmpty()) {
        showCount();
        return;
    }
    int index = -1;
    if (forward) {
        const auto it = std::find_if(m_matches.cbegin(), m_matches.cend(), [offset](const Match& m) { return m.start >= offset; });
        index = it == m_matches.cend() ? 0 : int(it - m_matches.cbegin());  // around the end
    } else {
        for (int i = int(m_matches.size()) - 1; i >= 0 && index < 0; --i)
            if (m_matches.at(i).start < offset) index = i;
        if (index < 0) index = int(m_matches.size()) - 1;  // around the start
    }
    m_editor->setSelection(rangeOf(m_matches.at(index).start, m_matches.at(index).end));
    showCount();
}

void FindBarWidget::step(bool forward) {
    if (!m_editor) return;
    if (!isVisible()) {
        open(false);
        return;
    }
    if (m_matches.isEmpty() && !search()) return;
    TextRange range;
    m_editor->selection(&range);
    // From the selected match on, or from the cursor.
    selectFrom(forward ? offsetOf(range.end) : offsetOf(range.start), forward);
}

void FindBarWidget::replaceOne() {
    if (!m_editor || m_editor->isReadOnly()) return;
    const int index = currentMatch();
    if (index < 0) {  // the selection is no match: the next one first
        step(true);
        return;
    }
    const Match match = m_matches.at(index);
    const QString text = m_regex->isChecked() ? expand(m_replacement->text(), match.captures) : m_replacement->text();
    m_editor->replace({{rangeOf(match.start, match.end), text}}, tr("Replace"));
    m_researching.stop();
    search();
    selectFrom(match.start + int(text.size()), true);
}

void FindBarWidget::replaceAll() {
    if (!m_editor || m_editor->isReadOnly() || m_matches.isEmpty()) return;
    QList<TextReplacement> replacements;
    replacements.reserve(m_matches.size());
    for (const Match& match : std::as_const(m_matches))
        replacements << TextReplacement{rangeOf(match.start, match.end),
                                        m_regex->isChecked() ? expand(m_replacement->text(), match.captures)
                                                             : m_replacement->text()};
    const int count = int(replacements.size());
    m_editor->replace(replacements, tr("Replace All"));
    m_researching.stop();
    search();
    m_count->setText(tr("%n replaced", nullptr, count));
}

void FindBarWidget::showCount() {
    QPalette palette = m_find->palette();
    palette.setColor(QPalette::Text, this->palette().color(QPalette::Text));
    if (!m_error.isEmpty()) {
        m_count->setText(tr("Invalid"));
        m_count->setToolTip(m_error);
        palette.setColor(QPalette::Text, QColor(0xc0, 0x10, 0x10));
    } else if (m_find->text().isEmpty()) {
        m_count->clear();
        m_count->setToolTip({});
    } else if (m_matches.isEmpty()) {
        m_count->setText(tr("No matches"));
        m_count->setToolTip({});
        palette.setColor(QPalette::Text, QColor(0xc0, 0x10, 0x10));
    } else {
        const int index = currentMatch();
        const QString total = m_matches.size() >= maxMatches ? QStringLiteral("%1+").arg(maxMatches)
                                                             : QString::number(m_matches.size());
        m_count->setText(index >= 0 ? tr("%1 of %2").arg(index + 1).arg(total) : tr("%1 matches").arg(total));
        m_count->setToolTip({});
    }
    m_find->setPalette(palette);
}

QString FindBarWidget::expand(const QString& replacement, const QStringList& captures) {
    QString result;
    for (int i = 0; i < replacement.size(); ++i) {
        const QChar c = replacement.at(i);
        const QChar next = i + 1 < replacement.size() ? replacement.at(i + 1) : QChar();
        if ((c == QLatin1Char('$') || c == QLatin1Char('\\')) && next.isDigit()) {
            const int group = next.digitValue();
            if (group < captures.size()) result += captures.at(group);
            ++i;
        } else if (c == QLatin1Char('$') && next == QLatin1Char('$')) {
            result += QLatin1Char('$');
            ++i;
        } else if (c == QLatin1Char('\\') && next == QLatin1Char('n')) {
            result += QLatin1Char('\n');
            ++i;
        } else if (c == QLatin1Char('\\') && next == QLatin1Char('t')) {
            result += QLatin1Char('\t');
            ++i;
        } else if (c == QLatin1Char('\\') && next == QLatin1Char('\\')) {
            result += QLatin1Char('\\');
            ++i;
        } else {
            result += c;
        }
    }
    return result;
}

bool FindBarWidget::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            close();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (watched == m_replacement) replaceOne();
            else step(!(key->modifiers() & Qt::ShiftModifier));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace spp
