# API Design Notes — 函數設計超出名稱/參數推導範圍之紀錄

> 本檔記錄 `zstreamer` 中那些無法僅憑函數名稱與簽名推導出實際用法的 API，
> 並標註未來修正目標（補註解 / 重命名 / 改簽名）。
> 產生來源：2026-09-03 對 `llms.txt`、`llms-full.txt` 及 `include/*.h` 的交叉比對。

---

## 總覽

| # | 函數 | 類別 | 主要問題 | 修正目標 |
|---|------|------|---------|---------|
| 1 | `zst_pad_add_probe()` | Pad | 傳回值為 handle ID，非 bool | `TODO-1` |
| 2 | `zst_pad_push_sticky_events()` | Pad | 「sticky」語義不可推導 | `TODO-2` |
| 3 | `zst_pad_set_unlinked_policy()` | Pad | policy 行為非顯見 | `TODO-3` |
| 4 | `zst_clock_wait()` | Clock | duration 而非 absolute time | `TODO-4` |
| 5 | `zst_bus_pop()` | Bus | timeout_ms 非阻塞與超時傳回值契約 | `TODO-5` |
| 6 | `zst_pipeline_update_ranks_from()` | Pipeline | 「ranks」概念隱藏 | `TODO-6` |
| 7 | `zst_buffer_pool_config_from_caps()` | Pool | heuristic 映射不可推導 | `TODO-7` |
| 8 | `zst_stream_info_clear()` | Stream | owned 字段需手動釋放 | `TODO-8` |
| 9 | `zst_pad_get_peer()` | Pad | ownership transfer 非顯見 | `TODO-9` |
| 10 | `zst_caps_intersect()` | Caps | 傳回新建物件需 free | `TODO-10` |
| 11 | `zst_pipeline_reconfigure_begin/end()` | Pipeline | 交易邊界非顯見 | `TODO-11` |
| 12 | `zst_pipeline_link_pads_dynamic()` | Pipeline | 與 `zst_pad_link` 差異不顯 | `TODO-12` |
| 13 | `zst_gl_comp_sink_capture()` | GLSink | GL read-pixel 非顯見 | `TODO-13` |
| 14 | `zst_webrtc_filter_sdp()` | WebRTC | 兩函數行為差異非顯見 | `TODO-14` |
| 15 | `zst_webrtc_select_codecs()` | WebRTC | 雙輸出 + 需 free 字串 | `TODO-15` |
| 16 | `zst_dante_video_coordinator_apply_flow()` | Dante | flow 概念為領域知識 | `TODO-16` |
| 17 | `zst_pad_block()` / `set_block_callback` | Pad | callback-driven unblock 非顯見 | `TODO-17` |
| 18 | `zst_queue_element_set_pool()` | Queue | pool 綁定位置非典型 | `TODO-18` |
| 19 | `zst_pad_push_event_upstream()` | Pad | 僅支援 SINK pad，傳入 SRC pad 直接報錯 | `TODO-19` |
| 20 | `zst_element_snapshot_src_pads()` | Element | double-indirection 分配 | `TODO-20` |
| 21 | `zst_buffer_t` / `zst_element_t` / `zst_pad_t` / `zst_pipeline_t` | Core | 結構體完全開放（非 opaque），非預期外洩內部欄位 | `TODO-21` |
| 22 | `zst_scheduler_run()` | Scheduler | 阻塞式執行，名稱看不出是非返回的長跑函數 | `TODO-22` |
| 23 | `zst_bus_set_handler()` vs `zst_bus_pop()` | Bus | 兩者皆可消費事件，但互斥行為非顯見 | `TODO-23` |
| 24 | `zst_pipeline_start()` / `stop()` vs `zst_scheduler_run()` | Pipeline | 兩者皆觸發播放，但作用層級不同（pipeline state vs scheduler loop） | `TODO-24` |
| 25 | `zst_scheduler_attach()` 無對應 `detach()` | Scheduler | detach 需 destroy scheduler，無明確分離點 | `TODO-25` |
| 26 | `zst_clock_wait()` 為 void | Clock | 無法得知等待是否因訊號中斷而提前返回 | `TODO-26` |
| 27 | `zst_element_ops_t` 在 header 中完全暴露 | Element | 開發者需實作所有欄位，optional callbacks 預設行為不明 | `TODO-27` |
| 28 | `zst_event_t` union 極大且無枚舉判別欄位 | Bus | 需靠 `type` 手動查閱 union 對應欄位，編譯器無法協助 | `TODO-28` |
| 29 | `zst_pad_get_caps()` | Pad | 傳回新建 copy 物件需手動 destroy，非內部借用指標 | `TODO-29` |
| 30 | `zst_pad_push()` | Pad | 僅借用 buffer，不轉移所有權（與 GStreamer 語意相反） | `TODO-30` |
| 31 | `zst_element_create()` | Element | `priv` 隱式要求必須由 malloc 分配，destroy 會 free(priv) | `TODO-31` |
| 32 | `zst_caps_append()` | Caps | 隱式轉移 `caps_struct` 所有權至 caps | `TODO-32` |
| 33 | `zst_bus_set_handler()` vs `zst_bus_pop()` | Bus | 非對稱 Event 銷毀責任（handler 自動銷毀 vs pop 需手動 destroy） | `TODO-33` |
| 34 | `zst_pipeline_destroy()` / `bin_destroy()` | Pipeline | 遞迴銷毀內部所有 Element 與 Bus（排他性所有權） | `TODO-34` |
| 35 | `zst_pad_push_event_upstream()` | Pad | 僅支援 SINK pad，傳入 SRC pad 直接回傳 ZST_ERROR | `TODO-35` |
| 36 | `zst_allocator_dmabuf_get_fd()` | Allocator | 回傳借用 fd，呼叫端絕對不可 close() | `TODO-36` |
| 37 | `zst_buffer_create()` | Buffer | 僅分配 wrapper 結構體，不分配 memory payload | `TODO-37` |
| 38 | `ZST_LOG_LEVEL` (Ceiling) | Log | Release 模式編譯期上限截斷，執行期調高 level 無效 | `TODO-38` |


---

## 詳細紀錄

### 1. `zst_pad_add_probe()` — 傳回值為 handle ID

```c
uint64_t zst_pad_add_probe(zst_pad_t* pad, uint32_t types,
                           zst_pad_probe_fn callback, void* user_data);
```

**預期：** 大多數 C 函數傳回 `zst_result_t`（成功/失敗）。  
**實際：** 傳回 `uint64_t` probe ID，用於後續 `zst_pad_remove_probe(pad, id)`。  
**原因：** 同一 pad 可掛多個 probe，需 handle 來區分。  
**FIXME:** 在標題列補上「returns probe handle」說明；或考慮改為傳入已分配的 id 指標。

---

### 2. `zst_pad_push_sticky_events()` — sticky 語義

```c
zst_result_t zst_pad_push_sticky_events(zst_pad_t* pad);
```

**預期：** 單純將某個 event push 出去。  
**實際：** 在 pad 被 link 時，自動重播該 pad 上最後一次 set 過的
`STREAM_START` / `CAPS` / `SEGMENT` 事件（即 sticky event 機制）。  
**原因：** GStreamer 移植概念，需在 header 明確說明触发時機。  
**FIXME:** 在 `zst_pad.h` 的函數註解中說明 sticky event 的概念與觸發時機。

---

### 3. `zst_pad_set_unlinked_policy()` — policy 行為

```c
zst_result_t zst_pad_set_unlinked_policy(zst_pad_t* pad,
                                         zst_pad_unlinked_policy_t policy,
                                         uint32_t max_queued_buffers);
```

**預期：** 設定某個策略。  
**實際：** 控制當 buffer push 到一個沒有 peer 的 pad 時的行為
（ERROR / DROP / BLOCK / QUEUE）。這是為動態 demuxer 設計的隱藏行為。  
**FIXME:** 補充每個 policy 的詳細行為說明；考慮將 enum 名稱改得更具描述性（如 `ZST_PAD_UNLINKED_DROP_NEW`）。

---

### 4. `zst_clock_wait()` — 參數是 duration，非絕對時間

```c
void zst_clock_wait(zst_clock_t* clock, zst_time_t time);
```

**預期：** `time` 是「等到哪個時間點」。  
**實際：** `time` 是「等待多久」（相對 duration，單位 nanoseconds）。  
**FIXME:** 重命名為 `zst_clock_wait_duration()`，或在註解中明確標示 `_duration_ns`。

---

### 5. `zst_bus_pop()` — timeout_ms 非阻塞與超時傳回值契約

```c
zst_result_t zst_bus_pop(zst_bus_t* bus, zst_event_t** event, uint32_t timeout_ms);
```

**預期：** 常見誤解認為 `timeout_ms=0` 且佇列無事件時會返回 `ZST_OK` 且 `*event = NULL`。  
**實際：** 當佇列為空且 `timeout_ms=0` 時，函數立即返回 `ZST_TIMEOUT`（非 `ZST_OK`）。若有事件則返回 `ZST_OK` 且 `*event` 保證非 NULL。若處於 flushing 狀態則返回 `ZST_ERROR`。  
**FIXME:** 在 header 註解明確說明傳回值契約：`ZST_OK`（取得事件）、`ZST_TIMEOUT`（超時或 non-blocking 無事件）、`ZST_ERROR`（flushing 或無效參數）。

---

### 6. `zst_pipeline_update_ranks_from()` — 圖 rank 重算

```c
void zst_pipeline_update_ranks_from(zst_pipeline_t* pipe, zst_element_t* start_el);
```

**預期：** 更新某個元素的状态。  
**實際：** 對 pipeline 執行圖的拓撲排序的「增量」重算——只重新計算
從 `start_el` 開始往下游的 ranks，而非全圖重排。  
**原因：** 用於動態圖修改後避免 O(N²) 的全圖排序。  
**FIXME:** 在 header 中補充「incremental topological rank update」說明。

---

### 7. `zst_buffer_pool_config_from_caps()` — heuristic 映射

```c
zst_buffer_pool_config_t zst_buffer_pool_config_from_caps(const zst_caps_t* caps);
```

**預期：** 從 caps 取某個固定值。  
**實際：** 根據 media type / resolution / format 等 heuristic 推導出
`min_buffers`、`max_buffers`、`buffer_size`。  
**FIXME:** 在註解中列出映射規則（或連結到 wiki），避免呼叫端誤以為是固定公式。

---

### 8. `zst_stream_info_clear()` — 需手動釋放 owned 字段

```c
void zst_stream_info_clear(zst_stream_info_t* info);
```

**預期：** 清除結構體的內部狀態。  
**實際：** 必須調用來釋放 `info->name`、`info->language`、`info->caps` 三個
**由 API 擁有**的字段，否則發生 memory leak。  
**FIXME:** 在註解中強調「API-owned fields must be cleared」，並在 `zst_stream.h` 頂部加 ownership 說明段落。

---

### 9. `zst_pad_get_peer()` — ownership transfer

```c
zst_pad_t* zst_pad_get_peer(zst_pad_t* pad);
```

**預期：** 取得 peer pad 的 pointer（不轉移 ownership）。  
**實際：** 傳回值是一份 **reference**（refcount +1），呼叫端必須自行調用
`zst_pad_unref()`。  
**FIXME:** 重命名為 `zst_pad_get_peer_ref()`，或在註解中明確標註「+1 ref」。

---

### 10. `zst_caps_intersect()` — 傳回新建物件

```c
zst_caps_t* zst_caps_intersect(const zst_caps_t* caps1, const zst_caps_t* caps2);
```

**預期：** 傳回 intersection 結果，可能是 void 或 status。  
**實際：** 傳回新建的 `zst_caps_t*`，呼叫端負責 `zst_caps_destroy()`。  
**FIXME:** 在 header 中註明「caller owns the returned pointer」；或考慮改為
`zst_caps_intersect_into(dst, src1, src2)` 避免隱式分配。

---

### 11. `zst_pipeline_reconfigure_begin()` / `end()` — 交易邊界

```c
zst_result_t zst_pipeline_reconfigure_begin(zst_pipeline_t* pipe);
zst_result_t zst_pipeline_reconfigure_end(zst_pipeline_t* pipe);
```

**預期：** 開始/結束某項操作。  
**實際：** 建立一個「原子圖修改交易」邊界。在此範圍內允許
`pipeline_add/remove`、`link_pads_dynamic/unlink_pads_dynamic`，
確保狀態傳播不會中途發生。  
**FIXME:** 改名為 `zst_pipeline_txn_begin()` / `zst_pipeline_txn_commit()` 或
`zst_pipeline_txn_abort()`，讓「交易」語義更明確。

---

### 12. `zst_pipeline_link_pads_dynamic()` vs `zst_pad_link()`

```c
zst_result_t zst_pipeline_link_pads_dynamic(zst_pipeline_t* pipe,
                                            zst_pad_t* src, zst_pad_t* sink);
```

**預期：** `zst_pad_link()` 的多一個 pipe 參數版本。  
**實際：** 在持有 `elements_lock` 的情況下執行鏈接，包含狀態傳播的原子性。
`zst_pad_link()` 不持有 pipeline lock，不能用於動態重配置期間。  
**FIXME:** 在 `zst_pad_link()` 註解中加入「not safe during reconfiguration」警告。

---

### 13. `zst_gl_comp_sink_capture()` — GL read-pixel

```c
zst_result_t zst_gl_comp_sink_capture(zst_element_t* el,
                                       uint32_t width, uint32_t height,
                                       uint8_t* rgba_out);
```

**預期：** 某個簡單的 capture 動作。  
**實際：** 執行同步的 OpenGL `glReadPixels`，將 compositor 的當前畫面
讀入 caller 提供的 RGBA buffer。若處於 null-mode（無 GL context）則回
`ZST_ERROR`。  
**FIXME:** 在 header 中說明此為同步 GL 讀取，可能引起渲染 stall。

---

### 14. `zst_webrtc_filter_sdp()` vs `zst_webrtc_compat_local_sdp()`

```c
char* zst_webrtc_filter_sdp(const char* sdp);
char* zst_webrtc_compat_local_sdp(const char* sdp);
```

**預期：** 兩者皆為 SDP 處理函數。  
**實際：**
- `filter_sdp`：移除不支援的 RTP header extensions 與 attributes。
- `compat_local_sdp`：補強 Chrome/Firefox 相容性（額外處理、修正）。
兩者皆傳回需 `free()` 的新字串。  
**FIXME:** 考慮合併或統一命名空間（如 `zst_webrtc_sdp_{filter,compat}`）。

---

### 15. `zst_webrtc_select_codecs()` — 多輸出 + 傳回值雙重契約

```c
char* zst_webrtc_select_codecs(const char* sdp, const char* preference,
                               char* selected_video_out, size_t video_out_len,
                               char* selected_audio_out, size_t audio_out_len);
```

**預期：** 單一字串輸入/輸出。  
**實際：**
1. 修改 SDP（移除未選編碼器），傳回新 SDP 字串（需 free）。
2. 同時把選擇的 video/audio 編碼器名稱寫入 caller 提供的 buffer。
**FIXME:** 考慮改用 struct 輸出參數來統一契約。

---

### 16. `zst_dante_video_coordinator_apply_flow()` — 領域知識

```c
zst_result_t zst_dante_video_coordinator_apply_flow(
    zst_element_t* coordinator, const zst_dante_flow_t* flow);
```

**預期：** 套用一个 flow。  
**實際：** `zst_dante_flow_t` 是一個包含網路位址、port、channel index、
multicast address 等的複雜結構。此函數在 coordinator 內部動態建立/路由
RTP 流（對應 Dante DVR 協議中的 flow）。需要理解 Dante 領域模型才能使用。  
**FIXME:** 在 header 中加入 Dante flow 的簡短說明段落，連結到 `DANTE_CONTROL_AUDIO_VIDEO_PROTOCOL.md`。

---

### 17. `zst_pad_block()` / `zst_pad_set_block_callback()` — callback-driven unblock

```c
zst_result_t zst_pad_block(zst_pad_t* pad);
zst_result_t zst_pad_set_block_callback(zst_pad_t* pad,
                                        zst_pad_probe_fn callback, void* user_data);
```

**預期：** block 就是暫停，unblock 就是恢復。  
**實際：** `block()` 觸發已設定的 callback（若存在），callback 回傳
`ZST_PAD_PROBE_OK` 解除阻擋，或 `ZST_PAD_PROBE_REBLOCK` 保持阻擋。
這是一個 callback-driven 的阻擋/解除機制，非簡單的 bool flag。  
**FIXME:** 在 header 中說明 callback 觸發時機與回傳值的含義。

---

### 18. `zst_queue_element_set_pool()` — 非典型 pool 綁定

```c
zst_result_t zst_queue_element_set_pool(zst_element_t* el, zst_buffer_pool_t* pool);
```

**預期：** pool 通常由 source 元素管理。  
**實際：** 此函數將 pool 綁定到 queue element 本身，queue 的 worker thread
在 pop 緩衝區後會將其放回 pool。  
**FIXME:** 在 header 中說明 pool 在 queue element 上的特殊作用。

---

### 19. `zst_pad_push_event_upstream()` — 僅限 SINK Pad

```c
zst_result_t zst_pad_push_event_upstream(zst_pad_t* pad, zst_pad_event_t* event);
```

**預期：** 名為 upstream，直覺以為無論 SRC 或 SINK pad 皆能向上游傳送事件。  
**實際：** 原始碼首行直接檢查 `if (sink->direction != ZST_PAD_SINK) return ZST_ERROR;`。只有 SINK pad 才能推動 upstream event，傳入 SRC pad 永遠直接返回錯誤。  
**FIXME:** 參數更名為 `sink_pad` 或函數重命名為 `zst_sink_pad_push_event_upstream()`，並在註解明確說明方向限制。

---

### 20. `zst_element_snapshot_src_pads()` — double-indirection 分配

```c
zst_result_t zst_element_snapshot_src_pads(zst_element_t* el,
                                           zst_pad_t*** pads_out,
                                           uint32_t* count_out);
```

**預期：** 取得 pad 陣列。  
**實際：** 傳回的是 `zst_pad_t**`（陣列的指標），需搭配
`zst_element_pad_snapshot_free(pads, count)` 釋放。
呼叫端容易忘記 free 或 free 錯指標。  
**FIXME:** 考慮改用 `zst_pad_t** pads_out`（單一指標）或在結構中封裝
allocate/free pair。

---

### 21. `zst_buffer_t` / `zst_element_t` / `zst_pad_t` / `zst_pipeline_t` — 結構體完全開放

```c
/* zst_types.h */
typedef struct zst_buffer      zst_buffer_t;
typedef struct zst_pad         zst_pad_t;
typedef struct zst_element     zst_element_t;
typedef struct zst_pipeline    zst_pipeline_t;

/* zst_buffer.h — 完整結構定義 */
struct zst_buffer {
    _Alignas(ZST_CACHE_LINE_SIZE) _Atomic(int) refcount;
    uint32_t type;
    uint32_t flags;
    zst_time_t pts;
    zst_time_t dts;
    zst_time_t duration;
    zst_memory_t memory;
    void* payload;
    void* metadata;
    struct zst_buffer_pool* pool;
    void (*destroy)(zst_buffer_t* buf);
};
```

**預期：** C 框架通常使用 opaque pimpl（前向宣告 + 私有實現），使用者看不到結構體欄位。
**實際：** `zst_buffer_t`、`zst_element_t`、`zst_pad_t`、`zst_pipeline_t` 的結構體定義都直接在 header 中公開。
這意味著：
- 呼叫端可以直接讀寫 `buf->pts`、`buf->flags`、`el->state` 等欄位
- ABI 穩定性依賴於結構體大小不變（無法透過 realloc 內部緩衝區）
- 沒有編譯器強制隱藏內部狀態的能力
**FIXME:** 考慮將上述結構體移至 `.c` 檔案中改為 opaque pointer，或至少在 header 頂部明確聲明這些結構為 internal-only。

---

### 22. `zst_scheduler_run()` — 阻塞式長跑函數

```c
zst_result_t zst_scheduler_run(zst_scheduler_t* sched);
```

**預期：** 「run」可能意味著啟動异步執行或返回一個 handle。
**實際：** 此函數為阻塞式，會一直執行直到收到 `zst_scheduler_stop()` 或 EOS 事件。
名稱看不出它會佔用呼叫线程直到主動停止。
**FIXME:** 在 header 註解中加入「blocks until stop or EOS」說明，並建議在獨立线程中調用。

---

### 23. `zst_bus_set_handler()` vs `zst_bus_pop()` — 互斥事件消費模式

```c
zst_result_t zst_bus_set_handler(zst_bus_t* bus, zst_bus_handler_t handler, void* user_data);
zst_result_t zst_bus_pop(zst_bus_t* bus, zst_event_t** event, uint32_t timeout_ms);
```

**預期：** 兩者似乎都是「從 bus 獲取事件」的不同方式。
**實際：** 這兩個模式是**互斥**的——一旦設定了 handler callback，`zst_bus_pop()` 就不會再返回事件；反之亦然。
這種互斥關係無法從簽名推導，需要閱讀源碼或文檔才能得知。
**FIXME:** 在 `zst_bus_set_handler` 註解中明確說明與 `zst_bus_pop` 的互斥關係。

---

### 24. `zst_pipeline_start()` / `stop()` vs `zst_scheduler_run()` — 雙重啟動機制

```c
zst_result_t zst_pipeline_start(zst_pipeline_t* pipe);
zst_result_t zst_pipeline_stop(zst_pipeline_t* pipe);
zst_result_t zst_scheduler_run(zst_scheduler_t* sched);
```

**預期：** 名稱暗示 pipeline 與 scheduler 各自獨立控制啟動。
**實際：** `zst_pipeline_set_state()` 是標準的狀態轉換方式，而 `zst_pipeline_start()`/`stop()` 是另一條路徑，兩者在行為上有重疊。
此外，scheduler 的 run 循環內部依賴 pipeline 的 state 設置，但 `start()` 的作用範圍不明確。
**FIXME:** 在 header 中澄清 `pipeline_start/stop` 與 `scheduler_run` 的互動關係，或考慮統一為單一路徑。

---

### 25. `zst_scheduler_attach()` 無對應 `detach()`

```c
zst_result_t zst_scheduler_attach(zst_scheduler_t* sched, zst_pipeline_t* pipe);
/* 無 zst_scheduler_detach() */
void zst_scheduler_destroy(zst_scheduler_t* sched);
```

**預期：** 有 attach 通常會有對應的 detach。
**實際：** detach 僅能在 `zst_scheduler_destroy()` 時隱式發生。
這使得無法在 runtime 更換 scheduler 所屬的 pipeline，也無法共享同一個 scheduler 給多個 pipeline。
**FIXME:** 考慮加 `zst_scheduler_detach()`，或在 destroy 前明確註解「implicit detach」。

---

### 26. `zst_clock_wait()` 為 void — 無法得知是否被訊號中斷

```c
void zst_clock_wait(zst_clock_t* clock, zst_time_t time);
```

**預期：** 等待函數通常回傳 status，告知因何原因提前結束。
**實際：** 此函數為 void，無法得知等待是否因訊號中斷（EINTR）或其他原因提前結束。
在實作上可能使用 `clock_nanosleep()`，若被 signal 打斷則直接返回而不补足時間。
**FIXME:** 改為回傳 `zst_result_t`，或在註解中明確說明「不處理 EINTR」的行為契約。

---

### 27. `zst_element_ops_t` — optional callbacks 預設行為不明

```c
typedef struct {
    zst_result_t (*open)(zst_element_t* el);
    zst_result_t (*close)(zst_element_t* el);
    zst_result_t (*start)(zst_element_t* el);
    zst_result_t (*stop)(zst_element_t* el);
    zst_result_t (*preroll)(zst_element_t* el);
    zst_result_t (*unpreroll)(zst_element_t* el);
    zst_result_t (*process)(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out);
    zst_caps_t* (*get_caps)(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter);
    zst_clock_t* (*provide_clock)(zst_element_t* el);
    zst_result_t (*set_property)(zst_element_t* el, const char* name, const char* value);
    zst_result_t (*get_property)(zst_element_t* el, const char* name, char* value_out, size_t max_len);
    zst_buffer_pool_t* (*get_pool)(zst_element_t* el);
    zst_result_t (*event)(zst_element_t* el, zst_pad_t* sink_pad, zst_pad_event_t* event);
    uint32_t (*get_stream_count)(zst_element_t* el);
    zst_result_t (*get_stream_info)(zst_element_t* el, uint32_t index, zst_stream_info_t* info_out);
    zst_pad_t* (*get_stream_pad)(zst_element_t* el, zst_stream_id_t stream_id);
} zst_element_ops_t;
```

**預期：** 所有欄位皆為必要實作。
**實際：** 許多欄位是 optional（如 `provide_clock`、`preroll`、`get_stream_*`），當為 NULL 時框架使用默認行為。
但 header 中沒有標記哪些是 optional，呼叫端必須通過查閱源碼或文檔才能得知。
**FIXME:** 在 ops struct 註解中明確標記 optional callbacks，並說明 NULL 時的默認行為。

---

### 28. `zst_event_t` — 巨型 union 無 tagged union 保護

```c
struct zst_event {
    zst_event_type_t type;
    zst_element_t* src;
    union {
        struct { zst_result_t result; char* message; } error;
        struct { zst_state_t old_state; zst_state_t new_state; } state_changed;
        zst_segment_t segment;
        /* ... 十幾個其他欄位 ... */
        struct { char* type; char* sdp; } webrtc_local_description;
        struct { zst_dante_flow_t flow; } dante_flow;
        /* 等等 */
    } as;
};
```

**預期：** 有 `type` 欄位即可區分 payload。
**實際：** union 中包含超過 20 個不同類型的 payload，且沒有任何編譯期檢查確保存取前已正確判別 `type`。
呼叫端很容易因 `type` 判斷錯誤而讀取錯誤的 union 欄位，造成未定義行為。
**FIXME:** 考虑使用 C11 `_Generic` 或 wrapper 函數來安全存取 union 欄位，或在結構體中加入 tag 確保型別安全。

---

### 29. `zst_pad_get_caps()` — 傳回全新配置的 Copy，非借用指標

```c
zst_caps_t* zst_pad_get_caps(zst_pad_t* pad);
```

**預期：** 一般 C 語言的 `get_xxx()` 通常只是借用（borrow）內部欄位的指標。  
**實際：** 內部實作是 `return zst_caps_copy(pad->caps);`。每次呼叫都會在 Heap 上新建一個 Caps 物件。  
**陷阱：** 呼叫端若像一般 getter 那樣頻繁呼叫檢查（如 `if (zst_pad_get_caps(pad) != NULL)`）且未呼叫 `zst_caps_destroy()`，每次呼叫都會造成記憶體洩漏。  
**FIXME:** 改名為 `zst_pad_copy_caps()`，或在 header 明確註解「caller owns the returned caps and must destroy it」。

---

### 30. `zst_pad_push()` — 僅借用 Buffer，不轉移所有權

```c
zst_result_t zst_pad_push(zst_pad_t* pad, zst_buffer_t* buf);
```

**預期：** GStreamer 的 `gst_pad_push()` 會直接接管 buffer 的生命週期（消耗 1 個 refcount，caller 不需要再 unref）。  
**實際：** `zst_pad_push()` **完全不增減 refcount**，僅僅是借用傳遞。  
**陷阱：** 若開發者帶入 GStreamer 經驗，push 後沒有呼叫 `zst_buffer_unref()`，Buffer 會永久洩漏；反之，若在 Sink 元素端誤以為自己持有所有權而多做一次 unref，會直接造成 Double-free。  
**FIXME:** 在 `zst_pad.h` 頂部以醒目警告標註：「`zst_pad_push()` does NOT consume the buffer reference」。

---

### 31. `zst_element_create()` — `priv` 隱式要求必須由 `malloc`/`calloc` 分配

```c
zst_element_t* zst_element_create(const zst_element_ops_t* ops, void* priv);
```

**預期：** `priv` 看似是純粹的 user context，由外部呼叫者自理生命週期。  
**實際：** 在 `zst_element_destroy()` 中，代碼直接呼叫了 `free(el->priv)`。  
**陷阱：** 若使用者傳入全域變數、堆疊位址、或 C++ 類別內的成員變數位址，當 Pipeline 銷毀時會直接觸發 `free(): invalid pointer` 崩潰（如在 `sc6f0-dante-rx` 遇到過的崩潰主因）。  
**FIXME:** 在 header 宣告註解「`priv` must be dynamically allocated via malloc/calloc or NULL, ownership is transferred to element」；或在 `zst_element_ops_t` 中加入 `destroy_priv` 回呼函式。

---

### 32. `zst_caps_append()` — 隱式轉移 `caps_struct` 所有權

```c
zst_result_t zst_caps_append(zst_caps_t* caps, zst_caps_struct_t* caps_struct);
```

**預期：** 類似容器加入元素，可能是 copy 或內部管理。  
**實際：** `caps` 直接接管 `caps_struct` 的指標並掛入鏈結列。  
**陷阱：** 若呼叫端在 append 後呼叫了 `zst_caps_struct_free(s)`，後續 `zst_caps_destroy` 時會引發 Double-free。  
**FIXME:** 在 header 明確標記 Ownership Transfer（接管指標生命週期）。

---

### 33. `zst_bus_set_handler()` vs `zst_bus_pop()` — 非對稱的 Event 銷毀責任

```c
zst_result_t zst_bus_set_handler(zst_bus_t* bus, zst_bus_handler_t handler, void* user_data);
zst_result_t zst_bus_pop(zst_bus_t* bus, zst_event_t** event, uint32_t timeout_ms);
```

**預期：** 無論用 handler 還是 pop，Event 的生命週期管理規則應該一致。  
**實際：**
- 當使用 `zst_bus_pop(&event)` 時：呼叫端**必須**自行呼叫 `zst_event_destroy(event)`。
- 當使用 `zst_bus_set_handler(handler)` 時：在 handler 回呼返回後，Bus 背景執行緒會**自動**呼叫 `zst_event_destroy(event)`。  
**陷阱：** 在 handler 中若嘗試自行 destroy，背景執行緒會二次釋放（Double-free）；若要在 handler 外保留 event，則必須主動 deep copy。兩者的所有權契約完全相反。  
**FIXME:** 在 `zst_bus.h` 增加「Event Ownership 契約說明矩陣」。

---

### 34. `zst_pipeline_destroy()` / `zst_bin_destroy()` — 深度遞迴銷毀子元件與 Bus

```c
void zst_pipeline_destroy(zst_pipeline_t* pipe);
void zst_bin_destroy(zst_element_t* bin);
```

**預期：** Pipeline 或 Bin 只是容器，可能只需 detach 子元件。  
**實際：**
- `zst_pipeline_destroy()` 會遞迴銷毀其內部的所有 `zst_element_t`，以及它自身的 `zst_bus_t`。
- `zst_bin_destroy()` 同樣會遞迴銷毀其所有子元件。  
**陷阱：** 若外部自管了某些元件或 Bus 的指標並企圖再次釋放，會發生重複銷毀。若需保留元件，必須在 destroy 容器前先呼叫 `zst_pipeline_remove()` 或 `zst_bin_remove()`。  
**FIXME:** 在容器 API 標明「容器具有 Exclusive Ownership（排他性所有權）」。

---

### 35. `zst_pad_push_event_upstream()` — 僅限 SINK Pad

```c
zst_result_t zst_pad_push_event_upstream(zst_pad_t* pad, zst_pad_event_t* event);
```

**預期：** 名為 upstream，直覺以為無論 SRC 或 SINK pad 皆能向上游傳送事件。  
**實際：** 原始碼首行直接檢查 `if (sink->direction != ZST_PAD_SINK) return ZST_ERROR;`。只有 SINK pad 才能推動 upstream event，傳入 SRC pad 永遠直接返回錯誤。  
**FIXME:** 參數更名為 `sink_pad` 或函數重命名為 `zst_sink_pad_push_event_upstream()`。

---

### 36. `zst_allocator_dmabuf_get_fd()` / `jetson_get_fd()` — 回傳 Borrowed FD

```c
int zst_allocator_dmabuf_get_fd(zst_allocator_t* allocator, void* ptr);
int zst_allocator_jetson_get_fd(zst_allocator_t* allocator, void* ptr);
```

**預期：** 取得 fd 後可自主管理或關閉。  
**實際：** 回傳的是 Allocator 內部快取的原始 fd，**呼叫端絕對不能 `close()`**。  
**陷阱：** 若使用者誤呼叫 `close(fd)`，Allocator 的內部映射表將包含失效或被系統重新指派的 fd，導致後續硬體編解碼與 DMABUF 匯出出現不可逆的損壞。  
**FIXME:** 在 header 明確加註「Borrowed fd, caller MUST NOT close」。

---

### 37. `zst_buffer_create()` — 僅分配結構體 Wrapper，不分配記憶體 Payload

```c
zst_buffer_t* zst_buffer_create(uint32_t type);
```

**預期：** 以爲建立了一個可直接讀寫資料的完整 buffer。  
**實際：** 回傳的 `buf->memory.data` 為 `NULL`，`size` 為 0。它只是一個純元資料結構體。  
**陷阱：** 新開發者常會直接嘗試 `memcpy(buf->memory.data, ...)` 導致 Segmentation Fault。  
**FIXME:** 在註解標註「Allocates metadata wrapper only; memory payload is NULL until assigned or created with allocator」。

---

### 38. `ZST_LOG_LEVEL` (Ceiling) — Release 模式編譯期截斷導致動態設定無效

```c
#ifndef ZST_LOG_LEVEL
#  ifdef NDEBUG
#    define ZST_LOG_LEVEL ZST_LOG_LEVEL_WARNING
#  else
#    define ZST_LOG_LEVEL ZST_LOG_LEVEL_TRACE
#  endif
#endif
```

**預期：** 在 Release 模式下呼叫 `zst_log_set_level(ZST_LOG_LEVEL_INFO)` 可以動態開啟 INFO 日誌除錯。  
**實際：** Release 模式（`NDEBUG`）下，前置處理器將 ceiling 預設截斷在 `ZST_LOG_LEVEL_WARNING`。  
**陷阱：** 在正式發布環境中，呼叫 `zst_log_set_level(ZST_LOG_LEVEL_INFO)` 或 `DEBUG` **完全不會產生任何效果**，因為相關的日誌語句在編譯時已經被 Dead-code elimination 剔除。  
**FIXME:** 在日誌系統文檔中明載「Runtime level cannot exceed compile-time ceiling」。

---

## 未來修正目標彙整

| TODO-ID | 優先級 | 內容 | 關聯函數 |
|---------|--------|------|---------|
| `TODO-4` | High | `zst_clock_wait` 重命名或補充 `_duration_ns` 標記 | `zst_clock_wait` |
| `TODO-5` | High | `zst_bus_pop` 補充 non-blocking 契約說明 | `zst_bus_pop` |
| `TODO-8` | High | `zst_stream_info` ownership 說明移至 header 頂部 | `zst_stream_info_clear` |
| `TODO-9` | High | `zst_pad_get_peer` 改為 `zst_pad_get_peer_ref` | `zst_pad_get_peer` |
| `TODO-11` | Medium | `reconfigure_begin/end` 改名為 `txn_begin/commit/abort` | `zst_pipeline_reconfigure_*` |
| `TODO-10` | Medium | `zst_caps_intersect` 改為 `intersect_into` 或加 ownership 註解 | `zst_caps_intersect` |
| `TODO-12` | Medium | `zst_pad_link` 加注「不適用於動態重配置」警告 | `zst_pad_link`, `zst_pipeline_link_pads_dynamic` |
| `TODO-15` | Medium | `zst_webrtc_select_codecs` 改用 struct 輸出 | `zst_webrtc_select_codecs` |
| `TODO-19` | Low | 在 header 中補充 upstream 僅限 SINK pad 說明 | `zst_pad_push_event_upstream` |
| `TODO-20` | Low | `snapshot_*_pads` 改用更安全的 allocate/free pair | `zst_element_snapshot_*_pads` |
| `TODO-21` | High | 核心結構體改為 opaque pimpl，或至少在 header 註解聲明 internal-only | `zst_buffer_t`, `zst_element_t`, `zst_pad_t`, `zst_pipeline_t` |
| `TODO-22` | High | 補充註解說明 `zst_scheduler_run()` 為阻塞式長跑函數，建議 in thread |
| `TODO-23` | High | 在 `zst_bus_set_handler` 註解中說明與 `zst_bus_pop` 互斥 |
| `TODO-24` | Medium | 補充 pipeline 與 scheduler 層級的啟動差別 |
| `TODO-25` | Low | 考慮加 `zst_scheduler_detach()` 或明確註解 destroy 即 detach |
| `TODO-26` | Medium | 改為 `zst_result_t` 傳回，或保證 POSIX cancel 安全並註解說明 |
| `TODO-27` | Low | 在 ops struct 註解中標明哪些 callback 為 optional（NULL = default） |
| `TODO-28` | Low | 為 event union 加 tagged union 結構，或改用單一 payload + type dispatch |
| `TODO-29` | High | `zst_pad_get_caps` 傳回物件需 destroy，註解說明或改名 `copy_caps` | `zst_pad_get_caps` |
| `TODO-30` | High | `zst_pad_push` 標明借用（不轉移 buffer 所有權） | `zst_pad_push` |
| `TODO-31` | High | `zst_element_create` 標明 `priv` 需為 malloc 分配或 NULL | `zst_element_create`, `zst_element_destroy` |
| `TODO-32` | Medium | `zst_caps_append` 標明接管 `caps_struct` 所有權 | `zst_caps_append` |
| `TODO-33` | High | `zst_bus` 明定 handler 自動 destroy vs pop 手動 destroy 契約矩陣 | `zst_bus_set_handler`, `zst_bus_pop` |
| `TODO-34` | Medium | 容器 API 標明排他性所有權（destroy 遞迴釋放所有元件與 bus） | `zst_pipeline_destroy`, `zst_bin_destroy` |
| `TODO-35` | Low | `zst_pad_push_event_upstream` 參數更名為 `sink_pad` | `zst_pad_push_event_upstream` |
| `TODO-36` | High | `zst_allocator_*_get_fd` 標明 borrowed fd 不可 close | `zst_allocator_dmabuf_get_fd`, `zst_allocator_jetson_get_fd` |
| `TODO-37` | Medium | `zst_buffer_create` 標明 memory.data 為 NULL 需額外分配 | `zst_buffer_create` |
| `TODO-38` | Medium | 日誌系統說明 runtime level 無法超越 compile-time ceiling | `zst_log_set_level`, `ZST_LOG_LEVEL` |
| `TODO-2` | Low | `zst_pad_push_sticky_events` 註解補全 | `zst_pad_push_sticky_events` |
| `TODO-3` | Low | `zst_pad_set_unlinked_policy` 補充 policy 行為說明 | `zst_pad_set_unlinked_policy` |
| `TODO-6` | Low | `zst_pipeline_update_ranks_from` 註解補全 | `zst_pipeline_update_ranks_from` |
| `TODO-7` | Low | `zst_buffer_pool_config_from_caps` 補充 heuristic 說明 | `zst_buffer_pool_config_from_caps` |
| `TODO-13` | Low | `zst_gl_comp_sink_capture` 註解補全同步讀取說明 | `zst_gl_comp_sink_capture` |
| `TODO-14` | Low | `zst_webrtc_{filter,compat}_sdp` 統一命名 | 相關兩函數 |
| `TODO-16` | Low | `zst_dante_video_coordinator_apply_flow` 補充領域說明 | `zst_dante_video_coordinator_apply_flow` |
| `TODO-17` | Low | `zst_pad_block` / `set_block_callback` 補充 callback 契約說明 | `zst_pad_block`, `zst_pad_set_block_callback` |
| `TODO-18` | Low | `zst_queue_element_set_pool` 補充 pool 作用說明 | `zst_queue_element_set_pool` |
| `TODO-2` | Low | `zst_pad_push_sticky_events` 註解補全 | `zst_pad_push_sticky_events` |
| `TODO-3` | Low | `zst_pad_set_unlinked_policy` 補充 policy 行為說明 | `zst_pad_set_unlinked_policy` |
| `TODO-6` | Low | `zst_pipeline_update_ranks_from` 註解補全 | `zst_pipeline_update_ranks_from` |
| `TODO-7` | Low | `zst_buffer_pool_config_from_caps` 補充 heuristic 說明 | `zst_buffer_pool_config_from_caps` |
| `TODO-13` | Low | `zst_gl_comp_sink_capture` 註解補全同步讀取說明 | `zst_gl_comp_sink_capture` |
| `TODO-14` | Low | `zst_webrtc_{filter,compat}_sdp` 統一命名 | 相關兩函數 |
| `TODO-16` | Low | `zst_dante_video_coordinator_apply_flow` 補充領域說明 | `zst_dante_video_coordinator_apply_flow` |
| `TODO-17` | Low | `zst_pad_block` / `set_block_callback` 補充 callback 契約說明 | `zst_pad_block`, `zst_pad_set_block_callback` |
| `TODO-18` | Low | `zst_queue_element_set_pool` 補充 pool 作用說明 | `zst_queue_element_set_pool` |

> **優先級說明：**
> - **High**：容易導致誤用且可能引發記憶體洩漏或行為錯誤。
> - **Medium**：行為模糊但誤用機率較低，或可用註解補救。
> - **Low**：語義清晰但有改進空間，不影響正確使用。

---

## 產生方法

1. 讀取 `llms.txt` 與 `llms-full.txt`（AI agent guide 文件）。
2. 交叉比對 `include/*.h` 及 `include/zstreamer/elements/*.h` 中的函數簽名。
3. 識別那些：
   - 傳回值非標準 `zst_result_t`（如 probe ID、raw pointer ownership）
   - 參數方向性不顯（如 upstream 相對性）
   - 隱含的狀態機或 transaction 語義（如 reconfigure begin/end、sticky events）
   - 需要領域知識才能理解（如 Dante flow、GL read-pixel）
   - 多輸出參數與傳回值共存（如 `select_codecs`）
   - **結構體完全開放**（非 opaque pimpl，內部欄位可直接存取）
   - **阻塞式長跑函數**（名稱看不出會佔用呼叫线程）
   - **互斥模式**（兩種看似等價的 API 實際互斥，需查閱文檔）
   - **optional callback 預設行為不明**（struct 中無法區分 mandatory vs optional）
   - **巨型 union 無型別保護**（event payload 存取需手動對照 type）
4. 分類並標記優先級與修正目標。
