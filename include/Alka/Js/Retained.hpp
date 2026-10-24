#pragma once

#include <Alka/Convert/Converter.hpp>
#include <Alka/Detail/Invoke.hpp>
#include <Alka/Detail/State.hpp>
#include <Alka/Js/Value.hpp>

#include <v8.h>

#include <cstdint>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

// A JS value held by a C++ object WITHOUT creating an uncollectable cycle.
//
//   struct Button {
//     Alka::Retained<Alka::Function<void()>> mOnClick;
//     ...
//    };
//
//   Alka::prop<"onClick", &Button::mOnClick>()   // button.onClick = () => button.x++
//   void Button::watch(Alka::Function<void()> f) {
//     mOnClick.set(this, f);
//   }  // from C++

namespace Alka {

template <class T = Value> class Retained {
    static_assert(std::is_base_of_v<Value, T>,
                  "Alka: Retained<T> takes Value, Object, Function<...> or Promise<...>");

public:
    Retained() = default;
    Retained(Retained&&) noexcept = default;
    Retained& operator=(Retained&& other) noexcept {
        if (this != &other) {
            reset();
            mCell = std::move(other.mCell);
            mLink = std::move(other.mLink);
        }
        return *this;
    }
    Retained(const Retained&) = delete;
    Retained& operator=(const Retained&) = delete;
    ~Retained() { reset(); }

    void set(ConvertCtx& cx, const v8::Local<v8::Object> owner, const v8::Local<v8::Value> value) {
        reset();
        auto cell = std::make_shared<Detail::RetainedCell>();
        if (const auto keeper = Detail::keepingWrapper(cx.mIsolate, owner);
            Detail::ownerKeepsValues(keeper)) {
            Detail::slotRetained(cx, keeper, *cell, value);
        } else {
            cell->mValue = Detail::ValueState::make(cx.mIsolate, cx.mContext, value);
            if (!keeper.IsEmpty() && Detail::isWrapper(keeper)) {
                cell->mOwner = Detail::ValueState::make(cx.mIsolate, cx.mContext, keeper, true);
                Detail::trackStrongRetained(keeper, cell);
            }
        }
        mLink = Detail::isolateLink(cx.mIsolate);
        mCell = std::move(cell);
    }

    template <class Owner> void set(Owner* owner, const T& value) {
        static_assert(BoundClass<std::remove_cv_t<Owner>>,
                      "Alka: Retained::set(owner, value) takes an object of a bound class");
        if (!value.valid() || !value.state()->mLink->onIsolateThread()) {
            reset();
            return;
        }
        const auto isolate = value.isolate();
        const v8::HandleScope scope(isolate);
        const auto context = value.creationContext(isolate);
        const v8::Context::Scope contextScope(context);
        ConvertCtx cx{isolate, context};
        set(cx, Detail::wrapperOf(cx, owner), value.local(isolate));
    }

    void reset() {
        if (mCell && mLink && mLink->onIsolateThread()) {
            const auto& owner = mCell->mOwner;
            if (mCell->mSlot != 0 && owner && !owner->mDead && !owner->mValue.IsEmpty()) {
                const auto isolate = mLink->isolate();
                const v8::HandleScope scope(isolate);
                const auto context = owner->mContext.Get(isolate);
                if (!context.IsEmpty()) {
                    const v8::Context::Scope contextScope(context);
                    ConvertCtx cx{isolate, context};
                    const auto wrapper = owner->mValue.Get(isolate).template As<v8::Object>();
                    if (const auto store = Detail::retainedStore(cx, wrapper, false);
                        !store.IsEmpty()) {
                        std::ignore = store->Delete(
                            context, v8::Integer::NewFromUnsigned(isolate, mCell->mSlot));
                    }
                }
            }
        }
        mCell.reset();
        mLink.reset();
    }

    [[nodiscard]] auto get() const -> T {
        if (!mCell || !mLink || !mLink->onIsolateThread()) {
            return T{};
        }
        const auto& state = mCell->mValue;
        if (!state || state->mDead || state->mValue.IsEmpty() || state->mContext.IsEmpty()) {
            return T{};
        }
        const auto isolate = mLink->isolate();
        const v8::HandleScope scope(isolate);
        const auto context = state->mContext.Get(isolate);
        const auto value = state->mValue.Get(isolate);
        slotIfOwnedNow(isolate, context, value);
        T out;
        static_cast<Value&>(out) = Value(Detail::ValueState::make(isolate, context, value));
        return out;
    }

    explicit operator bool() const noexcept {
        return mCell && mCell->mValue && !mCell->mValue->mDead && !mCell->mValue->mValue.IsEmpty();
    }

    [[nodiscard]] bool weak() const noexcept { return mCell && mCell->mSlot != 0; }

private:
    void slotIfOwnedNow(v8::Isolate* isolate, const v8::Local<v8::Context> context,
                        const v8::Local<v8::Value> value) const {
        const auto& owner = mCell->mOwner;
        if (mCell->mSlot != 0 || !owner || owner->mDead || owner->mValue.IsEmpty()) {
            return;
        }
        const auto wrapper = owner->mValue.Get(isolate).template As<v8::Object>();
        if (Detail::ownerKeepsValues(wrapper)) {
            const v8::Context::Scope contextScope(context);
            ConvertCtx cx{isolate, context};
            Detail::slotRetained(cx, wrapper, *mCell, value);
        }
    }

    std::shared_ptr<Detail::RetainedCell> mCell;
    std::shared_ptr<Detail::IsolateLink> mLink;
};

template <class T> struct Converter<Retained<T>> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<Retained<T>> {
        static_assert(FromJsConvertible<T>,
                      "Alka: JS cannot hand over this T (a Promise<...> is made by C++): set the "
                      "Retained from C++ (retained.set(this, value)) and bind a prop of it with "
                      "Alka::readonly");
        if (!value->IsNullOrUndefined()) {
            if (auto checked = Alka::fromJs<T>(cx, value); !checked) {
                return std::unexpected(std::move(checked).error());
            }
        }
        Retained<T> out;
        if (!value->IsNullOrUndefined()) {
            out.set(cx, cx.mReceiver, value);
        }
        return out;
    }
    static auto toJs(ConvertCtx& cx, const Retained<T>& value) -> v8::Local<v8::Value> {
        return Detail::handleToJs(cx, value.get(), v8::Undefined(cx.mIsolate));
    }
};

} // namespace Alka
