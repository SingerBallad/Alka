#pragma once

#include <Alka/Binding/Bind.hpp>
#include <Alka/Binding/Gc.hpp>
#include <Alka/Binding/Idioms.hpp>
#include <Alka/Binding/Params.hpp>
#include <Alka/Binding/Policy.hpp>
#include <Alka/Convert/Buffers.hpp>
#include <Alka/Convert/Chrono.hpp>
#include <Alka/Convert/Classes.hpp>
#include <Alka/Convert/Functional.hpp>
#include <Alka/Core/NameOf.hpp>
#include <Alka/Core/Traits.hpp>
#include <Alka/Js/Abort.hpp>
#include <Alka/Js/Function.hpp>
#include <Alka/Js/Promise.hpp>
#include <Alka/Js/Retained.hpp>
#include <Alka/Js/Task.hpp>
#include <Alka/Js/ThreadSafeFunction.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Alka {

enum class TsStyle { GlobalNamespace, EsModule };

struct TsType {
    std::string mText;
    bool mOptionalLike = false;
};

class TsNameRegistry {
public:
    struct Symbol {
        std::string mModule;
        std::string mName;
    };

    enum class Position : std::uint8_t { Return, Param };

    TsStyle mStyle = TsStyle::GlobalNamespace;
    std::string mCurrentModule;
    Position mPosition = Position::Return;
    Int64Mode mInt64Mode = kDefaultInt64Mode;

    class FlipScope {
    public:
        explicit FlipScope(const TsNameRegistry& reg) : mReg(const_cast<TsNameRegistry&>(reg)) {
            mSaved = mReg.mPosition;
            mReg.mPosition = mSaved == Position::Param ? Position::Return : Position::Param;
        }
        FlipScope(const FlipScope&) = delete;
        FlipScope& operator=(const FlipScope&) = delete;
        ~FlipScope() { mReg.mPosition = mSaved; }

    private:
        TsNameRegistry& mReg;
        Position mSaved;
    };

    void add(const std::type_info& id, std::string modulePath, std::string name) {
        mSymbols.insert_or_assign(std::type_index(id),
                                  Symbol{std::move(modulePath), std::move(name)});
    }

    [[nodiscard]] auto find(const std::type_info& id) const -> const Symbol* {
        const auto it = mSymbols.find(std::type_index(id));
        return it != mSymbols.end() ? &it->second : nullptr;
    }

    [[nodiscard]] auto refer(const std::type_info& id, const std::string_view fallback) const
        -> std::string {
        const auto symbol = find(id);
        if (symbol == nullptr) {
            return std::string(fallback);
        }
        if (mStyle == TsStyle::GlobalNamespace) {
            return symbol->mModule.empty() ? symbol->mName : symbol->mModule + "." + symbol->mName;
        }
        if (symbol->mModule == mCurrentModule) {
            return symbol->mName;
        }
        return "import(\"" + symbol->mModule + "\")." + symbol->mName;
    }

private:
    std::unordered_map<std::type_index, Symbol> mSymbols;
};

template <class T> struct TsTypeName;

template <class T>
concept TsNameable = requires(const TsNameRegistry& reg) {
    { TsTypeName<std::remove_cvref_t<T>>::get(reg) } -> std::same_as<TsType>;
};

namespace Detail {

[[nodiscard]] inline auto parenthesizeCompound(std::string text) -> std::string {
    if (text.find(" | ") != std::string::npos || text.find("=>") != std::string::npos) {
        return "(" + std::move(text) + ")";
    }
    return text;
}

template <class T> [[nodiscard]] auto tsNameOf(const TsNameRegistry& reg) -> TsType {
    static_assert(TsNameable<T>,
                  "Alka: no TypeScript mapping for this type; specialize Alka::TsTypeName or "
                  "override the declaration with Alka::tsType<\"...\">");
    return TsTypeName<std::remove_cvref_t<T>>::get(reg);
}

} // namespace Detail

template <class T>
    requires std::integral<T> && (!std::same_as<T, bool>) && (!std::same_as<T, char>) &&
             (sizeof(T) < 8)
struct TsTypeName<T> {
    static auto get(const TsNameRegistry&) -> TsType { return {"number"}; }
};

template <class T>
    requires std::integral<T> && (sizeof(T) == 8)
struct TsTypeName<T> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        if (reg.mPosition == TsNameRegistry::Position::Param) {
            return {.mText = "number | bigint"};
        }
        switch (reg.mInt64Mode) {
        case Int64Mode::BigInt:
            return {.mText = "bigint"};
        case Int64Mode::Number:
            return {.mText = "number"};
        case Int64Mode::Auto:
            return {.mText = "number | bigint"};
        }
        return {.mText = "bigint"};
    }
};

template <class T>
    requires std::floating_point<T>
struct TsTypeName<T> {
    static auto get(const TsNameRegistry&) -> TsType { return {.mText = "number"}; }
};

template <> struct TsTypeName<bool> {
    static auto get(const TsNameRegistry&) -> TsType { return {.mText = "boolean"}; }
};

template <> struct TsTypeName<void> {
    static auto get(const TsNameRegistry&) -> TsType { return {.mText = "void"}; }
};

template <> struct TsTypeName<std::string> {
    static auto get(const TsNameRegistry&) -> TsType { return {.mText = "string"}; }
};
template <> struct TsTypeName<std::string_view> : TsTypeName<std::string> {};

template <> struct TsTypeName<const char*> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText =
                    reg.mPosition == TsNameRegistry::Position::Param ? "string | null" : "string"};
    }
};

template <> struct TsTypeName<std::u16string> : TsTypeName<std::string> {};
template <> struct TsTypeName<std::u16string_view> : TsTypeName<std::string> {};
template <> struct TsTypeName<std::wstring> : TsTypeName<std::string> {};
template <> struct TsTypeName<std::filesystem::path> : TsTypeName<std::string> {};

template <> struct TsTypeName<std::monostate> {
    static auto get(const TsNameRegistry&) -> TsType { return {"undefined"}; }
};

template <> struct TsTypeName<std::nullptr_t> {
    static auto get(const TsNameRegistry&) -> TsType { return {"null"}; }
};

template <class T>
    requires BoundClass<T> || BoundRecord<T>
struct TsTypeName<T> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText = reg.refer(typeid(T), boundNameOf<T>())};
    }
};

template <Enum E> struct TsTypeName<E> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText = reg.refer(typeid(E), typeNameOf<E>.view())};
    }
};

template <class T>
    requires BoundClass<std::remove_cv_t<T>>
struct TsTypeName<T*> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText = Detail::tsNameOf<std::remove_cv_t<T>>(reg).mText + " | null"};
    }
};

template <class T>
    requires BoundClass<std::remove_cv_t<T>>
struct TsTypeName<Borrowed<T>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText = Detail::tsNameOf<std::remove_cv_t<T>>(reg).mText + " | null"};
    }
};

template <BoundClass T> struct TsTypeName<std::shared_ptr<T>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText = Detail::tsNameOf<T>(reg).mText + " | null"};
    }
};

template <BoundClass T> struct TsTypeName<std::weak_ptr<T>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText = Detail::tsNameOf<T>(reg).mText + " | null"};
    }
};

template <BoundClass T> struct TsTypeName<std::unique_ptr<T>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText = Detail::tsNameOf<T>(reg).mText + " | null"};
    }
};

namespace Detail {
template <class T>
concept TsByteLike = std::same_as<T, std::byte> || std::same_as<T, std::uint8_t>;
}

template <class T, class Alloc>
    requires(!Detail::TsByteLike<T>)
struct TsTypeName<std::vector<T, Alloc>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        std::string text = Detail::parenthesizeCompound(Detail::tsNameOf<T>(reg).mText) + "[]";
        if constexpr (Detail::TypedElement<T>) {
            if (reg.mPosition == TsNameRegistry::Position::Param) {
                text += " | " + std::string(Detail::typedArrayName(Detail::typedKindOf<T>()));
            }
        }
        return {.mText = std::move(text)};
    }
};

namespace Detail {

[[nodiscard]] inline auto tsBytesName(const TsNameRegistry& reg) -> TsType {
    return {.mText = reg.mPosition == TsNameRegistry::Position::Param
                         ? "ArrayBuffer | ArrayBufferView"
                         : "Uint8Array"};
}

} // namespace Detail

template <class T, class Alloc>
    requires(!Detail::TsByteLike<T>)
struct TsTypeName<std::deque<T, Alloc>> : TsTypeName<std::vector<T>> {};

template <class T, class Alloc>
    requires(!Detail::TsByteLike<T>)
struct TsTypeName<std::list<T, Alloc>> : TsTypeName<std::vector<T>> {};

template <class Rep, class Period> struct TsTypeName<std::chrono::duration<Rep, Period>> {
    static auto get(const TsNameRegistry&) -> TsType { return {.mText = "number"}; }
};

template <class D> struct TsTypeName<std::chrono::time_point<std::chrono::system_clock, D>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText =
                    reg.mPosition == TsNameRegistry::Position::Param ? "Date | number" : "Date"};
    }
};

template <Detail::TsByteLike B, class Alloc> struct TsTypeName<std::vector<B, Alloc>> {
    static auto get(const TsNameRegistry& reg) -> TsType { return Detail::tsBytesName(reg); }
};

template <> struct TsTypeName<BufferView> {
    static auto get(const TsNameRegistry& reg) -> TsType { return Detail::tsBytesName(reg); }
};

template <> struct TsTypeName<SharedBufferView> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText = reg.mPosition == TsNameRegistry::Position::Param
                             ? "ArrayBuffer | SharedArrayBuffer | ArrayBufferView"
                             : "Uint8Array"};
    }
};

template <Detail::TsByteLike B> struct TsTypeName<std::span<const B>> {
    static auto get(const TsNameRegistry& reg) -> TsType { return Detail::tsBytesName(reg); }
};

template <class T>
    requires(!Detail::TsByteLike<T>)
struct TsTypeName<std::span<const T>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return TsTypeName<std::vector<T>>::get(reg);
    }
};

template <Detail::TsByteLike B> struct TsTypeName<std::span<B>> {
    static auto get(const TsNameRegistry&) -> TsType { return {"ArrayBuffer | ArrayBufferView"}; }
};

template <Detail::TypedElement T> struct TsTypeName<std::span<T>> {
    static auto get(const TsNameRegistry&) -> TsType {
        return {.mText = std::string(Detail::typedArrayName(Detail::typedKindOf<T>()))};
    }
};

template <Detail::TypedElement T> struct TsTypeName<TypedView<T>> {
    static auto get(const TsNameRegistry&) -> TsType {
        return {.mText = std::string(Detail::typedArrayName(Detail::typedKindOf<T>()))};
    }
};

template <Detail::TypedElement T> struct TsTypeName<SharedTypedView<T>> {
    static auto get(const TsNameRegistry&) -> TsType {
        return {.mText = std::string(Detail::typedArrayName(Detail::typedKindOf<T>()))};
    }
};

template <class T> struct TsTypeName<Rest<T>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText = Detail::parenthesizeCompound(Detail::tsNameOf<T>(reg).mText) + "[]"};
    }
};

template <class V, class E> struct TsTypeName<std::expected<V, E>> {
    static auto get(const TsNameRegistry& reg) -> TsType { return Detail::tsNameOf<V>(reg); }
};

template <class T> struct TsTypeName<Promise<T>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText = "Promise<" + Detail::tsNameOf<T>(reg).mText + ">"};
    }
};

template <class T, std::size_t N> struct TsTypeName<std::array<T, N>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText = Detail::parenthesizeCompound(Detail::tsNameOf<T>(reg).mText) + "[]"};
    }
};

namespace Detail {

template <class K, class V> [[nodiscard]] auto tsMapName(const TsNameRegistry& reg) -> TsType {
    if constexpr (std::same_as<K, std::string>) {
        return {.mText = "Record<string, " + tsNameOf<V>(reg).mText + ">"};
    } else {
        return {.mText = "Map<" + tsNameOf<K>(reg).mText + ", " + tsNameOf<V>(reg).mText + ">"};
    }
}

} // namespace Detail

template <class K, class V, class C, class A> struct TsTypeName<std::map<K, V, C, A>> {
    static auto get(const TsNameRegistry& reg) -> TsType { return Detail::tsMapName<K, V>(reg); }
};
template <class K, class V, class H, class E, class A>
struct TsTypeName<std::unordered_map<K, V, H, E, A>> {
    static auto get(const TsNameRegistry& reg) -> TsType { return Detail::tsMapName<K, V>(reg); }
};

template <class T, class C, class A> struct TsTypeName<std::set<T, C, A>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText = "Set<" + Detail::tsNameOf<T>(reg).mText + ">"};
    }
};
template <class T, class H, class E, class A> struct TsTypeName<std::unordered_set<T, H, E, A>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText = "Set<" + Detail::tsNameOf<T>(reg).mText + ">"};
    }
};

template <class T> struct TsTypeName<std::optional<T>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText = Detail::tsNameOf<T>(reg).mText, .mOptionalLike = true};
    }
};

namespace Detail {

template <class... Ts>
[[nodiscard]] auto tsJoin(const TsNameRegistry& reg, std::string_view separator) -> std::string {
    std::string out;
    ((out += out.empty() ? "" : separator, out += tsNameOf<Ts>(reg).mText), ...);
    return out;
}

} // namespace Detail

template <class A, class B> struct TsTypeName<std::pair<A, B>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText = "[" + Detail::tsJoin<A, B>(reg, ", ") + "]"};
    }
};

template <class... Ts> struct TsTypeName<std::tuple<Ts...>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText = "[" + Detail::tsJoin<Ts...>(reg, ", ") + "]"};
    }
};

template <class... Ts> struct TsTypeName<std::variant<Ts...>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText = Detail::tsJoin<Ts...>(reg, " | ")};
    }
};

namespace Detail {

template <class R, class... Args>
[[nodiscard]] auto tsCallableName(const TsNameRegistry& reg) -> TsType {
    std::string out = "(";
    {
        const TsNameRegistry::FlipScope flip(reg);
        std::size_t index = 0;
        ((out += index > 0 ? ", " : "", out += "arg" + std::to_string(index++), out += ": ",
          out += tsNameOf<Args>(reg).mText),
         ...);
    }
    out += ") => ";
    out += tsNameOf<R>(reg).mText;
    return {.mText = std::move(out)};
}

} // namespace Detail

template <class R, class... Args> struct TsTypeName<std::function<R(Args...)>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return Detail::tsCallableName<R, Args...>(reg);
    }
};

template <class R, class... Args> struct TsTypeName<Function<R(Args...)>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return Detail::tsCallableName<R, Args...>(reg);
    }
};

namespace Detail {

template <class R, class ArgsTuple>
[[nodiscard]] auto tsJsCallableName(const TsNameRegistry& reg) -> TsType {
    std::string out = "(";
    {
        const TsNameRegistry::FlipScope flip(reg);
        std::size_t index = 0;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (
                [&] {
                    using A = std::tuple_element_t<Is, ArgsTuple>;
                    if constexpr (!isInjected<A>) {
                        out += index > 0 ? ", " : "";
                        out += (isRest<A> ? "...arg" : "arg") + std::to_string(index++) + ": " +
                               tsNameOf<std::remove_cvref_t<A>>(reg).mText;
                    }
                }(),
                ...);
        }(std::make_index_sequence<std::tuple_size_v<ArgsTuple>>{});
    }
    out += ") => ";
    out += tsNameOf<std::remove_cvref_t<R>>(reg).mText;
    return {.mText = std::move(out)};
}

} // namespace Detail

template <class R, class... Args> struct TsTypeName<ThreadSafeFunction<R(Args...)>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return Detail::tsCallableName<R, Args...>(reg);
    }
};

template <> struct TsTypeName<AbortSignal> {
    static auto get(const TsNameRegistry&) -> TsType { return {.mText = "AbortSignal"}; }
};

template <class F> struct TsTypeName<NativeFunction<F>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        using Traits = CallableTraitsOf<F>;
        return Detail::tsJsCallableName<typename Traits::Ret, typename Traits::ArgsTuple>(reg);
    }
};

#if ALKA_HAS_GENERATOR
template <class R, class V, class A> struct TsTypeName<std::generator<R, V, A>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {"IterableIterator<" +
                Detail::tsNameOf<std::ranges::range_value_t<std::generator<R, V, A>>>(reg).mText +
                ">"};
    }
};
#endif

template <class T> struct TsTypeName<Retained<T>> : TsTypeName<T> {};

template <class T> struct TsTypeName<Task<T>> : TsTypeName<Promise<T>> {};

template <class E> struct TsTypeName<JsIterator<E>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {.mText = "IterableIterator<" + Detail::tsNameOf<E>(reg).mText + ">"};
    }
};

template <> struct TsTypeName<Value> {
    static auto get(const TsNameRegistry&) -> TsType { return {.mText = "unknown"}; }
};

template <> struct TsTypeName<Object> {
    static auto get(const TsNameRegistry&) -> TsType { return {.mText = "object"}; }
};

template <> struct TsTypeName<Array> {
    static auto get(const TsNameRegistry&) -> TsType { return {.mText = "unknown[]"}; }
};

#if ALKA_HAS_CPPGC
template <class U>
    requires isGcClass<U>
struct TsTypeName<cppgc::Member<U>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {Detail::tsNameOf<U>(reg).mText + " | null"};
    }
};

template <class U>
    requires isGcClass<U>
struct TsTypeName<cppgc::WeakMember<U>> : TsTypeName<cppgc::Member<U>> {};

template <class U>
    requires isGcClass<U>
struct TsTypeName<cppgc::Persistent<U>> : TsTypeName<cppgc::Member<U>> {};

template <class U>
    requires isGcClass<U>
struct TsTypeName<cppgc::WeakPersistent<U>> : TsTypeName<cppgc::Member<U>> {};
#endif

} // namespace Alka
