/**
 * End-to-end tests for the built-in localhost GeoHTTPServer (the live status
 * viewer you open in a browser while the engine runs).
 *
 * These are REAL socket round-trips: a server thread is started, a raw TCP
 * client issues HTTP GET requests, and the raw responses are asserted.
 *
 * Run with: make test
 */

#include <gtest/gtest.h>
#include "geospatial/GeoHTTPServer.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// Raw HTTP GET helper; returns the full response (headers + body).
std::string HttpGet(int port, const std::string& path) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return "";
    }

    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n"
                            "Connection: close\r\n\r\n";
    send(fd, req.data(), static_cast<int>(req.size()), 0);

    char buf[8192];
    std::string resp;
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
        resp.append(buf, static_cast<size_t>(n));
    ::close(fd);
    return resp;
}

// Starts the server on the first free port in a private range (tests must not
// collide with the app's 8123 or with each other).
bool StartOnFreePort(geo::GeoHTTPServer& server,
                     geo::GeoHTTPServer::PayloadProvider provider) {
    for (int port = 18231; port < 18240; ++port) {
        if (server.start(port, provider)) return true;
    }
    return false;
}

// Raw HTTP POST helper; returns the full response (headers + body).
std::string HttpPost(int port, const std::string& path, const std::string& body) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return "";
    }

    const std::string req = "POST " + path + " HTTP/1.1\r\n"
                            "Host: localhost\r\n"
                            "Content-Type: application/json\r\n"
                            "Content-Length: " + std::to_string(body.size()) + "\r\n"
                            "Connection: close\r\n\r\n" + body;
    send(fd, req.data(), static_cast<int>(req.size()), 0);

    char buf[8192];
    std::string resp;
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
        resp.append(buf, static_cast<size_t>(n));
    ::close(fd);
    return resp;
}

}  // namespace

TEST(GeoHTTPServer, ServesStatusJson) {
    geo::GeoHTTPServer server;
    ASSERT_TRUE(StartOnFreePort(server, []() { return "{\"hello\":\"world\"}"; }));

    const std::string resp = HttpGet(server.port(), "/feed");
    EXPECT_NE(resp.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(resp.find("Content-Type: application/json"), std::string::npos);
    EXPECT_NE(resp.find("{\"hello\":\"world\"}"), std::string::npos);

    server.stop();
}

TEST(GeoHTTPServer, HealthEndpoint) {
    geo::GeoHTTPServer server;
    ASSERT_TRUE(StartOnFreePort(server, []() { return "{}"; }));

    const std::string resp = HttpGet(server.port(), "/health");
    EXPECT_NE(resp.find("\"status\":\"ok\""), std::string::npos);

    server.stop();
}

TEST(GeoHTTPServer, FeedAndStatusAliasesServePayload) {
    geo::GeoHTTPServer server;
    ASSERT_TRUE(StartOnFreePort(server, []() { return "{\"feed\":true}"; }));

    EXPECT_NE(HttpGet(server.port(), "/feed").find("{\"feed\":true}"), std::string::npos);
    EXPECT_NE(HttpGet(server.port(), "/status").find("{\"feed\":true}"), std::string::npos);

    server.stop();
}

TEST(GeoHTTPServer, UnknownPathReturnsErrorJson) {
    geo::GeoHTTPServer server;
    ASSERT_TRUE(StartOnFreePort(server, []() { return "{}"; }));

    const std::string resp = HttpGet(server.port(), "/nope");
    EXPECT_NE(resp.find("\"error\":\"not found\""), std::string::npos);

    server.stop();
}

TEST(GeoHTTPServer, PayloadProviderRunsPerRequest) {
    geo::GeoHTTPServer server;
    int calls = 0;
    ASSERT_TRUE(StartOnFreePort(server, [&calls]() {
        ++calls;
        return "{\"calls\":" + std::to_string(calls) + "}";
    }));

    EXPECT_NE(HttpGet(server.port(), "/feed").find("\"calls\":1"), std::string::npos);
    EXPECT_NE(HttpGet(server.port(), "/feed").find("\"calls\":2"), std::string::npos);

    server.stop();
}

TEST(GeoHTTPServer, ServesBuiltinHtmlViewerAtRoot) {
    geo::GeoHTTPServer server;
    ASSERT_TRUE(StartOnFreePort(server, []() { return "{}"; }));

    const std::string resp = HttpGet(server.port(), "/");
    EXPECT_NE(resp.find("<!DOCTYPE html>"), std::string::npos);
    EXPECT_NE(resp.find("Content-Type: text/html"), std::string::npos);
    EXPECT_NE(resp.find("Live Geospatial Feed"), std::string::npos);
    EXPECT_NE(resp.find("canvas"), std::string::npos);

    server.stop();
}

TEST(GeoHTTPServer, PostFeedAddInvokesHandler) {
    geo::GeoHTTPServer server;
    ASSERT_TRUE(StartOnFreePort(server, []() { return "{}"; }));

    std::string gotUrl, gotType;
    bool called = false;
    server.setFeedHandlers(
        [&](const std::string& url, const std::string& type) -> std::string {
            called = true;
            gotUrl = url;
            gotType = type;
            return "{\"ok\":true}";
        },
        [](size_t) -> std::string { return "{\"ok\":false}"; });

    const std::string resp = HttpPost(server.port(), "/feed",
                                      "{\"url\":\"http://feed.example/api\",\"type\":\"REST\"}");
    EXPECT_NE(resp.find("{\"ok\":true}"), std::string::npos);
    EXPECT_TRUE(called);
    EXPECT_EQ(gotUrl, "http://feed.example/api");
    EXPECT_EQ(gotType, "REST");

    server.stop();
}

TEST(GeoHTTPServer, PostFeedMissingUrlIsRejected) {
    geo::GeoHTTPServer server;
    ASSERT_TRUE(StartOnFreePort(server, []() { return "{}"; }));

    bool called = false;
    server.setFeedHandlers(
        [&](const std::string&, const std::string&) -> std::string {
            called = true;
            return "{\"ok\":true}";
        },
        [](size_t) -> std::string { return "{\"ok\":false}"; });

    const std::string resp = HttpPost(server.port(), "/feed", "{\"type\":\"REST\"}");
    EXPECT_NE(resp.find("missing url"), std::string::npos);
    EXPECT_FALSE(called);  // handler must not run on a malformed request

    server.stop();
}

TEST(GeoHTTPServer, PostFeedRemoveInvokesHandler) {
    geo::GeoHTTPServer server;
    ASSERT_TRUE(StartOnFreePort(server, []() { return "{}"; }));

    size_t gotIndex = 999;
    bool called = false;
    server.setFeedHandlers(
        [](const std::string&, const std::string&) -> std::string { return "{}"; },
        [&](size_t index) -> std::string {
            called = true;
            gotIndex = index;
            return "{\"ok\":true}";
        });

    const std::string resp = HttpPost(server.port(), "/feed/remove", "{\"index\":3}");
    EXPECT_NE(resp.find("{\"ok\":true}"), std::string::npos);
    EXPECT_TRUE(called);
    EXPECT_EQ(gotIndex, size_t(3));

    server.stop();
}

TEST(GeoHTTPServer, PostFeedRemoveInvalidIndexIsRejected) {
    geo::GeoHTTPServer server;
    ASSERT_TRUE(StartOnFreePort(server, []() { return "{}"; }));

    bool called = false;
    server.setFeedHandlers(
        [](const std::string&, const std::string&) -> std::string { return "{}"; },
        [&](size_t) -> std::string {
            called = true;
            return "{\"ok\":true}";
        });

    const std::string resp = HttpPost(server.port(), "/feed/remove", "{\"index\":-1}");
    EXPECT_NE(resp.find("invalid index"), std::string::npos);
    EXPECT_FALSE(called);

    server.stop();
}

TEST(GeoHTTPServer, StopIsIdempotentAndPortIsReusable) {
    geo::GeoHTTPServer server;
    ASSERT_TRUE(StartOnFreePort(server, []() { return "{}"; }));
    const int port = server.port();

    server.stop();
    server.stop();  // second stop must be a no-op, not a crash

    // The port should be immediately reusable (SO_REUSEADDR on the listener).
    geo::GeoHTTPServer again;
    ASSERT_TRUE(again.start(port, []() { return "{}"; }));
    EXPECT_NE(HttpGet(port, "/health").find("\"status\":\"ok\""), std::string::npos);
    again.stop();
}
