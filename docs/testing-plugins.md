@page testing_plugins Testing Plugins Guide

# Testing Plugins Guide

This guide explains how to properly write tests for your custom plugins in `zstreamer`.

## 1. Writing Tests in tests/test_core.c

When developing new plugins or core features, you should add your tests to `tests/test_core.c`. This file contains the test suite for `zstreamer`.

Key guidelines:
* Do NOT call `zst_plugin_registry_deinit()` within individual unit tests. It unregisters all plugins globally, causing subsequent tests that rely on `zst_element_factory_make` to fail.
* Ensure `zst_plugin_registry_init()` is called before instantiating built-in elements to prevent segmentation faults.

## 2. Using Mock Sources and Sinks

For testing your custom element, you typically need to isolate it by connecting it to mock elements like `fakesrc` (or specific test sources like `videotestsrc`/`audiotestsrc`) and `fakesink`.

```c
void test_my_custom_element() {
    zst_plugin_registry_init();

    // Create elements
    zst_element_t* src = zst_element_factory_make("videotestsrc");
    zst_element_t* my_el = zst_element_factory_make("my_element");
    zst_element_t* sink = zst_element_factory_make("fakesink");

    // Create and build pipeline
    zst_pipeline_t* pipeline = zst_pipeline_create();
    zst_pipeline_add(pipeline, src);
    zst_pipeline_add(pipeline, my_el);
    zst_pipeline_add(pipeline, sink);

    // Link elements
    zst_pad_link(zst_element_get_pad(src, "src"), zst_element_get_pad(my_el, "sink"));
    zst_pad_link(zst_element_get_pad(my_el, "src"), zst_element_get_pad(sink, "sink"));

    // Set pipeline to PLAYING state
    zst_pipeline_set_state(pipeline, ZST_STATE_PLAYING);

    // ... sleep or process events ...

    // Cleanup
    zst_pipeline_set_state(pipeline, ZST_STATE_NULL);
    zst_pipeline_destroy(pipeline);
}
```

### Manual Pad Linking in Tests

Sometimes you want to unit test the `process` function directly without a full pipeline. If your source element uses `zst_pad_push`, the source pad *must* be linked to a peer sink pad.

```c
zst_pad_t* dummy_sink = zst_pad_create("dummy_sink", ZST_PAD_SINK);
zst_pad_link(my_src_pad, dummy_sink);

// Now calling my_src_pad->push() won't return a not-linked flow error
```

If you push buffers to `fakesink` or pull buffers manually, remember that `fakesink` does not automatically unref buffers. You must manually call `zst_buffer_unref` to prevent memory leaks.

## 3. Ensuring Graceful Builds

Your plugin code must handle conditional compilation gracefully, especially if it depends on external libraries or specific hardware environments.

* Hardware-specific plugins (e.g., CUDA, Vulkan, Jetson) must gracefully fail or return `NULL` during creation if the required runtime environment or hardware is unavailable.
* Ensure memory is correctly managed to avoid leaks.
* Run tests with ThreadSanitizer (TSAN) to detect race conditions in custom elements. Compile using:
  `cmake -DCMAKE_C_FLAGS="-fsanitize=thread -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread -g" ..`
* Clean up any temporary files or script outputs your tests might generate to keep the repository clean.

All tests must be executed inside a Docker container (e.g., `docker run --rm zstreamer`) as specified in the project guidelines, which mounts the directory to `/app`. Avoid hardcoding local paths like `/workspace` in your plugin tests; use `/app/build/plugins` with fallback logic.
