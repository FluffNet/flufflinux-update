#pragma once

#include <KQuickConfigModule>

namespace flu {
class UpdateBackend;
}

// KDE's native entry point; application logic and state live in Rust.
class FluffUpdates final : public KQuickConfigModule
{
    Q_OBJECT
    Q_PROPERTY(QObject *backend READ backend CONSTANT)

public:
    explicit FluffUpdates(QObject *parent, const KPluginMetaData &data);
    QObject *backend() const;

private:
    flu::UpdateBackend *m_backend;
};
