#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "threads/synch.h"
extern struct lock fs_lock;   // 선언(정의 아님)

void syscall_init (void);

/* 예외 처리 등에서 직접 호출할 수 있게 노출 */
void sys_exit (int status);

#endif /* userprog/syscall.h */
