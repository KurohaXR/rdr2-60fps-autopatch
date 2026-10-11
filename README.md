# RDR2 60fps Autopatch (PS5)

60 FPS for Red Dead Redemption 2 (PS4, `CUSA03041` — versions 1.00, 1.29 and
1.32) on a jailbroken PS5, without installing any modded PKG and without touching
the disk: the patch is applied in memory each time the game boots. Nothing
persists, so shutting the console down leaves it clean.

Made by **KurohaXR**.

A note on who I am: I'm not a scene developer. I'm a user who started
reviewing kernel logs to track down a repeated crash and see whether a fix was
possible. I could not determine the exact cause of the bug in the update package
that includes the patch, so instead of fixing that package, I worked around it:
a clean install plus a 3-byte runtime patch for the one game that kept crashing
my console.

That also sets the scope: this was built specifically for RDR2. Porting it
elsewhere is not copy-paste work — it requires reading that game's own bytes,
locating its own framerate lock and verifying it live, exactly as done here. Do
not expect updates for other titles or versions. This repository does one job.

This project is a small fix, nothing more. I respect and admire the work of all
the real developers contributing to the scene — exploit researchers, tool authors,
patch makers. This exists only because their work exists first.

## The problem (summary)

An update package for 1.32 that includes the 60fps patch causes a reproducible
crash on jailbroken PS5s: install it, reboot, jailbreak again, and roughly 90
seconds later the console reboots on its own — without the game ever being launched.

## How it was found

The pattern was consistent for weeks: install RDR2 with the 60fps patch, play for
hours without a single issue, shut down — and the next jailbreak would kernel
panic about 90 seconds in. No game launched, no unusual payloads loaded. Deleting
the game restored stability every time; reinstalling brought the crashes back.

The striking part was that the game itself ran flawlessly. A defective patch would
be expected to crash during play, not idle in the menu the next day. That pointed
away from game code, so I started capturing the kernel log with klogsrv, before
and after reboot. The same sequence showed up all three times:

```
GetStorageInfo(...REDEMPTION...) -> end OK
out_of_range 'invalid string position' in SceAppInstallerJobQueue
SceShellCore dies -> coredump -> reboot
```

Neither the exploit, nor kstuff, nor anything I loaded: the system kills itself
while scanning that update package's data during the post-boot background scan.
The base game alone scans clean. A clean update scans clean. Only the package
that includes the patch trips it. Three out of three captures, identical signature.

Ruling alternatives out took most of the effort: a base-only install stays stable
across reboots, a clean 1.32 update stays stable, and stock firmware (no jailbreak)
with the mod stays stable for hours. It takes both conditions at once — jailbroken
state plus that update package — to trigger it. Deleting the game fixes it
because the scan simply skips a record that no longer exists (which also explains
why the freed space never quite added up: the uninstaller leaves stubs behind).

### Why not from ItemzFlow

That was tried first, naturally. ItemzFlow lists Jao's patch, allows enabling it,
reports it as matching 1.32 — and then nothing changes. Still 30fps.

The code explains why: ItemzFlow only lists, toggles and launches. Whatever writes
bytes into memory is a separate component — on PS4 it's GoldHEN's patch plugin; on
PS5 it's the etaHEN/ItemzFlow "xml patches" plugin. On my console that engine was
inactive: the toggle file on disk stayed at `0`, etaHEN's log showed no trace of it,
and reading the game's live memory showed the original bytes untouched.

Chasing that further would have meant working through etaHEN versions, toolbox
states and plugin reinstalls. The direct approach already had a verified result —
writing the 3 bytes through ps5debug works on the first attempt — so ItemzFlow was
set aside in favor of a dedicated daemon. Fewer moving parts, no ambiguity about
what writes what.

## The fix

Don't install the mod. Play on a **clean** install and unlock the framerate live:

| version | Address (`eboot+X`) | original | patch   |
|---------|---------------------|----------|---------|
| 01.00   | `0x04A8EE1F`        | `0f 44 f0` | `31 f6 90` |
| 01.29   | `0x0578AB59`        | `0f 44 f0` | `31 f6 90` |
| 01.32   | `0x05853029`        | `0f 44 f0` | `31 f6 90` |

The 1.32 *Unlock FPS* patch is by **Jao**, from
[`illusionyy/PS-Game-Patch`](https://github.com/illusionyy/PS-Game-Patch)
(file `patches/xml/RedDeadRedemption2-Orbis.xml` upstream — not vendored here).
The 1.00 and 1.29 rows use the same site, verified against the real eboots of
those versions (on 1.29, illusion's published address points at the `test` two
bytes before the target, a 5-byte overwrite; this tool overwrites only the
3-byte `cmove` with an equivalent result).

- Applier: `autopatch60-oneshot.elf`, which waits for RDR2's eboot, **detects
  the running version** (64-byte context gate per row — memory decides, not
  labels), **verifies the original bytes**, writes the patch, re-verifies,
  notifies (`RDR2 60fps ON v01.xx - By KurohaXR`) and **exits**. Nothing
  resident, no traffic after patching. If anything doesn't match, it writes
  nothing.
- One subtlety found through testing: the write has to land within the first
  seconds of the process's life (the game selects its video mode once at boot).
  The one-shot handles that by itself: it waits for the spawn and writes within
  seconds, before the game reads the value.

## Requirements

- Jailbroken PS5 with `elfldr` (port 9021).
- [`ps5debug-NG`](https://github.com/OpenSourcereR-dev/ps5debug-NG) v1.3.0
  (server on port 744; the autopatcher is its client on `127.0.0.1`).
- RDR2 (PS4) `CUSA03041` clean install: **1.00** (base), **1.29** or **1.32**
  (base + unmodded update).
- A Payload Manager with autoload (recommended) or manual loading of 2 ELFs.

## How to Use

1. After jailbreaking, load `ps5debug-NG.elf` (once, autoload-friendly) and then
   `autopatch60-oneshot.elf` manually once per play session (Payload Manager).
   On load you get a `RDR2: waiting for game` notification (if `ps5debug-NG` is
   missing you get `ps5debug-NG not found or not running` instead).
2. Launch RDR2 whenever convenient (no time pressure: it waits up to 10 minutes,
   then exits quietly if no game appears).
3. The `RDR2 60fps ON v01.xx - By KurohaXR` notification appears — play at 60fps.
4. When finished, close the game normally. Nothing remains (the patcher already
   exited; the patch lived only in the dead game's RAM).

A resident variant (patching continuously while the game ran) was tested during
development and then discarded: a kernel trap (`Fatal trap 12: page fault in
kernel mode`) was captured exactly at game close with it active. Correlation,
not proof — but combined with the one-shot's clean closes across all trials,
continuous debugger traffic during teardown is the leading suspect. This repo
ships one-shot only.

> ⚠️ Don't run `klogsrv` at the same time as `ps5debug-NG` (both listen on port 3232).

## Test results (PS5 FAT FW 12.00, real hardware)

- **v1.32 (clean update): 5/5 cold boots at 60fps**, playing across map areas
  and saving in different spots. A save-position dependence theory was proposed
  and did not hold up. Relaunching without loading the patcher gives 30fps, as
  expected — nothing persists.
- **v1.00 (base) and v1.29 (update) validated in-game** with the one-shot:
  version-tagged notification (`v01.00` / `v01.29`) and 60fps confirmed.
- Closing the game (Home → Options → Close) is clean with one-shot across all trials.
- With the **resident** daemon, one kernel trap (`Fatal trap 12: page fault in
  kernel mode`) was captured exactly at game close. Correlation, not proof — but
  combined with the one-shot's clean closes, continuous debugger traffic during
  teardown is the leading suspect, so only the one-shot ships here.
- 1 KP + power-off in 6 boots overall (undetermined cause, possibly other payloads
  in the chain — not attributed to either flavor without evidence).
- Rest mode → wake → relaunch: no issues.
- **Do NOT use the Common-FPS overlay for measuring on FW 12.00**: reloading it
  froze the console with a system error (no reboot). It is validated only on FW
  9.60, and it hooks ShellUI render paths with a documented history of kernel
  panics. FPS figures gathered during testing came from it before this was known —
  interpret them accordingly.

## Building (once, on a PC with Linux/WSL)

```bash
sudo apt install clang-18 lld-18 make git wget unzip
# binary SDK:
sudo mkdir -p /opt && cd /tmp
wget https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip
sudo unzip -d /opt ps5-payload-sdk.zip
export LLVM_CONFIG=/usr/bin/llvm-config-18   # if present; else: sudo apt install llvm-18-dev
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cd src && make        # -> autopatch60-oneshot.elf
```

## Disclaimer

Experimental software, no warranty of any kind. Provided "as is". Use it at your
own risk: it could crash the game (it should never touch the system, but nothing
is promised). The author accepts no responsibility for lost data, corrupted saves
or bans. If something breaks, restart the game; if the console misbehaves, reboot
it (nothing persists).

## Credits

- Made by **KurohaXR**.
- 1.32 *Unlock FPS* patch: **Jao**; patch repo
  [`illusionyy/PS-Game-Patch`](https://github.com/illusionyy/PS-Game-Patch) (**illusion**).
  The 1.00 / 1.29 rows use the same site, verified against those versions' real
  eboots (1.29 cross-checked against illusion's published address).
- [`ps5debug-NG`](https://github.com/OpenSourcereR-dev/ps5debug-NG) (**OpenSourcereR** et al.)
  + [`ps5-payload-sdk`](https://github.com/ps5-payload-dev/sdk),
  [`elfldr`](https://github.com/ps5-payload-dev/elfldr),
  `klogsrv`, `ftpsrv` (**John Törnblom** / ps5-payload-dev) — without these tools
  (and their authors) none of this would exist.
- Patch-format references: GoldHEN Cheat Repository, PS4 Cheats Manager (**bucanero**),
  ItemzFlow (**LightningMods**).

## License note

This repo's code is MIT. Building links against
[`ps5-payload-sdk`](https://github.com/ps5-payload-dev/sdk) (GPL-3); since the
full source is published here, distributed binaries comply as long as this same
source goes with them.

