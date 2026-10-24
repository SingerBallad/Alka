#pragma once

#include <Alka/Binding/Bind.hpp>
#include <Alka/Binding/Members.hpp>
#include <Alka/Binding/Meta.hpp>

#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Alka {

struct MemberView {
    MemberKind mKind{};
    std::string_view mName;
    std::string_view mDoc;
    bool mReadonly = false;
    bool mDeprecated = false;
    std::string_view mDeprecatedReason;
    std::size_t mOverloadCount = 1;
    std::vector<std::string_view> mArgNames;
    std::vector<std::pair<std::string_view, MetaValue>> mMeta;

    [[nodiscard]] auto meta(const std::string_view key) const noexcept -> const MetaValue* {
        for (const auto& [k, v] : mMeta) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    }
};

struct ClassView {
    std::string_view mName;
    std::string_view mDoc;
    std::string_view mExtends; // empty when the binding has no base
    std::vector<MemberView> mMembers;

    [[nodiscard]] auto member(const std::string_view name) const noexcept -> const MemberView* {
        for (const auto& m : mMembers) {
            if (m.mName == name) {
                return &m;
            }
        }
        return nullptr;
    }
};

template <Bound T> [[nodiscard]] auto classInfo() -> ClassView {
    ClassView view;
    view.mName = boundNameOf<T>();
    view.mDoc = boundDocOf<T>();
    if constexpr (!std::is_void_v<ExtendsOf<T>>) {
        view.mExtends = boundNameOf<ExtendsOf<T>>();
    }
    forEachMember<T>([&]<typename T0>(const T0& desc) {
        using Desc = std::remove_cvref_t<T0>;
        if constexpr (requires { desc.docText(); }) {
            MemberView member;
            member.mKind = Desc::kind;
            member.mName = Desc::name;
            member.mDoc = desc.docText();
            member.mReadonly = desc.readonlyFlag();
            member.mDeprecated = desc.deprecatedFlag();
            member.mDeprecatedReason = desc.deprecatedReason();
            if constexpr (requires { Desc::overloadCount; }) {
                member.mOverloadCount = Desc::overloadCount;
            }
            desc.forEachSpec([&]<typename T1>(const T1& s) {
                if constexpr (IsArgSpec<T1>) {
                    member.mArgNames.push_back(s.mName);
                } else if constexpr (IsMetaKv<T1>) {
                    member.mMeta.emplace_back(s.mKey, s.mValue);
                }
            });
            view.mMembers.push_back(std::move(member));
        }
    });
    return view;
}

} // namespace Alka
