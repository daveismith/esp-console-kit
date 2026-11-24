# esp-console-kit

`esp_console` commands shared between ESP-IDF projects. Each directory is one
IDF component. Needs ESP-IDF 6.0 or later.

| Component | Register with | Commands |
|---|---|---|
| `cmd_system` | `register_system_common()`, `register_gpio(table, n)`, `register_system_deep_sleep()`, `register_system_light_sleep()` | `version` `restart` `free` `heap` `membench` `flash-stats` `tasks` `top` `log_level` `gpio` `deep_sleep` `light_sleep` |
| `cmd_wifi` | `register_wifi()`, `wifi_known_register_commands()` + `wifi_known_start()`; `wifi_ap_start()`/`wifi_ap_stop()` | `join` `wifi_txpower` `wifi_link` `wifi_ps`, and saved networks: `wifi [on\|off]` `wifi_save` `wifi_forget` `wifi_known`; the access point: `wifi ap [on\|off] [--ssid] [--pass]` |
| `cmd_network` | `register_network_commands()` | `ip addr` `ping` `iperf` `traceroute` `dig` |
| `cmd_nvs` | `register_nvs()`; `register_nvs_backup(&cfg)` and `nvs_backup_exclude(ns, key, reason)` | `nvs_set` `nvs_get` `nvs_erase` `nvs_erase_namespace` `nvs_namespace` `nvs_list`, and configuration backup: `nvs_inventory` `nvs_export` `nvs_import` |
| `cmd_i2c` | `register_i2ctools()` | `i2cconfig` `i2cdetect` `i2cget` `i2cset` `i2cdump` |
| `i2c_bus_guard` | nothing: require it (`cmd_i2c` and `servo` do on IDF 6.1.0, any target but the original ESP32); `i2c_bus_guard_stale_cleared()` counts what it caught | none: a link-time patch for IDF 6.1.0's `i2c_master` |
| `cmd_fs` | `register_fs(&cfg)`; `register_ota(uart)` and `ota_confirm_running()`; `ota_core.h`; `fs_ops.h` | `fs ls` `df` `stat` `mkdir` `rmdir` `rm` `mv` `cat` `hexdump` `sha256` `bench`, and `put`/`get` over XMODEM-1K; `ota` (the app slots), `ota put` (a new image over XMODEM-1K), `ota pull` (one from a URL, with `web_server`) and `ota activate` |
| `web_api` | `web_fs_register()`, `web_servo_register(before_move)`, `web_net_register()` | HTTP: `/api/v1/fs*` (the volume: list, stat, upload, download, move, copy, delete); `/api/v1/servos*` (positions, moves, calibration, drive policy); `/api/v1/network*` (the link, known networks, scan, join, the access point) |
| `web_server` | `web_server_start(&cfg)`, `web_register()`, `web_ota_register()`, `web_server_register_commands()` | `web [on\|off]` `web password` `web hostname` `web cors`; HTTP: `/api/v1/info`, `/api/v1/restart`, `/api/v1/openapi.json`, `/api/v1/web`, `/api/v1/events`, and `/api/v1/ota*` |
| `events` | `events_declare()`, `events_changed()`, `events_happened()`; `events_listen()` | none: what changed, for the web server's event stream and any other listener |
| `servo` | `servo_attach_pca9685()` / `servo_attach_gpio()`, then `register_servo(attach_fn)` | `servo_list` `servo_register` `servo_move` `servo_sweep` `servo_config` `servo_off` |
| `holo` | `holo_start(holos, n, &cfg)`, `holo_register_command()`; `holo_motion()`, `holo_status()` | `holo`: `center` `move` `nudge` `twitch` `wag` `nod` `scan` `circle` `stop` `led` `leia` `off` `endpoints` |
| `canbus` | `register_can_commands(&cfg)` (it calls `canbus_init()`); `canbus_init()` first from `app_main` for a protocol on top that uses `canbus_start()` and the rest of `canbus.h` without the command | `can`: `start` `stop` `send` `dump` `echo` `recover` |
| `libcanard` | nothing: `cyphal` requires it | none: OpenCyphal's libcanard v4.0.0 (`third_party/libcanard`) packaged as a component |
| `cyphal` | `cyphal_init(&app)`, `cyphal_register_commands(how_to_start)`, then `cyphal_start(tx, rx)`; with `CONFIG_CYPHAL_CLIENT`, `cyphal_call()` and `cyphal_call_wait()` | `cyphal`: `id` `name` `bitrate` `restart` `reset` `register` (and `allocator` `allocations` with `CONFIG_CYPHAL_PNP_ALLOCATOR`) |

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

- `nvs_backup` (in `cmd_nvs`) exports the whole NVS partition as a typed text file with a
  CRC-32 trailer and imports it back, checking the whole file before the first write. It walks
  every namespace, so a new key is backed up with no code change; what stays out is an
  exclusion. The kit excludes IDF's internals and the credentials its own components keep
  (`wifi_known`'s networks, `wifi_ap`, `web/password`); an application adds its own with
  `nvs_backup_exclude()` before `register_nvs_backup()`. A credential is always excluded, by
  whoever adds it. `nvs_inventory` shows each entry's verdict; `nvs_export [file]` prints or
  writes to `nvs_backup_config_t.dir`; `nvs_import <file|url|->` takes a file there, a URL
  through the config's `fetch` hook, or the file on stdin (`stdin_refusal` says when it
  can't), snapshots to `pre-import.r2cfg` first, and wants a restart after.
- `i2c_bus_guard` patches a memory-corruption bug in IDF 6.1.0's `i2c_master`: a read that
  ends in a timeout or lost arbitration (a target stretching SCL too long, a line held low)
  leaves its state on the bus, and the next `i2c_master_probe()` copies RX FIFO bytes through
  a stale pointer into whatever the probing task's stack held -- a write somewhere random,
  then a panic somewhere else. Any firmware that probes (a driver looking for a device that
  may be absent, `i2cdetect`, the servo attach) on a bus where something also reads is
  exposed. The component wraps `i2c_master_probe`, `_receive`, `_transmit_receive` and
  `_execute_defined_operations` at link time (`--wrap`, INTERFACE options, linked whole), so
  every call in the image goes through it and none need change: a probe clears the stale
  state first, and probes and reads share a mutex per bus so no failed read slips in
  between. A bus with a transaction queue is left alone (its reads return once queued, so
  the state may be a read still in flight). It reads the driver's private struct and
  refuses to build against any IDF but 6.1.0, or for the original ESP32; delete it once
  IDF's probe resets that state itself. The kit's own probing components, `cmd_i2c` and
  `servo`, require it on exactly the builds that have the bug (`i2c_bus_guard/wanted.cmake`:
  IDF 6.1.0, not the original ESP32), so a firmware using either is covered with no change
  and builds on any other IDF as before; a newer IDF gets a configure warning to check the
  driver again. They require it only when it is registered, so a project that registers the
  kit's directories one by one must list `i2c_bus_guard` among them, and one that takes
  `cmd_i2c` or `servo` from the component manager on its own must require it itself. Any
  other firmware on 6.1.0 that probes requires it. A firmware that wants a newer IDF to stop
  at the guard's `#error`, rather than build on unguarded, lists it in its own requirements.
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
    heartbeat every 15 s. Without `Accept: text/event-stream`, the kept happenings as JSON, sent
    in chunks as they are stored (`events_open()`, `events_next()`), never parsed back into one
    tree: a few KB of internal RAM at most, however many are kept.
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
  listener can catch up with `events_since()`, or one at a time with `events_open()` and
  `events_next()`, for a reply sent in pieces with the ring unlocked between them. `web_server` streams them; `web_ota` publishes the
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
  - A twitch or a scan with a time limit (`-t`, `time_s`) holds where it is at that time, a
    segment in flight or not. Until 2026-11-23 the limit was looked at only between segments,
    and a scan, which starts its next leg the moment one ends, never ended.
    `tests/host/test_holo.c` checks the end of each, built on the computer against stub headers.

- `canbus` runs one TWAI (CAN) controller, classic frames only, on the ESP32-S3 or the ESP32-P4:
  frames in through a queue the ISR fills, out through a pool it empties, and the error state.
  One owner at a time holds it through a handle (the `can` command, or a protocol such as a
  Cyphal node); anyone may read `canbus_status()`. The application chooses the pins and guards
  them: `can_cmd_config_t.pins` names them and says when something else holds them, `on_pins`
  hears when `can` takes and gives them back (for a `gpio` reserved list), and `print`, if
  given, takes `can dump`'s lines, the only output made from a task other than the console's
  (for a firmware whose global stdout a network console can swap or close). Set
  `CONFIG_TWAI_ISR_CACHE_SAFE`, so a frame arriving during a flash write still reaches the
  queue; `CONFIG_CANBUS_RX_QUEUE_LEN` is the queue, sized against the longest flash stall the
  board sees under a saturated bus. At 1 Mbit/s both targets sample at 85.0 % with 20 quanta a
  bit (S3: 80 MHz APB, prescaler 4; P4: 40 MHz crystal, prescaler 2). `tools/can_check.py` is
  the computer's side: a gs_usb adapter (candleLight, CANable) through python-can, to send,
  listen, flood, or count a board's `can echo` back.

- `cyphal` is a Cyphal/CAN node on `canbus`, on libcanard v4.0.0 (`third_party/libcanard`; v5
  cannot send the anonymous plug-and-play request). It takes a node-ID, fixed or by
  plug-and-play (`uavcan.pnp.NodeIDAllocationData.1.0`, asked for again at every start), sends
  `uavcan.node.Heartbeat` every second and `uavcan.node.port.List` every 10 s, answers
  `uavcan.node.GetInfo`, `uavcan.register.List` and `Access`, and `uavcan.node.ExecuteCommand`,
  and stops, sending nothing more as that ID, when another node turns out to have its node-ID
  (in the listen before going online, or while online).
  - Who the board is comes from Kconfig, pinned in the firmware's `sdkconfig.defaults`: the
    name GetInfo reports (`CONFIG_CYPHAL_DEFAULT_NAME`), the register that holds it
    (`CONFIG_CYPHAL_NAME_REGISTER`), the hardware version, and the unique ID's 8-byte prefix
    (`CONFIG_CYPHAL_UNIQUE_ID_PREFIX`). The unique ID is that prefix, two zero bytes and the
    chip's base MAC, so changing the prefix gives every board a new node-ID from an allocator.
  - What a restart, a factory reset and identify do, and any ports and registers of the
    application's own, are `cyphal_app_t`. A NULL `restart` makes restart and factory reset bad
    commands; a NULL `identify` makes identify one; a NULL `factory_reset` resets the node's
    own settings only.
  - Its settings (node-ID, name, bitrate, each application port's ID) are registers, saved in
    NVS namespace `cyphal` and applied from the node's next start. None is a credential or a
    counter, so a configuration backup keeps them.
  - It has no stop: the application decides whether and on which pins it starts. While it runs
    it is `canbus`'s owner, and `can` only reports.
  - One task runs libcanard (`CONFIG_CYPHAL_TASK_*`, internal stack); a worker (internal stack,
    made at the first start) does what may wait: NVS writes, the restart, the application's
    requests; a second worker only when the application has slow work. libcanard's heap
    (`CONFIG_CYPHAL_HEAP_SIZE`) and the node's state are PSRAM when there is any. Set
    `CONFIG_TWAI_ISR_CACHE_SAFE`, as for `canbus`.
  - `dsdl/` holds the generated C headers of the standard `uavcan` types it and its users need,
    committed so a build needs no Python; `tools/gen_dsdl.py` regenerates them from
    `third_party/public_regulated_data_types` with nunavut 2.3.1 (`--check` compares and changes
    nothing). An application's own types are generated in its own tree.
  - **A plug-and-play allocator, optionally** (`CONFIG_CYPHAL_PNP_ALLOCATOR`, off by default;
    `cyphal_alloc.h`). With the option built in and the run-time setting on (`cyphal allocator
    on`, or the bit register `CONFIG_CYPHAL_ALLOCATOR_REGISTER`; saved, off until set, applied at
    the node's next start), the node answers `uavcan.pnp.NodeIDAllocationData.1.0` requests,
    giving node-IDs from `CONFIG_CYPHAL_ALLOCATOR_FIRST`..`_LAST` (100 to 119) and remembering
    them in a table of `CONFIG_CYPHAL_ALLOCATOR_TABLE` rows (16), saved in NVS (`cyphal`
    namespace, keys `pnp_alloc` and `pnp_table`), so a node that asks again gets its node-ID
    back, across restarts. The rules, from the type's own text and the plan that built it:
    - It allocates only with a fixed node-ID. With plug-and-play it asks for one as before and
      gives none; `cyphal` says why.
    - It is quiet through the fixed node-ID's 2 s listen, then answers each request at once
      (nominal priority; the spec names none), from its own node-ID, with the request's
      pseudo-ID and the row's node-ID: 9 bytes, two frames.
    - One allocator a bus: the first frame of another node's answer on subject 8166, heard at
      any time from the start, stops it until the node's next start, with one warning and the
      other's node-ID in `cyphal`. Plug-and-play v1 has no arbitration between allocators.
    - It never gives its own node-ID, or one heard on the bus within a heartbeat's offline
      timeout (3 s). A node heard with a node-ID in the range and no row gets a row of its own,
      marked heard and never given (the spec's mock entry).
    - A full table, or a range with nothing free, gets no answer (the spec's rule), counted and
      logged once a start. A row is saved by the worker when it is added, never once per
      request; a save that fails is tried again 1 s later, then ever less often up to every
      30 s, with one warning, and any save that commits (the console's too) ends that back-off.
    - A saved table from a later firmware (a newer blob version) is not read and not
      overwritten: while it is there the allocator, if on, adds no row, answers nothing and
      saves nothing, and `cyphal` and a boot warning say so, until `cyphal allocations clear`
      (which drops it) and the node's next start.
    - `cyphal allocations` lists the table; `forget <id>` and `clear` edit it (the spec's table
      only grows; these are the operator's way out; `forget` on a node that is online turns its
      row into a heard one within a heartbeat or two, so its next ask gets a new node-ID: seen
      with a real holo on the dome panel, 2026-11-22). `cyphal reset` and a factory reset over
      Cyphal turn the setting off and empty the table.
    - Keyed by pseudo-ID alone: the allocator makes no client calls (not even with the client
      below built in), so no `GetInfo` follow-up.
    - What it costs, on the dome panel's ESP32-P4 (2026-11-22): 8.8 KB of code, 200 bytes of
      static internal RAM, the table in PSRAM; through 200 requests a second for a minute, no
      NVS write, heartbeats 0.99-1.01 s apart, libcanard's heap 2,100 bytes at most.
    - With a real holo wired to the panel (2026-11-22), it was online on its row's node-ID
      (100) 1.3 to 2.9 s after each of its own restarts: the holo holds an allocation about
      1 s before it is heard.
    - Without the option nothing of it is compiled: `cyphal_alloc.c` is left out of the
      component, and the calls into it from `cyphal.c` and `cmd_cyphal.c` are under `#if`, so
      the objects' code and data are what they were, but for one value: `ESP_ERROR_CHECK`'s
      `__LINE__` in `cyphal_register_commands()`, which the lines added above it move (checked
      on the dome panel's build by an `objdump -d` diff; the debug line tables move too).
    - A reset (`cyphal reset`, a factory reset) also stops the allocator for the rest of the
      run, so no row is added after it. A row whose node-ID is the allocator's own is not
      answered (`cyphal allocations forget` it). An answer is not queued while the transmit
      queue is nearly full (128 of 160 frames), so a request storm can't starve the heartbeat.
      A node-ID answered in the last 2 s counts as taken even if its row was just forgotten
      (the allocatee holds it about 1 s before it is heard); the node remembers twice its
      table's rows of those answers and never pushes out one younger than 2 s, so if every
      entry were young it would give no new node-ID (refused, counted) rather than forget
      one. A `clear` while nodes in the range are online makes each a heard row at its next
      heartbeat. The port list names subject 8166 only while the allocator answers (not once
      it has yielded or been reset). A save queued before a reset does not write the erased
      table back.
  - **A client, optionally** (`CONFIG_CYPHAL_CLIENT`, off by default; `cyphal_client.h`). The
    node calls services on other nodes:
    - What it calls: the application's `CYPHAL_PORT_CLI` ports, whose service-IDs are registers
      (`uavcan.cln.<name>.id` and `.type`) and whose handlers give the response's extent
      (`cyphal_handler_t.extent`, `CYPHAL_CALL_RESPONSE_MAX` at most); and, with no port of the
      application's, the standard services on their fixed IDs, `CYPHAL_CALL_FIXED(id)`:
      `uavcan.node.GetInfo` (430), `ExecuteCommand` (435), `uavcan.register.Access` (384) and
      `List` (385), `uavcan.file.GetInfo` (405) and `List` (406). All are rows of one table: the
      application's are subscribed to (their responses) when the node goes online, a standard
      one at its first call in a run, so a firmware that only calls GetInfo pays for one
      subscription; each is listed among `port.List`'s clients once subscribed (a standard one's
      first call sends the list again). The service-IDs it calls are a set of their own: a called
      port may share an ID with a served one (the node serves GetInfo and calls it too), never
      with another called one or a standard one it calls (`cyphal_reg_called()`). Without the
      option a `CYPHAL_PORT_CLI` port is handled by the code as it was before the kind existed (the
      objects must not change): its ID is checked as a subject's, and its handler as a
      publication's, so it is refused and logged unless it sets `publish_max`, and then kept, never
      subscribed, and listed among `port.List`'s subscribers. Put one in a table only with the
      option.
    - `cyphal_call(port, server, request, size, timeout_ms, cb, ctx)`, from any task, never
      waits: the serialized request (`CYPHAL_CALL_REQUEST_MAX`, 515 bytes, at most) is copied
      into one of `CONFIG_CYPHAL_CLIENT_CALLS` records (8, PSRAM) and the node's task sends it at
      the port's priority (nominal for a standard service). The outcome goes to `cb` on the
      node's worker, exactly once: `ESP_OK` with the response (cut to the extent),
      `ESP_ERR_TIMEOUT`, `ESP_ERR_NO_MEM` (libcanard refused the request), or
      `ESP_ERR_INVALID_STATE` (the node stopped, restarted or lost its node-ID first). Refused at
      once, with no callback: `ESP_ERR_NOT_SUPPORTED` for a port it can't call (or off this run),
      `ESP_ERR_INVALID_ARG` (a server over 127 or the node itself, a timeout of 0, a size with no
      request), `ESP_ERR_INVALID_SIZE` (a request over 515 bytes), `ESP_ERR_INVALID_STATE` while
      the node isn't online on a node-ID, `ESP_ERR_NOT_FINISHED` with every record in use (or
      none, if they couldn't be allocated). The callback may run before `cyphal_call()` returns.
    - Order, as `cyphal_publish()` keeps its messages': each call queued is stamped with a
      sequence number, and the node's task queues requests for the bus oldest first (a task's
      calls in the order it made them); a request the transmit queue has no room for yet holds
      the younger ones behind it. libcanard's queue sends by CAN ID, so requests to one service on
      one server leave in that order, and others (another priority, service or server) may pass
      them. The worker runs the callbacks of the calls ended by its turn oldest call first; a call
      that ends while an older one is still out has its callback first.
    - A callback finds its call's record already free (the response is a copy in the worker's
      scratch), so it can make the next call even with every other record in use: a chain of calls
      needs one record, not one more than its length.
    - `cyphal_call_wait(...)` is the same call, waiting for the outcome on the caller's task
      (the response copied into its buffer, `*resp_size` the bytes copied: 0 on every failure but
      `ESP_ERR_INVALID_SIZE`, which copies what fits). `ESP_ERR_NOT_ALLOWED`, nothing sent, from a
      task the call would wait on: the node's task, its worker (the application's poll, its
      services' work, a callback), or a task holding the node's lock, which the worker takes (the
      allocator's rows, a register written over Cyphal): an application register's `write` hook,
      run under that lock for the console's `cyphal register`.
    - Only the node's task touches libcanard: it gives each request its transfer-ID (one counter
      for each port and server, as Cyphal keeps one a session, kept across the node's restarts,
      skipping a transfer-ID a call still awaiting its answer from that server on that service
      holds, so no response can answer two calls), queues it when the transmit queue has room for
      the whole of it (the mailboxes' rule) with its transfer's deadline the call's (a second at
      most), so no frame of it is handed to the controller once the call has timed out (the one
      already inside the controller may still go, on a bus that won't take it), matches each
      response by service-ID, source and transfer-ID, copies it out of libcanard's buffer (freed
      as every transfer's is) and times calls out. A response that matches no call (late, another
      node's, a transfer-ID nobody waits for) is dropped and counted. The worker runs the
      callbacks. The records not free are counted, so with no call out the node's task and the
      worker pass the client by without a scan.
    - A record's state word carries its generation (how many times it has been taken), and the
      caller's cancel and the node task's begin each name the word they saw: a cancel late enough
      to find its call failed by a stop, freed and the record taken by another call leaves that
      call alone, and the node's task reads a record's fields only once its begin has them.
    - `cyphal` prints its counters since boot (calls, answered, timed out, not sent, failed,
      responses dropped: a stop's failed calls stay counted beside the calls they were, so calls
      always add up), the records under way and the standard services called this run, only with
      the option.
    - Other nodes' heartbeats, for a client that needs to know a server restarted (its uptime
      going back: nothing else a node sends carries one) or went quiet: `cyphal_app_t.on_heartbeat`
      (a field only with the option), called on the node's task with the source and the heartbeat
      (`cyphal_heartbeat_t`) for every one heard while online. They are read from the frame
      alone (`cyphal_heartbeat_frame()`, host-tested), as the allocator reads another
      allocator's answer: a heartbeat is one frame, so there is no subscription, no libcanard
      memory and nothing kept for each node heard. With the hook set, `port.List` names 7509
      among the subscribers, since the node does hear it. Only a heartbeat of one frame (the 1.0
      type's 7 bytes) is heard: a later version that took more than one frame would not be. NULL
      reads none.
    - Known limit: a response later than the call's timeout that arrives after 32 more calls to
      the same service on the same server (the 5-bit transfer-ID round again) would answer the call
      then out on that transfer-ID. A server answering within its caller's timeout never meets it.
    - What it costs, on the dome panel's ESP32-P4 (2026-11-22): about 4.2 KB of flash, 92 bytes
      of static internal RAM, the rest PSRAM (about 1 KB a call record; the Kconfig help has the
      sum).
    - Without the option nothing of it is compiled: `cyphal_client.c` is left out, and the calls
      into it from `cyphal.c`, `cyphal_registers.c` and `cmd_cyphal.c` are under `#if` or macros
      that are nothing without it, so the objects' code and data are what they were but for
      `ESP_ERROR_CHECK`'s `__LINE__` in `cyphal_register_commands()`, as with the allocator
      (checked on holo-player-fw's build, byte for byte, and on the dome panel's with the
      allocator on, where only the RISC-V assembler's local `.L` labels are numbered
      differently).
  - Host tests: `make -C tests/host` builds `cyphal_util.c`, `cyphal_registers.c`,
    `cyphal_alloc.c` and `cyphal_client.c` with the computer's `cc` and runs them (no ESP-IDF),
    and the `holo` engine's `test_holo.c` beside them;
    the client's through libcanard, a client and servers handing frames between them. The first
    three need only `cc`; in a checkout without the libcanard submodule the client's is skipped,
    with a line saying so. They pass under `-fsanitize=address,undefined` too
    (`make -C tests/host BUILD=build/san CC="cc -fsanitize=address,undefined"`).
  - Clone recursively: libcanard and the DSDL are submodules under `third_party/`, which IDF
    does not take for components (it has no `CMakeLists.txt`).

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
`git clone --recursive`, or run `git submodule update --init --recursive` after cloning (the kit has submodules of its own, under `third_party/`).

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
- `canbus` and `tools/can_check.py` are holo-player-fw's (`components/canbus`, as of
  `82b2756`), made target-neutral for the ESP32-P4.
