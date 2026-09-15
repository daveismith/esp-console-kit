# esp-console-kit

`esp_console` commands shared between ESP-IDF projects. Each directory is one
IDF component. Needs ESP-IDF 6.0 or later.

| Component | Register with | Commands |
|---|---|---|
| `cmd_system` | `register_system_common()`, `register_gpio(table, n)`, `register_system_deep_sleep()`, `register_system_light_sleep()` | `version` `restart` `free` `heap` `membench` `flash-stats` `tasks` `top` `log_level` `gpio` `deep_sleep` `light_sleep` |
| `cmd_wifi` | `register_wifi()`, `wifi_known_register_commands()` + `wifi_known_start()` | `join` `wifi_txpower` `wifi_link` `wifi_ps`, and saved networks: `wifi [on\|off]` `wifi_save` `wifi_forget` `wifi_known` |
| `cmd_network` | `register_network_commands()` | `ip addr` `ping` `iperf` `traceroute` `dig` |
| `cmd_nvs` | `register_nvs()` | `nvs_set` `nvs_get` `nvs_erase` `nvs_erase_namespace` `nvs_namespace` `nvs_list` |
| `cmd_i2c` | `register_i2ctools()` | `i2cconfig` `i2cdetect` `i2cget` `i2cset` `i2cdump` |
| `cmd_fs` | `register_fs(&cfg)`; `register_ota(uart)` and `ota_confirm_running()` | `fs ls` `df` `stat` `mkdir` `rmdir` `rm` `mv` `cat` `hexdump` `sha256` `bench`, and `put`/`get` over XMODEM-1K; `ota` (the app slots) and `ota put` (a new image over XMODEM-1K) |
| `servo` | `servo_attach_pca9685()` / `servo_attach_gpio()`, then `register_servo(attach_fn)` | `servo_list` `servo_register` `servo_move` `servo_sweep` `servo_config` `servo_off` |
| `holo` | `holo_start(holos, n, &cfg)`, `holo_register_command()` | `holo`: `center` `move` `nudge` `twitch` `wag` `nod` `scan` `circle` `stop` `led` `leia` `off` `endpoints` |

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
  the application. It needs NVS and the default event loop before
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
- `servo` drives hobby servos on PCA9685 boards over I2C, or on the chip's own pins
  with MCPWM (one timer each, so six on an S3).
  - Each servo has an absolute pulse range it is never driven outside, and a working
    range, with invert, that percentages map onto.
  - Each also has a drive policy: hold, or go limp once settled, per zone.
  - The working range and policy are saved in NVS (namespace `servo`) under the board
    address and channel, or the pin, so they follow the wiring.
  - The application supplies the attach function, since only it knows what is fitted.
  - `servo.h` is the C API; `docs/servo_model_spec.md` describes the model behind it.
- `holo` moves a holoprojector's two servo axes, plus an optional light, on one task
  at the 20 ms servo frame. The motions are twitch, wag, nod, scan, circle, and move
  or nudge to a point.
  - By default the axes are servo idents moved through `servo.h`.
  - An application with its own servo registry or an arm switch passes hooks in
    `holo_config_t` instead.
  - With a single holo, `holo <verb>` works without naming it.

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
