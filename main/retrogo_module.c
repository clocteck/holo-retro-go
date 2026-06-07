#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../include/module_abi.h"
#include "holo_catalog.h"
#include "holo_port.h"

#define RETROGO_MODULE_EXPORT __attribute__((visibility("default"), used))
#define RETROGO_VERSION "0.1.0"
#define RETROGO_MIN_HOST_ABI MODULE_BOOTSTRAP_ABI_VERSION

void retrogo_core_app_main(void);
#if defined(RG_TARGET_HOLO_DYNMOD)
void rg_system_deinit_for_holo(void);
#endif

typedef struct retrogo_instance_t
{
    const module_host_api_v1 *host;
    void *task;
    volatile uint8_t running;
    volatile uint8_t stop_requested;
    uint32_t created_ms;
    char module_path[MODULE_PATH_MAX];
} retrogo_instance_t;

static const module_host_api_v1 *s_host;
static retrogo_instance_t s_static_instance;
static module_host_api_v1 s_resolved_host;

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
    if (!dst || dst_size == 0)
    {
        return;
    }
    if (!src)
    {
        dst[0] = '\0';
        return;
    }
    while (i + 1 < dst_size && src[i])
    {
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
    if (!host)
    {
        return 0;
    }
    if ((host->abi_version & 0xFFFF0000u) != module_major)
    {
        return 0;
    }
    if (host->abi_version < RETROGO_MIN_HOST_ABI)
    {
        return 0;
    }
    if (host->size < need_lua || host->size < need_heap)
    {
        return 0;
    }
    if (host->heap.size < sizeof(host->heap) ||
        !host->heap.malloc || !host->heap.calloc ||
        !host->heap.free || !host->heap.free_size ||
        !host->heap.largest_free_block)
    {
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
    if (!host)
    {
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
    if (host && host->serial.println)
    {
        host->serial.println(text);
    }
}

static int32_t resolve_required_proc(module_host_resolve_v1_fn resolve,
                                     void *resolve_ctx,
                                     uint32_t proc_id,
                                     void **out_proc)
{
    int32_t err;

    if (!resolve || !out_proc)
    {
        return MODULE_ERR_INVALID_ARG;
    }

    *out_proc = NULL;
    err = resolve(resolve_ctx, proc_id, out_proc);
    if (err != MODULE_OK)
    {
        return err;
    }
    return *out_proc ? MODULE_OK : MODULE_ERR_UNSUPPORTED;
}

#define RESOLVE_REQUIRED(proc_id, slot)                                              \
    do                                                                               \
    {                                                                                \
        void *proc = NULL;                                                           \
        int32_t err = resolve_required_proc(resolve, resolve_ctx, (proc_id), &proc); \
        if (err != MODULE_OK)                                                        \
        {                                                                            \
            return err;                                                              \
        }                                                                            \
        (slot) = (__typeof__(slot))proc;                                             \
    } while (0)

#define RESOLVE_OPTIONAL(proc_id, slot)                                                 \
    do                                                                                  \
    {                                                                                   \
        void *proc = NULL;                                                              \
        if (resolve_required_proc(resolve, resolve_ctx, (proc_id), &proc) == MODULE_OK) \
        {                                                                               \
            (slot) = (__typeof__(slot))proc;                                            \
        }                                                                               \
    } while (0)

static int32_t resolve_host_api(module_host_resolve_v1_fn resolve,
                                void *resolve_ctx,
                                module_host_api_v1 *out)
{
    if (!out)
    {
        return MODULE_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->abi_version = MODULE_ABI_VERSION;
    out->size = sizeof(*out);

    out->serial.size = sizeof(out->serial);
    RESOLVE_REQUIRED(MODULE_PROC_SERIAL_WRITE_V1, out->serial.write);
    RESOLVE_REQUIRED(MODULE_PROC_SERIAL_PRINT_V1, out->serial.print);
    RESOLVE_REQUIRED(MODULE_PROC_SERIAL_PRINTLN_V1, out->serial.println);
    RESOLVE_REQUIRED(MODULE_PROC_SERIAL_FLUSH_V1, out->serial.flush);

    out->sd.size = sizeof(out->sd);
    RESOLVE_REQUIRED(MODULE_PROC_SD_BEGIN_V1, out->sd.begin);
    RESOLVE_REQUIRED(MODULE_PROC_SD_MOUNTED_V1, out->sd.mounted);
    RESOLVE_REQUIRED(MODULE_PROC_SD_MOUNT_POINT_V1, out->sd.mount_point);
    RESOLVE_REQUIRED(MODULE_PROC_SD_EXISTS_V1, out->sd.exists);
    RESOLVE_REQUIRED(MODULE_PROC_SD_MKDIR_V1, out->sd.mkdir);
    RESOLVE_REQUIRED(MODULE_PROC_SD_REMOVE_V1, out->sd.remove);
    RESOLVE_REQUIRED(MODULE_PROC_SD_RENAME_V1, out->sd.rename);
    RESOLVE_REQUIRED(MODULE_PROC_SD_OPEN_V1, out->sd.open);

    out->file.size = sizeof(out->file);
    RESOLVE_REQUIRED(MODULE_PROC_FILE_CLOSE_V1, out->file.close);
    RESOLVE_REQUIRED(MODULE_PROC_FILE_AVAILABLE_V1, out->file.available);
    RESOLVE_REQUIRED(MODULE_PROC_FILE_READ_V1, out->file.read);
    RESOLVE_REQUIRED(MODULE_PROC_FILE_WRITE_V1, out->file.write);
    RESOLVE_REQUIRED(MODULE_PROC_FILE_SEEK_V1, out->file.seek);
    RESOLVE_REQUIRED(MODULE_PROC_FILE_POSITION_V1, out->file.position);
    RESOLVE_REQUIRED(MODULE_PROC_FILE_SIZE_BYTES_V1, out->file.size_bytes);
    RESOLVE_REQUIRED(MODULE_PROC_FILE_FLUSH_V1, out->file.flush);
    RESOLVE_REQUIRED(MODULE_PROC_FILE_IS_DIRECTORY_V1, out->file.is_directory);

    out->display.size = sizeof(out->display);
    RESOLVE_REQUIRED(MODULE_PROC_DISPLAY_WIDTH_V1, out->display.width);
    RESOLVE_REQUIRED(MODULE_PROC_DISPLAY_HEIGHT_V1, out->display.height);
    RESOLVE_REQUIRED(MODULE_PROC_DISPLAY_ACQUIRE_V1, out->display.acquire);
    RESOLVE_REQUIRED(MODULE_PROC_DISPLAY_RELEASE_V1, out->display.release);
    RESOLVE_REQUIRED(MODULE_PROC_DISPLAY_START_WRITE_V1, out->display.startWrite);
    RESOLVE_REQUIRED(MODULE_PROC_DISPLAY_PUSH_IMAGE_DMA_V1, out->display.pushImageDMA);
    RESOLVE_REQUIRED(MODULE_PROC_DISPLAY_END_WRITE_V1, out->display.endWrite);
    RESOLVE_REQUIRED(MODULE_PROC_DISPLAY_FILL_SCREEN_V1, out->display.fillScreen);
    RESOLVE_REQUIRED(MODULE_PROC_DISPLAY_SET_ADDR_WINDOW_V1, out->display.setAddrWindow);
    RESOLVE_REQUIRED(MODULE_PROC_DISPLAY_PUSH_PIXELS_DMA_V1, out->display.pushPixelsDMA);

    out->audio.size = sizeof(out->audio);
    RESOLVE_REQUIRED(MODULE_PROC_AUDIO_BEGIN_V1, out->audio.begin);
    RESOLVE_REQUIRED(MODULE_PROC_AUDIO_WRITE_V1, out->audio.write);
    RESOLVE_REQUIRED(MODULE_PROC_AUDIO_AVAILABLE_V1, out->audio.available);
    RESOLVE_REQUIRED(MODULE_PROC_AUDIO_END_V1, out->audio.end);

    out->time.size = sizeof(out->time);
    RESOLVE_REQUIRED(MODULE_PROC_TIME_MILLIS_V1, out->time.millis);
    RESOLVE_REQUIRED(MODULE_PROC_TIME_MICROS_V1, out->time.micros);
    RESOLVE_REQUIRED(MODULE_PROC_TIME_DELAY_V1, out->time.delay);
    RESOLVE_REQUIRED(MODULE_PROC_TIME_YIELD_V1, out->time.yield);

    out->heap.size = sizeof(out->heap);
    RESOLVE_REQUIRED(MODULE_PROC_HEAP_MALLOC_V1, out->heap.malloc);
    RESOLVE_REQUIRED(MODULE_PROC_HEAP_CALLOC_V1, out->heap.calloc);
    RESOLVE_REQUIRED(MODULE_PROC_HEAP_REALLOC_V1, out->heap.realloc);
    RESOLVE_REQUIRED(MODULE_PROC_HEAP_FREE_V1, out->heap.free);
    RESOLVE_REQUIRED(MODULE_PROC_HEAP_FREE_SIZE_V1, out->heap.free_size);
    RESOLVE_REQUIRED(MODULE_PROC_HEAP_LARGEST_FREE_BLOCK_V1, out->heap.largest_free_block);

    out->task.size = sizeof(out->task);
    RESOLVE_REQUIRED(MODULE_PROC_TASK_CREATE_V1, out->task.create);
    RESOLVE_REQUIRED(MODULE_PROC_TASK_REMOVE_V1, out->task.remove);
    RESOLVE_REQUIRED(MODULE_PROC_TASK_YIELD_V1, out->task.yield);
    RESOLVE_REQUIRED(MODULE_PROC_TASK_DELAY_V1, out->task.delay);
    RESOLVE_OPTIONAL(MODULE_PROC_TASK_CREATE_EX_V1, out->task.create_ex);

    out->lua.size = sizeof(out->lua);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_GETTOP_V1, out->lua.gettop);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_SETTOP_V1, out->lua.settop);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_TYPE_V1, out->lua.type);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_ISTABLE_V1, out->lua.istable);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_ISNIL_V1, out->lua.isnil);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_ISNUMBER_V1, out->lua.isnumber);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_ISSTRING_V1, out->lua.isstring);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_TOBOOLEAN_V1, out->lua.toboolean);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_TOINTEGER_V1, out->lua.tointeger);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_TONUMBER_V1, out->lua.tonumber);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_TOSTRING_V1, out->lua.tostring);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_CHECKINTEGER_V1, out->lua.checkinteger);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_CHECKNUMBER_V1, out->lua.checknumber);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_CHECKSTRING_V1, out->lua.checkstring);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_TOUSERDATA_V1, out->lua.touserdata);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_PUSHNIL_V1, out->lua.pushnil);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_PUSHBOOLEAN_V1, out->lua.pushboolean);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_PUSHINTEGER_V1, out->lua.pushinteger);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_PUSHNUMBER_V1, out->lua.pushnumber);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_PUSHSTRING_V1, out->lua.pushstring);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_PUSHLIGHTUSERDATA_V1, out->lua.pushlightuserdata);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_PUSHCFUNCTION_V1, out->lua.pushcfunction);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_PUSHCCLOSURE_V1, out->lua.pushcclosure);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_PUSHVALUE_V1, out->lua.pushvalue);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_NEWTABLE_V1, out->lua.newtable);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_CREATETABLE_V1, out->lua.createtable);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_GETFIELD_V1, out->lua.getfield);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_SETFIELD_V1, out->lua.setfield);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_GETGLOBAL_V1, out->lua.getglobal);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_SETGLOBAL_V1, out->lua.setglobal);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_REGISTRY_REF_V1, out->lua.registry_ref);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_REGISTRY_UNREF_V1, out->lua.registry_unref);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_REGISTRY_RAWGETI_V1, out->lua.registry_rawgeti);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_UPVALUE_INDEX_V1, out->lua.upvalue_index);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_ERROR_V1, out->lua.error);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_TOLSTRING_V1, out->lua.tolstring);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_CHECKLSTRING_V1, out->lua.checklstring);
    RESOLVE_REQUIRED(MODULE_PROC_LUA_PUSHLSTRING_V1, out->lua.pushlstring);

    return MODULE_OK;
}

#undef RESOLVE_REQUIRED
#undef RESOLVE_OPTIONAL

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
    if (!s_host || !s_host->lua.touserdata || !s_host->lua.upvalue_index)
    {
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
    if (host->lua.isstring(L, -1))
    {
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
    if (host->lua.isnumber(L, -1))
    {
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

    if (!host)
    {
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

    if (!host)
    {
        return 0;
    }
    create_log(host, "[retrogo.so] set_catalog enter");

    if (host->lua.isstring(L, 1))
    {
        if (host->lua.tolstring)
        {
            blob = host->lua.tolstring(L, 1, &len);
        }
        else
        {
            blob = host->lua.tostring(L, 1);
            len = blob ? strlen(blob) : 0;
        }
    }
    else if (host->lua.istable(L, 1))
    {
        top = host->lua.gettop(L);
        host->lua.getfield(L, 1, "blob");
        if (host->lua.isstring(L, -1))
        {
            if (host->lua.tolstring)
            {
                blob = host->lua.tolstring(L, -1, &len);
            }
            else
            {
                blob = host->lua.tostring(L, -1);
                len = blob ? strlen(blob) : 0;
            }
        }
        host->lua.settop(L, top);
    }
    else if (host->lua.isnil(L, 1))
    {
        holo_catalog_clear();
        host->lua.pushboolean(L, 1);
        return 1;
    }

    if (!blob)
    {
        create_log(host, "[retrogo.so] set_catalog bad arg");
        return push_error(L, host, "retrogo.set_catalog: expected catalog blob or {blob=...}");
    }
    if (!holo_catalog_load_blob(blob, len))
    {
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
    if (!inst)
    {
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
    if (inst->host && inst->host->task.remove)
    {
        inst->host->task.remove(NULL);
    }
    for (;;)
    {
        if (inst->host && inst->host->task.delay)
        {
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

    if (!inst || !host)
    {
        return push_error(L, host, "retrogo.start: instance missing");
    }
    create_log(host, "[retrogo.so] start enter");
    if (inst->running || inst->task)
    {
        create_log(host, "[retrogo.so] start already running");
        return push_error(L, host, "retro-go is already running");
    }

    if (host->lua.gettop(L) >= 1 && host->lua.istable(L, 1))
    {
        (void)read_table_string(L, host, 1, "app", app, sizeof(app));
        (void)read_table_string(L, host, 1, "system", app, sizeof(app));
        (void)read_table_string(L, host, 1, "rom", rom, sizeof(rom));
        (void)read_table_string(L, host, 1, "path", rom, sizeof(rom));
        (void)read_table_integer(L, host, 1, "flags", &flags);
    }
    else if (host->lua.gettop(L) >= 1 && host->lua.isstring(L, 1))
    {
        copy_text(app, sizeof(app), host->lua.tostring(L, 1));
        if (host->lua.gettop(L) >= 2 && host->lua.isstring(L, 2))
        {
            copy_text(rom, sizeof(rom), host->lua.tostring(L, 2));
        }
    }

    holo_launch_set(app, rom, (uint32_t)flags);
    inst->stop_requested = 0;

    if (!host->task.create)
    {
        create_log(host, "[retrogo.so] start no task api");
        return push_error(L, host, "host task API is missing");
    }
    create_log(host, "[retrogo.so] start task.create");
    if (host->task.create_ex)
    {
        if (host->task.create_ex("retrogo", retrogo_task_entry, inst, 24u * 1024u, 3u, 1,
                                 MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT, &inst->task) != MODULE_OK)
        {
            inst->task = NULL;
        }
    }
    else if (host->task.create("retrogo", retrogo_task_entry, inst, 24u * 1024u, 3u, 1, &inst->task) != MODULE_OK)
    {
        inst->task = NULL;
    }
    if (!inst->task)
    {
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

    if (!inst || !host)
    {
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

    if (!host)
    {
        return 0;
    }
    mask = host->lua.checkinteger(L, 1);
    if (mask < 0)
    {
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

    if (!inst || !host)
    {
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
    if (!out_instance || !host)
    {
        return MODULE_ERR_INVALID_ARG;
    }
    *out_instance = NULL;
    create_log(host, "[retrogo.so] create validate");
    if (!host_abi_is_compatible(host))
    {
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

RETROGO_MODULE_EXPORT int32_t module_create_v2(module_host_resolve_v1_fn resolve,
                                               void *resolve_ctx,
                                               const module_open_info_t *info,
                                               void **out_instance)
{
    int32_t err = resolve_host_api(resolve, resolve_ctx, &s_resolved_host);
    if (err != MODULE_OK)
    {
        return err;
    }
    return module_create_v1(&s_resolved_host, info, out_instance);
}

RETROGO_MODULE_EXPORT int32_t module_luaopen_v1(void *instance, lua_State *L)
{
    retrogo_instance_t *inst = (retrogo_instance_t *)instance;
    const module_host_api_v1 *host = inst ? inst->host : s_host;

    if (!inst || !host || !L)
    {
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

    if (!inst || !host)
    {
        return;
    }
    holo_runtime_request_stop();
    for (int i = 0; inst->running && i < 100; ++i)
    {
        if (host->task.delay)
        {
            host->task.delay(10);
        }
        else if (host->time.delay)
        {
            host->time.delay(10);
        }
    }
    holo_catalog_clear();
    holo_port_log("[retrogo.so] destroy");
    if (inst->running)
    {
        holo_port_log("[retrogo.so] destroy deferred; task still running, force release display");
        holo_display_release();
        return;
    }
    if (inst != &s_static_instance && host->heap.free)
    {
        host->heap.free(inst);
    }
}
