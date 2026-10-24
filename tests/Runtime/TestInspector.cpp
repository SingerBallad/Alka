#include "Support/V8Fixture.hpp"

#include <Alka/Alka.hpp>
#include <Alka/Engine/Inspector.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

TEST(Inspector, SessionsSpeakTheDevToolsProtocol) {
    AlkaTest::V8Fixture fx;
    std::vector<std::string> sent;
    fx.run([&](v8::Isolate* isolate, v8::Local<v8::Context> context) {
        Alka::Inspector inspector(isolate);
        inspector.contextCreated(context, "main");
        auto session = inspector.connect([&](std::string_view json) { sent.emplace_back(json); });

        session->dispatch(R"({"id":1,"method":"Runtime.evaluate","params":{"expression":"6*7"}})");
        ASSERT_FALSE(sent.empty());
        EXPECT_THAT(sent.back(), testing::HasSubstr(R"("id":1)"));
        EXPECT_THAT(sent.back(), testing::HasSubstr(R"("value":42)"));

        session->dispatch(R"({"id":2,"method":"Runtime.enable"})");
        session->dispatch(
            R"json({"id":3,"method":"Runtime.evaluate","params":{"expression":"console.log('héllo 你好')"}})json");
        const std::string backslash(1, '\\');
        const std::string escaped =
            "h" + backslash + "u00e9llo " + backslash + "u4f60" + backslash + "u597d";
        bool sawConsole = false;
        for (const auto& message : sent) {
            if (message.find("Runtime.consoleAPICalled") != std::string::npos &&
                message.find(escaped) != std::string::npos) {
                sawConsole = true;
            }
        }
        EXPECT_TRUE(sawConsole);
        inspector.contextDestroyed(context);
    });
}
