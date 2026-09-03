#include "maelys/sys.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d check failed: %s\n", \
            __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int expect_resource_failure(maelys_sys_loop_backend_t backend) {
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct rlimit limit = {.rlim_cur = 32, .rlim_max = 32};
        if (setrlimit(RLIMIT_NOFILE, &limit) != 0) _exit(10);
        int descriptors[64];
        size_t count = 0;
        while (count < 64) {
            int fd = open("/dev/null", O_RDONLY);
            if (fd < 0) break;
            descriptors[count++] = fd;
        }
        maelys_sys_loop_t *loop = (maelys_sys_loop_t *)(uintptr_t)1;
        maelys_sys_result_t result = maelys_sys_loop_create(backend, &loop);
        for (size_t i = 0; i < count; ++i) close(descriptors[i]);
        _exit(result != MAELYS_SYS_OK && loop == NULL ? 0 : 11);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return 0;
}

static int stale_fd_identity(void) {
    maelys_sys_loop_t *loop = NULL;
    int first[2] = {-1, -1};
    CHECK(maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) == MAELYS_SYS_OK);
    CHECK(maelys_sys_socketpair_cloexec(SOCK_STREAM, first) == MAELYS_SYS_OK);
    int reused_target = first[0];
    maelys_sys_watch_t stale = 0;
    CHECK(maelys_sys_loop_watch_fd(loop, first[0], MAELYS_SYS_INTEREST_READ,
        1, &stale) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_unwatch(loop, stale) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&first[0]) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&first[1]) == MAELYS_SYS_OK);

    int second[2] = {-1, -1};
    CHECK(maelys_sys_socketpair_cloexec(SOCK_STREAM, second) == MAELYS_SYS_OK);
    CHECK(second[0] == reused_target || second[1] == reused_target);
    int watched = second[0] == reused_target ? second[0] : second[1];
    maelys_sys_watch_t current = 0;
    CHECK(maelys_sys_loop_watch_fd(loop, watched, MAELYS_SYS_INTEREST_READ,
        2, &current) == MAELYS_SYS_OK);
    CHECK(current != stale);
    CHECK(maelys_sys_loop_modify(loop, stale, MAELYS_SYS_INTEREST_WRITE) ==
        MAELYS_SYS_ERR_NOT_FOUND);
    CHECK(maelys_sys_loop_unwatch(loop, stale) == MAELYS_SYS_ERR_NOT_FOUND);
    CHECK(maelys_sys_loop_unwatch(loop, current) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&second[0]) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&second[1]) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK);
    return 0;
}

int main(void) {
    CHECK(maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, NULL) ==
        MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_loop_wake(NULL) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(expect_resource_failure(MAELYS_SYS_LOOP_POLL) == 0);
    CHECK(expect_resource_failure(MAELYS_SYS_LOOP_AUTO) == 0);
    CHECK(stale_fd_identity() == 0);
    puts("ok - resource exhaustion is fail-closed");
    puts("ok - stale descriptor identity is rejected");
    return 0;
}
