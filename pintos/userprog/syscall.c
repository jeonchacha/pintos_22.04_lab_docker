#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

#include "threads/synch.h"
#include "threads/malloc.h"
#include "userprog/process.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

static int sys_write (int fd, const void *buffer, unsigned size);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
	// TODO: Your implementation goes here.
	uint64_t n = f->R.rax;
	switch (n) {
		case SYS_EXIT:
			sys_exit ((int) f->R.rdi);
			return;
		case SYS_WRITE:
			f->R.rax = sys_write ((int) f->R.rdi,
								  (const void *) f->R.rsi,
								  (unsigned) f->R.rdx);
			break;
		default:
			/* 아직 안 쓰는 syscall은 전부 종료시킴 */
			sys_exit (-1);
	}
}

/* 
	시스템콜 인자 전달은 레지스터로 이뤄짐.
	- RAX: 시스템콜 번호, 반환값
	- RDI: 1번째 인자
	- RSI: 2번째 인자
	- RDX: 3번째 인자
	(다음은 필요시 R10, R8, R9)
*/

void
sys_exit (int status) {
	struct thread *cur = thread_current ();
	printf ("%s: exit(%d)\n", thread_name (), status);

	/* 부모에게 종료 전달 */
	if (cur->wstatus) {
		cur->wstatus->exit_status = status;
		cur->wstatus->dead = true;
		sema_up (&(cur->wstatus->sema));				// 부모 깨우기
		if (--cur->wstatus->ref_cnt == 0) {				// 자식 몫 반납
			free (cur->wstatus);
		}
		cur->wstatus = NULL;
	}
  	thread_exit ();		/* 반환 없음. 되돌아오지 않음. */
}

static int 
sys_write (int fd, const void *buffer, unsigned size) {
	if (fd == 1 && buffer && size) {	/* stdout만 지원 */
		putbuf (buffer, size);
		return (int) size;
	}
	return -1;
}