# fpp-kfmt

An FPP plugin that drives a KulpLights K-FMT FM transmitter — a **QN8027**
reached over a **CP2112** USB-HID-to-I2C bridge, or over native I2C if one is
present. It broadcasts whatever audio FPP plays and sends RDS (PS, RadioText
and RT+) alongside it.

## Layout

| File | What it is |
|---|---|
| `src/FPPKFMTPlugin.cpp` | The plugin: worker thread, RDS scheduling, settings, HTTP API |
| `src/QN8027.cpp/.h` | Chip driver — registers, tuning, RDS sends |
| `src/CP2112.cpp/.h` | USB HID bridge; presents a byte-register interface |
| `src/RDSPacketBuilder.cpp/.h` | Builds RDS group payloads |
| `settings.json` | Setting metadata: types, ranges, tooltips, Advanced gating |
| `plugin_setup.php` | Settings page, status panel, station-ID preview |
| `scripts/fpp_install.sh`, `fpp_uninstall.sh` | Lifecycle (build in place) |
| `callbacks.sh` | **Must stay in the repo root** — see below |

## Architecture

**One worker thread owns the transmitter.** Playlist and media callbacks,
setting changes and HTTP handlers all put work on a queue; nothing else talks
to the chip. That keeps slow I2C off fppd's main loop and means two exchanges
can never overlap.

Two rules follow from that, and both have been violated here before:

- **A queued job must never reach the caller's stack.** A wait that times out
  does *not* cancel the job — it stays queued and runs later. Jobs write into
  the shared `RadioJob` they are handed and capture only `this`. Capturing by
  reference caused a use-after-free that crashed fppd.
- **Don't call into the queue while holding its lock.** `formatAndSendText()`
  takes the same non-recursive mutex the run loop holds. Calling it from inside
  the loop deadlocks the thread, which silently stops RDS, the status API and
  the reconnect retry together.

`callbacks.sh` stays in the root because FPP looks for it only at
`<plugindir>/<name>/callbacks[.sh|.pl|.php|.py]` — there is no `scripts/`
fallback — and the `c++` it prints for `--list` is what makes FPP load the
shared library at all. Moving it produces a plugin that installs fine and never
loads. `fpp_install.sh` *is* found in `scripts/`.

## Build and compatibility

Builds against FPP's headers (`make`, `SRCDIR` defaults to the FPP source
tree). **FPP 10+ only** — the status API uses `registerPluginApi()` and drogon.
The FPP 9 entry in `pluginInfo.json` is pinned to an older commit with
`allowUpdates: 0`; leave it frozen, or FPP 9 boxes get offered an update that
cannot compile.

On BeagleBone-class hardware (one core, ~480 MB) always build through the
distributed compiler. A plain `make` there thrashes the box, and the distributed
wrapper falls back to local compilation *silently* if it cannot reach its
servers — check that it actually offloaded rather than assuming.

## QN8027 facts worth knowing

These were established against real hardware and the datasheet; several
contradict what the code used to assume.

- **The register map is `00h`–`12h`, plus the undocumented `1Eh` (antenna
  tuning).** Nothing else exists. The vendor reference driver (PixelRadio's
  `QN8027Radio`) defines nothing above `1Eh` either.
- **Reads of absent registers return stale bus data**, not `0xFF` and not a
  NACK. An invented register can therefore look like it is working. A `0x30`
  "PACAP" once lived here and read `0x00` forever, which made an antenna-match
  check pass unconditionally.
- **There is no readable measure of antenna match.** Don't add one back.
  `1Eh` can be displayed, but it is not a verdict.
- **`GPLT` (`0x02`) is `tc[7] / priv_en[6] / t1m_sel[5:4] / gain_txplt[3:0]`**,
  hardware reset value `0xA9`. `t1m_sel` defaults to switching the PA off after
  ~60 s of silence — measured at exactly 60 s. It must be set to `11` (never),
  or the carrier dies a minute into every gap between playlists. The bit
  positions matter: reading `t1m_sel` one bit over is self-consistent enough to
  look right, but would make the fix a no-op *and* enable `priv_en`.
- **Power:** `PAC` 20–75, roughly `0.62 × PAC + 71` dBµV.
- **FSM** (status register, low 3 bits): 2 = Idle, 5 = Transmitting, 6 = PA Off.
- **`audioPeak` is a latched peak-hold**, not a live level. It reads high during
  silence, so it cannot tell you whether audio is flowing. Use the FSM.
- **Observer reads must not reset the bus.** Anything whose result is only
  displayed goes through `read1ByteOptional()`. Resetting the USB bridge for a
  status read costs a full re-init and ~150 ms of dead carrier.

## Is the module good or bad?

1. **Does it answer at all?** `detect()` reads `CID1` (`0x05`) and `CID2`
   (`0x06`). A healthy QN8027 returns **`0x41` / `0x44`**. Use these as a
   canary before trusting any other reading.
2. **Garbage or shifting values from every register** — including the IDs —
   almost always means **something else owns the bus**. fppd holds the device
   whenever the plugin is loaded. *Stop fppd before running any standalone
   probe*, or both sides read nonsense and you will chase a hardware ghost.
3. **Transmitting?** FSM 5 and `SYSTEM` bit 5 (TXREQ) set. `Freq: 0.0 MHz` in
   the log means the chip never came up.
4. **Carrier drops after about a minute of silence** → `t1m_sel` is not set to
   never. Check the log line `GPLT: B9  PA auto-off: never`. If it says
   `timed`, the running code is not the fixed code.

## Conventions

- **URLs in the page and `settings.json` must be relative** (`api/plugin-apis/kfmt`,
  not `/api/...`). A leading slash breaks the FPP proxy and FPPMon, and in
  `optionsURL` it is read as a local file path and kills the page mid-render.
- Settings live in `settings.json` with tooltips and `level: 1` for anything
  fiddly; the page renders them with `PrintSettingGroup`. `PrintSetting` emits
  `id=` and an inline `onChange`, but **no `name=`** — look fields up by id and
  *add* listeners rather than replacing FPP's own.
- Commit messages, comments and release notes carry no host names, IP
  addresses, local paths or show/sequence names. Describe the shape of the
  setup instead.
