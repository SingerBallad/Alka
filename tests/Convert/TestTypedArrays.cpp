#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

namespace TypedTest {

inline double sumFloats(const std::vector<float>& values) {
    return std::accumulate(values.begin(), values.end(), 0.0);
}
inline std::int32_t sumInts(const std::vector<std::int32_t>& values) {
    return std::accumulate(values.begin(), values.end(), 0);
}
inline std::int64_t sumBig(const std::vector<std::int64_t>& values) {
    return std::accumulate(values.begin(), values.end(), std::int64_t{0});
}
inline std::uint64_t firstUnsigned(const std::vector<std::uint64_t>& values) {
    return values.empty() ? 0 : values.front();
}
inline std::vector<double> squares(std::int32_t n) {
    std::vector<double> out;
    for (std::int32_t i = 0; i < n; ++i) {
        out.push_back(i * i);
    }
    return out;
}

inline std::uint64_t spanAddress(std::span<const float> values) {
    return reinterpret_cast<std::uintptr_t>(values.data());
}
inline std::uint64_t viewAddress(Alka::TypedView<float> values) {
    return reinterpret_cast<std::uintptr_t>(values.data());
}
inline double spanSum(std::span<const float> values) {
    return std::accumulate(values.begin(), values.end(), 0.0);
}
inline void scale(std::span<float> values, float factor) {
    for (auto& v : values) {
        v *= factor;
    }
}
inline void fillBytes(std::span<std::uint8_t> bytes) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(i + 1);
    }
}
inline Alka::TypedView<float> ramp(v8::Isolate* isolate, std::int32_t n) {
    auto out = Alka::TypedView<float>::allocate(isolate, static_cast<std::size_t>(n));
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<float>(i) / 2.0f;
    }
    return out;
}
inline Alka::TypedView<std::int64_t> bigs(v8::Isolate* isolate) {
    auto out = Alka::TypedView<std::int64_t>::allocate(isolate, 2);
    out[0] = -1;
    out[1] = std::int64_t{1} << 62;
    return out;
}

struct Mod;

} // namespace TypedTest

template <> struct Alka::Bind<TypedTest::Mod> : Module<"typed"> {
    static constexpr auto members = Alka::members(
        Alka::fn<&TypedTest::sumFloats>(arg("values")),
        Alka::fn<&TypedTest::sumInts>(arg("values")), Alka::fn<&TypedTest::sumBig>(arg("values")),
        Alka::fn<&TypedTest::firstUnsigned>(arg("values")), Alka::fn<&TypedTest::squares>(arg("n")),
        Alka::fn<&TypedTest::spanAddress>(arg("values")),
        Alka::fn<&TypedTest::viewAddress>(arg("values")),
        Alka::fn<&TypedTest::spanSum>(arg("values")),
        Alka::fn<&TypedTest::scale>(arg("values"), arg("factor")),
        Alka::fn<&TypedTest::fillBytes>(arg("bytes")), Alka::fn<&TypedTest::ramp>(arg("n")),
        Alka::fn<&TypedTest::bigs>());
};

namespace {

struct TypedFixture : AlkaTest::V8Fixture {
    TypedFixture() {
        run([](v8::Isolate*, v8::Local<v8::Context> context) {
            Alka::install<TypedTest::Mod>(context);
        });
    }
};

} // namespace

TEST(TypedArrays, NumberVectorsTakeTypedArraysAsWellAsArrays) {
    TypedFixture fx;
    EXPECT_EQ(fx.evalDouble("typed.sumFloats(new Float32Array([0.5, 1.5]))"), 2.0);
    EXPECT_EQ(fx.evalDouble("typed.sumFloats(new Float64Array([0.25, 0.75]))"), 1.0);
    EXPECT_EQ(fx.evalInt("typed.sumInts(new Int16Array([1, -2, 30]))"), 29);
    EXPECT_EQ(fx.evalInt("typed.sumInts([4, 5])"), 9);
    EXPECT_EQ(
        fx.evalString("try { typed.sumInts(new Float64Array([1, 1.5])) } catch (e) { e.message }"),
        "arg 1 (values) of typed.sumInts: element 1: expected integer, got non-integer number");
    EXPECT_EQ(
        fx.evalString("try { typed.sumInts(new BigInt64Array([1n])) } catch (e) { e.message }"),
        "arg 1 (values) of typed.sumInts: element 0: expected number, got bigint");
    EXPECT_EQ(fx.evalString("String(typed.sumBig(new BigInt64Array([2n ** 40n, 1n])))"),
              "1099511627777");
    EXPECT_EQ(
        fx.evalString(
            "try { typed.firstUnsigned(new BigInt64Array([-1n])) } catch (e) { e.message }"),
        "arg 1 (values) of typed.firstUnsigned: element 0: uint64 cannot represent this bigint");
    EXPECT_EQ(fx.evalString("try { typed.sumInts('x') } catch (e) { e.message }"),
              "arg 1 (values) of typed.sumInts: expected array or Int32Array, got string");
    EXPECT_EQ(fx.evalString("JSON.stringify(typed.squares(4))"), "[0,1,4,9]");
}

TEST(TypedArrays, SpanParametersUseAMatchingTypedArrayInPlace) {
    TypedFixture fx;
    fx.evalString("globalThis.a = new Float32Array([1, 2, 3]); 'ok'");
    EXPECT_TRUE(fx.evalBool("typed.spanAddress(a) === typed.viewAddress(a)"));
    EXPECT_TRUE(fx.evalBool("typed.spanAddress(a.subarray(1)) === typed.viewAddress(a) + 4n"));
    EXPECT_EQ(fx.evalDouble("typed.spanSum(new Float64Array([1, 2]))"), 3.0);
    EXPECT_EQ(fx.evalDouble("typed.spanSum([1, 2, 3])"), 6.0);
    EXPECT_EQ(
        fx.evalDouble("typed.spanSum(new Float32Array(new ArrayBuffer(8, { maxByteLength: 16 })))"),
        0.0);
}

TEST(TypedArrays, WritableSpanParametersWriteIntoJSMemory) {
    TypedFixture fx;
    EXPECT_EQ(fx.evalString("(() => { const a = new Float32Array([1, 2]); typed.scale(a, 3);"
                            " return a.join(','); })()"),
              "3,6");
    EXPECT_EQ(fx.evalString("(() => { const b = new Uint8Array(3); typed.fillBytes(b);"
                            " return b.join(','); })()"),
              "1,2,3");
    EXPECT_EQ(fx.evalString("(() => { const buffer = new ArrayBuffer(2); typed.fillBytes(buffer);"
                            " return new Uint8Array(buffer).join(','); })()"),
              "1,2");
    EXPECT_EQ(
        fx.evalString("try { typed.scale(new Float64Array(1), 2) } catch (e) { e.message }"),
        "arg 1 (values) of typed.scale: expected Float32Array (not shared, not resizable), got "
        "Float64Array");
    EXPECT_EQ(
        fx.evalString("try { typed.scale([1], 2) } catch (e) { e.message }"),
        "arg 1 (values) of typed.scale: expected Float32Array (not shared, not resizable), got "
        "array");
}

TEST(TypedArrays, TypedViewHandsCxxMemoryToJSAsATypedArray) {
    TypedFixture fx;
    EXPECT_TRUE(fx.evalBool("typed.ramp(3) instanceof Float32Array"));
    EXPECT_EQ(fx.evalString("typed.ramp(3).join(',')"), "0,0.5,1");
    EXPECT_EQ(fx.evalString("typed.bigs().join(',')"), "-1,4611686018427387904");
    EXPECT_EQ(fx.evalString("try { typed.viewAddress(new Int32Array(1)) } catch (e) { e.message }"),
              "arg 1 (values) of typed.viewAddress: expected Float32Array, got Int32Array");
}

TEST(TypedArrays, InTheDts) {
    const auto dts = Alka::generateDts<TypedTest::Mod>();
    EXPECT_THAT(dts,
                testing::HasSubstr("function sumFloats(values: number[] | Float32Array): number;"));
    EXPECT_THAT(dts, testing::HasSubstr(
                         "function sumBig(values: (number | bigint)[] | BigInt64Array): bigint;"));
    EXPECT_THAT(dts, testing::HasSubstr("function squares(n: number): number[];"));
    EXPECT_THAT(dts,
                testing::HasSubstr("function scale(values: Float32Array, factor: number): void;"));
    EXPECT_THAT(dts, testing::HasSubstr("function ramp(n: number): Float32Array;"));
    EXPECT_THAT(dts, testing::HasSubstr("function bigs(): BigInt64Array;"));
}
