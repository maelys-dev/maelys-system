#include "maelys/sys.h"

int main() {
    maelys_sys_result_t result = MAELYS_SYS_OK;
    maelys_sys_socket_t *socket_handle = nullptr;
    maelys_sys_connect_state_t state = MAELYS_SYS_CONNECT_IN_PROGRESS;
    maelys_sys_socket_bind_options_t options{};
    return result == MAELYS_SYS_OK && socket_handle == nullptr &&
        state == MAELYS_SYS_CONNECT_IN_PROGRESS && options.reuse_address == 0
        ? 0 : 1;
}
