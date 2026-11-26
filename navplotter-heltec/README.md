# NavPlotter node for Heltec WiFi LoRa 32 V3

Firmware for a **Heltec WiFi LoRa 32 V3 (868 MHz)** with an external GPS module. The board has no chart screen, so it does the navigation work and a phone, tablet or computer shows it in a browser:

- reads the GPS;
- exchanges name, position, course and speed with the other nodes over LoRa, using the same packet as the T-Deck plotter, and relays packets (mesh);
- computes the collision alarm (closest point of approach);
- creates its own WiFi network and serves a web app.

The user interface is written in English and translated into Italian; the language is chosen in the Setup page.

## Using it

1. Power the board. The OLED shows the WiFi network name and the address.
2. Join the WiFi network `NavPlotter-<node name>` (default password `navplotter`, change it in Setup).
3. Open `http://192.168.4.1` in the browser. It can be added to the home screen.

Pages:

| Page | What it does |
|---|---|
| Map | own position, other nodes, waypoints, line to the destination. Drag to pan, pinch or +/− to zoom, the ring button follows the boat. Tap a node or waypoint for details, long press to add a waypoint. |
| Compass | heading-up compass with the bearing of the destination. Heading is the GPS course. |
| Nearby | list of received nodes with distance, bearing, closest approach and signal. |
| Waypoints | add, rename, delete, go to. The Routes tab builds ordered lists of waypoints; following a route moves the destination to the next waypoint on arrival (within 0.05 NM) and shows the distance still to go. Stored on the board. |
| Setup | language, node name, node type, alarm range, alarm sound, chart source, WiFi password, status. |

### Connection

The Setup page offers three scenarios:

| Scenario | Node | Phone | Charts |
|---|---|---|---|
| Off-grid | creates its own WiFi | joins the node WiFi | SD card on the node |
| Node WiFi, online charts | creates its own WiFi | joins the node WiFi, uses mobile data for charts | online |
| Boat WiFi | joins the boat WiFi (name and password entered in Setup) | joins the boat WiFi | online, through the boat router |

On the boat WiFi the node is reached at `http://<node name>.local` or at the address shown on its display. The node's own WiFi stays on in every scenario, so the node can always be reached at `http://192.168.4.1` even if the boat WiFi is off or the password is wrong. Choosing a scenario presets the chart source, which can still be changed.

### Privacy

The **Share my position over LoRa** option in Setup decides whether the node transmits its own position. When it is off the node is invisible to the others, but it still receives them, raises the collision alarm and relays their packets through the mesh.

### Node type

A node can be a **boat**, a **person** or an **object** (for example a crew member's beacon, a tender, a buoy). The type is sent over LoRa and each type has its own symbol: arrow, ring with a dot, diamond.

### Alarms

The collision alarm fires when a node inside the **alarm range** (3, 6 or 12 NM) is predicted to pass closer than 0.1 NM within 5 minutes. The banner turns red and, if enabled, the phone beeps and vibrates (tap the page once after opening it, browsers only allow sound after a touch). The **Silence** button on the red banner mutes the current alarm only, without changing the Setup option: it lasts while those nodes stay in alarm, and a new node in alarm sounds again.

### Charts

- **Online**: OpenStreetMap tiles, needs internet on the phone.
- **SD card on the node**: tiles in `/tiles/z/x/y.jpg`, the same card layout as the T-Deck. Needs a micro SD SPI module.
- **None**: plain background.

## Messages

Short texts (63 characters at most) between NavPlotters, over LoRa and through the mesh, next to the positions:

- to **one node**: the message is repeated until that node confirms it has arrived ("received"), and the sender is told again when somebody there has read it ("read"). After four tries without a confirmation it shows "NO ANSWER";
- to **everybody** in range: sent twice, no confirmation asked.

The node has no chart screen: messages are written and read from the **Chat** page of the web app, also reached from a node's pop-up on the map and from the Nearby page.

An incoming message comes up over whatever page is open, with a sound repeated until it is closed. Only nodes with this firmware, switched on and in range, receive anything, and a packet can be lost: this is a convenience, not a safety system. Messages are kept in memory (the last 12) and are lost at power-off. A beacon asleep (Segnalatore) receives nothing.

The protocol is in `include/nav_messages.h`, which must be the same file in every NavPlotter that has messages. The web app reads and writes them through `/api/messages`.

## MOB and weather

The web app also has the two modules of the Android app and of the Linux node, the same files as `web/` of Navplotter-Linux:

- **MOB** (`web/navplotter-mob.js`): the red man-overboard button over every page. A tap puts a waypoint with a lifebuoy at the boat's position, makes it the destination and sounds an alarm until that waypoint is deleted.
- **Weather and passage planning** (`web/navplotter-meteo.js`): wind, current and thunderstorms on the chart, and the Plan tab in Waypoints (times, wind, current and waves for each leg of a route, tacks from the boat's polar). The forecasts come from Open-Meteo and are downloaded by the phone, so it needs internet of its own: on the boat WiFi, or on mobile data while joined to the node WiFi. The node does not download them.

## Wiring

| Function | Board pin |
|---|---|
| GPS module RX | 47 (board TX) |
| GPS module TX | 48 (board RX) |
| SD module SCK / MOSI / MISO / CS (optional) | 5 / 6 / 7 / 4 |

The GPS speed is detected automatically.

## Building

```
~/.platformio/penv/bin/pio run -t upload
```

The web app is `web/index.html`; `tools/embed_web.py` puts `web/navplotter-meteo.js` and `web/navplotter-mob.js` inside it and compresses it into `src/web_page.h` before every build. When those two files change in Navplotter-Linux, copy them here.
`include/traffic_packet.h` (packet, radio settings, mesh) must stay identical to the copies in the T-Deck plotter and in the simulators.

## Track

The node records the boat's track by itself: a point every 25 metres made good, none while the boat is still, also with the phone off. The points are kept in memory (4000 at most, about 54 nautical miles; when full the oldest quarter is dropped) and saved to the internal storage every couple of minutes, so they survive a restart. In the web app, page Waypoints, tab **Track**: length, start and duration, show on the map, **Save as GPX** (for OpenCPN, Google Earth and the like), **New track**. The web app reads it through `/api/track`, `/api/track.gpx` and `/api/track/new`; the code is in `include/nav_track.h`. On the serial port: `track`, `tracknew`, and `tracktest` to add made-up points on the desk.

## Anchor watch

The ⚓ button of the web app (under the MOB one) sets the anchor at the boat's position with a swinging radius (20-150 m, or any from 10 to 500). The node keeps point and radius on its flash and checks the distance once a second, also with the phone off; after a restart the watch goes on. Outside the circle for 8 seconds in a row, or without a GPS position for a minute, the alarm goes on: red banner and sound in the web app, `ANCHOR DRAG` on the bottom line of the display, and on the optional buzzer five beeps every two seconds, also with the alarm sound switched off. Back inside for 20 seconds it ends on its own. The **PRG** key, or Silence in the web app, quiets it for five minutes. Weigh anchor from the web app.

## Compass, dead reckoning and black box

The same as on the Linux node (Navplotter-Linux, `navplotter/imu.py`, `stima.py`, `blackbox.py`): the filter and the compass reader are ported line by line, with the same numbers, and were checked against the Python ones on a simulated passage (same results to the last digit). V3 only for the compass and the black box; the dead reckoning runs on the V2 too, with the GPS alone.

**Compass (BNO08x: BNO080, BNO085, BNO086)** on its own I2C bus, so that it does not disturb the display: **3V3** (not Ve), **GND**, **SDA to GPIO 41**, **SCL to GPIO 42**, short wires. Address 0x4A or 0x4B, found by itself; no library. The node looks for it at start and every 10 seconds, so it can be plugged in later. Board flat, chip up, X arrow towards the bow, away from iron, speakers and cables with current. It calibrates itself: after some slow turns the calibration in Setup goes to 2 or 3. On the serial port `imu` shows what it reads; `set imuoffset 90` adds 90 degrees to the heading if the board is mounted turned (the dead reckoning uses only the changes of the heading, so it does not need it). With the compass the display shows `GPS 9 sat  HDG 118`.

**Compass in the web app** (`web/navplotter-bussola.js`). The Compass page turns with the compass heading (magnetic) when there is one, else with the GPS course, else, without a fix, with the estimated course, and says which under the number. On the chart a small see-through compass rose, heading up, shows the same heading (`M` compass, `COG` GPS, `STIMA` estimate in amber; `M ?` while the compass is not calibrated yet): a long press moves it, a tap opens the Compass page.

**Dead reckoning.** While the GPS has a fix a Kalman filter follows it in its shadow. Without a fix for 5 seconds the web app draws the boat at the estimated position, in amber, inside the ellipse that holds it 95 times in 100, with a banner; with the compass it follows the turns. After 30 minutes, or with an ellipse wider than two miles, it is declared not reliable. The display shows `NO GPS 3:20 DR 340m` (` H` when the compass steers it). The estimate never goes into the node's own position: it is not transmitted, not added to the track. `dr` on the serial port shows it.

**Black box** on the SD card, in `/log`, one file a day (`2026-11-26.jsonl`) in the same format as the Linux node, so `tools/stima_replay.py` there replays it: GPS once a second when moving (once a minute when still), compass the same, dead reckoning every 10 seconds. Nothing is written before the GPS has given the date once (the board has no clock). Days older than 30 are deleted; closed days are not compressed. Setup > Download the recordings gives them all in a zip (`/api/log.zip`, or `/api/log.zip?day=2026-11-26` for one day); it is slow over WiFi, about 11 MB per day of sailing, and the black box pauses during the download. `log` on the serial port shows its state.

## Buzzer

An optional buzzer on **pin 2** (pin 13 on the V2) gives the collision alarm a voice that does not need the phone: three beeps every three seconds while a node is on a collision course, and two beeps when a message comes in. The pin gives a 2.7 kHz square wave at 3.3 V: a passive piezo buzzer goes straight between the pin and GND; anything that takes more than a few milliamps needs a transistor. It follows the "alarm sound" setting, and the Silence button of the web app quiets it for a minute. On the serial port, `beep` gives three beeps and `sd` lists what is on the card.

## Heltec WiFi LoRa 32 V2

The same firmware runs on the older V2 (ESP32, SX1276 radio, 20 dBm), which talks on the air with all the other NavPlotters:

```
~/.platformio/penv/bin/pio run -e heltec_wifi_lora_32_V2 -t upload
```

Differences from the V3: the GPS goes on other pins (GPS TX -> pin 23, GPS RX -> pin 17, optional), the battery voltage is not measured, there is no SD card module. If the display does not answer the node works without it. Tried on one board: start-up, radio reception, WiFi; the display of that board did not answer, so the display on a V2 has not been seen working.

## Not done yet

SD card tiles tested on real hardware, NMEA output for other navigation apps.
