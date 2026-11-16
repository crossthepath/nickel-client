# Scripting: player actions

The complete scripting API is in [scripting/reference.md](scripting/reference.md), with its Luau type definitions in
[scripting/nickel.d.luau](scripting/nickel.d.luau) and checked examples in [scripting/examples](scripting/examples).
This page details the calls that change the local player.

Scripts are Luau files in `%APPDATA%\Nickel\scripts`. Calls that change the local player are requests: they are
checked when called and applied on the next game tick, on the game's thread. A request made while no world is loaded,
or still pending when the world is left, is dropped.

| Call | Accepts | Effect on the next tick |
| --- | --- | --- |
| `player.setVelocity(v)` | a vector or `{x, y, z}` of real numbers, all present, finite, within ±10000 | sets the velocity (blocks per tick) |
| `player.addVelocity(v)` | same | adds to the velocity; several calls in one tick add up, the sum stays within ±10000 |
| `player.jump()` | — | returns whether the player stands on the ground; if so, jumps |
| `player.setSlot(n)` | `1` to `9` | selects that hotbar slot and tells the server; an empty slot is not selected |
| `player.swing()` | — | swings the arm |
| `player.teleport(v)` | a feet position like `player.position()`: x, y and z finite and within ±30,000,000 | returns `true` when queued, or `false` and a reason outside a world; moves the player there and clears the velocity |

An invalid argument raises a Lua error with the reason.

Limits:
- `player.teleport` moves the player locally, like the Look TP module. Servers check movement and may move the player
  back or kick them after a long move; short moves (Look TP uses up to 128 blocks) are the ones known to work.
- `world.time()` returns `nil`: the world time is not read yet.
