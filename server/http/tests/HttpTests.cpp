#include "neothemis/server/Http.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace http = neothemis::server::http;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool wait_until(const std::function<bool()>& predicate, int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

void test_incomplete_and_complete_request() {
    http::ParseResult partial = http::parse_request("GET /submit?problem=A HTTP/1.1\r\nHost: local");
    require(partial.state == http::ParseState::Incomplete,
            "partial headers should remain incomplete");

    http::ParseResult parsed = http::parse_request(
        "GET /submit?problem=A HTTP/1.1\r\n"
        "Host: local\r\n"
        "Cookie: mode=test; NTSID=session-token\r\n\r\n");
    require(parsed.state == http::ParseState::Complete, "GET request should parse");
    require(parsed.request.method == "GET", "method should be normalized");
    require(parsed.request.path == "/submit", "path should be separated from query");
    require(parsed.request.query.queryItemValue("problem") == "A", "query should parse");
    require(http::header_value(parsed.request, "HOST") == "local",
            "header lookup should be case-insensitive");
    require(http::cookie_value(parsed.request, "NTSID") == "session-token",
            "cookie should parse");
}

void test_post_body_and_forms() {
    const QByteArray body = "name=Ada+Lovelace&source=a%2Bb%3Dc";
    const QByteArray wire = "POST /submit HTTP/1.1\r\nContent-Length: " +
                            QByteArray::number(body.size()) + "\r\n\r\n" + body;
    http::ParseResult parsed = http::parse_request(wire);
    require(parsed.state == http::ParseState::Complete, "POST request should parse");
    require(parsed.request.body == body, "POST body should be retained exactly");

    const http::FormFields form = http::form_body(parsed.request);
    require(http::form_value(form, "name") == "Ada Lovelace", "plus should decode as space");
    require(http::form_value(form, "source") == "a+b=c", "percent escapes should decode");
}

void test_rejections() {
    struct RejectionCase {
        const char* name;
        QByteArray wire;
        QString error;
    };

    std::vector<RejectionCase> cases{
        {"unsupported method", "DELETE / HTTP/1.1\r\n\r\n", "unsupported method"},
        {"malformed content length",
         "POST / HTTP/1.1\r\nContent-Length: not-a-number\r\n\r\n",
         "invalid content length"},
        {"oversized body", "POST / HTTP/1.1\r\nContent-Length: 524289\r\n\r\n",
         "body too large"},
        {"same-case duplicate content length",
         "POST / HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n",
         "duplicate content length"},
        {"mixed-case duplicate content length",
         "POST / HTTP/1.1\r\nContent-Length: 0\r\ncontent-length: 0\r\n\r\n",
         "duplicate content length"},
        {"transfer encoding",
         "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
         "transfer encoding is not supported"},
        {"transfer encoding with content length",
         "POST / HTTP/1.1\r\nTransfer-Encoding: identity\r\nContent-Length: 0\r\n\r\n",
         "transfer encoding is not supported"},
        {"bare line feeds", "GET / HTTP/1.1\nHost: local\n\n", "invalid line ending"},
        {"mixed line endings", "GET / HTTP/1.1\r\nHost: local\n\r\n",
         "invalid line ending"},
        {"whitespace before colon", "GET / HTTP/1.1\r\nContent-Length : 0\r\n\r\n",
         "malformed header"},
        {"obsolete folded header", "GET / HTTP/1.1\r\n X-Test: value\r\n\r\n",
         "malformed header"},
        {"header without colon", "GET / HTTP/1.1\r\nBroken\r\n\r\n", "malformed header"},
        {"missing HTTP version", "GET /\r\n\r\n", "bad request line"},
        {"wrong HTTP version", "GET / HTTP/1.0\r\n\r\n", "bad request line"},
        {"extra request-line token", "GET / HTTP/1.1 extra\r\n\r\n", "bad request line"},
        {"ambiguous request-line spaces", "GET  / HTTP/1.1\r\n\r\n", "bad request line"},
        {"control in request target", "GET /bad\ttarget HTTP/1.1\r\n\r\n",
         "bad request line"},
        {"absolute-form request target", "GET http://local/ HTTP/1.1\r\n\r\n",
         "bad request line"},
        {"trailing body bytes", "POST / HTTP/1.1\r\nContent-Length: 3\r\n\r\nabcX",
         "unexpected trailing data"},
        {"pipelined request",
         "GET /one HTTP/1.1\r\n\r\nGET /two HTTP/1.1\r\n\r\n",
         "unexpected trailing data"}
    };

    cases.push_back({"oversized unfinished headers", QByteArray(16 * 1024 + 1, 'x'),
                     "headers too large"});
    QByteArray completed_headers = "GET / HTTP/1.1\r\nX-Fill: ";
    completed_headers += QByteArray(16 * 1024, 'x');
    completed_headers += "\r\n\r\n";
    cases.push_back({"oversized completed headers", completed_headers, "headers too large"});

    for (const RejectionCase& test : cases) {
        const http::ParseResult parsed = http::parse_request(test.wire);
        require(parsed.state == http::ParseState::Error,
                std::string(test.name) + " should be rejected");
        require(parsed.error == test.error,
                std::string(test.name) + " returned unexpected error: " +
                    parsed.error.toStdString());
    }
}

void test_connection_dispatches_once() {
    QTcpServer listener;
    require(listener.listen(QHostAddress::LocalHost, 0),
            "failed to listen for HTTP connection test");

    QTcpSocket client;
    QByteArray response_bytes;
    int handler_calls = 0;
    bool late_write_queued = false;

    QObject::connect(&client, &QTcpSocket::readyRead, [&]() {
        response_bytes += client.readAll();
    });
    QObject::connect(&listener, &QTcpServer::newConnection, &listener, [&]() {
        while (listener.hasPendingConnections()) {
            QTcpSocket* peer = listener.nextPendingConnection();
            http::serve_connection(peer, [&](const http::HttpRequest&) {
                ++handler_calls;
                if (handler_calls == 1) {
                    const QByteArray late_request =
                        "GET /late HTTP/1.1\r\nHost: local\r\n\r\n";
                    late_write_queued = client.write(late_request) == late_request.size();
                    client.flush();
                    // Force delivery while the first handler is still on the stack.
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                }
                http::HttpResponse response;
                response.body = "ok";
                return response;
            });
        }
    });

    client.connectToHost(QHostAddress::LocalHost, listener.serverPort());
    require(wait_until([&]() { return client.state() == QAbstractSocket::ConnectedState; }, 3000),
            "HTTP test client failed to connect");

    const QByteArray first_request = "GET /first HTTP/1.1\r\nHost: local\r\n\r\n";
    require(client.write(first_request) == first_request.size(),
            "failed to queue first HTTP request");
    client.flush();
    require(wait_until(
                [&]() {
                    response_bytes += client.readAll();
                    return response_bytes.contains("\r\n\r\nok");
                },
                5000),
            "single-dispatch test did not receive the successful response");
    require(wait_until([&]() { return client.state() == QAbstractSocket::UnconnectedState; },
                       5000),
            "HTTP test connection did not close");
    response_bytes += client.readAll();

    require(late_write_queued, "failed to queue late HTTP request");
    require(handler_calls == 1, "late bytes dispatched the HTTP handler more than once");
    require(response_bytes.startsWith("HTTP/1.1 200 OK\r\n"),
            "single-dispatch test received an invalid successful response");
    require(response_bytes.count("HTTP/1.1 ") == 1,
            "late bytes caused more than one HTTP response");
}

void test_response_framing_and_escaping() {
    http::HttpResponse response = http::redirect_response("/submit");
    const QByteArray framed = http::serialize_response(response);
    require(framed.startsWith("HTTP/1.1 303 See Other\r\n"), "status line should be framed");
    require(framed.contains("Content-Length: 8\r\n"), "content length should match body");
    require(framed.contains("Location: /submit\r\n"), "extra headers should be retained");
    require(framed.endsWith("\r\n\r\nRedirect"), "body should follow header delimiter");
    require(http::html_escape("<&\"'>") == "&lt;&amp;&quot;&#39;&gt;",
            "HTML special characters should be escaped");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        test_incomplete_and_complete_request();
        test_post_body_and_forms();
        test_rejections();
        test_connection_dispatches_once();
        test_response_framing_and_escaping();
        std::cout << "HTTP parser tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& ex) {
        std::cerr << "HTTP parser test failed: " << ex.what() << '\n';
        return EXIT_FAILURE;
    }
}
