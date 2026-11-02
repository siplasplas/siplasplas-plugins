// SPDX-License-Identifier: LGPL-3.0-only
// The find plugin: a bar searching and replacing in an spp::EditorView.

#include "FindBarWidget.h"

#include "siplasplas/plugin/FindPlugin.h"

#include <QObject>

namespace spp {

class TextFindPlugin : public QObject, public FindPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID SPP_FIND_PLUGIN_IID)
    Q_INTERFACES(spp::FindPlugin)

public:
    QString name() const override { return QStringLiteral("find"); }
    FindBar* createFindBar(QWidget* parent) override { return new FindBarWidget(parent); }
};

} // namespace spp

#include "FindPluginImpl.moc"
