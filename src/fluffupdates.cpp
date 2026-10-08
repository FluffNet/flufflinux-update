#include "fluffupdates.h"
#include "flu_bridge/src/backend.cxxqt.h"

#include <KPluginFactory>
#include <QApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QWidget>

K_PLUGIN_CLASS_WITH_JSON(FluffUpdates, "kcm_fluffupdates.json")

FluffUpdates::FluffUpdates(QObject *parent, const KPluginMetaData &data)
    : KQuickConfigModule(parent, data)
    , m_backend(new flu::UpdateBackend)
{
    m_backend->setParent(this);
    setButtons(NoAdditionalButton);

    // kcmshell6 embeds the Quick page inside a QWidget host.
    connect(this, &KQuickConfigModule::mainUiReady, this, [this] {
        QQuickItem *item = mainUi();
        if (!item) {
            return;
        }
        const QString title = m_backend->hostTitle();
        const auto applyQuick = [item, title] {
            if (auto *window = item->window()) {
                window->setMinimumSize(QSize(qMax(window->minimumWidth(), 480),
                                             qMax(window->minimumHeight(), 400)));
                window->setTitle(title);
            }
        };
        applyQuick();
        connect(item, &QQuickItem::windowChanged, item,
                [applyQuick](QQuickWindow *) { applyQuick(); });

        const auto applyWidget = [this, title] {
            QWidget *host = nullptr;
            for (QObject *ancestor = this->parent(); ancestor; ancestor = ancestor->parent()) {
                if (auto *widget = qobject_cast<QWidget *>(ancestor)) {
                    host = widget->window();
                }
            }
            if (!host) {
                host = QApplication::activeWindow();
            }
            if (host) {
                host->setMinimumSize(QSize(qMax(host->minimumWidth(), 480),
                                           qMax(host->minimumHeight(), 400)));
                host->setWindowTitle(title);
            }
        };
        applyWidget();
        QTimer::singleShot(0, this, applyWidget);
        QTimer::singleShot(250, this, applyWidget);
    });

    auto *networkTimer = new QTimer(this);
    networkTimer->setInterval(1000);
    connect(networkTimer, &QTimer::timeout, m_backend, &flu::UpdateBackend::pollNetwork);
    networkTimer->start();
}

QObject *FluffUpdates::backend() const
{
    return m_backend;
}

#include "fluffupdates.moc"
