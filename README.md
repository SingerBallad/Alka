# Alka

声明式的 C++23 ↔ V8 绑定库：**像写 C++ 类一样描述绑定**，元信息直接来自真实的 C++ 类型结构（编译期萃取），并从同一份绑定信息**全自动生成 TypeScript 定义文件**（.d.ts）。

```cpp
// 你的普通 C++ 类，无需修改
class Player : public Entity {
public:
    Player(std::string name, int level);
    void attack(Entity& target);
    int mHp = 100;
};

// 绑定 = 写一个特化，像写类一样（第三方类型也能绑）
template<>
struct Alka::Bind<Player> : Alka::Class<Player> {          // 类名自动反射 → "Player"
    using Extends = Entity;                                 // 必须是真实 C++ 基类（static_assert 校验）
    static constexpr auto doc = "A controllable player.";
    static constexpr auto members = Alka::members(
        Alka::ctor<std::string, int>(Alka::arg("name"), Alka::arg("level") = 1),
        Alka::method<&Player::attack>(Alka::doc("攻击目标")), // 方法名自动反射 → "attack"
        Alka::prop<&Player::mHp>(Alka::readonly));           // mHp 自动去前缀 → "hp"
};

// 安装到任意 v8::Context（宿主无关），一行生成 .d.ts
Alka::install<GameMod>(context);
std::string dts = Alka::generateDts<GameMod>();
```

```ts
// 生成的 game.d.ts（还有 ES 模块风格可选）
declare namespace game {
    /** A controllable player. */
    class Player extends Entity {
        constructor(name: string, level?: number);
        /** 攻击目标 */
        attack(target: Entity): void;
        readonly hp: number;
    }
}
```

## 特性

- **两种等价的声明形式**：非侵入式 `Alka::Bind<T>` 特化（可绑第三方类型），或类内 `alkaMembers`/`AlkaExtends` 声明；同一 concept 统一检测，特化优先
- **编译期名字反射**：类名 / 方法名 / 属性名 / 枚举项全部从 `__FUNCSIG__` 自动萃取，无需手写字符串；`mHp → hp` 命名策略可全局替换（`NamePolicy<>`），逐处可用 `method<"newName", &T::f>()` 覆盖
- **JS 结构与 C++ 完全一致**：原型链 = 真实继承链（含多级 / 非首基偏移修正）、`instanceof` 原生成立、static 继承、方法/访问器挂原型且不可枚举——并且 **JS `class extends` 原生类**开箱即用（`super()`、JS 覆写、传回 C++）
- **多态降型**：C++ 返回 `Entity*` 而实际是 `Player` 时，JS 拿到的就是 `Player`（typeid → TypeNode 反查）
- **对象身份缓存**：同一 C++ 对象在同一 context 中永远是同一个 JS 对象（`===` 成立，expando 属性在往返间保留）
- **生命周期**：JS 构造 → JS 拥有（GC 弱回调两趟释放）；返回 `T*`/`T&` → 借用别名；返回值 / `unique_ptr` → 移交所有权；`shared_ptr` 双向共享（可从 wrapper 还原原始控制块）
- **转换全家桶**：数值（int64 ↔ BigInt）、字符串（UTF-8）、`vector`/`array` ↔ Array、string 键 map ↔ 对象、其余 map ↔ Map、set ↔ Set、`optional` ↔ `undefined | T`、`tuple/pair` ↔ 定长数组、`variant` ↔ union、字节向量 ↔ Uint8Array、`std::function` ↔ JS 函数双向——全部经由可特化的 `Alka::Converter<T>`
- **重载与默认参**：`method<&A::f, &A::g>()` 声明序优先分发；`arg("dt") = 0.016` 在缺参或 `undefined` 时生效，.d.ts 输出 `dt?: number` 与 `@default`
- **元信息**：`doc` / `arg().doc()` / `deprecated` / 自定义 `meta<"key">(value)`，编译期存储、运行时可查（`classInfo<T>()`）、.d.ts 输出 JSDoc（自定义 tag 渲染钩子）
- **反向调用**：`Alka::Function<R(Args...)>`（`tryCall` → `std::expected` / `call` → 抛 `JsException`）、`Alka::Value`/`Object` 透传句柄
- **错误信息可读**：`arg 2 (dt) of Entity.update: expected number, got string`；C++ 异常自动转 JS `Error`，JS 异常带栈回传 C++

## 构建

Alka 不自带、也不下载 V8：**由宿主提供 V8**（BYO V8）。你需要一个携带 V8 头、配套 ABI 宏、以及 V8/Node 导入库的 CMake target，把它的名字传给 `ALKA_V8_TARGET`。

作为子项目（`FetchContent` / `add_subdirectory`）嵌入宿主——这是 Alka 的主要用法：

```cmake
set(ALKA_V8_TARGET   YourV8::Target)   # 例如 CloverNT 里的 QQNT::QQNT
set(ALKA_BUILD_ENGINE OFF)             # 用宿主已有的 isolate，不自托管 V8
FetchContent_MakeAvailable(Alka)       # 或 add_subdirectory(Alka)
target_link_libraries(yourTarget PRIVATE Alka::Alka)
```

`ALKA_V8_TARGET` 指向的 target 必须在其 INTERFACE 上携带 V8 头目录、V8 ABI 编译宏、V8/Node 链接库；Alka 以 PUBLIC 链接它，三样都会传播给 Alka 自身及所有消费方。

需要 C++23（含 `std::expected` / `std::format`）：MSVC ≥ 14.5x 或 clang ≥ 20 均可（反射对 `__PRETTY_FUNCTION__` 与 `__FUNCSIG__` 两条路径都覆盖，clang-cl 走前者）。

### 独立构建（Alka 自己的 tests / DemoHost）

tests 与 DemoHost 会自托管一个 V8（platform / ICU / `V8::Initialize`），因此 `ALKA_V8_TARGET` 需指向一个**含 libplatform** 的完整 V8 dev target，并保持 `ALKA_BUILD_ENGINE=ON`（默认）：

```powershell
cmake --preset msvc-debug -DALKA_V8_TARGET=<your-full-v8-target> -DALKA_V8_RUNTIME_DIR=<dir-with-dlls+icudtl.dat>
cmake --build --preset msvc-debug
ctest --preset msvc-debug
```

`ALKA_V8_RUNTIME_DIR` 让 `alka_copy_v8_runtime()` 把运行时（DLL + `icudtl.dat`）拷到输出目录；不设则不拷贝。未提供 `ALKA_V8_TARGET` 时 configure 会直接 `FATAL_ERROR`。

### 重要：ABI 宏

V8 的对象布局由 pointer compression / sandbox / 31-bit smis 等宏决定。**任何 include `<v8.h>` 的目标都必须经由 `Alka::Alka`（即 `ALKA_V8_TARGET`）获得与所链 V8 完全一致的宏集**，否则布局不一致——`V8::Initialize()` 会在启动自检时直接终止。这些宏由 `ALKA_V8_TARGET` PUBLIC 传播，故只要通过 `Alka::Alka` 触达 `<v8.h>` 就自动一致；切勿绕过它直接 include `<v8.h>`。`USING_V8_SHARED` 之类的形态宏由宿主的 V8 target 自行决定（如 Node/Electron 的共享 V8 需要它），Alka 不代为设定。

## 嵌入到你的宿主

核心完全宿主无关——拿到别人的 `v8::Local<v8::Context>` 就能工作（`ALKA_BUILD_ENGINE` 无论开关都可用）：

```cpp
Alka::install<GameMod, CoreMod>(context);   // 物化模块（可按 context 选装子集）
// ... 宿主自己的消息循环 ...
Alka::disposeIsolate(isolate);              // isolate 销毁前必须调用（V8 不跑收尾弱回调）
```

需要自托管 V8 时，用便利层（**仅当 `ALKA_BUILD_ENGINE=ON`**，且 `ALKA_V8_TARGET` 含 libplatform）：

```cpp
auto& engine = Alka::Engine::instance();    // ICU → platform → V8::Initialize
auto isolate = engine.createIsolate();      // RAII（自动 disposeIsolate）
Alka::ContextScope scope(isolate.get());
scope.install<GameMod>();
auto result = scope.evalToString("game.VERSION");
```

## 定制点速览

| 定制点 | 作用 |
|---|---|
| `Alka::Converter<T>` 特化 | 任意类型的 JS ↔ C++ 转换（单向亦可） |
| `Alka::TsTypeName<T>` 特化 | 该类型的 .d.ts 映射 |
| `Alka::tsType<"X">` spec | 单个声明处的 TS 类型覆盖 |
| `Alka::NamePolicy<>` 特化 | 全局命名策略（默认属性去 m 前缀） |
| `Alka::EnumRange<E>` 特化 | 枚举反射扫描范围（默认 -128..128） |
| `TsGenOptions::mMetaRenderers` | 自定义 meta → JSDoc tag 渲染 |
| `ALKA_CONTEXT_EMBEDDER_SLOT` 宏 | 与宿主冲突时更换 context 槽位 |

## 已知限制（v1）

- 绑定的回调需可作 NTTP：成员/函数指针或**无捕获** lambda（有状态逻辑经无捕获 lambda 转发）
- C++ 虚函数不会自动分派到 JS 覆写（JS 侧覆写只对 JS 调用生效）
- 非多态多重继承的次基指针，身份缓存可能退化为多个 wrapper（多态类型无此问题）
- `Alka::Value`/`Function` 等句柄勿跨 isolate 使用，且须在 isolate 销毁前释放
- 注册类型的析构函数不得回调 JS（GC 第二趟不是脚本安全点）

## 工具链已知问题

**clang 22（独立版 LLVM）+ 绑定 lambda 内的 `std::format`/`std::print`**：clang 22 会错误编译位于**绑定 lambda 体内**、且带字面量格式串（走 consteval 检查）的 `std::format`/`std::println` 调用——因为 Alka 经 NTTP 模板机制实例化该 lambda，clang 22 会对 MSVC STL 的 consteval 辅助函数 `_Compile_time_parse_format_specs` 发出未解析引用，链接期报 `lld-link: undefined symbol`。

- 与优化级别无关；VS 自带的 clang 20、以及 MSVC `cl` 均不受影响
- 触发需三者同时满足：consteval 格式检查（`std::vformat` 不走该路径，安全）+ 位于 **lambda**（自由/成员函数不触发）+ 经 Alka 的 NTTP 调用实例化
- **规避**：把该回调绑定为自由函数或成员函数（`fn<"name", &func>()`），或在 lambda 内改用 `std::vformat`/iostream。普通函数体内的 `std::format`/`std::print` 完全正常（DemoHost 的 CLI 即如此）

## 许可

MIT
