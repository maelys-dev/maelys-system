#include "maelys/sys/result.h"
#include "maelys/sys/version.h"

const char *maelys_sys_version_string(void) {
    return MAELYS_SYS_VERSION;
}

unsigned int maelys_sys_abi_version(void) {
    return MAELYS_SYS_ABI_VERSION;
}

const char *maelys_sys_result_string(maelys_sys_result_t result) {
    switch (result) {
        case MAELYS_SYS_OK: return "ok";
        case MAELYS_SYS_ERR_ARGUMENT: return "invalid argument";
        case MAELYS_SYS_ERR_MEMORY: return "out of memory";
        case MAELYS_SYS_ERR_OS: return "operating system error";
        case MAELYS_SYS_ERR_TIMEOUT: return "timeout";
        case MAELYS_SYS_ERR_CLOSED: return "closed";
        case MAELYS_SYS_ERR_STATE: return "invalid state";
        case MAELYS_SYS_ERR_NOT_FOUND: return "not found";
        case MAELYS_SYS_ERR_CAPACITY: return "capacity exceeded";
        case MAELYS_SYS_ERR_UNSUPPORTED: return "unsupported";
        case MAELYS_SYS_ERR_EXISTS: return "already exists";
        case MAELYS_SYS_ERR_BUSY: return "busy";
        case MAELYS_SYS_ERR_IDENTITY: return "unexpected file identity";
    }
    return "unknown result";
}

