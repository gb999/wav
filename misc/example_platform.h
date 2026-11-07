#ifndef EXAMPLE_PLATFORM_H
#define EXAMPLE_PLATFORM_H

// Files, threads and memory maps for example.c, on POSIX and Windows. wav.h itself uses none of this.
// The Windows half is not built or run by `make`.

#include "wav.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct plat_file {
    HANDLE handle;
} plat_file;

typedef struct plat_thread {
    HANDLE handle;
    void *(*fn)(void *);
    void *arg;
} plat_thread;

typedef struct plat_map {
    const uint8_t *data;
    uint64_t size;
    HANDLE file;
    HANDLE mapping;
} plat_map;

static bool plat_open(plat_file *file, const char *path, bool write) {
    DWORD access = write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
    DWORD creation = write ? CREATE_ALWAYS : OPEN_EXISTING;
    file->handle = CreateFileA(path, access, FILE_SHARE_READ, NULL, creation, FILE_ATTRIBUTE_NORMAL, NULL);
    return file->handle != INVALID_HANDLE_VALUE;
}

static void plat_close(plat_file *file) {
    CloseHandle(file->handle);
}

static uint64_t plat_size(const plat_file *file) {
    LARGE_INTEGER size;
    if(!GetFileSizeEx(file->handle, &size)) {
        return WAV_UNKNOWN_SIZE;
    }
    return (uint64_t)size.QuadPart;
}

static int64_t plat_read(void *context, uint64_t offset, void *buffer, size_t size) {
    OVERLAPPED position = {0};
    DWORD done = 0;
    position.Offset = (DWORD)offset;
    position.OffsetHigh = (DWORD)(offset >> 32);
    if(!ReadFile(((plat_file *)context)->handle, buffer, size > 0x40000000 ? 0x40000000 : (DWORD)size, &done, &position)) {
        return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    }
    return (int64_t)done;
}

static int64_t plat_write(void *context, uint64_t offset, const void *buffer, size_t size) {
    OVERLAPPED position = {0};
    DWORD done = 0;
    position.Offset = (DWORD)offset;
    position.OffsetHigh = (DWORD)(offset >> 32);
    if(!WriteFile(((plat_file *)context)->handle, buffer, size > 0x40000000 ? 0x40000000 : (DWORD)size, &done, &position)) {
        return -1;
    }
    return (int64_t)done;
}

static DWORD WINAPI plat_thread_main(LPVOID context) {
    plat_thread *thread = (plat_thread *)context;
    thread->fn(thread->arg);
    return 0;
}

static void plat_thread_start(plat_thread *thread, void *(*fn)(void *), void *arg) {
    thread->fn = fn;
    thread->arg = arg;
    thread->handle = CreateThread(NULL, 0, plat_thread_main, thread, 0, NULL);
}

static void plat_thread_join(plat_thread *thread) {
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
}

static bool plat_map_open(plat_map *map, const char *path) {
    plat_file file;
    if(!plat_open(&file, path, false)) {
        return false;
    }
    map->file = file.handle;
    map->size = plat_size(&file);
    map->mapping = CreateFileMappingA(map->file, NULL, PAGE_READONLY, 0, 0, NULL);
    map->data = map->mapping ? (const uint8_t *)MapViewOfFile(map->mapping, FILE_MAP_READ, 0, 0, 0) : NULL;
    if(!map->data) {
        if(map->mapping) {
            CloseHandle(map->mapping);
        }
        CloseHandle(map->file);
        return false;
    }
    return true;
}

static void plat_map_close(plat_map *map) {
    UnmapViewOfFile(map->data);
    CloseHandle(map->mapping);
    CloseHandle(map->file);
}

#else

#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct plat_file {
    int fd;
} plat_file;

typedef struct plat_thread {
    pthread_t thread;
} plat_thread;

typedef struct plat_map {
    const uint8_t *data;
    uint64_t size;
} plat_map;

static bool plat_open(plat_file *file, const char *path, bool write) {
    file->fd = write ? open(path, O_CREAT | O_RDWR | O_TRUNC, 0644) : open(path, O_RDONLY);
    return file->fd >= 0;
}

static void plat_close(plat_file *file) {
    close(file->fd);
}

static uint64_t plat_size(const plat_file *file) {
    struct stat st;
    if(fstat(file->fd, &st) != 0) {
        return WAV_UNKNOWN_SIZE;
    }
    return (uint64_t)st.st_size;
}

static int64_t plat_read(void *context, uint64_t offset, void *buffer, size_t size) {
    return (int64_t)pread(((plat_file *)context)->fd, buffer, size, (off_t)offset);
}

static int64_t plat_write(void *context, uint64_t offset, const void *buffer, size_t size) {
    return (int64_t)pwrite(((plat_file *)context)->fd, buffer, size, (off_t)offset);
}

static void plat_thread_start(plat_thread *thread, void *(*fn)(void *), void *arg) {
    pthread_create(&thread->thread, NULL, fn, arg);
}

static void plat_thread_join(plat_thread *thread) {
    pthread_join(thread->thread, NULL);
}

static bool plat_map_open(plat_map *map, const char *path) {
    plat_file file;
    if(!plat_open(&file, path, false)) {
        return false;
    }
    map->size = plat_size(&file);
    void *data = mmap(NULL, (size_t)map->size, PROT_READ, MAP_SHARED, file.fd, 0);
    plat_close(&file);
    map->data = (data == MAP_FAILED) ? NULL : (const uint8_t *)data;
    return map->data != NULL;
}

static void plat_map_close(plat_map *map) {
    munmap((void *)map->data, (size_t)map->size);
}

#endif

static wav_io plat_io(plat_file *file) {
    wav_io io = {file, plat_read, plat_write};
    return io;
}

#endif // EXAMPLE_PLATFORM_H
