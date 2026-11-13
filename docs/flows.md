# How the gauge decides

The gauge's processes as diagrams - [Mermaid](https://mermaid.js.org), which
GitHub draws from the text below. Edit the text with the code it describes,
in the same commit; there is nothing to regenerate. Each names the file that
holds the real rules.

- [Where the logs go](#where-the-logs-go)
- [The log upload](#the-log-upload)
- [The gauge's WiFi](#the-gauges-wifi)
- [Firmware updates](#firmware-updates)
- [The screens](#the-screens)
- [Sleep and wake](#sleep-and-wake)

## Where the logs go

Every drive is written to the gauge's SD card, encrypted to the phone's key,
and leaves it two ways: by itself to the home server as the car starts, and to
the phone over the gauge's WiFi. The phone reads them from either.

```mermaid
flowchart LR
  car[Car - CAN] --> gauge
  gps[GNSS 25 Hz] --> gauge
  gauge[Gauge] -->|"drive CSV, GPX, .vbo<br/>encrypted .nge"| card[(SD card)]
  card -->|"as the car starts:<br/>phone's hotspot + WireGuard"| server[(Home server)]
  card -->|"phone on the gauge's WiFi<br/>(sync screen)"| phone[Phone app]
  server -->|"app open, or hourly in the<br/>background - not at night"| phone
  phone -->|"decrypted, with the log key"| folder[(Logs folder<br/>Syncthing)]
```

## The log upload

`firmware/car_gauge_v1/upload.cpp`. Logs first, then a new firmware with the
time they left.

```mermaid
flowchart TD
  idle([Idle]) --> start{"Engine running, clock valid,<br/>card in, nobody on the AP,<br/>not asked for the WiFi,<br/>wake did not begin with a crash?"}
  start -- no --> idle
  start -- yes --> join[Join the phone's hotspot<br/>up to 90 s]
  join -- "no hotspot" --> fail
  join --> tunnel[WireGuard tunnel home<br/>resolve the server through it]
  tunnel -- "no handshake / no name" --> fail
  tunnel --> plan["POST /plan: what the server lacks"]
  plan --> send[Send the files, 3 at once<br/>until done or the time limit]
  send --> fw{Time left?}
  fw -- yes --> getfw["/fw/latest: newer than this?<br/>download - resumes a cut-off one -<br/>check the signature, stage it"]
  fw -- no --> done
  getfw --> done([Done this wake])
  fail[Failed try] --> tries{"8 tries this wake?"}
  tries -- no --> wait[Again in 1 min] --> start
  tries -- yes --> stop([No more this wake])
  ask[[WiFi screen shown, or a phone on the AP]] -. "stops at any wait<br/>not a failed try" .-> wait
```

## The gauge's WiFi

`firmware/car_gauge_v1/car_gauge_v1.ino`, `apTask`. With a home server set up
(setting "Gauge WiFi only from its screen", on by default) it is up only when
asked for; without one, whenever the gauge is awake and the car not driving.

```mermaid
flowchart TD
  pass([Every loop pass]) --> asked{"WiFi screen shown, or a phone on it,<br/>or within 3 min of either?"}
  asked -- yes --> giveway[The upload gives way:<br/>a running one stops, none starts]
  giveway --> up[AP up]
  asked -- no --> mode{"Only from its screen,<br/>and a server set up?"}
  mode -- yes --> down[AP down - the radio is free]
  mode -- no --> driving{"Driving, no phone on it,<br/>or the upload has the radio?"}
  driving -- yes --> down
  driving -- no --> up
```

## Firmware updates

`firmware/car_gauge_v1/ota.cpp`, `upload.cpp`, `mobile/src/lib/fw-download.ts`.
Every update is checked before it runs, and a new firmware is on probation: a
crash or a watchdog before it confirms itself boots the previous one again.

```mermaid
flowchart TD
  rel["A v* tag: CI builds and signs"] --> signed[Image + Ed25519 signature<br/>over SHA-256 + build time]
  signed --> gh[(GitHub release)]
  gh -->|"HTTPS, passed through"| srv[(Home server /fw)]
  srv -->|"plain HTTP in the tunnel, after the logs upload,<br/>newer only, resumable"| check1[Gauge checks the signature<br/>against its own key]
  gh -->|"app open with internet,<br/>once a day or its button"| check2[App checks the signature<br/>against the gauge's key]
  check1 --> staged[/firmware.bin on the card/]
  staged -->|"car parked, before sleep"| install
  check2 -->|"Gauge settings: Install"| install
  manual[A .bin picked in the app,<br/>or put on the card] --> install
  install[Write to the other app slot<br/>watchdog fed] --> boot[Reboot into it - on probation]
  boot --> ok{"Runs 30 s,<br/>or reaches sleep?"}
  ok -- yes --> confirmed([Confirmed])
  ok -- "crash / watchdog first" --> rollback([Rolled back to the previous])
```

## The screens

`firmware/car_gauge_v1/car_gauge_v1.ino` (`bootSplashTask`, `syncTask`,
`saverTask`, `viewTask`), `ui.cpp`.

```mermaid
stateDiagram-v2
  [*] --> Splash: boot
  Splash --> Gauges: the car sends a reading, a touch, or no car
  Gauges --> Gauges: swipe - the order set in the app
  Gauges --> WiFi: swipe to it
  WiFi --> Gauges: 60 s untouched
  Gauges --> Saver: car silent, 60 s untouched
  Saver --> Gauges: a touch, or the car talks
  Gauges --> Sync: phone pulling files, parked
  Sync --> Gauges: done, or a touch
  Gauges --> Status: swipe - if shown in the app
  note right of Saver: the N badge, dim and still
  note right of Status: upload step, last upload,<br/>firmware staged, CAN link
```

## Sleep and wake

`firmware/car_gauge_v1/podstate.h` (what holds sleep off), `power.cpp` (how it
sleeps and what wakes it). Deep sleep draws ~11 mA.

```mermaid
flowchart TD
  awake([Awake]) --> hold{"Anything holding it?<br/>car talking, moving, a touch,<br/>a phone on the WiFi, USB host,<br/>an upload running<br/>- each until 'Sleep after' passes"}
  hold -- yes --> awake
  hold -- no --> staged{Firmware staged?}
  staged -- yes --> install[Install it, reboot] --> awake
  staged -- no --> sleep[Close the logs, note the reason<br/>in sleeplog.txt, deep sleep]
  sleep --> wake{Wakes on}
  wake -->|"CAN line: the car"| awake
  wake -->|"the button"| awake
  wake -->|"motion: the door opening<br/>off after many empty wakes"| awake
  wake -->|"timer: hourly, or when the<br/>CAN line is stuck"| awake
```
