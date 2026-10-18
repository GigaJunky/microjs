# libdvi

DVI video output for the RP2040 and RP2350 from PIO and DMA, used by the `rp2350-pizero` profile
to drive the Waveshare RP2350-PiZero's mini HDMI port.

- **Original:** `software/libdvi` of [Wren6991/PicoDVI](https://github.com/Wren6991/PicoDVI) by
  Luke Wren, commit `dccd738bfa9af75badcb32acde3e41bd6a3fa30a`.
- **Licence:** BSD-3-Clause (see `LICENSE`, from that repository).
- `src/dvi_serialiser.pio.h` is the header generated from `dvi_serialiser.pio` (PicoDVI's build
  makes it with pioasm), taken from [Adafruit's PicoDVI fork](https://github.com/adafruit/PicoDVI)
  (same licence); the `.pio` program in both repositories is identical.
- **Changes in micro-js:** none; `library.json` added so PlatformIO builds it.
