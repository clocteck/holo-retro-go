#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../upstream/retro-go/components/retro-go/rg_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

void holo_catalog_clear(void);
int holo_catalog_load_blob(const char *blob, size_t len);
int holo_catalog_ready(void);
void holo_catalog_info(size_t *entries, size_t *dirs, size_t *files);

int holo_catalog_stat(const char *path, rg_stat_t *out);
int holo_catalog_exists(const char *path);
bool holo_catalog_scandir(const char *path, rg_scandir_cb_t *callback, void *arg, uint32_t flags);

#ifdef __cplusplus
}
#endif
