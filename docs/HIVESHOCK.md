# HiveShock integration

Ghostship can receive stream effects (gifts, chat, follows...) from the [HiveShock](https://hiveshock.yafel.dev) bridge.
Enable it in **Network > HiveShock** (`gRemote.HiveShock.Enabled`). It starts automatically on launch when enabled and
has no dependency on the Satella/`USE_NETWORKING` option (it is a no-op on Switch / Wii U, which lack BSD sockets).

## Protocol

The bridge opens a short TCP connection to the effects port (default `43000`) and sends one JSON line per effect:

```json
{"action":"goomba","user":"viewer","value":3,"duration":20,"token":"..."}
```

| Field | Meaning |
|---|---|
| `action` | Required. Name from the action table (see below). |
| `user` | Optional viewer name, shown in the log / menu. |
| `value` | Optional, action specific (health wedges, spawn count, coins, shells...). |
| `duration` | Optional, seconds, for timed effects. |
| `token` | Required only if `gRemote.HiveShock.Token` is set. |

The game reports `{"event":"player_death","deaths":N}` back to `127.0.0.1:43002` (best effort).

Effects are queued (max 64) and applied on the game thread, only while Mario is playable; they wait in the queue during
menus, warps and cutscenes.

## CVars

| CVar (`gRemote.HiveShock.*`) | Default | |
|---|---|---|
| `Enabled` | 0 | Master switch |
| `Port` / `EventPort` | 43000 / 43002 | TCP ports |
| `AllowLAN` | 0 | Listen on `0.0.0.0` instead of loopback (effects are unauthenticated unless `Token` is set) |
| `Token` | "" | Shared secret required in every command |
| `MinGapMs` | 0 | Minimum time between executed effects |
| `Cat.<Category>` | 1 | Enable/disable a whole category |

## Actions

Defined in one table, `sActions` in `src/game/hiveshock_effects.c`. The Network menu (toggles + test buttons) is generated
from it, so **adding an effect means writing one function and one table row**.

- **Gameplay**: `banana`, `heal`, `damage`, `kill`, `lives_add`, `coins_add`
- **Enemies**: `goomba`, `goomba_army`, `goomba_huge`, `bobomb`, `chuckya` (relentless chaser), `poison_mushroom`,
  `flyguy`, `amp`, `koopa_shell`, `bowling_ball`, `bowling_rain`, `clear_enemies`
- **Weapons**: `shotgun` (thrown in front of Mario, auto-equips on touch, fire with L), `shotgun_give`, `shotgun_fire`
- **Mario**: `burn`, `shock`, `squish`, `knockback`, `launch`, `ground_pound`, `blow`, `cap_off`
- **Power-ups**: `metal_cap`, `wing_cap`, `vanish_cap`, `low_gravity`, `high_gravity`, `speed_boost`

Only models that are loaded in every level are spawned (see `script_func_global_1` in `levels/scripts.c`); anything from a
per-level actor group would crash in levels that do not load it.

Console: `hs <action> [value] [duration]`, `banana`, `banana_peel`.

## Custom actors

`actors/BananaPeel_N64` and `actors/shotgun` are Fast64 exports compiled directly into the executable
(`hiveshock_banana.c`, `hiveshock_shotgun.c`) and attached with `process_geo_layout`, so no model ID or OTR asset is needed.
