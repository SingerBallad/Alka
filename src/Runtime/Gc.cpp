#include <Alka/Binding/Gc.hpp>

#if ALKA_HAS_CPPGC && defined(_MSC_VER) && !defined(__clang__) && defined(USING_V8_SHARED)
namespace {

v8::SourceLocation unknownSourceLocation() {
    return v8::SourceLocation();
}

} // namespace

extern "C" v8::SourceLocation (*const alka_SourceLocation_Current)() = &unknownSourceLocation;

#endif
