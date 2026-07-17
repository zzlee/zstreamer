/*=============================================================================
    zst_caps.c — Capabilities structure implementation
=============================================================================*/
#include "zst_caps.h"
#include <stdlib.h>
#include <string.h>

zst_caps_t*
zst_caps_create(void)
{
    zst_caps_t* caps = calloc(1, sizeof(*caps));
    return caps;
}

void
zst_caps_destroy(zst_caps_t* caps)
{
    if (!caps) return;
    zst_caps_struct_t* curr = caps->structs;
    while (curr) {
        zst_caps_struct_t* next = curr->next;
        zst_caps_struct_free(curr);
        curr = next;
    }
    free(caps);
}

zst_caps_t*
zst_caps_copy(const zst_caps_t* caps)
{
    if (!caps) return NULL;
    zst_caps_t* copy = zst_caps_create();
    if (!copy) return NULL;
    
    zst_caps_struct_t* curr = caps->structs;
    zst_caps_struct_t* tail = NULL;
    while (curr) {
        zst_caps_struct_t* s_copy = zst_caps_struct_copy(curr);
        if (!s_copy) {
            zst_caps_destroy(copy);
            return NULL;
        }
        if (!copy->structs) {
            copy->structs = s_copy;
        } else {
            tail->next = s_copy;
        }
        tail = s_copy;
        curr = curr->next;
    }
    return copy;
}

static void caps_field_free_contents(zst_caps_field_t* f);
static zst_result_t caps_field_copy(zst_caps_field_t* dest, const zst_caps_field_t* src);

static int
caps_media_type_is_raw_video(const char* media_type)
{
    return media_type && strcmp(media_type, "video/x-raw") == 0;
}

static int
caps_media_type_is_raw_audio(const char* media_type)
{
    return media_type && strcmp(media_type, "audio/x-raw") == 0;
}

static int
caps_is_raw_video_struct(const zst_caps_struct_t* s)
{
    return s && s->type == ZST_CAPS_VIDEO && caps_media_type_is_raw_video(s->media_type);
}

static int
caps_is_raw_audio_struct(const zst_caps_struct_t* s)
{
    return s && s->type == ZST_CAPS_AUDIO && caps_media_type_is_raw_audio(s->media_type);
}

static int
caps_is_raw_legacy_field(const zst_caps_struct_t* s, const char* key)
{
    if (caps_is_raw_video_struct(s)) {
        return strcmp(key, "width") == 0 || strcmp(key, "height") == 0 ||
               strcmp(key, "framerate") == 0 || strcmp(key, "pixel-format") == 0;
    }
    if (caps_is_raw_audio_struct(s)) {
        return strcmp(key, "channels") == 0 || strcmp(key, "sample-rate") == 0 ||
               strcmp(key, "format") == 0;
    }
    return 0;
}

static zst_caps_field_t*
caps_struct_find_field(const zst_caps_struct_t* s, const char* key)
{
    if (!s || !s->fields) return NULL;
    for (uint32_t i = 0; i < s->nb_fields; i++) {
        if (strcmp(s->fields[i].key, key) == 0) {
            return &s->fields[i];
        }
    }
    return NULL;
}

static zst_result_t
caps_struct_set_field(zst_caps_struct_t* s, const zst_caps_field_t* field)
{
    if (!s || !field) return ZST_ERROR;

    zst_caps_field_t* existing = caps_struct_find_field(s, field->key);
    if (existing) {
        caps_field_free_contents(existing);
        return caps_field_copy(existing, field);
    }

    if (s->nb_fields >= s->fields_capacity) {
        uint32_t new_cap = s->fields_capacity == 0 ? 4 : s->fields_capacity * 2;
        zst_caps_field_t* new_fields = realloc(s->fields, new_cap * sizeof(zst_caps_field_t));
        if (!new_fields) return ZST_ERROR;
        s->fields = new_fields;
        s->fields_capacity = new_cap;
    }

    if (caps_field_copy(&s->fields[s->nb_fields], field) != ZST_OK) {
        return ZST_ERROR;
    }
    s->nb_fields++;
    return ZST_OK;
}

zst_caps_struct_t*
zst_caps_struct_create_video(
    const char* media_type,
    int width,
    int height,
    double framerate,
    const char* pixel_format)
{
    zst_caps_struct_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    if (media_type) {
        strncpy(s->media_type, media_type, sizeof(s->media_type) - 1);
    }
    s->type = ZST_CAPS_VIDEO;
    s->video.width = width;
    s->video.height = height;
    s->video.framerate = framerate;
    if (pixel_format) {
        strncpy(s->video.pixel_format, pixel_format, sizeof(s->video.pixel_format) - 1);
    }
    s->next = NULL;

    int is_raw = caps_media_type_is_raw_video(s->media_type);
    zst_caps_field_t f;
    f.type = ZST_CAPS_FIELD_INT;
    
    if (is_raw || width != 0) {
        strcpy(f.key, "width");
        f.value.i_val = width;
        caps_struct_set_field(s, &f);
    }

    if (is_raw || height != 0) {
        strcpy(f.key, "height");
        f.value.i_val = height;
        caps_struct_set_field(s, &f);
    }

    if (is_raw || framerate != 0.0) {
        f.type = ZST_CAPS_FIELD_DOUBLE;
        strcpy(f.key, "framerate");
        f.value.d_val = framerate;
        caps_struct_set_field(s, &f);
    }

    if (is_raw && pixel_format) {
        f.type = ZST_CAPS_FIELD_STRING;
        strcpy(f.key, "pixel-format");
        f.value.s_val = (char*)pixel_format;
        caps_struct_set_field(s, &f);
    }

    return s;
}

zst_caps_struct_t*
zst_caps_struct_create_audio(
    const char* media_type,
    int channels,
    int sample_rate,
    const char* format)
{
    zst_caps_struct_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    if (media_type) {
        strncpy(s->media_type, media_type, sizeof(s->media_type) - 1);
    }
    s->type = ZST_CAPS_AUDIO;
    s->audio.channels = channels;
    s->audio.sample_rate = sample_rate;
    if (format) {
        strncpy(s->audio.format, format, sizeof(s->audio.format) - 1);
    }
    s->next = NULL;

    int is_raw = caps_media_type_is_raw_audio(s->media_type);
    zst_caps_field_t f;
    f.type = ZST_CAPS_FIELD_INT;

    if (is_raw || channels != 0) {
        strcpy(f.key, "channels");
        f.value.i_val = channels;
        caps_struct_set_field(s, &f);
    }

    if (is_raw || sample_rate != 0) {
        strcpy(f.key, "sample-rate");
        f.value.i_val = sample_rate;
        caps_struct_set_field(s, &f);
    }

    if (is_raw && format) {
        f.type = ZST_CAPS_FIELD_STRING;
        strcpy(f.key, "format");
        f.value.s_val = (char*)format;
        caps_struct_set_field(s, &f);
    }

    return s;
}

static void
caps_field_free_contents(zst_caps_field_t* f)
{
    if (!f) return;
    if (f->type == ZST_CAPS_FIELD_STRING) {
        free(f->value.s_val);
        f->value.s_val = NULL;
    } else if (f->type == ZST_CAPS_FIELD_BUFFER) {
        free(f->value.b_val.data);
        f->value.b_val.data = NULL;
    }
}

static zst_result_t
caps_field_copy(zst_caps_field_t* dest, const zst_caps_field_t* src)
{
    if (!dest || !src) return ZST_ERROR;
    strcpy(dest->key, src->key);
    dest->type = src->type;

    if (src->type == ZST_CAPS_FIELD_STRING) {
        dest->value.s_val = src->value.s_val ? strdup(src->value.s_val) : NULL;
    } else if (src->type == ZST_CAPS_FIELD_BUFFER) {
        if (src->value.b_val.data && src->value.b_val.size > 0) {
            dest->value.b_val.data = malloc(src->value.b_val.size);
            if (!dest->value.b_val.data) return ZST_ERROR;
            memcpy(dest->value.b_val.data, src->value.b_val.data, src->value.b_val.size);
            dest->value.b_val.size = src->value.b_val.size;
        } else {
            dest->value.b_val.data = NULL;
            dest->value.b_val.size = 0;
        }
    } else {
        dest->value = src->value;
    }
    return ZST_OK;
}

void
zst_caps_struct_free(zst_caps_struct_t* caps_struct)
{
    if (!caps_struct) return;
    if (caps_struct->fields) {
        for (uint32_t i = 0; i < caps_struct->nb_fields; i++) {
            caps_field_free_contents(&caps_struct->fields[i]);
        }
        free(caps_struct->fields);
    }
    free(caps_struct);
}

zst_caps_struct_t*
zst_caps_struct_copy(const zst_caps_struct_t* caps_struct)
{
    if (!caps_struct) return NULL;
    zst_caps_struct_t* copy = malloc(sizeof(*copy));
    if (!copy) return NULL;
    memcpy(copy, caps_struct, sizeof(*copy));
    copy->next = NULL;

    if (caps_struct->fields && caps_struct->nb_fields > 0) {
        copy->fields = malloc(caps_struct->nb_fields * sizeof(zst_caps_field_t));
        if (!copy->fields) {
            free(copy);
            return NULL;
        }
        copy->nb_fields = caps_struct->nb_fields;
        copy->fields_capacity = caps_struct->nb_fields;
        for (uint32_t i = 0; i < caps_struct->nb_fields; i++) {
            if (caps_field_copy(&copy->fields[i], &caps_struct->fields[i]) != ZST_OK) {
                for (uint32_t j = 0; j < i; j++) {
                    caps_field_free_contents(&copy->fields[j]);
                }
                free(copy->fields);
                free(copy);
                return NULL;
            }
        }
    } else {
        copy->fields = NULL;
        copy->nb_fields = 0;
        copy->fields_capacity = 0;
    }

    return copy;
}

zst_result_t
zst_caps_append(zst_caps_t* caps, zst_caps_struct_t* caps_struct)
{
    if (!caps || !caps_struct) return ZST_ERROR;
    caps_struct->next = NULL;
    if (!caps->structs) {
        caps->structs = caps_struct;
    } else {
        zst_caps_struct_t* curr = caps->structs;
        while (curr->next) {
            curr = curr->next;
        }
        curr->next = caps_struct;
    }
    return ZST_OK;
}

static bool
caps_field_equal(const zst_caps_field_t* f1, const zst_caps_field_t* f2)
{
    if (f1->type != f2->type) return false;
    switch (f1->type) {
        case ZST_CAPS_FIELD_INT:
            return f1->value.i_val == f2->value.i_val;
        case ZST_CAPS_FIELD_UINT:
            return f1->value.u_val == f2->value.u_val;
        case ZST_CAPS_FIELD_DOUBLE:
            return f1->value.d_val == f2->value.d_val;
        case ZST_CAPS_FIELD_STRING:
            if (!f1->value.s_val || !f2->value.s_val) return f1->value.s_val == f2->value.s_val;
            return strcmp(f1->value.s_val, f2->value.s_val) == 0;
        case ZST_CAPS_FIELD_FRACTION:
            return f1->value.f_val.num == f2->value.f_val.num &&
                   f1->value.f_val.denom == f2->value.f_val.denom;
        case ZST_CAPS_FIELD_BUFFER:
            if (f1->value.b_val.size != f2->value.b_val.size) return false;
            if (f1->value.b_val.size == 0) return true;
            if (!f1->value.b_val.data || !f2->value.b_val.data) return f1->value.b_val.data == f2->value.b_val.data;
            return memcmp(f1->value.b_val.data, f2->value.b_val.data, f1->value.b_val.size) == 0;
    }
    return false;
}

static zst_caps_struct_t*
zst_caps_struct_intersect(
    const zst_caps_struct_t* s1,
    const zst_caps_struct_t* s2)
{
    if (strcmp(s1->media_type, s2->media_type) != 0) {
        return NULL;
    }
    if (s1->type != s2->type) {
        return NULL;
    }
    
    zst_caps_struct_t* res = NULL;
    
    if (caps_is_raw_video_struct(s1)) {
        int w = 0, h = 0;
        double fr = 0.0;
        char fmt[32] = {0};
        
        // Intersect width
        if (s1->video.width != 0 && s2->video.width != 0) {
            if (s1->video.width != s2->video.width) return NULL;
            w = s1->video.width;
        } else {
            w = s1->video.width != 0 ? s1->video.width : s2->video.width;
        }
        
        // Intersect height
        if (s1->video.height != 0 && s2->video.height != 0) {
            if (s1->video.height != s2->video.height) return NULL;
            h = s1->video.height;
        } else {
            h = s1->video.height != 0 ? s1->video.height : s2->video.height;
        }
        
        // Intersect framerate
        if (s1->video.framerate != 0.0 && s2->video.framerate != 0.0) {
            if (s1->video.framerate != s2->video.framerate) return NULL;
            fr = s1->video.framerate;
        } else {
            fr = s1->video.framerate != 0.0 ? s1->video.framerate : s2->video.framerate;
        }
        
        // Intersect format
        if (s1->video.pixel_format[0] != '\0' && s2->video.pixel_format[0] != '\0') {
            if (strcmp(s1->video.pixel_format, s2->video.pixel_format) != 0) return NULL;
            strncpy(fmt, s1->video.pixel_format, sizeof(fmt) - 1);
        } else {
            strncpy(fmt, s1->video.pixel_format[0] != '\0' ? s1->video.pixel_format : s2->video.pixel_format, sizeof(fmt) - 1);
        }
        fmt[sizeof(fmt) - 1] = '\0';
        
        res = zst_caps_struct_create_video(s1->media_type, w, h, fr, fmt);
    } else if (caps_is_raw_audio_struct(s1)) {
        int ch = 0, rate = 0;
        char fmt[32] = {0};
        
        // Intersect channels
        if (s1->audio.channels != 0 && s2->audio.channels != 0) {
            if (s1->audio.channels != s2->audio.channels) return NULL;
            ch = s1->audio.channels;
        } else {
            ch = s1->audio.channels != 0 ? s1->audio.channels : s2->audio.channels;
        }
        
        // Intersect sample_rate
        if (s1->audio.sample_rate != 0 && s2->audio.sample_rate != 0) {
            if (s1->audio.sample_rate != s2->audio.sample_rate) return NULL;
            rate = s1->audio.sample_rate;
        } else {
            rate = s1->audio.sample_rate != 0 ? s1->audio.sample_rate : s2->audio.sample_rate;
        }
        
        // Intersect format
        if (s1->audio.format[0] != '\0' && s2->audio.format[0] != '\0') {
            if (strcmp(s1->audio.format, s2->audio.format) != 0) return NULL;
            strncpy(fmt, s1->audio.format, sizeof(fmt) - 1);
        } else {
            strncpy(fmt, s1->audio.format[0] != '\0' ? s1->audio.format : s2->audio.format, sizeof(fmt) - 1);
        }
        fmt[sizeof(fmt) - 1] = '\0';
        
        res = zst_caps_struct_create_audio(s1->media_type, ch, rate, fmt);
    } else {
        res = zst_caps_struct_copy(s1);
    }

    if (!res) return NULL;

    /* Intersect generic fields */
    if (s1->fields) {
        for (uint32_t i = 0; i < s1->nb_fields; i++) {
            const zst_caps_field_t* f1 = &s1->fields[i];
            
            if (caps_is_raw_legacy_field(s1, f1->key)) {
                continue;
            }

            const zst_caps_field_t* f2 = caps_struct_find_field(s2, f1->key);
            if (f2) {
                if (!caps_field_equal(f1, f2)) {
                    zst_caps_struct_free(res);
                    return NULL;
                }
                if (caps_struct_set_field(res, f1) != ZST_OK) {
                    zst_caps_struct_free(res);
                    return NULL;
                }
            } else {
                if (caps_struct_set_field(res, f1) != ZST_OK) {
                    zst_caps_struct_free(res);
                    return NULL;
                }
            }
        }
    }

    if (s2->fields) {
        for (uint32_t i = 0; i < s2->nb_fields; i++) {
            const zst_caps_field_t* f2 = &s2->fields[i];
            
            if (caps_is_raw_legacy_field(s2, f2->key)) {
                continue;
            }

            const zst_caps_field_t* f1 = caps_struct_find_field(s1, f2->key);
            if (!f1) {
                if (caps_struct_set_field(res, f2) != ZST_OK) {
                    zst_caps_struct_free(res);
                    return NULL;
                }
            }
        }
    }

    return res;
}

zst_caps_t*
zst_caps_intersect(const zst_caps_t* caps1, const zst_caps_t* caps2)
{
    if (!caps1 || !caps2) return NULL;
    zst_caps_t* result = zst_caps_create();
    if (!result) return NULL;
    
    for (zst_caps_struct_t* s1 = caps1->structs; s1 != NULL; s1 = s1->next) {
        for (zst_caps_struct_t* s2 = caps2->structs; s2 != NULL; s2 = s2->next) {
            zst_caps_struct_t* res_s = zst_caps_struct_intersect(s1, s2);
            if (res_s) {
                zst_caps_append(result, res_s);
            }
        }
    }
    return result;
}

int
zst_caps_struct_is_fixed(const zst_caps_struct_t* s)
{
    if (!s) return 0;
    if (s->type == ZST_CAPS_VIDEO) {
        if (!caps_is_raw_video_struct(s)) return s->media_type[0] != '\0';
        return s->video.width != 0 &&
               s->video.height != 0 &&
               s->video.framerate != 0.0 &&
               s->video.pixel_format[0] != '\0';
    } else if (s->type == ZST_CAPS_AUDIO) {
        if (!caps_is_raw_audio_struct(s)) return s->media_type[0] != '\0';
        return s->audio.channels != 0 &&
               s->audio.sample_rate != 0 &&
               s->audio.format[0] != '\0';
    }
    return s->media_type[0] != '\0';
}

int
zst_caps_is_fixed(const zst_caps_t* caps)
{
    if (!caps || !caps->structs) return 0;
    if (caps->structs->next != NULL) return 0;
    return zst_caps_struct_is_fixed(caps->structs);
}

zst_result_t
zst_caps_fixate(zst_caps_t* caps)
{
    if (!caps || !caps->structs) return ZST_ERROR;
    
    /* Keep only the first structure */
    zst_caps_struct_t* first = caps->structs;
    zst_caps_struct_t* curr = first->next;
    while (curr) {
        zst_caps_struct_t* next = curr->next;
        zst_caps_struct_free(curr);
        curr = next;
    }
    first->next = NULL;
    
    /* Resolve wildcards in the first structure */
    if (caps_is_raw_video_struct(first)) {
        if (first->video.width == 0) first->video.width = 640;
        if (first->video.height == 0) first->video.height = 480;
        if (first->video.framerate == 0.0) first->video.framerate = 30.0;
        if (first->video.pixel_format[0] == '\0') {
            strcpy(first->video.pixel_format, "YUV420P");
        }
        zst_caps_set_int(caps, "width", first->video.width);
        zst_caps_set_int(caps, "height", first->video.height);
        zst_caps_set_double(caps, "framerate", first->video.framerate);
        zst_caps_set_string(caps, "pixel-format", first->video.pixel_format);
    } else if (caps_is_raw_audio_struct(first)) {
        if (first->audio.channels == 0) first->audio.channels = 2;
        if (first->audio.sample_rate == 0) first->audio.sample_rate = 44100;
        if (first->audio.format[0] == '\0') {
            strcpy(first->audio.format, "S16LE");
        }
        zst_caps_set_int(caps, "channels", first->audio.channels);
        zst_caps_set_int(caps, "sample-rate", first->audio.sample_rate);
        zst_caps_set_string(caps, "format", first->audio.format);
    }
    
    return ZST_OK;
}

zst_caps_t*
zst_caps_new_simple(const char* media_type)
{
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    zst_caps_struct_t* s = calloc(1, sizeof(*s));
    if (!s) {
        zst_caps_destroy(caps);
        return NULL;
    }

    if (media_type) {
        strncpy(s->media_type, media_type, sizeof(s->media_type) - 1);
    }
    s->type = ZST_CAPS_ANY;
    if (media_type) {
        if (strncmp(media_type, "video/", 6) == 0) {
            s->type = ZST_CAPS_VIDEO;
        } else if (strncmp(media_type, "audio/", 6) == 0) {
            s->type = ZST_CAPS_AUDIO;
        }
    }
    s->next = NULL;

    zst_caps_append(caps, s);
    return caps;
}

zst_result_t
zst_caps_set_int(zst_caps_t* caps, const char* key, int value)
{
    if (!caps || !caps->structs) return ZST_ERROR;
    zst_caps_struct_t* s = caps->structs;

    if (s->type == ZST_CAPS_VIDEO) {
        if (strcmp(key, "width") == 0) s->video.width = value;
        else if (strcmp(key, "height") == 0) s->video.height = value;
    } else if (s->type == ZST_CAPS_AUDIO) {
        if (strcmp(key, "channels") == 0) s->audio.channels = value;
        else if (strcmp(key, "sample-rate") == 0 || strcmp(key, "sample_rate") == 0) s->audio.sample_rate = value;
    }

    zst_caps_field_t f;
    strncpy(f.key, key, sizeof(f.key) - 1);
    f.key[sizeof(f.key) - 1] = '\0';
    f.type = ZST_CAPS_FIELD_INT;
    f.value.i_val = value;
    return caps_struct_set_field(s, &f);
}

zst_result_t
zst_caps_set_uint(zst_caps_t* caps, const char* key, uint32_t value)
{
    if (!caps || !caps->structs) return ZST_ERROR;
    zst_caps_struct_t* s = caps->structs;

    zst_caps_field_t f;
    strncpy(f.key, key, sizeof(f.key) - 1);
    f.key[sizeof(f.key) - 1] = '\0';
    f.type = ZST_CAPS_FIELD_UINT;
    f.value.u_val = value;
    return caps_struct_set_field(s, &f);
}

zst_result_t
zst_caps_set_double(zst_caps_t* caps, const char* key, double value)
{
    if (!caps || !caps->structs) return ZST_ERROR;
    zst_caps_struct_t* s = caps->structs;

    if (s->type == ZST_CAPS_VIDEO && strcmp(key, "framerate") == 0) {
        s->video.framerate = value;
    }

    zst_caps_field_t f;
    strncpy(f.key, key, sizeof(f.key) - 1);
    f.key[sizeof(f.key) - 1] = '\0';
    f.type = ZST_CAPS_FIELD_DOUBLE;
    f.value.d_val = value;
    return caps_struct_set_field(s, &f);
}

zst_result_t
zst_caps_set_string(zst_caps_t* caps, const char* key, const char* value)
{
    if (!caps || !caps->structs) return ZST_ERROR;
    zst_caps_struct_t* s = caps->structs;

    if (s->type == ZST_CAPS_VIDEO) {
        if (strcmp(key, "pixel-format") == 0 || strcmp(key, "pixel_format") == 0) {
            if (s->video.pixel_format != value) {
                strncpy(s->video.pixel_format, value ? value : "", sizeof(s->video.pixel_format) - 1);
            }
        }
    } else if (s->type == ZST_CAPS_AUDIO) {
        if (strcmp(key, "format") == 0) {
            if (s->audio.format != value) {
                strncpy(s->audio.format, value ? value : "", sizeof(s->audio.format) - 1);
            }
        }
    }

    zst_caps_field_t f;
    strncpy(f.key, key, sizeof(f.key) - 1);
    f.key[sizeof(f.key) - 1] = '\0';
    f.type = ZST_CAPS_FIELD_STRING;
    f.value.s_val = (char*)value;
    return caps_struct_set_field(s, &f);
}

zst_result_t
zst_caps_set_fraction(zst_caps_t* caps, const char* key, int num, int denom)
{
    if (!caps || !caps->structs) return ZST_ERROR;
    zst_caps_struct_t* s = caps->structs;

    zst_caps_field_t f;
    strncpy(f.key, key, sizeof(f.key) - 1);
    f.key[sizeof(f.key) - 1] = '\0';
    f.type = ZST_CAPS_FIELD_FRACTION;
    f.value.f_val.num = num;
    f.value.f_val.denom = denom;
    return caps_struct_set_field(s, &f);
}

zst_result_t
zst_caps_set_buffer(zst_caps_t* caps, const char* key, const void* data, size_t size)
{
    if (!caps || !caps->structs) return ZST_ERROR;
    zst_caps_struct_t* s = caps->structs;

    zst_caps_field_t f;
    strncpy(f.key, key, sizeof(f.key) - 1);
    f.key[sizeof(f.key) - 1] = '\0';
    f.type = ZST_CAPS_FIELD_BUFFER;
    f.value.b_val.data = (void*)data;
    f.value.b_val.size = size;
    return caps_struct_set_field(s, &f);
}

zst_result_t
zst_caps_get_int(const zst_caps_t* caps, const char* key, int* value_out)
{
    if (!caps || !caps->structs || !value_out) return ZST_ERROR;
    const zst_caps_field_t* f = caps_struct_find_field(caps->structs, key);
    if (!f || f->type != ZST_CAPS_FIELD_INT) return ZST_ERROR;
    *value_out = f->value.i_val;
    return ZST_OK;
}

zst_result_t
zst_caps_get_uint(const zst_caps_t* caps, const char* key, uint32_t* value_out)
{
    if (!caps || !caps->structs || !value_out) return ZST_ERROR;
    const zst_caps_field_t* f = caps_struct_find_field(caps->structs, key);
    if (!f || f->type != ZST_CAPS_FIELD_UINT) return ZST_ERROR;
    *value_out = f->value.u_val;
    return ZST_OK;
}

zst_result_t
zst_caps_get_double(const zst_caps_t* caps, const char* key, double* value_out)
{
    if (!caps || !caps->structs || !value_out) return ZST_ERROR;
    const zst_caps_field_t* f = caps_struct_find_field(caps->structs, key);
    if (!f || f->type != ZST_CAPS_FIELD_DOUBLE) return ZST_ERROR;
    *value_out = f->value.d_val;
    return ZST_OK;
}

zst_result_t
zst_caps_get_string(const zst_caps_t* caps, const char* key, const char** value_out)
{
    if (!caps || !caps->structs || !value_out) return ZST_ERROR;
    const zst_caps_field_t* f = caps_struct_find_field(caps->structs, key);
    if (!f || f->type != ZST_CAPS_FIELD_STRING) return ZST_ERROR;
    *value_out = f->value.s_val;
    return ZST_OK;
}

zst_result_t
zst_caps_get_fraction(const zst_caps_t* caps, const char* key, int* num_out, int* denom_out)
{
    if (!caps || !caps->structs || !num_out || !denom_out) return ZST_ERROR;
    const zst_caps_field_t* f = caps_struct_find_field(caps->structs, key);
    if (!f || f->type != ZST_CAPS_FIELD_FRACTION) return ZST_ERROR;
    *num_out = f->value.f_val.num;
    *denom_out = f->value.f_val.denom;
    return ZST_OK;
}

zst_result_t
zst_caps_get_buffer(const zst_caps_t* caps, const char* key, const void** data_out, size_t* size_out)
{
    if (!caps || !caps->structs || !data_out || !size_out) return ZST_ERROR;
    const zst_caps_field_t* f = caps_struct_find_field(caps->structs, key);
    if (!f || f->type != ZST_CAPS_FIELD_BUFFER) return ZST_ERROR;
    *data_out = f->value.b_val.data;
    *size_out = f->value.b_val.size;
    return ZST_OK;
}
