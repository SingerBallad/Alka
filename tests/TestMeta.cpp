#include <Alka/Introspect.hpp>

#include <doctest/doctest.h>

#include <string>
#include <string_view>

using namespace std::string_view_literals;

namespace MetaTest {

struct Gadget {
    Gadget() = default;
    int mPower = 3;
    void zap(int) {}
    void oldZap() {}
};

}  // namespace MetaTest

template<>
struct Alka::Bind<MetaTest::Gadget> : Alka::Class<MetaTest::Gadget> {
    static constexpr auto doc = "a gadget";
    static constexpr auto members = Alka::members(
        Alka::ctor<>(),
        Alka::method<&MetaTest::Gadget::zap>(Alka::doc("zap it"), Alka::arg("volts") = 5,
                                             Alka::meta<"category">("tools"),
                                             Alka::meta<"cost">(2.5)),
        Alka::method<&MetaTest::Gadget::oldZap>(Alka::deprecated("use zap")),
        Alka::prop<&MetaTest::Gadget::mPower>(Alka::readonly, Alka::meta<"unit">("watts")));
};

TEST_CASE("classInfo exposes the binding metadata at runtime") {
    const Alka::ClassView view = Alka::classInfo<MetaTest::Gadget>();
    CHECK(view.mName == "Gadget"sv);
    CHECK(view.mDoc == "a gadget"sv);
    CHECK(view.mExtends.empty());
    CHECK(view.mMembers.size() == 4);

    const auto* zap = view.member("zap");
    REQUIRE(zap != nullptr);
    CHECK(zap->mKind == Alka::MemberKind::Method);
    CHECK(zap->mDoc == "zap it"sv);
    CHECK(zap->mArgNames.size() == 1);
    CHECK(zap->mArgNames[0] == "volts"sv);
    REQUIRE(zap->meta("category") != nullptr);
    CHECK(zap->meta("category")->mString == "tools"sv);
    REQUIRE(zap->meta("cost") != nullptr);
    CHECK(zap->meta("cost")->mNumber == 2.5);
    CHECK(zap->meta("missing") == nullptr);

    const auto* oldZap = view.member("oldZap");
    REQUIRE(oldZap != nullptr);
    CHECK(oldZap->mDeprecated);
    CHECK(oldZap->mDeprecatedReason == "use zap"sv);

    const auto* power = view.member("power");
    REQUIRE(power != nullptr);
    CHECK(power->mKind == Alka::MemberKind::Property);
    CHECK(power->mReadonly);
    REQUIRE(power->meta("unit") != nullptr);
    CHECK(power->meta("unit")->mString == "watts"sv);
}
