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

#include <string.h>			// memcpy
#include "threads/vaddr.h"	// is_user_vaddr, pg_ofs, PGSIZE
#include "threads/mmu.h"	// pml4_get_page
#include "threads/palloc.h" // palloc_get_page/palloc_free_page

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

static int sys_write (int fd, const void *buffer, unsigned size);
static tid_t sys_exec (const char *cmd_line);
static int sys_wait (tid_t tid);
static tid_t sys_fork (const char *name, struct intr_frame *f);

static void copy_in (void *kdst, const void *usrc, size_t n);
static void copy_out (void *udst, const void *ksrc, size_t n);
static char *copy_in_string_alloc (const char *us);

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

	// printf("[syscall] rax=%lld (EXIT=%d, EXEC=%d, WAIT=%d, WRITE=%d)\n", 
	// 	(long long)n, SYS_EXIT, SYS_EXEC, SYS_WAIT, SYS_WRITE);

	switch (n) {
		case SYS_EXIT:
			sys_exit ((int) f->R.rdi);
			return;
		case SYS_WRITE:
			f->R.rax = sys_write ((int) f->R.rdi,
								  (const void *) f->R.rsi,
								  (unsigned) f->R.rdx);
			break;
		case SYS_FORK:
			f->R.rax = sys_fork ((const char *) f->R.rdi, f);
			break;
		case SYS_EXEC:
			f->R.rax = sys_exec ((const char *) f->R.rdi);
			break;
		case SYS_WAIT:
			f->R.rax = sys_wait ((tid_t) f->R.rdi);
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
	if (fd != 1) return -1;								/* stdout만 지원 */

  	unsigned left = size;								// 남은 바이트
	unsigned wrote = 0;									// 누적 출력 바이트
  	
	char *kbuf = palloc_get_page(0);					// 한 페이지 크기 임시 커널 버퍼
  	if (!kbuf)
		sys_exit(-1);

  	while (left > 0) {									// 남은 데이터가 있을 동안 반복
		size_t chunk = left > PGSIZE ? PGSIZE : left;	// 한번에 처리할 크기(최대 1페이지)

		copy_in(kbuf, buffer, chunk);					// 유저 버퍼 -> 커널 버퍼로 안전 복사(검증 포함)
		putbuf(kbuf, chunk);							// 콘솔로 출력

		buffer = (const uint8_t*)buffer + chunk;		// 유저 버퍼 포인터를 chunk만큼 전진
		left -= chunk; 									// 남은 크기 감소
		wrote += chunk;									// 누적 출력 크기 증가.
  	}
  	palloc_free_page(kbuf);								// 임시 페이지 해제
	return (int)wrote;									// 출력한 바이트 수 반환(성공)
}

static tid_t
sys_exec (const char *cmd_line) {
	if (cmd_line == NULL) sys_exit(-1);

	char *kcmd = copy_in_string_alloc(cmd_line);	// 유저 문자열을 커널 페이지에 안전 복사(+검증)
													// 실패 시 여기서 sys_exit(-1)로 이미 종료됨

	/* process_exec()은 성공 시 do_iret()로 유저모드 진입하고
	 * '절대 돌아오지 않음', 실패 시 -1 반환. 또한 내부에서 palloc_free_page(file_name)로
	 * kcmd를 '반드시 해제'하므로 여기서는 free를 하면 안됨(소유권 전달). */
	if (process_exec ((void *) kcmd) < 0) {			// 성공 땐 이 함수로 절대 안돌아옴, 실패 시 -1 반환.
        sys_exit(-1);
    }
    NOT_REACHED ();
}

/* wait: 커널 구현으로 위임 */
static int
sys_wait (tid_t tid) {
	return process_wait (tid);
}

static tid_t
sys_fork (const char *name, struct intr_frame *f) {
	if (name == NULL) return TID_ERROR;
	// 임시: 진짜 fork 대신, 요청 이름으로 바로 프로세스 생성해서 pid만 리턴.
	return process_create_initd(name);
}

/* user -> kernel 임의 버퍼 복사 */
static void 
copy_in (void *kdst, const void *usrc, size_t n) {
	const uint8_t *u = usrc;			// user 포인터를 바이트 단위 증가가 쉬운 uint8_t*로 받음
	uint8_t *k = kdst;					// kernel 목적지도 마찬가지

	while (n > 0) {						// n 바이트를 다 복사할 때까지 루프
		if (!is_user_vaddr(u))			// [1] 현재 user 주소 u가 사용자 가상영역인지 확인 (커널/NULL 금지)
			sys_exit(-1);				// 잘못된 user 포인터면 해당 프로세스를 -1로 종료

		/* [2] u가 속한 페이지가 현재 프로세스의 페이지테이블에
		 * '매핑되어 있는지' 확인하고, 커널에서 접근 가능한 커널 가상주소를 얻음. */
		uint8_t *kva = pml4_get_page(thread_current ()->pml4, (void *)u);
		if (!kva)						// 매핑이 없으면 즉시 -1 종료 
			sys_exit(-1);

		size_t off = pg_ofs(u);			// u가 그 페이지에서 가지는 오프셋(0~PGSIZE-1)
		size_t chunk = PGSIZE - off; 	// 이번 페이지 끝까지 얼마를 한번에 읽을 수 있는지 계산
		if (chunk > n) 
			chunk = n;		   			// 남은 총 바이트 n보다 더 많이 읽지 않도록 제한

		memcpy(k, kva, chunk);			// [3] 현재 페이지 영역에서 안전 범위만큼 커널 버퍼로 복사
		u += chunk;						// user 포인터롤 chunk 만큼 전진
		k += chunk;						// kernel 포인터도 마찬가지
		n -= chunk;						// 남은 바이트 감소
	}
}

/* kernel -> user 임의 버퍼 복사 */
static void 
copy_out (void *udst, const void *ksrc, size_t n) {
	const uint8_t *k = ksrc;			// 커널 소스 버퍼(바이트 단위 진행)
	uint8_t *u = udst;					// 유저 목적지 버퍼

	while (n > 0) {
		if (!is_user_vaddr(u))			// [1] 유저 목적지가 사용자 가상영역인지 확인
			sys_exit(-1);
		
		/* [2] u가 가리키는 페이지가 매핑되었는지 확인하고 커널이 접근 가능한 주소를 얻음 */
		uint8_t *kva = pml4_get_page(thread_current()->pml4, (void *)u);
    	if (!kva) 
			sys_exit(-1);

		size_t off = pg_ofs(u);
		size_t chunk = PGSIZE - off;	// 이번 페이지에 쓸 수 있는 최대 크기
		if (chunk > n) 
			chunk = n;

		memcpy(kva, k, chunk);	// [3] 커널 버퍼에서 유저 페이지로 chunk 바이트 복사
		u += chunk; 
		k += chunk; 
		n -= chunk;
	}
}

/* user C-string -> kernel: NUL 포함, 한 페이지(<= PGSIZE) 버퍼를 만들어 반환.
 * 소유권(해제)은 호출자 설계에 따라서.. 
 * 현재 코드처럼 process_exec() 내부에서 free하면 여기선 free X. */
static char *
copy_in_string_alloc (const char *us) {
	char *kpage = palloc_get_page(0);			// 커널에서 한 페이지(4KiB) 할당
	if (!kpage) 
		sys_exit(-1);

	size_t i = 0;								// 커널 버퍼에 채운 바이트 수
	const char *p = us;							// 유저 문자열에서 현재 읽는 위치

	while (i < PGSIZE) {						// 커널 버펴 용량을 넘지 않는 한 반복
		if (!is_user_vaddr(p)) {				// [1] p가 유저 영역인지 체크
			palloc_free_page(kpage); 			// 실패면 해제 - 누수 방지
			sys_exit(-1); 
		}

		/* [2] p가 속한 페이지의 커널 접근 주소 */
		uint8_t *kva = pml4_get_page(thread_current()->pml4, (void *)p);
		if (!kva) {
			palloc_free_page(kpage); 
			sys_exit(-1); 
		}

		size_t off = pg_ofs(p);					// 페이지 내 오프셋
		size_t chunk = PGSIZE - off;			// 이번 페이지에서 연속으로 읽을 수 있는 물리 바이트 수

		while (chunk-- && i < PGSIZE) {			// 이번 페이지에서 가능한 만큼 바이트 단위로 스캔
			char c = *kva++;					// 유저페이지에서 한 글자 읽기 -> pml4_get_page가 이미 offset 반영된 KVA를 줌
			kpage[i++] = c;						// 커널 버퍼에 저장
			p++;								// 유저 포인터 한 칸 전진
			if (c == '\0') 						// 성공(NUL 만나면 C-문자열 끝)
				return kpage;   				// 커널 페이지(문자열 복사 완성)를 반환 (소유권: 호출자)
		}
	}

	/* 반복을 다 돌 때까지 NUL을 못 만났다는 건 문자열이 너무 길단 뜻(> PGSIZE=1). */
	palloc_free_page(kpage);
	sys_exit(-1);
	return NULL; /* 도달 안 함 */
}