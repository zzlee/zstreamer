/* Deterministic framing tests; fixtures are deliberately syntax-light. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zst_pad_event.h"
#include "zstreamer/elements/zst_fake_sink.h"
#include "zstreamer/elements/zst_h26x_parser.h"

typedef struct { unsigned count; size_t sizes[8]; uint32_t flags[8]; uint8_t data[8][32]; } capture_t;
typedef struct { unsigned caps, eos, discont; } events_t;

static zst_pad_probe_return_t capture(zst_pad_t *pad, zst_buffer_t *buf,
                                      zst_pad_probe_type_t type, void *opaque)
{
    capture_t *c = opaque;
    (void)pad; (void)type;
    assert(c->count < 8 && buf->memory.size <= sizeof(c->data[0]));
    c->sizes[c->count] = buf->memory.size;
    c->flags[c->count] = buf->flags;
    memcpy(c->data[c->count], buf->memory.data, buf->memory.size);
    c->count++;
    return ZST_PAD_PROBE_OK;
}

static zst_result_t capture_event(zst_element_t *el, zst_pad_t *pad, zst_pad_event_t *event)
{
    events_t *events = el->priv;
    (void)pad;
    if (event->type == ZST_PAD_EVENT_CAPS) events->caps++;
    if (event->type == ZST_PAD_EVENT_EOS) events->eos++;
    if (event->type == ZST_PAD_EVENT_DISCONT) events->discont++;
    return ZST_OK;
}

static zst_element_t *event_sink_create(void)
{
    static zst_element_ops_t ops = { .name = "event-capture", .event = capture_event };
    events_t *events = calloc(1, sizeof(*events));
    zst_element_t *sink = zst_element_create(&ops, events);
    zst_pad_t *pad = zst_pad_create("sink", ZST_PAD_SINK);
    assert(events && sink && pad && zst_element_add_pad(sink, pad) == ZST_OK);
    return sink;
}

static zst_buffer_t *input(const uint8_t *data, size_t len, int eos)
{
    zst_buffer_t *b = zst_buffer_create(ZST_BUFFER_VIDEO_PACKET);
    assert(b);
    uint8_t *copy = malloc(len);
    assert(copy);
    memcpy(copy, data, len);
    b->memory.type = ZST_MEMORY_CPU;
    b->memory.data = copy;
    b->memory.size = len;
    b->memory.priv = copy;
    b->memory.release = free;
    if (eos) b->flags |= ZST_BUFFER_FLAG_EOS;
    return b;
}

static void test_annexb_split(void)
{
    static const uint8_t stream[] = { 0,0,0,1, 0x65,0xaa, 0,0,1, 0x41,0xbb };
    static const uint8_t expected0[] = { 0,0,0,1, 0x65,0xaa };
    static const uint8_t expected1[] = { 0,0,0,1, 0x41,0xbb };
    capture_t c = {0};
    zst_element_t *parse = zst_h26x_parser_create();
    zst_element_t *sink = zst_fake_sink_create();
    zst_pad_t *src = zst_element_get_pad(parse, "src");
    zst_pad_t *in = zst_element_get_pad(parse, "sink");
    assert(parse && sink && src && in);
    assert(zst_element_set_property_string(parse, "codec", "h264") == ZST_OK);
    assert(zst_pad_link(src, zst_element_get_pad(sink, "sink")) == ZST_OK);
    assert(zst_pad_add_probe(src, ZST_PAD_PROBE_PRE_BUFFER, capture, &c));
    for (size_t i = 0; i < sizeof(stream); i++) { zst_buffer_t *b = input(stream + i, 1, i + 1 == sizeof(stream)); assert(in->push(in, b) == ZST_OK); zst_buffer_unref(b); }
    assert(c.count == 2);
    assert(c.sizes[0] == sizeof(expected0) && !memcmp(c.data[0], expected0, sizeof(expected0)));
    assert(c.sizes[1] == sizeof(expected1) && !memcmp(c.data[1], expected1, sizeof(expected1)));
    zst_element_destroy(parse); zst_element_destroy(sink);
}

static void test_length_prefixed_h265(void)
{
    static const uint8_t stream[] = { 0,0,0,2, 0x26,0x01, 0,0,0,2, 0x02,0x01 };
    capture_t c = {0};
    zst_element_t *parse = zst_h26x_parser_create();
    zst_element_t *sink = zst_fake_sink_create();
    zst_pad_t *src = zst_element_get_pad(parse, "src");
    zst_pad_t *in = zst_element_get_pad(parse, "sink");
    assert(parse && sink && src && in);
    assert(zst_element_set_property_string(parse, "codec", "h265") == ZST_OK);
    assert(zst_element_set_property_string(parse, "input-stream-format", "hvc1") == ZST_OK);
    assert(zst_pad_link(src, zst_element_get_pad(sink, "sink")) == ZST_OK);
    assert(zst_pad_add_probe(src, ZST_PAD_PROBE_PRE_BUFFER, capture, &c));
    zst_buffer_t *b = input(stream, sizeof(stream), 1);
    assert(in->push(in, b) == ZST_OK); zst_buffer_unref(b);
    assert(c.count == 2 && c.sizes[0] == 6 && c.sizes[1] == 6);
    assert(!memcmp(c.data[0], "\0\0\0\1\x26\x01", 6));
    zst_element_destroy(parse); zst_element_destroy(sink);
}

static void test_h264_flags_and_drop(void)
{
    static const uint8_t first[] = { 0,0,1, 0x65,0xaa, 0,0,1 };
    static const uint8_t second[] = { 0,0,1, 0x65,0xcc };
    capture_t c = {0};
    zst_element_t *parse = zst_h26x_parser_create();
    zst_element_t *sink = zst_fake_sink_create();
    zst_pad_t *src = zst_element_get_pad(parse, "src");
    zst_pad_t *in = zst_element_get_pad(parse, "sink");
    zst_buffer_t *b;
    assert(parse && sink && src && in);
    assert(zst_element_set_property_string(parse, "codec", "h264") == ZST_OK);
    assert(zst_element_set_property_string(parse, "aggregate-au", "true") == ZST_OK);
    assert(zst_pad_link(src, zst_element_get_pad(sink, "sink")) == ZST_OK);
    assert(zst_pad_add_probe(src, ZST_PAD_PROBE_PRE_BUFFER, capture, &c));
    b = input(first, sizeof(first), 0); assert(in->push(in, b) == ZST_OK); zst_buffer_unref(b);
    b = input((const uint8_t *)"x", 1, 0); b->flags |= ZST_BUFFER_FLAG_DROP;
    assert(in->push(in, b) == ZST_OK); zst_buffer_unref(b);
    b = input(second, sizeof(second), 1); assert(in->push(in, b) == ZST_OK); zst_buffer_unref(b);
    assert(c.count == 2);
    assert(c.flags[0] & ZST_BUFFER_FLAG_DROP);
    assert(c.sizes[1] == 6 && !memcmp(c.data[1], "\0\0\0\1\x65\xcc", 6));
    assert(c.flags[1] & ZST_BUFFER_FLAG_EOS);
    zst_element_destroy(parse); zst_element_destroy(sink);
}

static void test_h264_discont_eos_and_caps(void)
{
    static const uint8_t config_and_frame[] = {
        0,0,1, 0x67,0x42,0xc0,0x1e,0xd9,0x01,0x40,0x7b,0x20,
        0,0,1, 0x68,0xce,0x06,0xe2, 0,0,1, 0x65,0xaa,
        0,0,1, 0x65,0xcc, 0,0,1
    };
    static const uint8_t partial[] = { 0,0,1, 0x65,0xaa, 0,0,1 };
    static const uint8_t final[] = { 0,0,1, 0x65,0xcc };
    capture_t c = {0};
    zst_element_t *parse = zst_h26x_parser_create();
    zst_element_t *sink = event_sink_create();
    events_t *events = sink->priv;
    zst_pad_t *src = zst_element_get_pad(parse, "src");
    zst_pad_t *in = zst_element_get_pad(parse, "sink");
    zst_pad_event_t *event;
    zst_buffer_t *b;
    assert(parse && sink && src && in);
    assert(zst_element_set_property_string(parse, "codec", "h264") == ZST_OK);
    assert(zst_element_set_property_string(parse, "aggregate-au", "true") == ZST_OK);
    assert(zst_pad_link(src, zst_element_get_pad(sink, "sink")) == ZST_OK);
    assert(zst_pad_add_probe(src, ZST_PAD_PROBE_PRE_BUFFER, capture, &c));
    b = input(config_and_frame, sizeof(config_and_frame), 0); assert(in->push(in, b) == ZST_OK); zst_buffer_unref(b);
    assert(events->caps == 1);
    b = input(partial, sizeof(partial), 0); assert(in->push(in, b) == ZST_OK); zst_buffer_unref(b);
    event = zst_pad_event_new_discont(); assert(event);
    assert(parse->ops->event(parse, in, event) == ZST_OK); zst_pad_event_unref(event);
    b = input(final, sizeof(final), 0); assert(in->push(in, b) == ZST_OK); zst_buffer_unref(b);
    event = zst_pad_event_new_eos(); assert(event);
    assert(parse->ops->event(parse, in, event) == ZST_OK); zst_pad_event_unref(event);
    assert(events->caps == 1 && events->discont == 1 && events->eos == 1);
    assert(c.count == 3 && c.sizes[2] == 6 && !memcmp(c.data[2], "\0\0\0\1\x65\xcc", 6));
    assert(c.flags[2] & ZST_BUFFER_FLAG_EOS);
    zst_element_destroy(parse); zst_element_destroy(sink);
}

static void test_h264_caps_require_sps_and_pps(void)
{
    static const uint8_t stream[] = {
        0,0,1, 0x67,0x42,0xc0,0x1e,0xd9,0x01,0x40,0x7b,0x20,
        0,0,1, 0x68,0xce,0x06,0xe2, 0,0,1, 0x65,0xaa
    };
    zst_element_t *parse = zst_h26x_parser_create();
    zst_element_t *sink = event_sink_create();
    events_t *events = sink->priv;
    zst_pad_t *src = zst_element_get_pad(parse, "src");
    zst_pad_t *in = zst_element_get_pad(parse, "sink");
    zst_buffer_t *b;
    assert(parse && sink && src && in);
    assert(zst_element_set_property_string(parse, "codec", "h264") == ZST_OK);
    assert(zst_pad_link(src, zst_element_get_pad(sink, "sink")) == ZST_OK);
    b = input(stream, sizeof(stream), 1); assert(in->push(in, b) == ZST_OK); zst_buffer_unref(b);
    assert(events->caps == 1);
    zst_element_destroy(parse); zst_element_destroy(sink);
}

static void test_h264_caps_follow_slice_pps(void)
{
    static const uint8_t announced[] = {
        0,0,1, 0x67,0x42,0xc0,0x1e,0xd9,0x01,0x40,0x7b,0x20,
        0,0,1, 0x68,0xce,0x06,0xe2,
        0,0,1, 0x67,0x42,0xc0,0x1e,0x56,0x40,0x50,0x1e,0xc8,
        0,0,1, 0x68,0x48, 0,0,1
    };
    static const uint8_t vcl0[] = { 0,0,1, 0x65,0xaa, 0,0,1 };
    static const uint8_t vcl1[] = { 0,0,1, 0x65,0xd4 };
    static const uint8_t expected0[] = { 0,0,0,1, 0x67,0x42,0xc0,0x1e,0xd9,0x01,0x40,0x7b,0x20, 0,0,0,1, 0x68,0xce,0x06,0xe2 };
    static const uint8_t expected1[] = { 0,0,0,1, 0x67,0x42,0xc0,0x1e,0x56,0x40,0x50,0x1e,0xc8, 0,0,0,1, 0x68,0x48 };
    zst_element_t *parse = zst_h26x_parser_create();
    zst_element_t *sink = event_sink_create();
    events_t *events = sink->priv;
    zst_pad_t *src = zst_element_get_pad(parse, "src");
    zst_pad_t *in = zst_element_get_pad(parse, "sink");
    zst_buffer_t *b; zst_caps_t *caps; const void *codec_data, *avcc;
    size_t codec_data_len, avcc_len;
    assert(parse && sink && src && in);
    assert(zst_element_set_property_string(parse, "codec", "h264") == ZST_OK);
    assert(zst_pad_link(src, zst_element_get_pad(sink, "sink")) == ZST_OK);
    b = input(announced, sizeof(announced), 0); assert(in->push(in, b) == ZST_OK); zst_buffer_unref(b);
    assert(events->caps == 0);
    b = input(vcl0, sizeof(vcl0), 0); assert(in->push(in, b) == ZST_OK); zst_buffer_unref(b);
    assert(events->caps == 1);
    caps = zst_pad_get_caps(src); assert(caps && zst_caps_get_buffer(caps, "codec_data", &codec_data, &codec_data_len) == ZST_OK);
    assert(codec_data_len == sizeof(expected0) && !memcmp(codec_data, expected0, sizeof(expected0))); zst_caps_destroy(caps);
    caps = zst_pad_get_caps(src);
    assert(caps && zst_caps_get_buffer(caps, ZST_H26X_PARSER_CAPS_AVCC, &avcc, &avcc_len) == ZST_OK);
    assert(avcc_len == 24);
    assert(((const uint8_t*)avcc)[0] == 1 && ((const uint8_t*)avcc)[1] == 0x42);
    assert(((const uint8_t*)avcc)[2] == 0xc0 && ((const uint8_t*)avcc)[3] == 0x1e);
    assert(((const uint8_t*)avcc)[4] == 0xff && ((const uint8_t*)avcc)[5] == 0xe1);
    assert(((const uint8_t*)avcc)[6] == 0 && ((const uint8_t*)avcc)[7] == 9);
    assert(((const uint8_t*)avcc)[8] == 0x67 && ((const uint8_t*)avcc)[17] == 1);
    assert(((const uint8_t*)avcc)[18] == 0 && ((const uint8_t*)avcc)[19] == 4);
    assert(((const uint8_t*)avcc)[20] == 0x68);
    zst_caps_destroy(caps);
    b = input(vcl0, sizeof(vcl0), 0); assert(in->push(in, b) == ZST_OK); zst_buffer_unref(b);
    assert(events->caps == 1);
    b = input(vcl1, sizeof(vcl1), 1); assert(in->push(in, b) == ZST_OK); zst_buffer_unref(b);
    assert(events->caps == 2);
    caps = zst_pad_get_caps(src); assert(caps && zst_caps_get_buffer(caps, "codec_data", &codec_data, &codec_data_len) == ZST_OK);
    assert(codec_data_len == sizeof(expected1));
    assert(!memcmp(codec_data, expected1, sizeof(expected1))); zst_caps_destroy(caps);
    zst_element_destroy(parse); zst_element_destroy(sink);
}

int main(void) { test_annexb_split(); test_length_prefixed_h265(); test_h264_flags_and_drop(); test_h264_discont_eos_and_caps(); test_h264_caps_require_sps_and_pps(); test_h264_caps_follow_slice_pps(); return 0; }
