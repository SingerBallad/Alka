#pragma once

#include <Alka/Binding/Bind.hpp>
#include <Alka/Core/NameOf.hpp>

#include <v8.h>

#include <string_view>
#include <type_traits>
#include <typeinfo>

namespace Alka::Detail {
struct GcWrapperSlot;
} // namespace Alka::Detail

namespace Alka {

template <class T>
inline constexpr bool isGcClass = std::is_base_of_v<Detail::GcWrapperSlot, std::remove_cv_t<T>>;

} // namespace Alka

namespace Alka::Detail {

struct TypeNode {
    std::string_view mName;          // JS class name
    std::string_view mQualifiedName; // C++ name, for diagnostics
    const TypeNode* mParent = nullptr;
    void* (*mUpcast)(void*) = nullptr; // adjusts to mParent's type
    const std::type_info* mTypeId = nullptr;
    void (*mAttachSelf)(void* object, v8::Isolate* isolate, v8::Local<v8::Context> context,
                        v8::Local<v8::Object> self) = nullptr;
    bool mGc = false;
};

template <BoundClass T>
inline const TypeNode kTypeNode = [] {
    TypeNode node;
    node.mName = boundNameOf<T>();
    node.mQualifiedName = qualifiedTypeNameOf<T>.view();
    node.mTypeId = &typeid(T);
    node.mGc = isGcClass<T>;
    static_assert(!isGcClass<T> || !HasTrampoline<T>,
                  "Alka: a C++-heap (GcObject) class cannot declare a Trampoline");
    using E = ExtendsOf<T>;
    if constexpr (!std::is_void_v<E>) {
        static_assert(std::is_base_of_v<E, T>);
        node.mParent = &kTypeNode<E>;
        node.mUpcast = [](void* p) -> void* { return static_cast<E*>(static_cast<T*>(p)); };
    }
    if constexpr (HasTrampoline<T>) {
        using J = typename Bind<T>::Trampoline;
        static_assert(std::is_base_of_v<T, J>, "Alka: a Trampoline must derive from its class");
        static_assert(std::has_virtual_destructor_v<T>,
                      "Alka: a class with a Trampoline needs a virtual destructor");
        node.mAttachSelf = [](void* object, v8::Isolate* isolate, v8::Local<v8::Context> context,
                              v8::Local<v8::Object> self) {
            if (auto* trampoline = dynamic_cast<J*>(static_cast<T*>(object))) {
                trampoline->alkaAttach(isolate, context, self, dynamic_cast<void*>(trampoline),
                                       &methodJsName<T>);
            }
        };
    }
    return node;
}();

[[nodiscard]] inline auto rootOf(const TypeNode* node) noexcept -> const TypeNode* {
    while (node != nullptr && node->mParent != nullptr) {
        node = node->mParent;
    }
    return node;
}

[[nodiscard]] inline void* upcastAlong(const TypeNode* from, const TypeNode* to, void* ptr) {
    for (auto node = from; node != nullptr; node = node->mParent) {
        if (node == to) {
            return ptr;
        }
        if (node->mUpcast != nullptr) {
            ptr = node->mUpcast(ptr);
        }
    }
    return nullptr;
}

} // namespace Alka::Detail
