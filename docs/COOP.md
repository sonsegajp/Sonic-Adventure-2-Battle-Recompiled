# Online co-op

Two players play the story together, each on their own computer and screen. Work in progress: this
page grows with each step.

| Step | What | State |
|---|---|---|
| 1 | Connecting (launcher CO-OP page, in-game F1 menu), Discord Rich Presence | done |
| 2 | Your partner in your stage: P1 plays the story's character, P2 the other side's | next |
| 3 | Shared rings, enemies and item boxes: gone for both when one takes them, back after 10 s | |
| 4 | Both must reach the goal before going on; treasure hunts: either can find any piece | |
| 5 | Character colours: presets and a picker | |

## The rules

- **P1 (the host) plays the story's character**: Sonic, Tails or Knuckles in the Hero story, Shadow,
  Eggman or Rouge in the Dark story. **P2 plays the other side's**: Sonic ↔ Shadow, Tails ↔ Eggman,
  Knuckles ↔ Rouge. The host chooses the stages.
- Rings, enemies and item boxes are shared (one player takes them, they are gone for both) and come
  back after 10 seconds. Each player keeps their own ring count, lives and score.
- Both players must reach the goal before the story goes on.
- In treasure hunts, either player can find any piece (shard, key, emerald) at any time; it counts
  for both.
- **Co-op has its own save** (`saves/coop/MemoryCardA.raw`), apart from the main game's. Stages
  cleared together count in both players' co-op saves.

## Connecting

Either in the launcher - **CO-OP**: type your name, then **Host a game** (it shows a code to send to
your partner) or **Join with a code**; once you see each other, **PLAY** - or in the game: **F1**
opens the menu bar, **Co-op** → *Host a game* / *Join*. A game started without co-op starts again as
a co-op game when you host or join from F1 (so the co-op save is used, never the main one).

Nothing needs opening on your router: both games connect *out* to a relay server, which pairs them
by the code and passes their messages on.

### The relay

The Odyssey master server's UDP relay (`198.71.52.82`, UDP port 8901; another can be set in
`coop.ini`: `relay=host:port`). A peer registers with the datagram `ODYNPU HOST SA2B-<code>` or
`ODYNPU JOIN SA2B-<code>` (resent every 10 s as a keepalive; the relay answers `ODYRDY` and forgets
a peer after 60 s of silence); every other datagram from a registered peer goes to the other one.

### Our messages

Datagrams of `SA2C`, a type byte and a payload (`src/net/coopnet.cpp`):

| Type | Name | Payload |
|---|---|---|
| 1 | HELLO | protocol version (16-bit), role (0 host, 1 joiner), name length, name (UTF-8, up to 24 bytes) |
| 2 | PING | sender's clock (64-bit µs) |
| 3 | PONG | the PING's clock, echoed |
| 4 | BYE | - |
| 16+ | game | positions, events (step 2 on) |

HELLO is resent twice a second until the partner's arrives; a partner with another protocol version
is refused. With nothing from the partner for 10 seconds the link reports them as not responding and
keeps trying.

Codes are six characters from `ABCDEFGHJKLMNPQRSTUVWXYZ23456789` (no 0/O or 1/I to mix up).

## The F1 menu

Dear ImGui (as Aurora draws it), a bar along the top: **Co-op** (host, join, status, ping, your
code with a Copy button, leave), **Player** (your name; colours with step 5), **Discord** (Rich
Presence on/off, its status, the application id). Notices - partner joined, left, not responding -
show in the corner even while the menu is closed.

## Discord Rich Presence

Shows the stage and character (or the menus, the Chao World), *Single player* or *Co-op with
<partner>* with a party of 2, and the time played. It talks to the Discord app over its local pipe
(`\\.\pipe\discord-ipc-N`), no SDK. It needs the id of a Discord application, set in F1 → Discord
(stored in `coop.ini` as `discord_app_id`); the picture keys it uses are `logo` and, for the
character, `sonic`, `shadow`, `tails`, `eggman`, `knuckles`, `rouge`, `amy`, `metalsonic`, `chao`,
`darkchao`, `tikal`, `chaos` - images uploaded under those names in the application's Rich
Presence assets.

## Files

| File | What |
|---|---|
| `coop.ini` | name, relay, Discord on/off and application id, the last code joined (game and launcher) |
| `saves/coop/MemoryCardA.raw` | the co-op save |
| `sa2b.log` | `[coop]` lines: connecting, partner, ping |
