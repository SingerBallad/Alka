#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <libplatform/libplatform.h>
#include <v8.h>

#include <memory>

// Process-wide V8 startup lives here because V8 cannot be re-initialized
// within one process; individual tests only create isolates/contexts.
int main(int argc, char** argv) {
    v8::V8::InitializeICUDefaultLocation(argv[0]);
    std::unique_ptr<v8::Platform> platform = v8::platform::NewDefaultPlatform();
    v8::V8::InitializePlatform(platform.get());
    v8::V8::SetFlagsFromString("--expose-gc");
    v8::V8::Initialize();

    const int result = doctest::Context(argc, argv).run();

    v8::V8::Dispose();
    v8::V8::DisposePlatform();
    return result;
}
