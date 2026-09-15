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
| `cmd_fs` | `register_fs(&cfg)` | `fs ls` `df` `stat` `mkdir` `rmdir` `rm` `mv` `cat` `hexdump` `sha256` `bench`, and `put`/`get` over XMODEM-1K |

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

## Moving files: `tools/fs_xfer.py`

The host side of `fs put`/`fs get`. It needs only pyserial; the ESP-IDF Python
environment has it.

```sh
python tools/fs_xfer.py -p /dev/cu.wchusbserial... put data/*.mov        # upload, then verify SHA-256
python tools/fs_xfer.py put -f --to clips data/leia.mjpeg.mov           # replace, into a directory
python tools/fs_xfer.py get leia.mjpeg.mov /tmp/leia.mov                 # download
python tools/fs_xfer.py sha256 leia.mjpeg.mov                            # hash on the board
python tools/fs_xfer.py run "fs ls" "fs df"                              # any console command
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
