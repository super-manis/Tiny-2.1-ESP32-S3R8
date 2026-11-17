# TINY 2.1 — Local AI for the Waveshare ESP32-S3-R8 Smartwatch

Originally a 367,000-weight question-answering transformer written in Turbo C 2.0
for DOS ([`legacy/dos/`](legacy/dos)), **TINY 2.1** is adapted here into a native
ESP-IDF firmware & component suite for the **Waveshare ESP32-S3-R8** smartwatch
(dual-core Xtensa LX7 @ 240 MHz + 8 MB Octal PSRAM).

---

## Repository Layout

| Path | Description |
|---|---|
| [`components/tiny_engine`](components/tiny_engine) | Platform-neutral transformer engine + hand-written ESP32-S3 PIE SIMD int8 dot-product kernel (`EE.VMULAS.S8.ACCX`, 16 MACs/instruction), batched prompt prefill, and SRAM/PSRAM split allocator |
| [`components/tiny_watch`](components/tiny_watch) | FreeRTOS worker service (pinned to Core 1), `esp_pm` frequency lock, partition / VFS / embedded weight loader, and 16-bit PCM synthesiser for the classic TINY robot voice |
| [`components/tiny_watch_ui`](components/tiny_watch_ui) | Optional LVGL (v8/v9) round-bezel watch screen with live 4-layer neural activity bars, confidence meter, preset chips, and touch keyboard |
| [`main/main.c`](main/main.c) | Standalone ESP-IDF entry point (boot self-test, throughput benchmark, and interactive USB-JTAG / UART shell) |
| [`model/TINY.BIN`](model/TINY.BIN) | The trained 4-layer, 96-wide int8 network (`TNY2`, 382,100 bytes) |
| [`host/`](host) | Linux host build & 216-question regression suite against the unchanged DOS reference |
| [`docs/OS_OPTIMIZATION_GUIDE.md`](docs/OS_OPTIMIZATION_GUIDE.md) | Watch OS `sdkconfig`, memory budget, dual-core scheduling, and partition/OTA instructions for the companion flashing agent |
| [`legacy/dos/`](legacy/dos) | Original DOS / Turbo C 2.0 sources (`TINY.C`, `TINY.EXE`, `BUILD.BAT`) |

---

## Key Optimizations for the ESP32-S3-R8

1. **ESP32-S3 PIE SIMD (`tiny_kernels_s3.S`)** — Weight rows are repacked at load
   time to 16-byte aligned strides with row scales separated. The inner loop runs
   `EE.VMULAS.S8.ACCX.LD.IP` (16 int8 MACs + 128-bit load per cycle into the
   40-bit accumulator) without reading past row boundaries.
2. **Residual-int8 activation quantisation (`TINY_KERNEL_PRECISE`)** — Each
   activation vector is decomposed into an int8 primary vector plus an int8
   residual (`~15 bits` effective precision), both executed through the PIE
   int8 SIMD kernel. Matches the original DOS floating-point answers on
   **215 / 216 (99.5%)** of the regression suite while keeping all matrix
   multiplies in int8 SIMD. A single-pass `TINY_KERNEL_FAST` and a bit-exact
   `TINY_KERNEL_FLOAT` are also available at run time.
3. **Batched prompt prefill (`TINY_PREFILL_BLOCK = 16`)** — Instead of streaming
   all 367 KB of weights from PSRAM once per question letter, the prompt is
   pushed through in blocks of 16 letters so each weight row is fetched once per
   block and stays in L1 data cache across the token loop.
4. **Budgeted SRAM / Octal-PSRAM table placement** — Hot tables (`emb`, scales,
   scratch, and as many layer matrices as fit) are placed in internal SRAM while
   guaranteed to leave `CONFIG_TINY_FAST_RESERVE_KB` (default 72 KB) of internal
   heap free for the watch OS, DMA, and wireless stacks; remaining tables live
   in Octal PSRAM.
5. **Single-precision & table-free `fast_exp()`** — Eliminates all software-emulated
   `double` operations (`sqrt()`, `exp()`) present in the DOS code that otherwise
   stall the S3's single-precision FPU.
6. **Non-blocking dual-core execution** — Inference runs on Core 1 at lower
   priority than LVGL/audio, acquires `ESP_PM_CPU_FREQ_MAX` only while answering,
   and streams tokens + layer telemetry to the UI locklessly.

---

## Build & Verify

### 1. ESP-IDF Firmware (ESP32-S3-R8)

```bash
idf.py set-target esp32s3
idf.py build
```

To integrate into an existing watch OS project instead, add `components/tiny_engine`,
`components/tiny_watch`, and (optionally) `components/tiny_watch_ui` to
`EXTRA_COMPONENT_DIRS` and follow [`docs/OS_OPTIMIZATION_GUIDE.md`](docs/OS_OPTIMIZATION_GUIDE.md).

### 2. Host Regression Test (vs. the Original DOS Program)

```bash
make -C host test
```
