// SPDX-License-Identifier: LGPL-3.0-only
// The editor plugin: qcodeedit's editor as an spp::EditorView.

#include "EditorWidget.h"

#include "siplasplas/plugin/EditorPlugin.h"

#include <qce/encoding/Encoding.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>

#include <dlfcn.h>

namespace spp {
namespace {

// The directory of this plugin's file.
QString pluginDirectory()
{
    Dl_info info{};
    static const int anchor = 0;
    if (!dladdr(&anchor, &info) || !info.dli_fname) return {};
    return QFileInfo(QFile::decodeName(info.dli_fname)).absolutePath();
}

} // namespace

class QcodeeditEditorPlugin : public QObject, public EditorPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID SPP_EDITOR_PLUGIN_IID)
    Q_INTERFACES(spp::EditorPlugin)

public:
    QcodeeditEditorPlugin()
    {
        // cpg's detection data installed next to the plugin; otherwise the
        // place it had when the plugin was built (the build tree).
        const QDir data(pluginDirectory() + QStringLiteral("/spp-editor/cpg"));
        if (data.exists(QStringLiteral("languages.txt")))
            qce::encoding::setDetectionData(data.filePath(QStringLiteral("languages.txt")),
                                            data.filePath(QStringLiteral("models")));
        qce::encoding::preloadDetectionData();
    }

    QString name() const override { return QStringLiteral("qcodeedit"); }

    EditorView* createEditor(QWidget* parent) override { return new EditorWidget(parent); }
};

} // namespace spp

#include "EditorPluginImpl.moc"
