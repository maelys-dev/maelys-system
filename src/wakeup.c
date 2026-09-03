#include "src/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

struct maelys_sys_wakeup {
    int read_fd;
    int write_fd;
    pthread_mutex_t mutex;
    int pending;
};

maelys_sys_result_t maelys_sys_wakeup_create(
    maelys_sys_wakeup_t **out_wakeup) {
    if (!out_wakeup) return MAELYS_SYS_ERR_ARGUMENT;
    *out_wakeup = NULL;
    maelys_sys_wakeup_t *wakeup = calloc(1, sizeof(*wakeup));
    if (!wakeup) return MAELYS_SYS_ERR_MEMORY;
    wakeup->read_fd = -1;
    wakeup->write_fd = -1;
    int pair[2] = {-1, -1};
    maelys_sys_result_t result = maelys_sys_pipe_cloexec(pair);
    if (result != MAELYS_SYS_OK) {
        free(wakeup);
        return result;
    }
    wakeup->read_fd = pair[0];
    wakeup->write_fd = pair[1];
    if (maelys_sys_fd_set_nonblocking(wakeup->read_fd) != MAELYS_SYS_OK ||
        maelys_sys_fd_set_nonblocking(wakeup->write_fd) != MAELYS_SYS_OK) {
        int saved = errno;
        (void)maelys_sys_fd_close(&wakeup->read_fd);
        (void)maelys_sys_fd_close(&wakeup->write_fd);
        free(wakeup);
        errno = saved;
        return MAELYS_SYS_ERR_OS;
    }
    int status = pthread_mutex_init(&wakeup->mutex, NULL);
    if (status != 0) {
        (void)maelys_sys_fd_close(&wakeup->read_fd);
        (void)maelys_sys_fd_close(&wakeup->write_fd);
        free(wakeup);
        errno = status;
        return MAELYS_SYS_ERR_OS;
    }
    *out_wakeup = wakeup;
    return MAELYS_SYS_OK;
}

int maelys_sys_wakeup_fd(const maelys_sys_wakeup_t *wakeup) {
    return wakeup ? wakeup->read_fd : -1;
}

maelys_sys_result_t maelys_sys_wakeup_signal(maelys_sys_wakeup_t *wakeup) {
    if (!wakeup) return MAELYS_SYS_ERR_ARGUMENT;
    int status = pthread_mutex_lock(&wakeup->mutex);
    if (status != 0) {
        errno = status;
        return MAELYS_SYS_ERR_OS;
    }
    maelys_sys_result_t result = MAELYS_SYS_OK;
    if (!wakeup->pending) {
        unsigned char byte = 1;
        ssize_t written;
        do {
            written = write(wakeup->write_fd, &byte, 1);
        } while (written < 0 && errno == EINTR);
        if (written == 1) {
            wakeup->pending = 1;
        } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            wakeup->pending = 1;
        } else {
            result = errno == EPIPE ? MAELYS_SYS_ERR_CLOSED : MAELYS_SYS_ERR_OS;
        }
    }
    status = pthread_mutex_unlock(&wakeup->mutex);
    if (status != 0 && result == MAELYS_SYS_OK) {
        errno = status;
        result = MAELYS_SYS_ERR_OS;
    }
    return result;
}

maelys_sys_result_t maelys_sys_wakeup_consume(maelys_sys_wakeup_t *wakeup) {
    if (!wakeup) return MAELYS_SYS_ERR_ARGUMENT;
    int status = pthread_mutex_lock(&wakeup->mutex);
    if (status != 0) {
        errno = status;
        return MAELYS_SYS_ERR_OS;
    }
    maelys_sys_result_t result = MAELYS_SYS_OK;
    unsigned char buffer[64];
    for (;;) {
        ssize_t got = read(wakeup->read_fd, buffer, sizeof(buffer));
        if (got > 0) continue;
        if (got < 0 && errno == EINTR) continue;
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (got == 0) result = MAELYS_SYS_ERR_CLOSED;
        else result = MAELYS_SYS_ERR_OS;
        break;
    }
    if (result == MAELYS_SYS_OK) wakeup->pending = 0;
    status = pthread_mutex_unlock(&wakeup->mutex);
    if (status != 0 && result == MAELYS_SYS_OK) {
        errno = status;
        result = MAELYS_SYS_ERR_OS;
    }
    return result;
}

void maelys_sys_wakeup_destroy(maelys_sys_wakeup_t *wakeup) {
    if (!wakeup) return;
    (void)maelys_sys_fd_close(&wakeup->read_fd);
    (void)maelys_sys_fd_close(&wakeup->write_fd);
    (void)pthread_mutex_destroy(&wakeup->mutex);
    free(wakeup);
}

