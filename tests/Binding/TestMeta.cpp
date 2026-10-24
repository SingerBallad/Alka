#include <Alka/Binding/Introspect.hpp>

#include <gtest/gtest.h>

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

} // namespace MetaTest

template <> struct Alka::Bind<MetaTest::Gadget> : Class<MetaTest::Gadget> {
    static constexpr auto naming = memberPrefixes;
    static constexpr auto doc = "a gadget";
    static constexpr auto members =
        Alka::members(Alka::ctor<>(),
                      Alka::method<&MetaTest::Gadget::zap>(Alka::doc("zap it"), arg("volts") = 5,
                                                           Alka::meta<"category">("tools"),
                                                           Alka::meta<"cost">(2.5)),
                      Alka::method<&MetaTest::Gadget::oldZap>(deprecated("use zap")),
                      Alka::prop<&MetaTest::Gadget::mPower>(readonly, Alka::meta<"unit">("watts")));
};

TEST(Meta, ClassInfoExposesTheBindingMetadataAtRuntime) {
    const Alka::ClassView view = Alka::classInfo<MetaTest::Gadget>();
    EXPECT_EQ(view.mName, "Gadget"sv);
    EXPECT_EQ(view.mDoc, "a gadget"sv);
    EXPECT_TRUE(view.mExtends.empty());
    EXPECT_EQ(view.mMembers.size(), 4);

    const auto* zap = view.member("zap");
    ASSERT_NE(zap, nullptr);
    EXPECT_EQ(zap->mKind, Alka::MemberKind::Method);
    EXPECT_EQ(zap->mDoc, "zap it"sv);
    EXPECT_EQ(zap->mArgNames.size(), 1);
    EXPECT_EQ(zap->mArgNames[0], "volts"sv);
    ASSERT_NE(zap->meta("category"), nullptr);
    EXPECT_EQ(zap->meta("category")->mString, "tools"sv);
    ASSERT_NE(zap->meta("cost"), nullptr);
    EXPECT_EQ(zap->meta("cost")->mNumber, 2.5);
    EXPECT_EQ(zap->meta("missing"), nullptr);

    const auto* oldZap = view.member("oldZap");
    ASSERT_NE(oldZap, nullptr);
    EXPECT_TRUE(oldZap->mDeprecated);
    EXPECT_EQ(oldZap->mDeprecatedReason, "use zap"sv);

    const auto* power = view.member("power");
    ASSERT_NE(power, nullptr);
    EXPECT_EQ(power->mKind, Alka::MemberKind::Property);
    EXPECT_TRUE(power->mReadonly);
    ASSERT_NE(power->meta("unit"), nullptr);
    EXPECT_EQ(power->meta("unit")->mString, "watts"sv);
}
