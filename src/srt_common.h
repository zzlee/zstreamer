#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

void srt_global_init(void);
void srt_global_cleanup(void);

void srt_parse_uri_ext(const char* uri, char* host, size_t host_len, int* port,
                       char* mode, size_t mode_len, int* latency,
                       char* passphrase, size_t passphrase_len, int* pbkeylen,
                       char* streamid, size_t streamid_len, int* payload_size,
                       bool* tlpktdrop, int64_t* maxbw, int* rcvbuf, int* sndbuf,
                       int* peeridle, int* conntimeo, int* oheadbw,
                       int* ipttl, int* iptos, int* fc,
                       int* lossmaxttl, int64_t* mininputbw, int* snddropdelay,
                       char* bindtodevice, size_t bindtodevice_len,
                       char* congestion, size_t congestion_len,
                       char* packetfilter, size_t packetfilter_len);

void srt_parse_uri(const char* uri, char* host, size_t host_len, int* port,
                   char* mode, size_t mode_len, int* latency,
                   char* passphrase, size_t passphrase_len, int* pbkeylen,
                   char* streamid, size_t streamid_len, int* payload_size);
