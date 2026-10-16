// hw.cpp - JavaScript bindings for the board's hardware: display, touch, LED, SD, sound, WiFi.
// Everything board-specific (pins, which parts exist) comes from board.h.
#include <Arduino.h>
#include <SPI.h>
#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <algorithm>
#include <esp_heap_caps.h>
#include <sys/time.h>
#include <time.h>
#if JS_WIFI
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#endif
#include <memory>
#include <Update.h>
#include <mbedtls/base64.h>
#include "board.h"
#ifdef BOARD_SD_MMC_CLK
#include <SD_MMC.h>
#define SDCARD SD_MMC
#else
#define SDCARD SD
#endif
#include <Wire.h>  // touch / keyboard / codec, and sys.i2c*()
#include "hw.h"
#include "version.h"

using namespace js;

static const int CH_R = 0, CH_G = 1, CH_B = 2, CH_BL = 3, CH_SPK = 6;  // LEDC channels

#ifdef BOARD_SCREEN_HEADER
#include BOARD_SCREEN_HEADER  // board-supplied TFT_eSPI-compatible screen (e.g. VGA, see board.h)
static BOARD_SCREEN_CLASS tft;
#else
static TFT_eSPI tft;
#endif
static bool sdOk = false, fsOk = false;
static fs::FS* pickFs(const std::string& path, std::string& rel);  // "/sd/..." or the flash
#if JS_WIFI
static WebServer* webServer = nullptr;  // web.start's server (see "web:" below)
static DNSServer* dnsServer = nullptr;  // wifi.ap's captive portal: every name -> the board
static void webStop();
#endif
// Sound level for tones and music: audio.volume(0..100) and audio.mute(on). On a PWM beeper only
// silence is possible, so there any volume above 0 sounds the same.
static volatile int spkVolume = 100;
static volatile bool spkMuted = false;
static int rotation = BOARD_ROTATION;
static uint32_t toneEnd = 0;
static Object* touchHandlers = nullptr;
static Object* keyHandlers = nullptr;
static Object* joyHandlers = nullptr;
static Object* spriteProto = nullptr;

#define IA(i) I.toInt32(JS_ARG(i))
#define FA(i) ((float)I.toNum(JS_ARG(i)))
#define CA(i) colorArg(I, JS_ARG(i))

// ------------------------------------------------------------------ PWM (Arduino core 2.x and 3.x)
#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void pwmAttach(int pin, int ch, int freq) { ledcAttachChannel(pin, freq, 8, ch); }
static void pwmWrite(int pin, int ch, int duty) { (void)ch; ledcWrite(pin, duty); }
static void pwmTone(int pin, int ch, int freq) { (void)ch; ledcWriteTone(pin, freq); }
#else
static void pwmAttach(int pin, int ch, int freq) { ledcSetup(ch, freq, 8); ledcAttachPin(pin, ch); }
static void pwmWrite(int pin, int ch, int duty) { (void)pin; ledcWrite(ch, duty); }
static void pwmTone(int pin, int ch, int freq) { (void)pin; ledcWriteTone(ch, freq); }
#endif

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// ------------------------------------------------------------------ simple parts
#if BOARD_LED_R >= 0
// the RGB LED's pins: the board's, or "ledPins": [r, g, b] in micro-js.json for a board whose
// LED is wired differently (some CYDs have red on GPIO 22 instead of 4) - see ledPinsFromConfig
static int ledPins[3] = {BOARD_LED_R, BOARD_LED_G, BOARD_LED_B};
#endif
static void ledWrite(int r, int g, int b) {
#if BOARD_LED_WS2812 >= 0
  neopixelWrite(BOARD_LED_WS2812, clamp255(r), clamp255(g), clamp255(b));
#elif defined(BOARD_LED_BICOLOR_A)
  int rr = clamp255(r), gg = clamp255(g);  // only one colour at a time: current flows one way
  if (rr >= gg) {
    pwmWrite(BOARD_LED_BICOLOR_B, CH_G, 0);
    pwmWrite(BOARD_LED_BICOLOR_A, CH_R, rr);
  } else {
    pwmWrite(BOARD_LED_BICOLOR_A, CH_R, 0);
    pwmWrite(BOARD_LED_BICOLOR_B, CH_G, gg);
  }
  (void)b;
#elif BOARD_LED_R >= 0
  auto level = [](int v) { return BOARD_LED_ACTIVE_LOW ? 255 - clamp255(v) : clamp255(v); };
  pwmWrite(ledPins[0], CH_R, level(r));
  pwmWrite(ledPins[1], CH_G, level(g));
  pwmWrite(ledPins[2], CH_B, level(b));
#else
  (void)r; (void)g; (void)b;
#endif
}

static void backlightWrite(int level) {
#if BOARD_BACKLIGHT >= 0
  pwmWrite(BOARD_BACKLIGHT, CH_BL, clamp255(level));
#else
  (void)level;
#endif
}

#if BOARD_SPEAKER_I2S_DOUT >= 0
// I2S speaker: a task keeps the DMA fed with a square wave at the current frequency (or silence).
#include <driver/i2s.h>
static const int SPK_RATE = 24000, SPK_AMP = BOARD_SPEAKER_I2S_LEVEL;
static volatile int spkFreq = 0;
static volatile bool mp3Active = false;  // an MP3 is playing: the music task owns the I2S output
static void speakerTask(void*) {
  static int16_t buf[256 * 2];
  uint32_t phase = 0;  // 16.16 fixed point position in the wave
  for (;;) {
    if (mp3Active) { vTaskDelay(20); continue; }
    int f = spkFreq;
    uint32_t step = f > 0 ? (uint32_t)(((uint64_t)f << 16) / SPK_RATE) : 0;
    for (int i = 0; i < 256; i++) {
      int16_t v = 0;
#if BOARD_SPEAKER_ES8311
      if (step) { phase += step; v = (phase & 0x8000) ? SPK_AMP : -SPK_AMP; }  // the codec sets the level
#else
      if (step) { phase += step; v = (phase & 0x8000) ? SPK_AMP * spkVolume / 100 : -SPK_AMP * spkVolume / 100; }
#endif
      buf[i * 2] = buf[i * 2 + 1] = v;
    }
    size_t wrote;
    i2s_write(I2S_NUM_1, buf, sizeof(buf), &wrote, portMAX_DELAY);
  }
}
static void speakerBegin() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = SPK_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 128;  // short buffers: a new note starts within ~20 ms
  cfg.tx_desc_auto_clear = true;
  if (i2s_driver_install(I2S_NUM_1, &cfg, 0, nullptr) != ESP_OK) return;
  i2s_pin_config_t pins = {};
#if BOARD_SPEAKER_I2S_MCLK >= 0
  pins.mck_io_num = BOARD_SPEAKER_I2S_MCLK;  // 256 x the sample rate
#else
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
#endif
  pins.bck_io_num = BOARD_SPEAKER_I2S_BCK;
  pins.ws_io_num = BOARD_SPEAKER_I2S_WS;
  pins.data_out_num = BOARD_SPEAKER_I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_NUM_1, &pins);
#if BOARD_SPEAKER_ES8311
#if BOARD_SPEAKER_I2S_MCLK >= 0
  // ES8311 codec, as Espressif's driver sets it up: reset, clocks from the MCLK pin at 256 x 24 kHz
  // (dividers from its coefficient table), I2S slave with 16-bit words, DAC and output on
  static const uint8_t regs[][2] = {{0x00, 0x1F}, {0x00, 0x00}, {0x00, 0x80}, {0x01, 0x3F}, {0x02, 0x00},
                                    {0x03, 0x10}, {0x04, 0x10}, {0x05, 0x00}, {0x06, 0x03}, {0x07, 0x00},
                                    {0x08, 0xFF}, {0x09, 0x0C}, {0x0A, 0x0C}, {0x0D, 0x01}, {0x0E, 0x02},
                                    {0x12, 0x00}, {0x13, 0x10}, {0x1C, 0x6A}, {0x37, 0x08}, {0x32, BOARD_SPEAKER_ES8311_VOL}};
#else
  // ES8311 codec (settings from M5Unified): clocks from the I2S bit clock, DAC and output on, 0 dB
  static const uint8_t regs[][2] = {{0x00, 0x80}, {0x01, 0xB5}, {0x02, 0x18}, {0x0D, 0x01},
                                    {0x12, 0x00}, {0x13, 0x10}, {0x32, 0xBF}, {0x37, 0x08},
                                    {0x09, 0x0C}, {0x0A, 0x0C}};  // 16-bit I2S words, as we send
#endif
  int fails = 0;
  for (auto& r : regs) {
    Wire.beginTransmission(BOARD_SPEAKER_ES8311);
    Wire.write(r[0]);
    Wire.write(r[1]);
    if (Wire.endTransmission()) fails++;
  }
  if (fails) Serial.printf("speaker: no ES8311 codec answering at I2C 0x%02x\n", BOARD_SPEAKER_ES8311);
#endif
#if BOARD_SPEAKER_AMP_EN >= 0
  pinMode(BOARD_SPEAKER_AMP_EN, OUTPUT);
  digitalWrite(BOARD_SPEAKER_AMP_EN, BOARD_SPEAKER_AMP_ON);
#endif
  xTaskCreatePinnedToCore(speakerTask, "speaker", 2048, nullptr, 3, nullptr, 0);
}
#endif

// ---- AVI files (video.play): a RIFF list of headers, then a 'movi' list of chunks named "00dc"
// (a video frame, here a JPEG) and "01wb" (a piece of the sound, here MP3), interleaved.
// An AviStream walks the movi list giving just one kind of chunk.
static uint32_t rd32(const uint8_t* b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
struct AviStream {
  File f;
  uint32_t pos = 0, end = 0, left = 0;  // next chunk header, end of movi, bytes left in this chunk
  char k0 = 'd', k1 = 'c';              // the chunk kind: "dc" video, "wb" audio
  bool next(uint32_t& size) {  // to the next chunk of our kind; the file is left at its data
    uint8_t h[8];
    while (pos + 8 <= end) {
      if (!f.seek(pos) || f.read(h, 8) != 8) return false;
      uint32_t sz = rd32(h + 4);
      if (!memcmp(h, "LIST", 4)) { pos += 12; continue; }  // a "rec " group: look inside
      uint32_t data = pos + 8;
      pos = data + sz + (sz & 1);
      if (h[2] == k0 && h[3] == k1) { f.seek(data); size = sz; return true; }
    }
    return false;
  }
  int read(uint8_t* buf, int n) {  // our chunks' data, as one continuous stream
    int got = 0;
    while (got < n) {
      if (!left) {
        uint32_t sz;
        if (!next(sz)) break;
        left = sz;
        continue;
      }
      int r = f.read(buf + got, std::min<uint32_t>(left, n - got));
      if (r <= 0) break;
      got += r;
      left -= r;
    }
    return got;
  }
};

// Music (MP3, video sound) plays on any speaker: I2S (a DAC or codec), or a plain PWM pin used as an
// 8-bit DAC. The MP3 engine below writes through music*(), one set per kind of speaker.
#define HAS_MUSIC (BOARD_SPEAKER_I2S_DOUT >= 0 || BOARD_SPEAKER_PWM >= 0)
#if BOARD_SPEAKER_I2S_DOUT >= 0
static void musicBegin() {}
static void musicRate(uint32_t rate) { i2s_set_clk(I2S_NUM_1, rate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO); }
static void musicSilence() { i2s_zero_dma_buffer(I2S_NUM_1); }
static void musicEnd() {
  i2s_zero_dma_buffer(I2S_NUM_1);
  i2s_set_clk(I2S_NUM_1, SPK_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);  // back to tones
}
// pcm holds samples x channels; it's widened to stereo in place (it has room for 2 x samples)
static void musicWrite(int16_t* pcm, int samples, int channels) {
  if (channels == 1)  // mono: copy each sample to both sides, from the end backwards
    for (int i = samples - 1; i >= 0; i--) pcm[i * 2] = pcm[i * 2 + 1] = pcm[i];
#if !BOARD_SPEAKER_ES8311
  for (int i = 0, v = spkMuted ? 0 : spkVolume; i < samples * 2; i++) pcm[i] = pcm[i] * v / 100;  // no codec: scale here
#endif
  size_t wrote;
  i2s_write(I2S_NUM_1, pcm, samples * 2 * sizeof(int16_t), &wrote, portMAX_DELAY);
}
#elif BOARD_SPEAKER_PWM >= 0
// A PWM speaker pin as an 8-bit DAC (as the ESP32-CoCo emulator does): the PWM runs at 156.25 kHz,
// far above hearing, and a timer interrupt sets its duty once per sample from a small queue that
// the MP3 task fills. Mono, roughly AM-radio quality. Both run from the 80 MHz bus clock with whole
// dividers - the PWM at 80 MHz / 2 / 256, the samples at exactly a quarter of that (39062.5 Hz) - so
// every sample lasts exactly 4 PWM cycles. (With unrelated rates, samples last 7 or 8 cycles in a
// repeating pattern, which you hear as a steady whistle.) Songs are resampled to that rate.
static volatile bool mp3Active = false;
static const uint32_t PWM_RING = 2048;     // ~52 ms
static const uint32_t PWM_HZ = 156250;     // 80 MHz / 2 / 256 (8-bit)
static const uint32_t PWM_TICKS = 1024;    // sample period in 40 MHz timer ticks: 4 PWM cycles
static const double PWM_RATE = 40000000.0 / PWM_TICKS;  // 39062.5 samples a second
static uint8_t pwmRing[PWM_RING];
static volatile uint32_t pwmHead = 0, pwmTail = 0;
static hw_timer_t* pwmTimer = nullptr;
static uint32_t pwmStep = 0, pwmPhase = 0;  // resampling: input samples per output sample (16.16)
static int pwmLast = 0;                     // the previous input sample, for interpolation
static void pwmTick() {  // timer interrupt, once per sample (not IRAM: it waits out flash writes)
  uint32_t t = pwmTail;
  if (t != pwmHead) {
    ledcWrite(CH_SPK, pwmRing[t % PWM_RING]);
    pwmTail = t + 1;
  }
}
// The sample interrupt lives on core 1: core 0 has the software USB host's 1 ms interrupt (VGA
// board), which would delay samples every millisecond. An interrupt is handled on the core that
// sets it up, so a short task on core 1 does the setup (and teardown).
static void pwmOnCore1(void (*fn)()) {
  static void (*job)();
  static volatile bool done;
  job = fn;
  done = false;
  xTaskCreatePinnedToCore([](void*) { job(); done = true; vTaskDelete(nullptr); }, "pwmsetup", 3072, nullptr, 5, nullptr, 1);
  for (int i = 0; i < 200 && !done; i++) vTaskDelay(1);
}
static void musicBegin() {
  pwmHead = pwmTail = 0;
  pwmPhase = 0;
  pwmLast = 0;
  pwmStep = (uint32_t)(44100 / PWM_RATE * 65536);
  pwmAttach(BOARD_SPEAKER_PWM, CH_SPK, PWM_HZ);
  ledcWrite(CH_SPK, 128);
  pwmOnCore1([] {
    pwmTimer = timerBegin(3, 2, true);  // 40 MHz count (timer 0 may be the USB host's)
    timerAttachInterrupt(pwmTimer, pwmTick, true);
    timerAlarmWrite(pwmTimer, PWM_TICKS, true);
    timerAlarmEnable(pwmTimer);
  });
}
static void musicRate(uint32_t rate) { pwmStep = (uint32_t)(rate / PWM_RATE * 65536); }
static void musicSilence() { pwmTail = pwmHead; ledcWrite(CH_SPK, 128); }
static void musicEnd() {
  if (pwmTimer) pwmOnCore1([] {
    timerAlarmDisable(pwmTimer);
    timerDetachInterrupt(pwmTimer);
    timerEnd(pwmTimer);
    pwmTimer = nullptr;
  });
  ledcWrite(CH_SPK, 0);
  pwmTone(BOARD_SPEAKER_PWM, CH_SPK, 0);  // ready for tone() again
}
extern volatile bool mp3Stop;
static void musicWrite(int16_t* pcm, int samples, int channels) {
  int v = spkMuted ? 0 : spkVolume;
  for (int i = 0; i < samples; i++) {
    int cur = channels == 2 ? (pcm[2 * i] + pcm[2 * i + 1]) / 2 : pcm[i];
    // output samples that fall between the previous input sample and this one
    while (pwmPhase < 65536) {
      int s = pwmLast + (int)((int64_t)(cur - pwmLast) * pwmPhase >> 16);
      int u = 128 + s * v / 25600;  // 16-bit signed -> 8-bit centred, with the volume
      while (pwmHead - pwmTail >= PWM_RING) {  // queue full: wait for the timer to play some
        if (mp3Stop) return;
        vTaskDelay(1);
      }
      pwmRing[pwmHead % PWM_RING] = u < 0 ? 0 : u > 255 ? 255 : u;
      pwmHead = pwmHead + 1;
      pwmPhase += pwmStep;
    }
    pwmPhase -= 65536;
    pwmLast = cur;
  }
}
#endif

#if HAS_MUSIC
// ---- MP3 playback (audio.play): minimp3 decodes the file on core 0 in its own task, which owns
// the speaker while it runs (tone() is silent meanwhile). The task's stack (~20 KB, the decoder
// keeps its scratch space there) is only allocated while something plays.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"
volatile bool mp3Stop = false;
static volatile bool mp3Paused = false;
static volatile uint32_t mp3Pos = 0, mp3Size = 0, mp3Rate = 0, mp3Kbps = 0, mp3Played = 0;  // played: samples
static volatile uint32_t mp3PlayedAt = 0;  // millis() when mp3Played last moved (a video's clock fills the gaps)
static volatile int mp3Channels = 0;
static TaskHandle_t mp3Task = nullptr;
static std::string mp3Path, mp3Error;
static uint32_t mp3AviStart = 0, mp3AviEnd = 0;  // set: the audio is the MP3 chunks of an AVI's 'movi' list
static int codecVolume = -1;  // the ES8311 volume register as last set
static void codecSetVolume(int reg) {
#if BOARD_SPEAKER_ES8311
  if (reg == codecVolume) return;
  codecVolume = reg;
  Wire.beginTransmission(BOARD_SPEAKER_ES8311);
  Wire.write(0x32);
  Wire.write(reg);
  Wire.endTransmission();
#else
  (void)reg;
#endif
}
// music at 0..100%: the codec's DAC volume in 0.5 dB steps (0 dB at 100%, about -48 dB at 1%)
static void mp3ApplyVolume() {
  int v = spkMuted ? 0 : spkVolume;
  codecSetVolume(v <= 0 ? 0 : 0xBF - (100 - v) * 96 / 100);
}

static void mp3Run(void*) {
  std::string rel;
  fs::FS* fsys = pickFs(mp3Path, rel);
  File f = fsys ? fsys->open(rel.c_str(), "r") : File();
  AviStream avi;  // for a video's sound: only the audio chunks
  bool isAvi = mp3AviEnd > 0;
  if (isAvi) avi = AviStream{f, mp3AviStart, mp3AviEnd, 0, 'w', 'b'};
  const size_t IN = 16 * 1024;
  uint8_t* in = (uint8_t*)heap_caps_malloc(IN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!in) in = (uint8_t*)malloc(IN);
  mp3dec_t* dec = (mp3dec_t*)malloc(sizeof(mp3dec_t));
  int16_t* pcm = (int16_t*)malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * sizeof(int16_t));
  if (!f || !in || !dec || !pcm) mp3Error = !f ? "can't open the file" : "not enough memory";
  else {
    mp3Size = isAvi ? mp3AviEnd - mp3AviStart : f.size();
    uint8_t id3[10];  // skip an ID3v2 tag (cover art in it can look like MP3 frames)
    if (isAvi) {}
    else if (f.read(id3, 10) == 10 && !memcmp(id3, "ID3", 3))
      f.seek(10 + ((id3[6] & 0x7F) << 21 | (id3[7] & 0x7F) << 14 | (id3[8] & 0x7F) << 7 | (id3[9] & 0x7F)));
    else f.seek(0);
    mp3dec_init(dec);
    musicBegin();
    size_t have = 0;
    bool eof = false;
    uint32_t rate = 0;
    while (!mp3Stop) {
      if (mp3Paused) { musicSilence(); vTaskDelay(50); continue; }
      if (!eof && have < IN / 2) {  // keep the input buffer topped up
        int n = isAvi ? avi.read(in + have, IN - have) : f.read(in + have, IN - have);
        if (n <= 0) eof = true; else have += n;
      }
      if (!have) break;
      mp3dec_frame_info_t info;
      int samples = mp3dec_decode_frame(dec, in, have, pcm, &info);
      size_t used = info.frame_bytes;
      if (!used) { if (eof) break; continue; }
      memmove(in, in + used, have - used);
      have -= used;
      mp3Pos = isAvi ? avi.pos - mp3AviStart : f.position() - have;
      if (!samples) continue;  // skipped junk, or the decoder's first frame
      if ((uint32_t)info.hz != rate) {
        rate = info.hz;
        mp3Rate = rate;
        musicRate(rate);
      }
      mp3Channels = info.channels;
      mp3Kbps = info.bitrate_kbps;
      musicWrite(pcm, samples, info.channels);
      mp3Played += samples;
      mp3PlayedAt = millis();
    }
  }
  if (f) f.close();
  free(in);
  free(dec);
  free(pcm);
  musicEnd();
  mp3Active = false;
  mp3Task = nullptr;
  vTaskDelete(nullptr);
}
static void mp3StopWait() {
  if (!mp3Task) return;
  mp3Stop = true;
  for (int i = 0; i < 200 && mp3Task; i++) delay(5);
}
// Start playing an MP3 file, or (aviEnd set) the MP3 sound inside an AVI. "" or why not.
static std::string mp3Start(const std::string& path, uint32_t aviStart = 0, uint32_t aviEnd = 0) {
  mp3StopWait();
  if (mp3Task) return "the previous song didn't stop";
  mp3Path = path;
  mp3AviStart = aviStart;
  mp3AviEnd = aviEnd;
  mp3Error.clear();
  mp3Stop = mp3Paused = false;
  mp3Pos = mp3Size = mp3Rate = mp3Kbps = mp3Played = 0;
  mp3ApplyVolume();
  mp3Active = true;
  if (xTaskCreatePinnedToCore(mp3Run, "mp3", 20 * 1024, nullptr, 4, &mp3Task, 0) != pdPASS) {
    mp3Active = false;
    mp3Task = nullptr;
    return "not enough memory to play";
  }
  return "";
}
#endif

static void toneWrite(int freq) {
  if (spkMuted || spkVolume <= 0) freq = 0;
#if BOARD_SPEAKER_PWM >= 0
  if (mp3Active) return;  // the pin is playing music
  pwmTone(BOARD_SPEAKER_PWM, CH_SPK, freq);
#elif BOARD_SPEAKER_I2S_DOUT >= 0
#if BOARD_SPEAKER_ES8311
  // tones: the board's tone level at 100%, then 0.5 dB a step lower (about 5 dB per 10%)
  if (freq > 0 && !mp3Active) codecSetVolume(std::max(1, BOARD_SPEAKER_ES8311_VOL - (100 - spkVolume) * 96 / 100));
#endif
  spkFreq = freq;
#else
  (void)freq;
#endif
}

// "#rgb", "#rrggbb" or a 16-bit RGB565 number
static uint32_t colorArg(Interp& I, Value v) {
  if (v.isStr()) {
    const std::string& s = v.s()->s;
    if (!s.empty() && s[0] == '#') {
      uint32_t x = strtoul(s.c_str() + 1, nullptr, 16);
      uint8_t r, g, b;
      if (s.size() == 4) { r = ((x >> 8) & 15) * 17; g = ((x >> 4) & 15) * 17; b = (x & 15) * 17; }
      else { r = (x >> 16) & 255; g = (x >> 8) & 255; b = x & 255; }
      return tft.color565(r, g, b);
    }
  }
  return (uint32_t)I.toInt32(v) & 0xFFFF;
}

// ------------------------------------------------------------------ touch
// touchRaw() reads the controller; toScreen() maps its coordinates to the current rotation.
static float tcal[6];    // a calibration from calibrate.js (see touch.calibrate), if tcalRot >= 0
static int tcalRot = -1;
static const int PANEL_W = TFT_WIDTH, PANEL_H = TFT_HEIGHT;  // native (rotation 0) size

#if BOARD_TOUCH == TOUCH_XPT2046
// Resistive XPT2046, bit-banged so the SD card can have a hardware SPI bus to itself.
static int cal[4] = {BOARD_XPT_CAL};  // raw x min, x max, y min, y max

static uint16_t xptTransfer(uint8_t cmd) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(BOARD_XPT_MOSI, (cmd >> i) & 1);
    delayMicroseconds(1);
    digitalWrite(BOARD_XPT_CLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(BOARD_XPT_CLK, LOW);
  }
  digitalWrite(BOARD_XPT_MOSI, LOW);
  uint16_t v = 0;
  for (int i = 0; i < 16; i++) {
    delayMicroseconds(1);
    digitalWrite(BOARD_XPT_CLK, HIGH);
    delayMicroseconds(1);
    v = (v << 1) | (digitalRead(BOARD_XPT_MISO) ? 1 : 0);
    digitalWrite(BOARD_XPT_CLK, LOW);
  }
  return v >> 3;
}

static int bestTwoAvg(int a, int b, int c) {
  int ab = abs(a - b), ac = abs(a - c), bc = abs(b - c);
  if (ab <= ac && ab <= bc) return (a + b) / 2;
  if (ac <= ab && ac <= bc) return (a + c) / 2;
  return (b + c) / 2;
}

// raw 12-bit readings; x is the panel's long axis
static bool touchRaw(int& x, int& y, int& z) {
  digitalWrite(BOARD_XPT_CS, LOW);
  int z1 = xptTransfer(0xB1), z2 = xptTransfer(0xC1);
  z = z1 + 4095 - z2;
  if (z < 400) {
    xptTransfer(0x90);  // power down, PENIRQ enabled
    digitalWrite(BOARD_XPT_CS, HIGH);
    return false;
  }
  xptTransfer(0x91);  // first reading is noisy
  int xs[3], ys[3];
  for (int i = 0; i < 3; i++) {
    xs[i] = xptTransfer(0x91);
    ys[i] = xptTransfer(0xD1);
  }
  xptTransfer(0x90);
  digitalWrite(BOARD_XPT_CS, HIGH);
  x = bestTwoAvg(xs[0], xs[1], xs[2]);
  y = bestTwoAvg(ys[0], ys[1], ys[2]);
  return true;
}

static void toScreen(int rx, int ry, int& sx, int& sy) {
  const int L = PANEL_H, S = PANEL_W;  // long and short side
  int x1 = constrain(map(rx, cal[0], cal[1], 0, L - 1), 0, L - 1);
  int y1 = constrain(map(ry, cal[2], cal[3], 0, S - 1), 0, S - 1);
  switch (rotation & 3) {
    case 0: sx = S - 1 - y1; sy = x1; break;
    case 1: sx = x1; sy = y1; break;
    case 2: sx = y1; sy = L - 1 - x1; break;
    default: sx = L - 1 - x1; sy = S - 1 - y1; break;
  }
}

static void touchBegin() {
  pinMode(BOARD_XPT_CLK, OUTPUT);
  pinMode(BOARD_XPT_MOSI, OUTPUT);
  pinMode(BOARD_XPT_CS, OUTPUT);
  pinMode(BOARD_XPT_MISO, INPUT);
  digitalWrite(BOARD_XPT_CS, HIGH);
  digitalWrite(BOARD_XPT_CLK, LOW);
}

#elif BOARD_TOUCH == TOUCH_FT6336
// Capacitive FT6336U-family controller on I2C: reports panel pixels directly.
static int cal[4] = {0, 0, 0, 0};  // unused (capacitive panels need no calibration)

static bool touchRaw(int& x, int& y, int& z) {
  Wire.beginTransmission(BOARD_TOUCH_ADDR);
  Wire.write(0x02);  // TD_STATUS, then P1_XH, P1_XL, P1_YH, P1_YL
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)BOARD_TOUCH_ADDR, 5) != 5) return false;
  uint8_t b[5];
  for (int i = 0; i < 5; i++) b[i] = Wire.read();
  if ((b[0] & 0x0F) == 0) return false;
  x = ((b[1] & 0x0F) << 8) | b[2];
  y = ((b[3] & 0x0F) << 8) | b[4];
  z = 1000;
  return true;
}

static void toScreen(int rx, int ry, int& sx, int& sy) {
  int px = rx, py = ry;
  if (BOARD_TOUCH_SWAP_XY) std::swap(px, py);
  if (BOARD_TOUCH_FLIP_X) px = PANEL_W - 1 - px;
  if (BOARD_TOUCH_FLIP_Y) py = PANEL_H - 1 - py;
  px = constrain(px, 0, PANEL_W - 1);
  py = constrain(py, 0, PANEL_H - 1);
  switch (rotation & 3) {  // panel (portrait) coordinates -> rotated screen
    case 0: sx = px; sy = py; break;
    case 1: sx = py; sy = PANEL_W - 1 - px; break;
    case 2: sx = PANEL_W - 1 - px; sy = PANEL_H - 1 - py; break;
    default: sx = PANEL_H - 1 - py; sy = px; break;
  }
}

static void touchBegin() {
  if (BOARD_TOUCH_RST >= 0) {
    pinMode(BOARD_TOUCH_RST, OUTPUT);
    digitalWrite(BOARD_TOUCH_RST, LOW);
    delay(10);
    digitalWrite(BOARD_TOUCH_RST, HIGH);
    delay(300);
  }
  Wire.begin(BOARD_TOUCH_SDA, BOARD_TOUCH_SCL, 400000);
}

#else
static int cal[4] = {0, 0, 0, 0};
static bool touchRaw(int&, int&, int&) { return false; }
static void toScreen(int, int, int& sx, int& sy) { sx = sy = 0; }
static void touchBegin() {}
#endif

// A raw touch to screen coordinates: the calibration if there is one, else the board's mapping.
// The calibration is in its own rotation's screen coordinates: those go to the panel's (rotation 0)
// and then to the current rotation.
static void mapTouch(int rx, int ry, int& sx, int& sy) {
  if (tcalRot < 0) return toScreen(rx, ry, sx, sy);
  const int PW = TFT_WIDTH, PH = TFT_HEIGHT;
  int cx = (int)lroundf(tcal[0] * rx + tcal[1] * ry + tcal[2]), cy = (int)lroundf(tcal[3] * rx + tcal[4] * ry + tcal[5]);
  int px, py;
  switch (tcalRot) {
    case 0: px = cx; py = cy; break;
    case 1: px = PW - 1 - cy; py = cx; break;
    case 2: px = PW - 1 - cx; py = PH - 1 - cy; break;
    default: px = cy; py = PH - 1 - cx; break;
  }
  switch (rotation & 3) {
    case 0: sx = px; sy = py; break;
    case 1: sx = py; sy = PW - 1 - px; break;
    case 2: sx = PW - 1 - px; sy = PH - 1 - py; break;
    default: sx = PH - 1 - py; sy = px; break;
  }
  sx = constrain(sx, 0, tft.width() - 1);
  sy = constrain(sy, 0, tft.height() - 1);
}
// /touchcal.json, saved by calibrate.js: {"matrix":[a,b,c,d,e,f],"rotation":r}
static void loadTouchCal() {
  std::string t;
  if (!hwReadFile("/touchcal.json", t)) return;
  size_t m = t.find('['), r = t.find("\"rotation\"");
  if (m == std::string::npos || r == std::string::npos) return;
  const char* p = t.c_str() + m + 1;
  for (int i = 0; i < 6; i++) {
    char* e;
    tcal[i] = strtof(p, &e);
    if (e == p) return;
    p = e;
    while (*p == ',' || *p == ' ') p++;
  }
  tcalRot = atoi(t.c_str() + t.find(':', r) + 1) & 3;
}


// ------------------------------------------------------------------ keyboard
// Keys handled by the firmware itself (read and cleared by the main loop): F12 or Escape go back to
// the menu (like BOOT), Ctrl+C stops the running app and leaves you at the console. An app can
// claim Escape and Ctrl+C for itself with keyboard.capture(true) (the editor does).
static volatile bool menuKey = false, stopKey = false, keyCapture = false;
static volatile bool conActive = false;  // the on-screen console is showing (see below)
bool hwMenuKey() {
  bool k = menuKey;
  menuKey = false;
  return k;
}
bool hwStopKey() {
  bool k = stopKey;
  stopKey = false;
  return k;
}
void __attribute__((weak)) hwConsoleKey(const char*, uint8_t) {}  // main.cpp feeds these to the REPL

#if BOARD_KEYBOARD == KEYBOARD_CARDPUTER_TCA8418
// Cardputer ADV: TCA8418 key-matrix controller; layout as printed on the keys (M5Stack's map).
// Fn + ; , . / are the arrow keys, Fn + ` is Escape, Fn + Backspace is Delete.
static const char* const KB_KEYS[2][4][14] = {
  {{"`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "Backspace"},
   {"Tab", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "[", "]", "\\"},
   {"Fn", "Shift", "a", "s", "d", "f", "g", "h", "j", "k", "l", ";", "'", "Enter"},
   {"Control", "Opt", "Alt", "z", "x", "c", "v", "b", "n", "m", ",", ".", "/", " "}},
  {{"~", "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "_", "+", "Backspace"},
   {"Tab", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "{", "}", "|"},
   {"Fn", "Shift", "A", "S", "D", "F", "G", "H", "J", "K", "L", ":", "\"", "Enter"},
   {"Control", "Opt", "Alt", "Z", "X", "C", "V", "B", "N", "M", "<", ">", "?", " "}}};

static bool kbDown[4][14];
static uint8_t kbRepeatRow = 255, kbRepeatCol = 0;
static uint32_t kbRepeatAt = 0;

static uint8_t kbRead(uint8_t reg) {
  Wire.beginTransmission(BOARD_KEYBOARD_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((int)BOARD_KEYBOARD_ADDR, 1) != 1) return 0;
  return Wire.read();
}
static void kbWrite(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(BOARD_KEYBOARD_ADDR);
  Wire.write(reg);
  Wire.write(v);
  Wire.endTransmission();
}

static void keyboardBegin() {
  Wire.begin(BOARD_KEYBOARD_SDA, BOARD_KEYBOARD_SCL, 400000);
  kbWrite(0x1D, 0x7F);  // KP_GPIO1: rows 0-6 are part of the key matrix
  kbWrite(0x1E, 0xFF);  // KP_GPIO2: columns 0-7
  kbWrite(0x1F, 0x00);
  kbWrite(0x01, kbRead(0x01) | 0x01);  // CFG: key events into the FIFO
  while (kbRead(0x04)) {}  // flush old events
  kbWrite(0x02, 0x03);  // clear interrupt status
}

static const char* keyName(int row, int col) {
  bool fn = kbDown[2][0], shift = kbDown[2][1];
  if (fn) {
    if (row == 2 && col == 11) return "ArrowUp";
    if (row == 3 && col == 10) return "ArrowLeft";
    if (row == 3 && col == 11) return "ArrowDown";
    if (row == 3 && col == 12) return "ArrowRight";
    if (row == 0 && col == 0) return "Escape";
    if (row == 0 && col == 13) return "Delete";
  }
  return KB_KEYS[shift ? 1 : 0][row][col];
}
static bool isModifier(int row, int col) { return (row == 2 && col < 2) || (row == 3 && col < 3); }

static void fireKey(Interp& I, const char* ev, int row, int col);

static void keyboardPoll(Interp& I) {
  int n = kbRead(0x03) & 0x0F;  // KEY_LCK_EC: events waiting
  for (int i = 0; i < n; i++) {
    uint8_t e = kbRead(0x04);
    if (!e) break;
    bool pressed = e & 0x80;
    int raw = (e & 0x7F) - 1, r = raw / 10, c = raw % 10;
    int col = r * 2 + (c > 3 ? 1 : 0), row = (c + 4) % 4;  // controller -> printed layout
    if (row > 3 || col > 13) continue;
    kbDown[row][col] = pressed;
    if (isModifier(row, col)) continue;
    // the top-left key (` / Esc, with or without Fn) goes back to the menu, and Ctrl+C stops the app,
    // unless the console is showing or the app asked for them with keyboard.capture(true)
    if (!conActive && !keyCapture && ((row == 0 && col == 0) || (kbDown[3][0] && !strcmp(KB_KEYS[0][row][col], "c")))) {
      if (pressed) (row == 0 ? menuKey : stopKey) = true;
      continue;
    }
    fireKey(I, pressed ? "key" : "keyup", row, col);
    if (pressed) { kbRepeatRow = row; kbRepeatCol = col; kbRepeatAt = millis() + 400; }
    else if (row == kbRepeatRow && col == kbRepeatCol) kbRepeatRow = 255;
  }
  if (n) kbWrite(0x02, 0x03);
  if (kbRepeatRow != 255 && kbDown[kbRepeatRow][kbRepeatCol] && (int32_t)(millis() - kbRepeatAt) >= 0) {
    kbRepeatAt = millis() + 70;  // auto-repeat while held
    fireKey(I, "key", kbRepeatRow, kbRepeatCol);
  }
}

static Value heldKeys(Interp& I) {
  Array* a = I.newArray();
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 14; c++)
      if (kbDown[r][c]) a->items.push_back(I.str(keyName(r, c)));
  return Value::obj(a);
}
#elif BOARD_KEYBOARD == KEYBOARD_EXTERNAL
// Keys from a separate driver (e.g. a USB HID keyboard serviced by its own task), handed over
// with hwKeyEvent() through a small locked queue. Names follow the browser's KeyboardEvent.key
// ("a", "A", "Enter", "ArrowUp", ...); mods: 1 shift, 2 ctrl, 4 alt, 8 meta.
static const int EXT_Q = 32;
static struct { char key[16]; uint8_t mods; bool down; } extQ[EXT_Q];
static int extHead = 0, extTail = 0;
static portMUX_TYPE extMux = portMUX_INITIALIZER_UNLOCKED;
static std::vector<std::string> extHeld;
static std::string extRepeatKey;
static uint8_t extMods = 0;
static uint32_t extRepeatAt = 0;
void hwKeyEvent(const char* key, bool down, uint8_t mods) {
  bool ctrlC = (mods & 2) && (key[0] == 'c' || key[0] == 'C') && !key[1];
  bool esc = !strcmp(key, "Escape");
  if (!strcmp(key, "F12")) {  // the ESP32-CoCo menu key: always back to the menu
    if (down) menuKey = true;
    return;
  }
  if ((esc || ctrlC) && !conActive && !keyCapture) {  // the console and capturing apps get them as keys
    if (down) (esc ? menuKey : stopKey) = true;
    return;
  }
  portENTER_CRITICAL(&extMux);
  int next = (extHead + 1) % EXT_Q;
  if (next != extTail) {
    strncpy(extQ[extHead].key, key, sizeof extQ[0].key - 1);
    extQ[extHead].key[sizeof extQ[0].key - 1] = 0;
    extQ[extHead].mods = mods;
    extQ[extHead].down = down;
    extHead = next;
  }
  portEXIT_CRITICAL(&extMux);
}
void __attribute__((weak)) hwExternalKeyboardBegin() {}
static void keyboardBegin() { hwExternalKeyboardBegin(); }
static void fireExtKey(Interp& I, const char* ev, const std::string& key) {
  Prop* p = keyHandlers->own(ev);
  if (!p || !I.isCallable(p->v)) {
    if (keyHandlers->props.empty() && !strcmp(ev, "key")) hwConsoleKey(key.c_str(), extMods);
    return;
  }
  size_t mark = I.temps.size();
  Value fn = p->v;
  I.rootTemp(fn);
  Object* e = I.newObject();
  I.setOwn(e, "key", I.str(key));
  I.setOwn(e, "shift", Value::boolean(extMods & 1));
  I.setOwn(e, "ctrl", Value::boolean(extMods & 2));
  I.setOwn(e, "alt", Value::boolean(extMods & 4));
  I.setOwn(e, "meta", Value::boolean(extMods & 8));
  I.setOwn(e, "fn", Value::boolean(false));
  I.setOwn(e, "opt", Value::boolean(false));
  I.callSafe(fn, Value(), {Value::obj(e)});
  I.temps.resize(mark);
}
static void keyboardPoll(Interp& I) {
  for (;;) {
    char key[16];
    bool down = false, any;
    portENTER_CRITICAL(&extMux);
    any = extTail != extHead;
    if (any) {
      memcpy(key, extQ[extTail].key, sizeof key);
      extMods = extQ[extTail].mods;
      down = extQ[extTail].down;
      extTail = (extTail + 1) % EXT_Q;
    }
    portEXIT_CRITICAL(&extMux);
    if (!any) break;
    auto it = std::find(extHeld.begin(), extHeld.end(), key);
    if (down && it == extHeld.end()) extHeld.push_back(key);
    if (!down && it != extHeld.end()) extHeld.erase(it);
    if (down) { extRepeatKey = key; extRepeatAt = millis() + 400; }
    else if (extRepeatKey == key) extRepeatKey.clear();
    fireExtKey(I, down ? "key" : "keyup", key);
  }
  if (!extRepeatKey.empty() && (int32_t)(millis() - extRepeatAt) >= 0) {
    extRepeatAt = millis() + 70;  // auto-repeat while held, like the other keyboards
    fireExtKey(I, "key", extRepeatKey);
  }
}
static Value heldKeys(Interp& I) {
  Array* a = I.newArray();
  for (auto& k : extHeld) a->items.push_back(I.str(k));
  return Value::obj(a);
}

// ---- joysticks (from the same kind of external driver)
// The driver calls hwJoystickUpdate() from its task; state is copied under a lock. Unless an app
// turns it off with joystick.keys(false), the D-pad / left stick also sends arrow keys and buttons 1
// and 2 send Space and Enter, so menus and keyboard games work with a gamepad as they are.
static HwJoystick joy[HW_JOYSTICKS];
static portMUX_TYPE joyMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool joyKeys = true;
static uint8_t joyKeyState[HW_JOYSTICKS];  // bits: left, right, up, down, button 1, button 2
static const char* const JOY_KEY_NAMES[] = {"ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", " ", "Enter"};
void hwJoystickUpdate(int n, const HwJoystick& s) {
  if (n < 0 || n >= HW_JOYSTICKS) return;
  portENTER_CRITICAL(&joyMux);
  joy[n] = s;
  portEXIT_CRITICAL(&joyMux);
  // key emulation, with some hysteresis on the stick
  uint8_t was = joyKeyState[n], now = 0;
  int x = s.axis[0], y = s.axis[1], h = s.hat;
  auto stick = [&](int bit, bool far, bool near) { if (far || ((was >> bit) & 1 && near)) now |= 1 << bit; };
  stick(0, x < -64 || h == 5 || h == 6 || h == 7, x < -40);
  stick(1, x > 64 || h == 1 || h == 2 || h == 3, x > 40);
  stick(2, y < -64 || h == 7 || h == 0 || h == 1, y < -40);
  stick(3, y > 64 || h == 3 || h == 4 || h == 5, y > 40);
  if (s.buttons & 1) now |= 1 << 4;
  if (s.buttons & 2) now |= 1 << 5;
  if (!s.connected) now = 0;
  joyKeyState[n] = now;
  if (!joyKeys) return;
  for (int b = 0; b < 6; b++)
    if (((was ^ now) >> b) & 1) hwKeyEvent(JOY_KEY_NAMES[b], (now >> b) & 1, 0);
}
static void joystickPoll(Interp& I) {  // button press / release events for joystick.on()
  static uint32_t prevButtons[HW_JOYSTICKS];
  for (int n = 0; n < HW_JOYSTICKS; n++) {
    portENTER_CRITICAL(&joyMux);
    uint32_t b = joy[n].connected ? joy[n].buttons : 0;
    portEXIT_CRITICAL(&joyMux);
    uint32_t diff = b ^ prevButtons[n];
    prevButtons[n] = b;
    if (!diff || !joyHandlers) continue;
    for (int i = 0; i < 32; i++) {
      if (!((diff >> i) & 1)) continue;
      const char* ev = ((b >> i) & 1) ? "press" : "release";
      Prop* p = joyHandlers->own(ev);
      if (!p || !I.isCallable(p->v)) continue;
      size_t mark = I.temps.size();
      Value fn = p->v;
      I.rootTemp(fn);
      Object* e = I.newObject();
      I.setOwn(e, "joystick", Value::num(n));
      I.setOwn(e, "button", Value::num(i));
      I.setOwn(e, "pressed", Value::boolean((b >> i) & 1));
      I.callSafe(fn, Value(), {Value::obj(e)});
      I.temps.resize(mark);
    }
  }
}
static HwJoystick joyState(int n) {
  HwJoystick s{};
  if (n < 0 || n >= HW_JOYSTICKS) return s;
  portENTER_CRITICAL(&joyMux);
  s = joy[n];
  portEXIT_CRITICAL(&joyMux);
  return s;
}
#else
static void keyboardBegin() {}
static void keyboardPoll(Interp&) {}
static Value heldKeys(Interp& I) { return Value::obj(I.newArray()); }
#endif
#if BOARD_KEYBOARD != KEYBOARD_EXTERNAL  // joysticks need an external input driver
static volatile bool joyKeys = true;
void hwJoystickUpdate(int, const HwJoystick&) {}
static void joystickPoll(Interp&) {}
static HwJoystick joyState(int) { return HwJoystick{}; }
#endif

JS_NATIVE(keyboard_on) {
  (void)self;
  std::string ev = I.toStr(JS_ARG(0));
  if (ev != "key" && ev != "keyup") I.throwError("TypeError", "keyboard.on: event must be 'key' or 'keyup'");
  if (!I.isCallable(JS_ARG(1))) I.throwError("TypeError", "keyboard.on: handler must be a function");
  I.setOwn(keyHandlers, ev.c_str(), args[1]);
  return Value();
}
JS_NATIVE(keyboard_off) {
  (void)self;
  if (argc == 0) keyHandlers->props.clear();
  else I.delProp(Value::obj(keyHandlers), I.toStr(args[0]));
  return Value();
}
JS_NATIVE(keyboard_held) { (void)self; (void)args; (void)argc; return heldKeys(I); }
JS_NATIVE(joystick_read) {  // joystick.read(n = 0)
  (void)self;
  HwJoystick j = joyState(argc ? IA(0) : 0);
  Object* o = I.newObject();
  auto ax = [](int v) { return Value::num(std::round(v / 127.0 * 1000) / 1000); };  // -1..1
  I.setOwn(o, "connected", Value::boolean(j.connected));
  I.setOwn(o, "x", ax(j.axis[0]));
  I.setOwn(o, "y", ax(j.axis[1]));
  I.setOwn(o, "x2", ax(j.axis[2]));
  I.setOwn(o, "y2", ax(j.axis[3]));
  I.setOwn(o, "hat", Value::num(j.connected ? j.hat : -1));
  I.setOwn(o, "buttons", Value::num(j.buttons));
  Array* raw = I.newArray();
  for (int i = 0; i < j.rawLen; i++) raw->items.push_back(Value::num(j.raw[i]));
  I.setOwn(o, "raw", Value::obj(raw));
  return Value::obj(o);
}
JS_NATIVE(joystick_count) {
  (void)self; (void)args; (void)argc; (void)I;
  int n = 0;
  for (int i = 0; i < HW_JOYSTICKS; i++) n += joyState(i).connected;
  return Value::num(n);
}
JS_NATIVE(joystick_on) {
  (void)self;
  std::string ev = I.toStr(JS_ARG(0));
  if (ev != "press" && ev != "release") I.throwError("TypeError", "joystick.on: event must be 'press' or 'release'");
  if (!I.isCallable(JS_ARG(1))) I.throwError("TypeError", "joystick.on: handler must be a function");
  I.setOwn(joyHandlers, ev.c_str(), args[1]);
  return Value();
}
JS_NATIVE(joystick_off) {
  (void)self;
  if (argc == 0) joyHandlers->props.clear();
  else I.delProp(Value::obj(joyHandlers), I.toStr(args[0]));
  return Value();
}
JS_NATIVE(joystick_keys) {  // joystick.keys(false): stop the D-pad and buttons sending key presses
  (void)self;
  joyKeys = argc == 0 || I.truthy(args[0]);
  return Value();
}
JS_NATIVE(keyboard_capture) {  // keyboard.capture(true): Escape and Ctrl+C come to this app as keys
  (void)self; (void)I;
  keyCapture = argc == 0 || I.truthy(args[0]);
  return Value();
}

static Value pointObj(Interp& I, int x, int y, int z) {
  Object* o = I.newObject();
  I.setOwn(o, "x", Value::num(x));
  I.setOwn(o, "y", Value::num(y));
  I.setOwn(o, "z", Value::num(z));
  return Value::obj(o);
}

JS_NATIVE(touch_read) {
  (void)self; (void)args; (void)argc;
  int rx, ry, rz, sx, sy;
  if (!touchRaw(rx, ry, rz)) return Value::null();
  mapTouch(rx, ry, sx, sy);
  return pointObj(I, sx, sy, rz);
}
JS_NATIVE(touch_raw) {
  (void)self; (void)args; (void)argc;
  int rx, ry, rz;
  if (!touchRaw(rx, ry, rz)) return Value::null();
  return pointObj(I, rx, ry, rz);
}
// touch.calibrate([a, b, c, d, e, f], rotation): screen x = a*rawX + b*rawY + c, y = d*rawX + e*rawY + f
// at that screen rotation (calibrate.js works these out from touches on targets; other rotations
// follow). touch.calibrate(null) goes back to the board's built-in mapping. touch.calibrate()
// returns { matrix, rotation } or null. The old form touch.calibrate(xmin, xmax, ymin, ymax) sets the
// resistive CYD's raw ranges.
JS_NATIVE(touch_calibrate) {
  (void)self;
  if (argc >= 1 && args[0].isNull()) tcalRot = -1;
  else if (argc >= 1 && args[0].isObj() && args[0].o()->kind == K::Array) {
    auto& m = ((Array*)args[0].o())->items;
    if (m.size() < 6) I.throwError("TypeError", "touch.calibrate: needs 6 numbers");
    for (int i = 0; i < 6; i++) tcal[i] = (float)I.toNum(m[i]);
    tcalRot = argc >= 2 ? IA(1) & 3 : rotation;
  } else if (argc >= 4) { cal[0] = IA(0); cal[1] = IA(1); cal[2] = IA(2); cal[3] = IA(3); }
  if (tcalRot < 0) return Value::null();
  Object* o = I.newObject();
  Array* a = I.newArray();
  for (float v : tcal) a->items.push_back(Value::num(v));
  I.setOwn(o, "matrix", Value::obj(a));
  I.setOwn(o, "rotation", Value::num(tcalRot));
  return Value::obj(o);
}
JS_NATIVE(touch_on) {
  (void)self;
  std::string ev = I.toStr(JS_ARG(0));
  if (ev != "down" && ev != "move" && ev != "up") I.throwError("TypeError", "touch.on: event must be 'down', 'move' or 'up'");
  if (!I.isCallable(JS_ARG(1))) I.throwError("TypeError", "touch.on: handler must be a function");
  I.setOwn(touchHandlers, ev.c_str(), args[1]);
  return Value();
}
JS_NATIVE(touch_off) {
  (void)self;
  if (argc == 0) touchHandlers->props.clear();
  else I.delProp(Value::obj(touchHandlers), I.toStr(args[0]));
  return Value();
}

// ------------------------------------------------------------------ display & sprites
static void spriteFree(void* p) { delete (TFT_eSprite*)p; }

static TFT_eSPI* gfx(Interp& I, Value self) {
  if (self.isObj() && self.o()->finalize == spriteFree) {
    if (!self.o()->ext) I.throwError("Error", "sprite has been freed");
    return (TFT_eSprite*)self.o()->ext;
  }
  return &tft;
}
static TFT_eSprite* spriteOf(Interp& I, Value self) {
  if (!self.isObj() || self.o()->finalize != spriteFree) I.throwError("TypeError", "not a sprite");
  if (!self.o()->ext) I.throwError("Error", "sprite has been freed");
  return (TFT_eSprite*)self.o()->ext;
}
static bool isSprite(Value self) { return self.isObj() && self.o()->finalize == spriteFree && self.o()->ext; }

JS_NATIVE(d_fillScreen) {
  if (isSprite(self)) spriteOf(I, self)->fillSprite(CA(0));
  else gfx(I, self)->fillScreen(CA(0));
  return Value();
}
JS_NATIVE(d_drawPixel) { gfx(I, self)->drawPixel(IA(0), IA(1), CA(2)); return Value(); }
JS_NATIVE(d_readPixel) { return Value::num(gfx(I, self)->readPixel(IA(0), IA(1))); }
JS_NATIVE(d_drawLine) { gfx(I, self)->drawLine(IA(0), IA(1), IA(2), IA(3), CA(4)); return Value(); }
JS_NATIVE(d_drawWideLine) {
  gfx(I, self)->drawWideLine(FA(0), FA(1), FA(2), FA(3), FA(4), CA(5), argc > 6 ? CA(6) : 0x00FFFFFF);
  return Value();
}
JS_NATIVE(d_drawFastHLine) { gfx(I, self)->drawFastHLine(IA(0), IA(1), IA(2), CA(3)); return Value(); }
JS_NATIVE(d_drawFastVLine) { gfx(I, self)->drawFastVLine(IA(0), IA(1), IA(2), CA(3)); return Value(); }
JS_NATIVE(d_drawRect) { gfx(I, self)->drawRect(IA(0), IA(1), IA(2), IA(3), CA(4)); return Value(); }
JS_NATIVE(d_fillRect) { gfx(I, self)->fillRect(IA(0), IA(1), IA(2), IA(3), CA(4)); return Value(); }
JS_NATIVE(d_drawRoundRect) { gfx(I, self)->drawRoundRect(IA(0), IA(1), IA(2), IA(3), IA(4), CA(5)); return Value(); }
JS_NATIVE(d_fillRoundRect) { gfx(I, self)->fillRoundRect(IA(0), IA(1), IA(2), IA(3), IA(4), CA(5)); return Value(); }
JS_NATIVE(d_fillSmoothRoundRect) {
  gfx(I, self)->fillSmoothRoundRect(IA(0), IA(1), IA(2), IA(3), IA(4), CA(5), argc > 6 ? CA(6) : 0x00FFFFFF);
  return Value();
}
JS_NATIVE(d_drawCircle) { gfx(I, self)->drawCircle(IA(0), IA(1), IA(2), CA(3)); return Value(); }
JS_NATIVE(d_fillCircle) { gfx(I, self)->fillCircle(IA(0), IA(1), IA(2), CA(3)); return Value(); }
JS_NATIVE(d_fillSmoothCircle) {
  gfx(I, self)->fillSmoothCircle(IA(0), IA(1), IA(2), CA(3), argc > 4 ? CA(4) : 0x00FFFFFF);
  return Value();
}
JS_NATIVE(d_drawEllipse) { gfx(I, self)->drawEllipse(IA(0), IA(1), IA(2), IA(3), CA(4)); return Value(); }
JS_NATIVE(d_fillEllipse) { gfx(I, self)->fillEllipse(IA(0), IA(1), IA(2), IA(3), CA(4)); return Value(); }
JS_NATIVE(d_drawTriangle) { gfx(I, self)->drawTriangle(IA(0), IA(1), IA(2), IA(3), IA(4), IA(5), CA(6)); return Value(); }
JS_NATIVE(d_fillTriangle) { gfx(I, self)->fillTriangle(IA(0), IA(1), IA(2), IA(3), IA(4), IA(5), CA(6)); return Value(); }
JS_NATIVE(d_drawArc) {
  // drawArc(x, y, r, innerR, startDeg, endDeg, color, [bg]) - angles clockwise from 6 o'clock
  gfx(I, self)->drawArc(IA(0), IA(1), IA(2), IA(3), IA(4), IA(5), CA(6), argc > 7 ? CA(7) : 0x00FFFFFF, true);
  return Value();
}
JS_NATIVE(d_drawString) {
  std::string s = I.toStr(JS_ARG(0));
  TFT_eSPI* g = gfx(I, self);
  int w = argc > 3 && !args[3].isUndef() ? g->drawString(s.c_str(), IA(1), IA(2), IA(3)) : g->drawString(s.c_str(), IA(1), IA(2));
  return Value::num(w);
}
JS_NATIVE(d_print) {
  TFT_eSPI* g = gfx(I, self);
  for (int i = 0; i < argc; i++) {
    if (i) g->print(' ');
    g->print(args[i].isStr() ? args[i].s()->s.c_str() : I.inspect(args[i]).c_str());
  }
  return Value();
}
JS_NATIVE(d_println) { d_print(I, self, args, argc); gfx(I, self)->println(); return Value(); }
JS_NATIVE(d_setTextColor) {
  if (argc > 1) gfx(I, self)->setTextColor(CA(0), CA(1), true);
  else gfx(I, self)->setTextColor(CA(0));
  return Value();
}
JS_NATIVE(d_setTextSize) { gfx(I, self)->setTextSize(constrain(IA(0), 1, 7)); return Value(); }
JS_NATIVE(d_setTextFont) { gfx(I, self)->setTextFont(IA(0)); return Value(); }
JS_NATIVE(d_setTextDatum) { gfx(I, self)->setTextDatum(IA(0)); return Value(); }
JS_NATIVE(d_setTextPadding) { gfx(I, self)->setTextPadding(IA(0)); return Value(); }
JS_NATIVE(d_setTextWrap) { gfx(I, self)->setTextWrap(I.truthy(JS_ARG(0)), I.truthy(JS_ARG(1))); return Value(); }
JS_NATIVE(d_setCursor) { gfx(I, self)->setCursor(IA(0), IA(1)); return Value(); }
JS_NATIVE(d_textWidth) {
  std::string s = I.toStr(JS_ARG(0));
  TFT_eSPI* g = gfx(I, self);
  return Value::num(argc > 1 ? g->textWidth(s.c_str(), IA(1)) : g->textWidth(s.c_str()));
}
JS_NATIVE(d_fontHeight) {
  TFT_eSPI* g = gfx(I, self);
  return Value::num(argc > 0 ? g->fontHeight(IA(0)) : g->fontHeight());
}
JS_NATIVE(d_width) {
  (void)args; (void)argc;
  if (isSprite(self)) return Value::num(spriteOf(I, self)->width());
  return Value::num(tft.width());
}
JS_NATIVE(d_height) {
  (void)args; (void)argc;
  if (isSprite(self)) return Value::num(spriteOf(I, self)->height());
  return Value::num(tft.height());
}
JS_NATIVE(d_color) {
  (void)self;
  return Value::num(tft.color565(clamp255(IA(0)), clamp255(IA(1)), clamp255(IA(2))));
}
JS_NATIVE(lcd_setRotation) {
  (void)self;
  rotation = IA(0) & 3;
  tft.setRotation(rotation);
  return Value();
}
JS_NATIVE(lcd_getRotation) { (void)self; (void)I; (void)args; (void)argc; return Value::num(rotation); }
JS_NATIVE(lcd_invert) { (void)self; tft.invertDisplay(I.truthy(JS_ARG(0))); return Value(); }
JS_NATIVE(g_backlight) {
  (void)self;
  backlightWrite(argc ? IA(0) : 255);
  return Value();
}
// ---- images: lcd.drawImage for .jpg (TJpg_Decoder), .png (PNGdec) and .bmp (24/32-bit, or 16-bit 565)
//   lcd.drawImage(path)                  fit the whole screen (shrink or enlarge), centred
//   lcd.drawImage(path, x, y)            actual size at x, y
//   lcd.drawImage(path, x, y, zoom)      scaled by zoom (0.5 = half size, 2 = double)
//   lcd.drawImage(path, x, y, w, h)      fit inside that box, centred
// Pictures are resampled (nearest pixel) to any size; a JPEG is first decoded at 1/2, 1/4 or 1/8
// size when that's still at least as big as needed, which is much faster.
#if __has_include(<TJpg_Decoder.h>)
#include <TJpg_Decoder.h>
#define HAS_JPEG 1
#endif
#if __has_include(<PNGdec.h>)
#include <PNGdec.h>
#define HAS_PNG 1
#endif
static int imgX, imgY, imgDW, imgDH;  // where the picture goes on screen, and its size there
static double imgF;                   // screen pixels per decoded pixel
static uint16_t* imgBuf;              // output line / block
static bool imgStopped;               // decoding was stopped early (the rest is off the screen)
static int imgFirst(double v) { return (int)ceil(v * imgF - 1e-9); }  // first screen pixel at or after decoded v
static int imgSrc(int d) { return (int)floor((d + 0.5) / imgF); }       // the decoded pixel behind screen pixel d
static std::string imgExt(const std::string& p) {
  size_t d = p.rfind('.');
  std::string e = d == std::string::npos ? "" : p.substr(d + 1);
  for (auto& c : e) c = tolower(c);
  return e;
}
static uint32_t le32(const uint8_t* b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
// The picture's size in pixels, from its header. False if it isn't a picture we read.
static bool imageSize(fs::FS* f, const std::string& rel, const std::string& ext, int& w, int& h) {
  File file = f->open(rel.c_str(), "r");
  if (!file) return false;
  uint8_t b[32];
  bool ok = false;
  if (ext == "png") {  // the IHDR chunk follows the 8-byte signature
    ok = file.read(b, 24) == 24 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G';
    w = b[16] << 24 | b[17] << 16 | b[18] << 8 | b[19];
    h = b[20] << 24 | b[21] << 16 | b[22] << 8 | b[23];
  } else if (ext == "bmp") {
    ok = file.read(b, 30) == 30 && b[0] == 'B' && b[1] == 'M';
    w = (int32_t)le32(b + 18);
    h = abs((int32_t)le32(b + 22));
  }
  file.close();
#ifdef HAS_JPEG
  if (ext == "jpg" || ext == "jpeg") {
    uint16_t jw = 0, jh = 0;
    ok = TJpgDec.getFsJpgSize(&jw, &jh, rel.c_str(), *f) == JDR_OK;
    w = jw, h = jh;
  }
#endif
  return ok && w > 0 && h > 0;
}
#ifdef HAS_JPEG
// A decoded block (bx, by, bw x bh in decoded pixels) becomes the screen pixels that sample it.
static bool jpgBlock(int16_t bx, int16_t by, uint16_t bw, uint16_t bh, uint16_t* bmp) {
  int x0 = imgFirst(bx), x1 = std::min(imgFirst(bx + bw), imgDW), y0 = imgFirst(by), y1 = std::min(imgFirst(by + bh), imgDH);
  if (y0 < imgDH && imgY + y0 >= tft.height()) return !(imgStopped = true);  // below the screen: stop decoding
  if (x1 <= x0 || y1 <= y0) return true;
  if (imgF == 1.0) { tft.pushImage(imgX + bx, imgY + by, bw, bh, bmp); return true; }
  int n = 0;
  for (int dy = y0; dy < y1; dy++) {
    int sy = std::min(std::max(imgSrc(dy) - by, 0), bh - 1);
    for (int dx = x0; dx < x1; dx++) imgBuf[n++] = bmp[sy * bw + std::min(std::max(imgSrc(dx) - bx, 0), bw - 1)];
  }
  tft.pushImage(imgX + x0, imgY + y0, x1 - x0, y1 - y0, imgBuf);
  return true;
}
#endif
// One decoded line (srcY, w pixels) becomes the screen rows that sample it.
static bool imgLine(int srcY, const uint16_t* src, int w) {
  int y0 = imgFirst(srcY), y1 = std::min(imgFirst(srcY + 1), imgDH);
  if (y0 < imgDH && imgY + y0 >= tft.height()) return !(imgStopped = true);
  if (y1 <= y0) return true;
  for (int dx = 0; dx < imgDW; dx++) imgBuf[dx] = src[std::min(imgSrc(dx), w - 1)];
  for (int dy = y0; dy < y1; dy++) tft.pushImage(imgX, imgY + dy, imgDW, 1, imgBuf);
  return true;
}
#ifdef HAS_PNG
static fs::FS* pngFs;
static uint16_t* pngLine;
static void* pngOpen(const char* name, int32_t* size) {
  File* f = new File(pngFs->open(name, "r"));
  if (!*f) { delete f; return nullptr; }
  *size = f->size();
  return f;
}
static void pngClose(void* h) { if (h) { ((File*)h)->close(); delete (File*)h; } }
static int32_t pngRead(PNGFILE* p, uint8_t* buf, int32_t len) { return ((File*)p->fHandle)->read(buf, len); }
static int32_t pngSeek(PNGFILE* p, int32_t pos) { return ((File*)p->fHandle)->seek(pos) ? pos : -1; }
static PNG* pngDec;
static int pngDraw(PNGDRAW* d) {
  pngDec->getLineAsRGB565(d, pngLine, PNG_RGB565_LITTLE_ENDIAN, 0x000000);
  return imgLine(d->y, pngLine, d->iWidth) ? 1 : 0;
}
#endif
static std::string bmpDraw(fs::FS* f, const std::string& rel, int w, int h) {
  File file = f->open(rel.c_str(), "r");
  uint8_t hd[54];
  if (file.read(hd, 54) != 54) return "not a BMP picture";
  uint32_t data = le32(hd + 10);
  int bpp = hd[28] | hd[29] << 8, comp = le32(hd + 30);
  bool topDown = (int32_t)le32(hd + 22) < 0;
  if (!(bpp == 24 || bpp == 32 || (bpp == 16 && comp == 3)) || (comp != 0 && comp != 3)) return "BMP format not supported (use 16, 24 or 32 bits, uncompressed)";
  size_t stride = ((size_t)w * bpp / 8 + 3) & ~(size_t)3;
  uint8_t* row = (uint8_t*)malloc(stride);
  uint16_t* line = (uint16_t*)malloc(w * 2);
  if (!row || !line) { free(row); free(line); return "not enough memory"; }
  int last = -1;
  for (int dy = 0; dy < imgDH && imgY + dy < tft.height(); dy++) {  // each screen row reads its source row
    int sy = std::min(imgSrc(dy), h - 1);
    if (sy != last) {
      last = sy;
      if (!file.seek(data + (size_t)(topDown ? sy : h - 1 - sy) * stride) || file.read(row, stride) != (int)stride) break;
      for (int i = 0; i < w; i++) {
        const uint8_t* px = row + (size_t)i * (bpp / 8);
        line[i] = bpp == 16 ? (uint16_t)(px[0] | px[1] << 8) : (uint16_t)((px[2] & 0xF8) << 8 | (px[1] & 0xFC) << 3 | px[0] >> 3);
      }
    }
    for (int dx = 0; dx < imgDW; dx++) imgBuf[dx] = line[std::min(imgSrc(dx), w - 1)];
    tft.pushImage(imgX, imgY + dy, imgDW, 1, imgBuf);
  }
  free(row);
  free(line);
  return "";
}
JS_NATIVE(lcd_imageSize) {  // lcd.imageSize(path) -> { width, height } or null
  (void)self;
  std::string path = I.toStr(JS_ARG(0)), rel;
  fs::FS* f = pickFs(path, rel);
  int w, h;
  if (!f || !imageSize(f, rel, imgExt(path), w, h)) return Value::null();
  Object* o = I.newObject();
  I.setOwn(o, "width", Value::num(w));
  I.setOwn(o, "height", Value::num(h));
  return Value::obj(o);
}
JS_NATIVE(lcd_drawImage) {  // -> { width, height, zoom, x, y, w, h } (w, h: the size on screen)
  (void)self;
  std::string path = I.toStr(JS_ARG(0)), rel, ext = imgExt(path);
  fs::FS* f = pickFs(path, rel);
  int w, h;
  if (!f || !f->exists(rel.c_str())) I.throwError("Error", "drawImage: no such file " + path);
  if (!imageSize(f, rel, ext, w, h)) I.throwError("Error", "drawImage: not a JPEG, PNG or BMP picture: " + path);
  bool jpg = ext == "jpg" || ext == "jpeg";
  double zoom;
  int bx = 0, by = 0, bw = tft.width(), bh = tft.height();
  if (argc >= 5) bx = (int)I.toNum(args[1]), by = (int)I.toNum(args[2]), bw = (int)I.toNum(args[3]), bh = (int)I.toNum(args[4]);
  if (argc == 3 || argc == 4) {
    zoom = argc == 4 ? I.toNum(args[3]) : 1.0;
    if (!(zoom > 0.001 && zoom < 64)) I.throwError("RangeError", "drawImage: zoom must be between 0.001 and 64");
    imgX = (int)I.toNum(args[1]), imgY = (int)I.toNum(args[2]);
    imgDW = std::max(1, (int)(w * zoom)), imgDH = std::max(1, (int)(h * zoom));
  } else {  // fit the box, keeping the shape
    zoom = std::min((double)bw / w, (double)bh / h);
    imgDW = std::max(1, (int)(w * zoom + 0.5)), imgDH = std::max(1, (int)(h * zoom + 0.5));
    imgX = bx + (bw - imgDW) / 2, imgY = by + (bh - imgDH) / 2;
  }
  int jscale = 1;  // a JPEG decodes at 1/2, 1/4 or 1/8 while that's still big enough
  if (jpg) while (jscale < 8 && w / (jscale * 2) >= imgDW && h / (jscale * 2) >= imgDH) jscale *= 2;
  imgF = (double)imgDW / (jpg ? (w / jscale) : w);
  if (jpg && imgDW == w / jscale) imgF = 1.0;
  // the output buffer: a screen line, or a resampled JPEG block (16 x 16 decoded pixels at most)
  int blk = (int)ceil(16 * imgF) + 2;
  size_t need = std::max((size_t)imgDW + 1, jpg ? (size_t)blk * blk : 0);
  imgBuf = (uint16_t*)malloc(need * 2);
  if (!imgBuf) I.throwError("Error", "drawImage: not enough memory");
  bool swap = tft.getSwapBytes();
  tft.setSwapBytes(true);
  std::string err;
  imgStopped = false;
  if (jpg) {
#ifdef HAS_JPEG
    TJpgDec.setJpgScale(jscale);
    TJpgDec.setCallback(jpgBlock);
    if (TJpgDec.drawFsJpg(0, 0, rel.c_str(), *f) != JDR_OK && !imgStopped) err = "couldn't decode the JPEG (progressive JPEGs aren't supported)";
#else
    err = "JPEG support isn't built in";
#endif
  } else if (ext == "png") {
#ifdef HAS_PNG
    void* mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) mem = malloc(sizeof(PNG));
    pngLine = (uint16_t*)malloc((w + 1) * 2);
    if (!mem || !pngLine) err = "not enough memory for this PNG";
    else {
      pngDec = new (mem) PNG();
      pngFs = f;
      if (pngDec->open(rel.c_str(), pngOpen, pngClose, pngRead, pngSeek, pngDraw) != PNG_SUCCESS) err = "couldn't open the PNG";
      else {
        if (pngDec->decode(nullptr, 0) != PNG_SUCCESS && !imgStopped) err = "couldn't decode the PNG";
        pngDec->close();
      }
      pngDec->~PNG();
    }
    free(mem);
    free(pngLine);
    pngLine = nullptr;
#else
    err = "PNG support isn't built in";
#endif
  } else err = bmpDraw(f, rel, w, h);
  tft.setSwapBytes(swap);
  free(imgBuf);
  imgBuf = nullptr;
  if (!err.empty()) I.throwError("Error", "drawImage: " + err);
  Object* o = I.newObject();
  I.setOwn(o, "width", Value::num(w));
  I.setOwn(o, "height", Value::num(h));
  I.setOwn(o, "zoom", Value::num((double)imgDW / w));
  I.setOwn(o, "x", Value::num(imgX));
  I.setOwn(o, "y", Value::num(imgY));
  I.setOwn(o, "w", Value::num(imgDW));
  I.setOwn(o, "h", Value::num(imgDH));
  return Value::obj(o);
}
JS_NATIVE(lcd_createSprite) {
  (void)self;
  int w = IA(0), h = IA(1), depth = argc > 2 ? IA(2) : 16;
  if (w <= 0 || h <= 0 || w > 320 || h > 320) I.throwError("RangeError", "createSprite: bad size");
  if (depth != 1 && depth != 4 && depth != 8 && depth != 16) I.throwError("RangeError", "createSprite: depth must be 1, 4, 8 or 16");
  TFT_eSprite* sp = new TFT_eSprite(&tft);
  sp->setColorDepth(depth);
  if (!sp->createSprite(w, h)) {
    I.collect();  // unreachable sprites may be holding the memory
    sp->createSprite(w, h);
  }
  if (!sp->created()) {
    delete sp;
    I.throwError("Error", "createSprite: not enough memory (" + std::to_string(w * h * depth / 8) + " bytes, largest free block " +
                              std::to_string(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)) + ")");
  }
  Object* o = I.newObject(spriteProto);
  o->ext = sp;
  o->finalize = spriteFree;
  I.allocBytes += (size_t)w * h * depth / 8;
  return Value::obj(o);
}
JS_NATIVE(sprite_push) {
  TFT_eSprite* sp = spriteOf(I, self);
#ifdef BOARD_SCREEN_HEADER  // the screen is itself a sprite: blit into it
  if (argc > 2) sp->pushToSprite(&tft, IA(0), IA(1), (uint16_t)CA(2));
  else sp->pushToSprite(&tft, IA(0), IA(1));
#else
  if (argc > 2) sp->pushSprite(IA(0), IA(1), (uint16_t)CA(2));
  else sp->pushSprite(IA(0), IA(1));
#endif
  return Value();
}
JS_NATIVE(sprite_free) {
  (void)args; (void)argc;
  if (isSprite(self)) {
    delete (TFT_eSprite*)self.o()->ext;
    self.o()->ext = nullptr;
  }
  (void)I;
  return Value();
}

// ------------------------------------------------------------------ LED, speaker, sensors, GPIO
JS_NATIVE(g_led) {
  (void)self;
  ledWrite(IA(0), IA(1), IA(2));
  return Value();
}
JS_NATIVE(g_tone) {
  (void)self;
  int f = IA(0), ms = argc > 1 ? IA(1) : 0;
  if (f <= 0) { toneWrite(0); toneEnd = 0; return Value(); }
  toneWrite(f);
  toneEnd = ms > 0 ? (millis() + ms) | 1 : 0;
  return Value();
}
JS_NATIVE(g_noTone) { (void)I; (void)self; (void)args; (void)argc; toneWrite(0); toneEnd = 0; return Value(); }
JS_NATIVE(g_light) {
  (void)I; (void)self; (void)args; (void)argc;
#if BOARD_LIGHT_SENSOR >= 0
  return Value::num(analogRead(BOARD_LIGHT_SENSOR));
#else
  return Value::num(-1);  // this board has no light sensor
#endif
}
JS_NATIVE(g_pinMode) { (void)self; pinMode(IA(0), IA(1)); return Value(); }
JS_NATIVE(g_digitalWrite) { (void)self; digitalWrite(IA(0), I.truthy(JS_ARG(1)) ? HIGH : LOW); return Value(); }
JS_NATIVE(g_digitalRead) { (void)self; return Value::num(digitalRead(IA(0))); }
JS_NATIVE(g_analogRead) { (void)self; return Value::num(analogRead(IA(0))); }
JS_NATIVE(g_micros) { (void)I; (void)self; (void)args; (void)argc; return Value::num((double)micros()); }
JS_NATIVE(g_delay) {
  (void)self;
  uint32_t end = millis() + (uint32_t)std::max(0, IA(0));
  while ((int32_t)(end - millis()) > 0) {
    if (I.poll) I.poll();  // lets Ctrl-C interrupt
    hwService();
    vTaskDelay(1);
  }
  return Value();
}

// ------------------------------------------------------------------ system
JS_NATIVE(sys_heap) {
  (void)self; (void)args; (void)argc;
  Object* o = I.newObject();
  I.setOwn(o, "free", Value::num(ESP.getFreeHeap()));
  I.setOwn(o, "minFree", Value::num(ESP.getMinFreeHeap()));
  I.setOwn(o, "largest", Value::num(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  I.setOwn(o, "js", Value::num(I.heapBytes()));
  I.setOwn(o, "pool", Value::num(js::poolRegionLeft()));
  return Value::obj(o);
}
JS_NATIVE(sys_i2cScan) {  // sys.i2cScan() -> addresses answering on the board's I2C bus (touch / keyboard)
  (void)self; (void)args; (void)argc;
  Array* a = I.newArray();
  for (int addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (!Wire.endTransmission()) a->items.push_back(Value::num(addr));
  }
  return Value::obj(a);
}
JS_NATIVE(sys_i2cWrite) {  // sys.i2cWrite(addr, reg, value) -> true if the device acknowledged
  (void)self;
  if (argc < 3) return Value::boolean(false);
  Wire.beginTransmission((int)I.toNum(args[0]));
  Wire.write((int)I.toNum(args[1]));
  Wire.write((int)I.toNum(args[2]));
  return Value::boolean(Wire.endTransmission() == 0);
}
JS_NATIVE(sys_i2cRead) {  // sys.i2cRead(addr, reg) -> the register's value, or -1
  (void)self;
  if (argc < 2) return Value::num(-1);
  int addr = (int)I.toNum(args[0]);
  Wire.beginTransmission(addr);
  Wire.write((int)I.toNum(args[1]));
  if (Wire.endTransmission(false) || Wire.requestFrom(addr, 1) != 1) return Value::num(-1);
  return Value::num(Wire.read());
}
JS_NATIVE(audio_volume) {  // audio.volume(0..100) sets, audio.volume() reads: tones and music
  (void)self;
  if (argc) {
    spkVolume = std::max(0, std::min(100, (int)I.toNum(args[0])));
#if HAS_MUSIC
    if (mp3Active) mp3ApplyVolume();
#endif
  }
  return Value::num(spkVolume);
}
JS_NATIVE(audio_mute) {  // audio.mute(true | false) sets, audio.mute() reads
  (void)self;
  if (argc) {
    spkMuted = I.truthy(args[0]);
    if (spkMuted) toneWrite(0);
#if HAS_MUSIC
    if (mp3Active) mp3ApplyVolume();
#endif
  }
  return Value::boolean(spkMuted);
}

// ---- video: video.play(path) plays an AVI of Motion-JPEG video, with MP3 sound on boards with an
// I2S speaker. Frames are decoded by JPEGDEC (SIMD on the S3) in a task on the app's core and sent
// to the screen by DMA; the sound plays on the other core and is the clock: frames that would be
// late are skipped. Make one with ffmpeg (320 wide, 20 fps):
//   ffmpeg -i in.mp4 -vf "scale=320:-2,fps=20" -c:v mjpeg -q:v 6 -c:a libmp3lame -b:a 96k -ar 44100 out.avi
// While a video plays the app mustn't draw on the screen.
#if __has_include(<JPEGDEC.h>)
#include <JPEGDEC.h>
#define HAS_VIDEO 1
struct AviInfo { uint32_t usPerFrame = 0, frames = 0, w = 0, h = 0, moviStart = 0, moviEnd = 0; bool mjpeg = false, mp3 = false; };
static std::string aviParse(File& f, AviInfo& v) {
  uint8_t b[64];
  if (f.read(b, 12) != 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "AVI ", 4)) return "not an AVI file";
  uint32_t size = f.size();
  std::function<void(uint32_t, uint32_t)> walk = [&](uint32_t pos, uint32_t end) {
    char stream = 0;  // the type of the last stream header: 'v' or 'a'
    while (pos + 8 <= end && !v.moviStart) {
      f.seek(pos);
      if (f.read(b, 12) < 8) return;
      uint32_t sz = rd32(b + 4), data = pos + 8;
      if (!memcmp(b, "LIST", 4)) {
        if (!memcmp(b + 8, "movi", 4)) { v.moviStart = pos + 12; v.moviEnd = std::min(data + sz, size); return; }
        if (!memcmp(b + 8, "hdrl", 4) || !memcmp(b + 8, "strl", 4)) walk(pos + 12, std::min(data + sz, end));
      } else if (!memcmp(b, "avih", 4) && sz >= 40) {
        f.seek(data);
        f.read(b, 40);
        v.usPerFrame = rd32(b), v.frames = rd32(b + 16), v.w = rd32(b + 32), v.h = rd32(b + 36);
      } else if (!memcmp(b, "strh", 4)) {
        f.seek(data);
        f.read(b, 4);
        stream = !memcmp(b, "vids", 4) ? 'v' : !memcmp(b, "auds", 4) ? 'a' : 0;
      } else if (!memcmp(b, "strf", 4)) {
        f.seek(data);
        f.read(b, 20);
        if (stream == 'v') v.mjpeg = !memcmp(b + 16, "MJPG", 4) || !memcmp(b + 16, "mjpg", 4);
        if (stream == 'a') v.mp3 = (b[0] | b[1] << 8) == 0x55;
      }
      pos = data + sz + (sz & 1);
    }
  };
  walk(12, size);
  if (!v.moviStart) return "no video data in this AVI";
  if (!v.mjpeg) return "the video isn't Motion-JPEG (convert it with ffmpeg -c:v mjpeg)";
  if (!v.usPerFrame) v.usPerFrame = 50000;
  return "";
}
static volatile bool vidStop = false, vidPaused = false, vidSound = false;
static volatile bool vidHolding = false;  // paused and done drawing: the app may use the screen
static volatile uint32_t vidFrame = 0, vidShown = 0, vidDropped = 0;
static TaskHandle_t vidTask = nullptr;
static std::string vidPath, vidError;
static const char* vidEnded = "";  // why playback stopped: "end", "stopped", or what went wrong
static AviInfo vidInfo;
// DMA is off by default: on the CYD-S3 (TFT_eSPI 2.5.43, HSPI) DMA frames only reached part of the
// screen. video.dma(true) turns it back on to try on other boards.
static bool vidDma = false, vidDmaTried = false, vidDmaAllowed = false;
// DMA reads from two buffers of its own (internal, DMA-capable RAM): each strip of pixels is copied
// into one and sent while the decoder works on the next strip into the other.
static const int VID_DMA_PIXELS = 320 * 16;
static uint16_t* vidDmaBuf[2];
static int vidDmaNext = 0;
static int vidDraw(JPEGDRAW* d) {
#ifndef BOARD_SCREEN_HEADER
  if (vidDma && d->iWidth * d->iHeight <= VID_DMA_PIXELS) {
    tft.pushImageDMA(d->x, d->y, d->iWidth, d->iHeight, d->pPixels, vidDmaBuf[vidDmaNext]);
    vidDmaNext ^= 1;
    return 1;
  }
  if (vidDma) tft.dmaWait();
#endif
  tft.pushImage(d->x, d->y, d->iWidth, d->iHeight, d->pPixels);
  return 1;
}
static void vidRun(void*) {
  std::string rel;
  fs::FS* fsys = pickFs(vidPath, rel);
  File f = fsys ? fsys->open(rel.c_str(), "r") : File();
  AviStream vs{f, vidInfo.moviStart, vidInfo.moviEnd, 0, 'd', 'c'};
  void* jmem = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!jmem) jmem = malloc(sizeof(JPEGDEC));
  uint8_t* buf = nullptr;
  uint32_t bufSize = 0;
  if (!f || !jmem) vidError = !f ? "can't open the file" : "not enough memory";
  else {
    JPEGDEC* jpeg = new (jmem) JPEGDEC();
    int sw = tft.width(), sh = tft.height(), sc = 1;  // shrink by 2, 4 or 8 to fit the screen
    while (sc < 8 && ((int)vidInfo.w / sc > sw || (int)vidInfo.h / sc > sh)) sc *= 2;
    int opt = sc == 2 ? JPEG_SCALE_HALF : sc == 4 ? JPEG_SCALE_QUARTER : sc == 8 ? JPEG_SCALE_EIGHTH : 0;
    int x0 = (sw - (int)vidInfo.w / sc) / 2, y0 = (sh - (int)vidInfo.h / sc) / 2;
#ifndef BOARD_SCREEN_HEADER
    if (!vidDmaTried && vidDmaAllowed) {
      vidDmaTried = true;
      for (auto& b : vidDmaBuf) b = (uint16_t*)heap_caps_malloc(VID_DMA_PIXELS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
      vidDma = vidDmaBuf[0] && vidDmaBuf[1] && tft.initDMA();
    }
    if (!vidDmaAllowed) vidDma = false;
#endif
    // On a real panel the decoder writes pixels in the panel's byte order, so nothing gets swapped on
    // the way out; a screen that is a sprite (VGA) takes them the usual way.
#ifdef BOARD_SCREEN_HEADER
    const bool bigEndian = false;
#else
    const bool bigEndian = true;
#endif
    bool swap = tft.getSwapBytes();
    tft.setSwapBytes(!bigEndian);
    tft.fillScreen(TFT_BLACK);
    bool sound = false;
#if HAS_MUSIC
    sound = vidInfo.mp3 && mp3Start(vidPath, vidInfo.moviStart, vidInfo.moviEnd).empty();
#endif
    vidSound = sound;
    uint32_t t0 = millis(), pauseAt = 0, lastMs = 0;
    auto now = [&]() -> uint32_t {  // the play position in ms: the sound's while it plays, else the clock
#if HAS_MUSIC
      if (sound && mp3Task) {
        // the sound's position moves in ~26 ms steps (one MP3 frame); count the time since the last
        // step so frames come out evenly, and never step backwards
        uint32_t ms = mp3Rate ? (uint32_t)((uint64_t)mp3Played * 1000 / mp3Rate) : 0;
        if (ms) ms += std::min<uint32_t>(millis() - mp3PlayedAt, 40);
        return lastMs = std::max(lastMs, ms);
      }
      if (sound) { sound = false; t0 = millis() - lastMs; }  // the sound ended or failed: carry on by the clock
#endif
      return millis() - t0;
    };
    for (uint32_t n = 0; !vidStop; n++) {
      if (vidPaused) {  // hold the picture; the clock doesn't count paused time
        pauseAt = millis();
#if HAS_MUSIC
        mp3Paused = true;
#endif
        vidHolding = true;
        while (vidPaused && !vidStop) vTaskDelay(20);
        vidHolding = false;
#if HAS_MUSIC
        mp3Paused = false;
#endif
        t0 += millis() - pauseAt;
      }
      uint32_t size;
      if (!vs.next(size)) { vidEnded = vs.pos + 8 > vs.end ? "end" : "couldn't read the next frame"; break; }
      uint32_t due = (uint64_t)n * vidInfo.usPerFrame / 1000, frameMs = vidInfo.usPerFrame / 1000;
      if (n && now() > due + 2 * frameMs) { vidDropped++; continue; }  // running late: skip this one
      if (size > bufSize) {
        free(buf);
        bufSize = size + 4096;
        buf = (uint8_t*)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = (uint8_t*)malloc(bufSize);
        if (!buf) { vidError = "not enough memory for a frame"; break; }
      }
      if (vs.f.read(buf, size) != (int)size) { vidEnded = "a frame was cut short"; break; }
      while (!vidStop && !vidPaused && now() + 2 < due) vTaskDelay(1);  // early: wait for its time
      if (jpeg->openRAM(buf, size, vidDraw)) {
        jpeg->setPixelType(bigEndian ? RGB565_BIG_ENDIAN : RGB565_LITTLE_ENDIAN);
        if (vidDma) tft.startWrite();
        jpeg->decode(x0, y0, opt);
#ifndef BOARD_SCREEN_HEADER
        if (vidDma) { tft.dmaWait(); tft.endWrite(); }
#endif
        jpeg->close();
        vidShown++;
      }
      vidFrame = n;
      vTaskDelay(1);  // let the app (on this core) see touches and keys
    }
    if (vidStop) vidEnded = "stopped";
    tft.setSwapBytes(swap);
    jpeg->~JPEGDEC();
  }
#if HAS_MUSIC
  if (vidSound) { mp3Stop = true; while (mp3Task) vTaskDelay(5); }
#endif
  free(buf);
  free(jmem);
  if (f) f.close();
  vidTask = nullptr;
  vTaskDelete(nullptr);
}
static void videoStopWait() {
  if (!vidTask) return;
  vidStop = true;
  vidPaused = false;
  for (int i = 0; i < 400 && vidTask; i++) delay(5);
}
JS_NATIVE(video_play) {  // video.play(path) -> "" when it starts, else why not
  (void)self;
  videoStopWait();
  if (vidTask) return I.str("the previous video didn't stop");
  vidPath = argc ? I.toStr(args[0]) : "";
  std::string rel;
  fs::FS* fsys = pickFs(vidPath, rel);
  File f = fsys ? fsys->open(rel.c_str(), "r") : File();
  if (!f) return I.str("can't open " + vidPath);
  vidInfo = AviInfo();
  std::string err = aviParse(f, vidInfo);
  f.close();
  if (!err.empty()) return I.str(err);
  vidError.clear();
  vidStop = vidPaused = vidSound = vidHolding = false;
  vidFrame = vidShown = vidDropped = 0;
  vidEnded = "";
  if (xTaskCreatePinnedToCore(vidRun, "video", 10 * 1024, nullptr, 1, &vidTask, 1) != pdPASS) {
    vidTask = nullptr;
    return I.str("not enough memory to play");
  }
  return I.str("");
}
JS_NATIVE(video_stop) { (void)I; (void)self; (void)args; (void)argc; videoStopWait(); return Value(); }
JS_NATIVE(video_dma) {  // video.dma(false): send frames without DMA (slower; for screens it upsets)
  (void)self;
  if (argc) vidDmaAllowed = I.truthy(args[0]);
  return Value::boolean(vidDmaAllowed);
}
// video.pause(true | false), or toggle. Pausing returns once the frame being drawn is finished, so
// the app can then draw on the screen (a "paused" banner, say); clear that before resuming.
JS_NATIVE(video_pause) {
  (void)self;
  vidPaused = argc ? I.truthy(args[0]) : !vidPaused;
  if (vidPaused)
    for (int i = 0; i < 300 && vidTask && !vidHolding; i++) delay(5);  // up to 1.5 s for a slow frame
  return Value::boolean(vidPaused);
}
JS_NATIVE(video_status) {  // { playing, paused, frame, frames, shown, dropped, fps, width, height, seconds, duration, sound, error }
  (void)self; (void)args; (void)argc;
  Object* o = I.newObject();
  double fps = vidInfo.usPerFrame ? 1e6 / vidInfo.usPerFrame : 0;
  I.setOwn(o, "playing", Value::boolean(vidTask != nullptr));
  I.setOwn(o, "paused", Value::boolean(vidPaused));
  I.setOwn(o, "frame", Value::num(vidFrame));
  I.setOwn(o, "frames", Value::num(vidInfo.frames));
  I.setOwn(o, "shown", Value::num(vidShown));
  I.setOwn(o, "dropped", Value::num(vidDropped));
  I.setOwn(o, "fps", Value::num(fps));
  I.setOwn(o, "width", Value::num(vidInfo.w));
  I.setOwn(o, "height", Value::num(vidInfo.h));
  I.setOwn(o, "seconds", Value::num(fps ? vidFrame / fps : 0));
  I.setOwn(o, "duration", Value::num(fps ? vidInfo.frames / fps : 0));
  I.setOwn(o, "sound", Value::boolean(vidSound));
  I.setOwn(o, "error", I.str(vidError));
  I.setOwn(o, "ended", I.str(vidEnded));
  return Value::obj(o);
}
#else
JS_NATIVE(video_play) { (void)self; (void)args; (void)argc; return I.str("video isn't built in"); }
JS_NATIVE(video_stop) { (void)I; (void)self; (void)args; (void)argc; return Value(); }
JS_NATIVE(video_pause) { (void)I; (void)self; (void)args; (void)argc; return Value::boolean(false); }
JS_NATIVE(video_status) { (void)self; (void)args; (void)argc; Object* o = I.newObject(); I.setOwn(o, "playing", Value::boolean(false)); return Value::obj(o); }
JS_NATIVE(video_dma) { (void)I; (void)self; (void)args; (void)argc; return Value::boolean(false); }
static void videoStopWait() {}
#endif

// ---- audio: MP3 files from the SD card or flash
#if HAS_MUSIC
JS_NATIVE(audio_play) {  // audio.play(path) -> "" when it starts, else why not
  (void)self;
  return I.str(mp3Start(argc ? I.toStr(args[0]) : ""));
}
JS_NATIVE(audio_stop) { (void)I; (void)self; (void)args; (void)argc; mp3StopWait(); return Value(); }
JS_NATIVE(audio_pause) {  // audio.pause(true | false), or toggle
  (void)self;
  mp3Paused = argc ? I.truthy(args[0]) : !mp3Paused;
  return Value::boolean(mp3Paused);
}

JS_NATIVE(audio_status) {  // { playing, paused, path, position, size, seconds, duration, rate, kbps, channels, error }
  (void)self; (void)args; (void)argc;
  Object* o = I.newObject();
  I.setOwn(o, "playing", Value::boolean(mp3Task != nullptr));
  I.setOwn(o, "paused", Value::boolean(mp3Paused));
  I.setOwn(o, "path", I.str(mp3Path));
  I.setOwn(o, "position", Value::num(mp3Pos));
  I.setOwn(o, "size", Value::num(mp3Size));
  I.setOwn(o, "seconds", Value::num(mp3Rate ? (double)mp3Played / mp3Rate : 0));
  I.setOwn(o, "duration", Value::num(mp3Kbps ? (double)mp3Size * 8 / (mp3Kbps * 1000.0) : 0));
  I.setOwn(o, "rate", Value::num(mp3Rate));
  I.setOwn(o, "kbps", Value::num(mp3Kbps));
  I.setOwn(o, "channels", Value::num(mp3Channels));
  I.setOwn(o, "error", I.str(mp3Error));
  return Value::obj(o);
}
#else
JS_NATIVE(audio_play) { (void)self; (void)args; (void)argc; return I.str("this board has no I2S speaker for music"); }
JS_NATIVE(audio_stop) { (void)I; (void)self; (void)args; (void)argc; return Value(); }
JS_NATIVE(audio_pause) { (void)I; (void)self; (void)args; (void)argc; return Value::boolean(false); }

JS_NATIVE(audio_status) {
  (void)self; (void)args; (void)argc;
  Object* o = I.newObject();
  I.setOwn(o, "playing", Value::boolean(false));
  I.setOwn(o, "error", I.str("this board has no I2S speaker for music"));
  return Value::obj(o);
}
#endif
// sys.battery() -> { volts, percent, usb } (percent from a typical Li-Po discharge curve; above about
// 4.25 V a charger or USB is feeding the sense line rather than a battery), or null without one.
// usb: a computer is on the S3's USB port, so the battery is charging (a plain USB charger, which
// sends no data, can't be told apart from no cable).
JS_NATIVE(sys_battery) {
  (void)self; (void)args; (void)argc;
#if BOARD_BATTERY_ADC >= 0
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(BOARD_BATTERY_ADC);
  double v = mv / 16.0 / 1000.0 * BOARD_BATTERY_DIVIDER;
  static const float curve[][2] = {{3.30f, 0}, {3.50f, 5}, {3.60f, 12}, {3.65f, 20}, {3.70f, 30}, {3.75f, 40},
                                   {3.80f, 50}, {3.90f, 65}, {4.00f, 80}, {4.10f, 90}, {4.20f, 100}};
  double pct = v <= curve[0][0] ? 0 : 100;
  for (int i = 1; i < 11; i++)
    if (v < curve[i][0]) {
      pct = curve[i - 1][1] + (v - curve[i - 1][0]) / (curve[i][0] - curve[i - 1][0]) * (curve[i][1] - curve[i - 1][1]);
      break;
    }
  Object* o = I.newObject();
  I.setOwn(o, "volts", Value::num(round(v * 100) / 100));
  I.setOwn(o, "percent", Value::num(round(pct)));
  bool usb = false;
#if ARDUINO_USB_MODE == 1 && ARDUINO_USB_CDC_ON_BOOT
  usb = HWCDC::isPlugged();
#endif
  I.setOwn(o, "charging", Value::boolean(v > 4.25 || usb));
  I.setOwn(o, "usb", Value::boolean(usb));
  return Value::obj(o);
#else
  (void)I;
  return Value::null();
#endif
}
JS_NATIVE(sys_restart) { (void)I; (void)self; (void)args; (void)argc; ESP.restart(); return Value(); }
JS_NATIVE(sys_cpuMhz) { (void)I; (void)self; (void)args; (void)argc; return Value::num(getCpuFrequencyMhz()); }

static bool clockValid() { return time(nullptr) > 1600000000; }

JS_NATIVE(g_dateNow) {
  (void)self; (void)args; (void)argc; (void)I;
  if (clockValid()) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return Value::num((double)tv.tv_sec * 1000.0 + tv.tv_usec / 1000);
  }
  return Value::num(millis());
}
JS_NATIVE(g_localTime) {
  (void)self; (void)args; (void)argc;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  Object* o = I.newObject();
  I.setOwn(o, "year", Value::num(t.tm_year + 1900));
  I.setOwn(o, "month", Value::num(t.tm_mon + 1));
  I.setOwn(o, "day", Value::num(t.tm_mday));
  I.setOwn(o, "hour", Value::num(t.tm_hour));
  I.setOwn(o, "minute", Value::num(t.tm_min));
  I.setOwn(o, "second", Value::num(t.tm_sec));
  I.setOwn(o, "weekday", Value::num(t.tm_wday));
  I.setOwn(o, "valid", Value::boolean(clockValid()));
  return Value::obj(o);
}

// ------------------------------------------------------------------ files
static fs::FS* pickFs(const std::string& path, std::string& rel) {
  if (path == "/sd" || path.rfind("/sd/", 0) == 0) {
    rel = path.size() > 3 ? path.substr(3) : "/";
    return sdOk ? (fs::FS*)&SDCARD : nullptr;
  }
  rel = (!path.empty() && path[0] == '/') ? path : "/" + path;
  return fsOk ? (fs::FS*)&LittleFS : nullptr;
}

// ---- firmware files, installed into the spare OTA slot by flash.js. Three kinds:
//   .FLH  the ESP32-CoCo emulator's format, so the two can install each other:
//         "<checksum>-COCO ESP32 CUSTOM FIRMWARE/<release notes>~<app image>", checksum = the sum
//         of every byte from the '-' to the end of the file, in decimal
//   .bin  an app image (firmware.bin), or a merged full image (bootloader at 0, partition table
//         at 0x8000): then the app at 0x10000 is installed and the rest is left alone
struct FlhInfo { bool ok = false; std::string error, notes; size_t imageAt = 0, imageSize = 0; };
static bool fileRead(File& f, size_t at, void* buf, size_t n) { return f.seek(at) && f.read((uint8_t*)buf, n) == (int)n; }
// Check the app image at `at`: right chip, and its real length (segments + checksum + hash).
static bool appImage(File& f, size_t at, FlhInfo& r) {
  uint8_t h[24];
  if (!fileRead(f, at, h, sizeof(h)) || h[0] != 0xE9) { r.error = "no firmware image inside"; return false; }
  if ((h[12] | h[13] << 8) != CONFIG_IDF_FIRMWARE_CHIP_ID) { r.error = "firmware is for a different ESP32 chip"; return false; }
  size_t pos = at + 24;
  for (int i = 0; i < h[1]; i++) {
    uint32_t seg[2];
    if (!fileRead(f, pos, seg, 8) || seg[1] > 16 * 1024 * 1024) { r.error = "firmware image is cut short"; return false; }
    pos += 8 + seg[1];
  }
  size_t len = ((pos - at) + 16) & ~(size_t)15;  // padding and the checksum byte
  if (h[23]) len += 32;                          // SHA-256 appended
  if (at + len > f.size()) { r.error = "firmware image is cut short"; return false; }
  r.imageAt = at;
  r.imageSize = len;
  uint32_t desc[64];  // esp_app_desc_t: the project name and version, if the build has one
  if (fileRead(f, at + 32, desc, sizeof(desc)) && desc[0] == 0xABCD5432) {
    const char* d = (const char*)desc;
    char line[120];
    snprintf(line, sizeof(line), "%.32s %.32s\n(built %.16s, IDF %.32s)", d + 48, d + 16, d + 96, d + 112);
    if (r.notes.empty()) r.notes = line;
    else r.notes = std::string(line) + "\n\n" + r.notes;
  }
  return true;
}
static FlhInfo flhCheck(const std::string& path) {
  FlhInfo r;
  std::string rel;
  fs::FS* f = pickFs(path, rel);
  File file = f ? f->open(rel.c_str(), "r") : File();
  if (!file) { r.error = "can't open the file"; return r; }
  uint8_t first = 0, boot = 0;
  fileRead(file, 0, &first, 1);
  if (first == 0xFF) fileRead(file, 0x1000, &boot, 1);  // classic ESP32 full images: bootloader at 0x1000
  if (first == 0xE9 || boot == 0xE9) {  // a .bin: an app image, or a merged image with one at 0x10000
    uint8_t pt[2] = {0};
    bool merged = fileRead(file, 0x8000, pt, 2) && pt[0] == 0xAA && pt[1] == 0x50;
    if (merged) r.notes = "Full flash image: installing just its firmware (the part at 0x10000).";
    r.ok = appImage(file, merged ? 0x10000 : 0, r);
    file.close();
    return r;
  }
  size_t size = file.size(), pos = 0, dash = 0, tilde = 0;
  uint32_t want = 0, sum = 0;
  int state = 0;  // 0 checksum digits, 1 notes, 2 image
  static uint8_t buf[1024];
  file.seek(0);
  while (pos < size) {
    int n = file.read(buf, sizeof(buf));
    if (n <= 0) break;
    for (int i = 0; i < n; i++, pos++) {
      uint8_t c = buf[i];
      if (state == 0) {
        if (c == '-' && pos) { state = 1; dash = pos; }
        else if (c >= '0' && c <= '9' && pos < 12) { want = want * 10 + (c - '0'); continue; }
        else { r.error = "not a firmware file"; file.close(); return r; }
      }
      sum += c;
      if (state == 1) {
        if (c == '~') { state = 2; tilde = pos + 1; }
        else if (pos > dash && r.notes.size() < 700) r.notes += (char)c;
      }
    }
  }
  if (state < 2) { r.error = "not a firmware file (no '~')"; file.close(); return r; }
  if (sum != want) { r.error = "checksum mismatch - the file is damaged"; file.close(); return r; }
  const char* tag = "COCO ESP32 CUSTOM FIRMWARE/";
  if (r.notes.rfind(tag, 0) == 0) r.notes.erase(0, strlen(tag));
  r.ok = appImage(file, tilde, r);
  file.close();
  return r;
}
JS_NATIVE(sys_firmwareInfo) {  // sys.firmwareInfo(path) -> { ok, error, notes, size }
  (void)self;
  FlhInfo r = flhCheck(argc ? I.toStr(args[0]) : "");
  Object* o = I.newObject();
  I.setOwn(o, "ok", Value::boolean(r.ok));
  I.setOwn(o, "error", I.str(r.error));
  I.setOwn(o, "notes", I.str(r.notes));
  I.setOwn(o, "size", Value::num((double)r.imageSize));
  return Value::obj(o);
}
JS_NATIVE(sys_installFirmware) {  // sys.installFirmware(path): checks, writes the spare slot, restarts
  (void)self;
  std::string path = argc ? I.toStr(args[0]) : "";
  FlhInfo r = flhCheck(path);
  if (!r.ok) return I.str(r.error);
  if (!Update.begin(r.imageSize)) return I.str("this board has no room for an update (no OTA partition)");
  std::string rel;
  fs::FS* f = pickFs(path, rel);
  File file = f->open(rel.c_str(), "r");
  file.seek(r.imageAt);
  static uint8_t buf[4096];
  size_t done = 0;
  int W = tft.width(), H = tft.height(), bw = W - 40, by = H / 2 + 20;
  tft.drawRect(19, by - 1, bw + 2, 12, TFT_WHITE);
  while (done < r.imageSize) {
    int n = file.read(buf, std::min(sizeof(buf), r.imageSize - done));
    if (n <= 0 || Update.write(buf, n) != (size_t)n) break;
    done += n;
    tft.fillRect(20, by, (int)((uint64_t)bw * done / r.imageSize), 10, TFT_GREEN);
#ifdef BOARD_SCREEN_HEADER
    tft.service();
#endif
  }
  file.close();
  if (done != r.imageSize || !Update.end(true)) {
    std::string err = std::string("write failed: ") + Update.errorString();
    Update.abort();
    return I.str(err);
  }
  delay(300);
  ESP.restart();
  return Value();
}
bool hwReadFile(const std::string& path, std::string& out) {
  std::string rel;
  fs::FS* f = pickFs(path, rel);
  if (!f || !f->exists(rel.c_str())) return false;
  File file = f->open(rel.c_str(), "r");
  if (!file || file.isDirectory()) return false;
  size_t sz = file.size();
  // Loading a file is one contiguous allocation its whole size. When the heap is too fragmented
  // to hold it, fail gracefully here: on this board a failed operator new aborts (the C++ runtime
  // can't raise the out-of-memory exception when memory is this low), which would crash the board.
  if (sz + 2048 > heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)) { file.close(); return false; }
  out.clear();
  out.reserve(sz);
  char buf[512];
  int n;
  while ((n = file.read((uint8_t*)buf, sizeof buf)) > 0) out.append(buf, n);
  file.close();
  return true;
}

// Scan a file for a substring without loading it whole (a small sliding window). Used before a
// launch to decide whether the next app needs WiFi, so we can free the radio for the file read.
bool hwFileContains(const std::string& path, const char* needle) {
  std::string rel;
  fs::FS* f = pickFs(path, rel);
  if (!f || !f->exists(rel.c_str())) return false;
  File file = f->open(rel.c_str(), "r");
  if (!file || file.isDirectory()) return false;
  size_t nlen = strlen(needle);
  std::string window;
  char buf[256];
  int n;
  bool found = false;
  while ((n = file.read((uint8_t*)buf, sizeof buf)) > 0) {
    window.append(buf, n);
    if (window.find(needle) != std::string::npos) { found = true; break; }
    if (window.size() > nlen) window.erase(0, window.size() - (nlen - 1));  // keep the boundary tail
  }
  file.close();
  return found;
}

bool hwWriteFile(const std::string& path, const std::string& data, bool append) {
  std::string rel;
  fs::FS* f = pickFs(path, rel);
  if (!f) return false;
  File file = f->open(rel.c_str(), append ? FILE_APPEND : FILE_WRITE);
  if (!file) return false;
  size_t n = file.write((const uint8_t*)data.data(), data.size());
  file.close();
  return n == data.size();
}

bool hwRemove(const std::string& path) {
  std::string rel;
  fs::FS* f = pickFs(path, rel);
  return f && f->remove(rel.c_str());
}

std::vector<FileEntry> hwList(const std::string& dir) {
  std::vector<FileEntry> r;
  std::string rel;
  fs::FS* f = pickFs(dir.empty() ? "/" : dir, rel);
  if (!f) return r;
  File root = f->open(rel.c_str());
  if (!root || !root.isDirectory()) return r;
  for (File e = root.openNextFile(); e; e = root.openNextFile()) {
    std::string name = e.name();
    size_t slash = name.rfind('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    r.push_back({name, (size_t)e.size(), e.isDirectory()});
  }
  return r;
}

JS_NATIVE(fs_read) {
  (void)self;
  std::string out;
  if (!hwReadFile(I.toStr(JS_ARG(0)), out)) return Value::null();
  return I.str(out);
}
JS_NATIVE(fs_write) { (void)self; return Value::boolean(hwWriteFile(I.toStr(JS_ARG(0)), I.toStr(JS_ARG(1)))); }
JS_NATIVE(fs_append) { (void)self; return Value::boolean(hwWriteFile(I.toStr(JS_ARG(0)), I.toStr(JS_ARG(1)), true)); }
JS_NATIVE(fs_remove) { (void)self; return Value::boolean(hwRemove(I.toStr(JS_ARG(0)))); }
JS_NATIVE(fs_exists) {
  (void)self;
  std::string rel;
  fs::FS* f = pickFs(I.toStr(JS_ARG(0)), rel);
  return Value::boolean(f && f->exists(rel.c_str()));
}
JS_NATIVE(fs_list) {
  (void)self;
  Array* a = I.newArray();
  for (auto& e : hwList(argc ? I.toStr(args[0]) : "/")) a->items.push_back(I.str(e.dir ? e.name + "/" : e.name));
  return Value::obj(a);
}

static Value entryObj(Interp& I, const std::string& name, size_t size, bool dir) {
  Object* o = I.newObject();
  I.setOwn(o, "name", I.str(name));
  I.setOwn(o, "size", Value::num((double)size));
  I.setOwn(o, "dir", Value::boolean(dir));
  return Value::obj(o);
}
// Binary-safe file transfer for tools (tools/web-ide.html): the serial console carries text, so
// bytes travel as base64.
//   fs.writeBase64(path, b64, [append]) -> true;  fs.readBase64(path, offset, length) -> b64 ("" at the end)
JS_NATIVE(fs_writeBase64) {
  (void)self;
  std::string path = I.toStr(JS_ARG(0)), b64 = I.toStr(JS_ARG(1)), rel;
  bool append = argc > 2 && I.truthy(args[2]);
  size_t n = 0;
  std::string bytes(b64.size() * 3 / 4 + 4, '\0');
  if (mbedtls_base64_decode((unsigned char*)&bytes[0], bytes.size(), &n, (const unsigned char*)b64.data(), b64.size()) != 0)
    I.throwError("Error", "fs.writeBase64: that isn't base64");
  fs::FS* f = pickFs(path, rel);
  File file = f ? f->open(rel.c_str(), append ? "a" : "w") : File();
  if (!file) return Value::boolean(false);
  bool ok = file.write((const uint8_t*)bytes.data(), n) == n;
  file.close();
  return Value::boolean(ok);
}
JS_NATIVE(fs_readBase64) {
  (void)self;
  std::string path = I.toStr(JS_ARG(0)), rel;
  size_t off = argc > 1 ? (size_t)I.toNum(args[1]) : 0, len = argc > 2 ? (size_t)I.toNum(args[2]) : 480;
  len = std::min<size_t>(len, 3000);
  fs::FS* f = pickFs(path, rel);
  File file = f ? f->open(rel.c_str(), "r") : File();
  if (!file) return Value::null();
  std::string bytes(len, '\0');
  int got = file.seek(off) ? file.read((uint8_t*)&bytes[0], len) : 0;
  file.close();
  if (got <= 0) return I.str("");
  std::string out(((size_t)got + 2) / 3 * 4 + 1, '\0');
  size_t n = 0;
  mbedtls_base64_encode((unsigned char*)&out[0], out.size(), &n, (const unsigned char*)bytes.data(), got);
  out.resize(n);
  return I.str(out);
}
JS_NATIVE(fs_contains) {  // fs.contains(path, text): searched in pieces, never loaded whole
  (void)self;
  return Value::boolean(hwFileContains(I.toStr(JS_ARG(0)), I.toStr(JS_ARG(1)).c_str()));
}
JS_NATIVE(fs_dir) {  // fs.dir(path) -> [{ name, size, dir }]
  (void)self;
  Array* a = I.newArray();
  for (auto& e : hwList(argc ? I.toStr(args[0]) : "/")) a->items.push_back(entryObj(I, e.name, e.size, e.dir));
  return Value::obj(a);
}
JS_NATIVE(fs_stat) {  // fs.stat(path) -> { name, size, dir } or null
  (void)self;
  std::string path = I.toStr(JS_ARG(0)), rel;
  fs::FS* f = pickFs(path, rel);
  if (!f || !f->exists(rel.c_str())) return Value::null();
  File e = f->open(rel.c_str());
  if (!e) return Value::null();
  Value v = entryObj(I, path.substr(path.rfind('/') + 1), e.size(), e.isDirectory());
  e.close();
  return v;
}
JS_NATIVE(fs_mkdir) { (void)self; std::string rel; fs::FS* f = pickFs(I.toStr(JS_ARG(0)), rel); return Value::boolean(f && f->mkdir(rel.c_str())); }
JS_NATIVE(fs_rmdir) { (void)self; std::string rel; fs::FS* f = pickFs(I.toStr(JS_ARG(0)), rel); return Value::boolean(f && f->rmdir(rel.c_str())); }
JS_NATIVE(fs_rename) {  // fs.rename(from, to): on the same storage (flash or SD)
  (void)self;
  std::string ra, rb;
  fs::FS* a = pickFs(I.toStr(JS_ARG(0)), ra);
  fs::FS* b = pickFs(I.toStr(JS_ARG(1)), rb);
  return Value::boolean(a && a == b && a->rename(ra.c_str(), rb.c_str()));
}
JS_NATIVE(fs_copy) {  // fs.copy(from, to): any size, also between flash and SD
  (void)self;
  std::string ra, rb;
  fs::FS* a = pickFs(I.toStr(JS_ARG(0)), ra);
  fs::FS* b = pickFs(I.toStr(JS_ARG(1)), rb);
  if (!a || !b) return Value::boolean(false);
  File in = a->open(ra.c_str(), "r");
  if (!in || in.isDirectory()) return Value::boolean(false);
  File out = b->open(rb.c_str(), "w");
  if (!out) { in.close(); return Value::boolean(false); }
  static uint8_t buf[4096];
  bool ok = true;
  for (int n; (n = in.read(buf, sizeof(buf))) > 0;)
    if (out.write(buf, n) != (size_t)n) { ok = false; break; }
  in.close();
  out.close();
  if (!ok) b->remove(rb.c_str());  // don't leave half a file (e.g. the card filled up)
  return Value::boolean(ok);
}
JS_NATIVE(fs_space) {  // fs.space("/" or "/sd") -> { total, used } in bytes
  (void)self;
  std::string path = argc ? I.toStr(args[0]) : "/", rel;
  bool sd = pickFs(path, rel) == (fs::FS*)&SDCARD;
  Object* o = I.newObject();
  double total = sd ? (sdOk ? (double)SDCARD.totalBytes() : 0) : (fsOk ? (double)LittleFS.totalBytes() : 0);
  double used = sd ? (sdOk ? (double)SDCARD.usedBytes() : 0) : (fsOk ? (double)LittleFS.usedBytes() : 0);
  I.setOwn(o, "total", Value::num(total));
  I.setOwn(o, "used", Value::num(used));
  return Value::obj(o);
}

// ------------------------------------------------------------------ settings file
// readConfig(): the parsed contents of /sd/micro-js.json, else /micro-js.json (flash), else {}.
// The project's older names (micro-js.json, cyd-js.json) are still read when there's no micro-js.json.
static const char* CONFIG_PATHS[] = {"/sd/micro-js.json", "/micro-js.json", "/sd/esp32-js.json", "/esp32-js.json",
                                     "/sd/cyd-js.json", "/cyd-js.json"};

// The time zone for Date's local-time methods: utcOffsetHours from the config file, as a POSIX TZ
// string ("UTC5" is 5 hours *behind* UTC). Set once at boot; wifi.ntp() keeps it.
static std::string localTz = "UTC0";
// "ledPins": [r, g, b] in micro-js.json moves the RGB LED to other pins (read once at boot)
static void ledPinsFromConfig() {
#if BOARD_LED_R >= 0
  for (const char* path : CONFIG_PATHS) {
    std::string text;
    if (!hwReadFile(path, text)) continue;
    size_t k = text.find("\"ledPins\"");
    if (k == std::string::npos) return;
    size_t b = text.find('[', k);
    if (b == std::string::npos) return;
    int pins[3];
    const char* p = text.c_str() + b + 1;
    for (int i = 0; i < 3; i++) {
      char* e;
      long v = strtol(p, &e, 10);
      if (e == p || v < 0 || v > 48) return;
      pins[i] = (int)v;
      p = e;
      while (*p == ',' || *p == ' ') p++;
    }
    int chans[3] = {CH_R, CH_G, CH_B};
    for (int i = 0; i < 3; i++) {
      if (pins[i] == ledPins[i]) continue;
      ledcDetachPin(ledPins[i]);
      pinMode(ledPins[i], INPUT);
      ledPins[i] = pins[i];
      pwmAttach(ledPins[i], chans[i], 5000);
    }
    ledWrite(BOARD_LED_IDLE);
    return;
  }
#endif
}

// "speakerEnable": [pin, level] in micro-js.json drives a pin at boot that switches the speaker's
// amplifier on (CYDs that moved the red LED to GPIO 22 use GPIO 4, LOW, for this)
static void speakerEnableFromConfig() {
  for (const char* path : CONFIG_PATHS) {
    std::string text;
    if (!hwReadFile(path, text)) continue;
    size_t k = text.find("\"speakerEnable\"");
    if (k == std::string::npos) return;
    size_t b = text.find('[', k);
    if (b == std::string::npos) return;
    char* e;
    long pin = strtol(text.c_str() + b + 1, &e, 10);
    while (*e == ',' || *e == ' ') e++;
    long level = strtol(e, nullptr, 10);
    if (pin < 0 || pin > 48) return;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, level ? HIGH : LOW);
    return;
  }
}

static void setTimeZoneFromConfig() {
  for (const char* path : CONFIG_PATHS) {
    std::string text;
    if (!hwReadFile(path, text)) continue;
    size_t k = text.find("\"utcOffsetHours\"");
    if (k != std::string::npos) {
      size_t c = text.find(':', k);
      if (c != std::string::npos) {
        double hours = strtod(text.c_str() + c + 1, nullptr);
        int mins = (int)lround(-hours * 60);  // POSIX sign is reversed
        char tz[24];
        snprintf(tz, sizeof tz, "UTC%s%d:%02d", mins < 0 ? "-" : "", abs(mins) / 60, abs(mins) % 60);
        localTz = tz;
      }
    }
    break;
  }
  setenv("TZ", localTz.c_str(), 1);
  tzset();
}
JS_NATIVE(g_readConfig) {
  (void)self; (void)args; (void)argc;
  for (const char* path : CONFIG_PATHS) {
    std::string src;
    if (!hwReadFile(path, src)) continue;
    Value json = I.getGlobal("JSON"), text = I.str(src);
    try {
      return I.call(I.get(json, "parse"), json, &text, 1);
    } catch (JsThrow& e) {
      I.throwError("SyntaxError", std::string(path) + ": " + I.toStr(I.get(e.v, "message")));
    }
  }
  return Value::obj(I.newObject());
}

// ------------------------------------------------------------------ WiFi & HTTP
#if JS_WIFI
// Network reserve: a block set aside while the heap is still clean and released for the
// duration of each HTTP request, so connections and responses always find contiguous memory
// even after a script has fragmented the rest of the heap.
static const size_t NET_RESERVE = 12 * 1024;
static void* netReserve = nullptr;
static void netReserveTake() {
  for (size_t n = NET_RESERVE; !netReserve && n >= 4096; n /= 2) netReserve = malloc(n);
}
static void netReserveRelease() {
  free(netReserve);
  netReserve = nullptr;
}

// Turning WiFi off right after a connect was interrupted (Ctrl-C or BOOT during wifi.connect)
// deinitialises a driver that is still busy stopping: that crashes the board or leaves WiFi
// dead until reset. Abort the connect first and give the driver time to settle.
static void wifiOff() {
  if (dnsServer) { dnsServer->stop(); delete dnsServer; dnsServer = nullptr; }
  if (WiFi.getMode() & WIFI_AP) WiFi.softAPdisconnect(true);
  if (WiFi.getMode() == WIFI_OFF) return;
  WiFi.disconnect();
  for (int i = 0; i < 3 && WiFi.getMode() != WIFI_OFF; i++) {
    vTaskDelay(200);
    WiFi.mode(WIFI_OFF);
  }
}
// WiFi.mode(WIFI_STA), recovering once from a driver left half-stopped
static bool wifiOn() {
  if (WiFi.mode(WIFI_STA)) return true;
  wifiOff();
  vTaskDelay(200);
  return WiFi.mode(WIFI_STA);
}

JS_NATIVE(wifi_connect) {
  (void)self;
  std::string ssid = I.toStr(JS_ARG(0)), pass = argc > 1 ? I.toStr(args[1]) : "";
  uint32_t timeout = argc > 2 ? (uint32_t)IA(2) : 15000;
  if (!wifiOn()) I.throwError("Error", "wifi: the WiFi driver failed to start - restart the board");
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t start = millis();
  bool retried = false;
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeout) {
    if (I.poll) I.poll();
    vTaskDelay(50);
    // a first attempt that stalls (common with mesh networks) usually works on a fresh try
    if (!retried && millis() - start > timeout / 2) {
      retried = true;
      WiFi.disconnect();
      vTaskDelay(100);
      WiFi.begin(ssid.c_str(), pass.c_str());
    }
  }
  bool ok = WiFi.status() == WL_CONNECTED;
  if (ok) WiFi.setSleep(false);  // modem sleep causes HTTP timeouts; the board is USB-powered
  return Value::boolean(ok);
}
// The board's own network, when it can't join one: wifi.ap([name], [password]) ->
// { ssid, ip, password }. Without a name it's "micro-js-" and the end of the board's MAC address;
// without a password it's open (a password needs 8 or more characters). A small DNS server answers
// every name with the board's address, so phones that join show its page as a sign-in ("captive")
// page - serve your page for any path (web.on("*", ...)) to make that work.
JS_NATIVE(wifi_ap) {
  (void)self;
  std::string ssid = argc > 0 && args[0].isStr() ? args[0].s()->s : "", pass = argc > 1 && args[1].isStr() ? args[1].s()->s : "";
  if (!pass.empty() && pass.size() < 8) I.throwError("Error", "wifi.ap: the password needs 8 or more characters (or none)");
  if (ssid.empty()) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[24];
    snprintf(buf, sizeof buf, "micro-js-%02X%02X", mac[4], mac[5]);
    ssid = buf;
  }
  WiFi.disconnect();
  if (!WiFi.mode(WIFI_AP)) { wifiOff(); vTaskDelay(200); WiFi.mode(WIFI_AP); }
  if (!WiFi.softAP(ssid.c_str(), pass.empty() ? nullptr : pass.c_str())) I.throwError("Error", "wifi.ap: couldn't start the access point");
  WiFi.setSleep(false);
  vTaskDelay(100);
  if (!dnsServer) dnsServer = new DNSServer();
  dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer->start(53, "*", WiFi.softAPIP());
  Object* o = I.newObject();
  I.setOwn(o, "ssid", I.str(ssid));
  I.setOwn(o, "ip", I.str(WiFi.softAPIP().toString().c_str()));
  I.setOwn(o, "password", I.str(pass));
  return Value::obj(o);
}
JS_NATIVE(wifi_status) {
  (void)self; (void)args; (void)argc;
  if ((WiFi.getMode() & WIFI_AP) && WiFi.status() != WL_CONNECTED) return I.str("ap");
  switch (WiFi.status()) {
    case WL_CONNECTED: return I.str("connected");
    case WL_NO_SSID_AVAIL: return I.str("no-ssid");
    case WL_CONNECT_FAILED: return I.str("failed");
    case WL_CONNECTION_LOST: return I.str("lost");
    case WL_DISCONNECTED: return I.str("disconnected");
    case WL_IDLE_STATUS: return I.str("idle");
    default: return I.str("unknown");
  }
}
JS_NATIVE(wifi_ip) {
  (void)self; (void)args; (void)argc;
  bool ap = (WiFi.getMode() & WIFI_AP) && WiFi.status() != WL_CONNECTED;
  return I.str((ap ? WiFi.softAPIP() : WiFi.localIP()).toString().c_str());
}
JS_NATIVE(wifi_rssi) { (void)I; (void)self; (void)args; (void)argc; return Value::num(WiFi.RSSI()); }
JS_NATIVE(wifi_disconnect) { (void)I; (void)self; (void)args; (void)argc; wifiOff(); return Value(); }
JS_NATIVE(wifi_scan) {
  (void)self; (void)args; (void)argc;
  wifiOn();
  int n = WiFi.scanNetworks();
  Array* a = I.newArray();
  for (int i = 0; i < n; i++) {
    Object* o = I.newObject();
    I.setOwn(o, "ssid", I.str(WiFi.SSID(i).c_str()));
    I.setOwn(o, "rssi", Value::num(WiFi.RSSI(i)));
    I.setOwn(o, "open", Value::boolean(WiFi.encryptionType(i) == WIFI_AUTH_OPEN));
    a->items.push_back(Value::obj(o));
  }
  WiFi.scanDelete();
  return Value::obj(a);
}
JS_NATIVE(wifi_ntp) {
  (void)self;
  // NTP itself is UTC; the zone only affects local time. Without an explicit zone (or with the
  // old default "UTC0"), keep the one from the config file so Date's local methods stay right.
  std::string tz = argc > 0 && !args[0].isUndef() ? I.toStr(args[0]) : "UTC0";
  if (tz == "UTC0") tz = localTz;
  std::string server = argc > 1 ? I.toStr(args[1]) : "pool.ntp.org";
  configTzTime(tz.c_str(), server.c_str());
  uint32_t start = millis();
  while (!clockValid() && millis() - start < 10000) {
    if (I.poll) I.poll();
    vTaskDelay(100);
  }
  return Value::boolean(clockValid());
}

static Value httpRequest(Interp& I, const std::string& method, const std::string& url, const std::string* body,
                         Value headers) {
  if (WiFi.status() != WL_CONNECTED) I.throwError("Error", "http: WiFi is not connected");
  netReserveRelease();
  struct Retake { ~Retake() { netReserveTake(); } } retake;  // also on errors
  HTTPClient http;
  WiFiClient plain;
  std::unique_ptr<WiFiClientSecure> secure;  // TLS state is large: only create it for https
  bool https = url.rfind("https://", 0) == 0;
  if (https) {
    secure.reset(new WiFiClientSecure());
    secure->setInsecure();  // no certificate validation
  }
  auto send = [&]() {
    bool ok = https ? http.begin(*secure, url.c_str()) : http.begin(plain, url.c_str());
    if (!ok) I.throwError("Error", "http: bad url " + url);
    http.setTimeout(15000);
    if (headers.isObj())
      for (auto& k : I.ownKeys(headers)) http.addHeader(k.c_str(), I.toStr(I.getProp(headers, k.data(), k.size())).c_str());
    return body ? http.sendRequest(method.c_str(), (uint8_t*)body->data(), body->size())
                : http.sendRequest(method.c_str());
  };
  int status = send();
  if (status == HTTPC_ERROR_CONNECTION_REFUSED && !https) {
    // the first request just after connecting often fails (DNS not ready yet): try once more
    http.end();
    vTaskDelay(500);
    status = send();
  }
  if (status < 0) {
    std::string err = http.errorToString(status).c_str();
    http.end();
    if (https && status == HTTPC_ERROR_CONNECTION_REFUSED)
      err += " (HTTPS needs ~40 KB of free contiguous memory for the TLS handshake; largest free block is " +
             std::to_string(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)) + " bytes - try http:// if the site allows it)";
    I.throwError("Error", "http: " + err);
  }
  String resp = http.getString();
  http.end();
  Object* o = I.newObject();
  I.setOwn(o, "status", Value::num(status));
  I.setOwn(o, "body", I.str(std::string(resp.c_str(), resp.length())));
  return Value::obj(o);
}
JS_NATIVE(http_get) { (void)self; return httpRequest(I, "GET", I.toStr(JS_ARG(0)), nullptr, JS_ARG(1)); }
JS_NATIVE(http_post) {
  (void)self;
  Value b = JS_ARG(1);
  std::string body = b.isObj() ? I.toStr(I.call(I.get(I.getGlobal("JSON"), "stringify"), Value(), &b, 1)) : I.toStr(b);
  Value headers = JS_ARG(2);
  if (headers.isUndef() && b.isObj()) {
    Object* h = I.newObject();
    I.setOwn(h, "Content-Type", I.str("application/json"));
    headers = Value::obj(h);
  }
  return httpRequest(I, "POST", I.toStr(JS_ARG(0)), &body, headers);
}
#endif

// ------------------------------------------------------------------ setup
void hwBegin() {
  tft.init();
  tft.setRotation(rotation);
  tft.fillScreen(TFT_BLACK);

#if defined(BOARD_LED_BICOLOR_A)
  pwmAttach(BOARD_LED_BICOLOR_A, CH_R, 5000);
  pwmAttach(BOARD_LED_BICOLOR_B, CH_G, 5000);
#elif BOARD_LED_R >= 0
  pwmAttach(ledPins[0], CH_R, 5000);
  pwmAttach(ledPins[1], CH_G, 5000);
  pwmAttach(ledPins[2], CH_B, 5000);
#endif
  ledWrite(BOARD_LED_IDLE);
#if BOARD_BACKLIGHT >= 0
  pwmAttach(BOARD_BACKLIGHT, CH_BL, 5000);
#endif
  backlightWrite(255);
#if BOARD_SPEAKER_PWM >= 0
  pwmAttach(BOARD_SPEAKER_PWM, CH_SPK, 1000);
#endif
  toneWrite(0);
  touchBegin();
#if BOARD_LIGHT_SENSOR >= 0
  analogSetPinAttenuation(BOARD_LIGHT_SENSOR, ADC_0db);
#endif

  fsOk = LittleFS.begin(true);
  loadTouchCal();
#if defined(BOARD_SD_MMC_CLK)
  SD_MMC.setPins(BOARD_SD_MMC_CLK, BOARD_SD_MMC_CMD, BOARD_SD_MMC_D0);
  sdOk = SD_MMC.begin("/sdcard", true);  // 1-bit mode
#elif BOARD_SD_CS >= 0
#if BOARD_SD_PARK_CS >= 0
  pinMode(BOARD_SD_PARK_CS, OUTPUT);           // deselect the other device sharing this SPI bus
  digitalWrite(BOARD_SD_PARK_CS, HIGH);        // (e.g. a LoRa module), else it corrupts SD reads
#endif
  pinMode(BOARD_SD_MISO, INPUT_PULLUP);        // DAT0/MISO needs a pull-up to read reliably
  static SPIClass sdSpi(BOARD_SD_SPI_HOST);  // its own bus: the display has the other one
  sdSpi.begin(BOARD_SD_SCK, BOARD_SD_MISO, BOARD_SD_MOSI, BOARD_SD_CS);
  sdOk = SD.begin(BOARD_SD_CS, sdSpi);
#endif
  setTimeZoneFromConfig();
  ledPinsFromConfig();
  speakerEnableFromConfig();
  keyboardBegin();  // after the filesystems: a USB keyboard driver's timer must not run during a format
#if BOARD_SPEAKER_I2S_DOUT >= 0
  speakerBegin();  // after the keyboard and touch: an ES8311 codec shares their I2C bus
#endif
}

void hwSplash() {
  int w = tft.width(), h = tft.height();
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("micro-js", w / 2, h / 2 - 30, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("JavaScript ready  -  v" MICROJS_VERSION, w / 2, h / 2 + 5, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Serial REPL @ 115200 baud  -  type .help", w / 2, h / 2 + 30, 1);
  char buf[64];
  snprintf(buf, sizeof buf, "%s   flash: %s   sd: %s   heap: %u", BOARD_NAME, fsOk ? "ok" : "--", sdOk ? "ok" : "--",
           (unsigned)ESP.getFreeHeap());
  tft.drawString(buf, w / 2, h / 2 + 45, 1);
  tft.setTextDatum(TL_DATUM);
}

void hwService() {
#ifdef BOARD_SCREEN_HEADER
  tft.service();
#endif
  if (toneEnd && (int32_t)(millis() - toneEnd) >= 0) {
    toneWrite(0);
    toneEnd = 0;
  }
}

void hwStop(Interp& I) {
  (void)I;
  keyCapture = false;
  joyKeys = true;
  if (joyHandlers) joyHandlers->props.clear();
  if (touchHandlers) touchHandlers->props.clear();
  if (keyHandlers) keyHandlers->props.clear();
  toneWrite(0);
  toneEnd = 0;
}

void hwReset() {
  videoStopWait();  // leaving an app stops its video
#if JS_WIFI
  webStop();  // and its web server
#endif
#if HAS_MUSIC
  mp3StopWait();  // leaving an app stops its music
#endif
  toneWrite(0);
  toneEnd = 0;
  ledWrite(BOARD_LED_IDLE);
  backlightWrite(255);
  rotation = BOARD_ROTATION;
  tft.setRotation(rotation);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextPadding(0);
  tft.setTextWrap(true, false);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(0, 0);
  tft.fillScreen(TFT_BLACK);
}

// The WiFi driver needs several large contiguous blocks. Started after a big script has been
// parsed, it often cannot get them ("failed to create task"), so scripts that use WiFi get it
// started first; scripts that don't get its memory back.

void hwNetReserve() {
#if JS_WIFI
  netReserveTake();
#endif
}

void hwNetRelease() {
#if JS_WIFI
  netReserveRelease();  // free the HTTP reserve so the next script load has contiguous room
#endif
}

void hwWifiPrepare(bool use) {
#if JS_WIFI
  if (use) {
    wifiOn();
  } else {
    netReserveRelease();
    wifiOff();
  }
#else
  (void)use;
#endif
}

// ------------------------------------------------------------------ on-screen console
// On boards with a keyboard, the REPL also lives on the screen whenever no app is running: what it
// prints is kept in a character grid (so it's there to show when an app ends) and drawn while the
// console is up; unhandled keys are typed into it (hwConsoleKey, in main.cpp). 6x8 font.
bool hwHasHandlers() {
  return (keyHandlers && !keyHandlers->props.empty()) || (touchHandlers && !touchHandlers->props.empty()) ||
         (joyHandlers && !joyHandlers->props.empty());
}
#if BOARD_KEYBOARD != KEYBOARD_NONE
static const int CON_MAX_COLS = 64, CON_MAX_ROWS = 40;
static const uint16_t CON_FG = TFT_LIGHTGREY, CON_BG = TFT_BLACK, CON_CURSOR = TFT_GREEN;
static char conGrid[CON_MAX_ROWS][CON_MAX_COLS + 1];
static int conCols = 0, conRows = 0, conX = 0, conY = 0, conEsc = 0;

static void conText() {
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
  tft.setTextColor(CON_FG, CON_BG);
}
static void conInit() {
  if (conCols) return;
  conCols = std::min(CON_MAX_COLS, (int)tft.width() / 6);
  conRows = std::min(CON_MAX_ROWS, (int)tft.height() / 8);
  for (int r = 0; r < CON_MAX_ROWS; r++) {
    memset(conGrid[r], ' ', CON_MAX_COLS);
    conGrid[r][CON_MAX_COLS] = 0;
  }
}
static void conDrawRow(int r) {
  char saved = conGrid[r][conCols];
  conGrid[r][conCols] = 0;
  tft.drawString(conGrid[r], 0, r * 8);
  conGrid[r][conCols] = saved;
}
static void conCursor(bool on) { tft.fillRect(conX * 6, conY * 8 + 7, 6, 1, on ? CON_CURSOR : CON_BG); }
static void conNewline() {
  conX = 0;
  if (++conY < conRows) return;
  conY = conRows - 1;
  memmove(conGrid[0], conGrid[1], sizeof conGrid[0] * (conRows - 1));
  memset(conGrid[conRows - 1], ' ', CON_MAX_COLS);
  if (conActive)
    for (int r = 0; r < conRows; r++) conDrawRow(r);
}
static void conPut(char c) {
  if (conEsc) {  // ANSI sequences: ESC [ digits/; letter. Only "erase to end of line" matters here.
    if (conEsc == 1) { conEsc = c == '[' ? 2 : 0; return; }
    if (isdigit((unsigned char)c) || c == ';') return;
    conEsc = 0;
    if (c == 'K') {
      memset(conGrid[conY] + conX, ' ', CON_MAX_COLS - conX);
      if (conActive) conDrawRow(conY);
    }
    return;
  }
  switch (c) {
    case 27: conEsc = 1; return;
    case '\r': conX = 0; return;
    case '\n': conNewline(); return;
    case '\b': if (conX > 0) conX--; return;
    case '\t': do conPut(' '); while (conX % 2); return;
  }
  if ((uint8_t)c < 32) return;
  conGrid[conY][conX] = c;
  if (conActive) tft.drawChar(c, conX * 6, conY * 8, 1);
  if (++conX >= conCols) conNewline();
}
void hwConsoleWrite(const char* s, size_t n) {
  conInit();
  if (conActive) { conCursor(false); conText(); }
  for (size_t i = 0; i < n; i++) conPut(s[i]);
  if (conActive) conCursor(true);
}
void hwConsoleShow(bool on) {
  conInit();
  conActive = on;
  if (!on) return;
  tft.fillScreen(CON_BG);
  conText();
  for (int r = 0; r < conRows; r++) conDrawRow(r);
  conCursor(true);
}
#else
void hwConsoleWrite(const char*, size_t) {}
void hwConsoleShow(bool) {}
#endif

bool hwLowMemory() {
#if CONFIG_IDF_TARGET_ESP32
  return !psramFound();
#else
  return false;  // the S3's 512 KB is enough even without PSRAM (Cardputer)
#endif
}

void hwShowNotice(const std::string& title, const std::string& msg) {
  int w = tft.width(), h = tft.height();
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString(title.c_str(), w / 2, h / 2 - 30, h < 200 ? 2 : 4);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  // the message, word-wrapped to the screen
  std::string line, rest = msg;
  int y = h / 2 + (h < 200 ? -8 : 4), font = h < 200 ? 1 : 2;
  while (!rest.empty()) {
    size_t cut = rest.size();
    while (cut > 0 && tft.textWidth(rest.substr(0, cut).c_str(), font) > w - 16) {
      size_t sp = rest.rfind(' ', cut - 1);
      cut = sp == std::string::npos || sp == 0 ? cut - 1 : sp;
    }
    tft.drawString(rest.substr(0, cut).c_str(), w / 2, y, font);
    y += font == 1 ? 10 : 18;
    rest = rest.substr(cut);
    while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
  }
  tft.setTextDatum(TL_DATUM);
}

void hwShowError(const std::string& title, const std::string& msg) {
  tft.fillScreen(TFT_MAROON);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_MAROON);
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextWrap(true, false);
  tft.setCursor(0, 10);
  tft.println((" " + title).c_str());
  tft.println((" " + msg).c_str());
  tft.println("");
  tft.println(" Press BOOT to return to the menu.");
}

static void fire(Interp& I, const char* ev, int x, int y, int z) {
  Prop* p = touchHandlers->own(ev);
  if (!p || !I.isCallable(p->v)) return;
  size_t mark = I.temps.size();
  Value fn = p->v;
  I.rootTemp(fn);
  Value e = pointObj(I, x, y, z);
  I.callSafe(fn, Value(), {e});
  I.temps.resize(mark);
}

#if BOARD_KEYBOARD == KEYBOARD_CARDPUTER_TCA8418
static void fireKey(Interp& I, const char* ev, int row, int col) {
  Prop* p = keyHandlers->own(ev);
  if (!p || !I.isCallable(p->v)) {
    uint8_t mods = (kbDown[2][1] ? 1 : 0) | (kbDown[3][0] ? 2 : 0) | (kbDown[3][2] ? 4 : 0);
    if (keyHandlers->props.empty() && !strcmp(ev, "key")) hwConsoleKey(keyName(row, col), mods);
    return;
  }
  size_t mark = I.temps.size();
  Value fn = p->v;
  I.rootTemp(fn);
  Object* e = I.newObject();
  I.setOwn(e, "key", I.str(keyName(row, col)));
  I.setOwn(e, "shift", Value::boolean(kbDown[2][1]));
  I.setOwn(e, "fn", Value::boolean(kbDown[2][0]));
  I.setOwn(e, "ctrl", Value::boolean(kbDown[3][0]));
  I.setOwn(e, "opt", Value::boolean(kbDown[3][1]));
  I.setOwn(e, "alt", Value::boolean(kbDown[3][2]));
  I.callSafe(fn, Value(), {Value::obj(e)});
  I.temps.resize(mark);
}
#endif

void hwPoll(Interp& I) {
#if BOARD_KEYBOARD != KEYBOARD_NONE
  static uint32_t lastKey = 0;
  if (keyHandlers && millis() - lastKey >= 5) {
    lastKey = millis();
    keyboardPoll(I);
  }
#endif
  joystickPoll(I);
#if JS_WIFI
  if (dnsServer) dnsServer->processNextRequest();  // captive portal DNS (wifi.ap)
  if (webServer) webServer->handleClient();  // answer web requests (web.on handlers)
#endif
  static uint32_t last = 0;
  static bool down = false;
  static int tx = 0, ty = 0, tz = 0, releases = 0;
  hwService();
  uint32_t now = millis();
  if (now - last < 15 || !touchHandlers || touchHandlers->props.empty()) return;
  last = now;
  int rx, ry, rz, sx, sy;
  if (touchRaw(rx, ry, rz)) {
    mapTouch(rx, ry, sx, sy);
    releases = 0;
    if (!down) {
      down = true;
      tx = sx; ty = sy; tz = rz;
      fire(I, "down", sx, sy, rz);
    } else if (abs(sx - tx) > 2 || abs(sy - ty) > 2) {
      tx = sx; ty = sy; tz = rz;
      fire(I, "move", sx, sy, rz);
    }
  } else if (down && ++releases >= 2) {
    down = false;
    fire(I, "up", tx, ty, tz);
  }
}

// Built-in functions are created on first use (see NativeEntry in js.h).
static const NativeEntry LAZY_G[] = {{"fillScreen", d_fillScreen}, {"drawPixel", d_drawPixel}, {"readPixel", d_readPixel}, {"drawLine", d_drawLine}, {"drawWideLine", d_drawWideLine}, {"drawFastHLine", d_drawFastHLine}, {"drawFastVLine", d_drawFastVLine}, {"drawRect", d_drawRect}, {"fillRect", d_fillRect}, {"drawRoundRect", d_drawRoundRect}, {"fillRoundRect", d_fillRoundRect}, {"fillSmoothRoundRect", d_fillSmoothRoundRect}, {"drawCircle", d_drawCircle}, {"fillCircle", d_fillCircle}, {"fillSmoothCircle", d_fillSmoothCircle}, {"drawEllipse", d_drawEllipse}, {"fillEllipse", d_fillEllipse}, {"drawTriangle", d_drawTriangle}, {"fillTriangle", d_fillTriangle}, {"drawArc", d_drawArc}, {"drawString", d_drawString}, {"print", d_print}, {"println", d_println}, {"setTextColor", d_setTextColor}, {"setTextSize", d_setTextSize}, {"setTextFont", d_setTextFont}, {"setTextDatum", d_setTextDatum}, {"setTextPadding", d_setTextPadding}, {"setTextWrap", d_setTextWrap}, {"setCursor", d_setCursor}, {"textWidth", d_textWidth}, {"fontHeight", d_fontHeight}, {"width", d_width}, {"height", d_height}, {"color", d_color}, {nullptr, nullptr}};
static const NativeEntry LAZY_lcd[] = {{"setRotation", lcd_setRotation}, {"getRotation", lcd_getRotation}, {"invert", lcd_invert}, {"backlight", g_backlight}, {"createSprite", lcd_createSprite}, {"drawImage", lcd_drawImage}, {"imageSize", lcd_imageSize}, {nullptr, nullptr}};
static const NativeEntry LAZY_touch[] = {{"read", touch_read}, {"raw", touch_raw}, {"calibrate", touch_calibrate}, {"on", touch_on}, {"off", touch_off}, {nullptr, nullptr}};
static const NativeEntry LAZY_sys[] = {{"heap", sys_heap}, {"restart", sys_restart}, {"battery", sys_battery}, {"cpuMhz", sys_cpuMhz}, {"firmwareInfo", sys_firmwareInfo}, {"i2cScan", sys_i2cScan}, {"i2cWrite", sys_i2cWrite}, {"i2cRead", sys_i2cRead}, {"installFirmware", sys_installFirmware}, {nullptr, nullptr}};
static const NativeEntry LAZY_fsObj[] = {{"read", fs_read}, {"write", fs_write}, {"append", fs_append}, {"remove", fs_remove}, {"exists", fs_exists}, {"list", fs_list}, {"dir", fs_dir}, {"contains", fs_contains}, {"writeBase64", fs_writeBase64}, {"readBase64", fs_readBase64}, {"stat", fs_stat}, {"mkdir", fs_mkdir}, {"rmdir", fs_rmdir}, {"rename", fs_rename}, {"copy", fs_copy}, {"space", fs_space}, {nullptr, nullptr}};
static const NativeEntry LAZY_GLOBALS[] = {{"led", g_led}, {"backlight", g_backlight}, {"tone", g_tone}, {"noTone", g_noTone}, {"light", g_light}, {"pinMode", g_pinMode}, {"digitalWrite", g_digitalWrite}, {"digitalRead", g_digitalRead}, {"analogRead", g_analogRead}, {"delay", g_delay}, {"micros", g_micros}, {"localTime", g_localTime}, {"readConfig", g_readConfig}, {nullptr, nullptr}};
#if JS_WIFI
// ---- web: a small HTTP server for apps (web.js). Requests are answered from hwPoll, in the app's
// own event loop, so a handler can draw and beep like any other event handler.
//   web.start([port]) -> true; web.on(path, (req) => reply); web.off([path]); web.stop(); web.url()
//   req: { method, path, query: {name: value}, body, ip }. reply: a string (HTML if it starts with
//   "<", else text), an object (sent as JSON), { status, type, body }, or { file: path } to send a
//   file straight from flash or SD (any size; the type comes from its extension). web.on("*", fn)
//   catches any other path.
#if JS_WIFI
static Object* webRoutes = nullptr;  // path -> handler (rooted)
static Interp* webInterp = nullptr;
static void webRespond(Interp& I, Value r) {
  if (r.isStr()) {
    const std::string& t = r.s()->s;
    webServer->send(200, !t.empty() && t[0] == '<' ? "text/html; charset=utf-8" : "text/plain; charset=utf-8", t.c_str());
    return;
  }
  if (!r.isObj()) { webServer->send(200, "text/plain", ""); return; }
  Value fileV = I.get(r, "file");
  if (fileV.isStr()) {  // { file: path }: sent straight from storage, never held in memory
    std::string path = fileV.s()->s, rel, ext = imgExt(path);
    fs::FS* f = pickFs(path, rel);
    File file = f ? f->open(rel.c_str(), "r") : File();
    if (!file || file.isDirectory()) { webServer->send(404, "text/plain", "not found"); return; }
    Value ty0 = I.get(r, "type");
    const char* type = !ty0.isUndef() ? nullptr : ext == "html" || ext == "htm" ? "text/html; charset=utf-8" : ext == "js" ? "text/javascript" :
                       ext == "css" ? "text/css" : ext == "json" ? "application/json" : ext == "png" ? "image/png" :
                       ext == "jpg" || ext == "jpeg" ? "image/jpeg" : ext == "mp3" ? "audio/mpeg" : "text/plain; charset=utf-8";
    webServer->streamFile(file, type ? String(type) : String(I.toStr(ty0).c_str()));
    file.close();
    return;
  }
  Value st = I.get(r, "status"), ty = I.get(r, "type"), body = I.get(r, "body");
  if (st.isUndef() && ty.isUndef() && body.isUndef()) body = r;  // a plain object: JSON
  std::string type = ty.isUndef() ? "" : I.toStr(ty), text;
  if (body.isObj()) {
    text = I.toStr(I.call(I.get(I.getGlobal("JSON"), "stringify"), Value(), &body, 1));
    if (type.empty()) type = "application/json";
  } else if (!body.isUndef()) text = I.toStr(body);
  if (type.empty()) type = !text.empty() && text[0] == '<' ? "text/html; charset=utf-8" : "text/plain; charset=utf-8";
  webServer->send(st.isUndef() ? 200 : (int)I.toNum(st), type.c_str(), text.c_str());
}
static void webHandle() {
  Interp& I = *webInterp;
  String uri = webServer->uri();
  Prop* p = webRoutes->own(uri.c_str());
  if (!p || !I.isCallable(p->v)) p = webRoutes->own("*");
  if (!p || !I.isCallable(p->v)) { webServer->send(404, "text/plain", "not found"); return; }
  size_t mark = I.temps.size();
  Value fn = p->v;
  I.rootTemp(fn);
  Object* req = I.newObject();
  I.rootTemp(Value::obj(req));
  HTTPMethod m = webServer->method();
  I.setOwn(req, "method", I.str(m == HTTP_GET ? "GET" : m == HTTP_POST ? "POST" : m == HTTP_PUT ? "PUT" : m == HTTP_DELETE ? "DELETE" : "OTHER"));
  I.setOwn(req, "path", I.str(uri.c_str()));
  Object* q = I.newObject();
  for (int i = 0; i < webServer->args(); i++)
    if (webServer->argName(i) != "plain") I.setOwn(q, webServer->argName(i).c_str(), I.str(webServer->arg(i).c_str()));
  I.setOwn(req, "query", Value::obj(q));
  I.setOwn(req, "body", I.str(webServer->hasArg("plain") ? webServer->arg("plain").c_str() : ""));
  I.setOwn(req, "ip", I.str(webServer->client().remoteIP().toString().c_str()));
  Value result;
  if (I.callSafe(fn, Value(), {Value::obj(req)}, &result)) webRespond(I, result);
  else webServer->send(500, "text/plain", "the app's handler failed");
  I.temps.resize(mark);
}
static void webStop() {
  if (webServer) { webServer->stop(); delete webServer; webServer = nullptr; }
  if (webRoutes) webRoutes->props.clear();
}
JS_NATIVE(web_start) {
  (void)self;
  if (WiFi.status() != WL_CONNECTED && !(WiFi.getMode() & WIFI_AP)) I.throwError("Error", "web.start: connect WiFi (wifi.connect) or start an access point (wifi.ap) first");
  int port = argc ? (int)I.toNum(args[0]) : 80;
  if (webServer) { webServer->stop(); delete webServer; }
  webServer = new WebServer(port);
  webInterp = &I;
  webServer->onNotFound(webHandle);
  webServer->begin();
  return Value::boolean(true);
}
JS_NATIVE(web_on) {
  (void)self;
  if (!I.isCallable(JS_ARG(1))) I.throwError("TypeError", "web.on: handler must be a function");
  I.setOwn(webRoutes, I.toStr(JS_ARG(0)).c_str(), args[1]);
  return Value();
}
JS_NATIVE(web_off) {
  (void)self;
  if (argc == 0) webRoutes->props.clear();
  else I.delProp(Value::obj(webRoutes), I.toStr(args[0]));
  return Value();
}
JS_NATIVE(web_stop) { (void)I; (void)self; (void)args; (void)argc; webStop(); return Value(); }
JS_NATIVE(web_url) {
  (void)self; (void)args; (void)argc;
  if (!webServer) return I.str("");
  if (WiFi.status() == WL_CONNECTED) return I.str("http://" + std::string(WiFi.localIP().toString().c_str()) + "/");
  if (WiFi.getMode() & WIFI_AP) return I.str("http://" + std::string(WiFi.softAPIP().toString().c_str()) + "/");
  return I.str("");
}
#endif

static const NativeEntry LAZY_wifi[] = {{"connect", wifi_connect}, {"status", wifi_status}, {"ip", wifi_ip}, {"rssi", wifi_rssi}, {"disconnect", wifi_disconnect}, {"ap", wifi_ap}, {"scan", wifi_scan}, {"ntp", wifi_ntp}, {nullptr, nullptr}};
static const NativeEntry LAZY_http[] = {{"get", http_get}, {"post", http_post}, {nullptr, nullptr}};
#endif

void hwInstall(Interp& I) {
  // drawing methods shared by the screen and sprites
  Object* G = I.newObject();
  G->lazy = LAZY_G;
  struct { const char* n; uint32_t c; } colors[] = {
    {"BLACK", TFT_BLACK}, {"WHITE", TFT_WHITE}, {"RED", TFT_RED}, {"GREEN", TFT_GREEN}, {"BLUE", TFT_BLUE},
    {"YELLOW", TFT_YELLOW}, {"CYAN", TFT_CYAN}, {"MAGENTA", TFT_MAGENTA}, {"ORANGE", TFT_ORANGE},
    {"PINK", TFT_PINK}, {"PURPLE", TFT_PURPLE}, {"NAVY", TFT_NAVY}, {"MAROON", TFT_MAROON},
    {"OLIVE", TFT_OLIVE}, {"DARKGREEN", TFT_DARKGREEN}, {"DARKCYAN", TFT_DARKCYAN},
    {"LIGHTGREY", TFT_LIGHTGREY}, {"DARKGREY", TFT_DARKGREY}, {"GREENYELLOW", TFT_GREENYELLOW},
    {"BROWN", TFT_BROWN}, {"GOLD", TFT_GOLD}, {"SILVER", TFT_SILVER}, {"SKYBLUE", TFT_SKYBLUE},
    {"VIOLET", TFT_VIOLET}};
  for (auto& c : colors) I.setOwn(G, c.n, Value::num(c.c));
  const char* datums[] = {"TL", "TC", "TR", "ML", "MC", "MR", "BL", "BC", "BR"};
  for (int i = 0; i < 9; i++) I.setOwn(G, datums[i], Value::num(i));

  Object* lcd = I.newObject(G);
  lcd->lazy = LAZY_lcd;
  I.setGlobal("lcd", Value::obj(lcd));

  spriteProto = I.newObject(G);
  I.def(spriteProto, "push", sprite_push);
  I.def(spriteProto, "free", sprite_free);
  I.roots.push_back(Value::obj(spriteProto));

  touchHandlers = I.newObject();
  I.roots.push_back(Value::obj(touchHandlers));
  Object* touch = I.newObject();
  touch->lazy = LAZY_touch;
  I.setGlobal("touch", Value::obj(touch));

  I.lazyGlobals(LAZY_GLOBALS);
  I.setGlobal("INPUT", Value::num(INPUT));
  I.setGlobal("OUTPUT", Value::num(OUTPUT));
  I.setGlobal("INPUT_PULLUP", Value::num(INPUT_PULLUP));
  I.setGlobal("INPUT_PULLDOWN", Value::num(INPUT_PULLDOWN));
  I.setGlobal("HIGH", Value::num(HIGH));
  I.setGlobal("LOW", Value::num(LOW));
  Value date = I.getGlobal("Date");
  if (date.isObj()) I.def(date.o(), "now", g_dateNow);  // wall clock once wifi.ntp() succeeded

  keyHandlers = I.newObject();
  I.roots.push_back(Value::obj(keyHandlers));
  Object* keyboard = I.newObject();
  I.def(keyboard, "on", keyboard_on);
  I.def(keyboard, "off", keyboard_off);
  I.def(keyboard, "held", keyboard_held);
  I.def(keyboard, "capture", keyboard_capture);

  joyHandlers = I.newObject();
  I.roots.push_back(Value::obj(joyHandlers));
  Object* joystick = I.newObject();
  I.def(joystick, "read", joystick_read);
  I.def(joystick, "count", joystick_count);
  I.def(joystick, "on", joystick_on);
  I.def(joystick, "off", joystick_off);
  I.def(joystick, "keys", joystick_keys);
  I.setGlobal("joystick", Value::obj(joystick));
  Object* audio = I.newObject();
  I.def(audio, "play", audio_play);
  I.def(audio, "stop", audio_stop);
  I.def(audio, "pause", audio_pause);
  I.def(audio, "volume", audio_volume);
  I.def(audio, "status", audio_status);
  I.def(audio, "mute", audio_mute);
  I.setGlobal("audio", Value::obj(audio));
  Object* video = I.newObject();
  I.def(video, "play", video_play);
  I.def(video, "stop", video_stop);
  I.def(video, "pause", video_pause);
  I.def(video, "status", video_status);
  I.def(video, "dma", video_dma);
  I.setGlobal("video", Value::obj(video));
  I.setGlobal("keyboard", Value::obj(keyboard));

  Object* board = I.newObject();  // what this board has, so scripts can adapt
  I.setOwn(board, "name", I.str(BOARD_NAME));
  I.setOwn(board, "touch", I.str(BOARD_TOUCH == TOUCH_XPT2046 ? "resistive" : BOARD_TOUCH == TOUCH_FT6336 ? "capacitive" : "none"));
  I.setOwn(board, "hasTouch", Value::boolean(BOARD_TOUCH != TOUCH_NONE));
  I.setOwn(board, "hasKeyboard", Value::boolean(BOARD_KEYBOARD != KEYBOARD_NONE));
#ifdef BOARD_LED_BICOLOR_A
  I.setOwn(board, "hasLed", Value::boolean(true));
#else
  I.setOwn(board, "hasLed", Value::boolean(BOARD_LED_WS2812 >= 0 || BOARD_LED_R >= 0));
#endif
  {  // a unique id: the chip's MAC address (tools use it to tell boards apart)
    uint64_t mac = ESP.getEfuseMac();
    char id[18];
    snprintf(id, sizeof id, "%02X:%02X:%02X:%02X:%02X:%02X", (int)(mac & 0xFF), (int)(mac >> 8 & 0xFF), (int)(mac >> 16 & 0xFF),
             (int)(mac >> 24 & 0xFF), (int)(mac >> 32 & 0xFF), (int)(mac >> 40 & 0xFF));
    I.setOwn(board, "id", I.str(id));
  }
  I.setOwn(board, "lowMemory", Value::boolean(hwLowMemory()));  // too small for @needs-more-memory apps
  I.setOwn(board, "hasBattery", Value::boolean(BOARD_BATTERY_ADC >= 0));
  I.setOwn(board, "hasMusic", Value::boolean(HAS_MUSIC));
  I.setOwn(board, "hasSpeaker", Value::boolean(BOARD_SPEAKER_PWM >= 0 || BOARD_SPEAKER_I2S_DOUT >= 0));
  I.setOwn(board, "hasLight", Value::boolean(BOARD_LIGHT_SENSOR >= 0));
  I.setOwn(board, "hasSd", Value::boolean(sdOk));
  I.setOwn(board, "psram", Value::num(ESP.getPsramSize()));
  I.setGlobal("board", Value::obj(board));

  Object* sys = I.newObject();
  sys->lazy = LAZY_sys;
  I.setOwn(sys, "version", I.str(MICROJS_VERSION));
  I.setGlobal("sys", Value::obj(sys));

  Object* fsObj = I.newObject();
  fsObj->lazy = LAZY_fsObj;
  I.setGlobal("fs", Value::obj(fsObj));

#if JS_WIFI
  Object* wifi = I.newObject();
  wifi->lazy = LAZY_wifi;
  I.setGlobal("wifi", Value::obj(wifi));
  Object* http = I.newObject();
  http->lazy = LAZY_http;
  I.setGlobal("http", Value::obj(http));
  webRoutes = I.newObject();
  I.roots.push_back(Value::obj(webRoutes));
  Object* web = I.newObject();
  I.def(web, "start", web_start);
  I.def(web, "on", web_on);
  I.def(web, "off", web_off);
  I.def(web, "stop", web_stop);
  I.def(web, "url", web_url);
  I.setGlobal("web", Value::obj(web));
#endif
  I.temps.clear();
}
