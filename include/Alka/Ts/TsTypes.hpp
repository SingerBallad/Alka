#pragma once

#include <Alka/Bind.hpp>
#include <Alka/Function.hpp>
#include <Alka/NameOf.hpp>
#include <Alka/Traits.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
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

    TsStyle mStyle = TsStyle::GlobalNamespace;
    std::string mCurrentModule;

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
            return symbol->mModule.empty() ? symbol->mName
                                           : symbol->mModule + "." + symbol->mName;
        }
        if (symbol->mModule == mCurrentModule) {
            return symbol->mName;
        }
        return "import(\"" + symbol->mModule + "\")." + symbol->mName;
    }

private:
    std::unordered_map<std::type_index, Symbol> mSymbols;
};

template<class T>
struct TsTypeName;  // specialize to add/override mappings

template<class T>
concept TsNameable = requires(const TsNameRegistry& reg) {
    { TsTypeName<std::remove_cvref_t<T>>::get(reg) } -> std::same_as<TsType>;
};

namespace Detail {

// "(A | B)[]" needs the parens; "number[]" does not.
[[nodiscard]] inline auto parenthesizeCompound(std::string text) -> std::string {
    if (text.find(" | ") != std::string::npos || text.find("=>") != std::string::npos) {
        return "(" + std::move(text) + ")";
    }
    return text;
}

template<class T>
[[nodiscard]] auto tsNameOf(const TsNameRegistry& reg) -> TsType {
    static_assert(TsNameable<T>,
                  "Alka: no TypeScript mapping for this type; specialize Alka::TsTypeName or "
                  "override the declaration with Alka::tsType<\"...\">");
    return TsTypeName<std::remove_cvref_t<T>>::get(reg);
}

}  // namespace Detail

template<class T>
    requires std::integral<T> && (!std::same_as<T, bool>) && (!std::same_as<T, char>) &&
             (sizeof(T) < 8)
struct TsTypeName<T> {
    static auto get(const TsNameRegistry&) -> TsType { return {"number"}; }
};

template<class T>
    requires std::integral<T> && (sizeof(T) == 8)
struct TsTypeName<T> {
    static auto get(const TsNameRegistry&) -> TsType { return {"bigint"}; }
};

template<class T>
    requires std::floating_point<T>
struct TsTypeName<T> {
    static auto get(const TsNameRegistry&) -> TsType { return {"number"}; }
};

template<>
struct TsTypeName<bool> {
    static auto get(const TsNameRegistry&) -> TsType { return {"boolean"}; }
};

template<>
struct TsTypeName<void> {
    static auto get(const TsNameRegistry&) -> TsType { return {"void"}; }
};

template<>
struct TsTypeName<std::string> {
    static auto get(const TsNameRegistry&) -> TsType { return {"string"}; }
};
template<>
struct TsTypeName<std::string_view> : TsTypeName<std::string> {};
template<>
struct TsTypeName<const char*> : TsTypeName<std::string> {};

template<BoundClass T>
struct TsTypeName<T> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {reg.refer(typeid(T), boundNameOf<T>())};
    }
};

template<Enum E>
struct TsTypeName<E> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {reg.refer(typeid(E), typeNameOf<E>.view())};
    }
};

template<class T>
    requires BoundClass<std::remove_cv_t<T>>
struct TsTypeName<T*> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {Detail::tsNameOf<std::remove_cv_t<T>>(reg).mText + " | null"};
    }
};

template<BoundClass T>
struct TsTypeName<std::shared_ptr<T>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {Detail::tsNameOf<T>(reg).mText + " | null"};
    }
};

template<BoundClass T>
struct TsTypeName<std::unique_ptr<T>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {Detail::tsNameOf<T>(reg).mText + " | null"};
    }
};

namespace Detail {
template<class T>
concept TsByteLike = std::same_as<T, std::byte> || std::same_as<T, std::uint8_t>;
}

template<class T, class Alloc>
    requires(!Detail::TsByteLike<T>)
struct TsTypeName<std::vector<T, Alloc>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {Detail::parenthesizeCompound(Detail::tsNameOf<T>(reg).mText) + "[]"};
    }
};

template<Detail::TsByteLike B, class Alloc>
struct TsTypeName<std::vector<B, Alloc>> {
    static auto get(const TsNameRegistry&) -> TsType { return {"Uint8Array"}; }
};

template<Detail::TsByteLike B>
struct TsTypeName<std::span<const B>> {
    static auto get(const TsNameRegistry&) -> TsType { return {"Uint8Array"}; }
};

template<class T, std::size_t N>
struct TsTypeName<std::array<T, N>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {Detail::parenthesizeCompound(Detail::tsNameOf<T>(reg).mText) + "[]"};
    }
};

namespace Detail {

template<class K, class V>
[[nodiscard]] auto tsMapName(const TsNameRegistry& reg) -> TsType {
    if constexpr (std::same_as<K, std::string>) {
        return {"Record<string, " + tsNameOf<V>(reg).mText + ">"};
    } else {
        return {"Map<" + tsNameOf<K>(reg).mText + ", " + tsNameOf<V>(reg).mText + ">"};
    }
}

}  // namespace Detail

template<class K, class V, class C, class A>
struct TsTypeName<std::map<K, V, C, A>> {
    static auto get(const TsNameRegistry& reg) -> TsType { return Detail::tsMapName<K, V>(reg); }
};
template<class K, class V, class H, class E, class A>
struct TsTypeName<std::unordered_map<K, V, H, E, A>> {
    static auto get(const TsNameRegistry& reg) -> TsType { return Detail::tsMapName<K, V>(reg); }
};

template<class T, class C, class A>
struct TsTypeName<std::set<T, C, A>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {"Set<" + Detail::tsNameOf<T>(reg).mText + ">"};
    }
};
template<class T, class H, class E, class A>
struct TsTypeName<std::unordered_set<T, H, E, A>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {"Set<" + Detail::tsNameOf<T>(reg).mText + ">"};
    }
};

template<class T>
struct TsTypeName<std::optional<T>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {Detail::tsNameOf<T>(reg).mText, true};
    }
};

namespace Detail {

template<class... Ts>
[[nodiscard]] auto tsJoin(const TsNameRegistry& reg, std::string_view separator) -> std::string {
    std::string out;
    ((out += (out.empty() ? "" : separator), out += tsNameOf<Ts>(reg).mText), ...);
    return out;
}

}  // namespace Detail

template<class A, class B>
struct TsTypeName<std::pair<A, B>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {"[" + Detail::tsJoin<A, B>(reg, ", ") + "]"};
    }
};

template<class... Ts>
struct TsTypeName<std::tuple<Ts...>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {"[" + Detail::tsJoin<Ts...>(reg, ", ") + "]"};
    }
};

template<class... Ts>
struct TsTypeName<std::variant<Ts...>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return {Detail::tsJoin<Ts...>(reg, " | ")};
    }
};

namespace Detail {

template<class R, class... Args>
[[nodiscard]] auto tsCallableName(const TsNameRegistry& reg) -> TsType {
    std::string out = "(";
    std::size_t index = 0;
    ((out += index > 0 ? ", " : "", out += "arg" + std::to_string(index++), out += ": ",
      out += tsNameOf<Args>(reg).mText),
     ...);
    out += ") => ";
    out += tsNameOf<R>(reg).mText;
    return {std::move(out)};
}

}  // namespace Detail

template<class R, class... Args>
struct TsTypeName<std::function<R(Args...)>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return Detail::tsCallableName<R, Args...>(reg);
    }
};

template<class R, class... Args>
struct TsTypeName<Function<R(Args...)>> {
    static auto get(const TsNameRegistry& reg) -> TsType {
        return Detail::tsCallableName<R, Args...>(reg);
    }
};

template<>
struct TsTypeName<Value> {
    static auto get(const TsNameRegistry&) -> TsType { return {"unknown"}; }
};

template<>
struct TsTypeName<Object> {
    static auto get(const TsNameRegistry&) -> TsType { return {"object"}; }
};

}  // namespace Alka
