# reinkterface

Fork of [Valve's Inkterface](https://gitlab.steamos.cloud/SteamHardware/SteamMachine/inkterface/) for non Steam Machine usage with simpler hardware — now targeting the **TRMNL 7.5" (OG) DIY Kit**.

![](./docs/reinkterface.jpg)

## In more words
I wanted to have Valve's inkterface in my custom built PC running Bazzite.
In this version there is no soldering required and the kit's stock 2000 mAh battery is used as-is.
This will only run on Linux based machines (tested on Bazzite).

Note: This is just the firmware, you still have to build the official Valve's app that will connect to the board and send data to it.

Changes to the code (vs. the upstream Waveshare 5.83" build):
- **Panel:** switched from `GxEPD2_583_GDEQ0583T31` (5.83" 648×480) to `GxEPD2_750_GDEY075T7` (7.5" 800×480, UC8179 / GD7965) — the panel that ships in the TRMNL 7.5" (OG) DIY Kit.
- **Pin map:** updated to the EE04 / XIAO ESP32-S3 Plus pinout published by TRMNL (CS=44, DC=10, RST=38, BUSY=4, SCK=7, MOSI=9). The kit is wired exactly like that on the PCB.
- **Board target:** `seeed_xiao_esp32s3` with `BOARD_HAS_PSRAM` and `qio_opi` enabled so the full 48 KB 1 bpp framebuffer lives in OPI PSRAM instead of being paged.
- **Layout:** re-tuned for the 800×480 canvas — the three columns of sparkboxes grew from 209 → 262 px wide so they still tile edge-to-edge with the same 5 px gutters.
- **Battery logic restored:** the original 5.83" fork removed battery handling because that board was hard-wired to a motherboard's USB header. The TRMNL kit includes a JST-connected 2000 mAh Li-ion cell with a divider on GPIO1 and a load-switch enable on GPIO6. We sample that every 5 s and render `BAT <X.XX>V` in the bottom-right corner when the host message slot is empty.
- **Swapped the logo in the top-left corner** to Bazzite (unchanged from the 5.83" build — it's still 100×100 px and that lands in the same spot).

## Hardware needed
- Board, e-ink panel and battery - https://www.seeedstudio.com/TRMNL-7-5-Inch-OG-DIY-Kit-p-6481.html

  That kit includes the **Seeed XIAO ePaper Display Dev Board (EE04)** with an onboard XIAO ESP32-S3 Plus, the **Good Display GDEY075T7** 800×480 monochrome e-paper panel, a 2000 mAh Li-ion battery with a JST-PH 2.0 mm connector, a 10 cm FPC extension cable and a power switch on the PCB.

## Building and uploading firmware
- Install PlatformIO for your system
- Connect the display ribbon cable to the board (metal contacts up — see the Seeed wiki for the correct FPC orientation)
- Connect the board to a computer via USB-C

```
pio run -t upload
```

If PlatformIO picks the wrong serial port, force it explicitly:

```
pio run -t upload --upload-port /dev/cu.usbmodemXXXX
```

When it comes to building the Interface (the app that sends the data via BLE), follow the instructions on [Valve's GitLab repo](https://gitlab.steamos.cloud/SteamHardware/SteamMachine/inkterface). This fork uses the same BLE GATT layout (service `95c7b479-8e84-4ce7-a121-faf74bf48c84` + the six characteristics) so it is fully compatible.

## Logo
You can change the Bazzite logo to whatever you want that is 100x100 px.

How I did it:
- Got the BW Bazzite logo from their [Press Kit](https://github.com/ublue-os/bazzite/tree/main/press_kit)
- Removed the "d-pad" for clarity on small display using [Boxy SVG app](https://boxy-svg.com)
- Converted it to bitmap using [image2cpp](https://javl.github.io/image2cpp/)

## Fan RPM
By default, the Inkterface app from Valve won't report Fan RPM - this is to be expected, the app is reading fan rpm from a custom hardware monitor `steamdeck` which does not exist on normal Linux installs.
I found a way around this by poking in the app's code but this will vary based on your setup.
Ultimately it was a one line change in [sysstats.hpp](https://gitlab.steamos.cloud/SteamHardware/SteamMachine/inkterface/-/blob/main/include/sysstats.hpp?ref_type=heads):

```cpp
double getFanRPM() { return readHwmonNode("steamdeck_hwmon", "fan1_input"); }
```

You will need to find your fan in `/sys/class/hwmon`.
In my particular case I need `fan2_input` from `hwmon5` - the name of the hw monitor can be read from file called `name` inside of each `hwmon` directory - in my case it's `nct6792` therefore I changed this line like so:

```cpp
double getFanRPM() { return readHwmonNode("nct6792", "fan2_input"); }
```

After that I rebuilt the app and it started reporting Fan RPM and sending it to the screen.

![](./docs/inkterface_fan_rpm.png)

## Battery
This fork now reads battery voltage off the EE04's GPIO1 / GPIO6 divider because the TRMNL OG kit ships with a battery. The reading appears in the bottom-right corner of the panel (e.g. `BAT 4.05V`) unless the host has sent a message string to the FLUSH characteristic for that refresh — host messages win, battery fills the slot when idle.

Sampling cadence is 5 s. The ADC divider is only powered while a reading is in flight (the GPIO6 load switch is held low the rest of the time), so quiescent draw from the divider is essentially zero.

## Pin reference (EE04 + GDEY075T7)

| Signal | GPIO | Notes |
| --- | --- | --- |
| EPD_SCK  | 7  | SPI clock |
| EPD_MOSI | 9  | SPI data |
| EPD_CS   | 44 | chip select (strapping pin — safe after boot) |
| EPD_DC   | 10 | data / command |
| EPD_RST  | 38 | reset |
| EPD_BUSY | 4  | busy (input only) |
| BAT_ADC  | 1  | battery divider tap |
| BAT_EN   | 6  | divider load switch, active high |

This matches the official pin definitions used by `usetrmnl/trmnl-firmware` for `BOARD_XIAO_EPAPER_DISPLAY`.

## Disclaimer
I have used LLM to speed up the process of rewriting the code and adjusted it afterwards.
