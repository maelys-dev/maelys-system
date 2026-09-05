#ifndef MAELYS_SYS_VERSION_H
#define MAELYS_SYS_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

#define MAELYS_SYS_VERSION "0.8.1"
#define MAELYS_SYS_ABI_VERSION 1u

const char *maelys_sys_version_string(void);
unsigned int maelys_sys_abi_version(void);

#ifdef __cplusplus
}
#endif

#endif
