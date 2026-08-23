@page tutorials_network_streaming Network Streaming Tutorial

# Network Streaming Tutorial: Live Video over RTSP

In this tutorial, we will learn how to use zstreamer to stream a live video source over the network using the RTSP (Real Time Streaming Protocol). We will create a pipeline that generates a test video pattern, encodes it to H.264, and serves it over RTSP.

## Prerequisites

- `zstreamer` compiled and installed.
- The `rtsp_server` and `rtspsink` elements built (enabled by default).
- An RTSP client like VLC or `ffplay` installed on your machine to test the stream.

## Architecture

Our streaming pipeline will look like this:

1. **`videotestsrc`**: Generates a live colour bar pattern.
2. **`x264enc`**: Compresses the raw video frames into an H.264 bitstream.
3. **`rtspsink`**: Receives the encoded video and pushes it into the RTSP server for distribution.
4. **`rtsp_server`**: (A special background element) Listens for client connections on port 8554 and serves the stream.

## Writing the Code

Let's look at the complete C program to set this up.

```c
#include <stdio.h>
#include <stdlib.h>
#include <zstreamer/zst_pipeline.h>
#include <zstreamer/zst_scheduler.h>
#include <zstreamer/zst_element_factory.h>
#include <zstreamer/zst_pad.h>
#include <zstreamer/zst_bus.h>

int main() {
    // 1. Initialization
    zst_register_builtin_elements();
    zst_pipeline_t* pipe = zst_pipeline_create();

    // We use a multi-threaded scheduler for better network performance
    zst_scheduler_config_t cfg = {
        .mode = ZST_SCHEDULER_MULTI_THREAD,
        .worker_threads = 2
    };
    zst_scheduler_t* sched = zst_scheduler_create(&cfg);

    // 2. Create Elements
    // The RTSP server manages client connections and stream sessions.
    zst_element_t* server = zst_element_factory_make("rtspserver");
    zst_element_t* vsrc   = zst_element_factory_make("videotestsrc");
    zst_element_t* h264   = zst_element_factory_make("x264enc");
    // The RTSP sink connects our pipeline to a specific mount point on the server.
    zst_element_t* rsink  = zst_element_factory_make("rtspsink");

    // 3. Configure Properties
    // Set server port
    zst_element_set_property_int(server, "port", 8554);

    // Configure video source: 640x480 at 30fps, continuous streaming (no num-buffers limit)
    zst_element_set_property_int(vsrc, "width", 640);
    zst_element_set_property_int(vsrc, "height", 480);
    zst_element_set_property_int(vsrc, "fps", 30);
    // Since it's a live stream, we must slave to the pipeline clock
    zst_element_set_property_bool(vsrc, "use-clock", true);

    // Configure RTSP sink
    zst_element_set_property_string(rsink, "mount", "/live");

    // 4. Build Pipeline
    // Note: We add the server to the pipeline even though it isn't linked to via pads.
    // It needs to be in the pipeline to receive state changes (READY -> PLAYING).
    zst_pipeline_add(pipe, server);
    zst_pipeline_add(pipe, vsrc);
    zst_pipeline_add(pipe, h264);
    zst_pipeline_add(pipe, rsink);

    // Link the data path: vsrc -> h264 -> rsink
    zst_pad_link(zst_element_get_pad(vsrc, "src"), zst_element_get_pad(h264, "sink"));
    zst_pad_link(zst_element_get_pad(h264, "src"), zst_element_get_pad(rsink, "sink"));

    // 5. Run
    zst_scheduler_attach(sched, pipe);
    zst_pipeline_set_state(pipe, ZST_STATE_READY);
    zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
    zst_scheduler_run(sched);

    printf("RTSP Server running!\n");
    printf("Connect using: ffplay rtsp://127.0.0.1:8554/live\n");
    printf("Press Ctrl+C to stop.\n");

    // Keep the main thread alive while the scheduler runs in the background.
    // In a real application, you would handle signals like SIGINT here to break the loop.
    zst_bus_t* bus = zst_pipeline_get_bus(pipe);
    zst_event_t* ev = NULL;
    while (1) {
        if (zst_bus_pop(bus, &ev, 1000) == ZST_OK && ev) {
            if (ev->type == ZST_EVENT_ERROR) {
                fprintf(stderr, "Pipeline error!\n");
                zst_event_destroy(ev);
                break;
            }
            zst_event_destroy(ev);
        }
    }

    // 6. Cleanup
    zst_scheduler_stop(sched);
    zst_pipeline_set_state(pipe, ZST_STATE_NULL);
    zst_scheduler_destroy(sched);
    zst_pipeline_destroy(pipe);

    return 0;
}
```

## Compiling and Testing

Compile the program:

```bash
gcc rtsp_stream.c -o rtsp_stream $(pkg-config --cflags --libs zstreamer zstreamer-elements)
```

Run the server:

```bash
./rtsp_stream
```

Open a new terminal and view the stream using `ffplay`:

```bash
ffplay rtsp://127.0.0.1:8554/live
```

You should see the scrolling colour bars test pattern!

## Key Concepts to Remember

- **`use-clock`**: When streaming live over the network, synthetic sources like `videotestsrc` must have `use-clock` set to `true`. This ensures they output frames at real-time speeds (e.g., waiting 33ms between 30fps frames) rather than running as fast as the CPU allows.
- **Server Element**: Network servers in zstreamer (like `rtspserver`) are instantiated as elements and added to the pipeline to manage their lifecycle, even if they don't have direct pad links. The sink elements (`rtspsink`) handle the data transfer internally to the server.

---

## Low-Level Raw Network Streaming (netsink and netsrc)

For low-level, high-performance streaming of raw binary bytes (without application-layer framing like RTSP or RTMP), you can use the netsink and netsrc elements. These support TCP, UDP, and UNIX Domain Sockets.

### 1. UDP Streaming: Push Mode (Unicast / Multicast)
In Push Mode, the sender pushes packets directly to a target IP/port, and the receiver binds to that port and listens.

**Sender Configuration (netsink):**
```c
zst_element_t* sink = zst_element_factory_make("netsink");
zst_element_set_property_string(sink, "protocol", "udp-client"); // or "udp"
zst_element_set_property_string(sink, "host", "127.0.0.1"); // Target IP
zst_element_set_property_string(sink, "port", "5001");      // Target Port
```

**Receiver Configuration (netsrc):**
```c
zst_element_t* src = zst_element_factory_make("netsrc");
zst_element_set_property_string(src, "protocol", "udp");   // binds locally
zst_element_set_property_string(src, "port", "5001");      // Port to listen on
```

### 2. UDP Streaming: Pull Mode (NAT / Stateful Firewall Friendly)
When the receiver is behind NAT or a stateful firewall, Push Mode will fail because firewalls block unsolicited incoming UDP traffic. In Pull Mode, the receiver initiates contact.

**Sender Configuration (netsink acting as a Server):**
- Binds to a local port and waits for the client to contact it first.
```c
zst_element_t* sink = zst_element_factory_make("netsink");
zst_element_set_property_string(sink, "protocol", "udp-server");
zst_element_set_property_string(sink, "port", "5002"); // Port to bind to
```

**Receiver Configuration (netsrc acting as a Client):**
- Actively connects to the sender and sends a single registration byte `\0` to punch a hole through NAT and register its public IP/port with the sender.
```c
zst_element_t* src = zst_element_factory_make("netsrc");
zst_element_set_property_string(src, "protocol", "udp-client");
zst_element_set_property_string(src, "host", "127.0.0.1"); // Sender IP
zst_element_set_property_string(src, "port", "5002");      // Sender Port
```

### 3. TCP Streaming (Client / Server)
TCP provides a connection-oriented, reliable stream of bytes.

**TCP Client Source / TCP Server Sink:**
- **Receiver (netsrc in client mode):** Connects to `host:port`.
  ```c
  zst_element_set_property_string(src, "protocol", "tcp-client");
  zst_element_set_property_string(src, "host", "127.0.0.1");
  zst_element_set_property_string(src, "port", "5003");
  ```
- **Sender (netsink in server mode):** Listens on `port` for incoming client connections.
  ```c
  zst_element_set_property_string(sink, "protocol", "tcp-server");
  zst_element_set_property_string(sink, "port", "5003");
  ```
