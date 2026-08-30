#pragma once

#include <QString>
#include <QUrl>

#include <functional>

struct SigningKeyIssueDetails
{
    QString repository;
    QString fluVersion;
    QString osVersion;
    QString expectedFingerprint;
    QString receivedFingerprint;
    QString requestedFingerprint;
    QString failureCategory;
    int pacmanExitStatus = -1;
};

QString sanitizedDiagnosticValue(const QString &value, int maximumLength);
QUrl signingKeyIssueUrl(const SigningKeyIssueDetails &details);
bool openSigningKeyIssueUrl(
    const QUrl &url, const std::function<bool(const QUrl &)> &opener);
