# Watch OS & Flashing Agent Integration Guide (ESP32-S3-R8)

This document is written for the companion agent that controls the watch OS,
packaging, and flashing on the **Waveshare ESP32-S3-R8** smartwatch.

---

## 1. What TINY 2.1 Is on the Watch

TINY 2.1 is a 4-layer, 96-wide, 4-head causal transformer (`VOCAB = 97`
printable ASCII characters, `CTX = 128` characters) with **367,488 parameters**
stored as row-wise int8 weights (`scale[row]` in fp32 + `int8[cols]`).

The port splits the project into three ESP-IDF components under [`components/`](../components):

| Component | Role | Dependencies |
|---|---|---|
| [`tiny_engine`](../components/tiny_engine) | Pure inference engine + ESP32-S3 PIE SIMD kernel | `esp_timer`, `heap`, `freertos` |
| [`tiny_watch`](../components/tiny_watch) | FreeRTOS worker task, `esp_pm` frequency lock, partition/VFS/rodata loader, 16-bit PCM robot voice | `tiny_engine`, `esp_partition`, `spi_flash`, `esp_pm` |
| [`tiny_watch_ui`](../components/tiny_watch_ui) | Optional LVGL v8/v9 watch screen (compiled only if `lvgl` is in the build) | `tiny_watch`, `lvgl` (optional) |

---

## 2. Memory Budget on the ESP32-S3-R8

The ESP32-S3-R8 has **512 KB of internal SRAM** and **8 MB of Octal PSRAM**.
Every letter generated touches all 367 KB of weights, so memory placement
directly determines speed and whether the rest of the watch OS has enough
internal RAM for DMA, Wi-Fi/BLE, and LVGL draw buffers.

| Allocation | Size | Where it lives | Config knob |
|---|---|---|---|
| Weight tables (repacked, 16 B aligned) | **368 KB** total | First `N` KB in **internal SRAM**, rest in **Octal PSRAM** | `CONFIG_TINY_FAST_WEIGHT_BUDGET_KB` (default `128`) |
| Row scales & RMSNorm gains | **18 KB** | Internal SRAM | automatic |
| KV cache (4 layers x 128 ctx x 96 int8 x 2 + scales) | **102 KB** | Internal SRAM if above reserve, else PSRAM | `CONFIG_TINY_FAST_RESERVE_KB` (default `72`) |
| Block prefill scratch (`B = 16` tokens) | **51 KB** | Internal SRAM (always) | `CONFIG_TINY_PREFILL_BLOCK` (default `16`) |
| Worker task stack | **6 KB** | Internal SRAM | `tiny_watch_cfg_t.worker_stack` |

**Hard rule enforced by the allocator:** the engine will never allocate from
`MALLOC_CAP_INTERNAL` if doing so would leave fewer than
`CONFIG_TINY_FAST_RESERVE_KB` (default **72 KB**) of internal 8-bit RAM free
for the OS, DMA descriptors, and ISR stacks. It falls back to `MALLOC_CAP_SPIRAM`
per table (~9 KB - 24 KB each, so heap fragmentation does not hurt it).

### Recommended settings depending on what else the watch OS runs

* **BLE + LVGL watch face (no Wi-Fi):** `CONFIG_TINY_FAST_WEIGHT_BUDGET_KB=160`,
  `CONFIG_TINY_FAST_RESERVE_KB=72`. Keeps `emb`, all 16 attention matrices
  (`wq/wk/wv/wo`), and the scratch buffers in internal SRAM; only the FFN
  tables (`f1/f2`) stream from Octal PSRAM.
* **Wi-Fi + BLE + LVGL simultaneously active:** `CONFIG_TINY_FAST_WEIGHT_BUDGET_KB=48`,
  `CONFIG_TINY_FAST_RESERVE_KB=96`. `emb` + scratch stay in internal SRAM;
  everything else streams from PSRAM through the 32 KB / 64 B data cache.

---

## 3. OS-Side Optimizations (sdkconfig / RTOS / Hardware)

These settings are already present in [`sdkconfig.defaults`](../sdkconfig.defaults)
for standalone builds. **Please merge them into the watch OS `sdkconfig`:**

### 3.1 Octal PSRAM & Cache (Biggest Impact: ~3x Speedup)

The `-R8` package uses an **Octal** (8-line) SPI PSRAM. Running it in 40 MHz
or Quad mode starves the SIMD unit during weight streaming.

```ini
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y
CONFIG_ESP32S3_DATA_CACHE_32KB=y
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
```

Why `64B` cache lines matter: every weight row is either 96 bytes (`D = 96`) or
256 bytes (`HID = 256`), 16-byte aligned and read sequentially. A 64-byte line
halves the number of PSRAM transactions compared to the 32-byte default.

With `SPIRAM_FETCH_INSTRUCTIONS=y` and `SPIRAM_RODATA=y`, the S3 relocates
flash `.text`/`.rodata` into PSRAM at boot so the single MSPI bus does not have
to switch chip-selects between flash and PSRAM while TINY is running:

```ini
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
```

### 3.2 Dual-Core Task Pinning (Zero UI Jank)

| Core | What should run there | Priority |
|---|---|---|
| **Core 0 (`PRO_CPU`)** | Wi-Fi / BLE controller, LVGL tick & redraw (`lv_timer_handler`), touch/IMU/RTC I2C, I2S DMA | `5..22` |
| **Core 1 (`APP_CPU`)** | `tiny_ai` worker task (default in `tiny_watch_cfg_default()`) | `4` |

Because `tiny_ai` is pinned to Core 1 at priority 4 and yields every 250 ms via
`tiny_port_yield()`:

* Core 0's 60 Hz LVGL redraw and touch polling are **never preempted** by
  matrix multiplications.
* Disable the idle-task watchdog on Core 1 (or keep `tiny_port_yield()` as is):
  ```ini
  CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n
  ```

### 3.3 Dynamic Frequency Scaling & Battery Life

Do **not** lock the watch at 240 MHz permanently. Enable DFS + light sleep in
the OS:

```ini
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
```

Configure the OS power manager with `max_freq_mhz = 240`, `min_freq_mhz = 40`
(or `80`). `tiny_watch` automatically creates an `ESP_PM_CPU_FREQ_MAX` lock and
acquires it **only between the start of a question and the last letter of the
answer** (~0.15 - 0.4 s). When TINY is idle, it consumes **0 uA** of CPU budget.

### 3.4 Do Not Use `PIE` / `ACCX` from ISRs on Core 1

The ESP32-S3's PIE vector registers (`q0..q7`, `ACCX`) are **lazily saved** by
FreeRTOS between tasks, **not** on interrupt entry. Do not call `esp-dsp` /
PIE instructions inside an ISR routed to Core 1.

---

## 4. Packaging & Flashing `TINY.BIN`

`tiny_watch_start()` looks for the 382,100-byte `TINY.BIN` in four places, in
order:

1. `cfg->blob` / `cfg->blob_len` (caller-supplied pointer).
2. **Raw flash partition** labeled `"tiny_model"` (default, **recommended for
   OTA**): `esp_partition_mmap()` maps the partition during `tiny_watch_start()`,
   repacks the int8 tables into SRAM/PSRAM, and immediately unmaps it (zero
   permanent MMU pages used).
3. **Filesystem file** `/storage/TINY.BIN` (LittleFS / SPIFFS / SD).
4. **Embedded `.rodata`** when `CONFIG_TINY_EMBED_MODEL=y` (default for
   single-binary flashing).

### Recommended Partition Layout for the Flashing Agent

```csv
# Name,       Type, SubType, Offset,   Size
tiny_model,   data, 0x40,    0x290000, 0x70000
```

* Flash `model/TINY.BIN` once:
  ```bash
  esptool.py --chip esp32s3 write_flash 0x290000 model/TINY.BIN
  ```
* Set `CONFIG_TINY_EMBED_MODEL=n` in the watch OS build. Firmware updates then
  shrink by **382 KB** and do not re-flash the weights.

---

## 5. Embedding TINY into the Watch OS (Code)

### 5.1 Headless / Custom UI

```c
#include "tiny_watch.h"

static bool i2s_push(void *user, const int16_t *pcm, size_t n, int sr)
{
    size_t written = 0;
    /* Waveshare S3 watches typically use an ES8311 codec on I2S0 */
    i2s_channel_write(my_i2s_tx_chan, pcm, n * sizeof(int16_t), &written, pdMS_TO_TICKS(100));
    return true;
}

void watch_ai_init(void)
{
    tiny_watch_cfg_t cfg = tiny_watch_cfg_default();
    cfg.on_pcm = i2s_push;          /* NULL disables the robot voice */
    cfg.sample_rate_hz = 16000;
    tiny_watch_start(&cfg, &g_tiny);
}
```

Ask a question without blocking the UI task:

```c
tiny_watch_req_t req = {
    .question   = "what is the capital of peru",
    .creativity = 0,                /* 0 = deterministic quiz mode, 5 = chat */
    .kernel     = TINY_KERNEL_PRECISE,
    .on_token   = my_token_cb,      /* called per letter with confidence + layer activity */
    .on_done    = my_done_cb,
    .user       = my_ctx,
};
tiny_watch_ask_async(g_tiny, &req);
```

### 5.2 Ready-Made LVGL Watch Screen

If the watch OS uses LVGL (v8 or v9), one call builds a 360x360 round-bezel-aware
screen with the 4-layer neural activity bars, confidence badge, streaming
answer, preset chips, and pop-up keyboard:

```c
#include "tiny_watch_ui.h"

lv_obj_t *ai_tile = tiny_watch_ui_create(parent_tile_or_null, g_tiny);
/* Hook a hardware button or speech-to-text result to: */
tiny_watch_ui_ask("tell me a joke");
```
