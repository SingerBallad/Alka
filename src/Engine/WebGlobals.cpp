#include "EngineState.hpp"

#include <Alka/Convert/Buffers.hpp>
#include <Alka/Convert/Functional.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Alka::EngineDetail {

namespace {

constexpr std::string_view kConsoleSource = R"js((function (write, builtin) {
  'use strict';
  const forward = (name, args) => {
    if (builtin && typeof builtin[name] === 'function') Reflect.apply(builtin[name], builtin, args);
  };
  const counts = new Map();
  const timers = new Map();
  let indent = '';
  const quote = (s) => "'" + String(s).replace(/\\/g, '\\\\').replace(/'/g, "\\'")
                                      .replace(/\n/g, '\\n') + "'";
  const keyText = (k) => /^[A-Za-z_$][\w$]*$/.test(k) ? k : quote(k);
  function inspect(v, depth, seen) {
    switch (typeof v) {
      case 'string': return depth === 0 ? v : quote(v);
      case 'number': return Object.is(v, -0) ? '-0' : String(v);
      case 'bigint': return v + 'n';
      case 'symbol': return v.toString();
      case 'undefined': return 'undefined';
      case 'boolean': return String(v);
      case 'function': {
        let source = '';
        try { source = Function.prototype.toString.call(v); } catch (e) {}
        return (source.startsWith('class') ? '[class ' : '[Function: ') +
               (v.name || '(anonymous)') + ']';
      }
    }
    if (v === null) return 'null';
    if (seen.includes(v)) return '[Circular]';
    try {
      if (v instanceof Error) return v.stack || String(v);
      if (v instanceof Date) return isNaN(v.getTime()) ? 'Invalid Date' : v.toISOString();
      if (v instanceof RegExp) return String(v);
      if (v instanceof Promise) return 'Promise {}';
    } catch (e) {}
    if (depth > 2) return Array.isArray(v) ? '[Array]' : '[Object]';
    const inner = seen.concat([v]);
    const show = (x) => inspect(x, depth + 1, inner);
    if (Array.isArray(v)) {
      const items = [];
      for (let i = 0; i < Math.min(v.length, 100); ++i) items.push(i in v ? show(v[i]) : '<empty>');
      if (v.length > 100) items.push('... ' + (v.length - 100) + ' more items');
      return items.length ? '[ ' + items.join(', ') + ' ]' : '[]';
    }
    if (ArrayBuffer.isView(v) && !(v instanceof DataView)) {
      const items = Array.from(v.subarray(0, 100), show);
      if (v.length > 100) items.push('... ' + (v.length - 100) + ' more items');
      return v.constructor.name + '(' + v.length + ') [' +
             (items.length ? ' ' + items.join(', ') + ' ' : '') + ']';
    }
    if (v instanceof Map) {
      return 'Map(' + v.size + ') {' + (v.size ? ' ' + Array.from(v, ([k, x]) =>
             show(k) + ' => ' + show(x)).join(', ') + ' ' : '') + '}';
    }
    if (v instanceof Set) {
      return 'Set(' + v.size + ') {' + (v.size ? ' ' + Array.from(v, show).join(', ') + ' ' : '') +
             '}';
    }
    let keys = [];
    try { keys = Object.keys(v); } catch (e) {}
    const proto = Object.getPrototypeOf(v);
    const name = proto === null ? '[Object: null prototype] ' :
                 proto === Object.prototype ? '' :
                 ((proto.constructor && proto.constructor.name) || 'Object') + ' ';
    if (!keys.length) return name + '{}';
    return name + '{ ' + keys.map((k) => {
      let x;
      try { x = v[k]; } catch (e) { return keyText(k) + ': [Getter threw]'; }
      return keyText(k) + ': ' + show(x);
    }).join(', ') + ' }';
  }
  function format(args) {
    let head = '';
    let used = 0;
    if (typeof args[0] === 'string' && args.length > 1 && args[0].includes('%')) {
      head = args[0].replace(/%[sdifjoOc%]/g, (spec) => {
        if (spec === '%%') return '%';
        if (used + 1 >= args.length) return spec;
        const a = args[++used];
        switch (spec) {
          case '%s': return typeof a === 'string' ? a : inspect(a, 1, []);
          case '%d': return typeof a === 'bigint' ? a + 'n' : String(Number(a));
          case '%i': return typeof a === 'bigint' ? a + 'n' : String(parseInt(a));
          case '%f': return String(parseFloat(a));
          case '%j': try { return JSON.stringify(a); } catch (e) { return '[Circular]'; }
          case '%o': case '%O': return inspect(a, 1, []);
          case '%c': return '';
        }
        return spec;
      });
      used++;
    }
    const rest = args.slice(used).map((a) => inspect(a, 0, []));
    if (used > 0) rest.unshift(head);
    return rest.join(' ');
  }
  const emit = (level, text) =>
      write(level, indent ? text.split('\n').map((line) => indent + line).join('\n') : text);
  const methods = {
    log: (...a) => emit(0, format(a)),
    info: (...a) => emit(1, format(a)),
    debug: (...a) => emit(2, format(a)),
    warn: (...a) => emit(3, format(a)),
    error: (...a) => emit(4, format(a)),
    trace: (...a) => {
      const stack = (new Error().stack || '').split('\n').slice(3).join('\n');
      emit(4, 'Trace' + (a.length ? ': ' + format(a) : '') + (stack ? '\n' + stack : ''));
    },
    assert: (ok, ...a) => { if (!ok) emit(4, 'Assertion failed' + (a.length ? ': ' + format(a) : '')); },
    dir: (v) => emit(0, inspect(v, 1, [])),
    table: (v) => emit(0, inspect(v, 1, [])),
    count: (label = 'default') => {
      const n = (counts.get(label) || 0) + 1;
      counts.set(label, n);
      emit(0, label + ': ' + n);
    },
    countReset: (label = 'default') => { counts.delete(label); },
    time: (label = 'default') => { timers.set(label, Date.now()); },
    timeLog: (label = 'default', ...a) => {
      const t = timers.get(label);
      if (t !== undefined) emit(0, label + ': ' + (Date.now() - t) + 'ms' + (a.length ? ' ' + format(a) : ''));
    },
    timeEnd: (label = 'default') => {
      const t = timers.get(label);
      if (t !== undefined) { timers.delete(label); emit(0, label + ': ' + (Date.now() - t) + 'ms'); }
    },
    group: (...a) => { if (a.length) emit(0, format(a)); indent += '  '; },
    groupCollapsed: (...a) => { if (a.length) emit(0, format(a)); indent += '  '; },
    groupEnd: () => { indent = indent.slice(2); },
  };
  const console = {};
  for (const name of Object.keys(methods)) {
    const method = methods[name];
    Object.defineProperty(console, name, {
      value: { [name](...a) { method(...a); forward(name, a); } }[name],
      writable: true, enumerable: true, configurable: true,
    });
  }
  return console;
}))js";

constexpr std::string_view kWebSource = R"js((function (global, native) {
  'use strict';
  const define = (name, value) =>
      Object.defineProperty(global, name, { value, writable: true, configurable: true });
  const domError = (name, message) => { const e = new Error(message); e.name = name; return e; };
  const kInternal = Symbol('internal');

  define('reportError', function reportError(error) { native.reportError(error); });
  define('queueMicrotask', function queueMicrotask(callback) {
    if (typeof callback !== 'function') throw new TypeError('The callback must be a function');
    Promise.resolve().then(() => {
      try { callback(); } catch (error) { native.reportError(error); }
    });
  });
  define('structuredClone', function structuredClone(value, options = undefined) {
    const transfer = options != null && options.transfer != null ? Array.from(options.transfer) : [];
    return native.clone(value, transfer);
  });

  const timeOrigin = native.timeOrigin;
  define('performance', {
    timeOrigin,
    now: function now() { return native.now(); },
    toJSON: function toJSON() { return { timeOrigin }; },
  });

  class TextEncoder {
    get encoding() { return 'utf-8'; }
    encode(input = '') { return native.encodeUtf8(String(input)); }
    encodeInto(source, destination) {
      if (!(destination instanceof Uint8Array)) throw new TypeError('The destination must be a Uint8Array');
      const text = String(source);
      let read = 0;
      let written = 0;
      for (let i = 0; i < text.length;) {
        let c = text.charCodeAt(i);
        let units = 1;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.length) {
          const d = text.charCodeAt(i + 1);
          if (d >= 0xDC00 && d <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); units = 2; }
        }
        if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
        const size = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (written + size > destination.length) break;
        if (size === 1) {
          destination[written++] = c;
        } else {
          destination[written++] = (size === 2 ? 0xC0 : size === 3 ? 0xE0 : 0xF0) | (c >> (6 * (size - 1)));
          for (let shift = 6 * (size - 2); shift >= 0; shift -= 6) destination[written++] = 0x80 | ((c >> shift) & 0x3F);
        }
        i += units;
        read += units;
      }
      return { read, written };
    }
  }
  class TextDecoder {
    #fatal;
    #ignoreBOM;
    #pending = null;  // an incomplete sequence held back while streaming
    #started = false;
    constructor(label = 'utf-8', options = undefined) {
      const name = String(label).trim().toLowerCase();
      if (name !== 'utf-8' && name !== 'utf8' && name !== 'unicode-1-1-utf-8') {
        throw new RangeError('The "' + label + '" encoding is not supported');
      }
      this.#fatal = !!(options && options.fatal);
      this.#ignoreBOM = !!(options && options.ignoreBOM);
    }
    get encoding() { return 'utf-8'; }
    get fatal() { return this.#fatal; }
    get ignoreBOM() { return this.#ignoreBOM; }
    decode(input = undefined, options = undefined) {
      const stream = !!(options && options.stream);
      let bytes;
      if (input === undefined) {
        bytes = new Uint8Array(0);
      } else if (ArrayBuffer.isView(input)) {
        bytes = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
      } else if (input instanceof ArrayBuffer ||
                 (typeof SharedArrayBuffer === 'function' && input instanceof SharedArrayBuffer)) {
        bytes = new Uint8Array(input);
      } else {
        throw new TypeError('The input must be an ArrayBuffer or an ArrayBufferView');
      }
      if (this.#pending) {
        const joined = new Uint8Array(this.#pending.length + bytes.length);
        joined.set(this.#pending);
        joined.set(bytes, this.#pending.length);
        bytes = joined;
        this.#pending = null;
      }
      let end = bytes.length;
      if (stream) {
        let lead = end - 1;
        while (lead >= 0 && end - lead < 4 && (bytes[lead] & 0xC0) === 0x80) --lead;
        if (lead >= 0) {
          const b = bytes[lead];
          const need = b >= 0xF0 && b <= 0xF4 ? 4 : b >= 0xE0 && b <= 0xEF ? 3 : b >= 0xC2 && b <= 0xDF ? 2 : 1;
          if (end - lead < need) end = lead;
        }
        if (end < bytes.length) this.#pending = bytes.slice(end);
      }
      let start = 0;
      if (!this.#started && end > 0) {
        if (!this.#ignoreBOM && end >= 3 && bytes[0] === 0xEF && bytes[1] === 0xBB && bytes[2] === 0xBF) start = 3;
        this.#started = true;
      }
      const text = native.decodeUtf8(bytes.subarray(start, end), this.#fatal);
      if (!stream) {
        this.#started = false;
        this.#pending = null;
      }
      return text;
    }
  }
  define('TextEncoder', TextEncoder);
  define('TextDecoder', TextDecoder);

  const eventStates = new WeakMap();
  class Event {
    constructor(type, init = undefined) {
      if (arguments.length === 0) throw new TypeError('The "type" argument must be specified');
      eventStates.set(this, {
        type: String(type), cancelable: !!(init && init.cancelable), canceled: false,
        stopped: false, target: null, dispatching: false, timeStamp: native.now(),
      });
    }
    get type() { return eventStates.get(this).type; }
    get target() { return eventStates.get(this).target; }
    get currentTarget() { const s = eventStates.get(this); return s.dispatching ? s.target : null; }
    get srcElement() { return this.target; }
    get eventPhase() { return eventStates.get(this).dispatching ? 2 : 0; }
    get cancelable() { return eventStates.get(this).cancelable; }
    get defaultPrevented() { const s = eventStates.get(this); return s.cancelable && s.canceled; }
    get timeStamp() { return eventStates.get(this).timeStamp; }
    get bubbles() { return false; }
    get composed() { return false; }
    get isTrusted() { return false; }
    preventDefault() { eventStates.get(this).canceled = true; }
    stopPropagation() {}
    stopImmediatePropagation() { eventStates.get(this).stopped = true; }
    composedPath() { const s = eventStates.get(this); return s.dispatching ? [s.target] : []; }
  }
  const listenerLists = new WeakMap();
  const captureOf = (options) => typeof options === 'boolean' ? options : !!(options && options.capture);
  class EventTarget {
    constructor() { listenerLists.set(this, new Map()); }
    addEventListener(type, listener, options = undefined) {
      if (listener == null) return;
      const capture = captureOf(options);
      const once = typeof options === 'object' && options !== null && !!options.once;
      const signal = typeof options === 'object' && options !== null ? options.signal : undefined;
      if (signal && signal.aborted) return;
      const lists = listenerLists.get(this);
      const key = String(type);
      let list = lists.get(key);
      if (!list) lists.set(key, list = []);
      if (list.some((entry) => entry.listener === listener && entry.capture === capture)) return;
      list.push({ listener, capture, once, removed: false });
      if (signal) {
        signal.addEventListener('abort', () => this.removeEventListener(key, listener, capture), { once: true });
      }
    }
    removeEventListener(type, listener, options = undefined) {
      const list = listenerLists.get(this).get(String(type));
      if (!list) return;
      const capture = captureOf(options);
      const index = list.findIndex((entry) => entry.listener === listener && entry.capture === capture);
      if (index >= 0) {
        list[index].removed = true;
        list.splice(index, 1);
      }
    }
    dispatchEvent(event) {
      if (!(event instanceof Event)) throw new TypeError('The "event" argument must be an Event');
      const state = eventStates.get(event);
      if (state.dispatching) throw domError('InvalidStateError', 'The event is already being dispatched');
      state.target = this;
      state.dispatching = true;
      const list = listenerLists.get(this).get(state.type);
      for (const entry of list ? list.slice() : []) {
        if (entry.removed) continue;
        if (entry.once) this.removeEventListener(state.type, entry.listener, entry.capture);
        try {
          if (typeof entry.listener === 'function') entry.listener.call(this, event);
          else if (typeof entry.listener.handleEvent === 'function') entry.listener.handleEvent(event);
        } catch (error) {
          native.reportError(error);  // (like a browser: the other listeners still run)
        }
        if (state.stopped) break;
      }
      state.dispatching = false;
      return !event.defaultPrevented;
    }
  }
  define('Event', Event);
  define('EventTarget', EventTarget);

  const signalStates = new WeakMap();
  class AbortSignal extends EventTarget {
    constructor(key = undefined) {
      if (key !== kInternal) throw new TypeError('Illegal constructor');
      super();
      signalStates.set(this, { aborted: false, reason: undefined, onabort: null, handler: null });
    }
    get aborted() { return signalStates.get(this).aborted; }
    get reason() { return signalStates.get(this).reason; }
    throwIfAborted() { const s = signalStates.get(this); if (s.aborted) throw s.reason; }
    get onabort() { return signalStates.get(this).onabort; }
    set onabort(handler) {
      const s = signalStates.get(this);
      s.onabort = typeof handler === 'function' ? handler : null;
      if (s.onabort && !s.handler) {  // (an event handler: a listener in registration order)
        s.handler = (event) => { const h = signalStates.get(this).onabort; if (h) h.call(this, event); };
        this.addEventListener('abort', s.handler);
      } else if (!s.onabort && s.handler) {
        this.removeEventListener('abort', s.handler);
        s.handler = null;
      }
    }
    static abort(reason = undefined) {
      const controller = new AbortController();
      controller.abort(reason);
      return controller.signal;
    }
    static timeout(ms) {
      const controller = new AbortController();
      // (does not keep the host's event loop alive)
      native.unrefTimer(setTimeout(
          () => controller.abort(domError('TimeoutError', 'The operation timed out')), ms));
      return controller.signal;
    }
    static any(signals) {
      const controller = new AbortController();
      const list = Array.from(signals);
      for (const signal of list) {
        if (!(signal instanceof AbortSignal)) throw new TypeError('Expected AbortSignal instances');
        if (signal.aborted) {
          controller.abort(signal.reason);
          return controller.signal;
        }
      }
      for (const signal of list) {
        signal.addEventListener('abort', () => controller.abort(signal.reason), { once: true });
      }
      return controller.signal;
    }
  }
  function abortSignal(signal, reason) {
    const s = signalStates.get(signal);
    if (s.aborted) return;
    s.aborted = true;
    s.reason = reason === undefined ? domError('AbortError', 'This operation was aborted') : reason;
    signal.dispatchEvent(new Event('abort'));
  }
  class AbortController {
    #signal = new AbortSignal(kInternal);
    get signal() { return this.#signal; }
    abort(reason = undefined) { abortSignal(this.#signal, reason); }
  }
  define('AbortSignal', AbortSignal);
  define('AbortController', AbortController);
}))js";

template <class... Args>
auto runFactory(const v8::Local<v8::Context> context, const std::string_view source,
                const Args... args) -> v8::MaybeLocal<v8::Value> {
    const auto isolate = v8::Isolate::GetCurrent();
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> factory;
    if (!v8::Script::Compile(context, Detail::v8Str(isolate, source)).ToLocal(&script) ||
        !script->Run(context).ToLocal(&factory) || !factory->IsFunction()) {
        return {};
    }
    v8::Local<v8::Value> argv[] = {args...};
    return factory.As<v8::Function>()->Call(context, v8::Undefined(isolate),
                                            static_cast<int>(sizeof...(Args)), argv);
}

bool isValidUtf8(const std::uint8_t* bytes, const std::size_t size) {
    std::size_t i = 0;
    while (i < size) {
        const auto b = bytes[i];
        if (b < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        std::uint8_t low = 0x80;
        std::uint8_t high = 0xBF;
        if (b >= 0xC2 && b <= 0xDF) {
            length = 2;
        } else if (b >= 0xE0 && b <= 0xEF) {
            length = 3;
            low = b == 0xE0 ? 0xA0 : 0x80;
            high = b == 0xED ? 0x9F : 0xBF;
        } else if (b >= 0xF0 && b <= 0xF4) {
            length = 4;
            low = b == 0xF0 ? 0x90 : 0x80;
            high = b == 0xF4 ? 0x8F : 0xBF;
        } else {
            return false;
        }
        if (i + length > size || bytes[i + 1] < low || bytes[i + 1] > high) {
            return false;
        }
        for (std::size_t k = 2; k < length; ++k) {
            if ((bytes[i + k] & 0xC0) != 0x80) {
                return false;
            }
        }
        i += length;
    }
    return true;
}

auto cloneValue(const CallInfo& call, const Value& value, const std::vector<Value>& transfer)
    -> Value {
    const auto isolate = call.mIsolate;
    const auto context = call.mContext;
    v8::TryCatch tryCatch(isolate);
    std::vector<v8::Local<v8::ArrayBuffer>> buffers;
    for (const auto& item : transfer) {
        const auto local = item.local(isolate);
        if (local.IsEmpty() || !local->IsArrayBuffer() ||
            !local.As<v8::ArrayBuffer>()->IsDetachable()) {
            throw JsException("structuredClone: only detachable ArrayBuffers can be transferred");
        }
        buffers.push_back(local.As<v8::ArrayBuffer>());
    }
    v8::ValueSerializer serializer(isolate);
    serializer.WriteHeader();
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        serializer.TransferArrayBuffer(static_cast<std::uint32_t>(i), buffers[i]);
    }
    if (!serializer.WriteValue(context, value.local(isolate)).FromMaybe(false)) {
        throw JsException(Detail::caughtError(isolate, context, tryCatch));
    }
    const auto [data, size] = serializer.Release();
    const std::unique_ptr<std::uint8_t, void (*)(void*)> owned(data, &std::free);
    v8::ValueDeserializer deserializer(isolate, data, size);
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        auto store = buffers[i]->GetBackingStore();
        if (buffers[i]->Detach(v8::Local<v8::Value>()).IsNothing()) {
            throw JsException(Detail::caughtError(isolate, context, tryCatch));
        }
        deserializer.TransferArrayBuffer(static_cast<std::uint32_t>(i),
                                         v8::ArrayBuffer::New(isolate, std::move(store)));
    }
    v8::Local<v8::Value> copy;
    if (!deserializer.ReadHeader(context).FromMaybe(false) ||
        !deserializer.ReadValue(context).ToLocal(&copy)) {
        throw JsException(Detail::caughtError(isolate, context, tryCatch));
    }
    ConvertCtx cx{isolate, context};
    return {cx, copy};
}

} // namespace

void installConsole(const v8::Local<v8::Context> context, ConsoleSink sink) {
    const auto isolate = v8::Isolate::GetCurrent();
    const v8::HandleScope scope(isolate);
    ConvertCtx cx{isolate, context};
    const auto write =
        toJs(cx, callable(
                     [sink = std::move(sink)](const int level, const std::string& text) {
                         sink(static_cast<ConsoleLevel>(level), text);
                     },
                     "write"));
    const auto key = Detail::cachedKey<"console">(isolate);
    v8::Local<v8::Value> builtin;
    if (!context->Global()->Get(context, key).ToLocal(&builtin)) {
        builtin = v8::Undefined(isolate);
    }
    v8::Local<v8::Value> console;
    if (runFactory(context, kConsoleSource, write, builtin).ToLocal(&console)) {
        std::ignore = context->Global()->DefineOwnProperty(context, key, console, v8::DontEnum);
    }
}

void installWebGlobals(const v8::Local<v8::Context> context) {
    const auto isolate = v8::Isolate::GetCurrent();
    const v8::HandleScope scope(isolate);
    ConvertCtx cx{isolate, context};
    const auto natives = Object::create(cx);
    const auto origin = Clock::now();
    std::ignore = natives.set(
        "now",
        callable(
            [origin] {
                return std::chrono::duration<double, std::milli>(Clock::now() - origin).count();
            },
            "now"));
    std::ignore = natives.set("timeOrigin", std::chrono::duration<double, std::milli>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count());
    std::ignore = natives.set("reportError", callable(
                                                 [](const CallInfo& call, const Value& error) {
                                                     const auto text =
                                                         "Uncaught " +
                                                         describeError(call.mIsolate, call.mContext,
                                                                       error.local(call.mIsolate));
                                                     if (auto* state = stateOf(call.mIsolate)) {
                                                         state->report(text);
                                                     }
                                                 },
                                                 "reportError"));
    std::ignore = natives.set("clone", callable(&cloneValue, "clone"));
    std::ignore = natives.set("encodeUtf8",
                              callable(
                                  [](const CallInfo& call, const std::string& text) {
                                      auto bytes = BufferView::allocate(call.mIsolate, text.size());
                                      if (!text.empty()) {
                                          std::memcpy(bytes.data(), text.data(), text.size());
                                      }
                                      return bytes;
                                  },
                                  "encodeUtf8"));
    std::ignore = natives.set(
        "decodeUtf8",
        callable(
            [](const CallInfo& call, const SharedBufferView& bytes, const bool fatal) -> Value {
                const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
                if (fatal && !isValidUtf8(data, bytes.size())) {
                    Detail::throwTypeError(call.mIsolate, "The encoded data was not valid utf-8");
                    return {};
                }
                ConvertCtx inner{call.mIsolate, call.mContext};
                return {inner, Detail::utf8ToJs(
                                   inner, std::string_view(reinterpret_cast<const char*>(data),
                                                           bytes.size()))};
            },
            "decodeUtf8"));
    std::ignore = natives.set("unrefTimer", callable(
                                                [](const CallInfo& call, const double id) {
                                                    if (auto* state = stateOf(call.mIsolate)) {
                                                        if (const auto it = state->mTimers.find(
                                                                static_cast<std::uint32_t>(id));
                                                            it != state->mTimers.end()) {
                                                            it->second.mRef = false;
                                                        }
                                                    }
                                                },
                                                "unrefTimer"));
    std::ignore =
        runFactory(context, kWebSource, context->Global().As<v8::Value>(), natives.local(isolate));
}

} // namespace Alka::EngineDetail
