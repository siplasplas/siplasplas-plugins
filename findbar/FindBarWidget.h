// SPDX-License-Identifier: LGPL-3.0-only
#ifndef SPP_FINDBAR_FINDBARWIDGET_H
#define SPP_FINDBAR_FINDBARWIDGET_H

#include "siplasplas/plugin/FindPlugin.h"

#include <QList>
#include <QStringList>
#include <QTimer>
#include <QWidget>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QToolButton;

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
    void setEditor(EditorView* editor) override;
    void showFind() override;
    void showReplace() override;
    void findNext() override { step(true); }
    void findPrevious() override { step(false); }
    void editorTextChanged() override;

    // The replacement of a regular expression's match: $1 or \1 its groups
    // ($0 the whole match), \n a line break, \t a tab, \\ and $$ themselves.
    static QString expand(const QString& replacement, const QStringList& captures);

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
    FindQuery query() const;
    void open(bool replacing);
    void close();
    // The options or the text sought changed.
    void queryChanged();
    // Seeks the matches in the target's text (a huge one is given the query
    // to highlight); false for an invalid pattern.
    bool search();
    // Selects the match nearest after (or before) `offset`, around the end
    // when wrapping around.
    void selectFrom(int offset, bool forward);
    void step(bool forward);
    // Stops a huge target's search running.
    void cancelSearch();
    void replaceOne();
    void replaceAll();
    void showCount();
    void showStatus(const QString& text, bool bad);
    void updateReplaceRow();
    TextRange rangeOf(int start, int end) const;
    int offsetOf(TextPosition position) const;
    int currentMatch() const;

    QLineEdit* m_find = nullptr;
    QToolButton* m_wrap = nullptr;
    QToolButton* m_matchCase = nullptr;
    QToolButton* m_wholeWords = nullptr;
    QToolButton* m_regex = nullptr;
    QLabel* m_count = nullptr;
    QProgressBar* m_progress = nullptr;
    QWidget* m_replaceRow = nullptr;
    QLineEdit* m_replacement = nullptr;
    QPushButton* m_replaceButton = nullptr;
    QPushButton* m_replaceAllButton = nullptr;

    FindTarget* m_target = nullptr;
    bool m_replacing = false;  // shown for replacing, where the target can
    bool m_searching = false;  // a huge target's find() runs
    QString m_text;            // the target's text searched
    QList<int> m_lineStarts;   // offsets of its lines
    QList<Match> m_matches;
    QString m_error;           // of an invalid pattern
    QTimer m_researching;      // after edits in the target
};

} // namespace spp

#endif // SPP_FINDBAR_FINDBARWIDGET_H
