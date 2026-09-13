# H.264/H.265 Parser Filter Implementation Plan

## Status

⬜ Planned — documentation only; no implementation exists yet.

## 1. Goal

Add one encoded-video filter element, `h26xparse`, that:

- accepts `ZST_BUFFER_VIDEO_PACKET` carrying H.264/AVC or H.265/HEVC data;
- accepts buffers split at arbitrary byte boundaries and buffers containing multiple NAL units;
- parses parameter sets and exposes the active video format through source-pad caps;
- detects format changes while `PLAYING` and reports them through zstreamer's existing in-band pad-event and asynchronous bus-event mechanisms;
- optionally combines all slices and associated non-VCL NAL units of one picture into one output access-unit buffer;
- does not decode video frames.

Typical pipelines:

```text
rtpdepay → h26xparse → h264dec/h265dec
mp4demux → h26xparse → rtppay
netsrc   → h26xparse → queue → application sink
```

This is a filter with one always-present sink pad and one always-present source pad. It must not create dynamic pads or introduce a new event subsystem.

## 2. Architectural decisions

### 2.1 Element identity

- Canonical factory name: `h26xparse`.
- Public constructor: `zst_h26x_parser_create()`.
- Category: `Codec/Parser`.
- Sink and source template caps: `video/x-h264;video/x-h265`.
- The selected codec is taken from an incoming sink `ZST_PAD_EVENT_CAPS` event. The `codec` property is a fallback for sources that cannot provide caps.
- Automatic bitstream sniffing is allowed only before the codec is locked. A mid-stream H.264 ↔ H.265 switch must be announced by a new sink CAPS event; guessing a codec switch from payload bytes is intentionally unsupported.

### 2.2 Dependency and parser strategy

Implement the parser in portable C11 without opening a decoder. Use:

1. a bounded byte adapter and NAL scanner;
2. a bounds-checked RBSP bit reader with emulation-prevention-byte removal;
3. codec-specific VPS/SPS/PPS and slice-header parsers.

Do not depend on FFmpeg private structures. This keeps the element usable in reduced builds and makes malformed-input behavior deterministic. Existing AVCC/HVCC conversion code in decoders and muxers may be consulted, but reusable code must be moved deliberately rather than copied again.

### 2.3 Output representation

The first implementation emits canonical Annex-B byte stream only:

- every output NAL starts with `00 00 00 01`;
- source caps contain `stream-format=byte-stream`;
- source caps contain `alignment=nal` when `aggregate-au=false` and `alignment=au` when `aggregate-au=true`;
- `codec_data`, when present, is a canonical Annex-B concatenation of the active VPS/SPS/PPS NAL units.

Length-prefixed AVC/HEVC input is supported and normalized to Annex-B output. Producing AVCC/HVCC output is out of scope for the first implementation.

### 2.4 One input may produce zero or many outputs

`zst_element_ops_t::process()` cannot represent zero-to-many output cleanly. Follow the existing `rtpdepay` pattern:

- install a custom push function on the sink pad;
- parse one input buffer in a loop;
- call `zst_pad_push()` once for every completed NAL or access unit;
- retain incomplete input in the element's bounded adapter.

No core element-vtable ABI change is required.

### 2.5 Dynamic format notification

Use both existing notification paths, for different consumers:

1. **In-band:** update source-pad active caps and push `ZST_PAD_EVENT_CAPS` before the first output buffer that uses the new format.
2. **Out-of-band:** post `ZST_EVENT_CAPS_CHANGED` on the element bus after the in-band CAPS event has been accepted.

Required ordering:

```text
parse parameter set/slice selecting new format
    → build new canonical caps
    → zst_pad_set_caps(src, new_caps)
    → zst_pad_push_event(src, CAPS(new_caps))
    → zst_bus_post(CAPS_CHANGED(old_caps, new_caps))
    → push first NAL/AU using new format
```

The first complete active format is also reported as `CAPS_CHANGED` with `old_caps == NULL`. Repeated semantically identical parameter sets must not emit duplicate events. `ZST_EVENT_STREAM_CHANGED` is not emitted because this filter does not add, remove, or replace a stream.

### 2.6 When a format becomes active

Do not change output caps merely because a new SPS was received. Cache parameter sets by ID and activate a format when a VCL slice references a PPS/SPS chain:

- H.264: slice `pic_parameter_set_id` → PPS → SPS;
- H.265: slice `slice_pic_parameter_set_id` → PPS → SPS → optional VPS.

This prevents an unused parameter set from reconfiguring downstream elements prematurely. Parameter-set caps may be published before the first VCL only when incoming `codec_data` identifies a single unambiguous active configuration.

## 3. Input contract

### 3.1 Supported sink caps

```text
video/x-h264:
  stream-format = byte-stream | avc
  alignment     = nal | au (advisory; arbitrary buffer splits remain valid)
  codec_data    = optional avcC record or Annex-B parameter sets

video/x-h265:
  stream-format = byte-stream | hvc1 | hev1
  alignment     = nal | au (advisory)
  codec_data    = optional hvcC record or Annex-B parameter sets
```

If `stream-format` is absent, `input-stream-format=auto` detects Annex-B versus length-prefixed input once and then locks the result. For AVC/HVCC, parse `lengthSizeMinusOne` from `codec_data`; without `codec_data`, use `nal-length-size`.

### 3.2 Buffer and timestamp rules

- Input memory must be CPU-readable. Non-CPU memory returns `ZST_ERROR` and posts one error event; no implicit GPU/DMABUF mapping is attempted.
- Input timestamps are snapshots, not byte-level timing. A completed output uses the timestamp of the input buffer containing the first byte of its first VCL NAL. If an AU has no VCL NAL, use the first NAL's timestamp.
- Preserve the chosen input `pts`, `dts`, and `duration`. Do not infer frame duration from VUI timing for individual buffers.
- Derive `ZST_BUFFER_FLAG_KEYFRAME` from codec syntax:
  - H.264 IDR slices (`nal_unit_type == 5`);
  - H.265 BLA/IDR/CRA IRAP pictures (NAL types 16–21).
- EOS flushes the final complete access unit. An incomplete NAL is dropped according to `malformed-policy` before forwarding EOS.

## 4. Parsed format model

Keep an internal codec-neutral snapshot. It is not a new public struct in the first phase; applications consume its caps representation.

```c
typedef struct {
    int valid;
    int codec;                  /* H264 or H265 */
    uint32_t coded_width;
    uint32_t coded_height;
    uint32_t width;             /* conformance-cropped display size */
    uint32_t height;
    uint32_t chroma_format_idc;
    uint32_t bit_depth_luma;
    uint32_t bit_depth_chroma;
    uint32_t profile_idc;
    uint32_t level_idc;
    uint32_t tier_flag;         /* H.265; zero for H.264 */
    int progressive;
    int sar_num;
    int sar_den;
    int fps_num;
    int fps_den;
    int full_range;
    int colour_primaries;
    int transfer_characteristics;
    int matrix_coefficients;
    uint64_t generation;
} h26x_video_format_t;
```

Unknown optional values use zero, except tri-state boolean-like values (`progressive`, `full_range`) which use `-1` for unknown.

### 4.1 H.264 fields

Parse enough SPS/VUI/PPS/slice syntax to obtain and safely skip:

- `profile_idc`, constraint flags, `level_idc`, `seq_parameter_set_id`;
- high-profile chroma format and separate colour plane flag;
- luma/chroma bit depth;
- scaling-list presence and bounded skip logic;
- frame number and picture-order-count syntax needed for AU boundaries;
- coded dimensions, frame/field mode, and frame crop offsets;
- VUI aspect ratio, video signal type, colour description, and timing info;
- PPS-to-SPS mapping and slice `pic_parameter_set_id`.

Display width/height must apply the H.264 crop-unit rules for chroma format and field/frame mode, with checked arithmetic.

### 4.2 H.265 fields

Parse enough VPS/SPS/PPS/slice syntax to obtain and safely skip:

- VPS/SPS/PPS IDs and their reference chain;
- `profile_tier_level`, general profile, tier, and level;
- chroma format and separate colour plane flag;
- coded dimensions and conformance window;
- luma/chroma bit depth;
- sub-layer ordering and scaling-list syntax with bounded skip logic;
- short-term reference picture sets with explicit count limits;
- VUI aspect ratio, video signal type, colour description, and timing info;
- `first_slice_segment_in_pic_flag` and slice PPS ID.

Display width/height must apply HEVC conformance-window subsampling units with checked arithmetic.

### 4.3 Source caps fields

Publish only known fields:

| Caps key | Type | Meaning |
|---|---|---|
| media type | native | `video/x-h264` or `video/x-h265` |
| `stream-format` | string | always `byte-stream` |
| `alignment` | string | `nal` or `au` |
| `width`, `height` | int | cropped display dimensions |
| `coded-width`, `coded-height` | int | coded dimensions before cropping |
| `profile` | string | canonical profile name when mapped |
| `profile-idc`, `level-idc` | uint | numeric values from the active SPS |
| `tier` | string | H.265 `main` or `high` |
| `chroma-format` | string | `monochrome`, `4:2:0`, `4:2:2`, or `4:4:4` |
| `bit-depth-luma`, `bit-depth-chroma` | uint | component bit depths |
| `interlace-mode` | string | `progressive`, `interleaved`, or omitted if unknown |
| `pixel-aspect-ratio` | fraction | VUI sample aspect ratio |
| `framerate` | fraction | VUI-derived nominal rate when available |
| `full-range` | int | 0/1 when signaled |
| `colour-primaries` | uint | codec syntax value |
| `transfer-characteristics` | uint | codec syntax value |
| `matrix-coefficients` | uint | codec syntax value |
| `codec_data` | buffer | active parameter sets in Annex-B form |
| `format-generation` | uint | increments on each semantic format change |

A semantic change is any change to the above format fields or active parameter-set bytes. Byte-identical/repeated parameter sets do not increment the generation.

## 5. Access-unit aggregation

### 5.1 Property behavior

`aggregate-au=false`:

- emit one complete Annex-B NAL per output buffer;
- source caps use `alignment=nal`;
- lowest latency and minimal buffering.

`aggregate-au=true`:

- emit one Annex-B access unit per output buffer;
- include all slices belonging to the same coded picture;
- attach relevant AUD, parameter-set, prefix-SEI, and other leading non-VCL NALs to the following picture;
- attach suffix-SEI and trailing picture NALs to the current picture;
- source caps use `alignment=au`.

Changing `aggregate-au` is allowed only in `NULL` or `READY`; runtime changes would make alignment and buffered-data ownership ambiguous.

### 5.2 H.264 picture boundaries

Use AUD when present. Without AUD, implement the primary coded picture comparison from H.264 section 7.4.1.2.4. At minimum compare the parsed fields that can distinguish pictures:

- `frame_num`;
- active PPS ID;
- `field_pic_flag` and `bottom_field_flag`;
- reference/non-reference transition (`nal_ref_idc`);
- POC fields required by the active SPS/PPS;
- IDR versus non-IDR and `idr_pic_id`.

`first_mb_in_slice == 0` alone is not a sufficient boundary rule and must not be used as the sole criterion.

### 5.3 H.265 picture boundaries

A VCL NAL with `first_slice_segment_in_pic_flag == 1` starts a new picture. Handle AUD, EOS, and EOB NALs explicitly. Respect prefix/suffix NAL association so metadata is not shifted to the wrong AU.

### 5.4 Bounds

All aggregation is bounded by:

- `max-nal-size`;
- `max-au-size`;
- `max-buffered-bytes`;
- fixed maximum counts for cached parameter sets and syntax arrays.

Exceeding a limit follows `malformed-policy`; it must never trigger unbounded allocation or integer wraparound.

## 6. Properties

All descriptor defaults are string literals, matching the project property rules.

| Property | Type | Flags | Default | Meaning |
|---|---|---|---|---|
| `codec` | string | R/W | `auto` | `auto`, `h264`, or `h265` |
| `input-stream-format` | string | R/W | `auto` | `auto`, `byte-stream`, `avc`, `hvc1`, or `hev1` |
| `nal-length-size` | uint | R/W | `4` | fallback length field size, 1–4 |
| `aggregate-au` | bool | R/W | `false` | combine slices into access units |
| `malformed-policy` | string | R/W | `error` | `error`, `drop`, or `warn-drop` |
| `max-nal-size` | uint | R/W | `16777216` | maximum bytes in one NAL |
| `max-au-size` | uint | R/W | `67108864` | maximum bytes in one AU |
| `max-buffered-bytes` | uint | R/W | `67108864` | total parser buffering limit |
| `format-generation` | uint | R | `0` | number of published formats |
| `parsed-nals` | uint | R | `0` | complete NAL units parsed |
| `output-buffers` | uint | R | `0` | buffers emitted |
| `parse-errors` | uint | R | `0` | malformed syntax/length errors |
| `dropped-nals` | uint | R | `0` | NAL units dropped by policy |

Configuration properties are mutable only in `NULL`/`READY`. Statistics remain readable in all states.

## 7. Pad-event behavior

Implement `zst_element_ops_t::event` and preserve event order.

| Incoming event | Required behavior |
|---|---|
| `STREAM_START` | reset codec lock, parameter-set caches, adapter, AU state, and generation; forward downstream |
| `CAPS` | validate media type and stream-format; parse codec_data; reset only if codec/input representation changed; do not forward unparsed sink caps verbatim |
| `SEGMENT` | forward downstream after any pending pre-segment data is resolved |
| `FLUSH_START` | discard adapter/AU state immediately; forward |
| `FLUSH_STOP` | reset picture-boundary history and forward |
| `DISCONT` | discard incomplete NAL/AU, retain valid parameter-set caches, and forward |
| `EOS` | flush final complete NAL/AU, apply malformed policy to trailing bytes, then forward |
| other events | forward unchanged unless explicitly unsupported |

Add public constructors `zst_pad_event_new_flush_start()`, `zst_pad_event_new_flush_stop()`, and `zst_pad_event_new_discont()` only if the implementation must originate those events. Do not construct public event structs manually.

## 8. Error behavior

- Invalid property or unsupported caps: return `ZST_ERROR_INVALID_ARGUMENT`.
- Need more bytes: retain bounded data and return `ZST_OK`, not `ZST_AGAIN` to the upstream push path.
- Allocation failure or downstream push failure: return `ZST_ERROR` and post `ZST_EVENT_ERROR` once for the failing operation.
- Malformed syntax:
  - `error`: post error and fail the push;
  - `drop`: increment counters and silently resynchronize;
  - `warn-drop`: post `ZST_EVENT_WARNING`, increment counters, and resynchronize.
- Annex-B recovery scans for the next valid 3/4-byte start code within the configured bound.
- Length-prefixed recovery drops the current malformed unit; it must not reinterpret arbitrary payload as a new length field without a buffer/caps discontinuity.

## 9. Implementation layout

### 9.1 New files

```text
include/zstreamer/elements/zst_h26x_parser.h
src/h26x_parser.c
tests/test_h26x_parser.c
```

Keep bit-reader, NAL adapter, SPS/PPS/VPS caches, AU assembly, and element integration in separate sections of `src/h26x_parser.c` initially. Split internal helpers into `src/h26x_parser_*.c/.h` only if the implementation becomes difficult to review; internal headers must not be installed.

### 9.2 Existing files to update

```text
src/CMakeLists.txt
src/zst_builtins.c
tests/CMakeLists.txt
wiki/implementation-plan.md
```

Registration work includes:

- strong constructor declaration;
- `create_builtin_element("h26xparse")` branch;
- built-in pad template and typed property specs;
- built-in descriptor;
- `zstreamer-elements` source list;
- dynamic plugin target `zst_h26xparse`, plugin export descriptor, and `PLUGIN_TARGETS` entry;
- installed public constructor header (covered by the existing elements-header install rule).

The parser should be built regardless of `HAS_FFMPEG`.

## 10. Executable implementation phases

### Phase A — Element shell and byte adapter

- [ ] Add the public constructor header and element skeleton.
- [ ] Add sink/source pads, caps templates, lifecycle cleanup, properties, counters, and built-in/plugin registration.
- [ ] Implement bounded buffering for arbitrary input splits.
- [ ] Implement Annex-B 3-byte/4-byte start-code scanning across buffer boundaries.
- [ ] Implement 1/2/3/4-byte length-prefixed NAL extraction and avcC/hvcC length-size parsing.
- [ ] Normalize complete output NALs to 4-byte Annex-B start codes.
- [ ] Add malformed-input resynchronization and size/overflow checks.

**Exit criterion:** arbitrary chunking produces the same ordered NAL bytes as unsplit input for H.264 and H.265.

### Phase B — Safe codec syntax parsing

- [ ] Add bounded bit reads, unsigned/signed Exp-Golomb reads, and RBSP unescaping.
- [ ] Parse H.264 SPS/PPS and the slice fields needed for parameter-set activation and AU boundaries.
- [ ] Parse H.265 VPS/SPS/PPS and the slice fields needed for parameter-set activation and AU boundaries.
- [ ] Add explicit limits for IDs, sub-layers, scaling lists, RPS counts, and all allocation sizes.
- [ ] Cache raw parameter-set NALs by ID and replace them atomically at a push-call boundary.

**Exit criterion:** known AVC/HEVC fixtures produce correct dimensions, profile/level, bit depth, chroma, crop, VUI, and PPS/SPS references under ASan/UBSan.

### Phase C — Caps and dynamic format events

- [ ] Convert the active format snapshot into canonical source caps.
- [ ] Implement semantic format comparison and generation tracking.
- [ ] Set source active caps and push sticky in-band CAPS events.
- [ ] Post bus `ZST_EVENT_CAPS_CHANGED` with owned old/new caps copies.
- [ ] Guarantee CAPS-before-buffer ordering for the first and every changed format.
- [ ] Handle a caps-announced codec switch by dropping incomplete old-codec data, resetting caches, and starting a new format generation.

**Exit criterion:** a stream changing 1920×1080 → 1280×720 emits exactly two ordered CAPS events and two bus notifications, with no duplicate event for repeated SPS data.

### Phase D — Optional access-unit aggregation

- [ ] Implement H.264 primary-picture boundary comparison.
- [ ] Implement H.265 first-slice picture boundary handling.
- [ ] Associate AUD/SEI/parameter-set/EOS/EOB NALs with the correct AU.
- [ ] Derive keyframe flags from VCL NAL types.
- [ ] Preserve timestamp and duration according to section 3.2.
- [ ] Flush complete final AU on EOS and discard/report incomplete trailing data.

**Exit criterion:** two-slice H.264 and H.265 pictures each produce one output buffer in AU mode and two VCL outputs in NAL mode, with correct keyframe and timestamp metadata.

### Phase E — Integration, documentation, and hardening

- [ ] Test factory creation, property type checking, descriptor introspection, static linking, and dynamic plugin loading.
- [ ] Test `rtpdepay → h26xparse → h264dec/h265dec` and `mp4demux → h26xparse → fakesink` pipelines.
- [ ] Add malformed, truncated, oversized, random-chunk, and repeated-flush stress cases.
- [ ] Confirm no leaks across repeated `NULL → PLAYING → NULL` cycles.
- [ ] Add a user-facing element reference with caps, properties, event semantics, and pipeline examples.
- [ ] Update the project status only after all acceptance checks pass.

**Exit criterion:** the Docker dev build and all tests pass, and the installed SDK can create `h26xparse` through the public factory.

## 11. Test matrix

`tests/test_h26x_parser.c` must include deterministic local fixtures and no network dependency.

| Area | Required cases |
|---|---|
| Framing | Annex-B 3/4-byte start codes; AVCC/HVCC length sizes 1/2/4; many NALs per input; every possible split point |
| H.264 format | baseline/high profile; crop; interlaced flags; extended SAR; VUI timing/color; repeated and replaced SPS/PPS IDs |
| H.265 format | main/main10; conformance window; tier/level; VUI timing/color; repeated and replaced VPS/SPS/PPS IDs |
| Dynamic behavior | first format; same-format repeat; resolution/profile/bit-depth change; unused SPS does not change caps; explicit H.264→H.265 caps switch |
| Events | in-band CAPS precedes first affected buffer; sticky replay after late link; bus old/new caps ownership survives parser teardown |
| Aggregation | one slice; multiple slices; AUD and no AUD; SEI placement; H.264 field pictures; H.265 IRAP; EOS flush |
| Metadata | PTS/DTS/duration selection; H.264 IDR and H.265 IRAP keyframe flags |
| Robustness | truncated RBSP; invalid Exp-Golomb; oversized lengths; crop underflow; count bombs; emulation-prevention edge cases; non-CPU memory |
| Lifecycle | flush/discontinuity; repeated state changes; unlinked source; downstream push error |

Fixture comments must state codec, expected parsed values, and how the bytes were generated. Tests must compare values rather than only checking that parsing succeeds.

## 12. Docker validation commands

All compilation and testing must follow the repository Docker rule:

```bash
./scripts/build-docker.sh dev
docker run --rm zstreamer-build:dev
```

For focused development with a live source mount:

```bash
docker run --rm \
  -v "$(pwd):/workspace" \
  --entrypoint bash zstreamer-build:dev \
  -c 'cmake -S /workspace -B /tmp/zst-build -DBUILD_TESTS=ON && \
      cmake --build /tmp/zst-build -j && \
      ctest --test-dir /tmp/zst-build -R h26x_parser --output-on-failure'
```

Before completion, run the full suite with `ctest --output-on-failure`. If sanitizer options are added to the project, run the parser test under ASan/UBSan as a separate Docker configuration.

## 13. Acceptance criteria

The feature is complete only when all of the following are true:

- [ ] One `h26xparse` element accepts both H.264 and H.265 packet streams.
- [ ] Annex-B and length-prefixed input work with arbitrary buffer boundaries.
- [ ] Active dimensions and required format fields are represented in source caps.
- [ ] A referenced SPS/PPS/VPS format change is applied while `PLAYING` without recreating the element.
- [ ] Every semantic format change sends an in-band CAPS event and a bus `CAPS_CHANGED` event before affected data.
- [ ] Repeated equivalent parameter sets do not create event storms.
- [ ] `aggregate-au=false` emits complete NAL units; `aggregate-au=true` emits complete pictures containing all slices.
- [ ] Keyframe and timestamp behavior matches this document.
- [ ] Malformed and adversarial input remains bounded and leak-free.
- [ ] Built-in, static-library, installed-SDK, and dynamic-plugin creation paths work.
- [ ] Focused and full Docker test suites pass.

## 14. Explicit non-goals

- Decoding pixels or validating reconstructed pictures.
- RTP depayloading, packet-loss repair, or network reordering; these belong in `rtpdepay`/transport elements.
- Changing encoded resolution, profile, level, or bit depth; the filter reports changes but does not transcode.
- AVCC/HVCC output generation in the first implementation.
- Closed-caption extraction, HDR mastering metadata APIs, or SEI-to-bus messages in the first implementation.
- Guessing unannounced mid-stream codec switches.
