#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

void parse_urls_realloc(const char* value) {
    char* buf = strdup(value);

    uint32_t turn_cap = 4;
    char** turn_urls = malloc(turn_cap * sizeof(char*));
    uint32_t num_turn = 0;

    char* saveptr = NULL;
    char* token = strtok_r(buf, ",", &saveptr);
    while (token) {
        while (*token == ' ' || *token == '\t') token++;

        if (num_turn >= turn_cap) {
            turn_cap *= 2;
            turn_urls = realloc(turn_urls, turn_cap * sizeof(char*));
        }
        turn_urls[num_turn++] = strdup(token);

        token = strtok_r(NULL, ",", &saveptr);
    }

    for (uint32_t i = 0; i < num_turn; i++) free(turn_urls[i]);
    free(turn_urls);
    free(buf);
}

void parse_urls_original(const char* value) {
    uint32_t count = 1;
    for (const char* p = value; *p; p++) {
        if (*p == ',') count++;
    }
    char** turn_urls = calloc(count, sizeof(char*));
    char* buf = strdup(value);

    uint32_t idx = 0;
    char* saveptr = NULL;
    char* token = strtok_r(buf, ",", &saveptr);
    while (token && idx < count) {
        while (*token == ' ' || *token == '\t') token++;
        turn_urls[idx++] = strdup(token);
        token = strtok_r(NULL, ",", &saveptr);
    }
    for (uint32_t i = 0; i < idx; i++) free(turn_urls[i]);
    free(turn_urls);
    free(buf);
}

int main() {
    const char* value = "turn:turn1.example.com:3478, turn:turn2.example.com:3478, turn:turn3.example.com:3478, turn:turn4.example.com:3478, turn:turn5.example.com:3478, turn:turn6.example.com:3478, turn:turn7.example.com:3478";
    int iterations = 1000000;

    clock_t start = clock();
    for (int i=0; i<iterations; i++) parse_urls_original(value);
    clock_t end = clock();
    printf("Original: %f ms\n", (double)(end - start) * 1000.0 / CLOCKS_PER_SEC);

    start = clock();
    for (int i=0; i<iterations; i++) parse_urls_realloc(value);
    end = clock();
    printf("Realloc: %f ms\n", (double)(end - start) * 1000.0 / CLOCKS_PER_SEC);

    return 0;
}
