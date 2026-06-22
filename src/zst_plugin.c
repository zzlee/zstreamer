#include "zst_element_factory.h"
/*=============================================================================
    zst_plugin.c — dlopen-based dynamic plugin loader and registry
=============================================================================*/
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L  /* strdup */

#include "zst_plugin.h"
#include "zst_queue.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <dirent.h>
#include <stdio.h>

#ifdef _WIN32
#  include <windows.h>
#  define DLOPEN(f)   (void*)LoadLibraryA(f)
#  define DLSYM(h, n) (void*)GetProcAddress((HMODULE)h, n)
#  define DLCLOSE(h)   FreeLibrary((HMODULE)h)
#else
#  include <dlfcn.h>
#  ifndef RTLD_NODELETE
#    define RTLD_NODELETE 0x01000
#  endif
#  define DLOPEN(f)   dlopen(f, RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE)
#  define DLSYM(h, n) dlsym(h, n)
#  define DLCLOSE(h)   dlclose(h)
#endif

typedef struct zst_registry_entry {
    zst_plugin_t*              plugin;
    char*                      path;
    const zst_element_desc_t*  elements;
    uint32_t                   nb_elements;
    zst_create_element_fn      create_element;
    int                        is_builtin;
    struct zst_registry_entry* next;
} zst_registry_entry_t;

static struct {
    zst_registry_entry_t* head;
    pthread_mutex_t      lock;
    int                  initialized;
} g_registry = {
    .head = NULL,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .initialized = 0
};

/* suppress -Wpedantic warning for dlsym cast to function pointer */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

zst_plugin_t*
zst_plugin_load(const char* path)
{
    if (!path) return NULL;

    void* handle = DLOPEN(path);
    if (!handle) return NULL;

    zst_get_plugin_fn get_plugin = (zst_get_plugin_fn)DLSYM(handle, "zst_get_plugin");
    if (!get_plugin) {
        DLCLOSE(handle);
        return NULL;
    }

    zst_plugin_t* plugin = get_plugin();
    if (!plugin) {
        DLCLOSE(handle);
        return NULL;
    }

    plugin->refcount = 1;

    /* Call plugin init if provided */
    if (plugin->desc.init)
        plugin->desc.init();

    plugin->priv = (void*)handle;
    return plugin;
}

#pragma GCC diagnostic pop

void
zst_plugin_unload(zst_plugin_t* plugin)
{
    if (!plugin) return;

    if (plugin->desc.deinit)
        plugin->desc.deinit();

    if (plugin->priv)
        DLCLOSE(plugin->priv);

    free(plugin);
}

zst_plugin_t*
zst_plugin_ref(zst_plugin_t* plugin)
{
    if (!plugin) return NULL;
    __sync_fetch_and_add(&plugin->refcount, 1);
    return plugin;
}

void
zst_plugin_unref(zst_plugin_t* plugin)
{
    if (!plugin) return;
    if (__sync_sub_and_fetch(&plugin->refcount, 1) <= 0) {
        zst_plugin_unload(plugin);
    }
}

zst_result_t
zst_plugin_registry_init(void)
{
    pthread_mutex_lock(&g_registry.lock);
    if (g_registry.initialized) {
        pthread_mutex_unlock(&g_registry.lock);
        return ZST_OK;
    }
    g_registry.head = NULL;
    g_registry.initialized = 1;
    pthread_mutex_unlock(&g_registry.lock);
    return ZST_OK;
}

void
zst_plugin_registry_deinit(void)
{
    pthread_mutex_lock(&g_registry.lock);
    if (!g_registry.initialized) {
        pthread_mutex_unlock(&g_registry.lock);
        return;
    }
    zst_registry_entry_t* curr = g_registry.head;
    while (curr) {
        zst_registry_entry_t* next = curr->next;
        if (curr->plugin)
            zst_plugin_unref(curr->plugin);
        free(curr->path);
        free(curr);
        curr = next;
    }
    g_registry.head = NULL;
    g_registry.initialized = 0;
    pthread_mutex_unlock(&g_registry.lock);
}

static const zst_element_desc_t*
plugin_query_elements(zst_plugin_t* plugin, uint32_t* nb_elements_out)
{
    if (nb_elements_out) *nb_elements_out = 0;
    if (!plugin || !plugin->priv) return NULL;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    zst_get_plugin_elements_fn get_elements =
        (zst_get_plugin_elements_fn)DLSYM(plugin->priv, "zst_get_plugin_elements");
#pragma GCC diagnostic pop
    if (!get_elements) return NULL;

    return get_elements(nb_elements_out);
}

zst_result_t
zst_plugin_registry_scan(const char* directory)
{
    if (!directory) return ZST_ERROR;

    DIR* dir = opendir(directory);
    if (!dir) return ZST_ERROR;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len > 3 && strcmp(entry->d_name + len - 3, ".so") == 0) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);

            /* Check if already loaded in registry */
            pthread_mutex_lock(&g_registry.lock);
            zst_registry_entry_t* curr = g_registry.head;
            int already_loaded = 0;
            while (curr) {
                if (curr->path && strcmp(curr->path, path) == 0) {
                    already_loaded = 1;
                    break;
                }
                curr = curr->next;
            }
            pthread_mutex_unlock(&g_registry.lock);

            if (already_loaded) continue;

            zst_plugin_t* plugin = zst_plugin_load(path);
            if (plugin) {
                zst_registry_entry_t* node = malloc(sizeof(*node));
                if (node) {
                    node->plugin = plugin;
                    node->path = strdup(path);
                    node->elements = plugin_query_elements(plugin, &node->nb_elements);
                    node->create_element = plugin->create_element;
                    node->is_builtin = 0;
                    
                    pthread_mutex_lock(&g_registry.lock);
                    node->next = g_registry.head;
                    g_registry.head = node;
                    pthread_mutex_unlock(&g_registry.lock);
                } else {
                    zst_plugin_unref(plugin);
                }
            }
        }
    }

    closedir(dir);
    return ZST_OK;
}

zst_result_t
zst_plugin_registry_scan_env(void)
{
    const char* env = getenv("ZSTREAMER_PLUGIN_PATH");
    if (env) {
        char* env_copy = strdup(env);
        if (!env_copy) return ZST_ERROR;

        char* token = strtok(env_copy, ":");
        while (token) {
            zst_plugin_registry_scan(token);
            token = strtok(NULL, ":");
        }

        free(env_copy);
    }
#ifdef ZSTREAMER_DEFAULT_PLUGIN_DIR
    else {
        zst_plugin_registry_scan(ZSTREAMER_DEFAULT_PLUGIN_DIR);
    }
#endif
    return ZST_OK;
}

static const zst_element_desc_t*
entry_find_desc(zst_registry_entry_t* entry, const char* name)
{
    if (!entry || !name || !entry->elements) return NULL;
    for (uint32_t i = 0; i < entry->nb_elements; i++) {
        const zst_element_desc_t* desc = &entry->elements[i];
        if (desc->name && strcmp(desc->name, name) == 0) {
            return desc;
        }
    }
    return NULL;
}

zst_element_t*
zst_element_factory_make(const char* name)
{
    if (!name) return NULL;

    pthread_mutex_lock(&g_registry.lock);
    zst_registry_entry_t* curr = g_registry.head;
    while (curr) {
        const zst_element_desc_t* desc = entry_find_desc(curr, name);
        if (curr->create_element && (!curr->is_builtin || desc)) {
            zst_element_t* el = curr->create_element(name);
            if (el) {
                if (curr->plugin)
                    el->plugin = zst_plugin_ref(curr->plugin);
                el->desc = desc ? desc : entry_find_desc(curr, el->ops ? el->ops->name : name);
                pthread_mutex_unlock(&g_registry.lock);
                return el;
            }
        } else if (desc && desc->create) {
            zst_element_t* el = desc->create();
            if (el) {
                if (curr->plugin)
                    el->plugin = zst_plugin_ref(curr->plugin);
                el->desc = desc;
                pthread_mutex_unlock(&g_registry.lock);
                return el;
            }
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_registry.lock);
    return NULL;
}

uint32_t
zst_element_factory_list(const zst_element_desc_t*** elements_out)
{
    uint32_t count = 0;
    const zst_element_desc_t** out = NULL;

    if (!elements_out) return 0;
    *elements_out = NULL;

    pthread_mutex_lock(&g_registry.lock);
    for (zst_registry_entry_t* curr = g_registry.head; curr; curr = curr->next) {
        count += curr->nb_elements;
    }

    if (count > 0) {
        out = calloc(count, sizeof(*out));
        if (!out) {
            pthread_mutex_unlock(&g_registry.lock);
            return 0;
        }

        uint32_t idx = 0;
        for (zst_registry_entry_t* curr = g_registry.head; curr; curr = curr->next) {
            if (!curr->elements) continue;
            for (uint32_t i = 0; i < curr->nb_elements; i++) {
                out[idx++] = &curr->elements[i];
            }
        }
    }

    pthread_mutex_unlock(&g_registry.lock);
    *elements_out = out;
    return count;
}

void
zst_element_factory_list_free(const zst_element_desc_t** elements)
{
    free((void*)elements);
}

const zst_element_desc_t*
zst_element_factory_get_desc(const char* name)
{
    if (!name) return NULL;

    pthread_mutex_lock(&g_registry.lock);
    for (zst_registry_entry_t* curr = g_registry.head; curr; curr = curr->next) {
        const zst_element_desc_t* desc = entry_find_desc(curr, name);
        if (desc) {
            pthread_mutex_unlock(&g_registry.lock);
            return desc;
        }
    }
    pthread_mutex_unlock(&g_registry.lock);
    return NULL;
}

zst_result_t
zst_plugin_registry_add_entry(
    const zst_element_desc_t* elements,
    uint32_t nb_elements,
    zst_create_element_fn create_element)
{
    zst_registry_entry_t* node;

    if (!elements || nb_elements == 0 || !create_element) return ZST_ERROR;

    node = calloc(1, sizeof(*node));
    if (!node) return ZST_ERROR;

    node->plugin = NULL;
    node->path = NULL;
    node->elements = elements;
    node->nb_elements = nb_elements;
    node->create_element = create_element;
    node->is_builtin = 1;

    pthread_mutex_lock(&g_registry.lock);
    node->next = g_registry.head;
    g_registry.head = node;
    pthread_mutex_unlock(&g_registry.lock);

    return ZST_OK;
}
