// SPDX-License-Identifier: LGPL-3.0-only
#include "BreakpointGutter.h"

#include <qce/ViewportState.h>

#include <QPainter>
#include <QPolygonF>
#include <QRect>

#include <algorithm>

namespace spp {

BreakpointGutter::BreakpointGutter(Clicked clicked) : m_clicked(std::move(clicked)) {}

bool BreakpointGutter::hasLine(int line) const
{
    return std::binary_search(m_lines.begin(), m_lines.end(), line);
}

int BreakpointGutter::preferredWidth(const qce::ViewportState& vp) const
{
    return m_shown ? std::max(14, vp.lineHeight) : 0;
}

void BreakpointGutter::paint(QPainter& painter, const qce::ViewportState& vp, const QRect& marginRect)
{
    if (!m_shown || !vp.isValid()) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const qreal size = std::min(marginRect.width(), vp.lineHeight) - 4.0;
    const qreal centerX = marginRect.left() + marginRect.width() / 2.0;
    for (int i = 0; i < vp.rows.size(); ++i) {
        const auto& row = vp.rows[i];
        if (!row.isFirstRow) continue;
        const qreal centerY = marginRect.top() + vp.contentOffsetY + i * vp.lineHeight + vp.lineHeight / 2.0;
        if (hasLine(row.logicalLine)) {
            painter.setBrush(QColor(0xdb, 0x3a, 0x3a));
            painter.drawEllipse(QPointF(centerX, centerY), size / 2, size / 2);
        }
        if (row.logicalLine == m_executionLine) {
            // With a white edge, to stand out on a breakpoint's dot.
            painter.setPen(QPen(Qt::white, 1));
            painter.setBrush(QColor(0x2f, 0x7f, 0xe0));
            painter.drawPolygon(QPolygonF{{centerX - size / 2, centerY - size / 2},
                                          {centerX + size / 2, centerY},
                                          {centerX - size / 2, centerY + size / 2}});
            painter.setPen(Qt::NoPen);
        }
    }
    painter.restore();
}

void BreakpointGutter::mousePressed(const QPoint& local, const qce::ViewportState& vp, const QRect& marginRect)
{
    if (!m_shown || !vp.isValid()) return;
    const int y = local.y() - marginRect.top() - vp.contentOffsetY;
    if (y < 0) return;
    const int row = y / vp.lineHeight;
    // Any row of a wrapped line means that line.
    if (row < vp.rows.size()) m_clicked(vp.rows[row].logicalLine);
}

} // namespace spp
