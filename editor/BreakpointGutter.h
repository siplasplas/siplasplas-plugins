// SPDX-License-Identifier: LGPL-3.0-only
#ifndef SPP_EDITOR_BREAKPOINTGUTTER_H
#define SPP_EDITOR_BREAKPOINTGUTTER_H

#include <qce/IMargin.h>

#include <QList>

#include <functional>

namespace spp {

// The column of breakpoints beside the line numbers: a red dot on each, an
// arrow on the line the debugger stopped at. No width while it is hidden.
// Lines count from 0.
class BreakpointGutter : public qce::IMargin {
public:
    using Clicked = std::function<void(int line)>;

    explicit BreakpointGutter(Clicked clicked);

    bool isShown() const { return m_shown; }
    void setShown(bool shown) { m_shown = shown; }

    // In ascending order, each line once.
    const QList<int>& lines() const { return m_lines; }
    void setLines(const QList<int>& lines) { m_lines = lines; }
    bool hasLine(int line) const;

    // -1 for none.
    int executionLine() const { return m_executionLine; }
    void setExecutionLine(int line) { m_executionLine = line; }

    int preferredWidth(const qce::ViewportState& vp) const override;
    void paint(QPainter& painter, const qce::ViewportState& vp, const QRect& marginRect) override;
    void mousePressed(const QPoint& local, const qce::ViewportState& vp, const QRect& marginRect) override;

private:
    Clicked m_clicked;
    bool m_shown = false;
    QList<int> m_lines;
    int m_executionLine = -1;
};

} // namespace spp

#endif // SPP_EDITOR_BREAKPOINTGUTTER_H
