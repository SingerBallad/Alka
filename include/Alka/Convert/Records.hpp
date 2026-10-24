#pragma once

#include <Alka/Binding/Bind.hpp>
#include <Alka/Convert/Converter.hpp>
#include <Alka/Detail/State.hpp>

#include <v8.h>

#include <concepts>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Alka {

namespace Detail {

template <class X> inline constexpr bool isOptional = false;
template <class X> inline constexpr bool isOptional<std::optional<X>> = true;

template <class C, class M> auto recordFieldProbe(M C::*) -> M;

template <class Desc>
using RecordFieldTypeOf =
    std::remove_cvref_t<decltype(recordFieldProbe(std::get<0>(Desc::accessors)))>;

template <BoundRecord T> [[nodiscard]] consteval bool validRecordMembers() {
    return []<std::size_t... Is>(std::index_sequence<Is...>) {
        return ((std::tuple_element_t<Is, MembersTupleOf<T>>::kind == MemberKind::Property) &&
                ... && true);
    }(std::make_index_sequence<memberCount<T>>{});
}

} // namespace Detail

template <BoundRecord T> struct Converter<T> {
    static_assert(Detail::validRecordMembers<T>(),
                  "Alka: a Record's members must all be field props (Alka::prop<&T::field>())");
    static_assert(std::default_initializable<T>,
                  "Alka: a Record type must be default-constructible");

    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<T> {
        if (value.IsEmpty() || !value->IsObject() || value->IsArray()) {
            return std::unexpected(ConvertError::typeMismatch("object", describeJsValue(value)));
        }
        const auto object = value.As<v8::Object>();
        T out{};
        std::optional<ConvertError> error;
        v8::TryCatch tryCatch(cx.mIsolate);
        forEachMember<T>([&]<class D>(const D&) {
            static_assert(D::isField, "Alka: a Record's props must be fields");
            if (error) {
                return;
            }
            using F = Detail::RecordFieldTypeOf<D>;
            constexpr auto field = std::get<0>(D::accessors);
            v8::Local<v8::Value> raw;
            if (!object->Get(cx.mContext, Detail::nameKey<D>(cx.mIsolate)).ToLocal(&raw)) {
                error = ConvertError::forKey(
                    Detail::caughtError(cx.mIsolate, cx.mContext, tryCatch), D::name);
                return;
            }
            if (raw->IsUndefined() && Detail::isOptional<F>) {
                return;
            }
            auto converted = Alka::fromJs<F>(cx, raw);
            if (!converted) {
                error = ConvertError::forKey(std::move(converted).error(), D::name);
                return;
            }
            out.*field = std::move(*converted);
        });
        if (error) {
            return std::unexpected(std::move(*error));
        }
        return out;
    }

    static auto toJs(ConvertCtx& cx, const T& value) -> v8::Local<v8::Value> {
        const auto object = v8::Object::New(cx.mIsolate);
        forEachMember<T>([&]<class D>(const D&) {
            constexpr auto field = std::get<0>(D::accessors);
            if constexpr (Detail::isOptional<Detail::RecordFieldTypeOf<D>>) {
                if (!(value.*field).has_value()) {
                    return; // absent, not `undefined`-valued
                }
            }
            std::ignore = object->CreateDataProperty(cx.mContext, Detail::nameKey<D>(cx.mIsolate),
                                                     Alka::toJs(cx, value.*field));
        });
        return object;
    }
};

} // namespace Alka
