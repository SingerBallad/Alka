#include <gtest/gtest.h>

#include <Alka/Engine/Engine.hpp>

#include <libplatform/libplatform.h>
#include <v8.h>

#include <cstddef>
#include <cstdlib>
#include <memory>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace AlkaTest {
v8::Platform* gPlatform = nullptr;
} // namespace AlkaTest

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
#if defined(_MSC_VER) && defined(_DEBUG)
    char* heapCheck = nullptr;
    std::size_t heapCheckLength = 0;
    if (_dupenv_s(&heapCheck, &heapCheckLength, "ALKA_TEST_HEAPCHECK") == 0 &&
        heapCheck != nullptr) {
        if (const int every = std::atoi(heapCheck); every > 0) {
            const int flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
            _CrtSetDbgFlag(every == 1
                               ? (flags | _CRTDBG_CHECK_ALWAYS_DF)
                               : ((flags & 0xFFFF) | ((every < 0xFFFF ? every : 0xFFFF) << 16)));
        }
        std::free(heapCheck);
    }
#endif
    v8::V8::InitializeICUDefaultLocation(argv[0]);
    std::unique_ptr<v8::Platform> platform = v8::platform::NewDefaultPlatform();
    v8::V8::InitializePlatform(platform.get());
    AlkaTest::gPlatform = platform.get();
    v8::V8::SetFlagsFromString("--expose-gc --allow-natives-syntax");
#if V8_MAJOR_VERSION >= 13
    v8::V8::SetFlagsFromString("--js-explicit-resource-management");
#endif
    v8::V8::Initialize();
    (void)Alka::Engine::instance({.mPlatform = platform.get()});

    const int result = RUN_ALL_TESTS();

    v8::V8::Dispose();
    v8::V8::DisposePlatform();
    return result;
}
