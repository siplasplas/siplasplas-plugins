// SPDX-License-Identifier: LGPL-3.0-only
#include "FindBarWidget.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace spp {
namespace {

// More matches than this are not sought: the count says so.
constexpr int maxMatches = 100000;
// The steps of the progress bar.
constexpr int progressSteps = 1000;
// A selection longer than this (or of several lines) is searched in when the
// bar opens, not searched for.
constexpr int longSelection = 64;

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
    m_wrap = option(QStringLiteral("↩"), tr("Wrap around"), this);
    m_wrap->setChecked(true);
    m_matchCase = option(QStringLiteral("Aa"), tr("Match case (Alt+C)"), this);
    m_matchCase->setShortcut(QKeySequence(Qt::ALT | Qt::Key_C));
    m_wholeWords = option(QStringLiteral("W"), tr("Whole words (Alt+W)"), this);
    m_wholeWords->setShortcut(QKeySequence(Qt::ALT | Qt::Key_W));
    m_regex = option(QStringLiteral(".*"), tr("Regular expression (Alt+X)"), this);
    m_regex->setShortcut(QKeySequence(Qt::ALT | Qt::Key_X));
    m_count = new QLabel(this);
    m_count->setMinimumWidth(m_count->fontMetrics().horizontalAdvance(QStringLiteral("99999 of 99999")));
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, progressSteps);
    m_progress->setTextVisible(false);
    m_progress->setMaximumWidth(m_count->minimumWidth());
    m_progress->setToolTip(tr("Searching (Escape cancels)"));
    m_progress->hide();
    auto* previous = new QToolButton(this);
    previous->setText(QStringLiteral("↑"));
    previous->setToolTip(tr("Previous match (Shift+Enter, Shift+F3)"));
    previous->setAutoRaise(true);
    auto* next = new QToolButton(this);
    next->setText(QStringLiteral("↓"));
    next->setToolTip(tr("Next match (Enter, F3)"));
    next->setAutoRaise(true);
    m_scope = option(QStringLiteral("▽"),
                     tr("Search in selection (set by itself when the bar opens with several lines or a long "
                        "text selected)"),
                     this);
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
    findRow->addWidget(m_wrap);
    findRow->addWidget(m_matchCase);
    findRow->addWidget(m_wholeWords);
    findRow->addWidget(m_regex);
    findRow->addWidget(m_count);
    findRow->addWidget(m_progress);
    findRow->addWidget(previous);
    findRow->addWidget(next);
    findRow->addWidget(m_scope);
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

    // Typing seeks at once, from where the search began (a huge target
    // highlights what it shows; Enter seeks).
    connect(m_find, &QLineEdit::textEdited, this, &FindBarWidget::queryChanged);
    for (QToolButton* button : {m_matchCase, m_wholeWords, m_regex})
        connect(button, &QToolButton::toggled, this, &FindBarWidget::queryChanged);
    connect(m_scope, &QToolButton::toggled, this, &FindBarWidget::scopeToggled);
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
        if (isVisible() && search() && !huge()) showCount();
    });
    m_replaceRow->hide();
    hide();
}

FindBarWidget::~FindBarWidget() {
    setTarget(nullptr);
}

void FindBarWidget::setTarget(FindTarget* target) {
    if (target == m_target) return;
    if (m_target) {
        cancelSearch();
        m_target->setFindListener(nullptr);
        if (isVisible()) {
            if (huge()) {
                m_target->setQuery({});
            } else {
                m_target->setHighlights({});
                m_target->setScopeHighlights({});
            }
        }
    }
    m_target = target;
    m_matches.clear();
    // The selection searched in was the other target's.
    m_scopeAvailable = false;
    {
        const QSignalBlocker blocker(m_scope);
        m_scope->setChecked(false);
    }
    m_scope->setVisible(m_target && !m_target->isHuge());
    setEnabled(m_target != nullptr);
    if (m_target) m_target->setFindListener(this);
    updateReplaceRow();
    if (m_target && isVisible()) {
        search();
        if (!huge()) showCount();
    }
}

void FindBarWidget::setEditor(EditorView* editor) {
    setTarget(editor ? editor->findTarget() : nullptr);
}

void FindBarWidget::showFind() {
    open(false);
}

void FindBarWidget::showReplace() {
    open(true);
}

FindQuery FindBarWidget::query() const {
    return {m_find->text(), m_matchCase->isChecked(), m_wholeWords->isChecked(), m_regex->isChecked(),
            m_wrap->isChecked()};
}

void FindBarWidget::updateReplaceRow() {
    m_replaceRow->setVisible(m_replacing && m_target && m_target->canReplace());
}

void FindBarWidget::open(bool replacing) {
    m_replacing = replacing;
    updateReplaceRow();
    if (m_target) {
        m_scope->setVisible(!huge());
        // The selection is searched in, when opened with several lines or a
        // long text selected (shaded); captured before the first match is
        // selected. A short one in a line is what is sought, unless it is
        // the match selected (maybe found by a pattern).
        bool scoped = false;
        const bool selectedMatch = isVisible() && currentMatch() >= 0;
        if (!huge() && isHidden()) {
            takeText();
            TextRange range;
            m_scopeAvailable = m_target->selection(&range);
            if (m_scopeAvailable) {
                m_scopeStart = offsetOf(range.start);
                m_scopeEnd = offsetOf(range.end);
                scoped = range.start.line != range.end.line || m_scopeEnd - m_scopeStart > longSelection;
            }
            const QSignalBlocker blocker(m_scope);
            m_scope->setChecked(scoped);
        }
        const QString selected = scoped || selectedMatch ? QString() : m_target->selectedText();
        if (!selected.isEmpty()) m_find->setText(selected);
    }
    show();
    m_find->setFocus();
    m_find->selectAll();
    if (!m_target) return;
    if (huge()) {
        // Seeking a file of gigabytes waits for Enter.
        if (!m_searching) search();
        return;
    }
    if (search()) {
        TextRange range;
        m_target->selection(&range);
        selectFrom(offsetOf(range.start), true);
    }
}

void FindBarWidget::close() {
    hide();
    m_researching.stop();
    if (!m_target) return;
    if (huge()) {
        cancelSearch();
        m_target->setQuery({});
    } else {
        m_target->setHighlights({});
        m_target->setScopeHighlights({});
    }
    m_target->widget()->setFocus();
}

void FindBarWidget::editorTextChanged() {
    if (isVisible()) m_researching.start();
}

void FindBarWidget::queryChanged() {
    if (!m_target) return;
    if (huge()) {
        cancelSearch();
        search();
        return;
    }
    TextRange range;
    m_target->selection(&range);
    search();
    selectFrom(offsetOf(range.start), true);
}

void FindBarWidget::scopeToggled(bool enabled) {
    if (!m_target || huge()) return;
    if (enabled && !m_scopeAvailable) {
        // Turned on by hand: the selection now, if there is one.
        takeText();
        TextRange range;
        if (!m_target->selection(&range)) {
            const QSignalBlocker blocker(m_scope);
            m_scope->setChecked(false);
            return;
        }
        m_scopeStart = offsetOf(range.start);
        m_scopeEnd = offsetOf(range.end);
        m_scopeAvailable = true;
    }
    TextRange range;
    m_target->selection(&range);
    const int from = offsetOf(range.start);
    search();
    selectFrom(enabled ? m_scopeStart : from, true);
}

bool FindBarWidget::scoped() const {
    return m_scope->isChecked() && m_scopeAvailable && !huge();
}

void FindBarWidget::takeText() {
    const QString text = m_target->text();
    if (m_scopeAvailable && text != m_text) {
        // The selection searched in follows the edit: the part that differs
        // between the common start and end.
        const int oldSize = int(m_text.size());
        const int newSize = int(text.size());
        int prefix = 0;
        while (prefix < oldSize && prefix < newSize && m_text.at(prefix) == text.at(prefix)) ++prefix;
        int oldEnd = oldSize;
        int newEnd = newSize;
        while (oldEnd > prefix && newEnd > prefix && m_text.at(oldEnd - 1) == text.at(newEnd - 1)) {
            --oldEnd;
            --newEnd;
        }
        auto adjust = [prefix, oldEnd, newEnd](int pos, bool end) {
            if (pos < prefix || (pos == prefix && !end)) return pos;
            if (pos >= oldEnd) return pos + newEnd - oldEnd;
            return end ? newEnd : prefix;
        };
        m_scopeStart = adjust(m_scopeStart, false);
        m_scopeEnd = adjust(m_scopeEnd, true);
    }
    m_text = text;
    m_lineStarts = {0};
    for (int i = 0; i < m_text.size(); ++i)
        if (m_text.at(i) == QLatin1Char('\n')) m_lineStarts << i + 1;
}

bool FindBarWidget::search() {
    m_matches.clear();
    m_error.clear();
    if (!m_target) return false;
    const QString sought = m_find->text();
    QRegularExpression expression;
    if (!sought.isEmpty()) {
        QString pattern = m_regex->isChecked() ? sought : QRegularExpression::escape(sought);
        if (m_wholeWords->isChecked()) pattern = QStringLiteral("\\b(?:%1)\\b").arg(pattern);
        expression.setPattern(pattern);
        expression.setPatternOptions(
            (m_matchCase->isChecked() ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption)
            | QRegularExpression::UseUnicodePropertiesOption);
        if (!expression.isValid()) m_error = expression.errorString();
    }
    if (huge()) {
        // The target seeks itself; it highlights the matches it shows.
        m_target->setQuery(m_error.isEmpty() ? query() : FindQuery{});
        if (!m_error.isEmpty()) showStatus(tr("Invalid"), true);
        else showStatus({}, false);
        m_count->setToolTip(m_error);
        return m_error.isEmpty();
    }
    takeText();
    m_target->setScopeHighlights(scoped() && m_scopeEnd > m_scopeStart
                                     ? QList<TextRange>{rangeOf(m_scopeStart, m_scopeEnd)}
                                     : QList<TextRange>{});
    if (sought.isEmpty() || !m_error.isEmpty()) {
        m_target->setHighlights({});
        showCount();
        return m_error.isEmpty();
    }
    QList<TextRange> highlights;
    QRegularExpressionMatchIterator it = expression.globalMatch(m_text);
    while (it.hasNext() && m_matches.size() < maxMatches) {
        const QRegularExpressionMatch found = it.next();
        if (found.capturedLength() == 0) continue;  // nothing to select
        if (scoped() && (found.capturedStart() < m_scopeStart || found.capturedEnd() > m_scopeEnd)) continue;
        Match match{int(found.capturedStart()), int(found.capturedEnd()), found.capturedTexts()};
        highlights << rangeOf(match.start, match.end);
        m_matches << match;
    }
    m_target->setHighlights(highlights);
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
    if (!m_target) return -1;
    TextRange range;
    if (!m_target->selection(&range)) return -1;
    const int start = offsetOf(range.start);
    const int end = offsetOf(range.end);
    for (int i = 0; i < m_matches.size(); ++i)
        if (m_matches.at(i).start == start && m_matches.at(i).end == end) return i;
    return -1;
}

void FindBarWidget::selectFrom(int offset, bool forward) {
    if (!m_target || m_matches.isEmpty()) {
        showCount();
        return;
    }
    int index = -1;
    if (forward) {
        const auto it = std::find_if(m_matches.cbegin(), m_matches.cend(), [offset](const Match& m) { return m.start >= offset; });
        if (it != m_matches.cend()) index = int(it - m_matches.cbegin());
        else if (m_wrap->isChecked()) index = 0;  // around the end
    } else {
        for (int i = int(m_matches.size()) - 1; i >= 0 && index < 0; --i)
            if (m_matches.at(i).start < offset) index = i;
        if (index < 0 && m_wrap->isChecked()) index = int(m_matches.size()) - 1;  // around the start
    }
    if (index < 0) {  // the end (start) without wrapping around: the selection stays
        showCount();
        return;
    }
    m_target->setSelection(rangeOf(m_matches.at(index).start, m_matches.at(index).end));
    showCount();
}

void FindBarWidget::step(bool forward) {
    if (!m_target) return;
    if (!isVisible()) {
        open(false);
        return;
    }
    if (huge()) {
        if (m_find->text().isEmpty() || !m_error.isEmpty()) return;
        // Shown before find(), which may finish at once.
        m_searching = true;
        m_progress->setValue(0);
        m_progress->show();
        m_count->hide();
        m_target->find(query(), forward);
        return;
    }
    if (m_matches.isEmpty() && !search()) return;
    TextRange range;
    m_target->selection(&range);
    // From the selected match on, or from the cursor.
    selectFrom(forward ? offsetOf(range.end) : offsetOf(range.start), forward);
}

void FindBarWidget::cancelSearch() {
    if (!m_searching) return;
    m_searching = false;  // a findFinished() coming later is let go
    m_target->cancelFind();
    m_progress->hide();
    m_count->show();
}

void FindBarWidget::findProgress(qint64 done, qint64 total) {
    if (!m_searching || total <= 0) return;
    m_progress->setValue(int(std::clamp(done, qint64(0), total) * progressSteps / total));
}

void FindBarWidget::findFinished(FindResult result) {
    if (!m_searching) return;  // cancelled here, or another target's
    m_searching = false;
    m_progress->hide();
    m_count->show();
    switch (result) {
    case FindResult::Found: showStatus({}, false); break;
    case FindResult::Wrapped: showStatus(tr("Wrapped"), false); break;
    case FindResult::NotFound: showStatus(tr("No matches"), true); break;
    case FindResult::Invalid: showStatus(tr("Invalid"), true); break;
    case FindResult::Cancelled: showStatus(tr("Cancelled"), false); break;
    }
}

void FindBarWidget::replaceOne() {
    if (!m_target || huge() || !m_target->canReplace()) return;
    const int index = currentMatch();
    if (index < 0) {  // the selection is no match: the next one first
        step(true);
        return;
    }
    const Match match = m_matches.at(index);
    const QString text = m_regex->isChecked() ? expand(m_replacement->text(), match.captures) : m_replacement->text();
    m_target->replace({{rangeOf(match.start, match.end), text}}, tr("Replace"));
    m_researching.stop();
    search();
    selectFrom(match.start + int(text.size()), true);
}

void FindBarWidget::replaceAll() {
    if (!m_target || huge() || !m_target->canReplace() || m_matches.isEmpty()) return;
    QList<TextReplacement> replacements;
    replacements.reserve(m_matches.size());
    for (const Match& match : std::as_const(m_matches))
        replacements << TextReplacement{rangeOf(match.start, match.end),
                                        m_regex->isChecked() ? expand(m_replacement->text(), match.captures)
                                                             : m_replacement->text()};
    const int count = int(replacements.size());
    m_target->replace(replacements, tr("Replace All"));
    m_researching.stop();
    search();
    m_count->setText(tr("%n replaced", nullptr, count));
}

void FindBarWidget::showStatus(const QString& text, bool bad) {
    QPalette palette = m_find->palette();
    palette.setColor(QPalette::Text, bad ? QColor(0xc0, 0x10, 0x10) : this->palette().color(QPalette::Text));
    m_find->setPalette(palette);
    m_count->setText(text);
}

void FindBarWidget::showCount() {
    if (!m_error.isEmpty()) {
        showStatus(tr("Invalid"), true);
        m_count->setToolTip(m_error);
        return;
    }
    m_count->setToolTip({});
    if (m_find->text().isEmpty()) {
        showStatus({}, false);
    } else if (m_matches.isEmpty()) {
        showStatus(tr("No matches"), true);
    } else {
        const int index = currentMatch();
        const QString total = m_matches.size() >= maxMatches ? QStringLiteral("%1+").arg(maxMatches)
                                                             : QString::number(m_matches.size());
        showStatus(index >= 0 ? tr("%1 of %2").arg(index + 1).arg(total) : tr("%1 matches").arg(total), false);
    }
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
            if (m_searching) {  // the first Escape cancels the search
                cancelSearch();
                showStatus(tr("Cancelled"), false);
            } else {
                close();
            }
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
