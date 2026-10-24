#pragma once

#include <Alka/Binding/Bind.hpp>
#include <Alka/Binding/Idioms.hpp>
#include <Alka/Detail/FastApi.hpp>
#include <Alka/Detail/Invoke.hpp>
#include <Alka/Detail/State.hpp>
#include <Alka/Runtime/Install.hpp>

#include <v8-snapshot.h>
#include <v8.h>

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

// V8 startup snapshots of contexts with Alka bindings (self-hosted embedders;
// hosts like Electron own their snapshot).
//
//   // build time
//   v8::Isolate::CreateParams params;
//   params.external_references = Alka::snapshot::externalReferences<Game>();
//   v8::SnapshotCreator creator(params);
//   std::vector<std::size_t> templates;
//   {   ... enter isolate / context, Alka::install<Game>(context), run setup scripts ...
//       templates = Alka::snapshot::addTemplates<Game>(creator);
//       creator.SetDefaultContext(context, Alka::snapshot::internalFieldsSerializer());
//   }
//   Alka::snapshot::prepare(creator.GetIsolate());   // releases Alka's handles
//   auto blob = creator.CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
//
//   // start-up
//   params.snapshot_blob = &blob;                     // same external_references
//   auto* isolate = v8::Isolate::New(params);
//   Alka::snapshot::restoreTemplates<Game>(isolate, templates);
//   auto context = v8::Context::New(isolate, nullptr, {}, {},
//                                   Alka::snapshot::internalFieldsDeserializer());

namespace Alka::snapshot {

namespace Detail {

using namespace Alka::Detail;

class ClassWalk {
public:
    template <class Visit>
    explicit ClassWalk(Visit& visit) : mVisitClass(&visit), mThunk(&thunk<Visit>) {}

    template <BoundModule Mod> void module() {
        forEachMember<Mod>([&]<class D>(const D&) {
            if constexpr (D::kind == MemberKind::ClassRef) {
                if constexpr (BoundClass<typename D::Type>) {
                    cls<typename D::Type>();
                }
            } else if constexpr (D::kind == MemberKind::Submodule) {
                module<typename D::Module>();
            }
        });
    }

    template <BoundClass T> void cls() {
        if constexpr (!std::is_void_v<ExtendsOf<T>>) {
            cls<ExtendsOf<T>>();
        }
        if (mSeen.insert(&kTypeNode<T>).second) {
            mThunk(
                mVisitClass, &kTypeNode<T>,
                [](v8::Isolate* isolate) { return classTemplateOf<T>(isolate); },
                &registerRestored<T>);
        }
    }

    using TemplateOf = v8::Local<v8::FunctionTemplate> (*)(v8::Isolate*);
    using Restore = void (*)(v8::Isolate*, v8::Local<v8::FunctionTemplate>);

private:
    template <BoundClass T>
    static void registerRestored(v8::Isolate* isolate, const v8::Local<v8::FunctionTemplate> ft) {
        auto& data = perIsolateData(isolate);
        data.mClassTemplates.emplace(&kTypeNode<T>, v8::Global<v8::FunctionTemplate>(isolate, ft));
        registerKnownTypes<T>();
    }

    template <class Visit>
    static void thunk(void* visit, const TypeNode* node, TemplateOf templateOf, Restore restore) {
        (*static_cast<Visit*>(visit))(node, templateOf, restore);
    }

    void* mVisitClass;
    void (*mThunk)(void*, const TypeNode*, TemplateOf, Restore);
    std::unordered_set<const TypeNode*> mSeen;
};

class References {
public:
    template <class F> void add(F* fn) { mAll.push_back(reinterpret_cast<std::intptr_t>(fn)); }
    void add([[maybe_unused]] const v8::CFunction* fast) {
#if ALKA_FAST_API
        if (fast != nullptr) {
            mAll.push_back(reinterpret_cast<std::intptr_t>(fast->GetAddress()));
            mAll.push_back(reinterpret_cast<std::intptr_t>(fast->GetTypeInfo()));
        }
#endif
    }
    [[nodiscard]] auto finish() -> std::vector<std::intptr_t> {
        std::vector<std::intptr_t> out;
        std::unordered_set<std::intptr_t> seen;
        for (const auto ref : mAll) {
            if (seen.insert(ref).second) {
                out.push_back(ref);
            }
        }
        out.push_back(0);
        return out;
    }

    template <BoundModule Mod> void module() {
        forEachMemberIndexed<Mod>([&]<std::size_t I>(const auto&) {
            using D = DescAt<Mod, I>;
            if constexpr (D::kind == MemberKind::Function) {
                add(&functionCallback<Mod, I>);
                add(fastFunctionOf<void, Mod, I>());
            } else if constexpr (D::kind == MemberKind::ClassRef) {
                if constexpr (BoundClass<typename D::Type>) {
                    cls<typename D::Type>();
                }
            } else if constexpr (D::kind == MemberKind::Submodule) {
                module<typename D::Module>();
            }
        });
    }

    template <BoundClass T> void cls() {
        if (!mSeen.insert(&kTypeNode<T>).second) {
            return;
        }
        if constexpr (!std::is_void_v<ExtendsOf<T>>) {
            cls<ExtendsOf<T>>();
        }
        add(&ctorCallback<T>);
        forEachMemberIndexed<T>([&]<std::size_t I>(const auto&) {
            using D = DescAt<T, I>;
            if constexpr (D::kind == MemberKind::Method) {
                add(&methodCallback<T, I>);
                add(fastFunctionOf<T, T, I>());
            } else if constexpr (D::kind == MemberKind::StaticMethod) {
                add(&functionCallback<T, I>);
                add(fastFunctionOf<void, T, I>());
            } else if constexpr (D::kind == MemberKind::Property) {
                add(&getterCallback<T, I>);
                if constexpr (D::writable) {
                    add(&setterCallback<T, I>);
                }
            } else if constexpr (D::kind == MemberKind::Disposer) {
                add(&disposeCallback<T, I>);
            } else if constexpr (D::kind == MemberKind::StaticProperty) {
                add(&staticGetterCallback<T, I>);
                if constexpr (D::writable) {
                    add(&staticSetterCallback<T, I>);
                }
            } else if constexpr (D::kind == MemberKind::CallAsFunction) {
                add(&callAsFunctionCallback<T, I>);
            } else if constexpr (D::kind == MemberKind::NamedProperties) {
                add(&namedGetter<T, I>);
                add(&namedSetter<T, I>);
                add(&namedQuery<T, I>);
                if constexpr (NamedAccess<T, D>::kRemove) {
                    add(&namedDeleter<T, I>);
                }
                if constexpr (NamedAccess<T, D>::kKeys) {
                    add(&namedEnumerator<T, I>);
                }
            } else if constexpr (D::kind == MemberKind::Indexer) {
                add(&indexGetter<T, I>);
                add(&indexQuery<T, I>);
                add(&indexEnumerator<T, I>);
                if constexpr (IndexAccess<T, D>::kWritable) {
                    add(&indexSetter<T, I>);
                }
            }
        });
    }

private:
    std::vector<std::intptr_t> mAll{reinterpret_cast<std::intptr_t>(&returnThis)};
    std::unordered_set<const TypeNode*> mSeen;
};

[[nodiscard]] auto serializeInternalField(v8::Local<v8::Object> holder, int index, void* data)
    -> v8::StartupData;
void deserializeInternalField(v8::Local<v8::Object> holder, int index, v8::StartupData payload,
                              void* data);

} // namespace Detail

template <BoundModule... Mods> [[nodiscard]] auto externalReferences() -> const std::intptr_t* {
    static const std::vector<std::intptr_t> table = [] {
        Detail::References refs;
        (refs.module<Mods>(), ...);
        return refs.finish();
    }();
    return table.data();
}

template <BoundModule... Mods>
[[nodiscard]] auto addTemplates(v8::SnapshotCreator& creator) -> std::vector<std::size_t> {
    const auto isolate = creator.GetIsolate();
    const v8::HandleScope scope(isolate);
    std::vector<std::size_t> indices;
    auto visit = [&](const Detail::TypeNode*, Detail::ClassWalk::TemplateOf templateOf,
                     Detail::ClassWalk::Restore) {
        indices.push_back(creator.AddData(templateOf(isolate)));
    };
    Detail::ClassWalk walk(visit);
    (walk.template module<Mods>(), ...);
    return indices;
}

template <BoundModule... Mods>
void restoreTemplates(v8::Isolate* isolate, const std::vector<std::size_t>& indices) {
    const v8::Isolate::Scope isolateScope(isolate);
    const v8::HandleScope scope(isolate);
    std::size_t next = 0;
    auto visit = [&](const Detail::TypeNode*, Detail::ClassWalk::TemplateOf,
                     const Detail::ClassWalk::Restore restore) {
        if (next < indices.size()) {
            v8::Local<v8::FunctionTemplate> ft;
            if (isolate->GetDataFromSnapshotOnce<v8::FunctionTemplate>(indices[next++])
                    .ToLocal(&ft)) {
                restore(isolate, ft);
            }
        }
    };
    Detail::ClassWalk walk(visit);
    (walk.module<Mods>(), ...);
}

void prepare(v8::Isolate* isolate);

[[nodiscard]] inline auto internalFieldsSerializer() -> v8::SerializeInternalFieldsCallback {
    return {&Detail::serializeInternalField};
}
[[nodiscard]] inline auto internalFieldsDeserializer() -> v8::DeserializeInternalFieldsCallback {
    return {&Detail::deserializeInternalField};
}

} // namespace Alka::snapshot
