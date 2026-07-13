#pragma once

#include <QByteArray>
#include <QString>
#include <QUrlQuery>

#include <functional>
#include <map>
#include <vector>

class QTcpSocket;

namespace neothemis::server::http {

struct HttpRequest {
    QString method;
    QString path;
    QUrlQuery query;
    std::map<QString, QString> headers;
    QByteArray body;
};

struct HttpResponse {
    int status = 200;
    QString reason = "OK";
    QString content_type = "text/html; charset=utf-8";
    QByteArray body;
    std::vector<QString> headers;
};

enum class ParseState {
    Incomplete,
    Complete,
    Error
};

struct ParseResult {
    ParseState state = ParseState::Incomplete;
    HttpRequest request;
    QString error;
};

using FormFields = std::map<QString, QString>;
using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

ParseResult parse_request(const QByteArray& buffer);
QByteArray serialize_response(const HttpResponse& response);

QString header_value(const HttpRequest& request, const QString& key);
QString cookie_value(const HttpRequest& request, const QString& key);
FormFields form_body(const HttpRequest& request);
QString form_value(const FormFields& form, const QString& key);
QString html_escape(QString value);

HttpResponse html_response(const QString& html);
HttpResponse redirect_response(const QString& location);
HttpResponse error_response(int status, const QString& message);

void serve_connection(QTcpSocket* socket, RequestHandler handler);

} // namespace neothemis::server::http
