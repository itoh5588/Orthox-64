#ifndef NET_SOCKET_H
#define NET_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include "fs.h"

int sys_socket(int domain, int type, int protocol);
int sys_connect(int fd, const void* addr, uint32_t addrlen);
int sys_bind(int fd, const void* addr, uint32_t addrlen);
int sys_listen(int fd, int backlog);
int sys_accept(int fd, void* addr, uint32_t* addrlen);
int sys_setsockopt(int fd, int level, int optname, const void* optval, uint32_t optlen);
int sys_getsockname(int fd, void* addr, uint32_t* addrlen);
int sys_getpeername(int fd, void* addr, uint32_t* addrlen);
int sys_shutdown(int fd, int how);
int64_t sys_sendto(int fd, const void* buf, size_t len, int flags, const void* dest_addr, uint32_t addrlen);
int64_t sys_recvfrom(int fd, void* buf, size_t len, int flags, void* src_addr, uint32_t* addrlen);
int64_t net_socket_read_fd(file_descriptor_t* f, void* buf, size_t count);
int64_t net_socket_write_fd(file_descriptor_t* f, const void* buf, size_t count);
int net_socket_close_fd(file_descriptor_t* f);
void net_socket_dup_fd(file_descriptor_t* f);

#endif
