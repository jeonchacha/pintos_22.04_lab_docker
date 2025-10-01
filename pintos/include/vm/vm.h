#ifndef VM_VM_H
#define VM_VM_H
#include <stdbool.h>
#include "threads/palloc.h"

enum vm_type {
	/* 아직 초기화되지 않은(uninitialized) 페이지: lazy 로딩에 쓰임 */
	VM_UNINIT = 0,
	/* 파일과 무관한(익명) 페이지: 스왑으로만 백업/복원되는 일반 힙/스택 영역 */
	VM_ANON = 1,
	/* 파일로부터 내용이 채워지는 페이지: 코드/데이터, mmap 등이 해당 */
	VM_FILE = 2,
	/* page that hold the page cache, for project 4 */
	VM_PAGE_CACHE = 3,

	/* Bit flags to store state */

	/* Auxillary bit flag marker for store information. You can add more
	 * markers, until the value is fit in the int. */
	VM_MARKER_0 = (1 << 3),
	VM_MARKER_1 = (1 << 4),

	/* DO NOT EXCEED THIS VALUE. */
	VM_MARKER_END = (1 << 31),
};

#include "vm/uninit.h"
#include "vm/anon.h"
#include "vm/file.h"
#ifdef EFILESYS
#include "filesys/page_cache.h"
#endif

#include "hash.h"

struct page_operations;
struct thread;

#define VM_TYPE(type) ((type) & 7)

/* "struct page"는 모든 페이지 객체의 공통 헤더(부모 클래스 같은 개념).
 * 실제 데이터는 union의 각 타입별(struct uninit_page, struct anon_page, struct file_page) 안에 들어감. */
struct page {
	const struct page_operations *operations;
	void *va;              /* Address in terms of user space */
	struct frame *frame;   /* Back reference for frame */

	/* Your implementation */
	bool writable;				// 이 페이지를 유저가 쓸 수 있는지
	struct hash_elem h_elem;	// SPT(해시) 인덱싱용
	enum vm_type type;			// 캐시(ops->type과 동일, 디버깅 편의)

	/* Per-type data are binded into the union.
	 * Each function automatically detects the current union */
	union {
		struct uninit_page uninit;
		struct anon_page anon;
		struct file_page file;
#ifdef EFILESYS
		struct page_cache page_cache;
#endif
	};
};

/* "struct frame"은 물리 페이지(커널 가상주소로 맵핑된 한 프레임)를 뜻함. */
struct frame {
	void *kva;			// 이 프레임의 커널 가상주소(커널이 이 물리 페이지에 접근할 때 사용)
	struct page *page;	// 이 프레임을 점유 중인 상응하는 'struct page'(없으면 NULL)
};

/* 각 페이지 타입이 구현해야 하는 인터페이스(연산 테이블).
 * C에서 인터페이스/가상함수 흉내 내는 전형적인 패턴. */
struct page_operations {
	bool (*swap_in) (struct page *, void *);	// 디스크/스왑/파일에서 내용을 메모리로 들여와 매핑
	bool (*swap_out) (struct page *);			// 현재 프레임 내용을 적절한 백엔드(스왑/파일)에 내보내고 프레임 회수
	void (*destroy) (struct page *);			// 이 페이지 객체가 제거될 때의 정리 작업
	enum vm_type type;
};

#define swap_in(page, v) (page)->operations->swap_in ((page), v)
#define swap_out(page) (page)->operations->swap_out (page)
#define destroy(page) \
	if ((page)->operations->destroy) (page)->operations->destroy (page)

/* Representation of current process's memory space.
 * We don't want to force you to obey any specific design for this struct.
 * All designs up to you for this. */
struct supplemental_page_table {
	struct hash pages; 			// key: upage(va), value: struct page*
};

#include "threads/thread.h"
void supplemental_page_table_init (struct supplemental_page_table *spt);
bool supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src);
void supplemental_page_table_kill (struct supplemental_page_table *spt);
struct page *spt_find_page (struct supplemental_page_table *spt,
		void *va);
bool spt_insert_page (struct supplemental_page_table *spt, struct page *page);
void spt_remove_page (struct supplemental_page_table *spt, struct page *page);

/* VM 서브시스템 전역 초기화(프레임 풀, 스왑, 페이지 캐시 등 하위 시스템 초기화 포함 가능) */
void vm_init (void);

/* 페이지 폴트를 처리하려 시도: 
 * - addr: fault VA, 
 * - user: 유저 모드에서의 폴트인지, 
 * - write: 쓰기 폴트인지, 
 * - not_present: PTE의 present 비트가 0이었는지(=미매핑/스왑아웃/권한X 등 구분에 도움) */
bool vm_try_handle_fault (struct intr_frame *f, void *addr, bool user,
		bool write, bool not_present);

#define vm_alloc_page(type, upage, writable) \
	vm_alloc_page_with_initializer ((type), (upage), (writable), NULL, NULL)

/* 가장 중요한 API 중 하나: 'upage'에 대한 struct page를 SPT에 등록.
 * - type: VM_ANON/VM_FILE/… (VM_UNINIT로 등록하고 lazy init도 가능)
 * - writable: 유저 쓰기 허용 여부
 * - init: lazy 초기화자(첫 접근 시 내용을 채울 함수), 없으면 즉시 타입 초기화
 * - aux: lazy 초기화자가 필요로 하는 부가 정보(파일 포인터+오프셋 등) */
bool vm_alloc_page_with_initializer (enum vm_type type, void *upage,
		bool writable, vm_initializer *init, void *aux);
void vm_dealloc_page (struct page *page);

/* 주어진 VA를 '가져온다'(claim): 
 * - 프레임을 할당하고 
 * - swap_in(또는 uninit→실제 타입 초기화) 수행한 뒤 
 * - 실제 매핑(pml4_set_page)을 완성 */
bool vm_claim_page (void *va);
enum vm_type page_get_type (struct page *page);

#endif  /* VM_VM_H */
