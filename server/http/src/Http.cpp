#include "neothemis/server/Http.hpp"

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace neothemis::server::http {
namespace {

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr std::size_t kMaxBodyBytes = 512 * 1024;
constexpr int kReadTimeoutMs = 10 * 1000;

ParseResult parse_error(const QString& message) {
    ParseResult result;
    result.state = ParseState::Error;
    result.error = message;
    return result;
}

QString form_decode(QByteArray value) {
    value.replace('+', ' ');
    return QUrl::fromPercentEncoding(value);
}

bool invalid_line_endings(const QByteArray& bytes, int length, bool allow_trailing_carriage_return) {
    for (int i = 0; i < length; ++i) {
        const char ch = bytes[i];
        if (ch == '\n') {
            if (i == 0 || bytes[i - 1] != '\r') {
                return true;
            }
        } else if (ch == '\r') {
            if (i + 1 >= length) {
                return !allow_trailing_carriage_return;
            }
            if (bytes[i + 1] != '\n') {
                return true;
            }
        }
    }
    return false;
}

bool is_header_name_character(unsigned char ch) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9')) {
        return true;
    }
    switch (ch) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
        case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

bool valid_header_name(const QByteArray& name) {
    return !name.isEmpty() &&
           std::all_of(name.begin(), name.end(), [](char ch) {
               return is_header_name_character(static_cast<unsigned char>(ch));
           });
}

bool valid_header_value(const QByteArray& value) {
    return std::all_of(value.begin(), value.end(), [](char ch) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        return byte == '\t' || (byte >= 0x20 && byte != 0x7f);
    });
}

QByteArray trim_optional_whitespace(QByteArray value) {
    while (!value.isEmpty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove(0, 1);
    }
    while (!value.isEmpty() && (value.back() == ' ' || value.back() == '\t')) {
        value.chop(1);
    }
    return value;
}

bool valid_request_target(const QByteArray& target) {
    if (target.isEmpty() || !target.startsWith('/')) {
        return false;
    }
    return std::all_of(target.begin(), target.end(), [](char ch) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        return byte > 0x20 && byte != 0x7f;
    });
}

class HttpConnection final : public QObject {
public:
    HttpConnection(QTcpSocket* socket, RequestHandler handler)
        : socket_(socket), handler_(std::move(handler)) {
        QObject::connect(socket_, &QTcpSocket::readyRead, this, [this]() { read_available(); });
        QObject::connect(socket_, &QTcpSocket::disconnected, this, [this]() {
            socket_->deleteLater();
            deleteLater();
        });
        read_timeout_.setSingleShot(true);
        QObject::connect(&read_timeout_, &QTimer::timeout, this, [this]() {
            finish(error_response(408, "request timeout"));
        });
        read_timeout_.start(kReadTimeoutMs);
    }

private:
    void read_available() {
        if (finished_) {
            socket_->readAll();
            return;
        }
        buffer_ += socket_->readAll();
        ParseResult result = parse_request(buffer_);
        if (result.state == ParseState::Incomplete) {
            return;
        }
        if (result.state == ParseState::Error) {
            finish(error_response(400, result.error));
            return;
        }
        // Set the guard before invoking application code. A handler can spin a
        // nested event loop, allowing more readyRead signals to arrive.
        finished_ = true;
        read_timeout_.stop();
        send_and_close(handler_(result.request));
    }

    void finish(const HttpResponse& response) {
        if (finished_) {
            return;
        }
        finished_ = true;
        read_timeout_.stop();
        send_and_close(response);
    }

    void send_and_close(const HttpResponse& response) {
        socket_->write(serialize_response(response));
        socket_->disconnectFromHost();
    }

    QTcpSocket* socket_ = nullptr;
    RequestHandler handler_;
    QByteArray buffer_;
    QTimer read_timeout_;
    bool finished_ = false;
};

} // namespace

ParseResult parse_request(const QByteArray& buffer) {
    if (static_cast<std::size_t>(buffer.size()) > kMaxHeaderBytes + kMaxBodyBytes) {
        return parse_error("request too large");
    }

    const int header_end = buffer.indexOf("\r\n\r\n");
    if (header_end < 0) {
        if (invalid_line_endings(buffer, buffer.size(), true)) {
            return parse_error("invalid line ending");
        }
        if (static_cast<std::size_t>(buffer.size()) > kMaxHeaderBytes) {
            return parse_error("headers too large");
        }
        return {};
    }
    if (static_cast<std::size_t>(header_end + 4) > kMaxHeaderBytes) {
        return parse_error("headers too large");
    }
    if (invalid_line_endings(buffer, header_end + 4, false)) {
        return parse_error("invalid line ending");
    }

    const QByteArray header_bytes = buffer.left(header_end);
    QList<QByteArray> lines = header_bytes.split('\n');
    if (lines.isEmpty()) {
        return parse_error("bad request");
    }
    for (QByteArray& line : lines) {
        if (line.endsWith('\r')) {
            line.chop(1);
        }
    }

    const QList<QByteArray> request_line = lines[0].split(' ');
    if (request_line.size() != 3 || request_line[0].isEmpty() ||
        !valid_request_target(request_line[1]) ||
        request_line[2] != "HTTP/1.1") {
        return parse_error("bad request line");
    }

    ParseResult result;
    HttpRequest& request = result.request;
    request.method = QString::fromLatin1(request_line[0]);
    const QUrl url(QString::fromUtf8(request_line[1]));
    request.path = url.path();
    request.query = QUrlQuery(url);
    if (request.method != "GET" && request.method != "POST") {
        return parse_error("unsupported method");
    }

    bool has_content_length = false;
    QByteArray content_length_header;
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray& line = lines[i];
        if (line.isEmpty() || line.front() == ' ' || line.front() == '\t') {
            return parse_error("malformed header");
        }
        const int colon = line.indexOf(':');
        if (colon <= 0 || !valid_header_name(line.left(colon))) {
            return parse_error("malformed header");
        }
        const QByteArray name = line.left(colon);
        const QByteArray lower_name = name.toLower();
        const QByteArray raw_value = line.mid(colon + 1);
        if (!valid_header_value(raw_value)) {
            return parse_error("malformed header");
        }
        const QByteArray value = trim_optional_whitespace(raw_value);
        if (lower_name == "transfer-encoding") {
            return parse_error("transfer encoding is not supported");
        }
        if (lower_name == "content-length") {
            if (has_content_length) {
                return parse_error("duplicate content length");
            }
            has_content_length = true;
            content_length_header = value;
        }
        request.headers[QString::fromLatin1(name)] = QString::fromUtf8(value);
    }

    std::size_t content_length = 0;
    if (has_content_length) {
        const bool all_digits = !content_length_header.isEmpty() &&
                                std::all_of(content_length_header.begin(),
                                            content_length_header.end(), [](char ch) {
                                                return ch >= '0' && ch <= '9';
                                            });
        bool ok = false;
        const qulonglong parsed_length = content_length_header.toULongLong(&ok, 10);
        if (!all_digits || !ok) {
            return parse_error("invalid content length");
        }
        if (parsed_length > static_cast<qulonglong>(kMaxBodyBytes)) {
            return parse_error("body too large");
        }
        content_length = static_cast<std::size_t>(parsed_length);
    }
    const int total = header_end + 4 + static_cast<int>(content_length);
    if (buffer.size() < total) {
        return result;
    }
    if (buffer.size() > total) {
        return parse_error("unexpected trailing data");
    }

    request.body = buffer.mid(header_end + 4, static_cast<int>(content_length));
    result.state = ParseState::Complete;
    return result;
}

QByteArray serialize_response(const HttpResponse& response) {
    QByteArray framed;
    framed += "HTTP/1.1 " + QByteArray::number(response.status) + " " +
              response.reason.toUtf8() + "\r\n";
    framed += "Content-Type: " + response.content_type.toUtf8() + "\r\n";
    framed += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
    framed += "Connection: close\r\n";
    framed += "X-Content-Type-Options: nosniff\r\n";
    framed += "Referrer-Policy: same-origin\r\n";
    framed += "Content-Security-Policy: default-src 'self'; style-src 'unsafe-inline'\r\n";
    for (const QString& extra : response.headers) {
        framed += extra.toUtf8() + "\r\n";
    }
    framed += "\r\n";
    framed += response.body;
    return framed;
}

QString header_value(const HttpRequest& request, const QString& key) {
    const QString wanted = key.toLower();
    for (const auto& [name, value] : request.headers) {
        if (name.toLower() == wanted) {
            return value;
        }
    }
    return {};
}

QString cookie_value(const HttpRequest& request, const QString& key) {
    const QString cookie = header_value(request, "cookie");
    for (const QString& part : cookie.split(';')) {
        const QString trimmed = part.trimmed();
        const int equal = trimmed.indexOf('=');
        if (equal > 0 && trimmed.left(equal) == key) {
            return trimmed.mid(equal + 1);
        }
    }
    return {};
}

FormFields form_body(const HttpRequest& request) {
    FormFields fields;
    const QList<QByteArray> pairs = request.body.split('&');
    for (const QByteArray& pair : pairs) {
        const int equal = pair.indexOf('=');
        const QByteArray key = equal < 0 ? pair : pair.left(equal);
        const QByteArray value = equal < 0 ? QByteArray() : pair.mid(equal + 1);
        fields[form_decode(key)] = form_decode(value);
    }
    return fields;
}

QString form_value(const FormFields& form, const QString& key) {
    const auto found = form.find(key);
    return found == form.end() ? QString() : found->second;
}

QString html_escape(QString value) {
    value.replace('&', "&amp;");
    value.replace('<', "&lt;");
    value.replace('>', "&gt;");
    value.replace('"', "&quot;");
    value.replace('\'', "&#39;");
    return value;
}

HttpResponse html_response(const QString& html) {
    HttpResponse response;
    response.body = html.toUtf8();
    return response;
}

HttpResponse redirect_response(const QString& location) {
    HttpResponse response;
    response.status = 303;
    response.reason = "See Other";
    response.headers.push_back("Location: " + location);
    response.body = "Redirect";
    return response;
}

HttpResponse error_response(int status, const QString& message) {
    HttpResponse response;
    response.status = status;
    response.reason = status == 400 ? "Bad Request"
                    : status == 401 ? "Unauthorized"
                    : status == 403 ? "Forbidden"
                    : status == 404 ? "Not Found"
                    : status == 408 ? "Request Timeout"
                    : "Error";
    response.body = ("<h1>" + QString::number(status) + "</h1><p>" +
                     html_escape(message) + "</p>").toUtf8();
    return response;
}

void serve_connection(QTcpSocket* socket, RequestHandler handler) {
    new HttpConnection(socket, std::move(handler));
}

} // namespace neothemis::server::http
