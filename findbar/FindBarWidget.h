// SPDX-License-Identifier: LGPL-3.0-only
#ifndef SPP_FINDBAR_FINDBARWIDGET_H
#define SPP_FINDBAR_FINDBARWIDGET_H

#include "siplasplas/plugin/FindPlugin.h"

#include <QList>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;

namespace spp {

// The find bar: the text sought with its options and the number of matches;
// for replacing, the replacement with Replace and Replace All.
class FindBarWidget : public QWidget, public FindBar {
    Q_OBJECT
public:
    explicit FindBarWidget(QWidget* parent = nullptr);

    QWidget* widget() override { return this; }
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
        int start = 0;  // offsets in the editor's text
        int end = 0;
        QStringList captures;
    };

    void open(bool replacing);
    void close();
    // Seeks the matches in the editor's text; false for an invalid pattern.
    bool search();
    // Selects the match nearest after (or before) `offset`, around the end.
    void selectFrom(int offset, bool forward);
    void step(bool forward);
    void replaceOne();
    void replaceAll();
    void showCount();
    TextRange rangeOf(int start, int end) const;
    int offsetOf(TextPosition position) const;
    int currentMatch() const;

    QLineEdit* m_find = nullptr;
    QToolButton* m_matchCase = nullptr;
    QToolButton* m_wholeWords = nullptr;
    QToolButton* m_regex = nullptr;
    QLabel* m_count = nullptr;
    QWidget* m_replaceRow = nullptr;
    QLineEdit* m_replacement = nullptr;
    QPushButton* m_replaceButton = nullptr;
    QPushButton* m_replaceAllButton = nullptr;

    EditorView* m_editor = nullptr;
    QString m_text;            // the editor's text searched
    QList<int> m_lineStarts;   // offsets of its lines
    QList<Match> m_matches;
    QString m_error;           // of an invalid pattern
    QTimer m_researching;      // after edits in the editor
};

} // namespace spp

#endif // SPP_FINDBAR_FINDBARWIDGET_H
