#include "nativeqt.h"

#include <KLocalizedString>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkInformation>
#include <QNetworkInterface>
#include <QStandardPaths>
#include <QUrl>

namespace flu {
static QString string(::rust::Str value)
{
    return QString::fromUtf8(value.data(), value.size());
}

::rust::String translate(::rust::Str message, ::rust::Str plural, std::int64_t count,
                         const ::rust::Vec<::rust::String> &arguments)
{
    const QByteArray singular = string(message).toUtf8();
    const QByteArray multiple = string(plural).toUtf8();
    auto value = multiple.isEmpty()
        ? ki18nd("kcm_fluffupdates", singular.constData())
        : ki18ndp("kcm_fluffupdates", singular.constData(), multiple.constData());
    if (!multiple.isEmpty()) {
        value = value.subs(static_cast<qlonglong>(count));
    }
    for (const auto &argument : arguments) {
        value = value.subs(QString::fromUtf8(argument.data(), argument.size()));
    }
    return value.toString().toStdString();
}

::rust::String config_directory()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation).toStdString();
}

void initialize_locale(const ::rust::Vec<::rust::String> &values)
{
    KLocalizedString::addDomainLocaleDir(QByteArrayLiteral("kcm_fluffupdates"),
                                         QStringLiteral(FLUFFLINUX_LOCALE_DIR));
    QStringList languages;
    for (const auto &value : values) {
        languages.append(QString::fromUtf8(value.data(), value.size()));
    }
    if (!languages.isEmpty()) {
        KLocalizedString::setLanguages(languages);
    }
}

QList<QVariant> variants(::rust::Str json)
{
    return QJsonDocument::fromJson(string(json).toUtf8()).array().toVariantList();
}

void copy_text(const QString &text)
{
    QApplication::clipboard()->setText(text);
}

void open_url(::rust::Str url)
{
    QDesktopServices::openUrl(QUrl(string(url)));
}

std::int32_t reachability()
{
    if (!QNetworkInformation::instance()) {
        QNetworkInformation::loadDefaultBackend();
    }
    auto *information = QNetworkInformation::instance();
    return information ? static_cast<std::int32_t>(information->reachability()) : 0;
}

::rust::Vec<std::uint32_t> interface_flags()
{
    ::rust::Vec<std::uint32_t> result;
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        result.push_back(static_cast<std::uint32_t>(interface.flags().toInt()));
    }
    return result;
}
}
