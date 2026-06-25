# Preroll Implementation Plan

## Goal
Implement the `ZST_STATE_PAUSED` preroll state mechanism as described in the architecture document. Preroll allows the pipeline to fill up with data and block at the sink elements before transitioning to the `PLAYING` state. This ensures immediate playback with perfect A/V sync once the pipeline is started.

## Architectural Changes

### 1. Element Ops VTable Extension
- Add `zst_result_t (*pause)(zst_element_t* el);` to `zst_element_ops_t` in `include/zst_element.h`.
- This hook will be called during `PLAYING -> PAUSED` and `READY -> PAUSED` transitions.

### 2. State Machine Updates
- In `src/zst_element.c`, modify `zst_element_set_state` to properly handle `READY -> PAUSED` and `PLAYING -> PAUSED` transitions.
- Introduce `ZST_STATE_ASYNC` or similar asynchronous tracking for the pipeline to wait until all sink elements have completed preroll before emitting the `ZST_EVENT_STATE_CHANGED` for `PAUSED`.

### 3. Clock & A/V Sync Handling
- In `src/zst_clock.c` and scheduler logic, ensure the pipeline clock **freezes** while in the `PAUSED` state.
- When transitioning `PAUSED -> PLAYING`, the clock must resume smoothly without causing a sudden jump in time, to prevent QoS algorithms from erroneously dropping the prerolled buffers.

### 4. Sink Element Preroll Logic
- Core sink elements (e.g., `x11_sink`, `alsa_sink`, `file_sink`, `fake_sink`, `net_sink`, etc.) must be updated to handle the `PAUSED` state.
- If the element is in `PAUSED`, it should consume exactly one buffer (the preroll buffer) and then **block** the incoming thread (typically the upstream queue's worker thread) or return a specific code indicating preroll completion.

### 5. Live Source Handling
- Live sources (e.g., `v4l2_source`, `net_source`) cannot be physically paused. They must continue producing data.
- If downstream queues fill up due to sinks blocking in preroll, live sources must employ a leaky queue strategy (dropping older buffers) or explicitly drop new buffers to prevent deadlocks.

## Checklist

- [ ] Add `pause` to `zst_element_ops_t` in `include/zst_element.h`.
- [ ] Update `zst_element_set_state` in `src/zst_element.c` to call `pause`.
- [ ] Implement asynchronous state waiting in `zst_pipeline` for the `PAUSED` state.
- [ ] Update `zst_clock` and `zst_scheduler` to freeze/resume the clock accurately during `PAUSED` state.
- [ ] Implement preroll blocking logic in `src/fake_sink.c` as a reference.
- [ ] Implement preroll blocking logic in `src/x11_sink.c` and other critical sink elements.
- [ ] Update live source elements (e.g., `src/v4l2_source.c`) to handle backpressure via leaky queues during `PAUSED` state.
- [ ] Write unit tests verifying `READY -> PAUSED -> PLAYING` state transitions and preroll buffer handling.
