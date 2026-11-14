# PitBoss on Linux

PitBoss hosts a persistent, simultaneous-turn game without a display, sound device,
or logged-in player. Players connect, play their turns, disconnect and return later.
An offline civilization stays human. Its turn ends when the timer expires; the
operator can explicitly replace it with AI.

This first implementation uses OpenCiv4's own protocol. Both server and clients need
this Linux feature build, the same game-core revision and identical XML/Python rules
and map scripts. Original BtS clients cannot connect. The vanilla BtS 3.19 core and
data mods such as Next War are supported; custom C++ cores remain unsupported.

## Linux prerequisites

The full engine integration depends on [Linux runtime PR #32](https://github.com/bssth/openciv4/pull/32).
[Headless setup and regression PR #40](https://github.com/bssth/openciv4/pull/40)
adds reproducible dependencies, presets, native library naming and Linux CI.
The SDK include/formatting portability fixes are already upstream; the separate
[map-init logging fix](https://github.com/bssth/openciv4-dll/pull/13) changes only
a diagnostic. None of those changes are duplicated in this PitBoss-only proposal.
Until the runtime prerequisite lands, a plain upstream Linux checkout cannot run
the full server/client integration. Windows and macOS PitBoss transports are not
implemented. The complete working fork remains available on
[linux-port](https://github.com/foofly/openciv4/tree/linux-port).

The transport and persistence tests can run directly from this tree, with no SDK,
renderer, Python runtime, game assets or Linux engine port:

```sh
cmake -S tests/pitboss -B build/pitboss-transport -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/pitboss-transport
ctest --test-dir build/pitboss-transport --output-on-failure
```

They require a C++20 compiler, CMake, Ninja and OpenSSL development headers.
The dedicated Linux CI job runs these tests independently of the existing Windows
and macOS builds.

## Start a game

The commands below assume a complete Linux engine build as described above, with
its executable at `build/linux-release/bin/oc4core`. Each machine needs its own
installed game assets. The server uses OpenSSL Crypto at build/runtime, but starting
it does not initialize SDL or Vulkan.

Copy [pitboss.ini.example](pitboss.ini.example) outside the checkout and set `civ4`
to your installation root and `state_dir` to a private directory. Configure the human
seats (zero based), total players, map, size, seed, difficulty and speed before the
first start. `players=0` uses the map size's default. Seat IDs must be below the total
player count. The default timer is 24 hours; `turn_hours=0` disables it. Fractional
hours are accepted, with a minimum nonzero duration of 250 ms.

Set a password for every configured human seat while the server is stopped. The
commands prompt without terminal echo; passwords are not command-line arguments:

```sh
build/linux-release/bin/oc4core --pitboss-password /path/to/private/pitboss-state 0
build/linux-release/bin/oc4core --pitboss-password /path/to/private/pitboss-state 1
build/linux-release/bin/oc4core --pitboss /path/to/pitboss.ini
```

The default listen address is loopback and the default TCP port is 2056. To use a
LAN or VPN, set `listen` to the server's address on that network. Use a trusted LAN
or VPN: this protocol has no TLS, sends full simulation saves to authenticated
players, and provides no protection against cheating by a modified client.

The server resumes its own state on later starts. To create another game, use a
different state directory. Game settings, the original timer configuration and
the ruleset must match the checkpoint. Listen addresses, ports and asset paths can
change when the resolved rules remain identical. Importing unrelated multiplayer
saves and sequential-turn games are not supported.

## Join

Open the menu, choose **Multiplayer → Join a PitBoss server**, then enter the server,
assigned seat and password. Seat numbers are assigned by the operator; joining never
claims an unused civilization. The password is masked and is not saved in the profile.

The command-line equivalent prompts for the password:

```sh
build/linux-release/bin/oc4core --civ4 /path/to/Civ4 \
  --pitboss-join 192.168.1.10:2056 --seat 0
```

For automation, `--password-file /path/to/private/file` reads the first line instead.
Protect that file with mode `0600`. Add the same `--mod` on the client if the server
uses a data mod. A seat can have only one connection. Wrong passwords, incompatible
rules, invalid orders and checksum mismatches close the connection. Interactive
clients return to the PitBoss login page with a reason; reconnecting loads a fresh
server snapshot. There is no automatic retry loop.

## Operate

Administration uses a Unix socket inside the owner-only state directory. No TCP
administration port or separate admin password is exposed. Use the same Linux user:

```sh
build/linux-release/bin/oc4core --pitboss-admin /path/to/private/pitboss-state status
build/linux-release/bin/oc4core --pitboss-admin /path/to/private/pitboss-state pause
build/linux-release/bin/oc4core --pitboss-admin /path/to/private/pitboss-state resume
build/linux-release/bin/oc4core --pitboss-admin /path/to/private/pitboss-state save
build/linux-release/bin/oc4core --pitboss-admin /path/to/private/pitboss-state stop
```

| Command | Effect |
|---|---|
| `status` | Turn, frame, remaining milliseconds, pause flag and connected-seat bitmask. |
| `save` | Write a durable checkpoint immediately. |
| `pause` / `resume` | Freeze/restart the simulation and its remaining timer; persist the change. |
| `timer HOURS` | Change the timer and reset the current turn to that duration; zero is unlimited. |
| `kick SEAT` | Disconnect that player while leaving the civilization human. |
| `password SEAT` | Prompt for a new seat password and disconnect its current player. |
| `replace-ai SEAT` | Permanently replace the human seat with AI and require all clients to reconnect. |
| `motd TEXT` | Persist a short message and show it to current/new clients. |
| `stop` | Save and shut down cleanly. SIGINT/SIGTERM also save. |

`replace-ai` requires a frame boundary with no outstanding orders and refuses to
replace the last human seat. If it reports a pending frame, retry after clients have
caught up. Pause does not disable login or administration. The server continues
running while everyone is offline. Downtime and operator pauses do not consume the
remaining timer. An authenticated join freezes progress during snapshot transfer
and verification for at most 30 seconds; an unresponsive live client is dropped
after five seconds waiting for its frame acknowledgment.

The server advances at four simulation slices per second, matching the SDK's
PitBoss timer units. The displayed timer is updated on these slice boundaries.
Turn duration resets when the game advances to its next turn; if every human ends
early, the game can advance before the timer expires.

For unattended use, edit [the user service example](openciv4-pitboss.service), copy
it into `~/.config/systemd/user/`, then run:

```sh
systemctl --user daemon-reload
systemctl --user enable --now openciv4-pitboss.service
journalctl --user -u openciv4-pitboss.service -f
```

To keep user services running after logout, enable lingering for that account using
your distribution's normal system administration procedure.

## Persistence and compatibility

The private state directory contains salted PBKDF2-HMAC-SHA256 seat verifiers
(600,000 iterations), an exclusive server lock, an admin socket, checkpoints and a
frame journal. Credentials never enter the SDK save or client snapshot. Authentication
uses a fresh nonce and HMAC proof. Password changes take effect immediately.

Each frame's bounded command journal record is checksummed and fsynced before it
is executed and acknowledged. Checkpoints contain the SDK save, Python RNG state,
human roster, command sequences, timer and pause state, plus integrity hashes. A
new generation is written and synced before `CURRENT` is atomically replaced. The
journal is then rotated; records already in that generation are skipped if a crash
happens between the two operations. Checkpoints are made at turn changes, every 240
frames, on persistent admin changes and at shutdown. The previous generation is
kept for inspection, and older generations are removed.

Recovery loads `CURRENT` and replays complete journal records. An incomplete trailing
record is discarded. Corrupt checkpoints, checksum failures and frame gaps fail
startup with a diagnostic; recovery never silently selects an older game. Back up
the whole directory while stopped or paused. A new session epoch rejects packets
from a previous server run. Only acknowledged orders are guaranteed durable; an
order interrupted before acknowledgment may require sending again after reconnect.

Clients receive ordered frames and report an SDK sync checksum every frame. Late
joins wait for existing clients to acknowledge a common boundary before receiving
their snapshot. Commands must match their authenticated actor, reference owned
objects where applicable and satisfy the supported message schemas. Untyped mod
network messages, advanced-start actions, client AI replacement and unknown message
types are rejected. Gameplay legality remains the SDK's responsibility; this is a
trusted-player protocol. There is no public matchmaking, graphical server wizard,
email notification, original-client compatibility or save-format migration in v1.

## Validation

The asset-free `pitboss` CTest exercises authentication, occupied seats, ordered
commands, timer arithmetic, pause/downtime semantics, journal replay after SIGKILL,
torn trailing records, password rotation, kicks, roster changes and shutdown.

Run the integration harness with your own assets:

```sh
python3 tools/validate_pitboss.py --civ4 /path/to/Civ4
python3 tools/validate_pitboss.py --civ4 /path/to/Civ4 \
  --mod 'Mods/Next War' --out build/pitboss-nextwar-validation
```

It starts a real headless server and two SDK clients, sends research/end-turn
orders, checks offline turn expiry, late joins, paused recovery, journal recovery,
password rejection and shutdown. Logs remain in the ignored build directory;
credentials and runtime saves are created in a private temporary directory.
`--pitboss-check SECONDS` with the join options runs a headless diagnostic client
that loads the snapshot, follows frames and verifies checksums for that duration.
It exits unsuccessfully on disconnect or desynchronization.

The complete BtS/Next War two-client runs, graphical login, timed turns during
diplomacy, unchanged 100-turn single-player checksums, and sanitizer checks were
performed on the Linux fork at `aab50c8`. The rebased upstream proposal has separate
Linux transport validation, and its Windows build/CTest passed in
[upstream CI](https://github.com/bssth/openciv4/actions/runs/43587617244).
Those checks do not establish full Linux gameplay or macOS compatibility for this
branch. Full Linux integration on the prerequisite stack and an original-game save
fixture still need validation before this proposal is ready.
