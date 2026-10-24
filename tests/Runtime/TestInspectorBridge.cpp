#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>
#include <Alka/Engine/Inspector.hpp>
#include <InspectorBridge/InspectorBridge.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void closeSocket(const Socket s) {
    closesocket(s);
}
struct NetInit {
    NetInit() {
        WSADATA data;
        (void)WSAStartup(MAKEWORD(2, 2), &data);
    }
    NetInit(const NetInit&) = delete;
    NetInit& operator=(const NetInit&) = delete;
    ~NetInit() { WSACleanup(); }
};
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void closeSocket(const Socket s) {
    ::close(s);
}
struct NetInit {
    NetInit() {} // sockets need no start-up here (user-provided: no unused warning)
};
#endif

auto connectTo(const std::uint16_t port) -> Socket {
    const Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        closeSocket(s);
        return kNoSocket;
    }
#ifdef _WIN32
    const DWORD timeout = 10000;
    (void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                     sizeof timeout);
#else
    timeval timeout{10, 0};
    (void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
#endif
    return s;
}

bool sendAll(const Socket s, std::string_view data) {
    while (!data.empty()) {
        const auto sent = send(s, data.data(), static_cast<int>(data.size()), 0);
        if (sent <= 0) {
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

bool receiveExact(const Socket s, char* out, std::size_t size) {
    while (size > 0) {
        const auto got = recv(s, out, static_cast<int>(size), 0);
        if (got <= 0) {
            return false;
        }
        out += got;
        size -= static_cast<std::size_t>(got);
    }
    return true;
}

auto httpGet(const std::uint16_t port, const std::string_view path, const std::string_view host)
    -> std::string {
    const Socket s = connectTo(port);
    if (s == kNoSocket) {
        return "connect failed";
    }
    (void)sendAll(s, std::format("GET {} HTTP/1.1\r\nHost: {}\r\n\r\n", path, host));
    std::string response;
    std::array<char, 1024> buffer{};
    while (true) {
        const auto got = recv(s, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (got <= 0) {
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(got));
    }
    closeSocket(s);
    return response;
}

auto upgrade(const std::uint16_t port, const std::string_view path)
    -> std::pair<Socket, std::string> {
    const Socket s = connectTo(port);
    if (s == kNoSocket) {
        return {kNoSocket, "connect failed"};
    }
    (void)sendAll(s, std::format("GET {} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                                 "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
                                 path));
    std::string head;
    char c = 0;
    while (!head.ends_with("\r\n\r\n") && receiveExact(s, &c, 1)) {
        head += c;
    }
    return {s, head};
}

auto command(const int id, const std::string_view method) -> std::string {
    return std::format(R"({{"id":{},"method":"{}"}})", id, method);
}

auto clientFrame(const std::uint8_t opcode, const std::string_view payload) -> std::string {
    static constexpr std::array<std::uint8_t, 4> kMask{0x12, 0x34, 0x56, 0x78};
    std::string out;
    out += static_cast<char>(0x80 | opcode);
    if (payload.size() < 126) {
        out += static_cast<char>(0x80 | payload.size());
    } else {
        out += static_cast<char>(0x80 | 126);
        out += static_cast<char>((payload.size() >> 8) & 0xFF);
        out += static_cast<char>(payload.size() & 0xFF);
    }
    for (const auto byte : kMask) {
        out += static_cast<char>(byte);
    }
    for (std::size_t i = 0; i < payload.size(); ++i) {
        out += static_cast<char>(payload[i] ^ static_cast<char>(kMask[i % 4]));
    }
    return out;
}

struct Frame {
    std::uint8_t mOpcode = 0;
    std::string mPayload;
};

auto readFrame(const Socket s) -> std::optional<Frame> {
    std::array<char, 2> head{};
    if (!receiveExact(s, head.data(), head.size())) {
        return std::nullopt;
    }
    Frame frame;
    frame.mOpcode = static_cast<std::uint8_t>(head[0] & 0x0F);
    std::uint64_t length = static_cast<std::uint8_t>(head[1]) & 0x7Fu;
    if (length >= 126) {
        std::array<char, 8> extended{};
        const std::size_t bytes = length == 126 ? 2 : 8;
        if (!receiveExact(s, extended.data(), bytes)) {
            return std::nullopt;
        }
        length = 0;
        for (std::size_t i = 0; i < bytes; ++i) {
            length = (length << 8) | static_cast<std::uint8_t>(extended[i]);
        }
    }
    frame.mPayload.resize(length);
    if (!receiveExact(s, frame.mPayload.data(), frame.mPayload.size())) {
        return std::nullopt;
    }
    return frame;
}

bool readUntilClose(const Socket s) {
    while (const auto frame = readFrame(s)) {
        if (frame->mOpcode == 0x8) {
            return true;
        }
    }
    return false;
}

auto readUntil(const Socket s, const std::string_view needle) -> std::string {
    while (const auto frame = readFrame(s)) {
        if (frame->mOpcode == 0x1 && frame->mPayload.find(needle) != std::string::npos) {
            return frame->mPayload;
        }
    }
    return {};
}

auto pathOf(const std::string& webSocketUrl) -> std::string {
    return webSocketUrl.substr(webSocketUrl.find('/', std::string_view("ws://").size()));
}

bool pumpOnce(v8::Isolate* isolate) {
    const bool ran = Alka::Dispatcher(isolate).drain() > 0;
    isolate->PerformMicrotaskCheckpoint();
    return ran;
}

template <class Pred> bool pumpUntil(v8::Isolate* isolate, Pred done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        if (!pumpOnce(isolate)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return true;
}

} // namespace

TEST(InspectorBridge, DiscoveryHandshakeMessagesReconnects) {
    const NetInit net;
    AlkaTest::V8Fixture fx;
    fx.run([&](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::Inspector inspector(isolate);
        inspector.contextCreated(context, "main");
        AlkaTools::InspectorBridge bridge(inspector, isolate, 0, "bridge \"test\"");
        bridge.setPump([isolate] { return pumpOnce(isolate); });
        ASSERT_TRUE(bridge.listening());
        const auto port = bridge.port();
        const auto path = pathOf(bridge.webSocketUrl());

        const auto list = httpGet(port, "/json/list", std::format("127.0.0.1:{}", port));
        EXPECT_THAT(list, testing::StartsWith("HTTP/1.1 200"));
        EXPECT_THAT(list, testing::HasSubstr(std::format(R"("webSocketDebuggerUrl":"{}")",
                                                         bridge.webSocketUrl())));
        EXPECT_THAT(list, testing::HasSubstr(R"("title":"bridge \"test\"")"));
        EXPECT_THAT(httpGet(port, "/json/version", "localhost"),
                    testing::HasSubstr(R"("Protocol-Version":"1.3")"));
        EXPECT_THAT(httpGet(port, "/json/list", "attacker.example:9229"),
                    testing::StartsWith("HTTP/1.1 403"));
        // the WebSocket path is the random id
        EXPECT_THAT(upgrade(port, "/not-the-id").second, testing::StartsWith("HTTP/1.1 404"));

        for (int round = 0; round < 2; ++round) {
            SCOPED_TRACE("round = " + std::to_string(round));
            std::string handshake;
            std::string evaluated;
            bool ponged = false;
            bool closed = false;
            std::atomic checked{false};
            std::atomic done{false};
            std::thread frontEnd([&] {
                const auto [s, head] = upgrade(port, path);
                handshake = head;
                if (s == kNoSocket) {
                    done = true;
                    return;
                }
                (void)sendAll(s, clientFrame(0x1, R"({"id":1,"method":"Runtime.evaluate",)"
                                                  R"("params":{"expression":"6*7"}})"));
                (void)sendAll(s, clientFrame(0x9, "ping!"));
                (void)sendAll(s, clientFrame(0x1, command(2, "Runtime.runIfWaitingForDebugger")));
                while (const auto frame = readFrame(s)) {
                    if (frame->mOpcode == 0xA && frame->mPayload == "ping!") {
                        ponged = true;
                    } else if (frame->mOpcode == 0x1 &&
                               frame->mPayload.find(R"("id":1)") != std::string::npos) {
                        evaluated = frame->mPayload;
                    }
                    if (ponged && !evaluated.empty()) {
                        break;
                    }
                }
                while (!checked) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                (void)sendAll(s, clientFrame(0x8, std::string("\x03\xe8", 2)));
                closed = readUntilClose(s);
                closeSocket(s);
                done = true;
            });
            EXPECT_TRUE(bridge.waitForFrontend(std::chrono::seconds(10)));
            EXPECT_TRUE(bridge.connected());
            checked = true;
            EXPECT_TRUE(pumpUntil(isolate, [&] { return done.load(); }));
            frontEnd.join();
            EXPECT_TRUE(pumpUntil(isolate, [&] { return !bridge.connected(); }));
            EXPECT_THAT(handshake, testing::StartsWith("HTTP/1.1 101"));
            EXPECT_THAT(handshake,
                        testing::HasSubstr("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
            EXPECT_THAT(evaluated, testing::HasSubstr(R"("value":42)"));
            EXPECT_TRUE(ponged);
            EXPECT_TRUE(closed);
        }
    });
}

TEST(InspectorBridge, FrontEndThatIsReadyAndGoneWithinOnePumpStillCounts) {
    const NetInit net;
    AlkaTest::V8Fixture fx;
    fx.run([&](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::Inspector inspector(isolate);
        inspector.contextCreated(context, "main");
        AlkaTools::InspectorBridge bridge(inspector, isolate, 0);
        bridge.setPump([isolate] { return pumpOnce(isolate); });
        ASSERT_TRUE(bridge.listening());
        std::thread frontEnd([port = bridge.port(), path = pathOf(bridge.webSocketUrl())] {
            const auto [s, head] = upgrade(port, path);
            if (s == kNoSocket) {
                return;
            }
            (void)sendAll(s, clientFrame(0x1, command(1, "Runtime.runIfWaitingForDebugger")));
            (void)sendAll(s, clientFrame(0x8, ""));
            (void)readUntilClose(s);
            closeSocket(s);
        });
        frontEnd.join();
        EXPECT_TRUE(bridge.waitForFrontend(std::chrono::seconds(5)));
        EXPECT_FALSE(bridge.connected());
        EXPECT_FALSE(bridge.waitForFrontend(std::chrono::milliseconds(50))); // no stale readiness
    });
}

TEST(InspectorBridge, ShuttingDownWhileAFrontEndIsAttached) {
    const NetInit net;
    AlkaTest::V8Fixture fx;
    fx.run([&](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::Inspector inspector(isolate);
        inspector.contextCreated(context, "main");
        std::thread frontEnd;
        std::atomic<bool> dropped{false};
        {
            AlkaTools::InspectorBridge bridge(inspector, isolate, 0);
            bridge.setPump([isolate] { return pumpOnce(isolate); });
            ASSERT_TRUE(bridge.listening());
            frontEnd = std::thread([&, port = bridge.port(), path = pathOf(bridge.webSocketUrl())] {
                const auto [s, head] = upgrade(port, path);
                if (s == kNoSocket) {
                    return;
                }
                (void)sendAll(s, clientFrame(0x1, command(1, "Runtime.runIfWaitingForDebugger")));
                while (readFrame(s)) {
                }
                dropped = true;
                closeSocket(s);
            });
            ASSERT_TRUE(bridge.waitForFrontend(std::chrono::seconds(10)));
            EXPECT_TRUE(bridge.connected());
        }
        frontEnd.join();
        EXPECT_TRUE(dropped);
        std::string reply;
        auto session = inspector.connect([&](std::string_view json) { reply = json; });
        session->dispatch(command(9, "Runtime.enable"));
        EXPECT_THAT(reply, testing::HasSubstr(R"("id":9)"));
    });
}

TEST(InspectorBridge, PauseOnStartResumeFromTheFrontEnd) {
    const NetInit net;
    AlkaTest::V8Fixture fx;
    fx.run([&](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::Inspector inspector(isolate);
        inspector.contextCreated(context, "main");
        inspector.setPausePump([isolate] {
            if (!pumpOnce(isolate)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        AlkaTools::InspectorBridge bridge(inspector, isolate, 0);
        bridge.setPump([isolate] { return pumpOnce(isolate); });
        ASSERT_TRUE(bridge.listening());

        std::string paused;
        std::string resumed;
        std::atomic<bool> done{false};
        std::thread frontEnd([&, port = bridge.port(), path = pathOf(bridge.webSocketUrl())] {
            const auto [s, head] = upgrade(port, path);
            if (s == kNoSocket) {
                done = true;
                return;
            }
            (void)sendAll(s, clientFrame(0x1, command(1, "Debugger.enable")));
            (void)sendAll(s, clientFrame(0x1, command(2, "Runtime.runIfWaitingForDebugger")));
            paused = readUntil(s, R"("method":"Debugger.paused")");
            (void)sendAll(s, clientFrame(0x1, command(3, "Debugger.resume")));
            resumed = readUntil(s, R"("id":3)");
            (void)sendAll(s, clientFrame(0x8, std::string("\x03\xe8", 2)));
            (void)readUntilClose(s);
            closeSocket(s);
            done = true;
        });
        ASSERT_TRUE(bridge.waitForFrontend(std::chrono::seconds(10)));
        bridge.pauseOnStart();
        v8::Local<v8::Value> result;
        ASSERT_TRUE(
            AlkaTest::V8Fixture::compileAndRun(isolate, context, "globalThis.answer = 6 * 7")
                .ToLocal(&result));
        EXPECT_EQ(result->Int32Value(context).FromMaybe(0), 42);
        EXPECT_TRUE(pumpUntil(isolate, [&] { return done.load(); }));
        frontEnd.join();
        EXPECT_THAT(paused, testing::HasSubstr(R"("reason":"Break on start")"));
        EXPECT_THAT(resumed, testing::HasSubstr(R"("result":{})"));
    });
}
