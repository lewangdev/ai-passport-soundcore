<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Passport as a soundcore Work (D3200) Recorder

This firmware turns a FoloToy AI Passport into an emulated **soundcore Work**
(Anker AI voice recorder, product code **D3200**). A phone app written for the
real recorder — such as [Soundcore Manager](https://github.com/tacshi/Soundcore),
an open-source project whose published soundcore protocol notes this work follows —
discovers the AI Passport over Bluetooth LE, connects, and uses it like the
original device: device information, clock sync, encrypted sessions, start and
pause recording, live audio streaming for transcription, offline file download,
Wi‑Fi bulk export, binding and factory reset.

## What it does

- **Advertises as a D3200**: 32-bit service UUID `0x020CF5DA`
  (`020cf5da-0000-1000-8000-00805f9b34fb`), 8 bytes of manufacturer data laid out
  like a captured real device (MAC, then flags with the bound bit and color 1), and
  the name `soundcore Work`.
- **GATT**: write characteristic `0x7777`, notify characteristic `0x8888`, plus the
  real device's OTA service (`66666666-…`, writes are discarded) and Tx Power
  service. Every
  command uses the `08 EE …` frame and every reply the `09 FF …` frame with the
  additive checksum; replies are split into MTU-sized notifications.
- **Records with its own microphone**: 16 kHz mono Opus in 20 ms SILK frames
  (16 kbps), stored on a 4.9 MB LittleFS partition (about 40 minutes). Each frame
  is carried in the protocol's fixed 160-byte unit.
- **Encryption like the original**: ECDH P-256 handshake (`2E 01`), session key by
  HKDF-SHA256, a random AES-256 key per recording delivered wrapped in the file
  header, and AES-CTR on every 160-byte unit. Without a handshake the device sends
  a short header and plain units, which the client also accepts.
- **Live streaming**: when recording starts (from the app or the button) the
  device pushes `18 82` and `1A 06 <fileId>`; the app's realtime request
  (`1A 07`, realtimeFlag 1) then receives frames as they are encoded, resuming
  from any byte offset.
- **Wi‑Fi fast export**: `1A 05` brings up a SoftAP with the SSID and password the
  app chose, replies `192.168.4.1:443`, switches the radio from BLE to Wi‑Fi, and
  serves `wss://` WebSocket transfers (hex text requests, binary `1A 07/08/0A`
  frames). The TLS certificate is a self-signed P-256 certificate generated on the
  device the first time and cached in NVS; no key is stored in the repository.
  After `FINISH`, a timeout, or a long OK press, BLE advertising resumes.

## Protocol coverage

| Command | Behavior |
| --- | --- |
| `01 01` device info | Battery bucket, firmware, serial, storage KB, record state, Wi‑Fi firmware |
| `01 A6` sync time | Sets the clock and time zone; new recordings get real Unix timestamps as IDs |
| `01 B8` reset | Acknowledges, wipes recordings and settings, restarts |
| `01 03` push | Battery bucket changes while connected |
| `18 82` record | `01` start/resume, `02` pause; replies with the state (`00` stop includes duration) |
| `1A 0E` / `1B 0E` list | Pages of 10 entries (with end time for `1B`), plus the live file ID and duration |
| `1A 07` transfer | Offline download or realtime stream from an offset; header, `1A 08` slices, `1A 0A` done |
| `1A 10` delete | Deletes a finished recording |
| `1A 0F` / `1A 11` | Acknowledged (no classic Bluetooth; data always goes over BLE) |
| `1A 05` / `1A 02` | Wi‑Fi SoftAP export / close (a paused recording is saved first, since the app has no stop command) |
| `0B 87` bind / unbind | Bind waits up to 60 s for an OK press on the device, then persists the bound flag (also in the advertisement) and sends `0B 88`; unbind is immediate |
| `10 A2` find my | Stored and acknowledged |
| `2E 01` encryption | ECDH handshake as described above |

Not emulated: OTA (`01 BB` is rejected), the device log commands (`FF xx`), the
charging case (case fields mirror the device), and charging detection (the board
reports none).

## Using it

1. Flash this firmware and open the phone app made for soundcore Work.
2. Scan: the device appears as **soundcore Work**. Connect; the screen header turns
   green and the **SECURE** chip lights up after the encryption handshake.
3. Record from the app, or press **OK** on the device. The app transcribes the
   live stream; **BOUND**, **SECURE** and **LIVE** show the link state.
4. Recordings appear in the app's list. Download over BLE or choose Wi‑Fi export,
   then join the network shown on the device screen when the app asks.

| Key | Action |
| --- | --- |
| OK click | Start, pause, or resume recording; while "Press OK to pair" is shown, confirm the bind |
| OK long press | Stop and save the recording; during Wi‑Fi export, close the hotspot |
| UP / DOWN click | Recorder → Recordings → Device pages |

The screen dims after a minute without input; the first key press only wakes it.

## Screens

- **Recorder**: link state and battery in the header, a ring that follows the
  microphone level while recording (red) or stays full while paused (amber), the
  elapsed time, the live file or transfer progress, status chips, and storage use.
- **Recordings**: the eight newest recordings with date, time and duration.
- **Device**: emulated model, MAC, serial, firmware, bind/session/clock state,
  storage, and free heap.
- **Wi‑Fi Export** (automatic): network name and password to join, the endpoint,
  connection state and the number of exported files.

## Implementation map

| File | Role |
| --- | --- |
| `main/sc_proto.*` | Frame codec, command reassembly, reply payloads (pure C, host-tested) |
| `main/sc_crypto.*` | ECDH, HKDF, AES-CTR key wrap and unit encryption (mbedtls) |
| `main/sc_xfer.*` | Transfer header, encrypted slice and done frames |
| `main/sc_ring.*` | Recent-frame ring for live streaming (pure C, host-tested) |
| `main/sc_store.*` | LittleFS recordings with per-file key, seek index, power-loss recovery |
| `main/sc_audio.*` | ES8311 capture and Opus encoding task |
| `main/sc_ble.*` | NimBLE advertisement, GATT and notification sender |
| `main/sc_wifi.*` | SoftAP, HTTPS/WebSocket server, on-device certificate |
| `main/sc_core.*` | Command dispatch, recording state machine, BLE transfer task, Wi‑Fi lifecycle |
| `main/sc_ui.*` | LVGL screens |

`partitions.csv` uses a 3 MB factory application and a 4.9 MB `storage`
partition. Flashing a merged image keeps the partition contents only as described
in the [flashing and data policy](docs/development/engineering/firmware-layout.md#flashing-and-stored-data).

## Verification

- Host tests: `tests/test_sc_proto.c` and `tests/test_sc_ring.c` cover the frame
  layouts at the client's parsing offsets and the ring invariants.
  `tests/test_sc_client_compat.py` builds the firmware's protocol and crypto code
  for the host and checks it against a Python port of the client: ECDH shared
  secret, HKDF, file-key unwrapping, per-unit decryption, BLE-sized reassembly and
  command parsing. It is skipped when host mbedtls or `cryptography` is missing.
- The firmware builds and its merged image passes `tools/verify_firmware.py`.
- On hardware, with a Mac acting as the app over BLE (a Python port of the client):
  advertisement and scan response, connection with MTU 247, 251-byte data length and
  2M PHY, device info, time sync, ECDH handshake (about 0.3 s on the device), start
  and pause, the `1A 06` push, encrypted realtime streaming at 50 frames/s without
  gaps (every frame decrypts to a SILK-WB Opus packet), offline download, the file
  list, power-loss recovery of an open recording, and the `1A 05` reply with the
  switch to SoftAP and back to BLE after the join timeout.
- On an iPhone with Soundcore Manager: connect, device info, time sync, file list,
  offline downloads, the bind prompt on the screen, and the `1A 05` request that
  switches the device to SoftAP.
- **Not yet verified**: the bind confirmation end to end, a Wi‑Fi export with a joined
  client (TLS handshake and transfer), and microphone audio quality by ear.
- **Not supported**: the official soundcore app. It finds and connects to the device
  but never subscribes or sends a command, then disconnects; what it checks is not
  documented.

Measured on the device: about 24 KB of free internal heap with BLE running (20 KB
lowest during streaming), and about 3 KB of spare stack in the Opus task. With 1M
PHY the Mac link carried only 5–8 KB/s, below the 8.3 KB/s a realtime stream
needs, and some connections stalled for seconds. With 2M PHY requested, seven
consecutive 25-second runs streamed without a stall (up to 16 KB/s).

Use it with hardware you own, for interoperability. soundcore and Anker are
trademarks of their owners; this project is not affiliated with them.
