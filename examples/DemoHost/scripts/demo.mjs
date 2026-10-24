import { Player, Vec2, length, slowSquare } from "game";
import { think } from "game.ai";
import * as host from "host";

const hero = new Player("aria", 7);
host.print(hero.title());
host.print("length = " + length(new Vec2(6, 8)));
host.print(think(hero));

host.print("slowSquare(12) = " + await slowSquare(12));
