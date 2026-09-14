/* Deterministic framing tests; fixtures are deliberately syntax-light. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "zst_buffer.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zstreamer/elements/zst_fake_sink.h"
#include "zstreamer/elements/zst_h26x_parser.h"

typedef struct { unsigned count; size_t sizes[8]; uint8_t data[8][32]; } capture_t;

static zst_pad_probe_return_t capture(zst_pad_t *pad, zst_buffer_t *buf,
                                      zst_pad_probe_type_t type, void *opaque)
{
    capture_t *c = opaque;
    (void)pad; (void)type;
    assert(c->count < 8 && buf->memory.size <= sizeof(c->data[0]));
    c->sizes[c->count] = buf->memory.size;
    memcpy(c->data[c->count], buf->memory.data, buf->memory.size);
    c->count++;
    return ZST_PAD_PROBE_OK;
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

int main(void) { test_annexb_split(); test_length_prefixed_h265(); return 0; }
