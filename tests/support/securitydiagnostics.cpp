#include "securitydiagnostics.h"
#include "flu_test_bridge/src/recovery.cxx.h"
QString sanitizedDiagnosticValue(const QString &value,int maximumLength) {const auto result=flu::diagnostic_value(value.toStdString(),qMax(0,maximumLength));return QString::fromUtf8(result.data(),result.size());}
QUrl signingKeyIssueUrl(const SigningKeyIssueDetails &details) {
    rust::Vec<rust::String> fields;
    for(const auto &value: {details.repository,details.fluVersion,details.osVersion,details.expectedFingerprint,details.receivedFingerprint,details.requestedFingerprint,details.failureCategory})fields.push_back(value.toStdString());
    const auto result=flu::diagnostic_issue_url(rust::Slice<const rust::String>(fields.data(),fields.size()),details.pacmanExitStatus);
    return QUrl(QString::fromUtf8(result.data(),result.size()));
}
bool openSigningKeyIssueUrl(const QUrl &url,const std::function<bool(const QUrl &)> &opener) {return opener && flu::diagnostic_issue_url_allowed(url.toString(QUrl::FullyEncoded).toStdString()) && opener(url);}
