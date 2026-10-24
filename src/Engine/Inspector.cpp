#include <Alka/Engine/Engine.hpp>
#include <Alka/Engine/Inspector.hpp>

#include <v8-inspector.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>

namespace Alka {

namespace {

constexpr int kContextGroup = 1;

void appendUtf8(std::string& out, const char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

auto toUtf8(const v8_inspector::StringView& view) -> std::string {
    std::string out;
    out.reserve(view.length());
    if (view.is8Bit()) {
        for (std::size_t i = 0; i < view.length(); ++i) {
            appendUtf8(out, view.characters8()[i]);
        }
        return out;
    }
    const auto* chars = view.characters16();
    for (std::size_t i = 0; i < view.length(); ++i) {
        char32_t c = chars[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < view.length() && chars[i + 1] >= 0xDC00 &&
            chars[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (chars[i + 1] - 0xDC00);
            ++i;
        }
        appendUtf8(out, c);
    }
    return out;
}

auto toUtf16(const std::string_view utf8) -> std::u16string {
    std::u16string out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        const auto byte = static_cast<unsigned char>(utf8[i]);
        char32_t c = 0xFFFD;
        std::size_t extra = 0;
        if (byte < 0x80) {
            c = byte;
        } else if ((byte & 0xE0) == 0xC0) {
            c = byte & 0x1F;
            extra = 1;
        } else if ((byte & 0xF0) == 0xE0) {
            c = byte & 0x0F;
            extra = 2;
        } else if ((byte & 0xF8) == 0xF0) {
            c = byte & 0x07;
            extra = 3;
        }
        ++i;
        for (std::size_t k = 0; k < extra && i < utf8.size(); ++k, ++i) {
            c = (c << 6) | (static_cast<unsigned char>(utf8[i]) & 0x3F);
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (c >> 10));
            out += static_cast<char16_t>(0xDC00 + (c & 0x3FF));
        } else {
            out += static_cast<char16_t>(c);
        }
    }
    return out;
}

class Channel final : public v8_inspector::V8Inspector::Channel {
public:
    explicit Channel(std::function<void(std::string_view)> send) : mSend(std::move(send)) {}

    void sendResponse(int, std::unique_ptr<v8_inspector::StringBuffer> message) override {
        forward(*message);
    }
    void sendNotification(std::unique_ptr<v8_inspector::StringBuffer> message) override {
        forward(*message);
    }
    void flushProtocolNotifications() override {}

private:
    void forward(const v8_inspector::StringBuffer& message) {
        if (mSend) {
            mSend(toUtf8(message.string()));
        }
    }

    std::function<void(std::string_view)> mSend;
};

class Client final : public v8_inspector::V8InspectorClient {
public:
    explicit Client(v8::Isolate* isolate) : mIsolate(isolate) {
        mPump = [isolate] {
            if (!Engine::pump(isolate)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
    }

    void runMessageLoopOnPause(int) override {
        mPaused = true;
        while (mPaused) {
            mPump();
        }
    }
    void quitMessageLoopOnPause() override { mPaused = false; }

    void runIfWaitingForDebugger(int) override {
        if (mOnReady) {
            mOnReady();
        }
    }

    v8::Local<v8::Context> ensureDefaultContextInGroup(int) override {
        return mDefaultContext.Get(mIsolate);
    }

    double currentTimeMS() override {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    std::function<void()> mPump;
    std::function<void()> mOnReady;
    v8::Global<v8::Context> mDefaultContext; // weak

private:
    v8::Isolate* mIsolate;
    bool mPaused = false;
};

} // namespace

struct Inspector::Impl {
    explicit Impl(v8::Isolate* isolate)
        : mIsolate(isolate), mClient(isolate),
          mInspector(v8_inspector::V8Inspector::create(isolate, &mClient)) {}

    v8::Isolate* mIsolate;
    Client mClient;
    std::unique_ptr<v8_inspector::V8Inspector> mInspector;
};

struct InspectorSession::Impl {
    std::unique_ptr<Channel> mChannel;
    std::unique_ptr<v8_inspector::V8InspectorSession> mSession;
};

InspectorSession::InspectorSession(std::unique_ptr<Impl> impl) : mImpl(std::move(impl)) {}
InspectorSession::~InspectorSession() = default;

void InspectorSession::dispatch(const std::string_view message) {
    const auto utf16 = toUtf16(message);
    mImpl->mSession->dispatchProtocolMessage(v8_inspector::StringView(
        reinterpret_cast<const std::uint16_t*>(utf16.data()), utf16.size()));
}

void InspectorSession::schedulePauseOnNextStatement() {
    static constexpr std::string_view kReason = "Break on start";
    const v8_inspector::StringView reason(reinterpret_cast<const std::uint8_t*>(kReason.data()),
                                          kReason.size());
    mImpl->mSession->schedulePauseOnNextStatement(reason, reason);
}

Inspector::Inspector(v8::Isolate* isolate) : mImpl(std::make_unique<Impl>(isolate)) {}
Inspector::~Inspector() = default;

void Inspector::contextCreated(const v8::Local<v8::Context> context, const std::string_view name) {
    const auto utf16 = toUtf16(name);
    v8_inspector::V8ContextInfo info(
        context, kContextGroup,
        v8_inspector::StringView(reinterpret_cast<const std::uint16_t*>(utf16.data()),
                                 utf16.size()));
    static constexpr std::string_view kAuxData = R"({"isDefault":true})";
    info.auxData = v8_inspector::StringView(reinterpret_cast<const std::uint8_t*>(kAuxData.data()),
                                            kAuxData.size());
    mImpl->mInspector->contextCreated(info);
    if (mImpl->mClient.mDefaultContext.IsEmpty()) {
        mImpl->mClient.mDefaultContext.Reset(mImpl->mIsolate, context);
        mImpl->mClient.mDefaultContext.SetWeak();
    }
}

void Inspector::contextDestroyed(const v8::Local<v8::Context> context) {
    if (mImpl->mClient.mDefaultContext == context) {
        mImpl->mClient.mDefaultContext.Reset();
    }
    mImpl->mInspector->contextDestroyed(context);
}

auto Inspector::connect(std::function<void(std::string_view)> send)
    -> std::unique_ptr<InspectorSession> {
    auto impl = std::make_unique<InspectorSession::Impl>();
    impl->mChannel = std::make_unique<Channel>(std::move(send));
    impl->mSession =
        mImpl->mInspector->connect(kContextGroup, impl->mChannel.get(), v8_inspector::StringView(),
                                   v8_inspector::V8Inspector::kFullyTrusted,
                                   v8_inspector::V8Inspector::kNotWaitingForDebugger);
    return std::unique_ptr<InspectorSession>(new InspectorSession(std::move(impl)));
}

void Inspector::setPausePump(std::function<void()> pump) {
    mImpl->mClient.mPump = std::move(pump);
}

void Inspector::onFrontendReady(std::function<void()> ready) {
    mImpl->mClient.mOnReady = std::move(ready);
}

} // namespace Alka
