#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);

/* 예외 처리 등에서 직접 호출할 수 있게 노출 */
void sys_exit (int status);

void fd_close(int fd);

#endif /* userprog/syscall.h */
