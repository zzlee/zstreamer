/* AAC access-unit to ADTS framing element. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_element.h"
#include "zst_pad.h"
#include "zstreamer/elements/zst_aac_parser.h"

typedef struct {
    zst_pad_t* sink;
    zst_pad_t* src;
    unsigned profile;
    unsigned frequency_index;
    unsigned channels;
} aac_parser_t;

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_audio_specific_config(aac_parser_t* s, const char* hex)
{
    if (!hex || strlen(hex) < 4 || (strlen(hex) & 1)) return 0;
    int h0 = hex_value(hex[0]), l0 = hex_value(hex[1]);
    int h1 = hex_value(hex[2]), l1 = hex_value(hex[3]);
    if (h0 < 0 || l0 < 0 || h1 < 0 || l1 < 0) return 0;

    unsigned byte0 = (unsigned)((h0 << 4) | l0);
    unsigned byte1 = (unsigned)((h1 << 4) | l1);
    unsigned object_type = byte0 >> 3;
    unsigned frequency_index = ((byte0 & 7u) << 1) | (byte1 >> 7);
    unsigned channels = (byte1 >> 3) & 15u;
    if (object_type < 1 || object_type > 4 || frequency_index > 12 ||
        channels < 1 || channels > 7)
        return 0;

    s->profile = object_type - 1;
    s->frequency_index = frequency_index;
    s->channels = channels;
    return 1;
}

static zst_result_t aac_parser_set_property(zst_element_t* el,
                                             const char* name,
                                             const char* value)
{
    if (!el || !el->priv || !name || strcmp(name, ZST_AAC_PARSER_PROP_CONFIG) != 0)
        return ZST_ERROR;
    return parse_audio_specific_config(el->priv, value) ? ZST_OK : ZST_ERROR;
}

static zst_result_t aac_parser_process(zst_element_t* el,
                                       zst_buffer_t* in,
                                       zst_buffer_t** out)
{
    aac_parser_t* s = el ? el->priv : NULL;
    if (out) *out = NULL;
    if (!s || !in || !in->memory.data || in->memory.size == 0) return ZST_ERROR;

    /* rtspsrc puts the SDP fmtp config on the connected audio pad after
     * DESCRIBE. Resolve it lazily because pad linking occurs before DESCRIBE. */
    if (s->channels == 0) {
        zst_caps_t* caps = zst_pad_get_caps(s->sink);
        const char* config = NULL;
        if (caps) zst_caps_get_string(caps, "config", &config);
        int valid = config && parse_audio_specific_config(s, config);
        zst_caps_destroy(caps);
        if (!valid) return ZST_ERROR;
    }

    const uint8_t* src = in->memory.data;
    size_t size = in->memory.size;
    /* Avoid double-wrapping when the input is already an ADTS frame. */
    int already_adts = size >= 2 && src[0] == 0xff && (src[1] & 0xf6) == 0xf0;
    size_t header_size = already_adts ? 0 : 7;
    if (!already_adts && size > 8184) return ZST_ERROR;

    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_AUDIO_PACKET);
    if (!buf) return ZST_ERROR;
    uint8_t* data = malloc(size + header_size);
    if (!data) {
        zst_buffer_unref(buf);
        return ZST_ERROR;
    }

    if (already_adts) {
        memcpy(data, src, size);
    } else {
        unsigned frame_length = (unsigned)(size + header_size);
        data[0] = 0xff;
        data[1] = 0xf1; /* MPEG-4, no CRC */
        data[2] = (uint8_t)((s->profile << 6) | (s->frequency_index << 2) |
                            (s->channels >> 2));
        data[3] = (uint8_t)(((s->channels & 3) << 6) | (frame_length >> 11));
        data[4] = (uint8_t)(frame_length >> 3);
        data[5] = (uint8_t)(((frame_length & 7) << 5) | 0x1f);
        data[6] = 0xfc;
        memcpy(data + header_size, src, size);
    }

    buf->memory.data = data;
    buf->memory.size = size + header_size;
    buf->memory.priv = data;
    buf->memory.release = free;
    buf->pts = in->pts;
    buf->dts = in->dts;
    buf->duration = in->duration;
    buf->flags = in->flags;

    if (s->src->peer) {
        zst_result_t result = zst_pad_push(s->src, buf);
        zst_buffer_unref(buf);
        return result;
    }
    if (out) *out = buf;
    else zst_buffer_unref(buf);
    return ZST_OK;
}

static zst_result_t aac_parser_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !pad->parent) return ZST_ERROR;
    zst_buffer_t* out = NULL;
    zst_result_t result = aac_parser_process(pad->parent, buf, &out);
    if (out) zst_buffer_unref(out);
    return result;
}

static zst_caps_t* aac_parser_get_caps(zst_element_t* el,
                                       zst_pad_t* pad,
                                       const zst_caps_t* filter)
{
    (void)el;
    (void)pad;
    (void)filter;
    zst_caps_t* caps = zst_caps_create();
    if (caps) zst_caps_append(caps, zst_caps_struct_create_audio("audio/aac", 0, 0, ""));
    return caps;
}

zst_element_t* zst_aac_parser_create(void)
{
    static const zst_element_ops_t ops = {
        .name = "aacparse",
        .process = aac_parser_process,
        .get_caps = aac_parser_get_caps,
        .set_property = aac_parser_set_property,
    };
    aac_parser_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    zst_element_t* el = zst_element_create(&ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }
    priv->sink = zst_pad_create("sink", ZST_PAD_SINK);
    priv->src = zst_pad_create("src", ZST_PAD_SRC);
    if (!priv->sink || !priv->src ||
        zst_element_add_pad(el, priv->sink) != ZST_OK ||
        zst_element_add_pad(el, priv->src) != ZST_OK) {
        zst_element_destroy(el);
        return NULL;
    }
    priv->sink->push = aac_parser_sink_push;
    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t* plugin_create_element(const char* name)
{
    return name && strcmp(name, "aacparse") == 0 ? zst_aac_parser_create() : NULL;
}

static const zst_pad_template_t aac_parser_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "audio/aac" },
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "audio/aac" },
};

static const zst_element_desc_t aac_parser_elements[] = {{
    .name = "aacparse",
    .long_name = "AAC Parser",
    .category = "Codec/Parser",
    .description = "Adds ADTS framing to MPEG-4 AAC access units",
    .author = "zstreamer",
    .properties = NULL,
    .nb_properties = 0,
    .pads = aac_parser_pads,
    .nb_pads = sizeof(aac_parser_pads) / sizeof(aac_parser_pads[0]),
    .create = NULL,
}};

static zst_plugin_t aac_parser_plugin = {
    .desc = {
        .name = "aacparser_plugin",
        .author = "zstreamer",
        .version = "1.0.0",
        .init = NULL,
        .deinit = NULL,
    },
    .create_element = plugin_create_element,
};

ZST_PLUGIN_EXPORT const zst_element_desc_t*
zst_get_plugin_elements(uint32_t* count)
{
    if (count) *count = sizeof(aac_parser_elements) / sizeof(aac_parser_elements[0]);
    return aac_parser_elements;
}

ZST_PLUGIN_EXPORT zst_plugin_t* zst_get_plugin(void)
{
    zst_plugin_t* plugin = malloc(sizeof(*plugin));
    if (plugin) memcpy(plugin, &aac_parser_plugin, sizeof(*plugin));
    return plugin;
}
#endif
