# PicoPIOUSB (Pico-PIO-USB)

A software USB host (and device) made from the RP2040/RP2350's PIO, used by the `rp2350-pizero`
profile: the arduino-pico core's TinyUSB uses it as the host controller for the board's PIO-USB
port (keyboards, gamepads, hubs).

- **Original:** [sekigon-gonnoc/Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB),
  release 0.7.2 (commit `3c1eec341a5232640e4c00628b889b641af34b28`), `src/` only.
- **Licence:** MIT (see `LICENSE`, from that repository).
- **Why a copy:** the registry package depends on a second copy of Adafruit TinyUSB (and through
  it SdFat and others), which clash with the ones built into the arduino-pico core.
- **Changes in micro-js:** none; `library.json` added so PlatformIO builds it.
