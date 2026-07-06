#include <Alka/Bind.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <string_view>

using namespace std::string_view_literals;

namespace BindFormsTest {

struct SpecEntity {
    int mHp = 7;
    void poke() {}
    static int tally() { return 1; }
};

struct SpecDerived : SpecEntity {
    void extra() {}
};

// In-class form: the binding lives inside the type itself.
struct InClass {
    int mMana = 3;
    void cast() {}

    static constexpr auto alkaMembers =
        Alka::members(Alka::ctor<>(), Alka::method<&InClass::cast>(),
                      Alka::prop<&InClass::mMana>());
};

// Both forms present: the Bind<> specialization must win.
struct DualForm {
    void inClassOnly() {}
    static constexpr auto alkaMembers = Alka::members(Alka::method<&DualForm::inClassOnly>());
    void specOnly() {}
};

struct Plain {};

}  // namespace BindFormsTest

template<>
struct Alka::Bind<BindFormsTest::SpecEntity> : Alka::Class<BindFormsTest::SpecEntity> {
    static constexpr auto doc = "an entity";
    static constexpr auto members = Alka::members(
        Alka::ctor<>(Alka::doc("default ctor")),
        Alka::method<&BindFormsTest::SpecEntity::poke>(Alka::doc("poke it")),
        Alka::method<"prod", &BindFormsTest::SpecEntity::poke>(),
        Alka::staticMethod<&BindFormsTest::SpecEntity::tally>(),
        Alka::prop<&BindFormsTest::SpecEntity::mHp>(Alka::readonly),
        Alka::prop<"vitality", &BindFormsTest::SpecEntity::mHp>());
};

template<>
struct Alka::Bind<BindFormsTest::SpecDerived>
    : Alka::Class<BindFormsTest::SpecDerived, "Derived"> {
    using Extends = BindFormsTest::SpecEntity;
    static constexpr auto members =
        Alka::members(Alka::method<&BindFormsTest::SpecDerived::extra>(
            Alka::deprecated("use extraV2"), Alka::meta<"category">("test")));
};

template<>
struct Alka::Bind<BindFormsTest::DualForm> : Alka::Class<BindFormsTest::DualForm> {
    static constexpr auto members =
        Alka::members(Alka::method<&BindFormsTest::DualForm::specOnly>());
};

// ---- concepts ---------------------------------------------------------------
static_assert(Alka::Bound<BindFormsTest::SpecEntity>);
static_assert(Alka::Bound<BindFormsTest::InClass>);
static_assert(Alka::Bound<BindFormsTest::DualForm>);
static_assert(!Alka::Bound<BindFormsTest::Plain>);
static_assert(!Alka::Bound<int>);
static_assert(Alka::BoundClass<BindFormsTest::SpecEntity>);

// ---- names / docs / extends -------------------------------------------------
static_assert(Alka::boundNameOf<BindFormsTest::SpecEntity>() == "SpecEntity"sv);
static_assert(Alka::boundNameOf<BindFormsTest::SpecDerived>() == "Derived"sv);
static_assert(Alka::boundNameOf<BindFormsTest::InClass>() == "InClass"sv);
static_assert(Alka::boundDocOf<BindFormsTest::SpecEntity>() == "an entity"sv);
static_assert(Alka::boundDocOf<BindFormsTest::InClass>().empty());
static_assert(std::is_same_v<Alka::ExtendsOf<BindFormsTest::SpecDerived>,
                             BindFormsTest::SpecEntity>);
static_assert(std::is_void_v<Alka::ExtendsOf<BindFormsTest::SpecEntity>>);
static_assert(Alka::validateClassBinding<BindFormsTest::SpecDerived>());

// ---- member walking ---------------------------------------------------------
namespace {

struct KindCounts {
    std::size_t ctors = 0;
    std::size_t methods = 0;
    std::size_t statics = 0;
    std::size_t props = 0;
};

template<class T>
consteval KindCounts countKinds() {
    KindCounts counts{};
    Alka::forEachMember<T>([&](const auto& desc) {
        using Desc = std::remove_cvref_t<decltype(desc)>;
        if constexpr (Desc::kind == Alka::MemberKind::Ctor) {
            ++counts.ctors;
        } else if constexpr (Desc::kind == Alka::MemberKind::Method) {
            ++counts.methods;
        } else if constexpr (Desc::kind == Alka::MemberKind::StaticMethod) {
            ++counts.statics;
        } else if constexpr (Desc::kind == Alka::MemberKind::Property) {
            ++counts.props;
        }
    });
    return counts;
}

template<class T, Alka::MemberKind Kind>
consteval std::string_view nthName(std::size_t index) {
    std::string_view out{};
    std::size_t seen = 0;
    Alka::forEachMember<T>([&](const auto& desc) {
        using Desc = std::remove_cvref_t<decltype(desc)>;
        if constexpr (Desc::kind == Kind) {
            if (seen++ == index) {
                out = Desc::name;
            }
        }
    });
    return out;
}

}  // namespace

static_assert(countKinds<BindFormsTest::SpecEntity>().ctors == 1);
static_assert(countKinds<BindFormsTest::SpecEntity>().methods == 2);
static_assert(countKinds<BindFormsTest::SpecEntity>().statics == 1);
static_assert(countKinds<BindFormsTest::SpecEntity>().props == 2);

// auto names, policy application and explicit overrides
static_assert(nthName<BindFormsTest::SpecEntity, Alka::MemberKind::Method>(0) == "poke"sv);
static_assert(nthName<BindFormsTest::SpecEntity, Alka::MemberKind::Method>(1) == "prod"sv);
static_assert(nthName<BindFormsTest::SpecEntity, Alka::MemberKind::StaticMethod>(0) == "tally"sv);
static_assert(nthName<BindFormsTest::SpecEntity, Alka::MemberKind::Property>(0) == "hp"sv);
static_assert(nthName<BindFormsTest::SpecEntity, Alka::MemberKind::Property>(1) == "vitality"sv);

// the Bind<> specialization wins over the in-class member list
static_assert(nthName<BindFormsTest::DualForm, Alka::MemberKind::Method>(0) == "specOnly"sv);

// in-class form walks the same way
static_assert(countKinds<BindFormsTest::InClass>().methods == 1);
static_assert(nthName<BindFormsTest::InClass, Alka::MemberKind::Property>(0) == "mana"sv);

// ---- spec queries -----------------------------------------------------------
namespace {

template<class T, Alka::MemberKind Kind, class Query>
consteval auto queryNth(std::size_t index, Query&& query) {
    using R = decltype(query(std::declval<const Alka::CtorDesc<std::tuple<>>&>()));
    R out{};
    std::size_t seen = 0;
    Alka::forEachMember<T>([&](const auto& desc) {
        using Desc = std::remove_cvref_t<decltype(desc)>;
        if constexpr (Desc::kind == Kind) {
            if (seen++ == index) {
                out = query(desc);
            }
        }
    });
    return out;
}

}  // namespace

static_assert(queryNth<BindFormsTest::SpecEntity, Alka::MemberKind::Method>(
                  0, [](const auto& d) { return d.docText(); }) == "poke it"sv);
static_assert(queryNth<BindFormsTest::SpecEntity, Alka::MemberKind::Property>(
                  0, [](const auto& d) { return d.readonlyFlag(); }));
static_assert(!queryNth<BindFormsTest::SpecEntity, Alka::MemberKind::Property>(
                  1, [](const auto& d) { return d.readonlyFlag(); }));
static_assert(queryNth<BindFormsTest::SpecDerived, Alka::MemberKind::Method>(
                  0, [](const auto& d) { return d.deprecatedFlag(); }));

TEST_CASE("binding descriptors are also queryable at runtime") {
    std::size_t methods = 0;
    Alka::forEachMember<BindFormsTest::SpecEntity>([&](const auto& desc) {
        using Desc = std::remove_cvref_t<decltype(desc)>;
        if constexpr (Desc::kind == Alka::MemberKind::Method) {
            ++methods;
            CHECK(!Desc::name.empty());
        }
    });
    CHECK(methods == 2);
}
