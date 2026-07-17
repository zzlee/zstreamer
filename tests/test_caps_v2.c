#include "zst_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

int main()
{
    printf("Starting Caps v2 unit tests...\n");

    /* 1. Test creation and simple setters/getters */
    zst_caps_t* caps = zst_caps_new_simple("video/x-h264");
    assert(caps != NULL);
    assert(caps->structs != NULL);
    assert(strcmp(caps->structs->media_type, "video/x-h264") == 0);
    assert(caps->structs->type == ZST_CAPS_VIDEO);
    assert(zst_caps_is_fixed(caps));
    assert(caps->structs->nb_fields == 0);
    assert(zst_caps_fixate(caps) == ZST_OK);
    assert(caps->structs->nb_fields == 0);

    /* Set and get integer (width / height) and test legacy sync */
    assert(zst_caps_set_int(caps, "width", 1920) == ZST_OK);
    assert(zst_caps_set_int(caps, "height", 1080) == ZST_OK);
    assert(caps->structs->video.width == 1920);
    assert(caps->structs->video.height == 1080);

    int w = 0, h = 0;
    assert(zst_caps_get_int(caps, "width", &w) == ZST_OK);
    assert(zst_caps_get_int(caps, "height", &h) == ZST_OK);
    assert(w == 1920);
    assert(h == 1080);

    /* Set and get string and test legacy format sync */
    assert(zst_caps_set_string(caps, "pixel-format", "I420") == ZST_OK);
    assert(strcmp(caps->structs->video.pixel_format, "I420") == 0);

    const char* fmt = NULL;
    assert(zst_caps_get_string(caps, "pixel-format", &fmt) == ZST_OK);
    assert(strcmp(fmt, "I420") == 0);

    /* Set and get profile (generic metadata) */
    assert(zst_caps_set_string(caps, "profile", "high") == ZST_OK);
    const char* profile = NULL;
    assert(zst_caps_get_string(caps, "profile", &profile) == ZST_OK);
    assert(strcmp(profile, "high") == 0);

    /* Set and get uint */
    assert(zst_caps_set_uint(caps, "bitrate", 5000000) == ZST_OK);
    uint32_t uint_val = 0;
    assert(zst_caps_get_uint(caps, "bitrate", &uint_val) == ZST_OK);
    assert(uint_val == 5000000);

    /* Set and get buffer (codec_data / extradata) */
    char dummy_codec_data[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42};
    assert(zst_caps_set_buffer(caps, "codec_data", dummy_codec_data, sizeof(dummy_codec_data)) == ZST_OK);

    const void* buf_data = NULL;
    size_t buf_size = 0;
    assert(zst_caps_get_buffer(caps, "codec_data", &buf_data, &buf_size) == ZST_OK);
    assert(buf_size == sizeof(dummy_codec_data));
    assert(memcmp(buf_data, dummy_codec_data, buf_size) == 0);

    /* Set and get double (framerate) */
    assert(zst_caps_set_double(caps, "framerate", 60.0) == ZST_OK);
    assert(caps->structs->video.framerate == 60.0);
    double fr = 0.0;
    assert(zst_caps_get_double(caps, "framerate", &fr) == ZST_OK);
    assert(fr == 60.0);

    /* Set and get fraction */
    assert(zst_caps_set_fraction(caps, "aspect-ratio", 16, 9) == ZST_OK);
    int num = 0, denom = 0;
    assert(zst_caps_get_fraction(caps, "aspect-ratio", &num, &denom) == ZST_OK);
    assert(num == 16 && denom == 9);

    /* 2. Test Deep Copy */
    zst_caps_t* caps_copy = zst_caps_copy(caps);
    assert(caps_copy != NULL);
    assert(caps_copy->structs->video.width == 1920);
    
    int w_copy = 0;
    assert(zst_caps_get_int(caps_copy, "width", &w_copy) == ZST_OK);
    assert(w_copy == 1920);

    const char* profile_copy = NULL;
    assert(zst_caps_get_string(caps_copy, "profile", &profile_copy) == ZST_OK);
    assert(strcmp(profile_copy, "high") == 0);

    const void* buf_data_copy = NULL;
    size_t buf_size_copy = 0;
    assert(zst_caps_get_buffer(caps_copy, "codec_data", &buf_data_copy, &buf_size_copy) == ZST_OK);
    assert(buf_size_copy == sizeof(dummy_codec_data));
    assert(memcmp(buf_data_copy, dummy_codec_data, buf_size_copy) == 0);

    /* 3. Test Intersection with generic fields */
    zst_caps_t* caps1 = zst_caps_new_simple("video/x-h264");
    zst_caps_set_int(caps1, "width", 1920);
    zst_caps_set_string(caps1, "profile", "high");

    zst_caps_t* caps2 = zst_caps_new_simple("video/x-h264");
    zst_caps_set_int(caps2, "width", 1920);
    zst_caps_set_string(caps2, "profile", "high");

    zst_caps_t* intersect_ok = zst_caps_intersect(caps1, caps2);
    assert(intersect_ok != NULL);
    assert(intersect_ok->structs != NULL);
    int int_w = 0;
    assert(zst_caps_get_int(intersect_ok, "width", &int_w) == ZST_OK);
    assert(int_w == 1920);
    const char* int_profile = NULL;
    assert(zst_caps_get_string(intersect_ok, "profile", &int_profile) == ZST_OK);
    assert(strcmp(int_profile, "high") == 0);

    /* Incompatible profiles should fail to intersect */
    zst_caps_set_string(caps2, "profile", "baseline");
    zst_caps_t* intersect_fail = zst_caps_intersect(caps1, caps2);
    assert(intersect_fail != NULL);
    assert(intersect_fail->structs == NULL); // Empty caps

    /* 4. Test Fixation synchronization */
    zst_caps_t* encoded = zst_caps_new_simple("audio/aac");
    assert(encoded != NULL);
    assert(zst_caps_is_fixed(encoded));
    assert(zst_caps_fixate(encoded) == ZST_OK);
    assert(zst_caps_get_string(encoded, "format", &fmt) == ZST_ERROR);
    assert(zst_caps_get_int(encoded, "sample-rate", &w) == ZST_ERROR);

    zst_caps_t* caps_wildcard = zst_caps_new_simple("video/x-raw");
    /* width=0, height=0 are wildcards */
    assert(zst_caps_fixate(caps_wildcard) == ZST_OK);
    assert(caps_wildcard->structs->video.width == 640);
    int fix_w = 0;
    assert(zst_caps_get_int(caps_wildcard, "width", &fix_w) == ZST_OK);
    assert(fix_w == 640);

    /* Cleanups */
    zst_caps_destroy(caps);
    zst_caps_destroy(caps_copy);
    zst_caps_destroy(caps1);
    zst_caps_destroy(caps2);
    zst_caps_destroy(intersect_ok);
    zst_caps_destroy(intersect_fail);
    zst_caps_destroy(encoded);
    zst_caps_destroy(caps_wildcard);

    printf("Caps v2 unit tests PASSED!\n");
    return 0;
}
