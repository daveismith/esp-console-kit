# esp-console-kit

`esp_console` commands shared between ESP-IDF projects. Each directory is one
IDF component. Needs ESP-IDF 6.0 or later.

| Component | Register with | Commands |
|---|---|---|
| `cmd_system` | `register_system_common()`, `register_gpio(table, n)`, `register_system_deep_sleep()`, `register_system_light_sleep()` | `version` `restart` `free` `heap` `membench` `flash-stats` `tasks` `top` `log_level` `gpio` `deep_sleep` `light_sleep` |
| `cmd_wifi` | `register_wifi()`, `wifi_known_register_commands()` + `wifi_known_start()`; `wifi_ap_start()`/`wifi_ap_stop()` | `join` `wifi_txpower` `wifi_link` `wifi_ps`, and saved networks: `wifi [on\|off]` `wifi_save` `wifi_forget` `wifi_known`; the access point: `wifi ap [on\|off] [--ssid] [--pass]` |
| `cmd_network` | `register_network_commands()` | `ip addr` `ping` `iperf` `traceroute` `dig` |
| `cmd_nvs` | `register_nvs()` | `nvs_set` `nvs_get` `nvs_erase` `nvs_erase_namespace` `nvs_namespace` `nvs_list` |
| `cmd_i2c` | `register_i2ctools()` | `i2cconfig` `i2cdetect` `i2cget` `i2cset` `i2cdump` |
| `cmd_fs` | `register_fs(&cfg)`; `register_ota(uart)` and `ota_confirm_running()`; `ota_core.h`; `fs_ops.h` | `fs ls` `df` `stat` `mkdir` `rmdir` `rm` `mv` `cat` `hexdump` `sha256` `bench`, and `put`/`get` over XMODEM-1K; `ota` (the app slots), `ota put` (a new image over XMODEM-1K), `ota pull` (one from a URL, with `web_server`) and `ota activate` |
| `web_api` | `web_fs_register()`, `web_servo_register(before_move)`, `web_net_register()` | HTTP: `/api/v1/fs*` (the volume: list, stat, upload, download, move, copy, delete); `/api/v1/servos*` (positions, moves, calibration, drive policy); `/api/v1/network*` (the link, known networks, scan, join, the access point) |
| `web_server` | `web_server_start(&cfg)`, `web_register()`, `web_ota_register()`, `web_server_register_commands()` | `web [on\|off]` `web password` `web hostname` `web cors`; HTTP: `/api/v1/info`, `/api/v1/restart`, `/api/v1/openapi.json`, `/api/v1/web`, `/api/v1/events`, and `/api/v1/ota*` |
| `events` | `events_declare()`, `events_changed()`, `events_happened()`; `events_listen()` | none: what changed, for the web server's event stream and any other listener |
| `servo` | `servo_attach_pca9685()` / `servo_attach_gpio()`, then `register_servo(attach_fn)` | `servo_list` `servo_register` `servo_move` `servo_sweep` `servo_config` `servo_off` |
| `holo` | `holo_start(holos, n, &cfg)`, `holo_register_command()`; `holo_motion()`, `holo_status()` | `holo`: `center` `move` `nudge` `twitch` `wag` `nod` `scan` `circle` `stop` `led` `leia` `off` `endpoints` |

Notes:

- `tasks` and `top` need `CONFIG_FREERTOS_USE_TRACE_FACILITY`,
  `CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS` and
  `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`. `flash-stats` needs
  `CONFIG_SPI_FLASH_ENABLE_COUNTERS`.
- `register_gpio()` takes the pins the board owns, so `gpio set` refuses to drive them.
- `cmd_i2c` borrows the I2C master bus the application created on port 0 (see
  `I2C_TOOL_DEFAULT_PORT`). It never tears down a bus it did not create.
- `wifi_known` keeps saved networks in one NVS blob (namespace
  `CONFIG_CMD_WIFI_KNOWN_NVS_NAMESPACE`). It rejoins the last network at boot and
  reconnects when the link drops. `wifi_known_set_hook()` reports link changes to
  the application, and what was asked of the store from any door (enabled, disabled,
  forgotten), so a console command and an application's own UI are heard the same way.
  With a hook installed, a failed join or a lost link logs at INFO rather than WARN: the
  application reports it. It needs NVS and the default event loop before
  `wifi_known_start()`.

- `cmd_fs` works on any mounted VFS volume. `cmd_fs_config_t` gives it the mount
  point, an optional total/used callback for `df` (such as `esp_littlefs_info()`),
  and the UART used for transfers (the console UART by default). Paths are
  relative to the mount point.
- `fs put`/`fs get` take the console UART raw for the transfer. They bypass the VFS
  line-ending translation, mute log output, and can switch baud rate with
  `-b <baud>`. Uploads go to `<path>.part` and are renamed into place only when
  complete. Install the console's UART driver with an RX buffer of at least 2 KB,
  enough for a full 1029-byte block.
- `ota put` receives an application image the same way, into the OTA slot that isn't
  running, writing each block as it arrives.
  - ESP-IDF checks the whole image (header, chip, SHA-256) before it becomes the boot
    image and the board restarts into it.
  - A failed transfer, or a truncated or foreign image, leaves the running image booting.
  - With `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, the new image boots on trial. Call
    `ota_confirm_running()` once the application is up; a reset before then boots the
    previous image.
  - `ota` lists the slots, their versions and states. `ota put -d` is a link test that
    writes nothing.
- `ota_core` is the update session every transport shares: `ota put`, an HTTP upload, a pull.
  One at a time; it writes the slot that isn't running, checks an image's header, chip and
  project on its first 288 bytes, and only a verified image is ever *staged*, then made the boot
  image by `ota_core_activate()`. `ota_core_set_begin_hook()` lets the application stop what
  flash writes would disturb.
- `fs_ops` is what `fs` does, for other callers: list, stat, make, move, copy, delete and hash,
  returning errno values, and a writer that goes through `<name>.part` so a failed write never
  costs the file it replaces. `fs_path()` checks a path from the volume's root (no `..`, names of
  at most 63 bytes). `fs_ops_set_change_hook()` is told before anything is replaced, moved or
  deleted -- by `fs` or the API -- so the application can let go of a file it is using.
- `web_api` puts kit components on the HTTP API, one module at a time: `web_fs_register()` adds
  `/api/v1/fs` over `fs_ops`; uploads, downloads, copies and hashes are long operations.
- `web_server` serves an application's embedded pages (`web_asset_t`, gzipped) and a JSON API.
  Routes are registered with `web_register()` from any component. Changes (PUT, POST, PATCH,
  DELETE) must name the board in `Host` and, for POST and PATCH, be `application/json`, so other
  sites' pages can't drive it; `web password` (or `PATCH /api/v1/web`) adds a password (Bearer
  or Basic), which `WEB_AUTH` routes of any method need -- a GET that reads something private
  too. Query values are URL-decoded. Origins on a CORS allowlist (`CONFIG_WEB_SERVER_CORS_ORIGINS`,
  `https://*.example.com` for subdomains; `web cors` changes it, in NVS) may call the API from a
  browser, preflights answered. mDNS announces `<name>.local`; unknown paths from the access
  point redirect to the page, for phones' captive-portal checks. Needs `espressif/cjson` and
  `espressif/mdns` (managed components).
  - Long operations -- an upload, download, copy, scan -- run with `web_job_start()` on a task
    of their own, so the server keeps answering. One at a time, and none during a firmware
    update: another gets `409 busy`, naming what is running.
  - `/api/v1/web` reads (with the password) and changes the hostname, password and CORS list.
  - `/api/v1/events` streams the `events` component as Server-Sent Events: the handler answers
    with its own head and keeps the socket, and events are written to it later from the
    server's task (`httpd_queue_work()`), without waiting: a client that can't take them is
    closed, and its `EventSource` comes back. `CONFIG_WEB_SERVER_MAX_STREAMS` (3) at once; a
    heartbeat every 15 s. Without `Accept: text/event-stream`, the kept happenings as JSON.
  - `web_ota_register()` adds the `/api/v1/ota` routes over `ota_core`: upload (PUT,
    streamed, a long operation), session state, slots, activate, discard, pull, check.
  - `ota_pull` downloads an image from a URL or a release manifest (`parts[]` with `role:
    "app"`, `path`, `size`, `sha256`), refusing https-to-http redirects. `ota_pull_set_resolver()`
    lets the application turn channel names (`latest`) into manifest URLs. TLS runs on a task of
    `CONFIG_WEB_SERVER_PULL_STACK_SIZE`, never on the server's.
- `events` is what changed on the board, with nothing HTTP in it. A *state* kind (a resource) is
  declared with a builder, the JSON its GET returns, and its owner calls `events_changed()`: a
  listener is only marked, and builds the latest state when it sends, so a burst is one event. A
  *happening* (`events_happened()`) is numbered (`seq`), stamped and kept, the last 32, so a
  listener can catch up with `events_since()`. `web_server` streams them; `web_ota` publishes the
  update session (`ota`), `web_net` the network (`network`), and the web server `system` (uptime,
  memory, signal) every 15 s.
- `wifi_known` also lists the stored networks (`wifi_known_list()`), stores one without joining
  (`wifi_known_save()`), and scans (`wifi_known_scan()`, and `wifi scan` on the console).
- `wifi_known_is_connected()`: whether the station has an address.
- `wifi_ap` runs the board's own access point, on demand and never persisted -- or for a while,
  `wifi_ap_start_for(seconds)`, then off by itself (an app's fallback when it can't join a
  network; `off_in_s` in its info and the API, `off in m:ss` in `wifi ap`): WPA2 with a random
  passphrase kept in NVS (the MAC is the BSSID, so it would be a poor secret), DHCP offering the
  board as DNS and captive portal, and a small DNS responder answering every name with the board.
  `CONFIG_CMD_WIFI_AP_SSID_PREFIX` names it `<prefix>-xxxx`. With `CONFIG_CMD_WIFI_AP_CAPTIVE_DNS`
  off there is no captive portal at all -- no DNS responder and no portal URL in DHCP -- for a
  board with no web page.
- `servo` drives hobby servos on PCA9685 boards over I2C, or on the chip's own pins
  with MCPWM (one timer each, so six on an S3).
  - Each servo has an absolute pulse range it is never driven outside, and a working
    range, with invert, that percentages map onto.
  - Each also has a drive policy: hold, or go limp once settled, per zone.
  - The working range and policy are saved in NVS (namespace `servo`) under the board
    address and channel, or the pin, so they follow the wiring.
  - The application supplies the attach function, since only it knows what is fitted.
  - `servo.h` is the C API; `docs/servo_model_spec.md` describes the model behind it.
    `servo_count()` and `servo_ident()` list the servos, `servo_gpio()` gives a pin's.
- `holo` moves a holoprojector's two servo axes, plus an optional light, on one task
  at the 20 ms servo frame. The motions are twitch, wag, nod, scan, circle, and move
  or nudge to a point.
  - By default the axes are servo idents moved through `servo.h`.
  - An application with its own servo registry or an arm switch passes hooks in
    `holo_config_t` instead.
  - With a single holo, `holo <verb>` works without naming it.
  - `holo_motion()` and `holo_status()` are the console's verbs and status for other callers
    (an HTTP API), with the same ranges and refusals.

## Moving files: `tools/fs_xfer.py`

The host side of `fs put`/`fs get`. It needs only pyserial; the ESP-IDF Python
environment has it.

```sh
python tools/fs_xfer.py -p /dev/cu.wchusbserial... put data/*.mov        # upload, then verify SHA-256
python tools/fs_xfer.py put -f --to clips data/leia.mjpeg.mov           # replace, into a directory
python tools/fs_xfer.py get leia.mjpeg.mov /tmp/leia.mov                 # download
python tools/fs_xfer.py sha256 leia.mjpeg.mov                            # hash on the board
python tools/fs_xfer.py run "fs ls" "fs df"                              # any console command
python tools/fs_xfer.py ota build/holo-player-fw.bin                     # update the firmware, check it runs
```

- **Checking uploads.** `put` sends each file's size, so the board stores exactly that
  many bytes; plain XMODEM would otherwise pad the last block. It then checks the
  SHA-256 twice: once on the bytes the board received, and once on the file read
  back from flash with `fs sha256`.
- **Transfer speed.** Uploads run at `--xfer-baud` (default 460800), writing each block
  in 512-byte pieces and waiting for each to drain (`--chunk`). Downloads run at
  `--get-baud` (default 230400). The console returns to `--baud` afterwards. Uploads
  ran at 21 KB/s on 100 KB, and at 7–10 KB/s on 1.6–1.9 MB video files (2.5–4.5
  minutes each). Downloads ran at about 21 KB/s.
- **Why those rates.** They were measured on macOS with a CH343P bridge and WCH's CH34x
  driver. At 460800 and above, a whole 1029-byte block written in one go loses data
  between the driver and the bridge. Sleeping for each piece's line time didn't stop
  it, and neither did polling the output-queue count; only draining each piece
  (`tcdrain`) held. Draining costs about 35 ms a call, so raising the rate doesn't
  help: 921600 managed only 7.5 KB/s. Data coming from the board above 230400 loses
  bytes whatever the block size. These limits come from the host driver, not the
  protocol, so try higher rates on other hosts.
- **ACK before writing.** The receiver ACKs a block, then writes the one before it while
  the next is arriving.
  - Writing first delayed the ACK by a flash write's ~1.7 ms. At 460800, the CH34x driver
    then dropped the sender's next block whole every few blocks, costing 10 s each.
  - A 1.7 ms busy-wait in place of the write did the same, so the delay is the cause.
  - ACKing first, a 1.26 MB `ota` image went up in 44 s (28 KB/s) with no retries.
  - `-v` reports each resent block, and what arrived instead of its ACK.
- **Resets on open.** The tool opens the port without toggling EN on boards with the
  usual DTR/RTS auto-reset circuit.
- **One program at a time.** Close `idf.py monitor` before running the tool; only one
  program can hold the port.

## Using it

### As a git submodule (source editable in place)

```sh
git submodule add git@github.com:daveismith/esp-console-kit.git external/esp-console-kit
```

In the project's top-level `CMakeLists.txt`, before `include(project.cmake)`:

```cmake
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/external/esp-console-kit")
```

Then add the components you use to `main`'s `REQUIRES` / `PRIV_REQUIRES`. That's
required under `MINIMAL_BUILD`, and good practice otherwise. Clone the project with
`git clone --recursive`, or run `git submodule update --init` after cloning.

Don't put the submodule directly under `components/`. IDF only looks one level
deep there, so it would miss the components nested inside.

### As managed components (pinned by tag)

In `main/idf_component.yml`:

```yaml
dependencies:
  cmd_system:
    git: git@github.com:daveismith/esp-console-kit.git
    path: cmd_system
    version: v0.1.0
```

To work on a component locally, temporarily replace `git`/`version` with
`override_path: ../../esp-console-kit/cmd_system`.

## Origins

- The components started as the ESP-IDF `console_advanced` and `i2c_tools`
  examples (Unlicense / CC0 / Apache-2.0, per file headers). They were ported to
  IDF 6.x and extended in r2_domeplayer.
- `cmd_network` supersedes the standalone `daveismith/cmd_network` repo.
- `wifi_known.c` comes from r2_domeplayer's `panel_net.c`.
- `servo` is r2_domeplayer's `components/servo` (as of `8548fce`), with GPIO servos
  attachable through the C API. `holo` is its `main/panel_holo.c`, with the dome's
  joint registry and arm switch turned into hooks.
