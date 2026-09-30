# Elecrow ThinkNode M7

ESP32-S3 bare die (8 MB flash QIO, 8 MB OPI PSRAM) with a Semtech LR1110 radio and
a WCH CH390 SPI Ethernet controller, optionally powered over PoE. No display, no
GNSS, no buzzer, no battery and no SD card.

The board's purpose in ZephCore is a wired companion: the app and Home Assistant
reach it over TCP on the LAN rather than over BLE or USB.

## Build

```bash
# Companion (default role)
west build -b thinknode_m7/esp32s3/procpu zephcore --pristine

# Repeater
west build -b thinknode_m7/esp32s3/procpu zephcore --pristine -- \
  -DEXTRA_CONF_FILE="boards/common/repeater.conf"
```

The companion build serves the app over TCP on port 5000 on the wired link,
beside BLE, so Home Assistant's MeshCore integration reaches it as an ordinary
TCP companion. The Ethernet stack is added automatically because the board
manifest declares `capabilities: ethernet: true`.

Console is uart0 at 115200 through a dedicated **CH340K** bridge, which enumerates
as `1a86:7522`, binds `ch341-uart` and gives `/dev/ttyUSB0`. Its auto-reset circuit
works, so esptool's `--before default-reset` needs no buttons.

The bridge is not wired to the ESP32-S3's own USB pads, which cuts both ways. The
USB identity never changes with firmware or role, so there is no re-binding to do
and none of the identity churn native-USB ZephCore boards cause. But there is no
JTAG and no USB CDC, so no hardware debugger and no USB companion transport: uart0
is the only console, and the wired companion path has to be Ethernet. The ESP32-S3's native USB pads are not bonded to the connector, so
there is no USB CDC companion on this board. There is also no reset button:
reset by pulsing DTR/RTS or by power cycling.

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
| Radio configured | `freq=927875008 bw=62 sf=7 cr=5 pwr=-9` |
| Radio receive | Discovered real nodes, so the sync word is honoured on FW `0x0303` |
| Ethernet controller | CH390 `Found ID: 9151`; address set to the efuse base MAC plus 3 |
| Ethernet link | Up at 100 Mbps, agreed by the switch |
| DHCP | Lease acquired |
| Reachability | Ping 5/5 at 1.58 ms average; the companion's TCP port open from another host |
| Companion protocol | `CMD_DEVICE_QUERY` over TCP answered `DEVICE_INFO`: model `ThinkNode M7`, version `1.17.6-zephcore` |
| DHCP hostname in DNS | `518ce3cf.home.insanegenius.net` resolves and pings, some minutes after first boot |

Getting there needed three fixes in the DM9051 driver patch: the PHY power-on
has to be re-asserted after the MAC soft reset, the pre-transmit NSR poll must
not abort the frame when it times out, and the receive thread must not wait on
the edge interrupt alone. Without the first there is no link at all; without
the second nothing is ever transmitted; without the third the node completes
DHCP and then answers nothing.

## Connecting a companion app

The transport and framing are upstream MeshCore's `SerialWifiInterface`, so any
TCP-capable MeshCore client works. Two that do:

```bash
# meshcore-cli, verified against this board
uvx --from meshcore-cli meshcore-cli -t <node-address> -p 5000 infos
```

and the MeshCore companion app's **"Connect via WiFi"** (TCP/Network) mode,
pointed at the node's address on port 5000.

Note `meshcore-cli` prints `tx_power` unsigned, so a configured -9 dBm reads as
`247`. That is the CLI's formatting, not the node's setting.

**Only one TCP client at a time** — the transport logs `Second client rejected
(already connected)` and closes the second connection. That matters for a node whose job is to serve
Home Assistant: HA occupies the single TCP slot, so keep BLE enabled if you also
want to configure the node from the phone app. Every connected transport is
served at once (`MultiSerialInterface`), so BLE and TCP run in parallel and both
see every frame.

The BLE pairing passkey is `CONFIG_ZEPHCORE_BLE_PASSKEY`, **123456** by default.
It is a compile-time constant, overridable per node through the `ble_pin` pref,
and the device reports its current value in the `DEVICE_INFO` response. A fixed
well-known passkey is worth thinking about on a node that sits powered on a
shelf: `ble_off` in the prefs turns BLE advertising off once the node is
configured.

DHCP hostname: the node name goes out as DHCP option 12, sanitised to a DNS
label. Verified end to end — the lease records `518ce3cf`, the switch shows it on
the port, and the router resolves `518ce3cf.home.insanegenius.net`. Allow some
minutes after first boot before the name resolves.

Not yet exercised: sustained throughput, a long run on PoE alone, the GPIO4
button ladder, and the Home Assistant integration itself.

### Known issue: the link flaps

The link drops and recovers roughly once every couple of minutes. Measured over
150 s: up at 2.3 s, DHCP bound at 6.4 s, down at 42.6 s, up again 1.7 s later,
re-bound at 49.4 s. It always recovers on its own and DHCP re-binds to the same
address, so the node stays usable, but it is not understood and it is not
acceptable for a node meant to sit on a shelf.

Not yet separated: whether this is the PHY, the switch renegotiating PoE class,
or the driver's link-change handling. The switch reports the port as PoE+ at
100 Mbps throughout. Worth noting that slow DNS registration has previously been
a symptom of a half-working link rather than of propagation delay, so this is the
first thing to rule out if a name is slow to appear.

## Not ported

Display, GNSS, buzzer, battery monitoring and SD card: none are fitted. The
button's key mapping is deferred until its thresholds are characterised. WiFi is
left out of the board manifest because this board's network path is the wired
CH390 and it is unverified whether the bare die has a WiFi antenna fitted.
