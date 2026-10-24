#pragma once

#include <Alka/Binding/Bind.hpp>
#include <Alka/Binding/Members.hpp>
#include <Alka/Binding/Params.hpp>
#include <Alka/Binding/Policy.hpp>
#include <Alka/Convert/Converter.hpp>
#include <Alka/Convert/TypedArrays.hpp>
#include <Alka/Core/Error.hpp>
#include <Alka/Core/Traits.hpp>
#include <Alka/Detail/Holder.hpp>
#include <Alka/Detail/State.hpp>
#include <Alka/Detail/TypeNode.hpp>
#include <Alka/Runtime/Trackable.hpp>

#include <v8.h>

#if __has_include(<generator>)
#include <generator>
#endif
#if defined(__cpp_lib_generator)
#define ALKA_HAS_GENERATOR 1
#else
#define ALKA_HAS_GENERATOR 0
#endif

#include <array>
#include <cstddef>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace Alka::Detail {

struct CallSite {
    enum class Kind : std::uint8_t { Call, Getter, Setter, Construct };

    std::string mDisplayName;                // "math.add", "Player.attack", "new Player"
    std::vector<std::string_view> mArgNames; // JS parameter names from arg() specs
    Kind mKind = Kind::Call;

    [[nodiscard]] auto argName(const std::size_t i) const noexcept -> std::string_view {
        return i < mArgNames.size() ? mArgNames[i] : std::string_view{};
    }

    [[nodiscard]] auto argumentError(const ConvertError& error, const std::size_t jsIndex) const
        -> std::string {
        if (mKind == Kind::Setter) {
            return mDisplayName + ": " + error.message();
        }
        return ConvertError::forArgument(error, jsIndex, argName(jsIndex), mDisplayName).message();
    }

    void throwArgumentError(v8::Isolate* isolate, const ConvertError& error,
                            const std::size_t jsIndex) const {
        throwConversionError(isolate, error, argumentError(error, jsIndex));
    }
};

template <class Desc>
[[nodiscard]] auto argNamesOf(const Desc& desc) -> std::vector<std::string_view> {
    std::vector<std::string_view> names;
    desc.forEachSpec([&]<typename S>(const S& spec) {
        if constexpr (IsArgSpec<S>) {
            names.push_back(spec.mName);
        }
    });
    return names;
}

struct NoDesc {
    std::tuple<> mSpecs;
    template <class F> static constexpr void forEachSpec(F&&) {}
    [[nodiscard]] static consteval auto returnPolicy() -> ReturnPolicy {
        return ReturnPolicy::Automatic;
    }
    [[nodiscard]] static consteval auto int64Mode() -> Int64Mode { return kDefaultInt64Mode; }
    static constexpr std::size_t keepAliveCount = 0;
    static constexpr std::array<int, 0> keepAliveIndices{};
};
inline constexpr NoDesc kNoDesc{};

template <class T, class... Args> auto makeGcIn(v8::Isolate* isolate, Args&&... args) -> T*;
template <class T> auto wrapGc(ConvertCtx& cx, T* ptr) -> v8::Local<v8::Value>;
template <class T> void attachGcConstructed(ConvertCtx& cx, v8::Local<v8::Object> self, T* object);

template <class T> struct Unwrapped {
    T* mPtr = nullptr;
    Holder* mHolder = nullptr;
    std::shared_ptr<void> mKeep;
};

template <BoundClass T>
[[nodiscard]] auto unwrapWrapper(v8::Local<v8::Object> object) -> Result<Unwrapped<T>>;

template <BoundClass T>
[[nodiscard]] auto unwrapObject(const v8::Local<v8::Value> value) -> Result<Unwrapped<T>> {
    if (value.IsEmpty() || !value->IsObject()) {
        return std::unexpected(
            ConvertError::typeMismatch(kTypeNode<T>.mName, describeJsValue(value)));
    }
    const auto object = value.As<v8::Object>();
    if (!isWrapper(object)) {
        return std::unexpected(ConvertError::typeMismatch(kTypeNode<T>.mName, "a plain JS object"));
    }
    return unwrapWrapper<T>(object);
}

template <BoundClass T>
[[nodiscard]] auto unwrapWrapper(const v8::Local<v8::Object> object) -> Result<Unwrapped<T>> {
    const auto* node = readNode(object);
    if (node != nullptr && node->mGc) {
        void* const adjusted = upcastAlong(node, &kTypeNode<T>, readGcObject(object));
        if (adjusted == nullptr) {
            return std::unexpected(ConvertError::typeMismatch(kTypeNode<T>.mName, node->mName));
        }
        return Unwrapped<T>{static_cast<T*>(adjusted), nullptr, nullptr};
    }
    auto* holder = readHolder(object);
    if (node == nullptr || holder == nullptr) {
        return std::unexpected(ConvertError{
            std::string("the native ") + std::string(kTypeNode<T>.mName) +
            " object is missing (never constructed -- missing super()? -- or restored from a "
            "snapshot)"});
    }
    auto keep = holder->lockForCall();
    std::string why;
    void* const current =
        holder->mOwnership == Ownership::Weak && !keep ? nullptr : holder->resolve(why);
    if (current == nullptr) {
        return std::unexpected(ConvertError{
            std::string("the native ") + std::string(kTypeNode<T>.mName) +
            " object is not (or no longer) alive" + (why.empty() ? "" : " (" + why + ")")});
    }
    void* adjusted =
        upcastAlong(holder->isPath() ? holder->mPartNode : node, &kTypeNode<T>, current);
    if (adjusted == nullptr) {
        return std::unexpected(ConvertError::typeMismatch(kTypeNode<T>.mName, node->mName));
    }
    return Unwrapped<T>{static_cast<T*>(adjusted), holder, std::move(keep)};
}

// Native memory a JS-owned T accounts for, reported to V8 so the GC sees it:
//   template<> struct Alka::Bind<Mesh> : Alka::Class<Mesh> {
//       static std::size_t externalSize(const Mesh& m) { return m.bytes(); } ...
// or an in-class `std::size_t alkaExternalSize() const`. Default: none.
template <class T> [[nodiscard]] auto externalSizeOf(const T& object) -> std::int64_t {
    if constexpr (HasBindSpec<T> && requires { Bind<T>::externalSize(object); }) {
        return static_cast<std::int64_t>(Bind<T>::externalSize(object));
    } else if constexpr (requires { object.alkaExternalSize(); }) {
        return static_cast<std::int64_t>(object.alkaExternalSize());
    } else {
        return 0;
    }
}

struct NativeAddress {
    const TypeNode* mNode = nullptr;
    void* mPtr = nullptr;
    const void* mKey = nullptr;
};

template <BoundClass T> [[nodiscard]] auto locateNative(T* ptr) -> NativeAddress {
    if constexpr (std::is_polymorphic_v<T>) {
        const auto complete = dynamic_cast<void*>(ptr);
        if (const auto [mNode, mFromComplete] = lookupTypeNodeByTypeId(typeid(*ptr));
            mNode != nullptr) {
            return {.mNode = mNode, .mPtr = mFromComplete(complete), .mKey = complete};
        }
        // a dynamic type Alka does not know: reached through T, wherever T sits
        // inside it
        return {.mNode = &kTypeNode<T>, .mPtr = ptr, .mKey = complete};
    } else {
        return {.mNode = &kTypeNode<T>, .mPtr = ptr, .mKey = ptr};
    }
}

template <class Complete, class Node> [[nodiscard]] auto nodeFromComplete(void* complete) -> void* {
    return static_cast<Node*>(static_cast<Complete*>(complete));
}

template <BoundClass T> void registerKnownTypes() {
    registerTypeNodeIndex(typeid(T), &kTypeNode<T>, &nodeFromComplete<T, T>);
    if constexpr (HasTrampoline<T>) {
        using J = Bind<T>::Trampoline;
        registerTypeNodeIndex(typeid(J), &kTypeNode<T>, &nodeFromComplete<J, T>);
    }
}

template <class T>
    requires BoundClass<std::remove_cv_t<T>>
[[nodiscard]] auto wrapperOf(ConvertCtx& cx, T* ptr) -> v8::Local<v8::Object> {
    if (ptr == nullptr) {
        return {};
    }
    using U = std::remove_cv_t<T>;
    const auto at = locateNative<U>(const_cast<U*>(ptr));
    if (const auto local = findCachedWrapper(cx, at.mNode, at.mKey); !local.IsEmpty()) {
        return local;
    }
    if (const auto* holder = findOwningHolder(cx.mIsolate, at.mNode, at.mKey);
        holder != nullptr && holder->mEntry != nullptr && !holder->mEntry->mHandle.IsEmpty()) {
        return holder->mEntry->mHandle.Get(cx.mIsolate);
    }
    return {};
}

template <class T>
[[nodiscard]] auto wrapOwning(ConvertCtx& cx, std::shared_ptr<T> owner) -> v8::Local<v8::Value> {
    using U = std::remove_const_t<T>;
    static_assert(!isGcClass<U>, "Alka: a C++-heap (GcObject) class is owned by V8's heap, not "
                                 "by a shared_ptr / unique_ptr / a copy: return T* or T&");
    auto* ptr = const_cast<U*>(owner.get());
    const auto at = locateNative<U>(ptr);
    auto holder = Holder::owned(std::const_pointer_cast<U>(std::move(owner)), at.mPtr);
    holder->mExternalBytes = externalSizeOf<U>(*ptr);
    if (const v8::Local<v8::Object> cached = findCachedWrapper(cx, at.mNode, at.mKey, &holder);
        !cached.IsEmpty()) {
        return cached;
    }
    return wrapNative(cx, at.mNode, std::move(holder), at.mKey);
}

template <class T>
[[nodiscard]] auto wrapWeak(ConvertCtx& cx, const std::weak_ptr<T>& weak) -> v8::Local<v8::Value> {
    using U = std::remove_const_t<T>;
    const auto locked = weak.lock();
    if (!locked) {
        return v8::Null(cx.mIsolate);
    }
    auto* ptr = const_cast<U*>(locked.get());
    const auto at = locateNative<U>(ptr);
    auto holder = Holder::weak(std::const_pointer_cast<U>(locked), at.mPtr);
    if (const v8::Local<v8::Object> cached = findCachedWrapper(cx, at.mNode, at.mKey, &holder);
        !cached.IsEmpty()) {
        return cached;
    }
    return wrapNative(cx, at.mNode, std::move(holder), at.mKey);
}

template <class T> [[nodiscard]] auto wrapBorrowed(ConvertCtx& cx, T* ptr) -> v8::Local<v8::Value> {
    using U = std::remove_const_t<T>;
    if constexpr (isGcClass<U>) {
        return wrapGc(cx, ptr);
    }
    auto* mutablePtr = const_cast<U*>(ptr);
    if constexpr (requires { mutablePtr->weak_from_this(); }) {
        if (auto shared = mutablePtr->weak_from_this().lock()) {
            return wrapOwning(cx, std::shared_ptr<U>(std::move(shared), mutablePtr));
        }
    }
    const auto at = locateNative<U>(mutablePtr);
    if (const v8::Local<v8::Object> cached = findCachedWrapper(cx, at.mNode, at.mKey);
        !cached.IsEmpty()) {
        return cached;
    }
    auto holder = Holder::borrowed(at.mPtr);
    if constexpr (std::is_base_of_v<Trackable, U>) {
        holder->mTracked = mutablePtr->alkaLiveness();
    }
    return wrapNative(cx, at.mNode, std::move(holder), at.mKey);
}

template <class T> [[nodiscard]] consteval bool lendable() {
    return !ALKA_STRICT_BORROW || std::is_base_of_v<Trackable, T> || declaresLifetime<T>() ||
           isGcClass<T>;
}

template <bool Explicit, class T>
[[nodiscard]] auto lendToJs(ConvertCtx& cx, T* ptr) -> v8::Local<v8::Value> {
    using U = std::remove_const_t<T>;
    if constexpr (!Explicit && !lendable<U>()) {
        if constexpr (requires { const_cast<U*>(ptr)->weak_from_this(); }) {
            if (const_cast<U*>(ptr)->weak_from_this().expired()) {
                throw JsException(
                    "Alka (strict borrowing): this " + std::string(kTypeNode<U>.mName) +
                    " is not owned by a shared_ptr, so JS cannot hold it safely; declare "
                    "Bind<T>::lifetime, derive from Alka::Trackable, or state policy::borrow");
            }
        } else {
            static_assert(AlwaysFalse<U>,
                          "Alka (strict borrowing): this lends JS a T* / T& of a class Alka "
                          "cannot track -- if C++ destroys it, JS keeps a dangling wrapper. "
                          "Pick one: derive T from Alka::Trackable; declare `static constexpr "
                          "auto lifetime = Alka::Lifetime::Manual` (you call Alka::invalidate "
                          "before destroying) or `Lifetime::Static` (it outlives the isolate) "
                          "in Bind<T>; return an owning type (by value, unique_ptr, "
                          "shared_ptr); or state Alka::policy::borrow on this declaration "
                          "(Alka::borrow(ptr) for a value). ALKA_STRICT_BORROW=0 disables "
                          "this check.");
        }
    }
    return wrapBorrowed(cx, ptr);
}

template <class T>
[[nodiscard]] auto wrapView(ConvertCtx& cx, T* ptr, const v8::Local<v8::Object> owner)
    -> v8::Local<v8::Value> {
    using U = std::remove_const_t<T>;
    auto* mutablePtr = const_cast<U*>(ptr);
    const Holder* parent = !owner.IsEmpty() && isWrapper(owner) ? readHolder(owner) : nullptr;
    if (parent == nullptr) {
        const auto value = wrapBorrowed(cx, ptr);
        if (!owner.IsEmpty()) {
            retainOwner(cx, value, owner);
        }
        return value;
    }
    if (!parent->alive()) {
        return v8::Null(cx.mIsolate);
    }
    const auto at = locateNative<U>(mutablePtr);
    auto holder = Holder::borrowed(at.mPtr);
    holder->mParent = parent;
    v8::Local<v8::Value> value = findCachedWrapper(cx, at.mNode, at.mKey, &holder);
    if (value.IsEmpty()) {
        value = wrapNative(cx, at.mNode, std::move(holder), at.mKey);
    } else if (auto* cached = readHolder(value.As<v8::Object>());
               cached != nullptr && cached->borrowed() && cached->mParent == nullptr &&
               cached != parent) {
        cached->mParent = parent;
    }
    retainOwner(cx, value, owner);
    return value;
}

template <class T, auto Source> inline constexpr char kPathSource = 0;

template <class E>
[[nodiscard]] auto pathPart(const Holder& view, E* part, std::string& error) -> void* {
    if (part == nullptr) {
        error = "nothing is there now";
        return nullptr;
    }
    using U = std::remove_cv_t<E>;
    const auto at = locateNative<U>(const_cast<U*>(part));
    if (at.mNode != view.mPartNode) {
        error = "another type of object is there now";
        return nullptr;
    }
    return at.mPtr;
}

template <class R> [[nodiscard]] auto partOf(R&& result) {
    if constexpr (std::is_pointer_v<std::remove_cvref_t<R>>) {
        return result;
    } else {
        return std::addressof(result);
    }
}

template <class T, auto Field>
auto resolveMember(const Holder& view, void* start, std::string& error) -> void* {
    return pathPart(view, std::addressof(static_cast<T*>(start)->*Field), error);
}

template <class Start, class E>
[[nodiscard]] auto wrapPath(ConvertCtx& cx, const v8::Local<v8::Object> owner, E* part,
                            const PartResolver resolve, const void* source, const std::size_t index,
                            std::shared_ptr<void> args) -> v8::Local<v8::Value> {
    if (part == nullptr) {
        return v8::Null(cx.mIsolate);
    }
    Holder* const ownerHolder = isWrapper(owner) ? readHolder(owner) : nullptr;
    void* const gcOwner = ownerHolder == nullptr && isWrapper(owner) && isGcWrapper(owner)
                              ? readGcObject(owner)
                              : nullptr;
    const TypeNode* const ownerNode =
        ownerHolder != nullptr || gcOwner != nullptr ? readNode(owner) : nullptr;
    std::string why;
    void* const ownerNow = ownerHolder != nullptr ? ownerHolder->resolve(why) : gcOwner;
    if (ownerNow == nullptr || ownerNode == nullptr) {
        throw JsException("the native owner of this view is not (or no longer) alive");
    }
    void* const start = upcastAlong(
        ownerHolder != nullptr && ownerHolder->isPath() ? ownerHolder->mPartNode : ownerNode,
        &kTypeNode<Start>, ownerNow);
    if (start == nullptr) {
        throw JsException("this view's owner is not a " + std::string(kTypeNode<Start>.mName));
    }
    WrapperEntry* const ownerEntry =
        source != nullptr && ownerHolder != nullptr ? ownerHolder->mEntry : nullptr;
    const PathKey key{source, index};
    if (ownerEntry != nullptr) {
        if (const auto existing = findPath(cx.mIsolate, *ownerEntry, key); !existing.IsEmpty()) {
            return existing;
        }
    }
    using U = std::remove_cv_t<E>;
    const auto at = locateNative<U>(const_cast<U*>(part));
    auto holder = Holder::borrowed(at.mPtr);
    holder->mParent = ownerHolder;
    holder->mGcOwner = ownerHolder == nullptr ? gcOwner : nullptr;
    holder->mResolve = resolve;
    holder->mPartNode = at.mNode;
    holder->mStartDelta = static_cast<char*>(start) - static_cast<char*>(ownerNow);
    holder->mPartIndex = index;
    holder->mPartArgs = std::move(args);
    const auto view = wrapNative(cx, at.mNode, std::move(holder), nullptr);
    retainOwner(cx, view, owner);
    if (ownerEntry != nullptr) {
        rememberPath(cx.mIsolate, *ownerEntry, key, view);
    }
    return view;
}

template <class P>
inline constexpr bool isBoundClassRef =
    std::is_lvalue_reference_v<P> && BoundClass<std::remove_cvref_t<P>>;

template <class P>
inline constexpr bool isBoundClassPtr =
    std::is_pointer_v<std::remove_cvref_t<P>> &&
    BoundClass<std::remove_cv_t<std::remove_pointer_t<std::remove_cvref_t<P>>>>;

enum class ArgShape : std::uint8_t { Value, Rest, Injected };

template <class T, class Arg> struct DependentType {
    using Type = T;
};

template <class Arg> struct ArgTraits {
    using Stored = std::remove_cvref_t<Arg>;
    static constexpr auto shape = ArgShape::Value;

    static consteval void check() {
        static_assert(
            !(std::is_lvalue_reference_v<Arg> && !std::is_const_v<std::remove_reference_t<Arg>>),
            "Alka: non-const reference parameters are only supported for bound classes (JS "
            "values cannot alias primitives)");
        static_assert(FromJsConvertible<Stored>,
                      "Alka: no Converter<T>::fromJs for this parameter type; specialize "
                      "Alka::Converter or change the signature");
    }
    static auto convert(ConvertCtx& cx, v8::Local<v8::Value> value) -> Result<Stored> {
        return fromJs<Stored>(cx, value);
    }
    static auto forward(Stored& stored) -> Stored&& { return std::move(stored); }
};

template <class Arg>
    requires(isBoundClassRef<Arg>)
struct ArgTraits<Arg> {
    using T = std::remove_cvref_t<Arg>;
    using Stored = Unwrapped<T>;
    static constexpr auto shape = ArgShape::Value;

    static consteval void check() {}
    static auto convert(ConvertCtx&, v8::Local<v8::Value> value) -> Result<Stored> {
        return unwrapObject<T>(value);
    }
    static auto forward(Stored& stored) -> T& { return *stored.mPtr; }
};

template <class Arg>
    requires(isBoundClassPtr<Arg>)
struct ArgTraits<Arg> {
    using T = std::remove_cv_t<std::remove_pointer_t<std::remove_cvref_t<Arg>>>;
    using Stored = Unwrapped<T>;
    static constexpr auto shape = ArgShape::Value;

    static consteval void check() {}
    static auto convert(ConvertCtx&, v8::Local<v8::Value> value) -> Result<Stored> {
        if (value.IsEmpty() || value->IsNullOrUndefined()) {
            return Stored{};
        }
        return unwrapObject<T>(value);
    }
    static auto forward(Stored& stored) -> T* { return stored.mPtr; }
};

template <class Arg>
    requires std::same_as<std::remove_cvref_t<Arg>, std::string_view>
struct ArgTraits<Arg> {
    using Stored = DependentType<InlineUtf8, Arg>::Type;
    static constexpr auto shape = ArgShape::Value;

    static consteval void check() {}
    static auto convert(ConvertCtx& cx, v8::Local<v8::Value> value) -> Result<Stored> {
        if (!value->IsString()) {
            return std::unexpected(ConvertError::typeMismatch("string", describeJsValue(value)));
        }
        Stored out;
        visitString(
            cx.mIsolate, value.As<v8::String>(),
            [&]<class C>(const C* chars, const std::size_t length) { out.assign(chars, length); });
        return out;
    }
    static auto forward(const Stored& stored) -> std::string_view { return stored.view(); }
};

template <class Arg>
    requires std::same_as<std::remove_cvref_t<Arg>, const char*>
struct ArgTraits<Arg> {
    using Stored = DependentType<std::optional<std::string>, Arg>::Type;
    static constexpr auto shape = ArgShape::Value;

    static consteval void check() {}
    static auto convert(ConvertCtx& cx, v8::Local<v8::Value> value) -> Result<Stored> {
        return fromJs<Stored>(cx, value);
    }
    static auto forward(const Stored& stored) -> const char* {
        return stored ? stored->c_str() : nullptr;
    }
};

template <class X> inline constexpr bool isConstSpan = false;
template <class T> inline constexpr bool isConstSpan<std::span<const T>> = true;
template <class X> inline constexpr bool isMutableSpan = false;
template <class T>
    requires(!std::is_const_v<T>)
inline constexpr bool isMutableSpan<std::span<T>> = true;

template <class T>
inline constexpr bool isByteElement = std::same_as<std::remove_const_t<T>, std::byte> ||
                                      std::same_as<std::remove_const_t<T>, std::uint8_t>;

template <class T> struct SpanArg {
    using Elem = std::remove_const_t<T>;
    std::shared_ptr<v8::BackingStore> mStore;
    std::vector<Elem> mCopy;
    std::span<T> mView;

    SpanArg() = default;
    SpanArg(SpanArg&&) noexcept = default;
    SpanArg& operator=(SpanArg&&) noexcept = default;
    SpanArg(const SpanArg& other) : mStore(other.mStore), mCopy(other.mCopy) {
        mView = mStore ? other.mView : std::span<T>(mCopy);
    }
    SpanArg& operator=(const SpanArg& other) {
        if (this != &other) {
            *this = SpanArg(other);
        }
        return *this;
    }

    [[nodiscard]] static auto inPlace(const v8::Local<v8::Value> value) -> std::optional<SpanArg> {
        std::size_t offset = 0;
        std::size_t length = 0;
        std::shared_ptr<v8::BackingStore> store;
        if constexpr (isByteElement<T>) {
            store = byteMemory(value, offset, length);
        } else if constexpr (TypedElement<Elem>) {
            store = typedArrayMemory<Elem>(value, offset, length);
        }
        if (!store) {
            return std::nullopt;
        }
        SpanArg out;
        out.mView = length == 0 ? std::span<T>()
                                : std::span<T>(reinterpret_cast<T*>(
                                                   static_cast<std::byte*>(store->Data()) + offset),
                                               length);
        out.mStore = std::move(store);
        return out;
    }
};

template <class Arg>
    requires isConstSpan<std::remove_cvref_t<Arg>>
struct ArgTraits<Arg> {
    using Elem = std::remove_const_t<typename std::remove_cvref_t<Arg>::element_type>;
    using Stored = SpanArg<const Elem>;
    static constexpr auto shape = ArgShape::Value;

    static consteval void check() {
        static_assert(FromJsConvertible<std::vector<Elem>>,
                      "Alka: no Converter for this span's element type");
    }
    static auto convert(ConvertCtx& cx, v8::Local<v8::Value> value) -> Result<Stored> {
        if (auto inPlace = Stored::inPlace(value)) {
            return std::move(*inPlace);
        }
        auto copy = fromJs<std::vector<Elem>>(cx, value);
        if (!copy) {
            return std::unexpected(std::move(copy).error());
        }
        Stored out;
        out.mCopy = std::move(*copy);
        out.mView = out.mCopy;
        return out;
    }
    static auto forward(const Stored& stored) -> std::span<const Elem> { return stored.mView; }
};

template <class Arg>
    requires isMutableSpan<std::remove_cvref_t<Arg>>
struct ArgTraits<Arg> {
    using Elem = typename std::remove_cvref_t<Arg>::element_type;
    using Stored = SpanArg<Elem>;
    static constexpr auto shape = ArgShape::Value;

    static consteval void check() {
        static_assert(isByteElement<Elem> || TypedElement<Elem>,
                      "Alka: a writable std::span<T> parameter needs JS binary data of T: T must "
                      "be a number type with a typed array (or a byte)");
    }
    static auto convert(ConvertCtx&, v8::Local<v8::Value> value) -> Result<Stored> {
        if (auto inPlace = Stored::inPlace(value)) {
            return std::move(*inPlace);
        }
        if constexpr (isByteElement<Elem>) {
            return std::unexpected(ConvertError::typeMismatch(
                "ArrayBuffer or ArrayBufferView (not shared, not resizable)",
                describeJsValue(value)));
        } else {
            return std::unexpected(ConvertError::typeMismatch(
                std::string(typedArrayName(typedKindOf<Elem>())) + " (not shared, not resizable)",
                describeJsValue(value)));
        }
    }
    static auto forward(Stored& stored) -> std::span<Elem> { return stored.mView; }
};

template <class Arg>
    requires isRest<Arg>
struct ArgTraits<Arg> {
    using Stored = std::remove_cvref_t<Arg>;
    using Elem = Stored::value_type;
    static constexpr auto shape = ArgShape::Rest;

    static consteval void check() {
        static_assert(FromJsConvertible<Elem>,
                      "Alka: no Converter<T>::fromJs for this Rest<T> element type");
    }
    static auto convertRest(ConvertCtx& cx, const v8::FunctionCallbackInfo<v8::Value>& info,
                            const std::size_t from) -> Result<Stored> {
        Stored out;
        for (auto i = static_cast<int>(from); i < info.Length(); ++i) {
            auto converted = fromJs<Elem>(cx, info[i]);
            if (!converted) {
                return std::unexpected(ConvertError::forElement(
                    std::move(converted).error(), static_cast<std::size_t>(i) - from));
            }
            out.push_back(std::move(*converted));
        }
        return out;
    }
    static auto forward(Stored& stored) -> Stored&& { return std::move(stored); }
};

template <class Arg>
    requires isInjected<Arg>
struct ArgTraits<Arg> {
    using Bare = std::remove_cvref_t<Arg>;
    using Stored = std::conditional_t<std::is_same_v<Bare, ConvertCtx>, ConvertCtx*, Bare>;
    static constexpr auto shape = ArgShape::Injected;

    static consteval void check() {}
    static auto inject(ConvertCtx& cx, const v8::FunctionCallbackInfo<v8::Value>& info) -> Stored {
        if constexpr (std::is_same_v<Bare, CallInfo>) {
            return CallInfo{cx.mIsolate, cx.mContext, info.This(), info.NewTarget(), &info};
        } else if constexpr (std::is_same_v<Bare, ConvertCtx>) {
            return &cx;
        } else if constexpr (std::is_same_v<Bare, v8::Isolate*>) {
            return cx.mIsolate;
        } else {
            return cx.mContext;
        }
    }
    static auto forward(Stored& stored) -> decltype(auto) {
        if constexpr (std::is_same_v<Bare, ConvertCtx>) {
            return static_cast<ConvertCtx&>(*stored);
        } else {
            return static_cast<Stored&>(stored);
        }
    }
};

template <class A> inline constexpr auto shapeOf = ArgTraits<A>::shape;

template <class Args> [[nodiscard]] consteval auto jsParamCount() -> std::size_t {
    return []<std::size_t... Is>(std::index_sequence<Is...>) {
        return (std::size_t{shapeOf<std::tuple_element_t<Is, Args>> != ArgShape::Injected} + ... +
                std::size_t{0});
    }(std::make_index_sequence<std::tuple_size_v<Args>>{});
}

template <class Args, std::size_t I> [[nodiscard]] consteval auto jsIndexOf() -> std::size_t {
    return []<std::size_t... Js>(std::index_sequence<Js...>) {
        return (std::size_t{shapeOf<std::tuple_element_t<Js, Args>> != ArgShape::Injected} + ... +
                std::size_t{0});
    }(std::make_index_sequence<I>{});
}

template <class Args> struct JsArgsImpl;
template <class... A> struct JsArgsImpl<std::tuple<A...>> {
    using Type = decltype(std::tuple_cat(
        std::declval<std::conditional_t<isInjected<A>, std::tuple<>, std::tuple<A>>>()...));
};

template <class Args> using JsArgsOf = JsArgsImpl<Args>::Type;

template <class Args> [[nodiscard]] consteval bool hasRestParam() {
    using Js = JsArgsOf<Args>;
    constexpr auto n = std::tuple_size_v<Js>;
    if constexpr (n == 0) {
        return false;
    } else {
        return isRest<std::tuple_element_t<n - 1, Js>>;
    }
}

template <class Args> [[nodiscard]] consteval bool restOnlyLast() {
    using Js = JsArgsOf<Args>;
    return []<std::size_t... Is>(std::index_sequence<Is...>) {
        return ((!isRest<std::tuple_element_t<Is, Js>> || Is + 1 == std::tuple_size_v<Js>) && ... &&
                true);
    }(std::make_index_sequence<std::tuple_size_v<Js>>{});
}

template <class Desc>
using SpecTupleOf = std::remove_cvref_t<decltype(std::declval<const Desc&>().mSpecs)>;

template <class SpecTuple> struct ArgSpecIndex {
    static constexpr auto kData = []<std::size_t... Is>(std::index_sequence<Is...>) {
        std::array<std::size_t, std::tuple_size_v<SpecTuple> + 1> indices{};
        std::size_t count = 0;
        ((IsArgSpec<std::tuple_element_t<Is, SpecTuple>> ? (indices[count++] = Is, 0) : 0), ...);
        return std::pair{indices, count};
    }(std::make_index_sequence<std::tuple_size_v<SpecTuple>>{});

    static constexpr std::size_t count = kData.second;
    template <std::size_t J> static constexpr std::size_t tupleIndex = kData.first[J];
};

template <class Desc, std::size_t J> [[nodiscard]] consteval bool hasDefaultFor() {
    using Index = ArgSpecIndex<SpecTupleOf<Desc>>;
    if constexpr (J < Index::count) {
        return std::tuple_element_t<Index::template tupleIndex<J>, SpecTupleOf<Desc>>::hasDefault;
    } else {
        return false;
    }
}

template <class Desc, std::size_t J, class Stored>
[[nodiscard]] auto defaultArgValue(const Desc& desc) -> std::optional<Stored> {
    if constexpr (hasDefaultFor<Desc, J>()) {
        using Index = ArgSpecIndex<SpecTupleOf<Desc>>;
        constexpr auto at = Index::template tupleIndex<J>;
        static_assert(std::convertible_to<decltype(std::get<at>(desc.mSpecs).mDefault), Stored>,
                      "Alka: this arg() default value is not convertible to the parameter type");
        return static_cast<Stored>(std::get<at>(desc.mSpecs).mDefault);
    } else {
        return std::nullopt;
    }
}

template <class Desc, std::size_t Arity> [[nodiscard]] consteval auto minArityOf() -> std::size_t {
    std::size_t minArity = Arity;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        [[maybe_unused]] auto trailing = true; // (unused for no parameters)
        ((trailing && hasDefaultFor<Desc, Arity - 1 - Is>() ? --minArity
                                                            : (trailing = false, minArity)),
         ...);
    }(std::make_index_sequence<Arity>{});
    return minArity;
}

template <class Desc, class Args> [[nodiscard]] consteval auto minJsArity() -> std::size_t {
    return minArityOf<Desc, jsParamCount<Args>() - (hasRestParam<Args>() ? 1 : 0)>();
}

template <class Args> [[nodiscard]] consteval auto maxJsArity() -> std::size_t {
    return hasRestParam<Args>() ? (std::numeric_limits<std::size_t>::max)() : jsParamCount<Args>();
}

template <class Desc> [[nodiscard]] consteval bool anySpecDefault() {
    return []<std::size_t... Js>(std::index_sequence<Js...>) {
        return (hasDefaultFor<Desc, Js>() || ... || false);
    }(std::make_index_sequence<ArgSpecIndex<SpecTupleOf<Desc>>::count>{});
}

struct ArgFailure {
    ConvertError mError;
    std::size_t mJsIndex = 0;
};

template <class Args, class Seq = std::make_index_sequence<std::tuple_size_v<Args>>>
struct StoredTupleImpl;
template <class Args, std::size_t... Is> struct StoredTupleImpl<Args, std::index_sequence<Is...>> {
    using Type = std::tuple<typename ArgTraits<std::tuple_element_t<Is, Args>>::Stored...>;
};
template <class Args> using StoredTupleOf = typename StoredTupleImpl<Args>::Type;

template <class Args, class Desc>
[[nodiscard]] auto tryConvertArguments(const v8::FunctionCallbackInfo<v8::Value>& info,
                                       ConvertCtx& cx, const Desc& desc)
    -> std::expected<StoredTupleOf<Args>, ArgFailure> {
    static_assert(restOnlyLast<Args>(), "Alka: Rest<T> must be the last JS parameter");
    return [&]<std::size_t... Is>(
               std::index_sequence<Is...>) -> std::expected<StoredTupleOf<Args>, ArgFailure> {
        (ArgTraits<std::tuple_element_t<Is, Args>>::check(), ...);

        [[maybe_unused]] const auto convertOne = [&]<std::size_t I>() {
            using Traits = ArgTraits<std::tuple_element_t<I, Args>>;
            using Stored = Traits::Stored;
            constexpr std::size_t jsIndex = jsIndexOf<Args, I>();
            if constexpr (Traits::shape == ArgShape::Injected) {
                return Result<Stored>(Traits::inject(cx, info));
            } else if constexpr (Traits::shape == ArgShape::Rest) {
                return Traits::convertRest(cx, info, jsIndex);
            } else {
                const auto argc = static_cast<std::size_t>(info.Length());
                const bool missing =
                    jsIndex >= argc || info[static_cast<int>(jsIndex)]->IsUndefined();
                if (missing) {
                    if (auto fallback = defaultArgValue<Desc, jsIndex, Stored>(desc)) {
                        return Result<Stored>(std::move(*fallback));
                    }
                }
                const auto value = jsIndex < argc
                                       ? info[static_cast<int>(jsIndex)]
                                       : v8::Local<v8::Value>(v8::Undefined(cx.mIsolate));
                return Traits::convert(cx, value);
            }
        };

        std::tuple<Result<typename ArgTraits<std::tuple_element_t<Is, Args>>::Stored>...> converted{
            convertOne.template operator()<Is>()...};

        std::optional<ArgFailure> failure;
        (void)((std::get<Is>(converted).has_value()
                    ? false
                    : (failure.emplace(
                           ArgFailure{std::get<Is>(converted).error(), jsIndexOf<Args, Is>()}),
                       true)) ||
               ...);
        if (failure) {
            return std::unexpected(std::move(*failure));
        }
        return StoredTupleOf<Args>{std::move(*std::get<Is>(converted))...};
    }(std::make_index_sequence<std::tuple_size_v<Args>>{});
}

[[nodiscard]] inline auto describeArgs(const v8::FunctionCallbackInfo<v8::Value>& info)
    -> std::string {
    std::string out = "(";
    for (auto i = 0; i < info.Length(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += describeJsValue(info[i]);
    }
    out += ')';
    return out;
}

template <class Fn> void guarded(v8::Isolate* isolate, const CallSite& site, Fn&& fn) {
    const NativeCallScope nativeFrame;
    try {
        std::forward<Fn>(fn)();
    } catch (const JsException& e) {
        throwJsException(isolate, e.message(), e.jsException());
    } catch (...) {
        throwCppException(isolate, std::current_exception(), site.mDisplayName);
    }
}

template <class X> inline constexpr bool isExpected = false;
template <class V, class E> inline constexpr bool isExpected<std::expected<V, E>> = true;

template <class E> void throwExpectedError(ConvertCtx& cx, const E& error) {
    if constexpr (std::is_class_v<E> && !std::same_as<E, ConvertError>) {
        if (const auto value =
                registeredErrorValue(cx.mIsolate, cx.mContext, std::make_exception_ptr(error));
            !value.IsEmpty()) {
            cx.mIsolate->ThrowException(value);
            return;
        }
    }
    if constexpr (std::same_as<E, ConvertError>) {
        throwJsException(cx.mIsolate, error.message(), error.mJsException);
    } else if constexpr (std::convertible_to<const E&, std::string_view>) {
        throwJsException(cx.mIsolate, std::string_view(error), nullptr);
    } else if constexpr (std::derived_from<E, std::exception>) {
        throwJsException(cx.mIsolate, error.what(), nullptr);
    } else if constexpr (requires {
                             { error.message() } -> std::convertible_to<std::string>;
                         }) {
        throwJsException(cx.mIsolate, std::string(error.message()), nullptr); // error_code
    } else if constexpr (ToJsConvertible<E>) {
        if (!cx.mIsolate->IsExecutionTerminating()) {
            cx.mIsolate->ThrowException(toJs(cx, error)); // throw the converted value itself
        }
    } else {
        static_assert(AlwaysFalse<E>,
                      "Alka: cannot turn this std::expected error type into a JS exception; use "
                      "a string, std::exception, an error_code-like type or a JS-convertible type");
    }
}

template <class X> inline constexpr bool isUniquePtrOfBound = false;
template <class T> inline constexpr bool isUniquePtrOfBound<std::unique_ptr<T>> = BoundClass<T>;

template <class X>
inline constexpr bool isTask = requires { requires std::remove_cvref_t<X>::alkaIsTask; };

template <class X> inline constexpr bool isGenerator = false;
#if ALKA_HAS_GENERATOR
template <class R, class V, class A>
inline constexpr bool isGenerator<std::generator<R, V, A>> = true;
#endif

template <class X>
inline constexpr bool isCoroutineResult = isTask<X> || isGenerator<std::remove_cvref_t<X>>;

template <class G>
[[nodiscard]] auto generatorToJs(ConvertCtx& cx, G&& generator, v8::Local<v8::Object> self)
    -> v8::Local<v8::Value>;

template <class A>
inline constexpr bool isCoroutineSafeParam =
    !std::is_reference_v<A> && !std::is_pointer_v<A> && !isInjected<A> &&
    !std::is_same_v<std::remove_cvref_t<A>, std::string_view> &&
    !isConstSpan<std::remove_cvref_t<A>> && !isMutableSpan<std::remove_cvref_t<A>>;

template <class X> inline constexpr bool isSharedPtrOfBound = false;
template <class T>
inline constexpr bool isSharedPtrOfBound<std::shared_ptr<T>> = BoundClass<std::remove_const_t<T>>;

template <class Ret, ReturnPolicy P, class CallFn>
[[nodiscard]] auto produceResult(ConvertCtx& cx, const v8::Local<v8::Object> self, CallFn&& call)
    -> v8::Local<v8::Value> {
    using D = std::remove_cvref_t<Ret>;
    const auto keep = [&](const v8::Local<v8::Value> value) {
        if constexpr (P == ReturnPolicy::InternalRef) {
            if (!self.IsEmpty()) {
                retainOwner(cx, value, self);
            }
        }
        return value;
    };

    if constexpr (std::is_void_v<Ret>) {
        call();
        return {};
    } else if constexpr (isGenerator<D>) {
        return generatorToJs(cx, call(), self);
    } else if constexpr (isExpected<D>) {
        auto result = call();
        if (!result.has_value()) {
            throwExpectedError(cx, result.error());
            return {};
        } else if constexpr (!std::is_void_v<typename D::value_type>) {
            return produceResult<typename D::value_type, P>(
                cx, self, [&]() -> decltype(auto) { return std::move(*result); });
        } else {
            return {};
        }
    } else if constexpr (isTask<D>) {
        auto task = call();
        const auto promise = toJs(cx, task);
        if (!self.IsEmpty() && isWrapper(self) && promise->IsObject()) {
            retainOwner(cx, promise, self);
            if (const auto* holder = readHolder(self); holder != nullptr) {
                if (auto share = holder->shareOwnership()) {
                    trackExternalHolder(cx.mIsolate, promise.template As<v8::Object>(),
                                        Holder::owned(std::move(share), holder->mPtr));
                }
            }
        }
        return promise;
    } else if constexpr (isUniquePtrOfBound<D>) {
        auto owned = call();
        if (!owned) {
            return v8::Null(cx.mIsolate);
        }
        return keep(wrapOwning(cx, std::shared_ptr<typename D::element_type>(std::move(owned))));
    } else if constexpr (isSharedPtrOfBound<D>) {
        auto shared = call();
        if (!shared) {
            return v8::Null(cx.mIsolate);
        }
        return keep(wrapOwning(cx, std::move(shared)));
    } else if constexpr (isBoundClassPtr<Ret> &&
                         isGcClass<std::remove_cv_t<std::remove_pointer_t<D>>>) {
        static_assert(P != ReturnPolicy::Take && P != ReturnPolicy::Copy,
                      "Alka: a C++-heap (GcObject) object is neither taken nor copied");
        return wrapGc(cx, call());
    } else if constexpr (isBoundClassRef<Ret> && isGcClass<std::remove_cvref_t<Ret>>) {
        static_assert(P != ReturnPolicy::Copy && P != ReturnPolicy::Move,
                      "Alka: a C++-heap (GcObject) object is neither copied nor moved");
        return wrapGc(cx, std::addressof(call()));
    } else if constexpr (isBoundClassPtr<Ret>) {
        using T = std::remove_cv_t<std::remove_pointer_t<D>>;
        auto* ptr = call();
        if (ptr == nullptr) {
            return v8::Null(cx.mIsolate);
        } else if constexpr (P == ReturnPolicy::Take) {
            return keep(wrapOwning(cx, std::shared_ptr<T>(const_cast<T*>(ptr))));
        } else if constexpr (P == ReturnPolicy::Copy) {
            return keep(wrapOwning(cx, std::make_shared<T>(*ptr)));
        } else if constexpr (P == ReturnPolicy::InternalRef) {
            return wrapView(cx, ptr, self);
        } else {
            static_assert(P != ReturnPolicy::Move, "Alka: policy::move needs a reference result");
            return keep(lendToJs<P == ReturnPolicy::Borrow>(cx, ptr));
        }
    } else if constexpr (isBoundClassRef<Ret>) {
        using T = std::remove_cvref_t<Ret>;
        decltype(auto) ref = call();
        if constexpr (P == ReturnPolicy::Copy) {
            return keep(wrapOwning(cx, std::make_shared<T>(ref)));
        } else if constexpr (P == ReturnPolicy::Move) {
            static_assert(!std::is_const_v<std::remove_reference_t<Ret>>,
                          "Alka: policy::move cannot move from a const reference");
            return keep(wrapOwning(cx, std::make_shared<T>(std::move(ref))));
        } else if constexpr (P == ReturnPolicy::InternalRef) {
            return wrapView(cx, std::addressof(ref), self);
        } else {
            static_assert(P != ReturnPolicy::Take, "Alka: policy::take needs a pointer result");
            return keep(lendToJs<P == ReturnPolicy::Borrow>(cx, std::addressof(ref)));
        }
    } else if constexpr (BoundClass<D>) {
        static_assert(std::move_constructible<D> || std::copy_constructible<D>,
                      "Alka: returning a bound class by value requires it to be movable or "
                      "copyable");
        return keep(wrapOwning(cx, std::make_shared<D>(call())));
    } else {
        static_assert(ToJsConvertible<D>,
                      "Alka: no Converter<T>::toJs for this return type; specialize "
                      "Alka::Converter or change the signature");
        return toJs(cx, call());
    }
}

template <class D> [[nodiscard]] consteval bool directReturnCheck() {
    if constexpr (std::same_as<D, bool> || std::floating_point<D>) {
        return true;
    } else if constexpr (std::integral<D>) {
        return sizeof(D) <= 4 && !std::same_as<D, char> && !std::same_as<D, wchar_t> &&
               !std::same_as<D, char8_t> && !std::same_as<D, char16_t> &&
               !std::same_as<D, char32_t>;
    } else {
        return false;
    }
}
template <class D> inline constexpr bool isDirectReturn = directReturnCheck<D>();

template <class D>
    requires isDirectReturn<D>
void returnDirect(const v8::FunctionCallbackInfo<v8::Value>& info, const D value) {
    if constexpr (std::same_as<D, bool>) {
        info.GetReturnValue().Set(value);
    } else if constexpr (std::floating_point<D>) {
        info.GetReturnValue().Set(static_cast<double>(value));
    } else if constexpr (std::is_signed_v<D>) {
        info.GetReturnValue().Set(static_cast<std::int32_t>(value));
    } else {
        info.GetReturnValue().Set(static_cast<std::uint32_t>(value));
    }
}

template <class Ret, ReturnPolicy P, class CallFn>
void returnResult(const v8::FunctionCallbackInfo<v8::Value>& info, ConvertCtx& cx, CallFn&& call) {
    using D = std::remove_cvref_t<Ret>;
    if constexpr (isDirectReturn<D>) {
        returnDirect<D>(info, call());
    } else if (const auto value =
                   produceResult<Ret, P>(cx, info.This(), std::forward<CallFn>(call));
               !value.IsEmpty()) {
        info.GetReturnValue().Set(value);
    }
}

inline void returnThis(const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue().Set(info.This());
}

#if ALKA_HAS_GENERATOR

template <class G> struct GeneratorState {
    std::optional<G> mGenerator;
    std::optional<std::ranges::iterator_t<G>> mIt;
    bool mRunning = false;
    bool mDone = false;

    void finish() {
        mDone = true;
        mIt.reset();
        mGenerator.reset();
    }
};

inline const CallSite kGeneratorSite{"generator.next", {}, CallSite::Kind::Call};

template <class G>
[[nodiscard]] auto generatorStateOf(const v8::FunctionCallbackInfo<v8::Value>& info)
    -> GeneratorState<G>& {
    return *static_cast<GeneratorState<G>*>(
        static_cast<Holder*>(externalValue(info.Data().As<v8::External>(), kGeneratorTag))->mPtr);
}

inline void returnIteratorResult(const v8::FunctionCallbackInfo<v8::Value>& info,
                                 const v8::Local<v8::Value> value, const bool done) {
    const auto isolate = info.GetIsolate();
    const auto context = isolate->GetCurrentContext();
    const auto result = v8::Object::New(isolate);
    std::ignore = result->CreateDataProperty(context, cachedKey<"value">(isolate), value);
    std::ignore = result->CreateDataProperty(context, cachedKey<"done">(isolate),
                                             v8::Boolean::New(isolate, done));
    info.GetReturnValue().Set(result);
}

template <class G> void generatorNext(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const auto isolate = info.GetIsolate();
    enterCallback(isolate);
    auto& state = generatorStateOf<G>(info);
    if (state.mRunning) {
        throwTypeError(isolate, "Generator is already running");
        return;
    }
    ConvertCtx cx{isolate, isolate->GetCurrentContext()};
    v8::Local<v8::Value> value = v8::Undefined(isolate);
    if (!state.mDone) {
        state.mRunning = true;
        bool ok = false;
        guarded(isolate, kGeneratorSite, [&] {
            if (!state.mIt) {
                state.mIt.emplace(state.mGenerator->begin());
            } else {
                ++*state.mIt;
            }
            if (*state.mIt == std::default_sentinel) {
                state.mDone = true;
            } else {
                const auto element =
                    produceResult<std::ranges::range_reference_t<G>, ReturnPolicy::Automatic>(
                        cx, {}, [&]() -> decltype(auto) { return **state.mIt; });
                if (!element.IsEmpty()) {
                    value = element;
                }
            }
            ok = true;
        });
        state.mRunning = false;
        if (!ok || state.mDone) {
            state.finish();
            if (!ok) {
                return;
            }
        }
    }
    returnIteratorResult(info, value, state.mDone);
}

template <class G> void generatorReturn(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const auto isolate = info.GetIsolate();
    enterCallback(isolate);
    auto& state = generatorStateOf<G>(info);
    if (state.mRunning) {
        throwTypeError(isolate, "Generator is already running");
        return;
    }
    {
        const NativeCallScope nativeFrame;
        state.finish();
    }
    returnIteratorResult(info, info.Length() > 0 ? info[0] : v8::Undefined(isolate).As<v8::Value>(),
                         true);
}

template <class G>
auto generatorToJs(ConvertCtx& cx, G&& generator, const v8::Local<v8::Object> self)
    -> v8::Local<v8::Value> {
    using State = GeneratorState<std::remove_cvref_t<G>>;
    using D = std::remove_cvref_t<G>;
    auto state = std::make_shared<State>();
    state->mGenerator.emplace(std::forward<G>(generator));
    const auto isolate = cx.mIsolate;
    const auto iterator = v8::Object::New(isolate);
    auto holder = Holder::owned(state, state.get());
    const auto data = newExternal(isolate, holder.get(), kGeneratorTag);
    trackExternalHolder(isolate, iterator, std::move(holder)); // the iterator owns the frame
    v8::Local<v8::Function> next;
    v8::Local<v8::Function> finish;
    v8::Local<v8::Function> selfIterator;
    if (!v8::Function::New(cx.mContext, &generatorNext<D>, data, 0, v8::ConstructorBehavior::kThrow)
             .ToLocal(&next) ||
        !v8::Function::New(cx.mContext, &generatorReturn<D>, data, 1,
                           v8::ConstructorBehavior::kThrow)
             .ToLocal(&finish) ||
        !v8::Function::New(cx.mContext, &returnThis, {}, 0, v8::ConstructorBehavior::kThrow)
             .ToLocal(&selfIterator)) {
        return v8::Undefined(isolate); // an exception (termination) is pending
    }
    retainOwner(cx, next, iterator);
    retainOwner(cx, finish, iterator);
    std::ignore = iterator->CreateDataProperty(cx.mContext, cachedKey<"next">(isolate), next);
    std::ignore = iterator->CreateDataProperty(cx.mContext, cachedKey<"return">(isolate), finish);
    std::ignore =
        iterator->CreateDataProperty(cx.mContext, v8::Symbol::GetIterator(isolate), selfIterator);
    if (!self.IsEmpty() && isWrapper(self)) {
        retainOwner(cx, iterator, self);
        if (const auto* receiver = readHolder(self); receiver != nullptr) {
            if (auto share = receiver->shareOwnership()) {
                trackExternalHolder(isolate, iterator,
                                    Holder::owned(std::move(share), receiver->mPtr));
            }
        }
    }
    return iterator;
}

#endif // ALKA_HAS_GENERATOR

enum class Dispatch : std::uint8_t {
    Single,   // the only candidate: conversion failures throw a TypeError
    Overload, // one of several: a mismatch just declines
};

template <class Args, Dispatch D, class Desc, class Body>
bool invokeWith(const v8::FunctionCallbackInfo<v8::Value>& info, ConvertCtx& cx,
                const CallSite& site, const Desc& desc, Body&& body) {
    if constexpr (D == Dispatch::Overload) {
        const auto argc = static_cast<std::size_t>(info.Length());
        if (argc < minJsArity<Desc, Args>() || argc > maxJsArity<Args>()) {
            return false;
        }
    }
    std::optional<std::expected<StoredTupleOf<Args>, ArgFailure>> args;
    try {
        args.emplace(tryConvertArguments<Args>(info, cx, desc));
    } catch (...) {
        throwCppException(cx.mIsolate, std::current_exception(), site.mDisplayName);
        return true;
    }
    if (!*args) {
        if (const auto& failure = args->error(); D == Dispatch::Single ||
                                                 failure.mError.mJsException ||
                                                 cx.mIsolate->IsExecutionTerminating()) {
            site.throwArgumentError(cx.mIsolate, failure.mError, failure.mJsIndex);
            return true;
        }
        return false;
    }
    std::forward<Body>(body)(**args);
    return true;
}

template <auto F, class Args, class Stored, std::size_t... Is>
decltype(auto) callFree(Stored& stored, std::index_sequence<Is...>) {
    return F(ArgTraits<std::tuple_element_t<Is, Args>>::forward(std::get<Is>(stored))...);
}

template <class SelfParam, class T> [[nodiscard]] decltype(auto) selfAs(T* self) {
    if constexpr (std::is_pointer_v<std::remove_cvref_t<SelfParam>>) {
        return self;
    } else {
        return *self;
    }
}

template <auto F, class Args, class T, class Stored, std::size_t... Is>
decltype(auto) callOnSelf(T* self, Stored& stored, std::index_sequence<Is...>) {
    if constexpr (std::is_member_function_pointer_v<decltype(F)>) {
        return (self->*F)(
            ArgTraits<std::tuple_element_t<Is, Args>>::forward(std::get<Is>(stored))...);
    } else {
        using SelfParam = std::tuple_element_t<0, typename FnTraitsOf<F>::ArgsTuple>;
        return F(selfAs<SelfParam>(self),
                 ArgTraits<std::tuple_element_t<Is, Args>>::forward(std::get<Is>(stored))...);
    }
}

template <class Tuple, std::size_t... Is>
auto tailTupleProbe(std::index_sequence<Is...>)
    -> std::tuple<std::tuple_element_t<Is + 1, Tuple>...>;

template <class Tuple>
    requires(std::tuple_size_v<Tuple> >= 1)
using TailTuple =
    decltype(tailTupleProbe<Tuple>(std::make_index_sequence<std::tuple_size_v<Tuple> - 1>{}));

template <class FT, class T> [[nodiscard]] consteval auto methodArgsProbeType() {
    using Traits = FnTraitsOfType<FT>;
    if constexpr (std::is_member_function_pointer_v<FT>) {
        static_assert(std::is_base_of_v<typename Traits::Class, T>,
                      "Alka: this member function belongs to an unrelated class");
        return std::type_identity<typename Traits::ArgsTuple>{};
    } else {
        using FullArgs = Traits::ArgsTuple;
        static_assert(Traits::arity >= 1,
                      "Alka: a callable bound as a method must take the self object first");
        using SelfParam = std::tuple_element_t<0, FullArgs>;
        using SelfClass = std::remove_cv_t<std::remove_pointer_t<std::remove_cvref_t<SelfParam>>>;
        static_assert(std::is_base_of_v<SelfClass, T> || std::is_same_v<SelfClass, T>,
                      "Alka: the callable's first parameter must be the bound class (or a base)");
        return std::type_identity<TailTuple<FullArgs>>{};
    }
}

template <class FT, class T> using MethodArgsOfType = decltype(methodArgsProbeType<FT, T>())::type;
template <auto F, class T> using MethodArgsOf = MethodArgsOfType<decltype(F), T>;

template <class FT, class T> [[nodiscard]] consteval auto callArgsProbeType() {
    if constexpr (std::is_void_v<T>) {
        return std::type_identity<typename FnTraitsOfType<FT>::ArgsTuple>{};
    } else {
        return std::type_identity<MethodArgsOfType<FT, T>>{};
    }
}
template <class FT, class T> using CallArgsOfType = decltype(callArgsProbeType<FT, T>())::type;

template <class A>
inline constexpr bool isReplayableParam =
    !isInjected<A> && !isBoundClassRef<A> && !isBoundClassPtr<A> && !requires {
        std::remove_cvref_t<A>::alkaIsJsHandle;
    } && std::copy_constructible<typename ArgTraits<A>::Stored>;

template <class T, auto F, class Args>
auto resolveAccessor(const Holder& view, void* start, std::string& error) -> void* {
    try {
        StoredTupleOf<Args> args;
        if constexpr (std::tuple_size_v<Args> > 0) {
            args = *static_cast<const StoredTupleOf<Args>*>(view.mPartArgs.get());
        }
        return pathPart(
            view,
            partOf(callOnSelf<F, Args>(static_cast<T*>(start), args,
                                       std::make_index_sequence<std::tuple_size_v<Args>>{})),
            error);
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "its accessor failed";
    }
    return nullptr;
}

template <class T, auto F, class Args, class Stored>
void returnAccessorPath(const v8::FunctionCallbackInfo<v8::Value>& info, ConvertCtx& cx, T* self,
                        Stored& stored) {
    []<std::size_t... Is>(std::index_sequence<Is...>) {
        static_assert((isReplayableParam<std::tuple_element_t<Is, Args>> && ...),
                      "Alka: an internalRef result is a live path -- its accessor runs again on "
                      "every use, so it must take its parameters by value (no references or "
                      "pointers to bound objects, injected call state or JS handles)");
    }(std::make_index_sequence<std::tuple_size_v<Args>>{});
    std::shared_ptr<void> args;
    if constexpr (std::tuple_size_v<Args> > 0) {
        args = std::make_shared<StoredTupleOf<Args>>(stored);
    }
    auto* const part = partOf(
        callOnSelf<F, Args>(self, stored, std::make_index_sequence<std::tuple_size_v<Args>>{}));
    const void* const source = std::tuple_size_v<Args> == 0 ? &kPathSource<T, F> : nullptr;
    info.GetReturnValue().Set(wrapPath<T>(cx, info.This(), part, &resolveAccessor<T, F, Args>,
                                          source, 0, std::move(args)));
}

template <class Desc>
void afterCall(const v8::FunctionCallbackInfo<v8::Value>& info, ConvertCtx& cx) {
    if constexpr (Desc::keepAliveCount > 0) {
        for (const int index : Desc::keepAliveIndices) {
            if (index < info.Length()) {
                keepAliveValue(cx, info.This(), info[index]);
            }
        }
    }
}

template <class T, auto F, Dispatch D, ReturnPolicy P, class Desc>
bool invokeCallable(const v8::FunctionCallbackInfo<v8::Value>& info, ConvertCtx& cx,
                    const CallSite& site, const Desc& desc, T* self) {
    using Args = CallArgsOfType<decltype(F), T>;
    using Ret = FnTraitsOf<F>::Ret;
    if constexpr (isCoroutineResult<Ret>) {
        []<std::size_t... Is>(std::index_sequence<Is...>) {
            static_assert((isCoroutineSafeParam<std::tuple_element_t<Is, Args>> && ...),
                          "Alka: a bound coroutine (Alka::Task, std::generator) outlives its "
                          "call; take its parameters by value (no references, views, raw "
                          "pointers or injected call state)");
        }(std::make_index_sequence<std::tuple_size_v<Args>>{});
    }
    if constexpr (std::is_void_v<T>) {
        static_assert(P != ReturnPolicy::InternalRef && Desc::keepAliveCount == 0,
                      "Alka: internalRef / keepAlive need a `this` (use them on methods)");
    }
    (void)self;
    return invokeWith<Args, D>(info, cx, site, desc, [&](auto& stored) {
        [[maybe_unused]] T* target = nullptr;
        [[maybe_unused]] std::shared_ptr<void> keepAlive;
        if constexpr (!std::is_void_v<T>) {
            auto receiver = unwrapObject<T>(info.This());
            if (!receiver) {
                throwTypeError(cx.mIsolate, site.mDisplayName + ": " + receiver.error().message());
                return;
            }
            if constexpr (isCoroutineResult<Ret> && !isGcClass<T>) {
                if (!receiver->mHolder->shareOwnership()) {
                    throwTypeError(cx.mIsolate,
                                   site.mDisplayName +
                                       ": a coroutine method needs a receiver JS owns or shares "
                                       "(this one is borrowed from C++ and could be destroyed "
                                       "while the coroutine is suspended)");
                    return;
                }
            }
            target = receiver->mPtr;
            keepAlive = std::move(receiver->mKeep);
        }
        guarded(cx.mIsolate, site, [&] {
            if constexpr (!std::is_void_v<T> && P == ReturnPolicy::InternalRef &&
                          (isBoundClassRef<Ret> || isBoundClassPtr<Ret>)) {
                returnAccessorPath<T, F, Args>(info, cx, target, stored);
            } else {
                returnResult<Ret, P>(info, cx, [&]() -> decltype(auto) {
                    constexpr auto seq = std::make_index_sequence<std::tuple_size_v<Args>>{};
                    if constexpr (std::is_void_v<T>) {
                        return callFree<F, Args>(stored, seq);
                    } else {
                        return callOnSelf<F, Args>(target, stored, seq);
                    }
                });
            }
        });
        afterCall<Desc>(info, cx);
    });
}

template <class T, class Desc>
void dispatchCallables(const v8::FunctionCallbackInfo<v8::Value>& info, ConvertCtx& cx,
                       const CallSite& site, const Desc& desc, T* self) {
    constexpr auto P = Desc::returnPolicy();
    if constexpr (Desc::overloadCount == 1) {
        (void)invokeCallable<T, std::get<0>(Desc::callables), Dispatch::Single, P>(info, cx, site,
                                                                                   desc, self);
    } else {
        static_assert(!anySpecDefault<Desc>(),
                      "Alka: arg() defaults on an overload set are ambiguous; declare separate "
                      "single-callable bindings instead");
        const bool handled = [&]<std::size_t... Js>(std::index_sequence<Js...>) {
            return (invokeCallable<T, std::get<Js>(Desc::callables), Dispatch::Overload, P>(
                        info, cx, site, desc, self) ||
                    ...);
        }(std::make_index_sequence<Desc::overloadCount>{});
        if (!handled) {
            throwTypeError(cx.mIsolate, "no overload of " + site.mDisplayName +
                                            " matches the given arguments " + describeArgs(info));
        }
    }
}

template <Bound Owner, std::size_t I> using DescAt = std::tuple_element_t<I, MembersTupleOf<Owner>>;

template <Bound Owner, std::size_t I> [[nodiscard]] auto descAt() -> const DescAt<Owner, I>& {
    return std::get<I>(bindingMembers<Owner>());
}

template <Bound Owner> [[nodiscard]] auto scopeNameOf() -> std::string {
    if constexpr (BoundModule<Owner>) {
        return std::string(modulePathOf<Owner>());
    } else {
        return std::string(boundNameOf<Owner>());
    }
}

template <Bound Owner, std::size_t I, CallSite::Kind K>
inline const CallSite kSite = [] {
    CallSite site;
    site.mKind = K;
    const std::string scope = scopeNameOf<Owner>();
    if constexpr (K == CallSite::Kind::Construct) {
        site.mDisplayName = "new " + scope;
    } else {
        const std::string name(DescAt<Owner, I>::name);
        site.mDisplayName = scope.empty() ? name : scope + "." + name;
    }
    site.mArgNames = argNamesOf(descAt<Owner, I>());
    return site;
}();

template <class Desc>
[[nodiscard]] auto makeCtx(const v8::FunctionCallbackInfo<v8::Value>& info, const bool member)
    -> ConvertCtx {
    const auto isolate = info.GetIsolate();
    return ConvertCtx{isolate, isolate->GetCurrentContext(), Desc::int64Mode(),
                      member ? info.This() : v8::Local<v8::Object>()};
}

template <Bound Owner, std::size_t I>
void functionCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    using Desc = DescAt<Owner, I>;
    const auto isolate = info.GetIsolate();
    enterCallback(isolate);
    auto cx = makeCtx<Desc>(info, false);
    dispatchCallables<void>(info, cx, kSite<Owner, I, CallSite::Kind::Call>, descAt<Owner, I>(),
                            nullptr);
}

template <BoundClass T, bool checked = false>
[[nodiscard]] auto unwrapThis(const v8::FunctionCallbackInfo<v8::Value>& info, const CallSite& site)
    -> std::optional<Unwrapped<T>> {
    auto self = checked ? unwrapWrapper<T>(info.This()) : unwrapObject<T>(info.This());
    if (!self) {
        throwTypeError(info.GetIsolate(), site.mDisplayName + ": " + self.error().message());
        return std::nullopt;
    }
    return std::move(*self);
}

template <BoundClass T, std::size_t I>
void methodCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    using Desc = DescAt<T, I>;
    const auto isolate = info.GetIsolate();
    enterCallback(isolate);
    const auto& site = kSite<T, I, CallSite::Kind::Call>;
    const auto self = unwrapThis<T, true>(info, site);
    if (!self) {
        return;
    }
    auto cx = makeCtx<Desc>(info, true);
    if constexpr (std::is_polymorphic_v<T>) {
        const BaseCallScope baseCall(dynamic_cast<const void*>(self->mPtr), Desc::name);
        dispatchCallables<T>(info, cx, site, descAt<T, I>(), self->mPtr);
    } else {
        dispatchCallables<T>(info, cx, site, descAt<T, I>(), self->mPtr);
    }
}

template <class C, class M> auto memberTypeProbe(M C::*) -> M;
template <auto Field> using FieldTypeOf = decltype(memberTypeProbe(Field));

template <class P> struct MemberObjectPtrTraits;
template <class C, class M> struct MemberObjectPtrTraits<M C::*> {
    using Class = C;
    using Type = M;
};
template <class P> using FieldClassOfType = MemberObjectPtrTraits<P>::Class;
template <class P> using FieldTypeOfType = MemberObjectPtrTraits<P>::Type;

template <class Desc> [[nodiscard]] consteval auto getterPolicyOf() -> ReturnPolicy {
    constexpr auto declared = Desc::returnPolicy();
    if constexpr (Desc::isField && declared == ReturnPolicy::Automatic) {
        using FieldT = FieldTypeOfType<std::remove_cvref_t<decltype(std::get<0>(Desc::accessors))>>;
        return BoundClass<std::remove_cv_t<FieldT>> ? ReturnPolicy::InternalRef : declared;
    } else {
        return declared;
    }
}

template <BoundClass T, std::size_t I>
void getterCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    using Desc = DescAt<T, I>;
    const auto isolate = info.GetIsolate();
    enterCallback(isolate);
    const auto& site = kSite<T, I, CallSite::Kind::Getter>;
    const auto self = unwrapThis<T, true>(info, site);
    if (!self) {
        return;
    }
    constexpr auto A0 = std::get<0>(Desc::accessors);
    if constexpr (Desc::isField) {
        if constexpr (isDirectReturn<std::remove_cv_t<FieldTypeOf<A0>>>) {
            returnDirect<std::remove_cv_t<FieldTypeOf<A0>>>(info, self->mPtr->*A0);
            return;
        }
    }
    auto cx = makeCtx<Desc>(info, true);
    constexpr auto P = getterPolicyOf<Desc>();
    if constexpr (Desc::isField) {
        using FieldT = FieldTypeOf<A0>;
        guarded(isolate, site, [&] {
            if constexpr (P == ReturnPolicy::InternalRef && BoundClass<std::remove_cv_t<FieldT>>) {
                if (self->mHolder != nullptr && self->mHolder->isPath()) {
                    info.GetReturnValue().Set(
                        wrapPath<T>(cx, info.This(), std::addressof(self->mPtr->*A0),
                                    &resolveMember<T, A0>, &kPathSource<T, A0>, 0, nullptr));
                    return;
                }
            }
            returnResult<FieldT&, P>(info, cx, [&]() -> FieldT& { return self->mPtr->*A0; });
        });
    } else {
        (void)invokeCallable<T, A0, Dispatch::Single, P>(info, cx, site, descAt<T, I>(),
                                                         self->mPtr);
    }
}

template <BoundClass T, std::size_t I>
void staticGetterCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    using Desc = DescAt<T, I>;
    const auto isolate = info.GetIsolate();
    enterCallback(isolate);
    const auto& site = kSite<T, I, CallSite::Kind::Getter>;
    auto cx = makeCtx<Desc>(info, false);
    constexpr auto A0 = Desc::getAccessor;
    if constexpr (Desc::isVariable) {
        using V = std::remove_pointer_t<decltype(A0)>;
        constexpr auto P =
            BoundClass<std::remove_cv_t<V>> ? ReturnPolicy::Borrow : Desc::returnPolicy();
        guarded(isolate, site, [&] { returnResult<V&, P>(info, cx, [&]() -> V& { return *A0; }); });
    } else {
        (void)invokeCallable<void, A0, Dispatch::Single, Desc::returnPolicy()>(
            info, cx, site, descAt<T, I>(), nullptr);
    }
}

template <BoundClass T, std::size_t I>
void staticSetterCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    using Desc = DescAt<T, I>;
    const auto isolate = info.GetIsolate();
    enterCallback(isolate);
    const auto& site = kSite<T, I, CallSite::Kind::Setter>;
    auto cx = makeCtx<Desc>(info, false);
    if constexpr (Desc::isVariable) {
        constexpr auto A0 = Desc::getAccessor;
        using Value = std::remove_cv_t<std::remove_pointer_t<decltype(A0)>>;
        using Args = std::tuple<const Value&>;
        (void)invokeWith<Args, Dispatch::Single>(info, cx, site, kNoDesc, [&](auto& stored) {
            guarded(isolate, site,
                    [&] { *A0 = ArgTraits<const Value&>::forward(std::get<0>(stored)); });
        });
    } else {
        constexpr auto A1 = Desc::setAccessor;
        (void)invokeCallable<void, A1, Dispatch::Single, ReturnPolicy::Automatic>(
            info, cx, site, descAt<T, I>(), nullptr);
    }
}

template <BoundClass T, std::size_t I>
void callAsFunctionCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    using Desc = DescAt<T, I>;
    const auto isolate = info.GetIsolate();
    enterCallback(isolate);
    const auto& site = kSite<T, I, CallSite::Kind::Call>;
    const auto self = unwrapThis<T>(info, site);
    if (!self) {
        return;
    }
    auto cx = makeCtx<Desc>(info, true);
    dispatchCallables<T>(info, cx, site, descAt<T, I>(), self->mPtr);
}

template <BoundClass T, std::size_t I>
void setterCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    using Desc = DescAt<T, I>;
    const auto isolate = info.GetIsolate();
    enterCallback(isolate);
    const auto& site = kSite<T, I, CallSite::Kind::Setter>;
    const auto self = unwrapThis<T, true>(info, site);
    if (!self) {
        return;
    }
    auto cx = makeCtx<Desc>(info, true);
    if constexpr (Desc::isField) {
        constexpr auto A0 = std::get<0>(Desc::accessors);
        using Value = std::remove_cv_t<FieldTypeOf<A0>>;
        using Args = std::tuple<const Value&>;
        (void)invokeWith<Args, Dispatch::Single>(info, cx, site, kNoDesc, [&](auto& stored) {
            auto receiver = unwrapWrapper<T>(info.This());
            if (!receiver) {
                throwTypeError(isolate, site.mDisplayName + ": " + receiver.error().message());
                return;
            }
            guarded(isolate, site, [&] {
                receiver->mPtr->*A0 = ArgTraits<const Value&>::forward(std::get<0>(stored));
            });
        });
    } else {
        constexpr auto A1 = std::get<1>(Desc::accessors);
        (void)invokeCallable<T, A1, Dispatch::Single, ReturnPolicy::Automatic>(
            info, cx, site, descAt<T, I>(), self->mPtr);
    }
}

} // namespace Alka::Detail
