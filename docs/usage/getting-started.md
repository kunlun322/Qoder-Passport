**English** · [简体中文](getting-started.zh_CN.md)

# Getting started with Qoder-Passport

This is the end-user guide: unpacking a FoloToy AI Passport, putting
Qoder-Passport on it, connecting it to your network and your Qoder account, and
using it day to day. For board internals and building from source see the
[documentation index](/docs/README.md).

## What you need

| Item | Notes |
| --- | --- |
| FoloToy AI Passport device | ESP32-C3, 8 MB Flash, 240 x 320 display |
| USB Type-C cable | Must be a **data** cable, 5 V. Used for flashing and for the serial console |
| A computer with Python and `esptool` | Any OS. `pip install esptool` is enough for flashing |
| A 2.4 GHz Wi-Fi network | The ESP32-C3 radio does not support 5 GHz |
| A Qoder account with a Personal Access Token | Created in your Qoder account settings |
| A phone or laptop for provisioning | It joins the device hotspot and opens a web page |

You do **not** need ESP-IDF to flash a prebuilt release. You only need it to
build from source.

## Power and controls

- **Power button** (dedicated hardware button): hold about 0.5 s to start, about
  2 s to shut down.
- **UP / DOWN / OK**: the three function buttons on the front. Short press is
  180 ms, long press is 500 ms.
- After the screen turns off automatically, pressing any function button wakes
  the device.
- The display is not touch-sensitive; everything is done with the three buttons.

See the [button reference](/README.md#button-reference) for what each button does
on each screen.

## Flash the firmware

Find the serial port first. Do not guess: on Windows other COM ports may belong
to unrelated hardware.

```bash
# List ports (Linux/macOS: ls /dev/ttyACM* /dev/ttyUSB*)
python -m serial.tools.list_ports -v
```

The AI Passport enumerates as USB `VID:PID = 303A:1001` on its native
USB Serial/JTAG interface.

Then write the merged release image at offset `0x0`:

```bash
python -m esptool --chip esp32c3 -p <PORT> -b 460800 \
  write_flash 0x0 Qoder-Passport-v0.1.0-esp32c3-full.bin
```

Verify a download before flashing:

```bash
sha256sum Qoder-Passport-v0.1.0-esp32c3-full.bin
```

The hash must match the value published in the
[release notes](/docs/development/release/v0.1.0.md).

> [!WARNING]
> The merged image spans the bootloader, partition table, and application, and
> pads the gaps between them. Writing it at `0x0` **erases the NVS region**, so
> the stored Wi-Fi credentials, PAT, and volume setting are lost and the device
> will start in provisioning mode. That is expected for a first install.
>
> To upgrade and keep your settings, write only the bootloader and the
> application at their own offsets, and leave `0x8000` (partition table) and
> `0x9000` (NVS) untouched. This is only safe when the partition table is
> byte-identical to the one already on the device; the release notes state the
> partition-table hash so you can check. See
> [firmware layout and flashing policy](/docs/development/engineering/firmware-layout.md).
>
> Never run a whole-chip erase to fix an ordinary error.

## First run: configure Wi-Fi and PAT

Qoder-Passport never asks you to edit a source file or reflash to change
networks. It runs its own configuration portal.

1. Power on the device.
2. If no usable Wi-Fi credentials or no PAT are stored, the screen switches to
   the provisioning page. It shows:
   - a QR code,
   - the hotspot name (SSID),
   - a temporary WPA2 password, generated fresh for this session,
   - the portal address `192.168.4.1`.
3. On your phone or laptop, join that hotspot using the displayed password. The
   hotspot is WPA2-protected, not open.
4. Open `http://192.168.4.1`. If a captive-portal prompt appears, accept it.
   The device runs a DNS server on the gateway address, so arbitrary hostnames
   resolve to the portal.
5. The page lists nearby networks with signal strength. Pick yours. If your
   network is hidden, use the manual-entry option and type the exact SSID.
6. Enter the Wi-Fi password. If you are only changing the PAT, the page can
   reuse the existing Wi-Fi password.
7. Paste your Qoder Personal Access Token.
8. Press save. The page switches to a progress state and polls the device.

What happens next on the device:

- It connects to your network as a station. If the first association is refused
  by a home router (a common behaviour right after the device switches its radio
  from AP to station mode), it retries with a backoff of 1.2 s, 2.4 s, 3.0 s,
  3.0 s, 3.0 s.
- It calls the QCA API with your PAT to confirm the token really works. **The PAT
  is verified before anything is stored**, so an invalid token cannot leave the
  device in a half-configured state.
- On success it saves the Wi-Fi credentials and the PAT to NVS, shows a
  completion message, and reboots into normal mode after about 3 s.
- On failure the screen and the web page both report which step failed:
  Wi-Fi, PAT, service, or save. Each failure page offers retry, and long-pressing
  OK opens device settings.

If the page says you are not connected to the device hotspot, your browser is
reaching the device through some other interface. Rejoin the hotspot, disable
mobile data on the phone, and reload.

## Choose an Agent and an identity

After provisioning the device reboots and joins your network automatically. On
subsequent boots it reuses the channel it last saw your access point on, which
makes association noticeably faster.

1. **Step 1 — choose an Agent template.** The device fetches your QCA templates
   over HTTPS. Use UP/DOWN to move (double click moves 2, long press moves a
   page of 5) and OK to confirm. The counter in the top-right shows your
   position.
2. **Step 2 — choose a user identity.** Same controls. OK starts the
   conversation; long-pressing OK goes back to step 1.

If either list cannot be fetched, an error page explains why and offers
`OK` to retry. Long-pressing OK from the template screen opens device settings.

## Talk

Once the session opens the voice screen shows the template name as the title,
the identity name as the subtitle, a status word, and a transcription line.

The status word cycles through, in Simplified Chinese on screen: connecting,
listening, thinking, answering, paused, ended, and error.

- Just speak. The session is full duplex, so you can interrupt the agent while
  it is talking and it will stop and listen.
- **UP / DOWN** change the volume in steps of 10; a toast confirms the new level.
- **OK click** pauses and resumes.
- **OK long press** ends the conversation and returns to the identity list.
- The speaker is loud enough to be used hands-free; the microphone path applies
  on-device gain and clipping control so shouting does not distort the uplink.

## Device settings

Long-press **OK** on the template screen, on the provisioning screen, or on a
Wi-Fi/PAT failure screen to open settings. The menu offers:

| Entry | Effect |
| --- | --- |
| Cancel provisioning and reboot *(provisioning screen only)* | Leaves the hotspot without saving |
| Refresh the Agent list *(normal screens)* | Re-fetches templates and identities |
| Reconfigure Wi-Fi / PAT | Reboots into the provisioning portal |
| Factory reset | Two-step confirmation, see below |
| Back | Returns to the previous screen |

The cursor starts on **Back**, so an accidental long press does nothing unless
you deliberately move.

### Factory reset

Choosing factory reset shows a confirmation screen that states exactly what will
be deleted: Wi-Fi credentials, the PAT, and the volume setting. It also states
what is **not** deleted: server-side conversations.

- The confirmation defaults to **Cancel**.
- Selecting the destructive option still requires a **long press** on OK to run.
- The device stops Wi-Fi, deletes the stored values, then reboots into
  provisioning mode.
- If deletion fails part way, the device keeps the reset intent, tells you it did
  not enter normal networking, and offers retry. It never pretends a failed
  reset succeeded.

Factory reset does not erase the whole flash chip and does not touch the PHY
calibration partition beyond what the reset requires.

## Serial console

The console is on the native USB Serial/JTAG interface at **115200 baud** — not
the 2000000 baud used by some other ESP32 boards, and not UART0.

```bash
python -m esptool --chip esp32c3 -p <PORT> ...   # flashing
idf.py -p <PORT> monitor                          # if you have ESP-IDF
```

Opening the port can reset the device depending on the tool and the DTR/RTS
lines. The log never prints your SSID, Wi-Fi password, or PAT. Credential code
paths log only lengths and error names. The on-device messages are Simplified
Chinese; their shape is roughly:

```
I (1048) qca_pat: <PAT loaded notice, content not logged>
I (1049) main: <configuration ready, free heap 98972>
```

## Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| Screen stays black | Charge the battery; confirm you held power for 0.5 s. The display needs its backlight on GPIO21 |
| Portal page says you are not connected to the hotspot | Your phone/laptop reached the device over another interface. Rejoin the hotspot, turn off mobile data, reload |
| Portal will not load at all | Confirm you are associated with the device SSID and got a `192.168.4.x` address from its DHCP server |
| Page loads but saving does nothing | The portal rejects writes without its per-session CSRF token. Reload the page to get a fresh token |
| Wi-Fi fails after provisioning | The router may have refused the first association, or the password is wrong. Retry from the failure page; check the 2.4 GHz band |
| Device keeps returning to provisioning after a reboot | Either the network was not visible at boot (it retries for about 21 s, then falls back to the hotspot) or the PAT check failed. Re-run provisioning |
| PAT rejected | The token is wrong, expired, or lacks the needed scope. Create a new one in your Qoder account |
| Voice connects but no audio | The error page reports an audio initialisation failure if the codec did not come up. Reboot; if it persists, capture a serial log |
| Conversation drops under weak signal | The session reports the disconnect and offers retry. Move closer to the access point |
| Some Chinese characters render as boxes | A glyph is missing from the committed font. See the [Chinese font guide](/docs/development/engineering/lvgl-chinese-fonts.md) |

When reporting a problem, include the firmware version and the ELF SHA-256 from
the release notes so a crash log can be decoded against the right symbols.
Remove any Wi-Fi credentials, tokens, device QR content, and personal data from
logs before sharing them.

## Where to go next

- [Usage and button reference](/README.md)
- [v0.1.0 release notes](/docs/development/release/v0.1.0.md)
- [Hardware and software stack](/docs/development/engineering/qoder-passport-stack.md)
- [Hardware development guide](/docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md)
- [Getting help](/.github/SUPPORT.md) · [Filing issues](/docs/development/release/file-issues.md)
