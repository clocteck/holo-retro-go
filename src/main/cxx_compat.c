#include <stddef.h>
#include <stdlib.h>

static void *cxx_alloc(size_t size)
{
    void *ptr = malloc(size ? size : 1);
    if (!ptr) {
        abort();
    }
    return ptr;
}

void *_Znwj(size_t size)
{
    return cxx_alloc(size);
}

void *_Znaj(size_t size)
{
    return cxx_alloc(size);
}

void _ZdlPv(void *ptr)
{
    free(ptr);
}

void _ZdaPv(void *ptr)
{
    free(ptr);
}

void _ZdlPvj(void *ptr, size_t size)
{
    (void)size;
    free(ptr);
}

void _ZdaPvj(void *ptr, size_t size)
{
    (void)size;
    free(ptr);
}

void __cxa_pure_virtual(void)
{
    abort();
}
