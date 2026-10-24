#pragma once

#include <Alka/Convert/Converter.hpp>
#include <Alka/Convert/Primitives.hpp>
#include <Alka/Convert/TypedArrays.hpp>
#include <Alka/Core/FixedString.hpp>
#include <Alka/Core/NameOf.hpp>
#include <Alka/Detail/State.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Alka {

namespace Detail {

template <class T>
concept ByteLike = std::same_as<T, std::byte> || std::same_as<T, std::uint8_t>;

[[nodiscard]] inline auto arrayElement(ConvertCtx& cx, const v8::Local<v8::Array> array,
                                       const std::uint32_t index, v8::TryCatch& tryCatch)
    -> Result<v8::Local<v8::Value>> {
    v8::Local<v8::Value> element;
    if (!array->Get(cx.mContext, index).ToLocal(&element)) {
        return std::unexpected(caughtError(cx.mIsolate, cx.mContext, tryCatch));
    }
    return element;
}

template <class T>
concept ScalarElement = TypedElement<T> || std::same_as<T, bool>;

template <ScalarElement T>
[[nodiscard]] auto scalarFromElement(v8::Isolate* isolate, const v8::Local<v8::Value> element,
                                     T& out) -> bool {
    if constexpr (std::same_as<T, bool>) {
        if (!element->IsBoolean()) {
            return false;
        }
        out = element->BooleanValue(isolate);
        return true;
    } else if constexpr (std::floating_point<T>) {
        if (!element->IsNumber()) {
            return false;
        }
        out = static_cast<T>(element.As<v8::Number>()->Value());
        return true;
    } else if constexpr (sizeof(T) == 8) {
        if (element->IsBigInt()) {
            auto lossless = false;
            if constexpr (std::is_signed_v<T>) {
                out = static_cast<T>(element.As<v8::BigInt>()->Int64Value(&lossless));
            } else {
                out = static_cast<T>(element.As<v8::BigInt>()->Uint64Value(&lossless));
            }
            return lossless;
        }
        if (!element->IsNumber()) {
            return false;
        }
        const auto converted = integerFromNumber<T>(element.As<v8::Number>()->Value());
        return converted ? (out = *converted, true) : false;
    } else {
        if (!element->IsNumber()) {
            return false;
        }
        const auto converted = integerFromNumber<T>(element.As<v8::Number>()->Value());
        return converted ? (out = *converted, true) : false;
    }
}

template <ScalarElement T, class Alloc>
[[nodiscard]] auto scalarsFromArray(ConvertCtx& cx, const v8::Local<v8::Array> array)
    -> std::optional<Result<std::vector<T, Alloc>>> {
    struct Pass {
        v8::Isolate* mIsolate{};
        std::vector<T, Alloc> mOut;
        std::uint32_t mBadIndex = 0;
        v8::Global<v8::Value> mBad;
        bool mHole = false;
    } pass{.mIsolate = cx.mIsolate, .mOut = {}, .mBadIndex = 0, .mBad = {}, .mHole = false};
    pass.mOut.reserve((std::min)(static_cast<std::size_t>(array->Length()), std::size_t{4096}));
    v8::TryCatch tryCatch(cx.mIsolate);
    const auto step = [](const std::uint32_t index, const v8::Local<v8::Value> element,
                         void* data) -> v8::Array::CallbackResult {
        auto& state = *static_cast<Pass*>(data);
        if (index != state.mOut.size()) {
            state.mHole = true; // an index was skipped
            return v8::Array::CallbackResult::kBreak;
        }
        T value{};
        if (!scalarFromElement<T>(state.mIsolate, element, value)) {
            state.mBadIndex = index;
            state.mBad.Reset(state.mIsolate, element);
            return v8::Array::CallbackResult::kBreak;
        }
        state.mOut.push_back(value);
        return v8::Array::CallbackResult::kContinue;
    };
    if (array->Iterate(cx.mContext, step, &pass).IsNothing()) {
        return std::unexpected(caughtError(cx.mIsolate, cx.mContext, tryCatch));
    }
    if (pass.mHole || (pass.mBad.IsEmpty() && pass.mOut.size() != array->Length())) {
        return std::nullopt;
    }
    if (!pass.mBad.IsEmpty()) {
        auto error = Alka::fromJs<T>(cx, pass.mBad.Get(cx.mIsolate));
        return std::unexpected(ConvertError::forElement(
            error ? ConvertError{"a lossy bigint"} : std::move(error).error(), pass.mBadIndex));
    }
    return std::move(pass.mOut);
}

} // namespace Detail

template <class T, class Alloc>
    requires(!Detail::ByteLike<T>)
struct Converter<std::vector<T, Alloc>> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value)
        -> Result<std::vector<T, Alloc>> {
        static_assert(FromJsConvertible<T>, "Alka: the element type is not convertible from JS");
        if constexpr (Detail::TypedElement<T>) {
            if (!value.IsEmpty() && value->IsTypedArray()) {
                return Detail::typedArrayToVector<T, Alloc>(value);
            }
        }
        if (value.IsEmpty() || !value->IsArray()) {
            if constexpr (Detail::TypedElement<T>) {
                return std::unexpected(ConvertError::typeMismatch(
                    "array or " + std::string(Detail::typedArrayName(Detail::typedKindOf<T>())),
                    describeJsValue(value)));
            } else {
                return std::unexpected(ConvertError::typeMismatch("array", describeJsValue(value)));
            }
        }
        const auto array = value.As<v8::Array>();
        if constexpr (Detail::ScalarElement<T>) {
            if (auto converted = Detail::scalarsFromArray<T, Alloc>(cx, array)) {
                return std::move(*converted);
            }
        }
        std::vector<T, Alloc> out;
        out.reserve((std::min)(static_cast<std::size_t>(array->Length()), std::size_t{4096}));
        v8::TryCatch tryCatch(cx.mIsolate);
        for (std::uint32_t i = 0; i < array->Length(); ++i) {
            auto element = Detail::arrayElement(cx, array, i, tryCatch);
            if (!element) {
                return std::unexpected(ConvertError::forElement(std::move(element).error(), i));
            }
            auto converted = Alka::fromJs<T>(cx, *element);
            if (!converted) {
                return std::unexpected(ConvertError::forElement(std::move(converted).error(), i));
            }
            out.push_back(std::move(*converted));
        }
        return out;
    }

    static auto toJs(ConvertCtx& cx, const std::vector<T, Alloc>& value) -> v8::Local<v8::Value> {
        static_assert(ToJsConvertible<T>, "Alka: the element type is not convertible to JS");
        if constexpr (Detail::ScalarElement<T>) {
            std::vector<v8::Local<v8::Value>> elements;
            elements.reserve(value.size());
            for (const T element : value) {
                elements.push_back(Alka::toJs(cx, element));
            }
            return v8::Array::New(cx.mIsolate, elements.data(), elements.size());
        }
        const auto array = v8::Array::New(cx.mIsolate, static_cast<int>(value.size()));
        for (std::uint32_t i = 0; i < value.size(); ++i) {
            if (array->CreateDataProperty(cx.mContext, i, Alka::toJs(cx, value[i])).IsNothing()) {
                break; // execution is terminating
            }
        }
        return array;
    }
};

namespace Detail {

template <class Sequence> struct SequenceConverter {
    using T = typename Sequence::value_type;
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<Sequence> {
        auto items = Converter<std::vector<T>>::fromJs(cx, value);
        if (!items) {
            return std::unexpected(std::move(items).error());
        }
        return Sequence(std::make_move_iterator(items->begin()),
                        std::make_move_iterator(items->end()));
    }
    static auto toJs(ConvertCtx& cx, const Sequence& value) -> v8::Local<v8::Value> {
        const auto array = v8::Array::New(cx.mIsolate, static_cast<int>(value.size()));
        std::uint32_t i = 0;
        for (const auto& element : value) {
            if (array->CreateDataProperty(cx.mContext, i++, Alka::toJs(cx, element)).IsNothing()) {
                break; // execution is terminating
            }
        }
        return array;
    }
};

} // namespace Detail

template <class T, class Alloc>
    requires(!Detail::ByteLike<T>)
struct Converter<std::deque<T, Alloc>> : Detail::SequenceConverter<std::deque<T, Alloc>> {};

template <class T, class Alloc>
    requires(!Detail::ByteLike<T>)
struct Converter<std::list<T, Alloc>> : Detail::SequenceConverter<std::list<T, Alloc>> {};

template <class T, std::size_t N> struct Converter<std::array<T, N>> {
    static auto fromJs(ConvertCtx& cx, v8::Local<v8::Value> value) -> Result<std::array<T, N>> {
        auto vec = Converter<std::vector<T>>::fromJs(cx, value);
        if (!vec) {
            return std::unexpected(std::move(vec).error());
        }
        if (vec->size() != N) {
            return std::unexpected(ConvertError{"expected an array of length " + std::to_string(N) +
                                                ", got length " + std::to_string(vec->size())});
        }
        std::array<T, N> out;
        std::move(vec->begin(), vec->end(), out.begin());
        return out;
    }
    static auto toJs(ConvertCtx& cx, const std::array<T, N>& value) -> v8::Local<v8::Value> {
        const auto array = v8::Array::New(cx.mIsolate, static_cast<int>(N));
        for (std::uint32_t i = 0; i < N; ++i) {
            if (array->CreateDataProperty(cx.mContext, i, Alka::toJs(cx, value[i])).IsNothing()) {
                break; // execution is terminating
            }
        }
        return array;
    }
};

namespace Detail {

template <class MapT>
auto stringMapFromJs(ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<MapT> {
    using V = MapT::mapped_type;
    if (value.IsEmpty() || !value->IsObject() || value->IsArray()) {
        return std::unexpected(ConvertError::typeMismatch("plain object", describeJsValue(value)));
    }
    const auto object = value.As<v8::Object>();
    v8::TryCatch tryCatch(cx.mIsolate);
    v8::Local<v8::Array> names;
    if (!object
             ->GetOwnPropertyNames(
                 cx.mContext,
                 static_cast<v8::PropertyFilter>(v8::ONLY_ENUMERABLE | v8::SKIP_SYMBOLS),
                 v8::KeyConversionMode::kConvertToString)
             .ToLocal(&names)) {
        return std::unexpected(caughtError(cx.mIsolate, cx.mContext, tryCatch));
    }
    MapT out;
    for (std::uint32_t i = 0; i < names->Length(); ++i) {
        auto keyValue = arrayElement(cx, names, i, tryCatch);
        if (!keyValue) {
            return std::unexpected(std::move(keyValue).error());
        }
        auto key = Alka::fromJs<std::string>(cx, *keyValue);
        if (!key) {
            return std::unexpected(std::move(key).error());
        }
        v8::Local<v8::Value> element;
        if (!object->Get(cx.mContext, *keyValue).ToLocal(&element)) {
            return std::unexpected(
                ConvertError::forKey(caughtError(cx.mIsolate, cx.mContext, tryCatch), *key));
        }
        auto converted = Alka::fromJs<V>(cx, element);
        if (!converted) {
            return std::unexpected(ConvertError::forKey(std::move(converted).error(), *key));
        }
        out.emplace(std::move(*key), std::move(*converted));
    }
    return out;
}

template <class MapT>
auto stringMapToJs(ConvertCtx& cx, const MapT& value) -> v8::Local<v8::Value> {
    const auto object = v8::Object::New(cx.mIsolate);
    for (const auto& [key, mapped] : value) {
        if (object->CreateDataProperty(cx.mContext, v8Str(cx.mIsolate, key), Alka::toJs(cx, mapped))
                .IsNothing()) {
            break; // execution is terminating
        }
    }
    return object;
}

template <class MapT>
auto keyedMapFromJs(ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<MapT> {
    using K = MapT::key_type;
    using V = MapT::mapped_type;
    if (value.IsEmpty() || !value->IsMap()) {
        return std::unexpected(ConvertError::typeMismatch("Map", describeJsValue(value)));
    }
    const auto flat = value.As<v8::Map>()->AsArray();
    MapT out;
    v8::TryCatch tryCatch(cx.mIsolate);
    for (std::uint32_t i = 0; i + 1 < flat->Length(); i += 2) {
        auto keyValue = arrayElement(cx, flat, i, tryCatch);
        if (!keyValue) {
            return std::unexpected(std::move(keyValue).error());
        }
        auto mappedValue = arrayElement(cx, flat, i + 1, tryCatch);
        if (!mappedValue) {
            return std::unexpected(std::move(mappedValue).error());
        }
        auto key = Alka::fromJs<K>(cx, *keyValue);
        if (!key) {
            return std::unexpected(ConvertError::forElement(std::move(key).error(), i / 2));
        }
        auto mapped = Alka::fromJs<V>(cx, *mappedValue);
        if (!mapped) {
            return std::unexpected(ConvertError::forElement(std::move(mapped).error(), i / 2));
        }
        out.emplace(std::move(*key), std::move(*mapped));
    }
    return out;
}

template <class MapT> auto keyedMapToJs(ConvertCtx& cx, const MapT& value) -> v8::Local<v8::Value> {
    auto map = v8::Map::New(cx.mIsolate);
    for (const auto& [key, mapped] : value) {
        if (!map->Set(cx.mContext, Alka::toJs(cx, key), Alka::toJs(cx, mapped)).ToLocal(&map)) {
            break; // an exception (termination) is pending
        }
    }
    return map;
}

template <class K>
concept StringKey = std::same_as<K, std::string>;

} // namespace Detail

template <class K, class V, class C, class A> struct Converter<std::map<K, V, C, A>> {
    using MapT = std::map<K, V, C, A>;
    static Result<MapT> fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value) {
        if constexpr (Detail::StringKey<K>) {
            return Detail::stringMapFromJs<MapT>(cx, value);
        } else {
            return Detail::keyedMapFromJs<MapT>(cx, value);
        }
    }
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, const MapT& value) {
        if constexpr (Detail::StringKey<K>) {
            return Detail::stringMapToJs(cx, value);
        } else {
            return Detail::keyedMapToJs(cx, value);
        }
    }
};

template <class K, class V, class H, class E, class A>
struct Converter<std::unordered_map<K, V, H, E, A>> {
    using MapT = std::unordered_map<K, V, H, E, A>;
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<MapT> {
        if constexpr (Detail::StringKey<K>) {
            return Detail::stringMapFromJs<MapT>(cx, value);
        } else {
            return Detail::keyedMapFromJs<MapT>(cx, value);
        }
    }
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, const MapT& value) {
        if constexpr (Detail::StringKey<K>) {
            return Detail::stringMapToJs(cx, value);
        } else {
            return Detail::keyedMapToJs(cx, value);
        }
    }
};

namespace Detail {

template <class SetT>
auto setFromJs(ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<SetT> {
    using T = SetT::value_type;
    if (value.IsEmpty() || !value->IsSet()) {
        return std::unexpected(ConvertError::typeMismatch("Set", describeJsValue(value)));
    }
    const auto flat = value.As<v8::Set>()->AsArray();
    SetT out;
    v8::TryCatch tryCatch(cx.mIsolate);
    for (std::uint32_t i = 0; i < flat->Length(); ++i) {
        auto element = arrayElement(cx, flat, i, tryCatch);
        if (!element) {
            return std::unexpected(std::move(element).error());
        }
        auto converted = Alka::fromJs<T>(cx, *element);
        if (!converted) {
            return std::unexpected(ConvertError::forElement(std::move(converted).error(), i));
        }
        out.insert(std::move(*converted));
    }
    return out;
}

template <class SetT> auto setToJs(ConvertCtx& cx, const SetT& value) -> v8::Local<v8::Value> {
    auto set = v8::Set::New(cx.mIsolate);
    for (const auto& element : value) {
        if (!set->Add(cx.mContext, Alka::toJs(cx, element)).ToLocal(&set)) {
            break; // an exception (termination) is pending
        }
    }
    return set;
}

} // namespace Detail

template <class T, class C, class A> struct Converter<std::set<T, C, A>> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value)
        -> Result<std::set<T, C, A>> {
        return Detail::setFromJs<std::set<T, C, A>>(cx, value);
    }
    static auto toJs(ConvertCtx& cx, const std::set<T, C, A>& value) -> v8::Local<v8::Value> {
        return Detail::setToJs(cx, value);
    }
};

template <class T, class H, class E, class A> struct Converter<std::unordered_set<T, H, E, A>> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value)
        -> Result<std::unordered_set<T, H, E, A>> {
        return Detail::setFromJs<std::unordered_set<T, H, E, A>>(cx, value);
    }
    static auto toJs(ConvertCtx& cx, const std::unordered_set<T, H, E, A>& value)
        -> v8::Local<v8::Value> {
        return Detail::setToJs(cx, value);
    }
};

template <class T> struct Converter<std::optional<T>> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value)
        -> Result<std::optional<T>> {
        if (value.IsEmpty() || value->IsNullOrUndefined()) {
            return std::optional<T>{};
        }
        auto inner = Alka::fromJs<T>(cx, value);
        if (!inner) {
            return std::unexpected(std::move(inner).error());
        }
        return std::optional<T>(std::move(*inner));
    }
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, const std::optional<T>& value) {
        if (!value.has_value()) {
            return v8::Undefined(cx.mIsolate);
        }
        return Alka::toJs(cx, *value);
    }
};

namespace Detail {

template <class Tuple, std::size_t... Is>
auto tupleFromJs(ConvertCtx& cx, const v8::Local<v8::Value> value, std::index_sequence<Is...>)
    -> Result<Tuple> {
    if (value.IsEmpty() || !value->IsArray()) {
        return std::unexpected(ConvertError::typeMismatch("array (tuple)", describeJsValue(value)));
    }
    const auto array = value.As<v8::Array>();
    if (array->Length() != sizeof...(Is)) {
        return std::unexpected(ConvertError{"expected a tuple of length " +
                                            std::to_string(sizeof...(Is)) + ", got length " +
                                            std::to_string(array->Length())});
    }
    v8::TryCatch tryCatch(cx.mIsolate);
    std::tuple<Result<std::tuple_element_t<Is, Tuple>>...> converted{[&] {
        auto element = arrayElement(cx, array, static_cast<std::uint32_t>(Is), tryCatch);
        if (!element) {
            return Result<std::tuple_element_t<Is, Tuple>>(
                std::unexpected(std::move(element).error()));
        }
        return Alka::fromJs<std::tuple_element_t<Is, Tuple>>(cx, *element);
    }()...};

    const ConvertError* firstError = nullptr;
    std::size_t errorIndex = 0;
    (void)((std::get<Is>(converted).has_value()
                ? false
                : (firstError = &std::get<Is>(converted).error(), errorIndex = Is, true)) ||
           ...);
    if (firstError != nullptr) {
        return std::unexpected(ConvertError::forElement(ConvertError{*firstError}, errorIndex));
    }
    return Tuple{std::move(*std::get<Is>(converted))...};
}

template <class Tuple, std::size_t... Is>
auto tupleToJs(ConvertCtx& cx, const Tuple& value, std::index_sequence<Is...>)
    -> v8::Local<v8::Value> {
    const auto array = v8::Array::New(cx.mIsolate, sizeof...(Is));
    ((std::ignore = array->CreateDataProperty(cx.mContext, static_cast<std::uint32_t>(Is),
                                              Alka::toJs(cx, std::get<Is>(value)))),
     ...);
    return array;
}

} // namespace Detail

template <class A, class B> struct Converter<std::pair<A, B>> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value)
        -> Result<std::pair<A, B>> {
        return Detail::tupleFromJs<std::pair<A, B>>(cx, value, std::make_index_sequence<2>{});
    }
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, const std::pair<A, B>& value) {
        return Detail::tupleToJs(cx, value, std::make_index_sequence<2>{});
    }
};

template <class... Ts>
    requires(sizeof...(Ts) >= 1)
struct Converter<std::tuple<Ts...>> {
    static auto fromJs(ConvertCtx& cx, v8::Local<v8::Value> value) -> Result<std::tuple<Ts...>> {
        return Detail::tupleFromJs<std::tuple<Ts...>>(cx, value, std::index_sequence_for<Ts...>{});
    }
    static v8::Local<v8::Value> toJs(ConvertCtx& cx, const std::tuple<Ts...>& value) {
        return Detail::tupleToJs(cx, value, std::index_sequence_for<Ts...>{});
    }
};

namespace Detail {

template <class... Ts> [[nodiscard]] consteval auto variantNames() {
    constexpr auto join = []<class First, class... Rest>(auto self, std::type_identity<First>,
                                                         std::type_identity<Rest>... rest) {
        if constexpr (sizeof...(Rest) == 0) {
            return typeNameOf<First>;
        } else {
            return typeNameOf<First> + FixedString{" | "} + self(self, rest...);
        }
    };
    return join(join, std::type_identity<Ts>{}...);
}

} // namespace Detail

template <class... Ts> struct Converter<std::variant<Ts...>> {
    static constexpr auto kNames = Detail::variantNames<Ts...>();

    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value)
        -> Result<std::variant<Ts...>> {
        std::optional<std::variant<Ts...>> out;
        std::optional<ConvertError> thrown;
        (void)(([&]<class T>(std::type_identity<T>) {
                   auto converted = Alka::fromJs<T>(cx, value);
                   if (converted) {
                       out.emplace(std::in_place_type<T>, std::move(*converted));
                       return true;
                   }
                   if (converted.error().mJsException) {
                       thrown = std::move(converted).error();
                       return true;
                   }
                   return false;
               }(std::type_identity<Ts>{})) ||
               ...);
        if (thrown) {
            return std::unexpected(std::move(*thrown));
        }
        if (!out) {
            return std::unexpected(
                ConvertError::typeMismatch(kNames.view(), describeJsValue(value)));
        }
        return std::move(*out);
    }

    static auto toJs(ConvertCtx& cx, const std::variant<Ts...>& value) -> v8::Local<v8::Value> {
        return std::visit([&](const auto& v) { return Alka::toJs(cx, v); }, value);
    }
};

} // namespace Alka
