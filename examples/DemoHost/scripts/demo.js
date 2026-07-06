host.print("== Alka demo ==");

// constructing and using bound classes
const p = new game.Player("conan", 3);
host.print(p.title());                       // Lv3 conan
p.update(5);                                 // default-able arg used explicitly
host.print("hp after update: " + p.hp);      // readonly prop

// defaults from arg("level") = 1
const rookie = game.spawn("rookie");
host.print(rookie.title());

// value types and free functions
const v = new game.Vec2(3, 4);
host.print("length = " + game.length(v));

// enums are two-way frozen objects
host.print("Color.Green = " + game.Color.Green + ", name of 5 = " + game.Color[5]);

// containers convert to real arrays
host.print("range: " + game.range(4).join("+"));

// JS classes extend native classes seamlessly
class Boss extends game.Player {
    constructor() {
        super("demon", 99);
        this.rage = 5;
    }
    taunt() {
        return this.title() + "!!! rage=" + this.rage;
    }
}
const b = new Boss();
host.print(b.taunt());
host.print("instanceof Entity: " + (b instanceof game.Entity));
host.print(game.describeAny(b));             // JS subclass passes into C++
host.print(game.ai.think(b));                // submodule

host.print("VERSION = " + game.VERSION);
host.print("== done ==");
