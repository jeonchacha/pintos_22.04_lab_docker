/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"

#include "threads/mmu.h"             // pml4_set_page, pg_round_down
#include "threads/palloc.h"          // palloc_get_page/free, PAL_USER
#include "threads/vaddr.h"           // is_user_vaddr, pg_ofs
#include "threads/thread.h"          // thread_current
#include <string.h>                  // memset, memcpy
#include <debug.h>                   // ASSERT

#include "userprog/process.h" 		/* file_lazy_aux */

extern struct lock fs_lock;

/* ---------- SPT 해시용 보조 함수들 ---------- */

/* 페이지 키: upage(va)를 바로 해시 키로 사용 */
static uint64_t
page_hash (const struct hash_elem *e, void *aux UNUSED) {
	/* h_elem이 들어있는 struct page*로 되돌림 */
	const struct page *p = hash_entry (e, struct page, h_elem);
	/* VA 자체를 해시 키로 사용 (정렬된 페이지 시작 주소) */
	return hash_bytes (&p->va, sizeof p->va);
}

/* 페이지 비교: 동일 VA 여부 */
static bool
page_less (const struct hash_elem *a,
		   const struct hash_elem *b,
		   void *aux UNUSED) {
	const struct page *pa = hash_entry (a, struct page, h_elem);
	const struct page *pb = hash_entry (b, struct page, h_elem);
	return pa->va < pb->va;
}

/* ---------- VM 서브 시스템 초기화 ---------- */

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();	/* 익명 페이지 ops 등록 */
	vm_file_init ();	/* 파일 페이지 ops 등록 */
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
}

/* ---------- 타입 조회 ---------- */

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* ---------- 페이지 등록 (예약) ---------- */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)			/* lazy 예약은 uninit_new()로 처리할 것 */

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* 이미 등록된 VA면 실패 */
	if (spt_find_page (spt, upage) != NULL)
			return false;

	/* TODO: 페이지를 생성하고, VM 타입에 따라 초기화 함수(initializer)를 가져온 다음, 
	 * uninit_new를 호출하여 미초기화(uninit) 페이지 구조체를 생성할것.
	 * uninit_new를 호출한 후에는 해당 필드를 수정해야 함
	 * 해당 페이지를 SPT에 삽입할것. */

	/* 공용 헤더(struct page)만 먼저 잡음. 실제 데이터는 lazy로 채울 것 */
	struct page *page = calloc (1, sizeof *page);
	if (page == NULL)
			return false;

	/* 타입별 initializer 선택:
	   - VM_ANON:   anon_initializer (vm/anon.h에서 제공)
	   - VM_FILE:   file_backed_initializer (vm/file.h에서 제공)
	   uninit_new()가 내부적으로 page->operations를 UNINIT ops로 세팅하고,
	   첫 swap_in 때 init()을 호출해 실제 타입으로 전환하게 해줌. */
	bool (*page_initializer)(struct page*, enum vm_type, void *kva) = NULL;

	switch (VM_TYPE(type)) {
	case VM_ANON:
		page_initializer = anon_initializer;
		/* 익명 페이지의 기본 내용은 제로필: init 콜백이 NULL이라면 기본 제로필로 대체 */
		if (init == NULL) {
			extern bool anon_init_zero (struct page *page, void *aux);
			init = anon_init_zero;
			aux = NULL;
		}
		break;

	case VM_FILE:
		page_initializer = file_backed_initializer;
		break;
	default:
		free (page);
		return false;
	}

	/* 예약(uninit) 페이지 구성: 
	   - init: 최초 접근 시 호출할 '콘텐츠 채움' 함수(예: 파일에서 읽기)
	   - type: 최종 타입(VM_ANON/VM_FILE 등)
	   - aux : init이 필요로 하는 부가정보(파일 포인터+오프셋/읽을 바이트 수 등) */
	uninit_new (page, pg_round_down(upage), init, type, aux, page_initializer);

	/* uninit_new가 page를 다시 덮어쓰므로 ‘이후’에 writable 세팅 */
	page->writable = writable;
	page->type = VM_UNINIT;

	/* SPT에 등록 */
	if (!spt_insert_page (spt, page)) {
		free (page);
		return false;
	}

	return true;
}

/* ---------- SPT 조회/삽입/삭제 ---------- */

/* VA로 page를 찾아 리턴(없으면 NULL) */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	struct page key;					/* 조회용 임시 키(page) 객체를 스택에 만들고, 같은 키(h_elem)를 가진걸 찾는다.*/
	key.va = pg_round_down (va);		/* 키는 페이지 경계 기준 */

	struct hash_elem *e = hash_find (&spt->pages, &key.h_elem);
	if (e == NULL) return NULL;

	return hash_entry (e, struct page, h_elem);
}

/* PAGE를 SPT에 삽입 */
bool
spt_insert_page (struct supplemental_page_table *spt, struct page *page) {
	struct hash_elem *old = hash_insert (&spt->pages, &page->h_elem);
	return old == NULL;		/* 기존에 없었으면 성공 */
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	/* 해시에서 먼저 빼고, 그 다음 page 파괴 */
	hash_delete (&spt->pages, &page->h_elem);
	vm_dealloc_page (page);
}

/* ---------- 프레임 테이블 ---------- */
/* 지금은 eviction 없이: 부족하면 NULL 말고 반드시 하나 만들어 주기 위해
 * palloc_get_page(PAL_USER)를 바로 씀. 후속 단계에서 frame pool/eviction 확장 */

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	/* TODO: The policy for eviction is up to you. */

	/* 추후: clock/second-chance 등 정책으로 교체 대상 프레임을 고르는 곳 */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	/* 추후: victim을 swap_out하고 프레임 반환 */

	return NULL;
}

/* palloc()으로 유저 풀에서 프레임 1개를 확보해 frame 객체를 리턴 */
static struct frame *
vm_get_frame (void) {
	struct frame *frame = malloc (sizeof *frame);
	ASSERT (frame != NULL);

	/* 유저 풀에서 물리 페이지 1장 할당(0으로 지울 필요는 아직 없음) */
	void *kva = palloc_get_page (PAL_USER);
	if (kva == NULL) {
		/* 나중엔 evict 시도 -> 실패 시 kill. 지금은 바로 실패 처리 */
		free (frame);
		return NULL;
	}
	frame->kva = kva;		/* 커널 가상주소 기록 */
	frame->page = NULL;		/* 아직 소유 page 없음 */
	return frame;
}

/* ---------- 폴트 처리(우선 not-present + 등록된 페이지만) ---------- */

#define MAX_STACK_BYTES   (1 << 20)           /* 1MB 제한 */
#define RSP_SLACK_BYTES   8                   /* PUSH가 미리 체크하는 여유범위 */

static void
vm_stack_growth (void *addr) {
	struct thread *t = thread_current();
	
	/* fault 주소를 페이지 경계로 내림 */
	uint8_t *target = pg_round_down(addr);

	/* 이미 매핑된 최하단보다 더 아래로 내려가야 하면, 그 구간을 한 페이지씩 키워준다.
	 * (스택은 아래로 자라므로 target <= stack_bottom 여야 의미가 있음) */
	while (t->stack_bottom > target) {
		void *new_page = (uint8_t *)t->stack_bottom - PGSIZE;

		/* 1MB 한도 검사: USER_STACK에서 얼마나 내려왔는지로 계산 */
		size_t grown = (uintptr_t)USER_STACK - (uintptr_t)new_page;
		if (grown > MAX_STACK_BYTES) break;		/* 더는 못 키움(한도 도달) */

		/* 페이지 하나 등록+매핑 */
		if (!vm_alloc_page_with_initializer(VM_ANON, new_page, true, NULL, NULL))
			break;
		if (!vm_claim_page(new_page))
			break;
		
			t->stack_bottom = new_page;		/* 새 하한 갱신 */
	}
}

/* "이 폴트가 스택 성장 후보인지?" 휴리스틱 */
static bool
should_grow_stack (struct intr_frame *f, void *addr, bool user, bool write) {
	if (!user) return false;		/* 커널 모드 폴트는 스택 성장 아님 */
	if (!write) return false;		/* 쓰기 접근만 허용 */

	/* 현재 유저 RSP 가져오기 */
	uint64_t rsp = f ? f->rsp : thread_current()->user_rsp;
	if (rsp == 0) return false;		/* 못 얻으면 포기 */

	/* 주소 범위: USER_STACK 아래쪽이어야 하고 */
	if (addr >= USER_STACK) return false;

	/* RSP보다 너무 멀리 아래를 치지 않았는지("스택처럼 보이는" 근접 접근만 허용) */
	if ((uint64_t)addr + RSP_SLACK_BYTES < rsp)		/* addr <= rsp-8 이면 OK, 더 멀면 거절 */
		return false;

	/* 1MB 제한 체크(해당 페이지까지 키워도 1MB 이내여야) */
	uint64_t target = (uintptr_t)pg_round_down(addr);
	size_t would_be = (uintptr_t)USER_STACK - target;
	if (would_be > MAX_STACK_BYTES) return false;	

	return true;
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
	/* R/W 보호 위반 대응(추후 COW에서 사용). 지금은 미구현 */
	return false;
}

/* 폴트 처리 진입부
 * - not_present = 1 이고
 * - 해당 VA가 SPT에 등록되어 있으면
 *   → 프레임 할당 + swap_in/uninit-init + 매핑까지 수행 */
bool
vm_try_handle_fault (struct intr_frame *f, void *addr, bool user, bool write, bool not_present) {
	if (addr == NULL || !is_user_vaddr (addr))
		return false;

	/* present인데 write 폴트면 WP 처리 후보(나중에 COW에서) */
	if (!not_present) {
		return false;
	}

	/* 페이지 경계로 내림 -> 그 VA로 SPT 조회 */
	void *upage = pg_round_down (addr);
	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* 1) SPT에 등록된 페이지면: 권한 체크 후 클레임 */
	struct page *page = spt_find_page (spt, upage);
	if (page) {
		/* 쓰기 폴트인데 페이지가 읽기전용이면 거절 */
		if (write && !page->writable) return false;
		/* 실제로 메모리에 들여와 매핑 */
		return vm_do_claim_page (page);
	}

	/* 2) 등록된 페이지가 없고 "스택 성장 후보"라면: 성장 시도 */
	if (should_grow_stack(f, addr, user, write)) {
		vm_stack_growth(addr);
		/* 성장 후엔 해당 페이지가 매핑됐는지 확인(성장 실패 가능성 고려) */
		page = spt_find_page(spt, upage);
		return page && page->frame != NULL;
	}

	/* 3) 그 외는 잘못된 접근 */
	return false;	
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* ---------- 페이지 클레임(프레암 할당 + 매핑 + swap_in) ---------- */

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va) {
	struct page *page = spt_find_page (&thread_current ()->spt, va);
	if (page == NULL)
		return false;
	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	/* 프레임(물리 페이지) 하나 확보 */
	struct frame *frame = vm_get_frame ();
	if (frame == NULL)
		return false;

	/* 연결(서로 역참조) */
	frame->page = page;
	page->frame = frame;

	/* TODO: 페이지의 가상 주소(VA)를 프레임의 물리 주소(PA)에 매핑하도록 페이지 테이블 엔트리를 삽입. */

	/* PML4에 VA->KVA 매핑 설치(유저 쓰기 권한 반영) */
	if (!pml4_set_page (thread_current ()->pml4, 
						page->va, frame->kva, page->writable)) {
		/* 매핑 실패 시 프레임 반납 */
		page->frame = NULL;
		frame->page = NULL;
		palloc_free_page (frame->kva);
		free (frame);
		return false;
	}

	/* 실제 콘텐츠 채우기:
	   - UNINIT: init()을 통해 실제 타입으로 전환 후 내용 적재
	   - ANON : swap에서 끌어오거나(초기엔 zero-fill)
	   - FILE : 파일에서 읽어 채움
	*/
	if (!swap_in (page, frame->kva)) {
		/* 실패 시 매핑 해제 + 프레임 반납 */
		pml4_clear_page (thread_current ()->pml4, page->va);
		page->frame = NULL;
		frame->page = NULL;
		palloc_free_page (frame->kva);
		free (frame);
		return false;
	}

	return true;
}

/* ---------- SPT 생명주기 ---------- */

/* SPT 초기화: 해시 테이블 준비 */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	hash_init (&spt->pages, page_hash, page_less, NULL);
}

bool
supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src) {
	/* TODO: src를 순회하며:
	   - UNINIT  : 같은 init/aux로 예약만 복제
	   - ANON    : 새 anon 페이지를 만들고 내용을 복사(또는 lazy COW)
	   - FILE    : 같은 파일 백드로 예약 복제
	*/

	struct hash_iterator i;
	hash_first(&i, &src->pages);
	
	while (hash_next(&i)) {
		struct page *src_page = hash_entry(hash_cur(&i), struct page, h_elem);
		
        void *va = src_page->va;
		bool writable = src_page->writable;
        enum vm_type type = page_get_type(src_page);

		/* 1) 소스가 아직 UNINIT인 경우 -> lazy 상태 그대로 복제 */
		if (src_page->operations->type == VM_UNINIT) {
			vm_initializer *init = src_page->uninit.init;	/* 최초 fault 시 호출될 초기화자 그대로 사용 */
			void *aux = NULL;								/* 보조 데이터는 타입별로 다르게 */

			if (type == VM_FILE) {							/* UNINIT(파일-백드): aux 깊은복사 + 파일 핸들 분리 */
				struct file_lazy_aux *saux = src_page->uninit.aux;
				struct file_lazy_aux *daux = malloc(sizeof *daux);
				if (!daux) return false;

				lock_acquire(&fs_lock);
				daux->file = file_reopen(saux->file); 		/* 파일 핸들 분리: 파일 위치/수명 독립 */
				lock_release(&fs_lock);
				if (!daux->file) {
					free(daux);
					return false;
				}

				daux->ofs = saux->ofs;
				daux->read_bytes = saux->read_bytes;
				daux->zero_bytes = saux->zero_bytes;
				aux = daux;
			} else {
				/* UNINIT(ANON): 보통 aux/initalizer 안 쓰니 그대로 넘김 */
				aux = src_page->uninit.aux;
			}

			/* 새 UNINIT 페이지 예약(lazy 로딩 유지) */
			if (!vm_alloc_page_with_initializer(type, va, writable, init, aux)) {
				if (type == VM_FILE && aux) {			/* 실패 시 자원 정리 */
					struct file_lazy_aux *daux = aux;		
					lock_acquire(&fs_lock);
					file_close(daux->file);
					lock_release(&fs_lock);
				}
				return false;
			}
			/* UNINIT은 여기서 끝. 아직 프레임 할당/매핑 없음 */
			continue;
		}

		/* 2) 소스가 이미 실체화된 경우(= 프레임이 존재하는 페이지) */
		if (type == VM_ANON) {
			/* 자식에도 "새로운" ANON 페이지를 만들고 -> 즉시 클레임 -> 바이트 단위 복사 */
			if (!vm_alloc_page_with_initializer(VM_ANON, va, writable, NULL, NULL))
				return false;
			if (!vm_claim_page(va))
				return false;

			struct page *dst_page = spt_find_page(dst, va); 	/* 자식 페이지(프레임 보유) */
			ASSERT(dst_page && dst_page->frame && src_page->frame);

			memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);		/* KVA <-> KVA 복사(가장 안전) */
			continue;
		}

		if (type == VM_FILE) {
			/* 이미 로드된 파일-백드 페이지.
               테스트/단계상 스왑/쓰기-회수 미구현이면, 가장 단순하고 견고한 방법은
               "자식 쪽은 사본(ANON)으로 만들어 즉시 복제"하는 것.
               (fork 후 자식이 해당 페이지를 수정해도 파일에 쓰기-회수 안 함이 자연스러움) */
			if (!vm_alloc_page_with_initializer(VM_ANON, va, writable, NULL, NULL))
                return false;
            if (!vm_claim_page(va))
                return false;

            struct page *dst_page = spt_find_page(dst, va);
            ASSERT(dst_page && dst_page->frame && src_page->frame);

            memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
			continue;

			/* 나중에 mmap/페이지캐시/dirty write-back 구현 시
			 * - dirty면 파일에 쓰기,
			 * - clean이면 참조 공유(COW 준비) 등
			 * 파일-백드 전용 처리 추가. */
		}

		/* 다른 타입은 범위 밖 */
		return false;
	}
	return true;
}

/* SPT 파괴: 모든 page 제거(destroy 호출 포함) */
static void
spt_destroy_action (struct hash_elem *e, void *aux UNUSED) {
	struct page *p = hash_entry(e, struct page, h_elem);
	vm_dealloc_page(p);
}

void
supplemental_page_table_kill (struct supplemental_page_table *spt) {
	/* TODO: 해당 스레드가 보유하고 있는 모든 SPT를 파괴하고,
	 * 수정된 모든 내용을 저장소에 다시 기록(writeback) 할것 */

	 /* hash_destroy는 각 원소에 대해 지정한 함수 호출 후, 해시 내부 버킷을 해제
	  * -> 각 엔트리의 실질 정리는 spt_destroy_action -> vm_dealloc_page -> destroy(page) 로 위임. */
	 hash_destroy (&spt->pages, spt_destroy_action);
}

/* 위 처럼 분리하는 이유는
 * 1. 수명주기(Lifecycle)가 다름
 * - 페이지가 파괴되는 순간은 프로세스 종료뿐만 아니라,
 * -- eviction(강제 퇴출),
 * -- munmap/언매핑,
 * -- 실패한 lazy 초기화 롤백,
 * -- copy-on-write 해제
 * 등 “여러 곳”에서 발생.
 * supplemental_page_table_kill()은 그중 프로세스 종료 시나리오 하나만 담당함.
 * → 따라서 정리는 페이지 단위의 공통 경로(= destroy(page) → vm_dealloc_page())를 통해 어디서든 동일하게 수행돼야 함.
 * 2. 타입별 정리 작업이 제각각 (ops로 캡슐화해야 함)
 * - ANON: swap 슬롯 반환, 프레임 연결 해제…
 * - FILE: dirty면 파일/스왑으로 write-back, 파일 참조 해제, 페이지 캐시와의 동기화…
 * - UNINIT: 아직 실체화 전이라면 initializer/aux 메모리 해제…
 * 같은 타입별 책임을 ops->destroy()에 둬야, SPT 쪽은 타입을 몰라도 공통으로 호출만 하면 끝남.
 * 만약 supplemental_page_table_kill()에 몰아 쓰면, 
 * 타입 분기를 이 함수에 복붙하게 되고(나중에 타입 추가될 때마다 수정), 모듈 경계가 깨짐. */