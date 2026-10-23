# Online co-op

Two players play the story together, each on their own computer and screen, each seeing the other in
their stage.

## The rules

- **The host plays the story's character**: Sonic, Tails or Knuckles in the Hero story, Shadow,
  Eggman or Rouge in the Dark story. **The joiner plays the other side's**: Sonic ↔ Shadow,
  Tails ↔ Eggman, Knuckles ↔ Rouge (and their walkers).
- **The host drives**: the menus, the stage select and the cutscenes (only the host can skip them)
  follow the host's buttons in both games; the joiner's own buttons count only while playing a stage.
- **Rings, enemies and item boxes** come back 10 seconds after they are taken. Each player keeps
  their own ring count, lives and score.
- **Hitting your partner** (a spin or an attack) knocks them back - no rings lost, never a death.
- **Both must reach the goal** before the story goes on: the first one there waits (timer stopped,
  out of harm's way, "Waiting for <partner>" on screen), then both get their results together and
  pose side by side.
- **Treasure hunts**: both hunt the same pieces (the host's game hides them); a piece found by
  either player is found for both.
- **Bosses**: one boss for both - its health is the same in both games; a hit in either game counts
  in both, and a boss beaten in one game is beaten in the other.
- **Rival battles** (Sonic vs Shadow, Tails vs Eggman, Knuckles vs Rouge): both of you fight the
  story's rival, played by the computer and shown in grey; its health is shared like a boss's.
- **The last boss**: the host plays Super Sonic, the joiner Super Shadow, each with their own rings;
  no swapping between them.
- **Co-op has its own save** (`saves/coop/MemoryCardA.raw`), apart from the main game's. Stages
  cleared together count in both players' co-op saves.

The kart races, the Chao World and 2P battle play as they do alone.

## Connecting

Either in the launcher - **CO-OP**: type your name, then **Host a game** (it shows a code to send to
your partner) or **Join with a code**; once you see each other, **PLAY** - or in the game: **F1**
opens the menu bar, **Co-op** → *Host a game* / *Join*. A game started without co-op starts again as
a co-op game when you host or join from F1 (so the co-op save is used, never the main one).

Nothing needs opening on your router: both games connect *out* to a relay server, which pairs them
by the code and passes their messages on.

## In the game

- **F1** opens the menu bar: **Co-op** (host, join, status, ping, your code with a Copy button,
  leave), **Player** (your name and colours), **Discord** (Rich Presence on/off, its status, the
  application id). Notices - partner joined, left, not responding - show in the corner even while the
  menu is closed.
- **T** opens the chat: type, **Enter** sends, **Esc** closes. Names show in the colour of each
  player's character; a short chime plays as a line is sent or arrives. Your character stands still
  while you type.
- **Name tags**: your partner's name floats above their head, in their character's colour.
- **Colours** (F1 → Player): Sonic's blue, his jump ball and trails, Shadow's red stripes, his jump
  ball and trails, Tails' fur, Eggman's suit, Knuckles' red, Rouge's white fur, the Cyclone's blue
  and yellow, the Egg Walker's gray and red - each as the game has it, or one of the presets, or any
  colour from the picker. The area is made white with its shading kept, then tinted. Your partner
  sees your colours; they also apply when you play alone.

## Discord Rich Presence

Shows the stage and character (or the menus, the Chao World), *Single player* or *Co-op with
<partner>* with a party of 2, and the time played. It talks to the Discord app over its local pipe
(`\\.\pipe\discord-ipc-N`), no SDK. It needs the id of a Discord application, set in F1 → Discord
(stored in `coop.ini` as `discord_app_id`); the picture keys it uses are `logo` and, for the
character, `sonic`, `shadow`, `tails`, `eggman`, `knuckles`, `rouge`, `amy`, `metalsonic`, `chao`,
`darkchao`, `tikal`, `chaos` - images uploaded under those names in the application's Rich
Presence assets.

## How it works

`src/runtime/coopgame.cpp` (the stage side), `coopcolour.cpp` (colours), `coopmenu.cpp` (the F1
menu, chat, name tags), `src/net/coopnet.cpp` (the link).

- The partner is a second player in your game (`LoadCharacters` loads their character too, without
  2P mode). Every frame each game sends its player (position, turn, speed, its whole animation state
  and the controller as the character read it); your game runs the partner's character with that
  controller - so it rides its board, makes its effects and plays its voice itself - and then sets
  it to the partner's position and pose to draw it. A small buffer (two frames) evens out the
  network.
- Outside play the joiner's game reads the host's controller instead of its own, frame by frame and
  screen by screen (a press waits until the joiner's game shows the same menu or cutscene).
- The goal: `AwardWin` ends a stage for a player; in co-op the first to finish is held until the
  partner has too. Treasure: the emerald manager's hidden pieces are the host's; `GotPiece` marks a
  find, which goes to the other game. Bosses: each boss's health gauge is made from a pointer to its
  health, which both games keep at the lower of the two.
- Colours repaint the characters' textures in memory (they are CMPR: two colours a 4x4 block); where
  two players share a character's textures (a rival battle) they are painted as each is drawn.

### The relay

The Odyssey master server's UDP relay (`198.71.52.82`, UDP port 8901; another can be set in
`coop.ini`: `relay=host:port`). A peer registers with the datagram `ODYNPU HOST SA2B-<code>` or
`ODYNPU JOIN SA2B-<code>` (resent every 10 s as a keepalive; the relay answers `ODYRDY` and forgets
a peer after 60 s of silence); every other datagram from a registered peer goes to the other one.

### Our messages

Datagrams of `SA2C`, a type byte and a payload:

| Type | Name | Payload |
|---|---|---|
| 1 | HELLO | protocol version (16-bit), role (0 host, 1 joiner), name length, name (UTF-8, up to 24 bytes) |
| 2 | PING | sender's clock (64-bit µs) |
| 3 | PONG | the PING's clock, echoed |
| 4 | BYE | - |
| 16 | PLAYER | the sender's player this frame: stage, character, action, status, position, turn, speed, controller, animation |
| 17 | PAD | the host's controller, its last four frames and the screen each was on |
| 18 | HIT | the sender hit you (where from) |
| 19 | FINISHED | the sender reached the goal of a stage |
| 20 | TREASURE | the hunt: the host's hidden pieces, which are found |
| 21 | COLOURS | the sender's colours |
| 22 | BOSS | the boss's health in the sender's game |
| 23 | CHAT | a chat line: name, its colour, text |

HELLO is resent twice a second until the partner's arrives; a partner with another protocol version
is refused. With nothing from the partner for 10 seconds the link reports them as not responding and
keeps trying. Codes are six characters from `ABCDEFGHJKLMNPQRSTUVWXYZ23456789` (no 0/O or 1/I to mix
up).

## Files

| File | What |
|---|---|
| `coop.ini` | name, colours, relay, Discord on/off and application id, the last code joined (game and launcher) |
| `saves/coop/MemoryCardA.raw` | the co-op save |
| `sa2b.log` | `[coop]` and `[chat]` lines: connecting, partner, ping, stages, finds, the boss |
