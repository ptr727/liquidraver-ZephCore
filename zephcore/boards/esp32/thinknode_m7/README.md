# Elecrow ThinkNode M7

ESP32-S3 bare die (8 MB flash QIO, 8 MB OPI PSRAM) with a Semtech LR1110 radio and
a WCH CH390 SPI Ethernet controller, optionally powered over PoE. No display, no
GNSS, no buzzer, no battery and no SD card.

The board's purpose in ZephCore is a wired companion: the app reaches it over
TCP on the LAN rather than over BLE or USB, and Home Assistant is expected to
do the same, though that is not yet tested.

## Build

```bash
# Companion (default role)
west build -b thinknode_m7/esp32s3/procpu zephcore --pristine

# Repeater
west build -b thinknode_m7/esp32s3/procpu zephcore --pristine -- \
  -DEXTRA_CONF_FILE="boards/common/repeater.conf"
```

The companion build serves the app over TCP on port 5000 on the wired link,
the same as every other ZephCore TCP companion, so Home Assistant's MeshCore
integration should reach it unchanged. That is not yet tested. The Ethernet
stack is added automatically because the board manifest declares
`capabilities: ethernet: true`.

Console is uart0 at 115200 through a dedicated **CH340K** bridge, which enumerates
as `1a86:7522`, binds `ch341-uart` and gives `/dev/ttyUSB0`. Its auto-reset circuit
works, so esptool's `--before default-reset` needs no buttons.

The bridge is not wired to the ESP32-S3's own USB pads, which cuts both ways. The
USB identity never changes with firmware or role, so there is no re-binding to do
and none of the identity churn native-USB ZephCore boards cause. But there is no
JTAG and no USB CDC, so no hardware debugger and no CDC companion transport: the
serial companion is a plain UART on uart0 instead, which is why it and the
console cannot both have it.

There is also no reset button: reset by pulsing DTR/RTS or by power cycling.

## Pin map

Three independent sources agree on every pin below: the Arduino MeshCore
`variants/thinknode_m7`, the Meshtastic `ELECROW-ThinkNode-M7` variant, and an
independent ESPHome port of the same board. Elecrow publishes neither a pinout
nor a schematic, so there is no vendor document to check against.

ESP32-S3 GPIO banks: GPIO0-31 are `&gpio0`; GPIO32-47 are `&gpio1` at pin
GPIO minus 32.

| Function | GPIO | Node |
|---|---|---|
| LR1110 SCK | 11 | `spim2_default` |
| LR1110 MISO | 9 | `spim2_default` |
| LR1110 MOSI | 10 | `spim2_default` |
| LR1110 NSS | 12 | `&gpio0 12` |
| LR1110 BUSY | 13 | `&gpio0 13` |
| LR1110 DIO1 (IRQ) | 38 | `&gpio1 6` |
| LR1110 NRESET | 39 | `&gpio1 7`, active low |
| CH390 SCLK | 47 | `spim3_default` |
| CH390 MISO | 14 | `spim3_default` |
| CH390 MOSI | 48 | `spim3_default` |
| CH390 CS | 21 | `&gpio0 21` |
| CH390 INT | 45 | `&gpio1 13` |
| I2C0 SDA / SCL | 17 / 18 | `i2c0_default` |
| Status LED, green | 3 | `&gpio0 3`, active low |
| LoRa TX LED, blue | 46 | `&gpio1 14`, active low |
| Button | 4 | ADC unit 1 channel 3 |
| Console TX / RX | 43 / 44 | `esp32s3_console_uart0.dtsi` |

The LR1110's TCXO is on the radio's own DIO3 at 1.8 V, and the RF switch is on
the radio's DIO5 and DIO6 rather than on ESP32 pins. The truth table is identical
to the ThinkNode M9's.

## Strapping pins

GPIO3, GPIO45 and GPIO46 are all ESP32-S3 strapping pins and all three are wired
to something on this board.

GPIO45 is VDD_SPI and must read LOW at boot, or the chip selects a 1.8 V flash
supply and the 3.3 V flash browns out. It carries the CH390's interrupt, and the
driver programs the controller's interrupt polarity from the devicetree flags, so
`int-gpios` is `GPIO_ACTIVE_HIGH`: that is the controller's reset value and idles
the line low. Active-low would work on the first cold boot and hang every warm
one, since the CH390 has no reset line and would keep the pin asserted across a
CPU-only reset.

That settles the idle case but not the whole problem: the driver unmasks RX, TX
and link-change interrupts and clears the status register only from its RX
thread, so the CH390 really does drive GPIO45 high while an interrupt is
pending, and a reset landing in that window would present a high strap to the
next boot.

On this board it does not matter, and that is measured rather than assumed.
`espefuse.py summary` reports `VDD_SPI_FORCE = True` with
`VDD_SPI_TIEH = VDD3P3_RTC_IO`, so the flash supply is fixed at 3.3 V from
efuse and the GPIO45 strap is ignored. Re-read the efuse before assuming the
same of another unit.

GPIO3 and GPIO46 drive the two LEDs. The gpio-leds driver runs at POST_KERNEL,
long after the strapping latch, so this is safe.

## Button

The Meshtastic variant treats GPIO4 as a discrete active-low input with a pull-up;
the Arduino MeshCore variant calls it an analog ladder. With a single button the
two readings are indistinguishable. The ADC channel is declared so the raw level
can be read, but no key mapping is asserted until the thresholds are measured.

## Bring-up results

Verified on hardware 2026-09-30, on the board this port was written for.

| Step | Result |
|---|---|
| Console on `/dev/ttyUSB0` at 115200 | Zephyr banner and ZephCore startup |
| Flash and PSRAM | 8 MB flash; `octal_psram density 0x03 (64 Mbit)`, AP vendor, 8 MB, so octal is right |
| VDD_SPI strap | `VDD_SPI_FORCE = True`, `TIEH = 3.3 V`: the GPIO45 strap is overridden |
| LR1110 identity | `HW:0x22 Type:0x01 FW:0x0303`; RF switch `en=0x03 rx=0x01 tx=0x03 txhp=0x02` |
| Radio configured | `freq=927875008 bw=62 sf=7 cr=5 pwr=1` |
| Radio receive | Discovered real nodes, so the sync word is honoured on FW `0x0303` |
| Ethernet controller | CH390 `Found ID: 9151`; address set to the SoC's Ethernet MAC (`esp_read_mac`, the efuse base MAC plus 3 on the ESP32-S3's four-address scheme) |
| Ethernet link | Up at 100 Mbps, agreed by the switch |
| DHCP | Lease acquired |
| Reachability | Ping 5/5 at 1.58 ms average; the companion's TCP port open from another host |
| Companion protocol | `CMD_DEVICE_QUERY` over TCP answered `DEVICE_INFO`: model `ThinkNode M7`, version `1.17.6-zephcore` |
| DHCP hostname in DNS | `<node-name>.<lan-domain>` resolves and pings, some minutes after first boot |

Getting there needed driver changes beyond accepting the CH390's ID, now in
`patches/zephyr/0020-eth-dm9051-wch-ch390.patch` (one section per upstream-bound
commit). The CH390 soft reset powers the PHY down again, so it is powered back
on; the CH390 leaves the NSR transmit-end bits clear, so transmit waits on
TCR.TXREQ instead; and the receive thread services the interrupt again while
the line is still asserted, rather than waiting for an edge that never comes.
Without the first there is no link at all; without the second nothing is ever
transmitted; without the third the node completes DHCP and then answers
nothing. Each was reproduced on unpatched code and confirmed fixed on this
board.

## Connecting a companion app

The transport and framing are upstream MeshCore's `SerialWifiInterface`, so any
TCP-capable MeshCore client works. Two that do:

```bash
# meshcore-cli, verified against this board
uvx --from meshcore-cli meshcore-cli -t <node-address> -p 5000 infos
```

the **Colorado-Mesh desktop client** (<https://github.com/Colorado-Mesh/mesh-client>),
which connects over TCP and is what the maintainer uses, and the MeshCore
companion app's **"Connect via WiFi"** (TCP/Network) mode.

The MeshCore **web** client cannot reach it over the network: a browser cannot
open a raw TCP socket, so its WiFi mode reports "not supported on your device".
Only WebSocket, WebSerial and WebBluetooth are available to it, and ZephCore
has no WebSocket transport.

### uart0 is the console or a companion, never both

This board has exactly one UART behind the CH340K, and the binary companion
protocol cannot share a line with boot and log output. So it is one role or
the other, chosen at build time.

**Default: the console.** A board nobody can read is a board nobody can bring
up, so the stock build keeps uart0 as the console and reaches the companion
over TCP only. `CONFIG_LOG` is off as in any ZephCore release build; add
`boards/common/debug.conf` for application logs.

**Opt in to the companion** when a config app on the cable is worth more than
logs. TCP accepts a single client, so this is what lets a configuration
session coexist with Home Assistant:

```bash
west build -b thinknode_m7/esp32s3/procpu zephcore --pristine -- \
  -DEXTRA_CONF_FILE="boards/common/serial_companion.conf"
```

Verified on hardware: two `meshcore-cli` instances run concurrently, one on
`-s /dev/ttyUSB0` and one on `-t <node> -p 5000`, returning byte-identical
device info, and a third-party desktop client on TCP coexists with a USB
session.

Two things to know before relying on the companion build.

`ZEPHCORE_COMPANION_SERIAL` defaults to `y` for a board that has no Bluetooth
and names `zephcore,companion-uart`, which this board does on both counts.
That default is meant for a board whose UART is its *only* companion link; here
it is not, so `board.conf` pins it off and `serial_companion.conf` turns it
back on together with disabling the console. Without that pin the companion
and the console would both own uart0 and corrupt each other.

The line is not clean at boot, and nothing in firmware can make it so. The
ESP-ROM and the second-stage bootloader write about 2 KB of plain text to
uart0 before Zephyr runs, and the bridge's auto-reset circuit means opening
the port reboots the board, so a client receives that banner on every connect.
Measured at 1978 bytes each time, followed by complete silence once booted, so
ZephCore itself contributes nothing. MeshCore's framing skips any byte that is
not a frame marker, so clients resynchronise; `meshcore-cli` does so without
complaint.

### TX power: 1 dBm is the minimum

**The lowest valid TX power is 1 dBm. Do not use a negative value**, whether
on the CLI, from a client, or as `CONFIG_ZEPHCORE_DEFAULT_TX_POWER_DBM`.

The setting is unsigned on the wire. ZephCore's Kconfig and CLI accept values
down to -9, but a negative value wraps: -9 goes out as `0xF7`, `meshcore-cli`
shows `tx_power: 247`, and the Colorado-Mesh desktop client rejects it with
*"Device reports 247 dBm (slider max 22 dBm)"*. It is not a usable setting.

1 dBm round-trips cleanly. Set from a client, it persisted to prefs, applied to
the radio live, and survived a reboot (`radio started: ... pwr=1`).

### BLE is compiled out by default

`board.conf` sets `CONFIG_BT=n`. The reason is not only the ~66 KB of internal
DRAM and ~210 KB of flash it returns.

**BLE cannot be turned off at runtime on this board.** The `ble_disabled` pref
is written only by the on-device UI button (`helpers/ui/ui_mesh_actions.cpp`),
the companion protocol exposes no command for it, and the CLI has none; the
`ble_off` key in the prefs JSON is a file on `/lfs`, not something a client can
reach. A board with no display and no buttons therefore has no way to stop it
advertising.

And what it advertises is weak. The pairing passkey defaults to
`CONFIG_ZEPHCORE_BLE_PASSKEY`, **123456**, and a board with no display cannot
show a generated one. A client can set a private PIN with `CMD_SET_DEVICE_PIN`,
but any client on TCP or the serial companion can also read the current PIN
from `DEVICE_INFO`, unauthenticated. On a node nobody will ever pair with, that
is a permanently advertising admission path with a secret that is either the
published default or readable by anyone on the LAN.

To build *with* BLE, drop `CONFIG_BT=n` from `board.conf`. Note the consequence
before doing so on a node in service: TCP accepts a single client, so BLE is
what would let a configuration session coexist with Home Assistant, and without
either it the companion build (above) is the way to get that.

DHCP hostname: the node name goes out as DHCP option 12, sanitised to a DNS
label. Verified end to end: the lease records the node name, the switch shows
it on the port, and the router resolves `<node-name>.<lan-domain>`. Allow some
minutes after first boot before the name resolves.

Not yet exercised: sustained throughput, a long run on PoE alone, the GPIO4
button ladder, and the Home Assistant integration itself.

### Resolved: the link used to flap under load

Early bring-up saw the link drop and recover roughly every couple of minutes,
always self-healing after about 1.7 s with a fresh DHCP bind. The cause was in
the driver, not the hardware, and has two parts, both fixed in `0020`.

`eth_dm9051_recv_pkt()` treated any RX status error as fatal and called
`hw_start()`, which resets the MAC and the PHY and therefore drops the carrier.
The status bit that actually fired was the **FIFO overflow** — ordinary
congestion on a busy segment — so every busy moment cost a carrier loss, a
renegotiation and a new DHCP cycle. On the CH390 such a frame is now skipped by
its length and reception continues.

The same restart also fired for a legal frame of 1519 to 1522 bytes, such as a
full-size VLAN-tagged one: the chip delivers frames longer than 1518 bytes
(1540 was seen), while the driver's buffer holds 1518 without VLAN support, so
the length check took the frame for a corrupt header. A frame up to 1536 bytes,
the limit Linux's driver also uses, that is larger than the buffer is now
dropped by its length. Above 1536 the read pointer is not trusted and the
controller is still restarted. Standard Ethernet frames never get that long,
but any host on the same segment can send one, and each such frame costs a
link outage and a new DHCP lease.

Measured under a 5 packet per second ping, ten minutes each:

| | link downs | DHCP binds |
|---|---|---|
| before | 3 | 4 |
| after | **0** | 1 (the initial one) |

End to end afterwards: 600 pings, 0% loss, 1.44 ms average, 2.17 ms worst.

A frame skipped for a status error or dropped as oversized leaves no trace on
this board: the driver counts it as an RX error and logs it at debug level, but
the build includes neither network statistics nor debug-level driver logging.
Reception carries on with the frames queued behind it. Injected from a host on the same switch, ten frames each of
1519, 1522 and 1536 bytes were all dropped with no restart and no link event,
and the ping sent behind each one was answered. A 1540-byte frame, beyond the
1536-byte limit, still restarts the controller.

## Not ported

Display, GNSS, buzzer, battery monitoring and SD card: none are fitted. The
button's key mapping is deferred until its thresholds are characterised. WiFi is
left out of the board manifest because this board's network path is the wired
CH390 and it is unverified whether the bare die has a WiFi antenna fitted. The
repeater build still enables WiFi for OTA, as on every ESP32-S3 repeater, and
that is untested on this board.
