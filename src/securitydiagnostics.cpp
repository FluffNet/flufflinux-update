#include "securitydiagnostics.h"

#include <QRegularExpression>
#include <QUrlQuery>

QString sanitizedDiagnosticValue(const QString &value, int maximumLength)
{
    QString sanitized = value;
    sanitized.replace(
        QRegularExpression(QStringLiteral("[\\r\\n\\x00-\\x1f]")),
        QStringLiteral(" "));
    sanitized.replace(
        QRegularExpression(
            QStringLiteral("(?:/home/|/Users/)[^\\s]+"),
            QRegularExpression::CaseInsensitiveOption),
        QStringLiteral("[redacted-path]"));
    sanitized.replace(
        QRegularExpression(
            QStringLiteral("(?:password|passwd|token|secret|authorization)"
                           "\\s*[:=]\\s*[^\\s]+"),
            QRegularExpression::CaseInsensitiveOption),
        QStringLiteral("[redacted-secret]"));
    return sanitized.simplified().left(maximumLength);
}

QUrl signingKeyIssueUrl(const SigningKeyIssueDetails &details)
{
    const QString body = QStringLiteral(
        "Repository: %1\nFLU version: %2\nOS version: %3\nExpected fingerprint: %4\n"
        "Received fingerprint: %5\nRequested signing-subkey fingerprint: %6\n"
        "Failure category: %7\nPacman exit status: %8")
        .arg(sanitizedDiagnosticValue(details.repository, 64),
             sanitizedDiagnosticValue(details.fluVersion, 32),
             sanitizedDiagnosticValue(details.osVersion, 160),
             sanitizedDiagnosticValue(details.expectedFingerprint, 40),
             sanitizedDiagnosticValue(details.receivedFingerprint, 40),
             sanitizedDiagnosticValue(details.requestedFingerprint, 40),
             sanitizedDiagnosticValue(details.failureCategory, 128),
             QString::number(details.pacmanExitStatus));
    QUrl url(QStringLiteral(
        "https://github.com/FluffNet/flufflinux-update/issues/new"));
    QUrlQuery query;
    query.addQueryItem(
        QStringLiteral("title"),
        QStringLiteral("Repository signing-key verification failed"));
    query.addQueryItem(QStringLiteral("body"), body);
    url.setQuery(query);
    return url;
}

bool openSigningKeyIssueUrl(
    const QUrl &url, const std::function<bool(const QUrl &)> &opener)
{
    if (!opener || url.scheme() != QStringLiteral("https")
        || url.host() != QStringLiteral("github.com")
        || url.path() != QStringLiteral(
            "/FluffNet/flufflinux-update/issues/new")) {
        return false;
    }
    return opener(url);
}
