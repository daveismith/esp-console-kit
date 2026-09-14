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
