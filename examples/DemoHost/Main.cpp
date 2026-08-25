#include "GameBindings.hpp"

#include <Alka/Engine.hpp>
#include <Alka/Ts/Generator.hpp>

#include <fstream>
#include <iostream>
#include <print>
#include <sstream>
#include <string>
#include <string_view>

namespace {

struct HostMod;

void hostPrint(const std::string& text) { std::println("{}", text); }

}  // namespace

template<>
struct Alka::Bind<HostMod> : Alka::Module<"host"> {
    static constexpr auto doc = "demo host services";
    static constexpr auto members =
        Alka::members(Alka::fn<"print", &hostPrint>(Alka::arg("text")));
};

namespace {

int usage() {
    std::println("usage:");
    std::println("  DemoHost <script.js>                 run a script");
    std::println("  DemoHost --eval \"code\"               evaluate a snippet");
    std::println("  DemoHost --repl                      interactive prompt");
    std::println("  DemoHost --dts <out.d.ts> [--style global|module]");
    return 2;
}

int generateDefs(const std::string& outPath, std::string_view style) {
    Alka::TsGenOptions options;
    options.mStyle =
        style == "module" ? Alka::TsStyle::EsModule : Alka::TsStyle::GlobalNamespace;
    const std::string dts = Alka::generateDts<Demo::GameMod, HostMod>(options);
    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
        std::println(stderr, "cannot write {}", outPath);
        return 1;
    }
    out << dts;
    std::println("wrote {} ({} style)", outPath,
                 style == "module" ? "es-module" : "global-namespace");
    return 0;
}

int runEval(Alka::ContextScope& scope, std::string_view source, std::string_view name) {
    const auto result = scope.evalToString(source, name);
    if (!result) {
        std::println(stderr, "error: {}", result.error().message());
        return 1;
    }
    if (!result->empty() && *result != "undefined") {
        std::println("{}", *result);
    }
    return 0;
}

int repl(Alka::ContextScope& scope) {
    std::println("Alka {} REPL -- .exit to quit", Alka::versionString());
    std::string lineText;
    while (true) {
        std::print("> ");
        if (!std::getline(std::cin, lineText) || lineText == ".exit") {
            return 0;
        }
        if (lineText.empty()) {
            continue;
        }
        (void)runEval(scope, lineText, "<repl>");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage();
    }
    const std::string_view mode = argv[1];

    if (mode == "--dts") {
        if (argc < 3) {
            return usage();
        }
        std::string_view style = "global";
        if (argc >= 5 && std::string_view(argv[3]) == "--style") {
            style = argv[4];
        }
        return generateDefs(argv[2], style);  // pure: no V8 boot needed
    }

    Alka::Engine& engine = Alka::Engine::instance();
    Alka::IsolateHandle isolate = engine.createIsolate();
    Alka::ContextScope scope(isolate.get());
    scope.install<Demo::GameMod, HostMod>();

    if (mode == "--repl") {
        return repl(scope);
    }
    if (mode == "--eval") {
        if (argc < 3) {
            return usage();
        }
        return runEval(scope, argv[2], "<eval>");
    }

    // treat as a script path
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::println(stderr, "cannot open {}", argv[1]);
        return 1;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return runEval(scope, buffer.str(), argv[1]);
}
