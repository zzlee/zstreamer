@page tutorials_getting_started Getting Started

# Getting Started with zstreamer: Recording a webcam to MP4 in 5 steps

Welcome to zstreamer! This tutorial will walk you through building a simple C program that captures video from a webcam, encodes it into H.264, muxes it into an MP4 container, and saves it to a file.

## Overview of the Pipeline

In zstreamer, you build a media processing graph by creating **elements** and linking their **pads**. For this tutorial, our pipeline will look like this:

`v4l2src` → `x264enc` → `mp4mux` → `filesink`

## Prerequisites

- You have successfully compiled and installed `zstreamer` (see the README).
- You have a Linux environment with a V4L2 compatible webcam (e.g., `/dev/video0`).

## Step 1: Initialization and Setup

First, we need to include the necessary zstreamer headers and initialize the built-in elements.

```c
#include <stdio.h>
#include <stdlib.h>
#include <zstreamer/zst_pipeline.h>
#include <zstreamer/zst_scheduler.h>
#include <zstreamer/zst_element_factory.h>
#include <zstreamer/zst_pad.h>
#include <zstreamer/zst_bus.h>

int main() {
    // Register built-in elements like v4l2src, x264enc, etc.
    zst_register_builtin_elements();

    // Create the pipeline container
    zst_pipeline_t* pipe = zst_pipeline_create();

    // Create a single-threaded scheduler to drive the pipeline
    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_SINGLE_THREAD,
        .worker_threads = 1
    };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);
```

## Step 2: Creating and Configuring Elements

Next, we instantiate our elements using `zst_element_factory_make` and configure their properties using the typed property setters.

```c
    // Create elements
    zst_element_t* cam  = zst_element_factory_make("v4l2src");
    zst_element_t* h264 = zst_element_factory_make("x264enc");
    zst_element_t* mux  = zst_element_factory_make("mp4mux");
    zst_element_t* sink = zst_element_factory_make("filesink");

    // Configure the camera source (capture 100 frames from /dev/video0)
    zst_element_set_property_string(cam, "device", "/dev/video0");
    zst_element_set_property_int(cam, "num-buffers", 100);

    // Configure the MP4 muxer (matching camera typical resolution and framerate)
    zst_element_set_property_int(mux, "width", 640);
    zst_element_set_property_int(mux, "height", 480);
    zst_element_set_property_int(mux, "fps", 30);
    zst_element_set_property_string(mux, "location", "webcam_output.mp4");

    // Configure the file sink (must write to the same file)
    zst_element_set_property_string(sink, "location", "webcam_output.mp4");
```

## Step 3: Adding to Pipeline and Linking Pads

We add the elements to our pipeline and link them together in order.

```c
    // Add elements to the pipeline
    zst_pipeline_add(pipe, cam);
    zst_pipeline_add(pipe, h264);
    zst_pipeline_add(pipe, mux);
    zst_pipeline_add(pipe, sink);

    // Link the elements' pads
    // cam.src -> h264.sink
    zst_pad_link(zst_element_get_pad(cam, "src"), zst_element_get_pad(h264, "sink"));
    // h264.src -> mux.video
    zst_pad_link(zst_element_get_pad(h264, "src"), zst_element_get_pad(mux, "video"));
    // mux.src -> sink.sink
    zst_pad_link(zst_element_get_pad(mux, "src"), zst_element_get_pad(sink, "sink"));
```

## Step 4: Starting the Pipeline

With the topology set, we attach the pipeline to the scheduler and set the state to `PLAYING` to start the data flow.

```c
    // Attach and start
    zst_scheduler_attach(sched, pipe);
    zst_pipeline_set_state(pipe, ZST_STATE_READY);
    zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
    zst_scheduler_run(sched);

    printf("Recording from webcam...\n");
```

## Step 5: Waiting for Completion and Graceful Shutdown

The pipeline runs asynchronously. We listen to the pipeline's event bus until we receive an End-Of-Stream (EOS) event or an error. Finally, we clean up.

```c
    // Wait for EOS on the bus
    zst_bus_t* bus = zst_pipeline_get_bus(pipe);
    zst_event_t* ev = NULL;

    while (1) {
        zst_result_t r = zst_bus_pop(bus, &ev, 5000); // Wait up to 5 seconds
        if (r == ZST_OK && ev) {
            if (ev->type == ZST_EVENT_EOS) {
                printf("Recording finished successfully.\n");
                zst_event_destroy(ev);
                break;
            } else if (ev->type == ZST_EVENT_ERROR) {
                fprintf(stderr, "Pipeline error occurred!\n");
                zst_event_destroy(ev);
                break;
            }
            zst_event_destroy(ev);
        }
    }

    // Stop and cleanup
    zst_scheduler_stop(sched);
    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);

    return 0;
}
```

## Compiling and Running

Save the code above as `webcam_record.c`. Compile it by linking against the zstreamer and zstreamer-elements libraries:

```bash
gcc webcam_record.c -o webcam_record $(pkg-config --cflags --libs zstreamer zstreamer-elements)
```

Run the program:
```bash
./webcam_record
```

After a few seconds, you should have a `webcam_output.mp4` file in your directory!
