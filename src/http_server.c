/*=============================================================================
    http_server.c — Built-in HTTP server for HLS
=============================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "zst_element.h"
#include "zst_log.h"
#include "zst_element_factory.h"

typedef struct {
    int port;
    char document_root[256];
    
    int server_fd;
    pthread_t thread;
    int running;
} http_server_t;

static const char* get_mime_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".m3u8") == 0) return "application/vnd.apple.mpegurl";
    if (strcmp(ext, ".m4s") == 0) return "video/mp4";
    if (strcmp(ext, ".mp4") == 0) return "video/mp4";
    if (strcmp(ext, ".ts") == 0) return "video/mp2t";
    if (strcmp(ext, ".html") == 0) return "text/html";
    return "application/octet-stream";
}

static void handle_client(int client_fd, http_server_t* s) {
    char buf[4096];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    buf[n] = '\0';
    
    char method[16], path[1024];
    if (sscanf(buf, "%15s %1023s", method, path) != 2) {
        close(client_fd);
        return;
    }
    
    if (strcmp(method, "GET") != 0) {
        const char* resp = "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n";
        send(client_fd, resp, strlen(resp), 0);
        close(client_fd);
        return;
    }
    
    // basic sanitization
    if (strstr(path, "..")) {
        const char* resp = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
        send(client_fd, resp, strlen(resp), 0);
        close(client_fd);
        return;
    }
    
    char filepath[2048];
    if (strcmp(path, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "%s/index.html", s->document_root);
    } else {
        snprintf(filepath, sizeof(filepath), "%s%s", s->document_root, path);
    }
    
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        const char* resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        send(client_fd, resp, strlen(resp), 0);
        close(client_fd);
        return;
    }
    
    struct stat st;
    fstat(fd, &st);
    
    char header[1024];
    snprintf(header, sizeof(header), 
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %ld\r\n"
             "Access-Control-Allow-Origin: *\r\n"
             "Connection: close\r\n"
             "\r\n", 
             get_mime_type(filepath), (long)st.st_size);
             
    send(client_fd, header, strlen(header), 0);
    
    char fbuf[8192];
    ssize_t bytes_read;
    while ((bytes_read = read(fd, fbuf, sizeof(fbuf))) > 0) {
        ssize_t sent = 0;
        while (sent < bytes_read) {
            ssize_t s_ret = send(client_fd, fbuf + sent, bytes_read - sent, 0);
            if (s_ret <= 0) break;
            sent += s_ret;
        }
    }
    close(fd);
    close(client_fd);
}

static void* http_server_thread(void* arg) {
    http_server_t* s = (http_server_t*)arg;
    
    while (s->running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(s->server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (!s->running) break;
            continue;
        }
        handle_client(client_fd, s);
    }
    return NULL;
}

static zst_result_t http_server_start(zst_element_t* el) {
    http_server_t* s = el->priv;
    
    s->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->server_fd < 0) return ZST_ERROR;
    
    int opt = 1;
    setsockopt(s->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(s->port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(s->server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(s->server_fd);
        s->server_fd = -1;
        return ZST_ERROR;
    }
    
    if (listen(s->server_fd, 10) < 0) {
        close(s->server_fd);
        s->server_fd = -1;
        return ZST_ERROR;
    }
    
    s->running = 1;
    if (pthread_create(&s->thread, NULL, http_server_thread, s) != 0) {
        s->running = 0;
        close(s->server_fd);
        s->server_fd = -1;
        return ZST_ERROR;
    }
    
    return ZST_OK;
}

static zst_result_t http_server_stop(zst_element_t* el) {
    http_server_t* s = el->priv;
    if (s->running) {
        s->running = 0;
        shutdown(s->server_fd, SHUT_RDWR);
        close(s->server_fd);
        pthread_join(s->thread, NULL);
        s->server_fd = -1;
    }
    return ZST_OK;
}

static zst_result_t http_server_set_property(zst_element_t* el, const char* name, const char* value) {
    http_server_t* s = el->priv;
    if (strcmp(name, "port") == 0) {
        s->port = atoi(value);
        return ZST_OK;
    } else if (strcmp(name, "document-root") == 0) {
        snprintf(s->document_root, sizeof(s->document_root), "%s", value);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t http_server_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len) {
    http_server_t* s = el->priv;
    if (strcmp(name, "port") == 0) {
        snprintf(value_out, max_len, "%d", s->port);
        return ZST_OK;
    } else if (strcmp(name, "document-root") == 0) {
        snprintf(value_out, max_len, "%s", s->document_root);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_element_ops_t g_http_server_ops = {
    .name = "http_server",
    .start = http_server_start,
    .stop = http_server_stop,
    .set_property = http_server_set_property,
    .get_property = http_server_get_property
};

zst_element_t* zst_http_server_element_create(void) {
    http_server_t* priv = calloc(1, sizeof(http_server_t));
    if (!priv) return NULL;
    
    priv->port = 8080;
    snprintf(priv->document_root, sizeof(priv->document_root), ".");
    
    zst_element_t* el = zst_element_create(&g_http_server_ops, priv);
    return el;
}
