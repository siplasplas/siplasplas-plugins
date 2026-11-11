// SPDX-License-Identifier: LGPL-3.0-only
#ifndef SPP_FINDBAR_FINDBARWIDGET_H
#define SPP_FINDBAR_FINDBARWIDGET_H

#include "siplasplas/plugin/FindPlugin.h"

#include <QElapsedTimer>
#include <QList>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QWidget>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace spp {

// The find bar: the text sought with its options and the number of matches
// (or, in a huge target, the progress of the search); for replacing, the
// replacement with Replace and Replace All.
class FindBarWidget : public QWidget, public FindBar, private FindTargetListener {
    Q_OBJECT
public:
    explicit FindBarWidget(QWidget* parent = nullptr);
    ~FindBarWidget() override;

    QWidget* widget() override { return this; }
    void setTarget(FindTarget* target) override;
    void setPlacement(FindBarPlacement placement) override;
    void setEditor(EditorView* editor) override;
    void showFind() override;
    void showReplace() override;
    void findNext() override { step(true); }
    void findPrevious() override { step(false); }
    void editorTextChanged() override;

    // The replacement of a regular expression's match: $1 or \1 its groups
    // ($0 the whole match, all the digits read; one that does not exist
    // stays as written), ${name} a named group (`names` as
    // QRegularExpression::namedCaptureGroups()), \n a line break, \t a tab,
    // \r a carriage return, \\ and $$ themselves.
    static QString expand(const QString& replacement, const QStringList& captures, const QStringList& names = {});
    // The replacement in the case of the text replaced: all capitals, all
    // small, or a capital first; otherwise as written.
    static QString followCase(const QString& replacement, const QString& original);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Match {
        int start = 0;  // offsets in the target's text
        int end = 0;
        QStringList captures;
    };

    void findProgress(qint64 done, qint64 total) override;
    void findFinished(FindResult result) override;

    bool huge() const { return m_target && m_target->isHuge(); }
    // Shows or hides the bar (this widget and, on top, its panel).
    void setOpen(bool open);
    // On top of the target, with the panel over it.
    bool overlay() const;
    // Puts the panel in this widget, or over the top of the target (covering
    // it, or over this widget taking room above it).
    void place();
    void setCovering(bool covering);
    // On top: the selection (a match) is not left under the bar; among the
    // first rows of the text, they move down.
    void revealSelection();
    FindQuery query() const;
    void open(bool replacing);
    void close();
    // The options or the text sought changed.
    void queryChanged();
    // "Search in selection" turned on or off.
    void scopeToggled(bool enabled);
    // Searching in the selection captured (not in a huge target).
    bool scoped() const;
    // The target's text and its line starts, the selection searched in
    // following an edit since the last time.
    void takeText();
    // Seeks the matches in the target's text (a huge one is given the query
    // to highlight); false for an invalid pattern.
    bool search();
    // Selects the match nearest after (or before) `offset`, around the end
    // when wrapping around; false when none is.
    bool selectFrom(int offset, bool forward);
    // Highlights the matches but the one selected; none while closed.
    void updateHighlights();
    // A tooltip on the target, for F3 with the bar closed.
    void showNotFound();
    void step(bool forward);
    // Stops a huge target's search running.
    void cancelSearch();
    void replaceOne();
    void replaceAll();
    // Leaves the selected match out of Replace All, selecting the next one.
    void excludeCurrent();
    QString replacementFor(const Match& match) const;
    void showCount();
    void showStatus(const QString& text, bool bad);
    void updateReplaceRow();
    TextRange rangeOf(int start, int end) const;
    int offsetOf(TextPosition position) const;
    int currentMatch() const;

    QPointer<QWidget> m_panel;  // the controls
    QVBoxLayout* m_slotLayout = nullptr;
    FindBarPlacement m_placement = FindBarPlacement::Bottom;
    bool m_open = false;
    bool m_covering = false;  // on top: over the text, not above it
    bool m_placing = false;
    QLineEdit* m_find = nullptr;
    QToolButton* m_wrap = nullptr;
    QToolButton* m_matchCase = nullptr;
    QToolButton* m_wholeWords = nullptr;
    QToolButton* m_regex = nullptr;
    QToolButton* m_scope = nullptr;
    QLabel* m_count = nullptr;
    QProgressBar* m_progress = nullptr;
    QToolButton* m_expand = nullptr;
    QWidget* m_replaceRow = nullptr;
    QToolButton* m_preserveCase = nullptr;
    QPushButton* m_excludeButton = nullptr;
    QLineEdit* m_replacement = nullptr;
    QPushButton* m_replaceButton = nullptr;
    QPushButton* m_replaceAllButton = nullptr;

    FindTarget* m_target = nullptr;
    bool m_replacing = false;  // the Replace row is open, where the target can replace
    bool m_searching = false;  // a huge target's find() runs
    bool m_shownForSearch = false;  // opened by a long F3 search, closed after it
    QElapsedTimer m_searchStarted;
    QString m_text;            // the target's text searched
    QList<int> m_lineStarts;   // offsets of its lines
    QList<Match> m_matches;
    QList<Match> m_excluded;   // left out of the matches until the query or the text changes
    QStringList m_captureNames;
    QString m_error;           // of an invalid pattern
    bool m_scopeAvailable = false;  // a selection captured to search in
    int m_scopeStart = 0;           // its offsets in m_text
    int m_scopeEnd = 0;
    QTimer m_researching;      // after edits in the target
};

} // namespace spp

#endif // SPP_FINDBAR_FINDBARWIDGET_H
