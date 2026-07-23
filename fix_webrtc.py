import re

with open('src/webrtc_endpoint.c', 'r') as f:
    content = f.read()

# Fix webrtc_sink_push
content = re.sub(
    r'    webrtc_endpoint_t\* s = el->priv;\n\n#ifdef HAS_WEBRTC\n',
    r'#ifdef HAS_WEBRTC\n    webrtc_endpoint_t* s = el->priv;\n\n',
    content
)

# Replace remaining occurrences
funcs_to_fix = [
    'zst_webrtc_create_offer',
    'zst_webrtc_restart_ice',
    'zst_webrtc_add_ice_candidate',
    'zst_webrtc_create_data_channel',
    'zst_webrtc_add_video_track',
    'zst_webrtc_add_video_track_with_pt',
    'zst_webrtc_add_audio_track',
    'zst_webrtc_send_media',
    'zst_webrtc_send_data',
    'zst_webrtc_request_keyframe',
    'zst_webrtc_request_bitrate'
]

# For functions where it is:
#     webrtc_endpoint_t* s = el->priv;
#
# #ifdef HAS_WEBRTC
content = re.sub(
    r'    webrtc_endpoint_t\* s = el->priv;\n\n#ifdef HAS_WEBRTC\n',
    r'#ifdef HAS_WEBRTC\n    webrtc_endpoint_t* s = el->priv;\n\n',
    content
)

content = re.sub(
    r'    webrtc_endpoint_t\* s = el->priv;\n#ifdef HAS_WEBRTC\n',
    r'#ifdef HAS_WEBRTC\n    webrtc_endpoint_t* s = el->priv;\n',
    content
)

# Remove (void)s; in #else blocks
content = re.sub(
    r'#else\n    \(void\)s;\n',
    r'#else\n',
    content
)

content = re.sub(
    r'#else\n    \(void\)s; ',
    r'#else\n    ',
    content
)

with open('src/webrtc_endpoint.c', 'w') as f:
    f.write(content)
