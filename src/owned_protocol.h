/*
 * The wire between a host and its keeper: records of 64 bytes on a stream
 * socket, one request or one report each; the GO record is followed by a
 * body that describes the program. Shared by src/owned.c and the keeper,
 * same machine, same build: no byte order, no padding to agree on beyond
 * the fixed layout. Not installed; the protocol number is what a keeper
 * and a library compare.
 */
#ifndef MAELYS_SYS_OWNED_PROTOCOL_H
#define MAELYS_SYS_OWNED_PROTOCOL_H

#include <stdint.h>

#define OWNED_MAGIC 0x4d534f57u /* "MSOW" */
#define OWNED_PROTOCOL 1u
#define OWNED_RECORD_SIZE 64u
#define OWNED_BODY_MAX (1u << 20)
#define OWNED_PENDING_MAX 16u
#define OWNED_RESULTS_KEPT 16u
#define OWNED_STOP_SEQUENCES 16u /* the last sequence numbers, kept for stops */

/* Descriptors the keeper is started with. */
#define OWNED_FD_LIFE 3
#define OWNED_FD_CONTROL 4
#define OWNED_FD_SOURCES 5

enum owned_type {
    /* keeper -> host */
    OWNED_K_HELLO = 1,       /* a: protocol */
    OWNED_K_ARMED = 2,
    OWNED_K_STARTED = 3,     /* pid: X */
    OWNED_K_EXEC_FAILED = 4, /* value: errno */
    OWNED_K_LOST = 5,        /* value: errno-like cause */
    OWNED_K_EXITED = 6,      /* a: exited, b: exit_code or term_signal, value: signaled */
    OWNED_K_PHASE = 7,       /* a: phase, b: flags (OWNED_F_*), value: keeper_failure */
    OWNED_K_ACK = 8,         /* seq, value: result of the request */
    OWNED_K_BEAT = 9,
    OWNED_K_DONE = 10,       /* a: final phase, b: flags, value: keeper_failure */
    /* host -> keeper */
    OWNED_H_GO = 32,         /* length: body length; a body follows */
    OWNED_H_ABORT = 33,      /* seq */
    OWNED_H_SIGNAL = 34,     /* seq, value: signo, a: to_group */
    OWNED_H_STOP = 35,       /* seq, length: grace_ms */
    OWNED_H_TRANSFER = 36,   /* seq: refused ENOTSUP */
    OWNED_H_DONE_ACK = 37
};

/* Flags of PHASE and DONE. */
#define OWNED_F_LEADER_UNREAPED 1u
#define OWNED_F_SIGNAL_REFUSED 2u
#define OWNED_F_MEMBERS_PRESENT 4u

/* Exit codes of the keeper: its own terminal state, decoded by the host
 * when no DONE was read. Anything else is a loss. */
#define OWNED_EXIT_ENDED 0
#define OWNED_EXIT_UNCERTAIN 1
#define OWNED_EXIT_NOT_STARTED 2
#define OWNED_EXIT_FAILED_CLOSED 3

typedef struct owned_record {
    uint32_t magic;
    uint16_t protocol;
    uint16_t type;
    uint32_t seq;
    int32_t value;
    int32_t pid;
    uint32_t a;
    uint32_t b;
    uint64_t length;
    uint8_t pad[OWNED_RECORD_SIZE - 40];
} owned_record_t;

/* The head of the GO body; the strings follow it, each NUL-terminated:
 * path, argv[argc], envp[envc], cwd (empty: none); then fdc int32
 * targets, the sources being OWNED_FD_SOURCES + index in the keeper. */
typedef struct owned_go_head {
    uint32_t argc;
    uint32_t envc;
    uint32_t fdc;
    uint32_t flags;             /* maelys_sys_owned_flags */
    int32_t owner_pid;
    int32_t reserved;
    uint64_t owner_birth;
    uint64_t owner_boot;
    uint64_t heartbeat_ms;
    uint64_t observe_ms;
    uint64_t linger_ms;
    uint64_t start_deadline_ms; /* absolute monotone, the host's clock being the keeper's */
    uint64_t strings_length;
} owned_go_head_t;

#endif
