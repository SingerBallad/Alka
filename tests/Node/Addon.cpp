#include <node.h>

#include <Alka/Alka.hpp>
#include <Alka/Host/Node.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace NodeTest {

struct Counter {
    inline static std::atomic<int> sAlive{0};
    explicit Counter(const int start) : mValue(start) { ++sAlive; }
    Counter(const Counter&) = delete;
    Counter& operator=(const Counter&) = delete;
    ~Counter() { --sAlive; }
    int increment() { return ++mValue; }
    int mValue = 0;
};

int alive() {
    return Counter::sAlive;
}

Alka::Promise<int> doubleLater(const int value, const int ms) {
    auto promise = Alka::Promise<int>::create();
    std::thread([settler = promise.settler(), value, ms] {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        (void)settler.resolve(value * 2);
    }).detach();
    return promise;
}

Alka::Task<std::string> shout(std::string text) {
    const auto worker = co_await Alka::offload([] { return std::this_thread::get_id(); });
    std::string upper = co_await Alka::offload([text] {
        std::string out = text;
        for (auto& c : out) {
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        return out;
    });
    co_return worker != std::this_thread::get_id() ? upper + "!" : "same thread?";
}

Alka::Task<void> failLater() {
    co_await Alka::offload([] {});
    throw std::runtime_error("failed off-thread");
}

std::string v8Version() {
    return v8::V8::GetVersion();
}

int add(const int a, const int b) {
    return a + b;
}
inline std::string gSeen;
void see(std::string_view text) {
    gSeen = text;
}
std::string seen() {
    return gSeen;
}
bool fastUsable() {
    return Alka::Detail::kFastApiUsable;
}
double fastCalls() {
#if ALKA_FAST_API_STATS
    return static_cast<double>(Alka::Detail::gFastCalls.load());
#else
    return -1;
#endif
}

#if ALKA_HAS_CPPGC
// On V8's C++ heap (Node gives every isolate one).
inline int gGcAlive = 0;
class GcNode : public Alka::GcObject<GcNode> {
public:
    explicit GcNode(const int value) : mValue(value) { ++gGcAlive; }
    GcNode(const GcNode&) = delete;
    GcNode& operator=(const GcNode&) = delete;
    ~GcNode() { --gGcAlive; }
    void Trace(cppgc::Visitor* visitor) const override {
        GcObject::Trace(visitor);
        visitor->Trace(mNext);
    }
    int mValue = 0;
    cppgc::Member<GcNode> mNext;
};
inline cppgc::Persistent<GcNode> gGcKept;
inline cppgc::WeakPersistent<GcNode> gGcWatched;
int gcAlive() {
    return gGcAlive;
}
GcNode* gcMake(const int value) {
    return Alka::makeGc<GcNode>(value);
}
void gcKeep(GcNode* node) {
    gGcKept = node;
}
GcNode* gcKept() {
    return gGcKept.Get();
}
void gcWatch(const cppgc::WeakPersistent<GcNode>& node) {
    gGcWatched = node;
}
cppgc::WeakPersistent<GcNode> gcWatched() {
    return gGcWatched;
}
std::string gcDts(); // (the module's own .d.ts: defined below its binding)
struct GcMod;
#endif

struct Grid {
    std::vector<int> mCells = std::vector(4, 0);
    [[nodiscard]] std::size_t size() const { return mCells.size(); }
    int& operator[](const std::size_t i) { return mCells[i]; }
    const int& operator[](const std::size_t i) const { return mCells[i]; }
};
std::string gridKind() {
    return "GridClass";
}

struct Bag {
    std::map<std::string, int> mItems;
    [[nodiscard]] std::optional<int> find(const std::string& key) const {
        const auto it = mItems.find(key);
        return it == mItems.end() ? std::nullopt : std::optional(it->second);
    }
    void put(const std::string& key, const int value) { mItems[key] = value; }
    bool erase(const std::string& key) { return mItems.erase(key) > 0; }
    [[nodiscard]] std::vector<std::string> names() const {
        std::vector<std::string> out;
        for (const auto& key : mItems | std::views::keys) {
            out.push_back(key);
        }
        return out;
    }
};

struct Shape {
    Shape() = default;
    Shape(const Shape&) = delete;
    Shape& operator=(const Shape&) = delete;
    virtual ~Shape() = default;
    virtual std::string name() { return "shape"; }
};
struct JsShape : Shape, Alka::Overridable<Shape> {
    std::string name() override { return ALKA_OVERRIDE(std::string, Shape, name); }
};
std::string describe(Shape& shape) {
    return "it is a " + shape.name();
}

struct Size {
    int mW = 0;
    int mH = 0;
};
int area(const Size size) {
    return size.mW * size.mH;
}

struct Oops : std::runtime_error {
    using std::runtime_error::runtime_error;
};
void fail() {
    throw Oops("oops");
}

struct Button {
    Alka::Retained<Alka::Function<int(int)>> mOnClick;
    int click(const int x) { return mOnClick.get().call(x); }
};

Alka::BufferView fill(v8::Isolate* isolate, const int n) {
    auto out = Alka::BufferView::allocate(isolate, static_cast<std::size_t>(n));
    for (auto& b : out.bytes()) {
        b = std::byte{7};
    }
    return out;
}
int sumBytes(const Alka::BufferView bytes) {
    int sum = 0;
    for (const auto b : bytes.bytes()) {
        sum += static_cast<int>(b);
    }
    return sum;
}
void scale(const Alka::TypedView<float> values, const float factor) {
    for (auto& v : values.span()) {
        v *= factor;
    }
}
bool aborted(const Alka::AbortSignal signal) {
    return signal.aborted();
}

struct ShapesMod;
struct Mod;
struct MathMod;

int twice(const int value) {
    return value * 2;
}

} // namespace NodeTest

template <> struct Alka::Bind<NodeTest::Counter> : Class<NodeTest::Counter> {
    static constexpr auto members =
        Alka::members(Alka::ctor<int>(arg("start")), Alka::method<&NodeTest::Counter::increment>(),
                      Alka::prop<"value", &NodeTest::Counter::mValue>());
};

#if ALKA_HAS_CPPGC
template <> struct Alka::Bind<NodeTest::GcNode> : Alka::Class<NodeTest::GcNode> {
    static constexpr auto members = Alka::members(Alka::ctor<int>(Alka::arg("value")),
                                                  Alka::prop<"value", &NodeTest::GcNode::mValue>(),
                                                  Alka::prop<"next", &NodeTest::GcNode::mNext>());
};

template <> struct Alka::Bind<NodeTest::GcMod> : Alka::Module<"alkaGc"> {
    static constexpr auto members = Alka::members(
        Alka::type<NodeTest::GcNode>, Alka::fn<"alive", &NodeTest::gcAlive>(),
        Alka::fn<"make", &NodeTest::gcMake>(), Alka::fn<"keep", &NodeTest::gcKeep>(),
        Alka::fn<"kept", &NodeTest::gcKept>(), Alka::fn<"watch", &NodeTest::gcWatch>(),
        Alka::fn<"watched", &NodeTest::gcWatched>(), Alka::fn<"dts", &NodeTest::gcDts>());
};

std::string NodeTest::gcDts() {
    return Alka::generateDts<NodeTest::GcMod>();
}
#endif

template <> struct Alka::Bind<NodeTest::Grid> : Class<NodeTest::Grid> {
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), indexer(), Alka::iterable<NodeTest::Grid>(),
                      Alka::staticProp<"@@toStringTag", &NodeTest::gridKind>());
};

template <> struct Alka::Bind<NodeTest::Bag> : Class<NodeTest::Bag> {
    static constexpr auto members = Alka::members(
        Alka::ctor<>(), Alka::namedProperties<&NodeTest::Bag::find, &NodeTest::Bag::put, nullptr,
                                              &NodeTest::Bag::erase, &NodeTest::Bag::names>());
};

template <> struct Alka::Bind<NodeTest::Shape> : Class<NodeTest::Shape> {
    using Trampoline = NodeTest::JsShape;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<"name", &NodeTest::Shape::name>());
};

template <> struct Alka::Bind<NodeTest::Size> : Record<NodeTest::Size> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::prop<&NodeTest::Size::mW>(), Alka::prop<&NodeTest::Size::mH>());
};

template <> struct Alka::Bind<NodeTest::Button> : Class<NodeTest::Button> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::prop<&NodeTest::Button::mOnClick>(),
                      Alka::method<&NodeTest::Button::click>());
};

template <> struct Alka::Bind<NodeTest::ShapesMod> : Module<"alkaShapes"> {
    static constexpr auto members = Alka::members(
        Alka::type<NodeTest::Grid>, Alka::type<NodeTest::Bag>, Alka::type<NodeTest::Shape>,
        Alka::type<NodeTest::Size>, Alka::type<NodeTest::Button>,
        Alka::error<"Oops", NodeTest::Oops>(), Alka::fn<&NodeTest::describe>(),
        Alka::fn<&NodeTest::area>(), Alka::fn<&NodeTest::fail>(), Alka::fn<&NodeTest::fill>(),
        Alka::fn<&NodeTest::sumBytes>(), Alka::fn<&NodeTest::scale>(),
        Alka::fn<&NodeTest::aborted>());
};

template <> struct Alka::Bind<NodeTest::MathMod> : Module<"alkaTest.math"> {
    static constexpr auto members = Alka::members(Alka::fn<&NodeTest::twice>(Alka::arg("value")));
};

template <> struct Alka::Bind<NodeTest::Mod> : Module<"alkaTest"> {
    static constexpr auto members =
        Alka::members(Alka::type<NodeTest::Counter>, Alka::fn<&NodeTest::alive>(),
                      Alka::fn<&NodeTest::doubleLater>(arg("value"), arg("ms")),
                      Alka::fn<&NodeTest::shout>(arg("text")), Alka::fn<&NodeTest::failLater>(),
                      Alka::fn<&NodeTest::v8Version>(), Alka::submodule<NodeTest::MathMod>,
                      Alka::fn<&NodeTest::add>(arg("a"), arg("b")),
                      Alka::fn<&NodeTest::see>(arg("text")), Alka::fn<&NodeTest::seen>(),
                      Alka::fn<&NodeTest::fastUsable>(), Alka::fn<&NodeTest::fastCalls>());
};

extern "C" NODE_MODULE_EXPORT void NODE_MODULE_INITIALIZER(const v8::Local<v8::Object> exports,
                                                           v8::Local<v8::Value> /*module*/,
                                                           const v8::Local<v8::Context> context) {
    Alka::Node::attach(context);
    Alka::installInto<NodeTest::Mod>(context, exports);
    {
        const auto isolate = v8::Isolate::GetCurrent();
        const auto shapes = v8::Object::New(isolate);
        Alka::installInto<NodeTest::ShapesMod>(context, shapes);
        std::ignore =
            exports->Set(context, v8::String::NewFromUtf8Literal(isolate, "shapes"), shapes);
    }
#if ALKA_HAS_CPPGC
    const auto isolate = v8::Isolate::GetCurrent();
    const auto gc = v8::Object::New(isolate);
    Alka::installInto<NodeTest::GcMod>(context, gc);
    std::ignore = exports->Set(context, v8::String::NewFromUtf8Literal(isolate, "gc"), gc);
#endif
}
