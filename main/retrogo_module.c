#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../include/module_abi.h"
#include "holo_catalog.h"
#include "holo_port.h"

#define RETROGO_MODULE_EXPORT __attribute__((visibility("default"), used))
#define RETROGO_VERSION "0.1.0"
#define RETROGO_MIN_HOST_ABI 0x00020002u

void retrogo_core_app_main(void);
#if defined(RG_TARGET_HOLO_DYNMOD)
void rg_system_deinit_for_holo(void);
#endif

typedef struct retrogo_instance_t {
    const module_host_api_v1 *host;
    void *task;
    volatile uint8_t running;
    volatile uint8_t stop_requested;
    uint32_t created_ms;
    char module_path[MODULE_PATH_MAX];
} retrogo_instance_t;

static const module_host_api_v1 *s_host;
static retrogo_instance_t s_static_instance;

static const module_manifest_t s_manifest = {
    MODULE_MANIFEST_MAGIC,
    MODULE_ABI_VERSION,
    sizeof(module_manifest_t),
    "retrogo",
    RETROGO_VERSION,
    "Retro-Go dynamic module for Holocubic",
    0,
    RETROGO_MIN_HOST_ABI,
};

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (i + 1 < dst_size && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static int host_abi_is_compatible(const module_host_api_v1 *host)
{
    const uint32_t module_major = MODULE_ABI_VERSION & 0xFFFF0000u;
    const size_t need_lua = offsetof(module_host_api_v1, lua) + sizeof(host->lua);
    const size_t need_heap = offsetof(module_host_api_v1, heap) + sizeof(host->heap);
    if (!host) {
        return 0;
    }
    if ((host->abi_version & 0xFFFF0000u) != module_major) {
        return 0;
    }
    if (host->abi_version < RETROGO_MIN_HOST_ABI) {
        return 0;
    }
    if (host->size < need_lua || host->size < need_heap) {
        return 0;
    }
    if (host->heap.size < sizeof(host->heap) ||
        !host->heap.malloc || !host->heap.calloc ||
        !host->heap.free || !host->heap.free_size ||
        !host->heap.largest_free_block) {
        return 0;
    }
    return host->task.create &&
           host->lua.createtable && host->lua.setfield &&
           host->lua.pushstring && host->lua.pushboolean &&
           host->lua.pushinteger && host->lua.pushlightuserdata &&
           host->lua.pushcclosure && host->lua.gettop &&
           host->lua.settop && host->lua.getfield &&
           host->lua.isstring && host->lua.istable &&
           host->lua.tostring && host->lua.tointeger;
}

static int push_error(lua_State *L, const module_host_api_v1 *host, const char *err)
{
    if (!host) {
        return 0;
    }
    host->lua.pushnil(L);
    host->lua.pushstring(L, err ? err : "retrogo failed");
    return 2;
}

static void set_string_field(lua_State *L, const module_host_api_v1 *host, const char *key, const char *value)
{
    host->lua.pushstring(L, value ? value : "");
    host->lua.setfield(L, -2, key);
}

static void set_integer_field(lua_State *L, const module_host_api_v1 *host, const char *key, int64_t value)
{
    host->lua.pushinteger(L, value);
    host->lua.setfield(L, -2, key);
}

static void set_boolean_field(lua_State *L, const module_host_api_v1 *host, const char *key, int value)
{
    host->lua.pushboolean(L, value ? 1 : 0);
    host->lua.setfield(L, -2, key);
}

static void set_closure_field(lua_State *L,
                              const module_host_api_v1 *host,
                              const char *key,
                              module_lua_cfunction_t fn,
                              void *upvalue)
{
    host->lua.pushlightuserdata(L, upvalue);
    host->lua.pushcclosure(L, fn, 1);
    host->lua.setfield(L, -2, key);
}

static void create_log(const module_host_api_v1 *host, const char *text)
{
    if (host && host->serial.println) {
        host->serial.println(text);
    }
}

static void set_gamepad_constants(lua_State *L, const module_host_api_v1 *host)
{
    set_integer_field(L, host, "BTN_A", MODULE_GAMEPAD_A);
    set_integer_field(L, host, "BTN_B", MODULE_GAMEPAD_B);
    set_integer_field(L, host, "BTN_SELECT", MODULE_GAMEPAD_SELECT);
    set_integer_field(L, host, "BTN_START", MODULE_GAMEPAD_START);
    set_integer_field(L, host, "BTN_UP", MODULE_GAMEPAD_UP);
    set_integer_field(L, host, "BTN_DOWN", MODULE_GAMEPAD_DOWN);
    set_integer_field(L, host, "BTN_LEFT", MODULE_GAMEPAD_LEFT);
    set_integer_field(L, host, "BTN_RIGHT", MODULE_GAMEPAD_RIGHT);
    set_integer_field(L, host, "BTN_X", MODULE_GAMEPAD_X);
    set_integer_field(L, host, "BTN_Y", MODULE_GAMEPAD_Y);
    set_integer_field(L, host, "BTN_L", MODULE_GAMEPAD_L);
    set_integer_field(L, host, "BTN_R", MODULE_GAMEPAD_R);
    set_integer_field(L, host, "BTN_HOME", MODULE_GAMEPAD_HOME);
    set_integer_field(L, host, "BTN_MENU", MODULE_GAMEPAD_MENU);
}

static retrogo_instance_t *instance_from_lua(lua_State *L)
{
    if (!s_host || !s_host->lua.touserdata || !s_host->lua.upvalue_index) {
        return NULL;
    }
    return (retrogo_instance_t *)s_host->lua.touserdata(L, s_host->lua.upvalue_index(1));
}

static int read_table_string(lua_State *L,
                             const module_host_api_v1 *host,
                             int table_index,
                             const char *key,
                             char *out,
                             size_t out_size)
{
    int top = host->lua.gettop(L);
    int found = 0;
    host->lua.getfield(L, table_index, key);
    if (host->lua.isstring(L, -1)) {
        copy_text(out, out_size, host->lua.tostring(L, -1));
        found = 1;
    }
    host->lua.settop(L, top);
    return found;
}

static int read_table_integer(lua_State *L,
                              const module_host_api_v1 *host,
                              int table_index,
                              const char *key,
                              int64_t *out)
{
    int top = host->lua.gettop(L);
    int found = 0;
    host->lua.getfield(L, table_index, key);
    if (host->lua.isnumber(L, -1)) {
        *out = host->lua.tointeger(L, -1);
        found = 1;
    }
    host->lua.settop(L, top);
    return found;
}

static int l_catalog_info(lua_State *L)
{
    retrogo_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? inst->host : s_host;
    size_t entries = 0;
    size_t dirs = 0;
    size_t files = 0;

    if (!host) {
        return 0;
    }
    holo_catalog_info(&entries, &dirs, &files);
    host->lua.createtable(L, 0, 5);
    set_integer_field(L, host, "entries", (int64_t)entries);
    set_integer_field(L, host, "dirs", (int64_t)dirs);
    set_integer_field(L, host, "files", (int64_t)files);
    set_boolean_field(L, host, "ready", holo_catalog_ready());
    return 1;
}

static int l_set_catalog(lua_State *L)
{
    retrogo_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? inst->host : s_host;
    const char *blob = NULL;
    size_t len = 0;
    int top;

    if (!host) {
        return 0;
    }
    create_log(host, "[retrogo.so] set_catalog enter");

    if (host->lua.isstring(L, 1)) {
        if (host->lua.tolstring) {
            blob = host->lua.tolstring(L, 1, &len);
        } else {
            blob = host->lua.tostring(L, 1);
            len = blob ? strlen(blob) : 0;
        }
    } else if (host->lua.istable(L, 1)) {
        top = host->lua.gettop(L);
        host->lua.getfield(L, 1, "blob");
        if (host->lua.isstring(L, -1)) {
            if (host->lua.tolstring) {
                blob = host->lua.tolstring(L, -1, &len);
            } else {
                blob = host->lua.tostring(L, -1);
                len = blob ? strlen(blob) : 0;
            }
        }
        host->lua.settop(L, top);
    } else if (host->lua.isnil(L, 1)) {
        holo_catalog_clear();
        host->lua.pushboolean(L, 1);
        return 1;
    }

    if (!blob) {
        create_log(host, "[retrogo.so] set_catalog bad arg");
        return push_error(L, host, "retrogo.set_catalog: expected catalog blob or {blob=...}");
    }
    if (!holo_catalog_load_blob(blob, len)) {
        create_log(host, "[retrogo.so] set_catalog oom");
        return push_error(L, host, "retrogo.set_catalog: out of memory");
    }
    create_log(host, "[retrogo.so] set_catalog done");
    host->lua.pushboolean(L, 1);
    return 1;
}

static void retrogo_task_entry(void *arg)
{
    retrogo_instance_t *inst = (retrogo_instance_t *)arg;
    if (!inst) {
        return;
    }
    inst->running = 1;
    holo_port_log("[retrogo.so] task entry");
    holo_port_log("[retrogo.so] retro-go task start");
    holo_runtime_bind_task(&inst->running, &inst->task);
    retrogo_core_app_main();
#if defined(RG_TARGET_HOLO_DYNMOD)
    rg_system_deinit_for_holo();
#endif
    holo_runtime_unbind_task();
    inst->running = 0;
    inst->task = NULL;
    holo_display_release();
    holo_port_log("[retrogo.so] display release");
    holo_port_log("[retrogo.so] retro-go task stop");
    if (inst->host && inst->host->task.remove) {
        inst->host->task.remove(NULL);
    }
    for (;;) {
        if (inst->host && inst->host->task.delay) {
            inst->host->task.delay(1000);
        }
    }
}

static int l_start(lua_State *L)
{
    retrogo_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? inst->host : s_host;
    char app[16] = "launcher";
    char rom[MODULE_PATH_MAX] = "";
    int64_t flags = 0;

    if (!inst || !host) {
        return push_error(L, host, "retrogo.start: instance missing");
    }
    create_log(host, "[retrogo.so] start enter");
    if (inst->running || inst->task) {
        create_log(host, "[retrogo.so] start already running");
        return push_error(L, host, "retro-go is already running");
    }

    if (host->lua.gettop(L) >= 1 && host->lua.istable(L, 1)) {
        (void)read_table_string(L, host, 1, "app", app, sizeof(app));
        (void)read_table_string(L, host, 1, "system", app, sizeof(app));
        (void)read_table_string(L, host, 1, "rom", rom, sizeof(rom));
        (void)read_table_string(L, host, 1, "path", rom, sizeof(rom));
        (void)read_table_integer(L, host, 1, "flags", &flags);
    } else if (host->lua.gettop(L) >= 1 && host->lua.isstring(L, 1)) {
        copy_text(app, sizeof(app), host->lua.tostring(L, 1));
        if (host->lua.gettop(L) >= 2 && host->lua.isstring(L, 2)) {
            copy_text(rom, sizeof(rom), host->lua.tostring(L, 2));
        }
    }

    holo_launch_set(app, rom, (uint32_t)flags);
    inst->stop_requested = 0;

    if (!host->task.create) {
        create_log(host, "[retrogo.so] start no task api");
        return push_error(L, host, "host task API is missing");
    }
    create_log(host, "[retrogo.so] start task.create");
    if (host->task.create("retrogo", retrogo_task_entry, inst, 24u * 1024u, 3u, 1, &inst->task) != MODULE_OK) {
        inst->task = NULL;
        create_log(host, "[retrogo.so] start task.create failed");
        return push_error(L, host, "failed to create retro-go task");
    }

    create_log(host, "[retrogo.so] start task.create done");
    host->lua.pushboolean(L, 1);
    return 1;
}

static int l_stop(lua_State *L)
{
    retrogo_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? inst->host : s_host;

    if (!inst || !host) {
        return push_error(L, host, "retrogo.stop: instance missing");
    }
    inst->stop_requested = 1;
    holo_runtime_request_stop();
    host->lua.pushboolean(L, 1);
    return 1;
}

static int l_set_input_mask(lua_State *L)
{
    retrogo_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? inst->host : s_host;
    int64_t mask;

    if (!host) {
        return 0;
    }
    mask = host->lua.checkinteger(L, 1);
    if (mask < 0) {
        mask = 0;
    }
    holo_input_set_mask((uint32_t)mask);
    host->lua.pushboolean(L, 1);
    return 1;
}

static int l_info(lua_State *L)
{
    retrogo_instance_t *inst = instance_from_lua(L);
    const module_host_api_v1 *host = inst ? inst->host : s_host;
    holo_launch_t launch;

    if (!inst || !host) {
        return push_error(L, host, "retrogo.info: instance missing");
    }

    holo_launch_get(&launch);
    host->lua.createtable(L, 0, 12);
    set_string_field(L, host, "version", RETROGO_VERSION);
    set_string_field(L, host, "module_path", inst->module_path);
    set_string_field(L, host, "app", launch.config_ns);
    set_string_field(L, host, "rom", launch.rom_path);
    set_integer_field(L, host, "boot_flags", launch.boot_flags);
    set_integer_field(L, host, "input_mask", holo_input_get_raw_mask());
    set_integer_field(L, host, "rg_input_mask", holo_input_get_mask());
    set_boolean_field(L, host, "running", inst->running);
    set_boolean_field(L, host, "catalog_ready", holo_catalog_ready());
    return 1;
}

RETROGO_MODULE_EXPORT const module_manifest_t *module_query_v1(void)
{
    return &s_manifest;
}

RETROGO_MODULE_EXPORT int32_t module_create_v1(const module_host_api_v1 *host,
                                               const module_open_info_t *info,
                                               void **out_instance)
{
    retrogo_instance_t *inst;

    create_log(host, "[retrogo.so] create enter");
    if (!out_instance || !host) {
        return MODULE_ERR_INVALID_ARG;
    }
    *out_instance = NULL;
    create_log(host, "[retrogo.so] create validate");
    if (!host_abi_is_compatible(host)) {
        return MODULE_ERR_VERSION;
    }

    s_host = host;
    holo_port_set_host(host);
    create_log(host, "[retrogo.so] create host set");

    inst = &s_static_instance;
    memset(inst, 0, sizeof(*inst));

    inst->host = host;
    inst->created_ms = host->time.millis ? host->time.millis() : 0;
    copy_text(inst->module_path, sizeof(inst->module_path), info ? info->path : "");
    *out_instance = inst;
    create_log(host, "[retrogo.so] create done");
    return MODULE_OK;
}

RETROGO_MODULE_EXPORT int32_t module_luaopen_v1(void *instance, lua_State *L)
{
    retrogo_instance_t *inst = (retrogo_instance_t *)instance;
    const module_host_api_v1 *host = inst ? inst->host : s_host;

    if (!inst || !host || !L) {
        return MODULE_ERR_INVALID_ARG;
    }

    host->lua.createtable(L, 0, 20);
    set_string_field(L, host, "VERSION", RETROGO_VERSION);
    set_boolean_field(L, host, "CATALOG_BLOB", 1);
    set_boolean_field(L, host, "RETRO_GO_CORE", 1);
    set_gamepad_constants(L, host);
    set_closure_field(L, host, "set_catalog", l_set_catalog, inst);
    set_closure_field(L, host, "catalog_info", l_catalog_info, inst);
    set_closure_field(L, host, "start", l_start, inst);
    set_closure_field(L, host, "start_launcher", l_start, inst);
    set_closure_field(L, host, "stop", l_stop, inst);
    set_closure_field(L, host, "set_input_mask", l_set_input_mask, inst);
    set_closure_field(L, host, "info", l_info, inst);
    return MODULE_OK;
}

RETROGO_MODULE_EXPORT void module_destroy_v1(void *instance)
{
    retrogo_instance_t *inst = (retrogo_instance_t *)instance;
    const module_host_api_v1 *host = inst ? inst->host : s_host;

    if (!inst || !host) {
        return;
    }
    holo_runtime_request_stop();
    for (int i = 0; inst->running && i < 100; ++i) {
        if (host->task.delay) {
            host->task.delay(10);
        } else if (host->time.delay) {
            host->time.delay(10);
        }
    }
    holo_catalog_clear();
    holo_port_log("[retrogo.so] destroy");
    if (inst->running) {
        holo_port_log("[retrogo.so] destroy deferred; task still running");
        return;
    }
    if (inst != &s_static_instance && host->heap.free) {
        host->heap.free(inst);
    }
}
