#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif
#include "threads/malloc.h"
#include "threads/synch.h"

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

extern struct lock fs_lock;

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}

/* "initd"라는 첫 번째 사용자 프로그램이 FILE_NAME에서 로드되어 시작됩니다.
 *	새로운 스레드는 process_create_initd() 함수가 반환되기 전에 이미 스케줄링되거나 종료될 수도 있습니다. 
 *	이 함수는 initd의 스레드 ID를 반환하며, 스레드를 생성할 수 없는 경우 TID_ERROR를 반환합니다.
 *	주의: 이 함수는 단 한 번만 호출되어야 합니다. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	/* 부모 쪽 wait_status 준비 및 children 등록 */
	struct wait_status *w = malloc (sizeof (*w));
	if (w == NULL) {
		palloc_free_page (fn_copy);
		return TID_ERROR;
	}
	w->tid = TID_ERROR;			/* 아직 thread_create()를 호출하기 전이라 자식 TID를 모름. */
	w->exit_status = 0;			/* 실제 값은 자식이 exit(status) 호출할 때 sys_exit()에서 기록. */
	w->ref_cnt = 2;				/* 참조 카운트: 이 객체는 부모 1 + 자식 1, 두 쪽이 공유하므로 초기값 2. 
								 * 이후 해제 타이밍:
								 * 부모가 process_wait()을 마치면 --ref_cnt
								 * 자식이 sys_exit()에서 정리할 때 --ref_cnt
								 * 0이 되면 free(w) (둘 다 손을 뗀 시점) */
	w->dead = false;			/* 자식이 exit()를 호출하면 sys_exit()에서 true로 바꾸고 부모를 깨움. */
	sema_init (&(w->sema), 0);	/* 세마포어 초기값 0: 부모가 process_wait()에서 sema_down() 하면 즉시 블록됨.
								 * 자식이 종료할 때 sys_exit()에서 sema_up(&w->sema) 호출 → 부모 대기 해제. */

	/* initd에 넘길 aux(파일명 페이지, wstatus) */
	struct {
		char *fname;
		struct wait_status *w;
	} *aux = malloc (sizeof (*aux));
	if (aux == NULL) {
		free(w);
		palloc_free_page (fn_copy);
		return TID_ERROR;
	}
	aux->fname = fn_copy;
	aux->w = w;

	// 스레드 "표시 이름"만 첫 토큰으로 잘라서 준비
    char tname[16];
    size_t i = 0;
    while (file_name[i] != '\0' && file_name[i] != ' ' && i < sizeof tname - 1) {
        tname[i] = file_name[i];
        i++;
    }
    tname[i] = '\0';

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (tname, PRI_DEFAULT, initd, aux);
	if (tid == TID_ERROR) {
		free(aux);
		free(w);
		palloc_free_page (fn_copy);
		return TID_ERROR;
	}
		
	/* tid 기록, 내 children 리스트에 등록 */
	w->tid = tid;
	list_push_back (&(thread_current ()->children), &(w->elem));
	return tid;
}

/* 사용자 프로세스를 처음으로 실행하는 스레드 함수. */
static void
initd (void *aux_) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	/* 자식 스레드 입장에서 내 wstatus 연결 */
	// 1) 부모가 보낸 포인터를 우리가 아는 구조체로 캐스팅
	struct {
		char *fname;
		struct wait_status *w;
	} *aux = aux_;

	// 2) 필요한 포인터 값들을 '지역 변수'로 먼저 빼둠
	char *fname = aux->fname;
	struct wait_status *w = aux->w;

	// 3) 래퍼(struct …)* aux 자체는 이제 필요 없으니 여기서 바로 해제
	free (aux); 

	// 4) 빼둔 값 사용: 내 wstatus 연결
	thread_current ()->wstatus = w;

	// 5) exec 호출: 여기서 성공하면 더는 initd로 돌아오지 않음
	if (process_exec (fname) < 0) {
		sys_exit(-1);
	} 
	NOT_REACHED ();
}

/* 현재 프로세스를 'name'이라는 이름의 새 스레드로 복제한다.
 * 성공 시 자식의 tid를 반환, 실패 시 TID_ERROR 반환. 
 * 부모는 자식 준비(복제 성공/실패)가 확정되기 전까지 반환하지 않는다. */
tid_t
process_fork (const char *name, struct intr_frame *if_) {
	struct thread *parent = thread_current();		/* 현재 실행 중인 스레드 포인터를 부모로 보관 */

	/* wait_status 생성, 초기화 */
	struct wait_status *w = malloc (sizeof (*w));
	if (w == NULL) return TID_ERROR;

	w->tid = TID_ERROR;								/* 아직 자식 tid를 모름(스레드 생성 전) */
	w->exit_status = 0;								/* 기본값 0, 실제 값은 자식이 exit(status) 때 기록 */
	w->ref_cnt = 2;									/* 부모 1 + 자식 1 이 참조 → 초기 2 */
	w->dead = false;
	sema_init (&(w->sema), 0);						/* 부모가 wait 할 세마포어(자식 종료 시 up) 초기 0 */

	/* 자식 스레드 시작 루틴(__do_fork)에 건네줄 보조 패킷 할당 */
	struct fork_aux *aux = malloc (sizeof (*aux));
	if (aux == NULL) {
		free(w);
		return TID_ERROR;
	}
	aux->parent_if = *if_;							/* 부모의 intr_frame을 '값 복사' (부모 스택 사라져도 안전) */
	aux->parent = parent;
	aux->w = w;										/* wait_status 전달(자식이 자기 필드에 연결하고 사용) */
	aux->success = false;							/* 자식이 성공 시 true로 바꿈 */
	sema_init(&aux->done, 0);						/* 부모가 자식 준비 완료를 기다릴 세마포어(자식이 up) */

	/* 새 스레드 생성: 자식은 __do_fork()에서 부모 복제를 수행 */
	tid_t tid = thread_create (name, PRI_DEFAULT, __do_fork, aux);
	if (tid == TID_ERROR) {
		free(aux);
		free(w);
		return TID_ERROR;
	}

	/* 자식이 복제 작업을 마치고 성공/실패를 표시할 때까지 대기 */
	sema_down(&(aux->done));						/* __do_fork()가 끝에서 sema_up() 해 줄 때까지 잠듦 */

	/* 복제 실패 */
	if (!aux->success) {
		if (--w->ref_cnt == 0)
			free(w);

		free(aux);
		return TID_ERROR;
	}

	/* 복제 성공: 부모의 children 목록에 wait_status를 연결 */
	w->tid = tid;
	list_push_back(&(parent->children), &(w->elem));

	free(aux);
	return tid;									/* 자식의 tid를 부모에게 반환 → fork 성공 */
}

/* vm에서 안씀 */
#ifndef VM 
/* 부모의 주소 공간을 복제하기 위해 이 함수를 pml4_for_each에 전달. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {		/* 부모의 PTE 하나를 자식 주소공간으로 복제하는 콜백 */
	/* 현재 실행 중인 스레드 = 자식(복제 대상이 되는 쪽) */
	struct thread *current = thread_current ();

	/* pml4_for_each()에 aux로 실어 보낸 부모 스레드 포인터 복원 */
	struct thread *parent = (struct thread *) aux;

	/* 커널 영역 주소는 복제 대상 아님 → 그냥 건너뛰고 계속 진행 */
	if (!is_user_vaddr(va)) return true;			

	/* 부모 pml4에서 가상주소 va가 매핑된 커널 가상주소(KVA)를 얻음 */
	void *parent_page = pml4_get_page(parent->pml4, va);
	/* 부모가 해당 va를 매핑하지 않았다면 복제할 게 없음 → 통과 */
	if (parent_page == NULL) return true;

	/* 자식 쪽에 올릴 새 사용자 페이지 할당 */
	void *newpage = palloc_get_page(PAL_USER);
	/* 메모리 부족 등으로 실패하면 전체 복제를 중단(콜백 실패 반환) */
	if (newpage == NULL) return false;

	/* 부모 페이지의 내용을 그대로 자식 새 페이지로 바이트 단위 복사 */
	memcpy(newpage, parent_page, PGSIZE);
	/* 부모 PTE의 쓰기 가능 플래그를 읽어 동일한 접근권한을 유지 */
	bool writable = is_writable(pte);
	
	/* 자식 pml4에 (va → newpage) 매핑을 동일 권한으로 설치 */
	if (!pml4_set_page(current->pml4, va, newpage, writable)) {
		palloc_free_page(newpage);							/* 매핑에 실패했으면 방금 할당한 물리 페이지 반납(누수 방지) */
		return false;										/* 이 PTE 복제 실패를 상위로 알림(전체 fork 실패로 이어짐) */
  	}
	return true;											/* 이 PTE 복제를 성공적으로 마쳤음을 알림(다음 엔트리로 진행) */
}
#endif

/* 부모의 execution context을 복사하는 스레드 함수.
 * thread 구조체의 tf는 커널 컨텍스트라서 사용자 레지스터 값이 아님.
 * 그래서 process_fork()에서 전달한 parent_if를 사용해야 함. */
static void
__do_fork (void *aux_) {							/* 자식 스레드 본체: thread_create가 이 함수를 실행 */
	struct fork_aux *aux = aux_;					/* 부모가 넘겨준 보조 패킷을 원래 타입으로 캐스팅 */
	struct thread *parent = aux->parent;			/* 보조 패킷에 들어있는 부모 스레드 포인터 획득 */
	struct thread *current = thread_current ();		/* 현재 실행 중(자식) 스레드 포인터 */

	struct intr_frame if_ = aux->parent_if;			/* 1. 부모의 사용자 레지스터 컨텍스트를 로컬에 복사. */	
	bool ok = true;									/* 진행 중 오류가 있었는지 표시하는 플래그 */
	
	/* 페이지 테이블 복제(자식의 주소 공간 만들기) */
	current->pml4 = pml4_create();					/* 2. 자식용 최상위 페이지 테이블(PML4) 새로 생성 */
	if (current->pml4 == NULL) {
		ok = false;
		goto out;
	}
	process_activate (current);						/* 새 pml4 활성화 */

#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt)) {
		ok = false;
		goto out;
	}
#else
	/* (no-VM 버전) 부모의 모든 PTE를 순회하며 복제 */
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent)) { 
		ok = false;
		goto out;
	}
#endif

	/* 파일 디스크립터 복제 */
	for (int fd = 2; fd < FD_MAX; fd++) {			/* 표준입출력 0/1은 이미 세팅되어 있다고 가정, 2부터 복제 */
		struct file *pf = parent->fd_table[fd];		/* 부모의 fd 엔트리(열린 파일 객체 포인터) 획득 */
		if (pf != NULL) {							/* 실제로 열려 있는 슬롯만 처리 */
			lock_acquire(&fs_lock);					/* 파일 시스템 계층은 스레드 안전하지 않으므로 전역 락 획득 */
      		struct file *cf = file_duplicate(pf);	/* 부모 파일 객체를 안전하게 복제 */
      		lock_release(&fs_lock);					/* 크리티컬 섹션 종료 후 락 해제 */

			if (cf == NULL) {
				ok = false;
				goto out; 
			}

			current->fd_table[fd] = cf;				/* 자식의 동일 fd 인덱스에 복제된 파일 객체 저장 */
		}
	}
	current->fd_next = parent->fd_next;				/* 다음 할당할 fd 인덱스도 동일하게 복사 */

	current->wstatus = aux->w;						/* 자식 스레드에 wait_status 연결(부모와 공유) */
	if_.R.rax = 0 ; 								/* 자식의 fork() 반환값 = 0 */

out:												/* 공통 정리 지점 */
	aux->success = ok;
	sema_up(&(aux->done));							/* 부모를 깨움: process_fork()의 sema_down()을 풀어줌 */

	if (!ok) thread_exit ();						/* 실패 시 자식 스레드 종료 */

	do_iret (&if_);									/* 유저 모드로 진입: 부모가 fork를 호출한 지점으로 복귀하되 RAX=0으로 실행 시작 */
	NOT_REACHED();									/* 여기는 도달할 수 없음(이미 유저모드로 넘어감) */		
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {		// 새 프로세스 생성이 아니라 현제 프로세스의 메모리 이미지를 교체하는 동작.
	char *file_name = f_name;
	bool success;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup ();

	/* And then load the binary */
	success = load (file_name, &_if);

	/* If load failed, quit. */
	palloc_free_page (file_name);
	if (!success)
		return -1;

	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}

/* 지정된 스레드(TID)가 종료될 때까지 기다렸다가 해당 스레드의 종료 상태를 반환합니다. 
 * 만약 스레드가 커널에 의해 (예: 예외로 인해) 종료되었다면 -1을 반환합니다. 
 * TID가 유효하지 않거나, 호출한 프로세스의 자식이 아니거나, 
 * 이미 해당 TID에 대해 process_wait()가 성공적으로 호출되었다면, 
 * 기다리지 않고 즉시 -1을 반환합니다. */
int
process_wait (tid_t child_tid) {
	struct thread *cur = thread_current ();
	struct list_elem *e;
	struct wait_status *w = NULL;

	/* 내 자식인지 찾기 */
	for (e = list_begin (&(cur->children)); e != list_end (&(cur->children)); e = list_next (e)) {
		struct wait_status *cand = list_entry (e, struct wait_status, elem);
		if (cand->tid == child_tid) {
			w = cand;
			break;
		}
	}
	if (w == NULL) return -1;		/* 내 자식 아님 또는 이미 기다림 */

	/* 이중 wait 방지: 리스트에서 제거 */
	list_remove (&w->elem);

	/* 자식 종료까지 대기 (이미 종료면 바로 통과) */
	if (!w->dead) {
		sema_down (&(w->sema));
	}

	int status = w->exit_status;
	if (--w->ref_cnt == 0) {
		free(w);
	}
	return status;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();

	/* 내가 아직 wait하지 않은 자식들의 wstatus 정리 (부모 선종료 케이스)
	   목적: 자식들의 wstatus 누수/댕글링 방지 */
	struct list_elem *e, *next;
	for (e = list_begin (&(curr->children)); e != list_end (&(curr->children)); e = next) {
		next = list_next (e);
		struct wait_status *w = list_entry (e, struct wait_status, elem);
		list_remove (&(w->elem)); // 자식 목록에서 참조만 끊음. 자식 스레드는 그대로 잘 돌아감.
		if (--w->ref_cnt == 0) {
			free (w);
		}
	}
	process_cleanup ();
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();				// 페이지 디렉토리 생성. page map level 4
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());	// 페이지 테이블 활성화

	/* 
		Pintos의 페이지 할당기에서 한 페이지(4KiB) 를 빌려오는 함수
		인자 flags는 옵션 비트마스크:
			PAL_ZERO -> 할당된 페이지를 0으로 초기화
			PAL_USER -> user 풀에서 가져옴(스택 페이지처럼 사용자 매핑에 쓸 때)
			0이면 커널 풀에서, 제로 초기화 없이 한 페이지를 가져옴.
	*/
	char *buf = palloc_get_page (0);
	if (buf == NULL) goto done;
	strlcpy (buf, file_name, PGSIZE);

	/* tokenize */
	char *argv_tok[64];		// args-many 도 충분
	int argc = 0;
	char *save_ptr = NULL;
	for (char *t = strtok_r (buf, " ", &save_ptr);
		 t && argc < 64; t = strtok_r (NULL, " ", &save_ptr)) 
	{
		argv_tok[argc++] = t;
	}

	if (argc == 0) goto done;	// 토큰 0개 처리

	/* Open executable file. -> first token (program name) */
	file = filesys_open (argv_tok[0]);		// 프로그램 파일 open
	if (file == NULL) {
		printf ("load: %s: open failed\n", argv_tok[0]);
		goto done;
	}

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", argv_tok[0]);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	uint64_t rsp = (uint64_t) USER_STACK;	// x86-64에서 포인터/레지스터는 64비트
	const uint64_t UBASE = (uint64_t) USER_STACK - PGSIZE;	// 스택은 1페이지만 매핑, 유효범위 = [BASE, TOP)
	uint64_t arg_addr[64];

	/* 1) strings (reverse) */
	for (int i = argc - 1; i >= 0; i--) {
		size_t len = strlen (argv_tok[i]) + 1;	// +1은 \0때문
		if (rsp < UBASE + len) goto done; 		// 페이지 경계 체크
		rsp -= len;
		memcpy ((void *) rsp, argv_tok[i], len);
		arg_addr[i] = rsp;						// 포인터 값
	}

	/* 2) 8B align */
	rsp &= ~0x7ULL;

	/* 3) argv[argc] = NULL */
	if (rsp < UBASE + 8) goto done; 
	rsp -= 8;
	*(uint64_t *) rsp = 0;

	/* 4) argv pointers (reverse) -> argv[0]이 가장 낮은 슬롯 */
	for (int i = argc - 1; i >= 0; i--) {
		if (rsp < UBASE + 8) goto done; 		
		rsp -= 8;
		*(uint64_t *) rsp = (uint64_t) arg_addr[i];
	}
	uint64_t argv_ptr = rsp;

	/* 5) fake return */
	if (rsp < UBASE + 8) goto done; 		
	rsp -= 8;
	*(uint64_t *) rsp = 0;

	/* 6) pass regs */
	if_->R.rdi = (uint64_t) argc;
	if_->R.rsi = (uint64_t) argv_ptr;
	if_->rsp   = (uint64_t) rsp;

	success = true;
	
done:
	/* We arrive here whether the load is successful or not. */
	if (buf) palloc_free_page (buf);

	if (file) file_close (file);
	return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */