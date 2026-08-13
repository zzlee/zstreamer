# videotestsrc & audiotestsrc 配置說明指南 (Test Sources Guide)

`zstreamer` 提供兩個核心測試訊號源 Element：`videotestsrc`（影像測試訊號源）與 `audiotestsrc`（音訊測試訊號源）。本指南詳細說明其圖案 (pattern)、波形 (wave)、像素/採樣格式、屬性設定及 C API 使用範例。

---

## 1. `videotestsrc` (影像測試訊號源)

`videotestsrc` 用於產生合成影像測試圖案，支援多種色彩圖案、解析度、影格率及像素格式。

### 1.1 可用圖案 (Patterns)

| Pattern 名稱 | 描述 | 範例與用途 |
|---|---|---|
| `bars` **(預設)** | 經典色彩條紋圖案 (EBU/SMPTE Color Bars) | 校正螢幕顏色、測試編碼/解碼色彩失真 |
| `gradient` | 動態水平漸層圖案 | 測試動態畫面漸層、視訊量化與位元率適應力 |
| `checkerboard` | 黑白高對比棋盤格圖案 | 測試邊緣清晰度、縮放與交錯/去交錯演算法 |
| `noise` | 動態雪花隨機雜訊 | 高複雜度動態測試，評估編碼器最大碼率壓力 |
| `black` | 全黑靜止畫面 | 測試畫面開頭/結尾、空白影格填補 |

### 1.2 支援像素格式 (Pixel Formats)

- `YUV420P` (或 `I420`, `YUV420`) — 平面 YUV 4:2:0 **(預設)**
- `NV12` — 雙平面 YUV 4:2:0 (Y 平面 + UV 交錯)
- `YUYV422` (或 `YUY2`, `YUYV`) — 打包 YUV 4:2:2
- `RGB24` (或 `RGB`) — 打包 24-bit RGB
- `BGR24` (或 `BGR`) — 打包 24-bit BGR
- `RGBA` — 32-bit RGBA (帶 Alpha 聲道)
- `BGRA` — 32-bit BGRA
- `ARGB` — 32-bit ARGB
- `ABGR` — 32-bit ABGR

### 1.3 核心屬性 (Properties)

| 屬性名稱 | 型態 | 預設值 | 說明 |
|---|---|---|---|
| `width` | UINT | `640` | 畫面寬度 (像素) |
| `height` | UINT | `480` | 畫面高度 (像素) |
| `fps` | UINT | `30` | 影格率 (FPS) |
| `pattern` | STRING | `"bars"` | 測試圖案 (`bars`, `gradient`, `checkerboard`, `noise`, `black`) |
| `pixel-format` | STRING | `"YUV420P"` | 像素格式 |
| `num-frames` | INT | `-1` | 輸出影格總數 (`-1` 代表無限循環) |
| `use-clock` | BOOL | `false` | 是否使用 Pipeline Clock 填寫 PTS 時間戳記 |
| `real-time-pacing`| BOOL | `false` | 是否按真實時間節奏 (FPS) 暫停控速 |

---

## 2. `audiotestsrc` (音訊測試訊號源)

`audiotestsrc` 用於產生合成音訊測試波形，支援單聲道/雙聲道/多聲道、獨立頻率、音量控制及多種採樣格式。

### 2.1 可用波形 (Waves)

| Wave 名稱 | 描述 | 聲道行為 / 用途 |
|---|---|---|
| `sine` **(預設)** | 單頻率純正弦波 (Sine Wave) | 所有聲道輸出 `frequency` 指定之音調 (如 440Hz / 1kHz) |
| `stereo-tone` | 雙聲道獨立辨識音調 (Stereo Tone) | **左聲道 1,000 Hz (1kHz)**，**右聲道 440 Hz**（用於分辨左右聲道） |
| `square` | 方波 (Square Wave) | 所有聲道輸出高諧波方波音調 |
| `white-noise` | 白雜訊 (White Noise) | 所有聲道輸出均勻分佈隨機白雜訊（測試頻響） |
| `pink-noise` | 粉紅雜訊 (Pink Noise) | 所有聲道輸出 Kellet 濾波粉紅雜訊（符合人耳聽感） |
| `silence` | 全靜音 (Silence) | 所有聲道輸出數值 0（靜音訊號） |

> **提示：** `stereo-tone` 可寫為 `stereo`, `stereo_tone`, `stereo-1k-440`。

### 2.2 支援採樣格式 (Sample Formats)

- `S16LE` (或 `S16`) — 16-bit 簽名整數 (Interleaved) **(預設)**
- `S32LE` (或 `S32`) — 32-bit 簽名整數 (Interleaved)
- `F32LE` (或 `F32`) — 32-bit 單精度浮點數 (Interleaved)
- `U8` — 8-bit 無簽名整數 (Interleaved)
- `S16P` — 16-bit 簽名整數 (Planar 平面)
- `S32P` — 32-bit 簽名整數 (Planar 平面)
- `F32P` (或 `FLTP`) — 32-bit 浮點數 (Planar 平面)

### 2.3 核心屬性 (Properties)

| 屬性名稱 | 型態 | 預設值 | 說明 |
|---|---|---|---|
| `sample-rate` | UINT | `44100` | 採樣率 (Hz，如 `48000`, `44100`) |
| `channels` | UINT | `2` | 聲道數 (1 = 單聲道, 2 = 雙聲道) |
| `sample-format` | STRING | `"S16LE"` | 採樣格式 |
| `wave` | STRING | `"sine"` | 波形種類 (`sine`, `stereo-tone`, `square`, `white-noise`, `pink-noise`, `silence`) |
| `frequency` | DOUBLE | `440.0` | 音調頻率 (Hz) |
| `volume` | DOUBLE | `0.8` | 音量大小 (`0.0` ~ `1.0`) |
| `samples-per-buffer` | UINT | `1024` | 每個 Buffer 包含的 Samples 數量 |
| `num-samples` | INT | `-1` | 輸出 Samples 總數 (`-1` 無限) |
| `num-buffers` | INT | `-1` | 輸出 Buffer 總數 (`-1` 無限) |
| `use-clock` | BOOL | `false` | 是否使用 Pipeline Clock 填寫 PTS 時間戳記 |
| `real-time-pacing`| BOOL | `false` | 是否按真實音訊時間節奏控速 |

---

## 3. C API 配置使用範例

### 範例 1：使用 Element Factory 通用屬性 API
```c
/* 建立 videotestsrc 視訊源: 1080p 30FPS 彩條圖案 */
zst_element_t* vsrc = zst_element_factory_make("videotestsrc");
zst_element_set_property_uint(vsrc, "width", 1920);
zst_element_set_property_uint(vsrc, "height", 1080);
zst_element_set_property_uint(vsrc, "fps", 30);
zst_element_set_property_string(vsrc, "pattern", "bars");        /* 或 "gradient", "checkerboard", "noise" */
zst_element_set_property_string(vsrc, "pixel-format", "YUV420P");

/* 建立 audiotestsrc 音訊源: 48kHz 雙聲道 左右聲道獨立音調 */
zst_element_t* asrc = zst_element_factory_make("audiotestsrc");
zst_element_set_property_uint(asrc, "sample-rate", 48000);
zst_element_set_property_uint(asrc, "channels", 2);
zst_element_set_property_string(asrc, "sample-format", "S16LE");
zst_element_set_property_string(asrc, "wave", "stereo-tone");   /* 左聲道 1kHz, 右聲道 440Hz */
zst_element_set_property_double(asrc, "volume", 0.8);
```

### 範例 2：使用結構體 Struct Config API
```c
#include "zstreamer/elements/zst_video_test_src.h"
#include "zstreamer/elements/zst_audio_test_src.h"

/* 使用與類別同名的專用 Config 建立 videotestsrc */
zst_video_test_src_config_t vcfg = {
    .struct_size = sizeof(zst_video_test_src_config_t),
    .width = 1920,
    .height = 1080,
    .fps = 30,
    .pattern = "bars",
    .pixel_format = "YUV420P"
};
zst_element_t* vsrc = zst_video_test_src_create_with_config(&vcfg);

/* 使用與類別同名的專用 Config 建立 audiotestsrc */
zst_audio_test_src_config_t acfg = {
    .struct_size = sizeof(zst_audio_test_src_config_t),
    .sample_rate = 48000,
    .channels = 2,
    .sample_format = "S16LE",
    .wave = "stereo-tone",
    .volume = 0.8
};
zst_element_t* asrc = zst_audio_test_src_create_with_config(&acfg);
```
