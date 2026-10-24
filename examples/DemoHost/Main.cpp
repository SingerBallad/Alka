#include "GameBindings.hpp"

#include <Alka/Engine/Engine.hpp>
#include <Alka/Engine/Inspector.hpp>
#include <Alka/Ts/Generator.hpp>
#include <InspectorBridge/InspectorBridge.hpp>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>

namespace {

struct HostMod;

void hostPrint(const std::string& text) {
    std::println("{}", text);
}

} // namespace

template <> struct Alka::Bind<HostMod> : Module<"host"> {
    static constexpr auto doc = "demo host services";
    static constexpr auto members = Alka::members(Alka::fn<"print", &hostPrint>(arg("text")));
};

namespace {

int usage() {
    std::println("usage:");
    std::println("  DemoHost <script.js>                 run a script");
    std::println("  DemoHost --module <module.mjs>       run an ES module (import from \"game\")");
    std::println("  DemoHost --eval \"code\"               evaluate a snippet");
    std::println("  DemoHost --repl                      interactive prompt");
    std::println("  DemoHost --dts <out.d.ts> [--style global|module]");
    std::println("  DemoHost --inspect-brk[=port] <script.js> | --module <module.mjs>");
    std::println("                                       debug it in Chrome DevTools");
    return 2;
}

std::optional<std::uint16_t> inspectPort(const std::string_view flag) {
    if (!flag.starts_with("--inspect-brk")) {
        return std::nullopt;
    }
    std::uint16_t port = 9229;
    if (const auto eq = flag.find('='); eq != std::string_view::npos) {
        const auto digits = flag.substr(eq + 1);
        if (std::from_chars(digits.data(), digits.data() + digits.size(), port).ec != std::errc{}) {
            return std::nullopt;
        }
    }
    return port;
}

int generateDefs(const std::string& outPath, std::string_view style) {
    Alka::TsGenOptions options;
    options.mStyle = style == "module" ? Alka::TsStyle::EsModule : Alka::TsStyle::GlobalNamespace;
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
    return Alka::Engine::runUntilIdle(scope.isolate()) ? 0 : 1;
}

int runModule(const Alka::ContextScope& scope, const char* path) {
    scope.registerModules<Demo::GameMod, HostMod>();
    if (const auto result = scope.runModuleFile(path); !result) {
        std::println(stderr, "error: {}", result.error().message());
        return 1;
    }
    return Alka::Engine::runUntilIdle(scope.isolate()) ? 0 : 1;
}

std::string readFile(const char* path, bool& ok) {
    const std::ifstream in(path, std::ios::binary);
    ok = static_cast<bool>(in);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

int repl(Alka::ContextScope& scope) {
    std::println("Alka {} REPL, input .exit to quit", Alka::versionString());
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage();
    }
    const auto debugPort = inspectPort(argv[1]);
    if (std::string_view(argv[1]).starts_with("--inspect-brk")) {
        if (!debugPort || argc < 3) {
            return usage();
        }
        ++argv;
        --argc;
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
        return generateDefs(argv[2], style);
    }

    const Alka::IsolateHandle isolate = Alka::Engine::createIsolate();
    Alka::ContextScope scope(isolate.get());
    scope.install<Demo::GameMod, HostMod>();

    if (const bool runsFile = mode != "--repl" && mode != "--eval" && mode != "--dts";
        debugPort && !runsFile) {
        return usage();
    }
    std::optional<Alka::Inspector> inspector;
    std::optional<AlkaTools::InspectorBridge> bridge; // (destroyed before the inspector)
    if (debugPort) {
        inspector.emplace(isolate.get());
        inspector->contextCreated(scope.context(), "DemoHost");
        bridge.emplace(*inspector, isolate.get(), *debugPort, "DemoHost");
        if (!bridge->listening()) {
            std::println(stderr, "cannot listen on 127.0.0.1:{}", *debugPort);
            return 1;
        }
        std::println("Debugger listening on {}", bridge->webSocketUrl());
        std::println("Open chrome://inspect (configure 127.0.0.1:{}) or {}", bridge->port(),
                     bridge->frontendUrl());
        std::fflush(stdout);
        (void)bridge->waitForFrontend();
        bridge->pauseOnStart();
    }

    if (mode == "--repl") {
        return repl(scope);
    }
    if (mode == "--eval") {
        if (argc < 3) {
            return usage();
        }
        return runEval(scope, argv[2], "<eval>");
    }

    const bool asModule = mode == "--module";
    if (asModule && argc < 3) {
        return usage();
    }
    const char* path = asModule ? argv[2] : argv[1];
    int status = 0;
    if (asModule) {
        status = runModule(scope, path);
    } else {
        bool ok = false;
        const std::string source = readFile(path, ok);
        if (!ok) {
            std::println(stderr, "cannot open {}", path);
            return 1;
        }
        status = runEval(scope, source, path);
    }
    if (bridge && bridge->connected()) {
        std::println("(finished -- close DevTools to exit)");
        std::fflush(stdout);
        bridge->serveUntilDetached();
    }
    return status;
}
