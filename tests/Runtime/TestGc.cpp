#include <Alka/Alka.hpp>
#include <Alka/Engine/Engine.hpp>

#include <gtest/gtest.h>

#if ALKA_HAS_CPPGC

#include <string>

namespace GcTest {

inline int gAlive = 0;

class Node : public Alka::GcObject<Node> {
public:
    explicit Node(const int value) : mValue(value) { ++gAlive; }
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    virtual ~Node() { --gAlive; }

    void Trace(cppgc::Visitor* visitor) const override {
        GcObject::Trace(visitor);
        visitor->Trace(mNext);
    }

    [[nodiscard]] int total() const {
        int sum = 0;
        for (const Node* node = this; node != nullptr; node = node->mNext.Get()) {
            sum += node->mValue;
            if (node->mNext.Get() == this) {
                break;
            }
        }
        return sum;
    }
    [[nodiscard]] virtual std::string kind() const { return "node"; }

    int mValue = 0;
    cppgc::Member<Node> mNext;
};

class Leaf final : public Node {
public:
    using Node::Node;
    [[nodiscard]] std::string kind() const override { return "leaf"; }
};

inline cppgc::Persistent<Node> gKept;

inline Node* make(const int value) {
    return Alka::makeGc<Node>(value);
}
inline Node* makeLeaf(const int value) {
    return Alka::makeGc<Leaf>(value);
}
inline void keep(Node* node) {
    gKept = node;
}
inline Node* kept() {
    return gKept.Get();
}
inline int valueOf(const Node& node) {
    return node.mValue;
}

struct Mod;

} // namespace GcTest

template <> struct Alka::Bind<GcTest::Node> : Alka::Class<GcTest::Node> {
    static constexpr auto members = Alka::members(
        Alka::ctor<int>(Alka::arg("value")), Alka::prop<"value", &GcTest::Node::mValue>(),
        Alka::prop<"next", &GcTest::Node::mNext>(), Alka::method<&GcTest::Node::total>(),
        Alka::method<"kind", &GcTest::Node::kind>());
};

template <> struct Alka::Bind<GcTest::Leaf> : Alka::Class<GcTest::Leaf> {
    using Extends = GcTest::Node;
    static constexpr auto members = Alka::members(Alka::ctor<int>(Alka::arg("value")));
};

template <> struct Alka::Bind<GcTest::Mod> : Alka::Module<"gc"> {
    static constexpr auto members =
        Alka::members(Alka::type<GcTest::Node>, Alka::type<GcTest::Leaf>, Alka::fn<&GcTest::make>(),
                      Alka::fn<&GcTest::makeLeaf>(), Alka::fn<&GcTest::keep>(),
                      Alka::fn<&GcTest::kept>(), Alka::fn<&GcTest::valueOf>());
};

namespace {

auto run(const Alka::ContextScope& scope, const std::string_view source) -> std::string {
    const auto result = scope.evalToString(source);
    if (!result) {
        ADD_FAILURE() << "JS failed: " << result.error().message();
        return {};
    }
    return *result;
}

void collect(v8::Isolate* isolate) {
    for (int i = 0; i < 3; ++i) {
        isolate->GetCppHeap()->CollectGarbageForTesting(cppgc::EmbedderStackState::kNoHeapPointers);
        (void)Alka::Engine::pump(isolate);
    }
}

} // namespace

TEST(Gc, CxxHeapClassesMadeFromJSAndCxxOneWrapperEach) {
    auto isolate = Alka::Engine::createIsolate({.mCppHeap = true});
    const Alka::ContextScope scope(isolate.get());
    scope.install<GcTest::Mod>();

    EXPECT_EQ(run(scope, R"(
        const a = new gc.Node(1), b = new gc.Node(2);
        a.next = b;
        [a.next === b, a.total(), gc.valueOf(b), a.next.next === null].join()
    )"),
              "true,3,2,true");
    EXPECT_EQ(run(scope, "globalThis.k = new gc.Node(4); gc.keep(k); gc.kept() === k"), "true");
    EXPECT_EQ(run(scope,
                  "const c = gc.make(5); gc.keep(c); [c.value, gc.kept() === c, gc.make(5) === "
                  "c].join()"),
              "5,true,false");
    EXPECT_EQ(
        run(scope, "const l = gc.makeLeaf(7); [l instanceof gc.Leaf, l.kind(), l.value].join()"),
        "true,leaf,7");
    EXPECT_EQ(run(scope, "const m = new gc.Leaf(8); [m.kind(), m.total()].join()"), "leaf,8");
    EXPECT_EQ(run(scope, "try { gc.valueOf({}); 'took it' } catch (e) { e.constructor.name }"),
              "TypeError");
}

TEST(Gc, CxxHeapClassesCyclesThroughJSAndCxxAreCollected) {
    auto isolate = Alka::Engine::createIsolate({.mCppHeap = true});
    {
        const Alka::ContextScope scope(isolate.get());
        scope.install<GcTest::Mod>();
        const int before = GcTest::gAlive;
        ASSERT_TRUE(scope
                        .eval(R"((() => {
            const a = new gc.Node(1), b = new gc.Node(2);
            a.next = b; b.next = a; a.peer = b; b.self = b;
            for (let i = 0; i < 100; ++i) new gc.Node(i).next = gc.make(i);
        })(); undefined)")
                        .has_value());
        EXPECT_GE(GcTest::gAlive, before + 202);
        ASSERT_TRUE(
            scope
                .eval("(() => { const k = new gc.Node(9); k.tag = 'mine'; gc.keep(k); })(); "
                      "undefined")
                .has_value());
        collect(isolate.get());
        EXPECT_EQ(GcTest::gAlive, before + 1);
        EXPECT_EQ(run(scope, "[gc.kept().value, gc.kept().tag].join()"), "9,mine");
        GcTest::gKept.Clear();
        collect(isolate.get());
        EXPECT_EQ(GcTest::gAlive, before);
    }
}

#endif // ALKA_HAS_CPPGC
