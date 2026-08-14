/*=============================================================================
    srt_plugin.c — Unified dynamic plugin for SRT source and sink
=============================================================================*/
#include "zst_plugin.h"
#include <string.h>
#include <stdlib.h>

zst_element_t* zst_srt_source_create(void);
zst_element_t* zst_srt_sink_create(void);

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "srtsrc") == 0) {
        return zst_srt_source_create();
    }
    if (strcmp(name, "srtsink") == 0) {
        return zst_srt_sink_create();
    }
    return NULL;
}

static const zst_property_spec_t g_srtsrc_properties[] = {
    { "uri", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "SRT Connection URI" },
    { "host", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "127.0.0.1", "SRT peer host (caller/rendezvous modes)" },
    { "port", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "9000", "SRT port" },
    { "mode", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "caller", "SRT connection mode (caller, listener, rendezvous)" },
    { "latency", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "120", "SRT latency in milliseconds" },
    { "passphrase", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "SRT AES encryption passphrase" },
    { "pbkeylen", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "16", "SRT AES key length (16, 24, 32)" },
    { "streamid", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "SRT stream ID" },
    { "payload-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1316", "SRT packet payload size" },
    { "lossmaxttl", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Maximum possible packet reorder tolerance (-1 for default)" },
    { "mininputbw", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Minimum estimate of input stream rate (-1 for default)" },
    { "snddropdelay", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-2", "Extra delay towards latency for sender TLPKTDROP decision (-2 for default, -1 for off)" },
    { "sndtimeo", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "SRT send timeout in milliseconds (-1 for default)" },
    { "rcvtimeo", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "SRT receive timeout in milliseconds (-1 for default)" },
    { "ipv6only", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "SRT IPv6 only mode (-1 for default)" }
};

static const zst_property_spec_t g_srtsink_properties[] = {
    { "uri", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "SRT Destination URI" },
    { "host", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "127.0.0.1", "SRT peer host (caller/rendezvous modes)" },
    { "port", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "9000", "SRT port" },
    { "mode", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "caller", "SRT connection mode (caller, listener, rendezvous)" },
    { "latency", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "120", "SRT latency in milliseconds" },
    { "passphrase", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "SRT AES encryption passphrase" },
    { "pbkeylen", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "16", "SRT AES key length (16, 24, 32)" },
    { "streamid", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "SRT stream ID" },
    { "payload-size", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "1316", "SRT packet payload size" },
    { "lossmaxttl", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Maximum possible packet reorder tolerance (-1 for default)" },
    { "mininputbw", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "Minimum estimate of input stream rate (-1 for default)" },
    { "snddropdelay", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-2", "Extra delay towards latency for sender TLPKTDROP decision (-2 for default, -1 for off)" },
    { "sndtimeo", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "SRT send timeout in milliseconds (-1 for default)" },
    { "rcvtimeo", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "SRT receive timeout in milliseconds (-1 for default)" },
    { "ipv6only", ZST_PROPERTY_INT, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "-1", "SRT IPv6 only mode (-1 for default)" }
};

static const zst_pad_template_t g_srtsrc_pads[] = {
    { "src", ZST_PAD_SRC, ZST_PAD_ALWAYS, "ANY" }
};

static const zst_pad_template_t g_srtsink_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "ANY" }
};

static const zst_element_desc_t g_srt_elements[] = {
    {
        .name = "srtsrc",
        .long_name = "SRT Source",
        .category = "Source/Network",
        .description = "Receives buffers over Secure Reliable Transport (SRT)",
        .author = "zstreamer",
        .properties = g_srtsrc_properties,
        .nb_properties = sizeof(g_srtsrc_properties) / sizeof(g_srtsrc_properties[0]),
        .pads = g_srtsrc_pads,
        .nb_pads = sizeof(g_srtsrc_pads) / sizeof(g_srtsrc_pads[0]),
        .create = NULL
    },
    {
        .name = "srtsink",
        .long_name = "SRT Sink",
        .category = "Sink/Network",
        .description = "Sends buffers over Secure Reliable Transport (SRT)",
        .author = "zstreamer",
        .properties = g_srtsink_properties,
        .nb_properties = sizeof(g_srtsink_properties) / sizeof(g_srtsink_properties[0]),
        .pads = g_srtsink_pads,
        .nb_pads = sizeof(g_srtsink_pads) / sizeof(g_srtsink_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "srt_plugin",
        .author = "zstreamer",
        .version = "1.0.0",
        .init = NULL,
        .deinit = NULL
    },
    .create_element = plugin_create_element
};

ZST_PLUGIN_EXPORT
const zst_element_desc_t*
zst_get_plugin_elements(uint32_t* nb_elements_out)
{
    if (nb_elements_out) {
        *nb_elements_out = sizeof(g_srt_elements) / sizeof(g_srt_elements[0]);
    }
    return g_srt_elements;
}

ZST_PLUGIN_EXPORT
zst_plugin_t*
zst_get_plugin(void)
{
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) {
        *p = g_plugin;
    }
    return p;
}
