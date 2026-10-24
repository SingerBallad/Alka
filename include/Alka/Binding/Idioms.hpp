#pragma once

#include <Alka/Binding/Members.hpp>
#include <Alka/Binding/Meta.hpp>
#include <Alka/Binding/Params.hpp>
#include <Alka/Binding/Policy.hpp>
#include <Alka/Convert/Converter.hpp>
#include <Alka/Detail/Invoke.hpp>
#include <Alka/Detail/State.hpp>
#include <Alka/Js/Value.hpp>

#include <v8.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// Make C++ classes in JS:
//
//   Alka::iterable<Path>()          for (const p of path) ... / [...path]
//   Alka::indexer()                 path[0], path[1] = p  (size() + operator[])
//   Alka::indexer<&Grid::cell, &Grid::setCell, &Grid::count>()   (any accessors)
//   Alka::toStringFromFormat<Vec>() String(v), `${v}`     (std::formatter<Vec>)
//   Alka::toJSON<Vec>()             JSON.stringify(v)     (from its props)
//   Alka::equals<Vec>()             a.equals(b)           (operator==)
//   Alka::disposable<&File::close>()  f.dispose(), `using f = ...` (release now)
//   Alka::namedProperties<&Config::find, &Config::put>()   cfg.anyName (dynamic)
//   Alka::asFunction<&Matcher::test>()                     matcher("text")
//
// Elements of bound-class type are COPIES by default; pass
// Alka::policy::internalRef for live views: `path[0]` then stays "element 0 of
// path" -- looked up again on every use, so it never dangles when the
// container reallocates or shrinks (a gone element throws instead).

namespace Alka {

namespace Detail {

template <class T>
concept IndexableContainer = requires(T& t, std::size_t i) {
    { t.size() } -> std::convertible_to<std::size_t>;
    t[i];
};

template <class T>
concept RangeContainer = requires(T& t) {
    std::begin(t);
    std::end(t);
};

template <class T> using IndexedElementOf = decltype(std::declval<T&>()[std::size_t{}]);

template <class T> using RangeElementOf = decltype(*std::begin(std::declval<T&>()));

template <class T> struct IterElement {
    using Type = RangeElementOf<T>;
};
template <IndexableContainer T> struct IterElement<T> {
    using Type = IndexedElementOf<T>;
};
template <class T> using IterElementOf = IterElement<T>::Type;

template <class Elem, ReturnPolicy Declared>
[[nodiscard]] consteval auto elementPolicy() -> ReturnPolicy {
    using E = std::remove_cvref_t<Elem>;
    if constexpr (Declared != ReturnPolicy::Automatic) {
        return Declared;
    } else if constexpr (BoundClass<E> && std::copy_constructible<E>) {
        return ReturnPolicy::Copy;
    } else if constexpr (BoundClass<E>) {
        static_assert(AlwaysFalse<E>,
                      "Alka: elements of this bound class cannot be copied; pass "
                      "Alka::policy::internalRef for live views (index-addressable containers)");
        return ReturnPolicy::InternalRef;
    } else {
        return ReturnPolicy::Automatic;
    }
}

template <class... Specs> [[nodiscard]] consteval auto policyFromSpecs() -> ReturnPolicy {
    auto out = ReturnPolicy::Automatic;
    ((IsPolicySpec<Specs> ? (void)(out = specPolicy<Specs>()) : (void)0), ...);
    return out;
}

inline auto iterTargetKey(v8::Isolate* isolate) {
    return cachedPrivate<"Alka::iterTarget">(isolate);
}
inline auto iterIndexKey(v8::Isolate* isolate) {
    return cachedPrivate<"Alka::iterIndex">(isolate);
}

inline const CallSite kIteratorSite{"iterator.next", {}, CallSite::Kind::Call};

struct DefaultIndexDesc {
    static constexpr auto getAccessor = nullptr;
    static constexpr auto setAccessor = nullptr;
    static constexpr auto sizeAccessor = nullptr;
};

template <class T, class Desc> struct IndexAccess;

template <class T, class Access, ReturnPolicy P>
[[nodiscard]] auto produceElement(ConvertCtx& cx, v8::Local<v8::Object> owner, T& container,
                                  std::size_t index) -> v8::Local<v8::Value>;

template <class T, ReturnPolicy P>
void iteratorNext(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const auto isolate = info.GetIsolate();
    enterCallback(isolate);
    const auto context = isolate->GetCurrentContext();
    const auto iterator = info.This();
    v8::Local<v8::Value> target;
    v8::Local<v8::Value> position;
    if (!iterator->GetPrivate(context, iterTargetKey(isolate)).ToLocal(&target) ||
        !iterator->GetPrivate(context, iterIndexKey(isolate)).ToLocal(&position) ||
        !target->IsObject() || !position->IsUint32()) {
        throwTypeError(isolate, "next() called on an incompatible receiver");
        return;
    }
    auto self = unwrapObject<T>(target);
    if (!self) {
        throwTypeError(isolate, "iterator.next: " + self.error().message());
        return;
    }
    const auto index = position.As<v8::Uint32>()->Value();
    auto& container = *self->mPtr;
    ConvertCtx cx{isolate, context};
    v8::Local<v8::Value> value = v8::Undefined(isolate);
    bool done = true;
    bool ok = false;
    guarded(isolate, kIteratorSite, [&] {
        if (index < static_cast<std::size_t>(container.size())) {
            using Elem = IndexedElementOf<T>;
            const auto element =
                produceElement<T, IndexAccess<T, DefaultIndexDesc>, elementPolicy<Elem, P>()>(
                    cx, target.As<v8::Object>(), container, index);
            if (!element.IsEmpty()) {
                value = element;
            }
            done = false;
        }
        ok = true;
    });
    if (!ok) {
        return;
    }
    if (!done) {
        (void)iterator->SetPrivate(context, iterIndexKey(isolate),
                                   v8::Integer::NewFromUnsigned(isolate, index + 1));
    }
    const auto result = v8::Object::New(isolate);
    std::ignore = result->CreateDataProperty(context, cachedKey<"value">(isolate), value);
    std::ignore = result->CreateDataProperty(context, cachedKey<"done">(isolate),
                                             v8::Boolean::New(isolate, done));
    info.GetReturnValue().Set(result);
}

} // namespace Detail

template <class E> class JsIterator : public Value {
public:
    JsIterator() = default;
    JsIterator(ConvertCtx& cx, const v8::Local<v8::Object> iterator) : Value(cx, iterator) {}
};

template <class E> struct Converter<JsIterator<E>> {
    static auto toJs(ConvertCtx& cx, const JsIterator<E>& value) -> v8::Local<v8::Value> {
        return Detail::handleToJs(cx, value, v8::Undefined(cx.mIsolate));
    }
};

namespace Detail {

template <class T, ReturnPolicy P>
auto makeIterator(T& self, CallInfo call) -> JsIterator<std::remove_cvref_t<IterElementOf<T>>> {
    using Result = JsIterator<std::remove_cvref_t<IterElementOf<T>>>;
    const auto isolate = call.mIsolate;
    const auto context = call.mContext;
    ConvertCtx cx{isolate, context};
    if constexpr (IndexableContainer<T>) {
        (void)self;
        const auto iterator = v8::Object::New(isolate);
        (void)iterator->SetPrivate(context, iterTargetKey(isolate), call.mThis);
        (void)iterator->SetPrivate(context, iterIndexKey(isolate),
                                   v8::Integer::NewFromUnsigned(isolate, 0));
        v8::Local<v8::Function> next;
        v8::Local<v8::Function> selfIterator;
        if (!v8::Function::New(context, &iteratorNext<T, P>, {}, 0, v8::ConstructorBehavior::kThrow)
                 .ToLocal(&next) ||
            !v8::Function::New(context, &returnThis, {}, 0, v8::ConstructorBehavior::kThrow)
                 .ToLocal(&selfIterator)) {
            return Result(); // an exception (termination) is pending
        }
        std::ignore = iterator->CreateDataProperty(context, cachedKey<"next">(isolate), next);
        std::ignore =
            iterator->CreateDataProperty(context, v8::Symbol::GetIterator(isolate), selfIterator);
        return Result(cx, iterator);
    } else {
        static_assert(RangeContainer<T>,
                      "Alka: iterable<T>() needs size() + operator[] or begin() / end()");
        static_assert(elementPolicy<IterElementOf<T>, P>() != ReturnPolicy::InternalRef,
                      "Alka: live element views need an index-addressable container (size() + "
                      "operator[]) to find their element again; this container's elements are "
                      "copied (drop policy::internalRef)");
        const auto array = v8::Array::New(isolate);
        std::uint32_t i = 0;
        for (auto&& element : self) {
            using Elem = decltype(element);
            auto value = produceResult<Elem, elementPolicy<Elem, P>()>(
                cx, call.mThis, [&]() -> decltype(auto) { return static_cast<Elem>(element); });
            if (value.IsEmpty()) {
                value = v8::Undefined(isolate);
            }
            if (array->CreateDataProperty(context, i++, value).IsNothing()) {
                return Result(); // execution is terminating
            }
        }
        v8::Local<v8::Value> iteratorFn;
        v8::Local<v8::Value> iterator;
        if (!array->Get(context, v8::Symbol::GetIterator(isolate)).ToLocal(&iteratorFn)) {
            return Result();
        }
        if (!iteratorFn->IsFunction()) {
            throwTypeError(isolate, "Array.prototype[Symbol.iterator] is not a function");
            return Result();
        }
        if (!iteratorFn.As<v8::Function>()->Call(context, array, 0, nullptr).ToLocal(&iterator)) {
            return Result();
        }
        if (!iterator->IsObject()) {
            throwTypeError(isolate, "the array iterator is not an object");
            return Result();
        }
        return Result(cx, iterator.As<v8::Object>());
    }
}

template <class T> auto formatOf(const T& self) -> std::string {
    return std::vformat("{}", std::make_format_args(self));
}

template <class T> bool equalsOf(const T& a, const T& b) {
    return a == b;
}

template <class T>
bool copyProps(const v8::Local<v8::Object> self, const v8::Local<v8::Object> out,
               v8::Isolate* isolate, const v8::Local<v8::Context> context) {
    if constexpr (!std::is_void_v<ExtendsOf<T>>) {
        if (!copyProps<ExtendsOf<T>>(self, out, isolate, context)) {
            return false;
        }
    }
    bool ok = true;
    forEachMember<T>([&]<class D>(const D&) {
        if constexpr (D::kind == MemberKind::Property) {
            if constexpr (!D::name.starts_with("@@")) { // symbol keys are not JSON
                if (!ok) {
                    return;
                }
                const auto key = nameKey<D>(isolate);
                v8::Local<v8::Value> value;
                if (!self->Get(context, key).ToLocal(&value)) {
                    ok = false;
                    return;
                }
                std::ignore = out->CreateDataProperty(context, key, value);
            }
        }
    });
    return ok;
}

template <class T> auto jsonOf(const T&, const CallInfo call) -> Object {
    const auto isolate = call.mIsolate;
    const auto context = call.mContext;
    ConvertCtx cx{isolate, context};
    const auto out = v8::Object::New(isolate);
    if (!copyProps<T>(call.mThis, out, isolate, context)) {
        return {}; // let it propagate
    }
    return {cx, out};
}

} // namespace Detail

template <class T, AnySpec... Specs> [[nodiscard]] constexpr auto iterable(Specs... specs) {
    return method<"@@iterator", &Detail::makeIterator<T, Detail::policyFromSpecs<Specs...>()>>(
        specs...);
}

template <auto Get = nullptr, auto Set = nullptr, auto Size = nullptr, class... Specs>
    requires(AnySpec<Specs> && ...)
[[nodiscard]] constexpr auto indexer(Specs... specs) {
    return IndexerDesc<std::tuple<Specs...>, Get, Set, Size>{{std::tuple<Specs...>(specs...)}};
}

template <class T, AnySpec... Specs>
[[nodiscard]] constexpr auto toStringFromFormat(Specs... specs) {
    return method<"toString", &Detail::formatOf<T>>(specs...);
}

template <class T, AnySpec... Specs> [[nodiscard]] constexpr auto toJSON(Specs... specs) {
    return method<"toJSON", &Detail::jsonOf<T>>(specs...);
}

template <class T, AnySpec... Specs> [[nodiscard]] constexpr auto equals(Specs... specs) {
    return method<"equals", &Detail::equalsOf<T>>(arg("other"), specs...);
}

// Get    std::optional<V>(const std::string&) or V*: empty / null = no such property
// Set    (const std::string&, V): writable (else dynamic names are read-only)
// Has    bool(const std::string&)  (default: Get's answer)
// Remove bool or void(const std::string&): `delete obj.name`
// Keys   a range of strings: Object.keys / for...in
// Alka::namedProperties<&Config::find, &Config::put, nullptr, &Config::erase, &Config::names>()
template <auto Get, auto Set = nullptr, auto Has = nullptr, auto Remove = nullptr,
          auto Keys = nullptr, AnySpec... Specs>
[[nodiscard]] constexpr auto namedProperties(Specs... specs) {
    return NamedPropertiesDesc<std::tuple<Specs...>, Get, Set, Has, Remove, Keys>{
        {std::tuple<Specs...>(specs...)}};
}

// Instances callable as functions: obj(args...) runs F on the object
// (a member function, or a callable taking the object first).
//   Alka::asFunction<&Matcher::test>()           matcher("text")
template <auto F, AnySpec... Specs> [[nodiscard]] constexpr auto asFunction(Specs... specs) {
    return CallAsFunctionDesc<std::tuple<Specs...>, F>{{std::tuple<Specs...>(specs...)}};
}

template <auto Before = nullptr, FixedString Name = "dispose", AnySpec... Specs>
[[nodiscard]] constexpr auto disposable(Specs... specs) {
    static_assert(Detail::isIdentifier(Name.view()),
                  "Alka: disposable<>'s name must be an identifier");
    return DisposerDesc<Name, std::tuple<Specs...>, Before>{{std::tuple<Specs...>(specs...)}};
}

namespace Detail {

template <class T>
concept AssignableElements = requires(
    T& t, std::size_t i, std::remove_cvref_t<IndexedElementOf<T>> v) { t[i] = std::move(v); };

template <class T, class Get> [[nodiscard]] consteval auto indexedElementProbe() {
    if constexpr (std::is_null_pointer_v<Get>) {
        return std::type_identity<IndexedElementOf<T>>{};
    } else {
        return std::type_identity<std::invoke_result_t<Get, T&, std::size_t>>{};
    }
}

template <class Element, class Set> [[nodiscard]] consteval auto indexStoredProbe() {
    if constexpr (std::is_null_pointer_v<Set>) {
        return std::type_identity<std::remove_cvref_t<Element>>{};
    } else {
        using Args = FnTraitsOfType<Set>::ArgsTuple;
        constexpr std::size_t valueAt = std::is_member_function_pointer_v<Set> ? 1 : 2;
        return std::type_identity<std::remove_cvref_t<std::tuple_element_t<valueAt, Args>>>{};
    }
}

template <class T, class Desc> struct IndexAccess {
    using GetFn = std::remove_cv_t<decltype(Desc::getAccessor)>;
    using SetFn = std::remove_cv_t<decltype(Desc::setAccessor)>;
    using SizeFn = std::remove_cv_t<decltype(Desc::sizeAccessor)>;
    static constexpr bool kCustomGet = !std::is_null_pointer_v<GetFn>;
    static constexpr bool kCustomSet = !std::is_null_pointer_v<SetFn>;
    static constexpr bool kCustomSize = !std::is_null_pointer_v<SizeFn>;
    static_assert(
        kCustomGet || requires(T& t, std::size_t i) { t[i]; },
        "Alka: indexer() needs operator[] -- or pass a getter: indexer<&T::at>()");
    static_assert(
        kCustomSize || requires(T& t) {
            { t.size() } -> std::convertible_to<std::size_t>;
        }, "Alka: indexer() needs size() -- or pass one: indexer<Get, Set, &T::count>()");

    using Element = decltype(indexedElementProbe<T, GetFn>())::type;
    using Stored = decltype(indexStoredProbe<Element, SetFn>())::type;
    static constexpr bool kWritable = kCustomSet || (!kCustomGet && AssignableElements<T>);

    [[nodiscard]] static auto size(T& self) -> std::size_t {
        if constexpr (kCustomSize) {
            return static_cast<std::size_t>(std::invoke(Desc::sizeAccessor, self));
        } else {
            return static_cast<std::size_t>(self.size());
        }
    }
    [[nodiscard]] static auto get(T& self, const std::size_t i) -> Element {
        if constexpr (kCustomGet) {
            return std::invoke(Desc::getAccessor, self, i);
        } else {
            return self[i];
        }
    }
    static void set(T& self, const std::size_t i, Stored value) {
        if constexpr (kCustomSet) {
            std::invoke(Desc::setAccessor, self, i, std::move(value));
        } else {
            self[i] = std::move(value);
        }
    }
};

template <class T> inline constexpr char kElementSource = 0;

template <class T, class Access>
auto resolveElement(const Holder& view, void* start, std::string& error) -> void* {
    try {
        auto& container = *static_cast<T*>(start);
        const auto size = Access::size(container);
        if (view.mPartIndex >= size) {
            error = "element " + std::to_string(view.mPartIndex) +
                    " is gone: the container holds " + std::to_string(size);
            return nullptr;
        }
        return pathPart(view, partOf(Access::get(container, view.mPartIndex)), error);
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "the element accessor failed";
    }
    return nullptr;
}

template <class T, class Access, ReturnPolicy P>
auto produceElement(ConvertCtx& cx, const v8::Local<v8::Object> owner, T& container,
                    const std::size_t index) -> v8::Local<v8::Value> {
    using Elem = Access::Element;
    if constexpr (P == ReturnPolicy::InternalRef &&
                  (isBoundClassRef<Elem> || isBoundClassPtr<Elem>)) {
        return wrapPath<T>(cx, owner, partOf(Access::get(container, index)),
                           &resolveElement<T, Access>, &kElementSource<T>, index, nullptr);
    } else {
        static_assert(P != ReturnPolicy::InternalRef || !BoundClass<std::remove_cvref_t<Elem>>,
                      "Alka: live element views need an accessor returning a reference or a "
                      "pointer; this one returns a copy");
        return produceResult<Elem, P>(
            cx, owner, [&]() -> decltype(auto) { return Access::get(container, index); });
    }
}

template <BoundClass T, std::size_t I>
auto indexGetter(const std::uint32_t index, const v8::PropertyCallbackInfo<v8::Value>& info)
    -> v8::Intercepted {
    using Desc = DescAt<T, I>;
    using Access = IndexAccess<T, Desc>;
    const auto isolate = info.GetIsolate();
    auto self = unwrapObject<T>(holderOf(info));
    if (!self) {
        return v8::Intercepted::kNo;
    }
    ConvertCtx cx{isolate, isolate->GetCurrentContext(), Desc::int64Mode()};
    using Elem = Access::Element;
    v8::Local<v8::Value> value;
    bool inRange = false;
    bool done = false;
    guarded(isolate, kSite<T, I, CallSite::Kind::Getter>, [&] {
        inRange = index < Access::size(*self->mPtr);
        if (inRange) {
            value = produceElement<T, Access, elementPolicy<Elem, Desc::returnPolicy()>()>(
                cx, holderOf(info), *self->mPtr, index);
        }
        done = true;
    });
    if (done && !inRange) {
        return v8::Intercepted::kNo;
    }
    if (!value.IsEmpty()) {
        info.GetReturnValue().Set(value);
    } else {
        info.GetReturnValue().SetUndefined();
    }
    return v8::Intercepted::kYes;
}

template <BoundClass T, std::size_t I>
auto indexSetter(const std::uint32_t index, const v8::Local<v8::Value> value,
                 const SetterInfo& info) -> v8::Intercepted {
    const auto isolate = info.GetIsolate();
    if (!unwrapObject<T>(holderOf(info))) {
        return v8::Intercepted::kNo;
    }
    using Access = IndexAccess<T, DescAt<T, I>>;
    using V = Access::Stored;
    ConvertCtx cx{isolate, isolate->GetCurrentContext()};
    const auto& site = kSite<T, I, CallSite::Kind::Setter>;
    std::optional<Result<V>> converted;
    try {
        converted.emplace(fromJs<V>(cx, value));
    } catch (...) {
        throwCppException(isolate, std::current_exception(), site.mDisplayName);
        return v8::Intercepted::kYes;
    }
    if (!*converted) {
        throwConversionError(isolate, converted->error(),
                             site.mDisplayName + ": " + converted->error().message());
        return v8::Intercepted::kYes;
    }
    auto self = unwrapObject<T>(holderOf(info));
    if (!self) {
        throwTypeError(isolate, site.mDisplayName + ": " + self.error().message());
        return v8::Intercepted::kYes;
    }
    guarded(isolate, site, [&] {
        if (index < Access::size(*self->mPtr)) {
            Access::set(*self->mPtr, index, std::move(**converted));
        }
    });
    return v8::Intercepted::kYes;
}

template <BoundClass T, std::size_t I>
auto indexQuery(const std::uint32_t index, const v8::PropertyCallbackInfo<v8::Integer>& info)
    -> v8::Intercepted {
    using Access = IndexAccess<T, DescAt<T, I>>;
    auto self = unwrapObject<T>(holderOf(info));
    if (!self) {
        return v8::Intercepted::kNo;
    }
    bool inRange = false;
    bool done = false;
    guarded(info.GetIsolate(), kSite<T, I, CallSite::Kind::Getter>, [&] {
        inRange = index < Access::size(*self->mPtr);
        done = true;
    });
    if (!done) {
        info.GetReturnValue().Set(v8::None);
        return v8::Intercepted::kYes;
    }
    if (!inRange) {
        return v8::Intercepted::kNo;
    }
    const auto attributes = Access::kWritable ? v8::None : v8::ReadOnly;
    info.GetReturnValue().Set(static_cast<std::int32_t>(attributes));
    return v8::Intercepted::kYes;
}

template <BoundClass T, std::size_t I>
void indexEnumerator(const v8::PropertyCallbackInfo<v8::Array>& info) {
    const auto isolate = info.GetIsolate();
    auto self = unwrapObject<T>(holderOf(info));
    if (!self) {
        return;
    }
    std::size_t count = 0;
    guarded(isolate, kSite<T, I, CallSite::Kind::Getter>,
            [&] { count = IndexAccess<T, DescAt<T, I>>::size(*self->mPtr); });
    const auto size = static_cast<std::uint32_t>(count);
    const auto context = isolate->GetCurrentContext();
    const auto indices = v8::Array::New(isolate, static_cast<int>(size));
    for (std::uint32_t i = 0; i < size; ++i) {
        std::ignore = indices->Set(context, i, v8::Integer::NewFromUnsigned(isolate, i));
    }
    info.GetReturnValue().Set(indices);
}

template <BoundClass T, std::size_t I>
void disposeCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    using Desc = DescAt<T, I>;
    const auto isolate = info.GetIsolate();
    enterCallback(isolate);
    const auto self = info.This();
    if (const Holder* holder = isWrapper(self) ? readHolder(self) : nullptr; holder != nullptr) {
        if (std::string why; holder->resolve(why) == nullptr) {
            return;
        }
    }
    const auto& site = kSite<T, I, CallSite::Kind::Call>;
    auto unwrapped = unwrapObject<T>(self);
    if (!unwrapped) {
        throwTypeError(isolate, site.mDisplayName + ": " + unwrapped.error().message());
        return;
    }
    guarded(isolate, site, [&] {
        if constexpr (!std::is_null_pointer_v<std::remove_cv_t<decltype(Desc::before)>>) {
            std::invoke(Desc::before, *unwrapped->mPtr); // (throws: nothing is released)
        }
        disposeFromJs(isolate, self);
    });
}

template <BoundClass T, std::size_t I>
void installIndexer(const v8::Local<v8::FunctionTemplate> ft) {
    decltype(&indexSetter<T, I>) setter = nullptr;
    if constexpr (IndexAccess<T, DescAt<T, I>>::kWritable) {
        setter = &indexSetter<T, I>;
    }
    ft->InstanceTemplate()->SetHandler(v8::IndexedPropertyHandlerConfiguration(
        &indexGetter<T, I>, setter, &indexQuery<T, I>, nullptr, &indexEnumerator<T, I>));
}

template <class T, class Desc> struct NamedAccess {
    using GetFn = std::remove_cv_t<decltype(Desc::getAccessor)>;
    using SetFn = std::remove_cv_t<decltype(Desc::setAccessor)>;
    using GetResult = std::invoke_result_t<GetFn, T&, const std::string&>;
    static constexpr bool kOptional = requires(GetResult r) {
        r.has_value();
        *r;
    } && !std::is_pointer_v<GetResult>;
    static_assert(kOptional || std::is_pointer_v<GetResult>,
                  "Alka: namedProperties<Get>: Get returns a std::optional<V> or a V* -- empty / "
                  "null: no such property");
    using Value = std::remove_cvref_t<decltype(*std::declval<GetResult&>())>;
    static constexpr bool kWritable = !std::is_null_pointer_v<SetFn>;
    static constexpr bool kHas =
        !std::is_null_pointer_v<std::remove_cv_t<decltype(Desc::hasAccessor)>>;
    static constexpr bool kRemove =
        !std::is_null_pointer_v<std::remove_cv_t<decltype(Desc::removeAccessor)>>;
    static constexpr bool kKeys =
        !std::is_null_pointer_v<std::remove_cv_t<decltype(Desc::keysAccessor)>>;

    [[nodiscard]] static auto get(T& self, const std::string& name) -> GetResult {
        return std::invoke(Desc::getAccessor, self, name);
    }
    [[nodiscard]] static bool has(T& self, const std::string& name) {
        if constexpr (kHas) {
            return static_cast<bool>(std::invoke(Desc::hasAccessor, self, name));
        } else {
            return static_cast<bool>(get(self, name));
        }
    }
};

template <class T, class Desc> [[nodiscard]] consteval auto namedStoredProbe() {
    using SetFn = std::remove_cv_t<decltype(Desc::setAccessor)>;
    using Args = typename FnTraitsOfType<SetFn>::ArgsTuple;
    return std::type_identity<
        std::remove_cvref_t<std::tuple_element_t<std::tuple_size_v<Args> - 1, Args>>>{};
}

inline auto namedKey(v8::Isolate* isolate, const v8::Local<v8::Name> name) -> std::string {
    return stringToUtf8(isolate, name.As<v8::String>());
}

template <BoundClass T, std::size_t I>
auto namedGetter(const v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Value>& info)
    -> v8::Intercepted {
    using Desc = DescAt<T, I>;
    using Access = NamedAccess<T, Desc>;
    const auto isolate = info.GetIsolate();
    auto self = unwrapObject<T>(holderOf(info));
    if (!name->IsString() || !self) {
        return v8::Intercepted::kNo;
    }
    ConvertCtx cx{isolate, isolate->GetCurrentContext(), Desc::int64Mode()};
    v8::Local<v8::Value> value;
    bool found = false;
    bool done = false;
    guarded(isolate, kSite<T, I, CallSite::Kind::Getter>, [&] {
        auto result = Access::get(*self->mPtr, namedKey(isolate, name));
        if (result) {
            found = true;
            if constexpr (Access::kOptional) {
                value = produceResult<typename Access::Value, Desc::returnPolicy()>(
                    cx, holderOf(info), [&]() -> decltype(auto) { return std::move(*result); });
            } else if constexpr (BoundClass<typename Access::Value>) {
                value = produceResult<decltype(result), Desc::returnPolicy()>(
                    cx, holderOf(info), [&]() { return result; });
            } else {
                value = produceResult<const typename Access::Value&, ReturnPolicy::Automatic>(
                    cx, holderOf(info), [&]() -> const Access::Value& { return *result; });
            }
        }
        done = true;
    });
    if (done && !found) {
        return v8::Intercepted::kNo;
    }
    if (!value.IsEmpty()) {
        info.GetReturnValue().Set(value);
    } else {
        info.GetReturnValue().SetUndefined();
    }
    return v8::Intercepted::kYes;
}

template <BoundClass T, std::size_t I>
auto namedSetter(const v8::Local<v8::Name> name, const v8::Local<v8::Value> value,
                 const SetterInfo& info) -> v8::Intercepted {
    using Desc = DescAt<T, I>;
    using Access = NamedAccess<T, Desc>;
    const auto isolate = info.GetIsolate();
    if (!name->IsString() || !unwrapObject<T>(holderOf(info))) {
        return v8::Intercepted::kNo;
    }
    const auto& site = kSite<T, I, CallSite::Kind::Setter>;
    const auto key = namedKey(isolate, name);
    if constexpr (!Access::kWritable) {
        bool exists = false;
        bool done = false;
        guarded(isolate, site, [&] {
            exists = Access::has(*unwrapObject<T>(holderOf(info))->mPtr, key);
            done = true;
        });
        if (done && !exists) {
            return v8::Intercepted::kNo;
        }
        if (done) {
            throwTypeError(isolate, site.mDisplayName + ": '" + key + "' is read-only");
        }
        return v8::Intercepted::kYes;
    } else {
        using V = decltype(namedStoredProbe<T, Desc>())::type;
        ConvertCtx cx{isolate, isolate->GetCurrentContext()};
        std::optional<Result<V>> converted;
        try {
            converted.emplace(fromJs<V>(cx, value));
        } catch (...) {
            throwCppException(isolate, std::current_exception(), site.mDisplayName);
            return v8::Intercepted::kYes;
        }
        if (!*converted) {
            throwConversionError(isolate, converted->error(),
                                 site.mDisplayName + ": " + converted->error().message());
            return v8::Intercepted::kYes;
        }
        auto self = unwrapObject<T>(holderOf(info));
        if (!self) {
            throwTypeError(isolate, site.mDisplayName + ": " + self.error().message());
            return v8::Intercepted::kYes;
        }
        guarded(isolate, site, [&] {
            (void)std::invoke(Desc::setAccessor, *self->mPtr, key, std::move(**converted));
        });
        return v8::Intercepted::kYes;
    }
}

template <BoundClass T, std::size_t I>
auto namedQuery(const v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Integer>& info)
    -> v8::Intercepted {
    using Access = NamedAccess<T, DescAt<T, I>>;
    auto self = unwrapObject<T>(holderOf(info));
    if (!name->IsString() || !self) {
        return v8::Intercepted::kNo;
    }
    bool exists = false;
    bool done = false;
    guarded(info.GetIsolate(), kSite<T, I, CallSite::Kind::Getter>, [&] {
        exists = Access::has(*self->mPtr, namedKey(info.GetIsolate(), name));
        done = true;
    });
    if (done && !exists) {
        return v8::Intercepted::kNo;
    }
    info.GetReturnValue().Set(
        static_cast<std::int32_t>(Access::kWritable ? v8::None : v8::ReadOnly));
    return v8::Intercepted::kYes;
}

template <BoundClass T, std::size_t I>
auto namedDeleter(const v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Boolean>& info)
    -> v8::Intercepted {
    using Desc = DescAt<T, I>;
    auto self = unwrapObject<T>(holderOf(info));
    if (!name->IsString() || !self) {
        return v8::Intercepted::kNo;
    }
    bool removed = false;
    guarded(info.GetIsolate(), kSite<T, I, CallSite::Kind::Setter>, [&] {
        const auto key = namedKey(info.GetIsolate(), name);
        if constexpr (std::is_void_v<std::invoke_result_t<decltype(Desc::removeAccessor), T&,
                                                          const std::string&>>) {
            std::invoke(Desc::removeAccessor, *self->mPtr, key);
            removed = true;
        } else {
            removed = static_cast<bool>(std::invoke(Desc::removeAccessor, *self->mPtr, key));
        }
    });
    info.GetReturnValue().Set(removed);
    return v8::Intercepted::kYes;
}

template <BoundClass T, std::size_t I>
void namedEnumerator(const v8::PropertyCallbackInfo<v8::Array>& info) {
    using Desc = DescAt<T, I>;
    const auto isolate = info.GetIsolate();
    auto self = unwrapObject<T>(holderOf(info));
    if (!self) {
        return;
    }
    guarded(isolate, kSite<T, I, CallSite::Kind::Getter>, [&] {
        const auto keys = std::invoke(Desc::keysAccessor, *self->mPtr);
        std::vector<v8::Local<v8::Value>> names;
        for (const auto& key : keys) {
            names.emplace_back(v8Str(isolate, std::string_view(key)));
        }
        info.GetReturnValue().Set(v8::Array::New(isolate, names.data(), names.size()));
    });
}

template <BoundClass T, std::size_t I>
void installNamedProperties(const v8::Local<v8::FunctionTemplate> ft) {
    using Desc = DescAt<T, I>;
    using Access = NamedAccess<T, Desc>;
    v8::NamedPropertyDeleterCallback deleter = nullptr;
    if constexpr (Access::kRemove) {
        deleter = &namedDeleter<T, I>;
    }
    v8::NamedPropertyEnumeratorCallback enumerator = nullptr;
    if constexpr (Access::kKeys) {
        enumerator = &namedEnumerator<T, I>;
    }
    ft->InstanceTemplate()->SetHandler(v8::NamedPropertyHandlerConfiguration(
        &namedGetter<T, I>, &namedSetter<T, I>, &namedQuery<T, I>, deleter, enumerator, nullptr,
        nullptr, v8::Local<v8::Value>(),
        static_cast<v8::PropertyHandlerFlags>(
            static_cast<int>(v8::PropertyHandlerFlags::kNonMasking) |
            static_cast<int>(v8::PropertyHandlerFlags::kOnlyInterceptStrings))));
}

template <BoundClass T>
[[nodiscard]] consteval auto firstMemberOf(const MemberKind kind) -> std::size_t {
    using Members = MembersTupleOf<T>;
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        std::size_t index = sizeof...(Is);
        ((std::tuple_element_t<Is, Members>::kind == kind && index == sizeof...(Is)
              ? (void)(index = Is)
              : (void)0),
         ...);
        return index;
    }(std::make_index_sequence<std::tuple_size_v<Members>>{});
}

template <BoundClass T, MemberKind Kind, class Install> void installNearest(Install&& install) {
    constexpr auto found = firstMemberOf<T>(Kind);
    if constexpr (found < memberCount<T>) {
        install.template operator()<T, found>();
    } else if constexpr (!std::is_void_v<ExtendsOf<T>>) {
        installNearest<ExtendsOf<T>, Kind>(std::forward<Install>(install));
    }
}

template <BoundClass T> void installIndexerOf(const v8::Local<v8::FunctionTemplate> ft) {
    installNearest<T, MemberKind::Indexer>(
        [&]<class C, std::size_t I>() { installIndexer<C, I>(ft); });
}

template <BoundClass T> void installInstanceHandlersOf(const v8::Local<v8::FunctionTemplate> ft) {
    installIndexerOf<T>(ft);
    installNearest<T, MemberKind::NamedProperties>(
        [&]<class C, std::size_t I>() { installNamedProperties<C, I>(ft); });
    installNearest<T, MemberKind::CallAsFunction>([&]<class C, std::size_t I>() {
        ft->InstanceTemplate()->SetCallAsFunctionHandler(&callAsFunctionCallback<C, I>);
    });
}

} // namespace Detail

} // namespace Alka
