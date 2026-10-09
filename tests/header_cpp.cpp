#include "maelys/sys.h"

int main() {
    maelys_sys_result_t result = MAELYS_SYS_OK;
    maelys_sys_socket_t *socket_handle = nullptr;
    maelys_sys_connect_state_t state = MAELYS_SYS_CONNECT_IN_PROGRESS;
    maelys_sys_socket_bind_options_t options{};
    maelys_sys_file_expectations_t expectations{};
    maelys_sys_file_lock_options_t lock_options{};
    maelys_sys_file_mismatch_t mismatch = MAELYS_SYS_FILE_MATCH;
    (void)mismatch;
    static_assert(MAELYS_SYS_ERR_IDENTITY > MAELYS_SYS_ERR_UNSUPPORTED,
        "new result codes are appended");
    (void)expectations; (void)lock_options;
    maelys_sys_fdpass_flags_t fdpass_flags = MAELYS_SYS_FDPASS_SURPLUS;
    (void)fdpass_flags;
    maelys_sys_dirwatch_change_t dirwatch_change{};
    (void)dirwatch_change;
    maelys_sys_process_options_t process_options{};
    maelys_sys_process_fd_t process_fd{};
    maelys_sys_process_status_t process_status{};
    maelys_sys_process_identity_t process_identity{};
    (void)process_options; (void)process_fd; (void)process_status; (void)process_identity;
    return result == MAELYS_SYS_OK && socket_handle == nullptr &&
        state == MAELYS_SYS_CONNECT_IN_PROGRESS && options.reuse_address == 0
        ? 0 : 1;
}
