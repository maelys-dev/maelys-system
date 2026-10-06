/*
 * Directory watching: inotify on Linux, a kqueue of EVFILT_VNODE on macOS.
 * Both are asked for the same thing and nothing more, the changes of a
 * directory's own entries, so the two hosts answer alike: see
 * maelys/sys/dirwatch.h for what is and is not promised.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE /* O_EVTONLY */
#endif

#include "maelys/sys/dirwatch.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/inotify.h>
#define DIRWATCH_INOTIFY 1
#define DIRWATCH_KQUEUE 0
#elif defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#define DIRWATCH_INOTIFY 0
#define DIRWATCH_KQUEUE 1
#else
#define DIRWATCH_INOTIFY 0
#define DIRWATCH_KQUEUE 0
#endif

/*
 * Fault seam of the white-box test: a step that returns non-zero with errno
 * set fails before its system call runs; "overflow" makes the next poll act
 * as if the kernel had dropped events, which only inotify does by itself.
 * Production builds compile it away.
 */
#ifdef MAELYS_SYS_DIRWATCH_TESTING
static int dirwatch_fault(const char *step);
#define DIRWATCH_FAULT(step) (dirwatch_fault(step) != 0)
#else
#define DIRWATCH_FAULT(step) 0
#endif

#if DIRWATCH_INOTIFY
/* The entries of the directory, and what makes the directory itself go. No
 * IN_CLOSE_WRITE, IN_MODIFY or IN_ATTRIB: they would report what kqueue
 * cannot, a write in place and a change of metadata. */
#define DIRWATCH_ENTRIES (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)
#define DIRWATCH_SELF (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)
#define DIRWATCH_MASK (DIRWATCH_ENTRIES | DIRWATCH_SELF)
#endif
#if DIRWATCH_KQUEUE
/* No NOTE_ATTRIB: it reports a change of the directory's own metadata,
 * which is not a change of its entries. */
#define DIRWATCH_ENTRIES (NOTE_WRITE | NOTE_LINK)
#define DIRWATCH_SELF (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE)
#define DIRWATCH_NOTES (DIRWATCH_ENTRIES | DIRWATCH_SELF)
#endif

typedef struct dirwatch_entry {
    maelys_sys_dirwatch_entry_t id; /* 0: the slot is free */
    maelys_sys_token_t token;
    unsigned pending;
#if DIRWATCH_INOTIFY
    int wd;
#elif DIRWATCH_KQUEUE
    int fd;
    dev_t device;
    ino_t inode;
#endif
} dirwatch_entry_t;

struct maelys_sys_dirwatch {
    pthread_t owner;
    int kernel_fd;
    size_t capacity;
    size_t cursor; /* where the next poll starts serving, so entries take turns */
    maelys_sys_dirwatch_entry_t next_id;
    dirwatch_entry_t *entries;
};

#if DIRWATCH_INOTIFY || DIRWATCH_KQUEUE

static int owned(const maelys_sys_dirwatch_t *dirwatch) {
    return pthread_equal(dirwatch->owner, pthread_self());
}

static dirwatch_entry_t *free_slot(maelys_sys_dirwatch_t *dirwatch) {
    for (size_t index = 0; index < dirwatch->capacity; ++index) {
        if (!dirwatch->entries[index].id) return &dirwatch->entries[index];
    }
    return NULL;
}

/* Gives the kernel its registration back and frees the slot. */
static void release_entry(maelys_sys_dirwatch_t *dirwatch, dirwatch_entry_t *entry) {
#if DIRWATCH_INOTIFY
    /* EINVAL when the kernel dropped the watch itself, with the directory. */
    (void)inotify_rm_watch(dirwatch->kernel_fd, entry->wd);
#else
    (void)dirwatch;
    (void)close(entry->fd); /* closing the descriptor removes its kevent */
#endif
    memset(entry, 0, sizeof(*entry));
}

/* Why the kernel refused the path: missing, a link, not a directory, or its
 * own reason, kept in errno. */
static maelys_sys_result_t path_failure(const char *path, int error) {
    if (error == ENOENT) return MAELYS_SYS_ERR_NOT_FOUND;
    if (error == ENOTDIR || error == ELOOP) {
        struct stat status;
        if (lstat(path, &status) != 0) {
            if (errno == ENOENT) return MAELYS_SYS_ERR_NOT_FOUND;
            errno = error;
            return MAELYS_SYS_ERR_OS;
        }
        if (S_ISLNK(status.st_mode)) return MAELYS_SYS_ERR_IDENTITY;
        if (!S_ISDIR(status.st_mode)) return MAELYS_SYS_ERR_ARGUMENT;
    }
    errno = error;
    return MAELYS_SYS_ERR_OS;
}

static void mark_overflow(maelys_sys_dirwatch_t *dirwatch) {
    for (size_t index = 0; index < dirwatch->capacity; ++index) {
        if (dirwatch->entries[index].id) {
            dirwatch->entries[index].pending |=
                MAELYS_SYS_DIRWATCH_CHANGED | MAELYS_SYS_DIRWATCH_OVERFLOW;
        }
    }
}

#endif

maelys_sys_result_t maelys_sys_dirwatch_create(
    size_t entry_capacity,
    maelys_sys_dirwatch_t **out_dirwatch) {
    if (out_dirwatch) *out_dirwatch = NULL;
    if (!out_dirwatch || !entry_capacity) return MAELYS_SYS_ERR_ARGUMENT;
#if DIRWATCH_INOTIFY || DIRWATCH_KQUEUE
    maelys_sys_dirwatch_t *dirwatch = calloc(1, sizeof(*dirwatch));
    if (!dirwatch) return MAELYS_SYS_ERR_MEMORY;
    dirwatch->entries = calloc(entry_capacity, sizeof(*dirwatch->entries));
    if (!dirwatch->entries) {
        free(dirwatch);
        return MAELYS_SYS_ERR_MEMORY;
    }
#if DIRWATCH_INOTIFY
    int fd = DIRWATCH_FAULT("create") ? -1 : inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
#else
    int fd = DIRWATCH_FAULT("create") ? -1 : kqueue();
    if (fd >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        int saved = errno;
        (void)close(fd);
        errno = saved;
        fd = -1;
    }
#endif
    if (fd < 0) {
        int saved = errno;
        free(dirwatch->entries);
        free(dirwatch);
        errno = saved;
        return saved == ENOSYS ? MAELYS_SYS_ERR_UNSUPPORTED : MAELYS_SYS_ERR_OS;
    }
    dirwatch->kernel_fd = fd;
    dirwatch->capacity = entry_capacity;
    dirwatch->next_id = 1;
    dirwatch->owner = pthread_self();
    *out_dirwatch = dirwatch;
    return MAELYS_SYS_OK;
#else
    return MAELYS_SYS_ERR_UNSUPPORTED;
#endif
}

int maelys_sys_dirwatch_fd(const maelys_sys_dirwatch_t *dirwatch) {
    return dirwatch ? dirwatch->kernel_fd : -1;
}

maelys_sys_result_t maelys_sys_dirwatch_add(
    maelys_sys_dirwatch_t *dirwatch,
    const char *path,
    maelys_sys_token_t token,
    maelys_sys_dirwatch_entry_t *out_entry) {
    if (out_entry) *out_entry = 0;
    if (!dirwatch || !path || !out_entry) return MAELYS_SYS_ERR_ARGUMENT;
#if DIRWATCH_INOTIFY || DIRWATCH_KQUEUE
    if (!owned(dirwatch)) return MAELYS_SYS_ERR_STATE;
    dirwatch_entry_t *slot = free_slot(dirwatch);
#if DIRWATCH_INOTIFY
    int wd = DIRWATCH_FAULT("register") ? -1 : inotify_add_watch(dirwatch->kernel_fd,
        path, DIRWATCH_MASK | IN_DONT_FOLLOW | IN_ONLYDIR | IN_EXCL_UNLINK);
    if (wd < 0) return path_failure(path, errno);
    /* The kernel answers by inode: for a directory this handle already
     * watches, by whatever path, it hands back the watch it has. Every entry
     * asks for the same mask, so that call changed nothing. */
    for (size_t index = 0; index < dirwatch->capacity; ++index) {
        if (dirwatch->entries[index].id && dirwatch->entries[index].wd == wd) {
            return MAELYS_SYS_ERR_EXISTS;
        }
    }
    if (!slot) {
        (void)inotify_rm_watch(dirwatch->kernel_fd, wd);
        return MAELYS_SYS_ERR_CAPACITY;
    }
    slot->wd = wd;
#else
    int fd = DIRWATCH_FAULT("open") ? -1 :
        open(path, O_EVTONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return path_failure(path, errno);
    struct stat status;
    if (fstat(fd, &status) != 0) {
        int saved = errno;
        (void)close(fd);
        errno = saved;
        return MAELYS_SYS_ERR_OS;
    }
    /* kqueue would take the same directory twice, through two descriptors:
     * the identity of the one just opened decides. */
    for (size_t index = 0; index < dirwatch->capacity; ++index) {
        const dirwatch_entry_t *entry = &dirwatch->entries[index];
        if (entry->id && entry->device == status.st_dev && entry->inode == status.st_ino) {
            (void)close(fd);
            return MAELYS_SYS_ERR_EXISTS;
        }
    }
    if (!slot) {
        (void)close(fd);
        return MAELYS_SYS_ERR_CAPACITY;
    }
    struct kevent change;
    memset(&change, 0, sizeof(change));
    change.ident = (uintptr_t)fd;
    change.filter = EVFILT_VNODE;
    change.flags = EV_ADD | EV_CLEAR;
    change.fflags = DIRWATCH_NOTES;
    if (DIRWATCH_FAULT("register") ||
        kevent(dirwatch->kernel_fd, &change, 1, NULL, 0, NULL) != 0) {
        int saved = errno;
        (void)close(fd);
        errno = saved;
        return MAELYS_SYS_ERR_OS;
    }
    slot->fd = fd;
    slot->device = status.st_dev;
    slot->inode = status.st_ino;
#endif
    slot->id = dirwatch->next_id++;
    slot->token = token;
    slot->pending = 0;
    *out_entry = slot->id;
    return MAELYS_SYS_OK;
#else
    (void)token;
    return MAELYS_SYS_ERR_UNSUPPORTED;
#endif
}

maelys_sys_result_t maelys_sys_dirwatch_remove(
    maelys_sys_dirwatch_t *dirwatch,
    maelys_sys_dirwatch_entry_t entry) {
    if (!dirwatch) return MAELYS_SYS_ERR_ARGUMENT;
#if DIRWATCH_INOTIFY || DIRWATCH_KQUEUE
    if (!owned(dirwatch)) return MAELYS_SYS_ERR_STATE;
    if (entry) {
        for (size_t index = 0; index < dirwatch->capacity; ++index) {
            if (dirwatch->entries[index].id == entry) {
                release_entry(dirwatch, &dirwatch->entries[index]);
                return MAELYS_SYS_OK;
            }
        }
    }
    return MAELYS_SYS_ERR_NOT_FOUND;
#else
    (void)entry;
    return MAELYS_SYS_ERR_UNSUPPORTED;
#endif
}

#if DIRWATCH_INOTIFY
/* Reads everything the kernel queued into the pending bits. The names in
 * the records only serve to step over them. */
static maelys_sys_result_t take_kernel_events(maelys_sys_dirwatch_t *dirwatch) {
    _Alignas(struct inotify_event) char buffer[4096];
    for (;;) {
        ssize_t length = DIRWATCH_FAULT("read") ? -1 :
            read(dirwatch->kernel_fd, buffer, sizeof(buffer));
        if (length < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return MAELYS_SYS_OK;
            return MAELYS_SYS_ERR_OS;
        }
        if (length == 0) return MAELYS_SYS_OK;
        size_t offset = 0;
        while ((size_t)length - offset >= sizeof(struct inotify_event)) {
            const struct inotify_event *event =
                (const struct inotify_event *)(const void *)(buffer + offset);
            size_t size = sizeof(*event) + event->len;
            if (size > (size_t)length - offset) break;
            offset += size;
            if (event->mask & IN_Q_OVERFLOW) {
                mark_overflow(dirwatch);
                continue;
            }
            for (size_t index = 0; index < dirwatch->capacity; ++index) {
                dirwatch_entry_t *entry = &dirwatch->entries[index];
                if (!entry->id || entry->wd != event->wd) continue;
                /* IN_IGNORED: the kernel let the watch go, for a reason it
                 * may not have told otherwise. Never a silent release. */
                if (event->mask & (DIRWATCH_SELF | IN_IGNORED)) {
                    entry->pending |= MAELYS_SYS_DIRWATCH_GONE;
                }
                if (event->mask & DIRWATCH_ENTRIES) {
                    entry->pending |= MAELYS_SYS_DIRWATCH_CHANGED;
                }
                break;
            }
        }
    }
}
#endif

#if DIRWATCH_KQUEUE
/* Takes every event the kqueue holds, in bounded batches, into the pending
 * bits. The kernel already merged what happened to one descriptor. */
static maelys_sys_result_t take_kernel_events(maelys_sys_dirwatch_t *dirwatch) {
    enum { BATCH = 64 };
    struct kevent events[BATCH];
    const struct timespec no_wait = {0, 0};
    for (;;) {
        int count = DIRWATCH_FAULT("read") ? -1 :
            kevent(dirwatch->kernel_fd, NULL, 0, events, BATCH, &no_wait);
        if (count < 0) {
            if (errno == EINTR) continue;
            return MAELYS_SYS_ERR_OS;
        }
        for (int position = 0; position < count; ++position) {
            if (events[position].filter != EVFILT_VNODE) continue;
            for (size_t index = 0; index < dirwatch->capacity; ++index) {
                dirwatch_entry_t *entry = &dirwatch->entries[index];
                if (!entry->id || (uintptr_t)entry->fd != events[position].ident) continue;
                if (events[position].fflags & DIRWATCH_SELF) {
                    entry->pending |= MAELYS_SYS_DIRWATCH_GONE;
                }
                if (events[position].fflags & DIRWATCH_ENTRIES) {
                    entry->pending |= MAELYS_SYS_DIRWATCH_CHANGED;
                }
                break;
            }
        }
        if (count < BATCH) return MAELYS_SYS_OK;
    }
}
#endif

maelys_sys_result_t maelys_sys_dirwatch_poll(
    maelys_sys_dirwatch_t *dirwatch,
    maelys_sys_dirwatch_change_t *changes,
    size_t capacity,
    size_t *out_count) {
    if (out_count) *out_count = 0;
    if (!dirwatch || !changes || !capacity || !out_count) return MAELYS_SYS_ERR_ARGUMENT;
#if DIRWATCH_INOTIFY || DIRWATCH_KQUEUE
    if (!owned(dirwatch)) return MAELYS_SYS_ERR_STATE;
    if (DIRWATCH_FAULT("overflow")) mark_overflow(dirwatch);
    maelys_sys_result_t taken = take_kernel_events(dirwatch);
    if (taken != MAELYS_SYS_OK) return taken;
    /* Serve from where the last call stopped: an entry that changes again
     * between two calls does not keep the others waiting. */
    size_t produced = 0;
    size_t last = dirwatch->cursor;
    for (size_t visited = 0; visited < dirwatch->capacity && produced < capacity; ++visited) {
        size_t index = (dirwatch->cursor + visited) % dirwatch->capacity;
        dirwatch_entry_t *entry = &dirwatch->entries[index];
        if (!entry->id || !entry->pending) continue;
        changes[produced].token = entry->token;
        changes[produced].entry = entry->id;
        changes[produced].flags = entry->pending;
        ++produced;
        last = index;
        if (entry->pending & MAELYS_SYS_DIRWATCH_GONE) release_entry(dirwatch, entry);
        else entry->pending = 0;
    }
    if (!produced) return MAELYS_SYS_ERR_WOULD_BLOCK;
    dirwatch->cursor = (last + 1) % dirwatch->capacity;
    *out_count = produced;
    return MAELYS_SYS_OK;
#else
    return MAELYS_SYS_ERR_UNSUPPORTED;
#endif
}

maelys_sys_result_t maelys_sys_dirwatch_destroy(maelys_sys_dirwatch_t **dirwatch) {
    if (!dirwatch || !*dirwatch) return MAELYS_SYS_OK;
#if DIRWATCH_INOTIFY || DIRWATCH_KQUEUE
    maelys_sys_dirwatch_t *handle = *dirwatch;
    if (!owned(handle)) return MAELYS_SYS_ERR_STATE;
    for (size_t index = 0; index < handle->capacity; ++index) {
        if (handle->entries[index].id) release_entry(handle, &handle->entries[index]);
    }
    (void)close(handle->kernel_fd);
    free(handle->entries);
    free(handle);
    *dirwatch = NULL;
#endif
    return MAELYS_SYS_OK;
}
