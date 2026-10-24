#pragma once

#include <Alka/Core/FixedString.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>

// How the names Alka takes from C++ declarations become JS names:
//
//   template<> struct Alka::Bind<Player> : Alka::Class<Player> {
//       static constexpr auto naming = Alka::naming(Alka::stripPrefix<"m">, Alka::camelCase,
//                                                   Alka::rename<"mHTTPPort", "httpPort">);
//       static constexpr auto members = Alka::members(...);
//   };
//
// (in-class: alkaNaming). Without one, a class takes its bound base's, then the
// project's: ALKA_NAMING, Alka::verbatim unless set. Explicit names never pass
// through a naming. A class's naming names the class and its members, a
// module's its functions, enums and enumerators, a record's its fields.
//
//   stripPrefix<"m", "m_">   mHp -> hp, m_hp -> hp (a letter prefix only before a capital)
//   stripSuffix<"_">         hp_ -> hp
//   camelCase                max_speed, MaxSpeed -> maxSpeed (methods, properties, functions)
//   pascalCase               player_state -> PlayerState (types)
//   only<NameKind::X...>(s)  s, for exactly these kinds
//   a naming                 its steps
//   a constexpr callable     (NameKind, std::string_view) or (std::string_view) -> a
//                            NameText, string_view or literal (std::string: not with
//                            clang + libstdc++, whose std::string fails at compile time)
//   rename<"cxx", "js">      this exact C++ name gets this exact JS name, whatever the
//                            other steps say
//
// The result must be an identifier, and no two members of a binding may share a
// JS name: both are compile errors.

namespace Alka {

enum class NameKind : std::uint8_t {
    Type,
    Method,
    StaticMethod,
    Property,
    StaticProperty,
    Function,
    EnumValue,
};

namespace Detail {

inline void nameLongerThanNameTextCapacity() {}

} // namespace Detail

class NameText {
public:
    static constexpr std::size_t kCapacity = 256;

    constexpr NameText() = default;
    constexpr NameText(const std::string_view text) { append(text); }
    constexpr NameText(const char* text) : NameText(std::string_view(text)) {}

    constexpr NameText& append(const std::string_view text) {
        for (const char c : text) {
            push_back(c);
        }
        return *this;
    }

    constexpr void push_back(const char c) {
        if (mSize == kCapacity) {
            if consteval {
                Detail::nameLongerThanNameTextCapacity();
            }
            return;
        }
        mData[mSize++] = c;
    }

    constexpr NameText& operator+=(const std::string_view text) { return append(text); }
    constexpr NameText& operator+=(const char c) {
        push_back(c);
        return *this;
    }

    [[nodiscard]] constexpr auto size() const noexcept -> std::size_t { return mSize; }
    [[nodiscard]] constexpr bool empty() const noexcept { return mSize == 0; }
    [[nodiscard]] constexpr char operator[](const std::size_t i) const { return mData[i]; }
    [[nodiscard]] constexpr char& operator[](const std::size_t i) { return mData[i]; }
    [[nodiscard]] constexpr auto view() const noexcept -> std::string_view {
        return {mData, mSize};
    }
    constexpr operator std::string_view() const noexcept { return view(); }

    friend constexpr bool operator==(const NameText& a, const std::string_view b) {
        return a.view() == b;
    }

private:
    char mData[kCapacity]{};
    std::size_t mSize = 0;
};

template <class... Steps> struct Naming;

namespace Detail {

[[nodiscard]] constexpr bool isUpperAscii(const char c) {
    return c >= 'A' && c <= 'Z';
}
[[nodiscard]] constexpr bool isLowerAscii(const char c) {
    return c >= 'a' && c <= 'z';
}
[[nodiscard]] constexpr bool isDigitAscii(const char c) {
    return c >= '0' && c <= '9';
}
[[nodiscard]] constexpr bool isAlnumAscii(const char c) {
    return isUpperAscii(c) || isLowerAscii(c) || isDigitAscii(c);
}

[[nodiscard]] constexpr char lowerAscii(const char c) {
    return isUpperAscii(c) ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr char upperAscii(const char c) {
    return isLowerAscii(c) ? static_cast<char>(c - 'a' + 'A') : c;
}

[[nodiscard]] constexpr auto lowerFirstWord(const std::string_view name) -> NameText {
    NameText out(name);
    std::size_t run = 0;
    while (run < out.size() && (isUpperAscii(out[run]) || (run > 0 && isDigitAscii(out[run])))) {
        ++run;
    }
    std::size_t end = run;
    if (run > 1 && run < out.size() && isLowerAscii(out[run])) {
        const bool plural =
            out[run] == 's' && (run + 1 == out.size() || !isLowerAscii(out[run + 1]));
        if (!plural) {
            end = run - 1;
        }
    }
    for (std::size_t i = 0; i < end; ++i) {
        out[i] = lowerAscii(out[i]);
    }
    return out;
}

[[nodiscard]] constexpr auto upperFirst(const std::string_view word) -> NameText {
    NameText out(word);
    if (!out.empty()) {
        out[0] = upperAscii(out[0]);
    }
    return out;
}

[[nodiscard]] constexpr bool isScreamingWord(const std::string_view word) {
    bool letter = false;
    for (const char c : word) {
        if (isLowerAscii(c)) {
            return false;
        }
        letter = letter || isUpperAscii(c);
    }
    return letter;
}

[[nodiscard]] constexpr auto lowerAll(const std::string_view word) -> NameText {
    NameText out(word);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = lowerAscii(out[i]);
    }
    return out;
}

[[nodiscard]] constexpr auto recase(const std::string_view name, const bool upper) -> NameText {
    std::size_t begin = 0;
    while (begin < name.size() && (name[begin] == '_' || name[begin] == '$')) {
        ++begin;
    }
    std::size_t end = name.size();
    while (end > begin && name[end - 1] == '_') {
        --end;
    }
    if (begin == end) {
        return {name};
    }
    const auto core = name.substr(begin, end - begin);
    NameText out(name.substr(0, begin));
    if (core.find('_') == std::string_view::npos) {
        out += upper ? upperFirst(core) : lowerFirstWord(core);
    } else {
        bool first = true;
        std::size_t at = 0;
        while (at < core.size()) {
            std::size_t next = core.find('_', at);
            if (next == std::string_view::npos) {
                next = core.size();
            }
            if (next > at) {
                const auto word = core.substr(at, next - at);
                const NameText plain = isScreamingWord(word) ? lowerAll(word) : NameText(word);
                out += first && !upper ? lowerFirstWord(plain) : upperFirst(plain);
                first = false;
            }
            at = next + 1;
        }
    }
    out += name.substr(end);
    return out;
}

[[nodiscard]] constexpr bool stripPrefixOnce(NameText& out, const std::string_view name,
                                             const std::string_view prefix) {
    if (name.size() <= prefix.size() || !name.starts_with(prefix)) {
        return false;
    }
    const auto rest = name.substr(prefix.size());
    if (isAlnumAscii(prefix.back()) ? !isUpperAscii(rest.front()) : isDigitAscii(rest.front())) {
        return false;
    }
    out = isLowerAscii(name.front()) ? lowerFirstWord(rest) : NameText(rest);
    return true;
}

[[nodiscard]] constexpr bool stripSuffixOnce(NameText& out, const std::string_view name,
                                             const std::string_view suffix) {
    if (name.size() <= suffix.size() || !name.ends_with(suffix)) {
        return false;
    }
    out = NameText(name.substr(0, name.size() - suffix.size()));
    return true;
}

[[nodiscard]] constexpr bool isMemberOrFunctionKind(const NameKind kind) {
    return kind != NameKind::Type && kind != NameKind::EnumValue;
}

} // namespace Detail

template <FixedString... Prefixes> struct StripPrefix {
    static_assert(sizeof...(Prefixes) > 0 && ((Prefixes.size() > 0) && ...),
                  "Alka: stripPrefix<> needs non-empty prefixes");

    [[nodiscard]] constexpr auto operator()(NameKind, const std::string_view name) const
        -> NameText {
        NameText out(name);
        static_cast<void>((Detail::stripPrefixOnce(out, name, Prefixes.view()) || ...));
        return out;
    }
};

template <FixedString... Suffixes> struct StripSuffix {
    static_assert(sizeof...(Suffixes) > 0 && ((Suffixes.size() > 0) && ...),
                  "Alka: stripSuffix<> needs non-empty suffixes");

    [[nodiscard]] constexpr auto operator()(NameKind, const std::string_view name) const
        -> NameText {
        NameText out(name);
        static_cast<void>((Detail::stripSuffixOnce(out, name, Suffixes.view()) || ...));
        return out;
    }
};

struct CamelCase {
    [[nodiscard]] static constexpr bool appliesTo(const NameKind kind) {
        return Detail::isMemberOrFunctionKind(kind);
    }
    [[nodiscard]] constexpr auto operator()(NameKind, const std::string_view name) const
        -> NameText {
        return Detail::recase(name, false);
    }
};

struct PascalCase {
    [[nodiscard]] static constexpr bool appliesTo(const NameKind kind) {
        return kind == NameKind::Type;
    }
    [[nodiscard]] constexpr auto operator()(NameKind, const std::string_view name) const
        -> NameText {
        return Detail::recase(name, true);
    }
};

template <FixedString From, FixedString To> struct Rename {
    static_assert(!From.empty() && !To.empty(), "Alka: rename<> names must not be empty");
    static constexpr std::string_view cxxName = From.view();
    static constexpr std::string_view jsName = To.view();
};

template <class Step, NameKind... Kinds> struct Only {
    Step mStep;

    [[nodiscard]] static constexpr bool covers(const NameKind kind) {
        return ((kind == Kinds) || ...);
    }
};

template <FixedString... Prefixes> inline constexpr StripPrefix<Prefixes...> stripPrefix{};
template <FixedString... Suffixes> inline constexpr StripSuffix<Suffixes...> stripSuffix{};
inline constexpr CamelCase camelCase{};
inline constexpr PascalCase pascalCase{};
template <FixedString From, FixedString To> inline constexpr Rename<From, To> rename{};

namespace Detail {

template <class S> inline constexpr bool isRenameStep = false;
template <FixedString From, FixedString To>
inline constexpr bool isRenameStep<Rename<From, To>> = true;

template <class S> inline constexpr bool isOnlyStep = false;
template <class Step, NameKind... Kinds>
inline constexpr bool isOnlyStep<Only<Step, Kinds...>> = true;

template <class S> inline constexpr bool isNaming = false;
template <class... Steps> inline constexpr bool isNaming<Naming<Steps...>> = true;

template <class R>
concept NameResult = std::convertible_to<R, std::string_view>;

template <class S>
concept KindNameHandler = requires(const S& step, NameKind kind, std::string_view name) {
    { step(kind, name) } -> NameResult;
};

template <class S>
concept NameHandler = requires(const S& step, std::string_view name) {
    { step(name) } -> NameResult;
};

template <class S>
concept NamingStep =
    isRenameStep<S> || isOnlyStep<S> || isNaming<S> || KindNameHandler<S> || NameHandler<S>;

template <class S>
[[nodiscard]] constexpr bool findRename(const S& step, const NameKind kind,
                                        const std::string_view cxx, std::string_view& out) {
    if constexpr (isRenameStep<S>) {
        if (cxx != S::cxxName) {
            return false;
        }
        out = S::jsName;
        return true;
    } else if constexpr (isOnlyStep<S>) {
        return S::covers(kind) && findRename(step.mStep, kind, cxx, out);
    } else if constexpr (isNaming<S>) {
        return step.findRenameOf(kind, cxx, out);
    } else {
        return false;
    }
}

template <class S> constexpr void transformBy(const S& step, NameKind kind, NameText& name);

template <class S> constexpr void applyStep(const S& step, const NameKind kind, NameText& name) {
    if constexpr (isOnlyStep<S>) {
        if (S::covers(kind)) {
            transformBy(step.mStep, kind, name);
        }
    } else if constexpr (requires { S::appliesTo(NameKind::Type); }) {
        if (S::appliesTo(kind)) {
            transformBy(step, kind, name);
        }
    } else {
        transformBy(step, kind, name);
    }
}

template <class S> constexpr void transformBy(const S& step, const NameKind kind, NameText& name) {
    if constexpr (isRenameStep<S>) {
        // (renames apply first, see findRename)
    } else if constexpr (isOnlyStep<S>) {
        applyStep(step, kind, name);
    } else if constexpr (isNaming<S>) {
        step.transform(kind, name);
    } else if constexpr (KindNameHandler<S>) {
        name = NameText(std::string_view(step(kind, name.view())));
    } else {
        name = NameText(std::string_view(step(name.view())));
    }
}

} // namespace Detail

template <class... Steps> struct Naming {
    std::tuple<Steps...> mSteps;

    [[nodiscard]] constexpr auto apply(const NameKind kind, const std::string_view cxx) const
        -> NameText {
        if (std::string_view byRename; findRenameOf(kind, cxx, byRename)) {
            return {byRename};
        }
        NameText name(cxx);
        transform(kind, name);
        return name;
    }

    [[nodiscard]] constexpr bool findRenameOf(const NameKind kind, const std::string_view cxx,
                                              std::string_view& out) const {
        return std::apply(
            [&](const auto&... steps) {
                return (Detail::findRename(steps, kind, cxx, out) || ...);
            },
            mSteps);
    }

    constexpr void transform(const NameKind kind, NameText& name) const {
        std::apply([&](const auto&... steps) { (Detail::applyStep(steps, kind, name), ...); },
                   mSteps);
    }
};

template <class... Steps> [[nodiscard]] constexpr auto naming(const Steps... steps) {
    static_assert((Detail::NamingStep<Steps> && ...),
                  "Alka: naming() takes naming steps (stripPrefix<...>, camelCase, rename<...>, "
                  "only<...>(step), a naming) or constexpr callables taking (NameKind, "
                  "std::string_view) or (std::string_view) and returning a string");
    return Naming<Steps...>{std::tuple<Steps...>(steps...)};
}

template <NameKind... Kinds, class Step> [[nodiscard]] constexpr auto only(const Step step) {
    static_assert(sizeof...(Kinds) > 0, "Alka: only<>() needs a NameKind");
    static_assert(Detail::NamingStep<Step>, "Alka: only<>() takes a naming step");
    return Only<Step, Kinds...>{step};
}

inline constexpr auto verbatim = naming();

inline constexpr auto memberPrefixes =
    naming(only<NameKind::Property>(stripPrefix<"m_", "m">),
           only<NameKind::StaticProperty>(stripPrefix<"s_", "s">));

} // namespace Alka

#ifdef ALKA_NAMING_HEADER
#include ALKA_NAMING_HEADER
#endif
#ifndef ALKA_NAMING
#define ALKA_NAMING ::Alka::verbatim
#endif

namespace Alka::Detail {

inline constexpr auto kProjectNaming = ALKA_NAMING;
static_assert(isNaming<std::remove_cv_t<decltype(kProjectNaming)>>,
              "Alka: ALKA_NAMING must be an Alka::Naming (e.g. Alka::memberPrefixes)");

} // namespace Alka::Detail

#if defined(_MSC_VER)
#define ALKA_DETAIL_STRINGIZE_(x) #x
#define ALKA_DETAIL_STRINGIZE(x) ALKA_DETAIL_STRINGIZE_(x)
#pragma detect_mismatch("Alka ALKA_NAMING", ALKA_DETAIL_STRINGIZE(ALKA_NAMING))
#undef ALKA_DETAIL_STRINGIZE
#undef ALKA_DETAIL_STRINGIZE_
#endif
