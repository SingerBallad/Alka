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
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "InspectorBridge/InspectorBridge.hpp"

#include <Alka/Config.hpp>
#include <Alka/Engine/Engine.hpp>
#include <Alka/Runtime/Dispatcher.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace AlkaTools {

namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void closeSocket(const Socket s) {
    closesocket(s);
}
void shutdownSocket(const Socket s) {
    shutdown(s, SD_BOTH);
}
constexpr int kSendFlags = 0;
struct NetInit {
    NetInit() {
        WSADATA data;
        mOk = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    NetInit(const NetInit&) = delete;
    NetInit& operator=(const NetInit&) = delete;
    ~NetInit() {
        if (mOk) {
            WSACleanup();
        }
    }
    bool mOk = false;
};
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void closeSocket(const Socket s) {
    ::close(s);
}
void shutdownSocket(const Socket s) {
    ::shutdown(s, SHUT_RDWR);
}
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL; // a dropped front end must not SIGPIPE the host
#else
constexpr int kSendFlags = 0;
#endif
struct NetInit {
    bool mOk = true;
};
#endif

bool sendAll(const Socket s, std::string_view data) {
    while (!data.empty()) {
        const auto chunk = static_cast<int>((std::min)(data.size(), std::size_t{1} << 20));
        const auto sent = ::send(s, data.data(), chunk, kSendFlags);
        if (sent <= 0) {
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

void setTimeout(const Socket s, const int option, const int milliseconds) {
#ifdef _WIN32
    const DWORD value = static_cast<DWORD>(milliseconds);
    (void)setsockopt(s, SOL_SOCKET, option, reinterpret_cast<const char*>(&value), sizeof value);
#else
    timeval value{};
    value.tv_sec = milliseconds / 1000;
    value.tv_usec = (milliseconds % 1000) * 1000;
    (void)setsockopt(s, SOL_SOCKET, option, &value, sizeof value);
#endif
}

bool readable(const Socket s, const int milliseconds) {
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval timeout{};
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
    return select(0, &set, nullptr, nullptr, &timeout) > 0;
#else
    pollfd entry{s, POLLIN, 0}; // (not select: descriptors may exceed FD_SETSIZE)
    return ::poll(&entry, 1, milliseconds) > 0;
#endif
}

class Reader {
public:
    Reader(const Socket s, std::string pending) : mSocket(s), mPending(std::move(pending)) {}

    bool read(void* out, std::size_t size) {
        auto* dst = static_cast<char*>(out);
        const auto fromPending = (std::min)(size, mPending.size() - mOffset);
        std::copy_n(mPending.data() + mOffset, fromPending, dst);
        mOffset += fromPending;
        dst += fromPending;
        size -= fromPending;
        while (size > 0) {
            const auto chunk = static_cast<int>((std::min)(size, std::size_t{1} << 20));
            const auto got = ::recv(mSocket, dst, chunk, 0);
            if (got <= 0) {
                return false;
            }
            dst += got;
            size -= static_cast<std::size_t>(got);
        }
        return true;
    }

private:
    Socket mSocket;
    std::string mPending;
    std::size_t mOffset = 0;
};

auto sha1(const std::string_view data) -> std::array<std::uint8_t, 20> {
    std::array h{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::string message(data);
    const std::uint64_t bits = (data.size()) * 8;
    message += static_cast<char>(0x80);
    while (message.size() % 64 != 56) {
        message += '\0';
    }
    for (int i = 7; i >= 0; --i) {
        message += static_cast<char>((bits >> (i * 8)) & 0xFF);
    }
    for (std::size_t chunk = 0; chunk < message.size(); chunk += 64) {
        std::array<std::uint32_t, 80> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto* p = reinterpret_cast<const unsigned char*>(message.data() + chunk + i * 4);
            w[i] = (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
                   (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
        }
        for (std::size_t i = 16; i < 80; ++i) {
            w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        auto [a, b, c, d, e] = h;
        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t t = std::rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = std::rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::array<std::uint8_t, 20> out{};
    for (std::size_t i = 0; i < 5; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            out[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (24 - j * 8));
        }
    }
    return out;
}

auto base64(const std::span<const std::uint8_t> bytes) -> std::string {
    static constexpr std::string_view kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t n = (std::uint32_t{bytes[i]} << 16) |
                                (i + 1 < bytes.size() ? std::uint32_t{bytes[i + 1]} << 8 : 0) |
                                (i + 2 < bytes.size() ? std::uint32_t{bytes[i + 2]} : 0);
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? kTable[(n >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? kTable[n & 63] : '=';
    }
    return out;
}

auto webSocketAccept(const std::string_view key) -> std::string {
    const auto digest = sha1(std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return base64(digest);
}

auto lowered(std::string_view text) -> std::string {
    std::string out(text);
    std::ranges::transform(
        out, out.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

auto trimmed(std::string_view text) -> std::string_view {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

struct Request {
    std::string mMethod;
    std::string mPath;
    std::string mHost;
    std::string mUpgrade;
    std::string mKey;
    std::string mPending;
};

auto readRequest(const Socket s) -> std::optional<Request> {
    std::string head;
    std::array<char, 2048> buffer{};
    std::size_t end = std::string::npos;
    while ((end = head.find("\r\n\r\n")) == std::string::npos) {
        if (head.size() > 16 * 1024) {
            return std::nullopt;
        }
        const auto got = ::recv(s, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (got <= 0) {
            return std::nullopt;
        }
        head.append(buffer.data(), static_cast<std::size_t>(got));
    }
    Request request;
    request.mPending = head.substr(end + 4);
    head.resize(end + 2);
    std::string_view rest = head;
    const auto lineEnd = rest.find("\r\n");
    const std::string_view requestLine = rest.substr(0, lineEnd);
    rest.remove_prefix(lineEnd + 2);
    const auto firstSpace = requestLine.find(' ');
    const auto secondSpace = requestLine.find(' ', firstSpace + 1);
    if (firstSpace == std::string_view::npos || secondSpace == std::string_view::npos) {
        return std::nullopt;
    }
    request.mMethod = requestLine.substr(0, firstSpace);
    request.mPath = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    while (!rest.empty()) {
        const auto next = rest.find("\r\n");
        const std::string_view line = rest.substr(0, next);
        rest.remove_prefix(next == std::string_view::npos ? rest.size() : next + 2);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const auto name = lowered(trimmed(line.substr(0, colon)));
        const auto value = trimmed(line.substr(colon + 1));
        if (name == "host") {
            request.mHost = value;
        } else if (name == "upgrade") {
            request.mUpgrade = lowered(value);
        } else if (name == "sec-websocket-key") {
            request.mKey = value;
        }
    }
    return request;
}

bool hostAllowed(const std::string_view host) {
    if (host.empty()) {
        return false;
    }
    if (host.front() == '[') {
        return host.find(']') != std::string_view::npos;
    }
    std::string_view name = host;
    if (const auto colon = name.rfind(':'); colon != std::string_view::npos) {
        name = name.substr(0, colon);
    }
    if (lowered(name) == "localhost") {
        return true;
    }
    return !name.empty() && std::ranges::all_of(name, [](const char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '.';
    });
}

auto jsonEscaped(const std::string_view text) -> std::string {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
        } else {
            out += c;
        }
    }
    return out;
}

void respond(const Socket s, const std::string_view status, const std::string_view body) {
    (void)sendAll(s, std::format("HTTP/1.1 {}\r\n"
                                 "Content-Type: application/json; charset=UTF-8\r\n"
                                 "Cache-Control: no-cache\r\n"
                                 "Content-Length: {}\r\n"
                                 "Connection: close\r\n\r\n{}",
                                 status, body.size(), body));
}

constexpr std::uint8_t kText = 0x1;
constexpr std::uint8_t kClose = 0x8;
constexpr std::uint8_t kPing = 0x9;
constexpr std::uint8_t kPong = 0xA;
constexpr std::uint64_t kMaxMessage = 64ull << 20;

auto frame(const std::uint8_t opcode, const std::string_view payload) -> std::string {
    std::string out;
    out += static_cast<char>(0x80 | opcode);
    const auto size = payload.size();
    if (size < 126) {
        out += static_cast<char>(size);
    } else if (size <= 0xFFFF) {
        out += 126;
        out += static_cast<char>((size >> 8) & 0xFF);
        out += static_cast<char>(size & 0xFF);
    } else {
        out += 127;
        for (int i = 7; i >= 0; --i) {
            out += static_cast<char>((size >> (i * 8)) & 0xFF);
        }
    }
    out += payload;
    return out;
}

} // namespace

struct InspectorBridge::Impl {
    struct Shared {
        Alka::Inspector* mInspector = nullptr;
        std::unique_ptr<Alka::InspectorSession> mSession;
        // any thread
        std::atomic<bool> mClosed{false};
        std::atomic<bool> mConnected{false};
        std::atomic<bool> mReady{false};
        std::atomic<std::uint64_t> mReadySignals{0};
        std::atomic<bool> mSessionDone{true};
        std::mutex mMutex;
        Socket mClient = kNoSocket;
        std::vector<Socket> mSockets;
        void track(const Socket s) {
            const std::lock_guard lock(mMutex);
            mSockets.push_back(s);
        }
        void release(const Socket s) {
            const std::lock_guard lock(mMutex);
            std::erase(mSockets, s);
            closeSocket(s);
        }

        void sendFrame(const std::uint8_t opcode, const std::string_view payload) {
            const std::lock_guard lock(mMutex);
            if (mClient != kNoSocket && !sendAll(mClient, frame(opcode, payload))) {
                shutdownSocket(mClient);
            }
        }
    };

    Impl(Alka::Inspector& inspector, v8::Isolate* isolate, std::string title)
        : mIsolate(isolate), mDispatcher(isolate), mTitle(std::move(title)) {
        mShared->mInspector = &inspector;
        std::random_device random;
        std::array words{random(), random(), random(), random()};
        mId = std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:04x}{:08x}", words[0], words[1] >> 16,
                          words[1] & 0xFFFF, words[2] >> 16, words[2] & 0xFFFF, words[3]);
        mPump = [isolate] { return Alka::Engine::pump(isolate); };
    }

    [[nodiscard]] auto webSocketTarget() const -> std::string {
        return std::format("127.0.0.1:{}/{}", mPort, mId);
    }

    bool listen(const std::uint16_t port) {
        if (!mNet.mOk) {
            return false;
        }
        mListener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (mListener == kNoSocket) {
            return false;
        }
        constexpr int on = 1;
#ifdef _WIN32
        (void)setsockopt(mListener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                         reinterpret_cast<const char*>(&on), sizeof on);
#else
        (void)setsockopt(mListener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // never reachable from other hosts
        if (::bind(mListener, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0 ||
            ::listen(mListener, 8) != 0) {
            closeSocket(mListener);
            mListener = kNoSocket;
            return false;
        }
        sockaddr_in bound{};
#ifdef _WIN32
        int length = sizeof bound;
#else
        socklen_t length = sizeof bound;
#endif
        (void)::getsockname(mListener, reinterpret_cast<sockaddr*>(&bound), &length);
        mPort = ntohs(bound.sin_port);
        mAcceptor = std::thread([this] { acceptLoop(); });
        return true;
    }

    void acceptLoop() {
        while (!mShared->mClosed) {
            if (mSessionThread.joinable() && mShared->mSessionDone) {
                mSessionThread.join();
            }
            if (!readable(mListener, 200)) {
                continue;
            }
            const Socket client = accept(mListener, nullptr, nullptr);
            if (client == kNoSocket) {
                continue;
            }
            mShared->track(client);
            if (mShared->mClosed) {
                mShared->release(client);
                break;
            }
            setTimeout(client, SO_RCVTIMEO, 2000);
            if (!handleRequest(client)) {
                mShared->release(client);
            }
        }
    }

    bool handleRequest(const Socket client) {
        auto request = readRequest(client);
        if (!request) {
            return false;
        }
        if (!hostAllowed(request->mHost)) {
            respond(client, "403 Forbidden", R"({"error":"the Host must be localhost or an IP"})");
            return false;
        }
        if (request->mUpgrade == "websocket") {
            if (mShared->mClosed) {
                return false;
            }
            if (request->mPath != "/" + mId || request->mKey.empty()) {
                respond(client, "404 Not Found", "{}");
                return false;
            }
            if (!mShared->mSessionDone) {
                respond(client, "409 Conflict", R"({"error":"a front end is already attached"})");
                return false;
            }
            if (!sendAll(client, std::format("HTTP/1.1 101 Switching Protocols\r\n"
                                             "Upgrade: websocket\r\n"
                                             "Connection: Upgrade\r\n"
                                             "Sec-WebSocket-Accept: {}\r\n\r\n",
                                             webSocketAccept(request->mKey)))) {
                return false;
            }
            setTimeout(client, SO_RCVTIMEO, 0);
            setTimeout(client, SO_SNDTIMEO, 10000); // a front end that stops reading
            if (mSessionThread.joinable()) {
                mSessionThread.join(); // the previous session, finished
            }
            mShared->mSessionDone = false;
            try {
                mSessionThread =
                    std::thread([this, client, pending = std::move(request->mPending)] {
                        runSession(client, pending);
                    });
            } catch (...) {
                mShared->mSessionDone = true;
                return false;
            }
            return true;
        }
        if (request->mMethod != "GET") {
            respond(client, "405 Method Not Allowed", "{}");
        } else if (request->mPath == "/json" || request->mPath == "/json/list") {
            respond(client, "200 OK",
                    std::format(R"([{{"description":"Alka","devtoolsFrontendUrl":"{}",)"
                                R"("id":"{}","title":"{}","type":"node","url":"file://",)"
                                R"("webSocketDebuggerUrl":"ws://{}"}}])",
                                frontendUrl(), mId, jsonEscaped(mTitle), webSocketTarget()));
        } else if (request->mPath == "/json/version") {
            respond(client, "200 OK",
                    std::format(R"({{"Browser":"Alka/{}","Protocol-Version":"1.3"}})",
                                Alka::versionString()));
        } else {
            respond(client, "404 Not Found", "{}");
        }
        return false;
    }

    void runSession(const Socket client, std::string pending) {
        const auto shared = mShared;
        {
            const std::lock_guard lock(shared->mMutex);
            shared->mClient = client;
        }
        (void)mDispatcher.post([shared] {
            if (shared->mClosed) {
                return;
            }
            shared->mSession = shared->mInspector->connect(
                [raw = shared.get()](std::string_view json) { raw->sendFrame(kText, json); });
            shared->mConnected = true;
        });
        Reader reader(client, std::move(pending));
        std::string message;
        while (true) {
            std::array<std::uint8_t, 2> head{};
            if (!reader.read(head.data(), head.size())) {
                break;
            }
            const bool last = (head[0] & 0x80) != 0;
            const auto opcode = static_cast<std::uint8_t>(head[0] & 0x0F);
            std::uint64_t length = head[1] & 0x7Fu;
            if ((head[1] & 0x80) == 0) {
                break;
            }
            if (length >= 126) {
                std::array<std::uint8_t, 8> extended{};
                const std::size_t bytes = length == 126 ? 2 : 8;
                if (!reader.read(extended.data(), bytes)) {
                    break;
                }
                length = 0;
                for (std::size_t i = 0; i < bytes; ++i) {
                    length = (length << 8) | extended[i];
                }
            }
            if (length > kMaxMessage || message.size() + length > kMaxMessage) {
                break;
            }
            std::array<std::uint8_t, 4> mask{};
            std::string payload(length, '\0');
            if (!reader.read(mask.data(), mask.size()) ||
                !reader.read(payload.data(), payload.size())) {
                break;
            }
            for (std::size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(payload[i] ^ static_cast<char>(mask[i % 4]));
            }
            if (opcode == kClose) {
                shared->sendFrame(kClose, payload.substr(0, 2));
                break;
            }
            if (opcode == kPing) {
                shared->sendFrame(kPong, payload);
                continue;
            }
            if (opcode == kPong) {
                continue;
            }
            message += payload;
            if (!last) {
                continue;
            }
            (void)mDispatcher.post([shared, text = std::move(message)] {
                if (!shared->mClosed && shared->mSession) {
                    shared->mSession->dispatch(text);
                }
            });
            message.clear();
        }
        {
            const std::lock_guard lock(shared->mMutex);
            shared->mClient = kNoSocket;
        }
        (void)mDispatcher.post([shared] {
            if (!shared->mClosed) {
                shared->mSession.reset();
                shared->mConnected = false;
                shared->mReady = false;
            }
        });
        shared->release(client);
        shared->mSessionDone = true;
    }

    [[nodiscard]] auto frontendUrl() const -> std::string {
        return "devtools://devtools/bundled/js_app.html?experiments=true&v8only=true&ws=" +
               webSocketTarget();
    }

    NetInit mNet;
    v8::Isolate* mIsolate;
    std::shared_ptr<Shared> mShared = std::make_shared<Shared>();
    Alka::Dispatcher mDispatcher;
    std::string mTitle;
    std::string mId;
    std::function<bool()> mPump;
    Socket mListener = kNoSocket;
    std::uint16_t mPort = 0;
    std::thread mAcceptor;
    std::thread mSessionThread;
};

InspectorBridge::InspectorBridge(Alka::Inspector& inspector, v8::Isolate* isolate,
                                 const std::uint16_t port, std::string title)
    : mImpl(std::make_unique<Impl>(inspector, isolate, std::move(title))) {
    inspector.onFrontendReady([shared = mImpl->mShared.get()] {
        shared->mReady = true;
        ++shared->mReadySignals;
    });
    (void)mImpl->listen(port);
}

InspectorBridge::~InspectorBridge() {
    const auto& shared = mImpl->mShared;
    shared->mClosed = true;
    const auto wakeReaders = [&] {
        const std::lock_guard lock(shared->mMutex);
        for (const auto s : shared->mSockets) {
            shutdownSocket(s);
        }
    };
    wakeReaders();
    if (mImpl->mAcceptor.joinable()) {
        mImpl->mAcceptor.join();
    }
    wakeReaders();
    if (mImpl->mSessionThread.joinable()) {
        mImpl->mSessionThread.join();
    }
    if (mImpl->mListener != kNoSocket) {
        closeSocket(mImpl->mListener);
    }
    shared->mInspector->onFrontendReady({});
    shared->mSession.reset();
}

bool InspectorBridge::listening() const noexcept {
    return mImpl->mListener != kNoSocket;
}

auto InspectorBridge::port() const noexcept -> std::uint16_t {
    return mImpl->mPort;
}

auto InspectorBridge::webSocketUrl() const -> std::string {
    return "ws://" + mImpl->webSocketTarget();
}

auto InspectorBridge::frontendUrl() const -> std::string {
    return mImpl->frontendUrl();
}

bool InspectorBridge::connected() const noexcept {
    return mImpl->mShared->mConnected;
}

void InspectorBridge::setPump(std::function<bool()> pump) {
    mImpl->mPump = std::move(pump);
}

bool InspectorBridge::waitForFrontend(const std::chrono::milliseconds timeout) {
    const auto& shared = *mImpl->mShared;
    const auto start = std::chrono::steady_clock::now();
    const auto signalsBefore = shared.mReadySignals.load();
    while (!shared.mReady && shared.mReadySignals.load() == signalsBefore) {
        if (timeout != std::chrono::milliseconds::max() &&
            std::chrono::steady_clock::now() - start >= timeout) {
            return false;
        }
        if (!mImpl->mPump()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    return true;
}

void InspectorBridge::pauseOnStart() {
    if (const auto& session = mImpl->mShared->mSession) {
        session->schedulePauseOnNextStatement();
    }
}

void InspectorBridge::serveUntilDetached() {
    while (mImpl->mShared->mConnected) {
        if (!mImpl->mPump()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

} // namespace AlkaTools
