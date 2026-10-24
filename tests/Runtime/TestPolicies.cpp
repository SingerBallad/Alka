#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace PolicyTest {

struct Item {
    inline static int sAlive = 0;
    Item() { ++sAlive; }
    explicit Item(int v) : mValue(v) { ++sAlive; }
    Item(const Item& other) : mValue(other.mValue) { ++sAlive; }
    Item(Item&& other) noexcept : mValue(other.mValue) {
        other.mValue = -1;
        ++sAlive;
    }
    ~Item() { --sAlive; }
    int mValue = 0;
};

inline Item gItem{7};
inline Item& itemRef() {
    return gItem;
}
inline Item* itemPtr() {
    return &gItem;
}
inline Item* freshItem() {
    return new Item(42);
}

struct Bag {
    std::vector<Item*> mItems;
    void add(Item* item) { mItems.push_back(item); }
    [[nodiscard]] int sum() const {
        int total = 0;
        for (const auto* item : mItems) {
            total += item->mValue;
        }
        return total;
    }
};

struct Node : std::enable_shared_from_this<Node> {
    inline static int sAlive = 0;
    Node() { ++sAlive; }
    ~Node() { --sAlive; }
    int mId = 3;
};

inline std::shared_ptr<Node> gNode;
inline Node* nodeRaw() {
    return gNode.get();
}
inline std::weak_ptr<Node> nodeWeak() {
    return gNode;
}

inline std::shared_ptr<Item> gKept;
inline void keepShared(std::shared_ptr<Item> item) {
    gKept = std::move(item);
}

inline std::string describeCall(Alka::CallInfo call, int value) {
    return std::string(call.mThis->IsObject() ? "this:object" : "this:other") + "/" +
           std::to_string(value);
}
inline bool hasIsolate(v8::Isolate* isolate, v8::Local<v8::Context> context, int) {
    return isolate != nullptr && !context.IsEmpty();
}
inline std::int64_t big() {
    return 9007199254740993LL;
} // 2^53 + 1
inline std::int64_t small() {
    return 12;
}
inline std::int64_t echo64(std::int64_t v) {
    return v;
}

struct Mod;

} // namespace PolicyTest

template <> struct Alka::Bind<PolicyTest::Item> : Class<PolicyTest::Item> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members =
        Alka::members(Alka::ctor<int>(), Alka::prop<&PolicyTest::Item::mValue>());
};

template <> struct Alka::Bind<PolicyTest::Bag> : Class<PolicyTest::Bag> {
    static constexpr auto members =
        Alka::members(Alka::ctor<>(), Alka::method<&PolicyTest::Bag::add>(Alka::keepAlive<0>),
                      Alka::method<&PolicyTest::Bag::sum>());
};

template <> struct Alka::Bind<PolicyTest::Node> : Class<PolicyTest::Node> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members = Alka::members(Alka::prop<&PolicyTest::Node::mId>());
};

template <> struct Alka::Bind<PolicyTest::Mod> : Module<"pol"> {
    static constexpr auto members = Alka::members(
        Alka::type<PolicyTest::Item>, Alka::type<PolicyTest::Bag>, Alka::type<PolicyTest::Node>,
        Alka::fn<"refBorrow", &PolicyTest::itemRef>(policy::borrow),
        Alka::fn<"refCopy", &PolicyTest::itemRef>(policy::copy),
        Alka::fn<"refMove", &PolicyTest::itemRef>(policy::move),
        Alka::fn<"ptrCopy", &PolicyTest::itemPtr>(policy::copy),
        Alka::fn<"take", &PolicyTest::freshItem>(policy::take),
        Alka::fn<&PolicyTest::nodeRaw>(policy::borrow), Alka::fn<&PolicyTest::nodeWeak>(),
        Alka::fn<&PolicyTest::keepShared>(), Alka::fn<&PolicyTest::describeCall>(arg("value")),
        Alka::fn<&PolicyTest::hasIsolate>(arg("value")), Alka::fn<"bigDefault", &PolicyTest::big>(),
        Alka::fn<"bigNumber", &PolicyTest::big>(Alka::int64As<Int64Mode::Number>),
        Alka::fn<"bigAuto", &PolicyTest::big>(Alka::int64As<Int64Mode::Auto>),
        Alka::fn<"smallAuto", &PolicyTest::small>(Alka::int64As<Int64Mode::Auto>),
        Alka::fn<&PolicyTest::echo64>(arg("v")));
};

namespace {

struct PolicyFixture : AlkaTest::V8Fixture {
    PolicyFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<PolicyTest::Mod>(context);
        });
    }
};

} // namespace

TEST(Policies, ReturnPoliciesDecideCopyMoveBorrowAndOwnershipTransfer) {
    PolicyFixture fx;
    PolicyTest::gItem.mValue = 7;
    EXPECT_TRUE(fx.evalBool("pol.refBorrow() === pol.refBorrow()"));
    fx.evalString("pol.refBorrow().value = 8; 'ok'");
    EXPECT_EQ(PolicyTest::gItem.mValue, 8);
    EXPECT_EQ(fx.evalInt("(() => { const c = pol.refCopy(); c.value = 100; return c.value; })()"),
              100);
    EXPECT_EQ(PolicyTest::gItem.mValue, 8);
    EXPECT_EQ(fx.evalInt("pol.ptrCopy().value"), 8);
    EXPECT_EQ(fx.evalInt("pol.refMove().value"), 8);
    EXPECT_EQ(PolicyTest::gItem.mValue, -1);
    const int before = PolicyTest::Item::sAlive;
    fx.evalString("globalThis.t = pol.take(); 'ok'");
    EXPECT_EQ(PolicyTest::Item::sAlive, before + 1);
    EXPECT_EQ(fx.evalInt("t.value"), 42);
    fx.evalString("globalThis.t = null; 'ok'");
    fx.collectGarbage();
    EXPECT_LE(PolicyTest::Item::sAlive, before);
}

TEST(Policies, KeepAliveIKeepsAnArgumentAliveAsLongAsTheReceiver) {
    PolicyFixture fx;
    const int before = PolicyTest::Item::sAlive;
    fx.evalString(
        "globalThis.bag = new pol.Bag(); bag.add(new pol.Item(5)); bag.add(new pol.Item(6)); 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(PolicyTest::Item::sAlive, before + 2);
    EXPECT_EQ(fx.evalInt("bag.sum()"), 11);
    fx.evalString("globalThis.bag = null; 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(PolicyTest::Item::sAlive, before);
}

TEST(Policies, SharedFromThisObjectsAreSharedNotBorrowed) {
    PolicyFixture fx;
    PolicyTest::gNode = std::make_shared<PolicyTest::Node>();
    fx.evalString("globalThis.n = pol.nodeRaw(); 'ok'");
    PolicyTest::gNode.reset();
    EXPECT_EQ(PolicyTest::Node::sAlive, 1);
    EXPECT_EQ(fx.evalInt("n.id"), 3);
    fx.evalString("globalThis.n = null; 'ok'");
    fx.collectGarbage();
    EXPECT_EQ(PolicyTest::Node::sAlive, 0);
}

TEST(Policies, WeakPtrResultsStopWorkingOnceCxxLetsGo) {
    PolicyFixture fx;
    PolicyTest::gNode = std::make_shared<PolicyTest::Node>();
    fx.evalString("globalThis.w = pol.nodeWeak(); 'ok'");
    EXPECT_EQ(fx.evalInt("w.id"), 3);
    PolicyTest::gNode.reset();
    EXPECT_EQ(PolicyTest::Node::sAlive, 0);
    EXPECT_EQ(fx.evalString("try { w.id } catch (e) { e.message }"),
              "Node.id: the native Node object is not (or no longer) alive");
    EXPECT_TRUE(fx.evalBool("pol.nodeWeak() === null"));
}

TEST(Policies, JSCreatedObjectsCanBeSharedWithCxx) {
    PolicyFixture fx;
    PolicyTest::gKept.reset();
    fx.evalString("pol.keepShared(new pol.Item(9)); 'ok'");
    fx.collectGarbage();
    ASSERT_TRUE(PolicyTest::gKept);
    EXPECT_EQ(PolicyTest::gKept->mValue, 9);
    PolicyTest::gKept.reset();
}

TEST(Policies, InjectedParametersTakeNoJSArgument) {
    PolicyFixture fx;
    EXPECT_EQ(fx.evalString("pol.describeCall(5)"), "this:object/5");
    EXPECT_TRUE(fx.evalBool("pol.hasIsolate(1)"));
    EXPECT_EQ(fx.evalInt("pol.describeCall.length"), 1);
    EXPECT_EQ(fx.evalString("try { pol.describeCall('x') } catch (e) { e.message }"),
              "arg 1 (value) of pol.describeCall: expected number, got string");
    const auto dts = Alka::generateDts<PolicyTest::Mod>();
    EXPECT_THAT(dts, testing::HasSubstr("function describeCall(value: number): string;"));
    EXPECT_THAT(dts, testing::HasSubstr("function hasIsolate(value: number): boolean;"));
}

TEST(Policies, Int64OutputModes) {
    PolicyFixture fx;
    EXPECT_EQ(fx.evalString("typeof pol.bigDefault()"), "bigint");
    EXPECT_EQ(fx.evalString("pol.bigDefault().toString()"), "9007199254740993");
    EXPECT_EQ(fx.evalString("typeof pol.bigNumber()"), "number");
    EXPECT_EQ(fx.evalString("typeof pol.bigAuto()"), "bigint");
    EXPECT_EQ(fx.evalString("typeof pol.smallAuto()"), "number");
    // input accepts either form
    EXPECT_EQ(fx.evalString("pol.echo64(5).toString()"), "5");
    EXPECT_EQ(fx.evalString("pol.echo64(5n).toString()"), "5");

    const auto dts = Alka::generateDts<PolicyTest::Mod>();
    EXPECT_THAT(dts, testing::HasSubstr("function bigDefault(): bigint;"));
    EXPECT_THAT(dts, testing::HasSubstr("function bigNumber(): number;"));
    EXPECT_THAT(dts, testing::HasSubstr("function bigAuto(): number | bigint;"));
    EXPECT_THAT(dts, testing::HasSubstr("function echo64(v: number | bigint): bigint;"));
}

TEST(Policies, BoundFunctionsLookLikeRealFunctions) {
    PolicyFixture fx;
    EXPECT_EQ(fx.evalString("pol.describeCall.name"), "describeCall");
    EXPECT_EQ(fx.evalString("Object.getOwnPropertyDescriptor(pol.Bag.prototype, 'sum').value.name"),
              "sum");
    EXPECT_EQ(fx.evalInt("pol.Bag.prototype.add.length"), 1);
    EXPECT_TRUE(fx.evalBool("(() => { try { new (new pol.Bag().sum)(); return false; }"
                            " catch (e) { return e instanceof TypeError; } })()"));
}
