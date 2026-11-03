// SPDX-License-Identifier: LGPL-3.0-only
#ifndef SPP_EDITOR_COMPLETIONPOPUP_H
#define SPP_EDITOR_COMPLETIONPOPUP_H

#include "siplasplas/plugin/EditorPlugin.h"

#include <QTreeWidget>

namespace spp {

// The list of completions over the text, the current one chosen: labels with
// their details. It never takes the focus: keys stay with the editor, which
// passes on those meant for the list.
class CompletionPopup : public QTreeWidget {
    Q_OBJECT
public:
    explicit CompletionPopup(QWidget* parent);

    // Those of `items` matching what was typed: those starting with it first,
    // then those having its letters in order, each kept in the given order;
    // no case. False when none does.
    bool fill(const QList<CompletionItem>& items, const QString& typed);
    // The index in the given items of the chosen one.
    int chosen() const;
    // Moves the choice, around the ends when `wrap`.
    void moveChoice(int rows, bool wrap);
    // Its size for what it shows.
    QSize wantedSize() const;

signals:
    void accepted();

private:
    int rowHeightOfFirst() const { return rowHeight(indexFromItem(topLevelItem(0))); }
};

} // namespace spp

#endif // SPP_EDITOR_COMPLETIONPOPUP_H
