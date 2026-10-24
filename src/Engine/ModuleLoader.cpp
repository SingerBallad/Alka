#include "EngineState.hpp"

#include <Alka/Convert/Functional.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

namespace Alka::EngineDetail {

namespace {

auto toPath(const std::string& utf8) -> std::filesystem::path {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

auto toUtf8(const std::filesystem::path& path) -> std::string {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

auto fileUrlOf(const std::filesystem::path& path) -> std::string {
    constexpr std::string_view kHex = "0123456789ABCDEF";
    constexpr std::string_view kPlain = "-._~/:!$&'()*+,;=@";
    std::string url = "file://";
    const auto generic = path.generic_u8string();
    if (!generic.empty() && generic.front() != u8'/') {
        url += '/'; // a drive letter: file:///C:/...
    }
    for (const char8_t c : generic) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') ||
            kPlain.find(static_cast<char>(byte)) != std::string_view::npos) {
            url += static_cast<char>(byte);
        } else {
            url += '%';
            url += kHex[byte >> 4];
            url += kHex[byte & 15];
        }
    }
    return url;
}

auto percentDecoded(const std::string_view text) -> std::string {
    const auto digit = [](const char c) -> int {
        return c >= '0' && c <= '9'   ? c - '0'
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
               : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                      : -1;
    };
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && digit(text[i + 1]) >= 0 &&
            digit(text[i + 2]) >= 0) {
            out += static_cast<char>(digit(text[i + 1]) * 16 + digit(text[i + 2]));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

auto readFile(const std::filesystem::path& path) -> std::optional<std::string> {
    const std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

auto locate(const std::filesystem::path& candidate) -> std::optional<std::filesystem::path> {
    std::error_code error;
    for (const auto& path : {candidate, std::filesystem::path(candidate).concat(".mjs"),
                             std::filesystem::path(candidate).concat(".js"),
                             candidate / "index.mjs", candidate / "index.js"}) {
        if (std::filesystem::is_regular_file(path, error)) {
            return std::filesystem::weakly_canonical(path, error);
        }
    }
    return std::nullopt;
}

auto recordOf(ContextState& context, const v8::Local<v8::Module> module) -> ModuleRecord* {
    for (const auto& record : context.mModules) {
        if (record->mModule == module) {
            return record.get();
        }
    }
    return nullptr;
}

auto recordByPath(ContextState& context, const std::string& path) -> ModuleRecord* {
    for (const auto& record : context.mModules) {
        if (!record->mPath.empty() && record->mPath == path) {
            return record.get();
        }
    }
    return nullptr;
}

void throwModuleError(v8::Isolate* isolate, const std::string& message) {
    isolate->ThrowException(v8::Exception::Error(Detail::v8Str(isolate, message)));
}

auto contextFor(const v8::Local<v8::Context> context) -> ContextState* {
    auto* state = stateOf(v8::Isolate::GetCurrent());
    return state != nullptr ? contextStateOf(*state, context) : nullptr;
}

auto evaluateJsonModule(const v8::Local<v8::Context> context, const v8::Local<v8::Module> module)
    -> v8::MaybeLocal<v8::Value> {
    const auto isolate = v8::Isolate::GetCurrent();
    auto* state = contextFor(context);
    auto* record = state != nullptr ? recordOf(*state, module) : nullptr;
    if (record == nullptr || record->mJson.IsEmpty() ||
        module
            ->SetSyntheticModuleExport(isolate, Detail::cachedKey<"default">(isolate),
                                       record->mJson.Get(isolate))
            .IsNothing()) {
        return {};
    }
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver) ||
        resolver->Resolve(context, v8::Undefined(isolate)).IsNothing()) {
        return {};
    }
    return resolver->GetPromise();
}

auto loadFileModule(ContextState& state, const v8::Local<v8::Context> context,
                    const std::string& specifier, const std::filesystem::path& base)
    -> v8::MaybeLocal<v8::Module> {
    const auto isolate = v8::Isolate::GetCurrent();
    std::filesystem::path target;
    if (specifier.starts_with("file://")) {
        auto rest = percentDecoded(std::string_view(specifier).substr(7));
        if (rest.size() >= 3 && rest[0] == '/' && rest[2] == ':') {
            rest.erase(0, 1); // file:///C:/x -> C:/x
        }
        target = toPath(rest);
    } else if (specifier.starts_with("./") || specifier.starts_with("../") ||
               specifier.starts_with('/') || toPath(specifier).is_absolute()) {
        target = toPath(specifier).is_absolute() ? toPath(specifier) : base / toPath(specifier);
    } else {
        throwModuleError(isolate, "Cannot find module '" + specifier + "'");
        return {};
    }
    const auto found = locate(target.lexically_normal());
    if (!found) {
        throwModuleError(isolate, "Cannot find module '" + specifier + "' (looked for " +
                                      toUtf8(target.lexically_normal()) + ")");
        return {};
    }
    const auto path = toUtf8(*found);
    if (const auto* existing = recordByPath(state, path)) {
        return existing->mModule.Get(isolate);
    }
    const auto source = readFile(*found);
    if (!source) {
        throwModuleError(isolate, "Cannot read module '" + path + "'");
        return {};
    }
    if (found->extension() == ".json") {
        v8::Local<v8::Value> data;
        if (!v8::JSON::Parse(context, Detail::v8Str(isolate, *source)).ToLocal(&data)) {
            return {};
        }
        const v8::Local<v8::String> exports[] = {Detail::cachedKey<"default">(isolate)};
        const auto module = v8::Module::CreateSyntheticModule(
            isolate, Detail::v8Str(isolate, path),
            v8::MemorySpan<const v8::Local<v8::String>>(exports, 1), &evaluateJsonModule);
        auto record = std::make_unique<ModuleRecord>();
        record->mModule.Reset(isolate, module);
        record->mPath = path;
        record->mUrl = fileUrlOf(*found);
        record->mJson.Reset(isolate, data);
        state.mModules.push_back(std::move(record));
        return module;
    }
    return compileModule(state, context, *source, path, path);
}

auto baseOf(ContextState& state, const v8::Local<v8::Module> referrer) -> std::filesystem::path {
    if (!referrer.IsEmpty()) {
        if (const auto* record = recordOf(state, referrer);
            record != nullptr && !record->mPath.empty()) {
            return toPath(record->mPath).parent_path();
        }
    }
    return state.mModuleRoot;
}

auto resolveSpecifier(const v8::Local<v8::Context> context, const v8::Local<v8::String> specifier,
                      const std::filesystem::path* base, const v8::Local<v8::Module> referrer)
    -> v8::MaybeLocal<v8::Module> {
    const auto isolate = v8::Isolate::GetCurrent();
    v8::Local<v8::Module> builtin;
    if (resolveModule(context, specifier).ToLocal(&builtin)) {
        return builtin;
    }
    const auto text = Detail::stringToUtf8(isolate, specifier);
    auto* state = contextFor(context);
    if (state == nullptr) {
        throwModuleError(isolate, "Cannot find module '" + text + "'");
        return {};
    }
    return loadFileModule(*state, context, text,
                          base != nullptr ? *base : baseOf(*state, referrer));
}

auto importDynamically(const v8::Local<v8::Context> context, v8::Local<v8::Data>,
                       const v8::Local<v8::Value> resourceName,
                       const v8::Local<v8::String> specifier, v8::Local<v8::FixedArray>)
    -> v8::MaybeLocal<v8::Promise> {
    const auto isolate = v8::Isolate::GetCurrent();
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
        return {};
    }
    const auto promise = resolver->GetPromise();
    v8::TryCatch tryCatch(isolate);
    const auto reject = [&] {
        if (tryCatch.HasCaught() && !tryCatch.HasTerminated()) {
            std::ignore = resolver->Reject(context, tryCatch.Exception());
            tryCatch.Reset();
        }
    };

    std::optional<std::filesystem::path> base;
    if (resourceName->IsString()) {
        const auto name = toPath(Detail::stringToUtf8(isolate, resourceName.As<v8::String>()));
        std::error_code error;
        if (name.is_absolute() && std::filesystem::is_regular_file(name, error)) {
            base = name.parent_path();
        }
    }
    v8::Local<v8::Module> module;
    if (!resolveSpecifier(context, specifier, base ? &*base : nullptr, {}).ToLocal(&module) ||
        !module->InstantiateModule(context, &resolveFileModule).FromMaybe(false)) {
        reject();
        return promise;
    }
    v8::Local<v8::Value> evaluation;
    if (!module->Evaluate(context).ToLocal(&evaluation)) {
        reject();
        return promise;
    }
    const auto ns = module->GetModuleNamespace();
    if (!evaluation->IsPromise()) {
        std::ignore = resolver->Resolve(context, ns);
        return promise;
    }

    ConvertCtx cx{isolate, context};
    auto settle =
        std::make_shared<std::pair<v8::Global<v8::Promise::Resolver>, v8::Global<v8::Value>>>(
            v8::Global<v8::Promise::Resolver>(isolate, resolver),
            v8::Global<v8::Value>(isolate, ns));
    const auto onFulfilled =
        toJs(cx, callable([settle](const CallInfo& call) {
                 std::ignore = settle->first.Get(call.mIsolate)
                                   ->Resolve(call.mContext, settle->second.Get(call.mIsolate));
             }));
    const auto onRejected = toJs(cx, callable([settle](const CallInfo& call, const Value& reason) {
                                     std::ignore =
                                         settle->first.Get(call.mIsolate)
                                             ->Reject(call.mContext, reason.local(call.mIsolate));
                                 }));
    if (onFulfilled->IsFunction() && onRejected->IsFunction()) {
        std::ignore = evaluation.As<v8::Promise>()->Then(context, onFulfilled.As<v8::Function>(),
                                                         onRejected.As<v8::Function>());
    }
    return promise;
}

void initializeImportMeta(const v8::Local<v8::Context> context, const v8::Local<v8::Module> module,
                          const v8::Local<v8::Object> meta) {
    const auto isolate = v8::Isolate::GetCurrent();
    auto* state = contextFor(context);
    auto* record = state != nullptr ? recordOf(*state, module) : nullptr;
    if (record != nullptr) {
        std::ignore = meta->CreateDataProperty(context, Detail::cachedKey<"url">(isolate),
                                               Detail::v8Str(isolate, record->mUrl));
    }
}

} // namespace

void installModuleHooks(v8::Isolate* isolate) {
    isolate->SetHostImportModuleDynamicallyCallback(&importDynamically);
    isolate->SetHostInitializeImportMetaObjectCallback(&initializeImportMeta);
}

auto resolveFileModule(const v8::Local<v8::Context> context, const v8::Local<v8::String> specifier,
                       v8::Local<v8::FixedArray>, const v8::Local<v8::Module> referrer)
    -> v8::MaybeLocal<v8::Module> {
    return resolveSpecifier(context, specifier, nullptr, referrer);
}

auto compileModule(ContextState& state, const v8::Local<v8::Context> context,
                   const std::string_view source, const std::string_view name,
                   const std::string& path) -> v8::MaybeLocal<v8::Module> {
    const auto isolate = v8::Isolate::GetCurrent();
    const v8::ScriptOrigin origin(Detail::v8Str(isolate, name), 0, 0, false, -1,
                                  v8::Local<v8::Value>(), false, false, true);
    v8::ScriptCompiler::Source compileSource(Detail::v8Str(isolate, source), origin);
    v8::Local<v8::Module> module;
    if (!v8::ScriptCompiler::CompileModule(isolate, &compileSource).ToLocal(&module)) {
        return {};
    }
    auto record = std::make_unique<ModuleRecord>();
    record->mModule.Reset(isolate, module);
    record->mPath = path;
    record->mUrl = path.empty() ? std::string(name) : fileUrlOf(toPath(path));
    state.mModules.push_back(std::move(record));
    (void)context;
    return module;
}

} // namespace Alka::EngineDetail
