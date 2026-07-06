#pragma once

#include <Alka/Bind.hpp>
#include <Alka/NameOf.hpp>

#include <string_view>
#include <type_traits>
#include <typeinfo>

namespace Alka::Detail {

struct TypeNode {
    std::string_view mName;           // JS class name
    std::string_view mQualifiedName;  // C++ name, for diagnostics
    const TypeNode* mParent = nullptr;
    void* (*mUpcast)(void*) = nullptr;      // adjusts to mParent's type
    const std::type_info* mTypeId = nullptr;
};

template<BoundClass T>
inline const TypeNode kTypeNode = [] {
    TypeNode node;
    node.mName = boundNameOf<T>();
    node.mQualifiedName = qualifiedTypeNameOf<T>.view();
    node.mTypeId = &typeid(T);
    using E = ExtendsOf<T>;
    if constexpr (!std::is_void_v<E>) {
        static_assert(std::is_base_of_v<E, T>);
        node.mParent = &kTypeNode<E>;
        node.mUpcast = [](void* p) -> void* { return static_cast<E*>(static_cast<T*>(p)); };
    }
    return node;
}();

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

}  // namespace Alka::Detail
