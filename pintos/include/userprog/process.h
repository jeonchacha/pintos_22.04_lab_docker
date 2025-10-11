#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"
#include <stdbool.h>
#include "filesys/file.h"

/* 부모-자식 간 대기/종료 상태 공유 목적 */
struct wait_status {
    tid_t tid;                  /* 자식 tid */
    int exit_status;            /* 자식 exit status */
    int ref_cnt;                /* 참조 카운트(부모 1 + 자식 1 = 초기값 2) */
    bool dead;                  /* 자식 종료 여부 */
    struct semaphore sema;      /* 자식 종료 시 sema_up으로 부모를 깨움 */
    struct list_elem elem;      /* 부모의 children 리스트에 들어갈 리스트 원소 */
};

/* 자식 쪽 __do_fork()에 넘겨줄 보조 정보 패킷 */
struct fork_aux {
    struct intr_frame parent_if;      /* 부모의 intr_frame(레지스터 컨텍스트)을 '값'으로 통째 복사해 담아둠 */
    struct thread *parent;
    struct semaphore done;            /* 부모-자식 동기화용 세마포어: 자식 준비 완료 알림 */
    bool success;                     /* 자식이 복제에 성공했는지 부모에게 알려줄 플래그 */
    struct wait_status *w;            /* 부모-자식 wait/exit 상태 공유 객체 포인터 */
};

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);

#endif /* userprog/process.h */
