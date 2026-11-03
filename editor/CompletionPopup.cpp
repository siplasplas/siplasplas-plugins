// SPDX-License-Identifier: LGPL-3.0-only
#include "CompletionPopup.h"

#include <QHeaderView>
#include <QPalette>
#include <QScrollBar>

#include <algorithm>

namespace spp {
namespace {

constexpr int indexRole = Qt::UserRole;
// A server's thousands of items wait: typing narrows them down.
constexpr int maxShown = 100;
constexpr int maxRows = 10;
constexpr int maxWidth = 700;

bool hasLettersInOrder(const QString& text, const QString& letters)
{
    qsizetype from = 0;
    for (const QChar letter : letters) {
        from = text.indexOf(letter, from, Qt::CaseInsensitive);
        if (from < 0) return false;
        ++from;
    }
    return true;
}

} // namespace

CompletionPopup::CompletionPopup(QWidget* parent) : QTreeWidget(parent)
{
    setColumnCount(2);
    setHeaderHidden(true);
    setRootIsDecorated(false);
    setUniformRowHeights(true);
    setFocusPolicy(Qt::NoFocus);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    header()->setStretchLastSection(true);
    connect(this, &QTreeWidget::itemClicked, this, [this] { emit accepted(); });
    hide();
}

bool CompletionPopup::fill(const QList<CompletionItem>& items, const QString& typed)
{
    QList<int> first;
    QList<int> rest;
    for (int i = 0; i < items.size(); ++i) {
        const QString& text = items.at(i).filterText.isEmpty() ? items.at(i).label : items.at(i).filterText;
        if (text.startsWith(typed, Qt::CaseInsensitive)) first << i;
        else if (hasLettersInOrder(text, typed)) rest << i;
    }
    first += rest;
    clear();
    if (first.isEmpty()) return false;
    QColor detailColor = palette().color(QPalette::Text);
    detailColor.setAlpha(140);
    for (const int index : first.mid(0, maxShown)) {
        auto* row = new QTreeWidgetItem(this, {items.at(index).label, items.at(index).detail});
        row->setData(0, indexRole, index);
        row->setForeground(1, detailColor);
        row->setToolTip(0, items.at(index).label);
        row->setToolTip(1, items.at(index).detail);
    }
    setCurrentItem(topLevelItem(0));
    resizeColumnToContents(0);
    return true;
}

int CompletionPopup::chosen() const
{
    return currentItem() ? currentItem()->data(0, indexRole).toInt() : -1;
}

void CompletionPopup::moveChoice(int rows, bool wrap)
{
    const int count = topLevelItemCount();
    if (count == 0) return;
    int row = indexOfTopLevelItem(currentItem()) + rows;
    if (wrap) row = (row + count) % count;
    else row = std::clamp(row, 0, count - 1);
    setCurrentItem(topLevelItem(row));
}

QSize CompletionPopup::wantedSize() const
{
    const int count = topLevelItemCount();
    const int rowHeight = count > 0 ? std::max(1, rowHeightOfFirst()) : 1;
    const int height = std::min(count, maxRows) * rowHeight + 2 * frameWidth();
    int width = 2 * frameWidth() + columnWidth(0);
    int detail = 0;
    for (int i = 0; i < count; ++i)
        detail = std::max(detail, fontMetrics().horizontalAdvance(topLevelItem(i)->text(1)));
    width += detail + 2 * fontMetrics().averageCharWidth();
    if (count > maxRows) width += verticalScrollBar()->sizeHint().width();
    return {std::min(width, maxWidth), height};
}

} // namespace spp
