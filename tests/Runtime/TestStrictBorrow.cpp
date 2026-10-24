#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace StrictTest {

struct Untracked {
    int mValue = 1;
};

struct Managed {
    int mValue = 2;
};
struct ManagedChild : Managed {};

struct Watched : Alka::Trackable {
    int mValue = 3;
};

struct Pooled : std::enable_shared_from_this<Pooled> {
    int mValue = 4;
};

inline Untracked gUntracked;
inline Untracked* untracked() {
    return &gUntracked;
}

inline std::shared_ptr<Pooled> gPooled = std::make_shared<Pooled>();
inline Pooled gLoosePooled;
inline Pooled* pooled() {
    return gPooled.get();
}
inline Pooled* loosePooled() {
    return &gLoosePooled;
}

inline ManagedChild gChild;
inline ManagedChild* child() {
    return &gChild;
}

struct Mod;

} // namespace StrictTest

template <> struct Alka::Bind<StrictTest::Untracked> : Class<StrictTest::Untracked> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members = Alka::members(Alka::prop<&StrictTest::Untracked::mValue>());
};

template <> struct Alka::Bind<StrictTest::Managed> : Class<StrictTest::Managed> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto lifetime = Lifetime::Manual;
    static constexpr auto members = Alka::members(Alka::prop<&StrictTest::Managed::mValue>());
};

template <> struct Alka::Bind<StrictTest::ManagedChild> : Class<StrictTest::ManagedChild> {
    using Extends = StrictTest::Managed;
    static constexpr auto members = Alka::members();
};

template <> struct Alka::Bind<StrictTest::Watched> : Class<StrictTest::Watched> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members = Alka::members(Alka::prop<&StrictTest::Watched::mValue>());
};

template <> struct Alka::Bind<StrictTest::Pooled> : Class<StrictTest::Pooled> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto members = Alka::members(Alka::prop<&StrictTest::Pooled::mValue>());
};

template <> struct Alka::Bind<StrictTest::Mod> : Module<"strict"> {
    static constexpr auto members = Alka::members(
        Alka::type<StrictTest::Untracked>, Alka::type<StrictTest::Managed>,
        Alka::type<StrictTest::ManagedChild>, Alka::type<StrictTest::Watched>,
        Alka::type<StrictTest::Pooled>, Alka::fn<&StrictTest::untracked>(policy::borrow),
        Alka::fn<&StrictTest::pooled>(), Alka::fn<&StrictTest::loosePooled>(),
        Alka::fn<&StrictTest::child>());
};

static_assert(!Alka::Detail::lendable<StrictTest::Untracked>());
static_assert(Alka::Detail::lendable<StrictTest::Managed>());
static_assert(Alka::Detail::lendable<StrictTest::ManagedChild>());
static_assert(Alka::Detail::lendable<StrictTest::Watched>());
static_assert(!Alka::Detail::lendable<StrictTest::Pooled>());

namespace {

struct StrictFixture : AlkaTest::V8Fixture {
    StrictFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<StrictTest::Mod>(context);
        });
    }
};

} // namespace

TEST(StrictBorrow, StrictBorrowingLendsDeclaredTrackedAndExplicitlyLentObjects) {
    StrictFixture fx;
    EXPECT_EQ(fx.evalInt("strict.untracked().value"), 1);
    EXPECT_EQ(fx.evalInt("strict.child().value"), 2);
    EXPECT_TRUE(fx.evalBool("strict.child() instanceof strict.ManagedChild"));
}

TEST(StrictBorrow, SelfSharingClassIsSharedWhenASharedPtrOwnsItRefusedOtherwise) {
    StrictFixture fx;
    EXPECT_EQ(fx.evalInt("strict.pooled().value"), 4);
    EXPECT_EQ(
        fx.evalString("try { strict.loosePooled() } catch (e) { e.message }"),
        "Alka (strict borrowing): this Pooled is not owned by a shared_ptr, so JS cannot hold "
        "it safely; declare Bind<T>::lifetime, derive from Alka::Trackable, or state "
        "policy::borrow");
}

TEST(StrictBorrow, AlkaBorrowLendsAValueExplicitly) {
    StrictFixture fx;
    fx.run([](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::ConvertCtx cx{isolate, context};
        const auto lent = Alka::toJs(cx, Alka::borrow(&StrictTest::gUntracked));
        std::ignore = context->Global()->Set(context, Alka::Detail::v8Str(isolate, "lent"), lent);
    });
    EXPECT_EQ(fx.evalInt("lent.value"), 1);
    EXPECT_TRUE(fx.evalBool("lent === strict.untracked()")); // the same object: one wrapper
}
