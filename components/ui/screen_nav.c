#include "screen_nav.h"

#include <stddef.h>
#include <string.h>

#include <esp_log.h>

static const char *TAG = "screen_nav";

typedef struct {
    const char *name;
    screen_create_fn_t create;
} screen_entry_t;

#define SCREEN_REGISTRY_MAX 8
#define SCREEN_STACK_MAX    8

static screen_entry_t s_screen_registry[SCREEN_REGISTRY_MAX];
static size_t s_screen_registry_count;
static const char *s_screen_stack[SCREEN_STACK_MAX];
static size_t s_screen_stack_depth;
static const char *s_current_screen;

void screen_register(const char *name, screen_create_fn_t create)
{
    if (s_screen_registry_count >= SCREEN_REGISTRY_MAX) {
        ESP_LOGE(TAG, "screen_register: registry full, dropping \"%s\"", name);
        return;
    }
    s_screen_registry[s_screen_registry_count].name = name;
    s_screen_registry[s_screen_registry_count].create = create;
    s_screen_registry_count++;
}

static screen_create_fn_t screen_lookup(const char *name)
{
    for (size_t i = 0; i < s_screen_registry_count; i++) {
        if (strcmp(s_screen_registry[i].name, name) == 0) {
            return s_screen_registry[i].create;
        }
    }
    return NULL;
}

void screen_activate(const char *name)
{
    screen_create_fn_t create = screen_lookup(name);
    if (create == NULL) {
        ESP_LOGE(TAG, "screen_activate: unknown screen \"%s\"", name);
        return;
    }
    s_current_screen = name;
    create();
}

void screen_push(const char *name)
{
    if (s_current_screen != NULL) {
        if (s_screen_stack_depth < SCREEN_STACK_MAX) {
            s_screen_stack[s_screen_stack_depth++] = s_current_screen;
        } else {
            ESP_LOGW(TAG, "screen_push: back stack full, losing \"%s\"", s_current_screen);
        }
    }
    screen_activate(name);
}

void screen_pop(void)
{
    if (s_screen_stack_depth > 0) {
        screen_activate(s_screen_stack[--s_screen_stack_depth]);
    } else {
        screen_activate(SCREEN_HOME);
    }
}
