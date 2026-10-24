'use strict';

const assert = require('node:assert/strict');
const path = require('node:path');
const {Worker, isMainThread, parentPort, workerData} = require('node:worker_threads');

const addon = require(process.env.ALKA_ADDON || path.join(__dirname, 'alka_node_test.node'));
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function runWorker(data) {
    return new Promise((resolve, reject) => {
        const worker = new Worker(__filename, {workerData: data});
        let result;
        worker.on('message', (value) => {
            result = value;
        });
        worker.once('error', reject);
        worker.once('exit', (code) => (code === 0 ? resolve(result) : reject(new Error(`worker exited ${code}`))));
    });
}

async function main() {
    process.exitCode = 1;

    // bindings, on Node's own V8
    assert.equal(addon.v8Version(), process.versions.v8);
    const counter = new addon.Counter(5);
    assert.equal(counter.increment(), 6);
    assert.equal(counter.value, 6);
    assert.equal(addon.math.twice(21), 42);
    assert.throws(() => new addon.Counter('x'), TypeError);

    if (addon.fastCalls() >= 0 && addon.fastUsable() && process.execArgv.includes('--allow-natives-syntax')) {
        const optimize = new Function('f', 'a', 'b', '%PrepareFunctionForOptimization(f); f(a, b); f(a, b);' +
            ' %OptimizeFunctionOnNextCall(f); return f(a, b);');
        const sum = (a, b) => addon.add(a, b);
        const look = (s) => addon.see(s);
        let before = addon.fastCalls();
        assert.equal(optimize(sum, 2, 3), 5);
        assert.ok(addon.fastCalls() > before, 'add took the fast path');
        assert.throws(() => sum(1.5, 1), /expected integer/);
        optimize(look, 'x');
        before = addon.fastCalls();
        look('café');
        assert.equal(addon.seen(), 'café');
        assert.ok(addon.fastCalls() > before, 'see took the fast path');
        look('你好');  // (two-byte: the regular path)
        assert.equal(addon.seen(), '你好');
        console.log(`fast API: ${addon.fastCalls()} fast calls`);
    }

    if (addon.gc) {
        const gc = addon.gc;
        const before = gc.alive();
        (() => {
            const a = new gc.GcNode(1), b = new gc.GcNode(2);
            a.next = b;
            b.next = a;
            a.peer = b;
            for (let i = 0; i < 50; ++i) new gc.GcNode(i).next = gc.make(i);
        })();
        const kept = new gc.GcNode(9);
        kept.tag = 'mine';
        gc.keep(kept);
        assert.equal(gc.kept(), kept);
        assert.equal(gc.make(3).next, null);
        for (let round = 0; round < 20 && gc.alive() > before + 1; ++round) {
            global.gc();
            await sleep(10);
        }
        assert.equal(gc.alive(), before + 1);
        assert.equal(gc.kept().tag, 'mine');
        (() => {
            const watched = new gc.GcNode(7);
            gc.watch(watched);
            assert.equal(gc.watched(), watched);
        })();
        for (let round = 0; round < 20 && gc.watched() !== null; ++round) {
            global.gc();
            await sleep(10);
        }
        assert.equal(gc.watched(), null);
        // cppgc references in the .d.ts: the class, or null
        assert.match(gc.dts(), /next: (alkaGc\.)?GcNode \| null;/);
        assert.match(gc.dts(), /function watched\(\): (alkaGc\.)?GcNode \| null;/);
        console.log(`cppgc: ok`);
    }

    {
        const s = addon.shapes;
        const grid = new s.Grid();
        (() => {
            'use strict';
            grid[1] = 5;
            grid[9] = 1;
        })();
        assert.equal(grid[1], 5);
        assert.equal(grid[9], undefined);
        assert.deepEqual([...grid], [0, 5, 0, 0]);
        assert.equal(Object.prototype.toString.call(s.Grid), '[object GridClass]');
        const bag = new s.Bag();
        (() => {
            'use strict';
            bag.apple = 3;
        })();
        assert.equal(bag.apple, 3);
        assert.deepEqual(Object.keys(bag), ['apple']);
        assert.equal(delete bag.apple, true);
        assert.equal(bag.apple, undefined);

        class Circle extends s.Shape {
            name() {
                return 'circle';
            }
        }

        assert.equal(s.describe(new Circle()), 'it is a circle');
        assert.equal(s.describe(new s.Shape()), 'it is a shape');
        const bytes = s.fill(4);
        assert.ok(bytes instanceof Uint8Array);
        assert.equal(s.sumBytes(bytes), 28);
        const floats = new Float32Array([1, 2]);
        s.scale(floats, 3);
        assert.deepEqual([...floats], [3, 6]);
        assert.equal(s.area({w: 3, h: 4}), 12);
        assert.throws(() => s.fail(), (e) => e instanceof s.Oops && e instanceof Error && e.message === 'oops');
        const button = new s.Button();
        button.onClick = (x) => x * 10;
        assert.equal(button.click(4), 40);
        const controller = new AbortController();
        assert.equal(s.aborted(controller.signal), false);
        controller.abort();
        assert.equal(s.aborted(controller.signal), true);
        console.log('shapes: ok');
    }

    assert.equal(await addon.doubleLater(21, 100), 42);
    // a coroutine hopping to worker threads and back
    assert.equal(await addon.shout('hi'), 'HI!');
    await assert.rejects(addon.failLater(), /failed off-thread/);

    const before = addon.alive();
    assert.deepEqual(await Promise.all([1, 2, 3].map((n) => runWorker({n}))), [2, 4, 6]);
    assert.equal(addon.alive(), before);
    const doomed = new Worker(__filename, {workerData: {n: 9, hang: true}});
    await new Promise((resolve) => doomed.once('message', resolve));
    await doomed.terminate();
    await sleep(400);
    assert.equal(addon.alive(), before);

    for (let i = 0; i < 200; ++i) new addon.Counter(i);
    for (let round = 0; round < 10 && addon.alive() > before; ++round) {
        global.gc();
        await sleep(10);
    }
    assert.equal(addon.alive(), before);

    addon.doubleLater(1, 200).then((value) => {
        assert.equal(value, 2);
        console.log(`ok (node ${process.version}, V8 ${process.versions.v8})`);
        process.exitCode = 0;
    });
}

if (isMainThread) {
    main().catch((error) => {
        console.error(error);
        process.exitCode = 1;
    });
} else {
    (async () => {
        const counter = new addon.Counter(workerData.n);
        if (workerData.hang) {
            parentPort.postMessage('started');
            await addon.doubleLater(workerData.n, 300);
            return;
        }
        parentPort.postMessage(await addon.doubleLater(counter.value, 20));
    })();
}
