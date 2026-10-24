host.print("== Alka demo ==");

const p = new game.Player("conan", 3);
host.print(p.title());
p.update(5);
host.print("hp after update: " + p.hp);

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
host.print(game.describeAny(b));
host.print(game.ai.think(b));

class Slime extends game.Entity {
    describe() {
        return "a wobbly " + super.describe();
    }
}

host.print(game.describeAny(new Slime("slime")));

try {
    game.forbidden();
} catch (e) {
    host.print(e.name + " (instanceof game.GameError: " + (e instanceof game.GameError) + "): " + e.message);
}

game.slowSquare(7).then(v => host.print("slowSquare(7) = " + v));
game.fetchGreeting("demo").then(t => host.print("fetchGreeting: " + t));

host.print("VERSION = " + game.VERSION);
host.print("== done ==");
