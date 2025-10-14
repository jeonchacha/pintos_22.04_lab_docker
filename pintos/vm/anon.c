/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include <string.h>
#include <stdbool.h>
#include "threads/synch.h"
#include "threads/vaddr.h"		// is_user_vaddr, pg_ofs, PGSIZE
#include "lib/kernel/bitmap.h"

#define SECTORS_PER_PAGE (PGSIZE / DISK_SECTOR_SIZE)	/* 4096 /512 = 8 */

static struct disk *swap_disk;		/* 스왑 디스크 핸들 */
static struct bitmap *swap_map;		/* 슬롯 사용 여부 테이블(1슬롯=1페이지) */
static struct lock swap_lock;		/* swap_map 보호 */

/* 앞으로 쓸, "제로로 채우는" init 콜백(UNINIT.initialize가 호출해줌) */
bool anon_init_zero (struct page *page, void *aux) {
	/* vm_do_claim_page()에서 이미 PTE 매핑과 frame 배정이 끝났고,
       지금은 UNINIT.swap_in(=uninit_initialize) 내부에서 불리는 'init' 콜백 단계.
       따라서 page->frame->kva로 바로 쓸 수 있음. */
	memset(page->frame->kva, 0, PGSIZE);
	return true;
}

/* DO NOT MODIFY BELOW LINE */
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get(1, 1);		/* Pintos 기본: chan=1, dev=1 */
	if (swap_disk == NULL) PANIC("no swap disk");

	size_t slots = disk_size(swap_disk) / SECTORS_PER_PAGE;
	swap_map = bitmap_create(slots);
	if (swap_map == NULL) PANIC("no swap bitmap");

	lock_init(&swap_lock);
}

/* 타입 초기화기: ops만 세팅해 타입을 VM_ANON으로 바꿔준다.
   실제 내용 채우기는 'init 콜백'(위 anon_init_zero)에서 수행. */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	page->operations = &anon_ops;
	page->anon.swap_slot = SIZE_MAX; 	/* 아직 스왑 안탐 */
	return true;
}

/* 스왑인: 슬롯에서 읽어오고 슬롯을 반납 (신규 anon이면 제로필 )*/
static bool
anon_swap_in (struct page *page, void *kva) {
	size_t slot = page->anon.swap_slot;
	if (slot == SIZE_MAX) {		/* 아직 스왑 간 적 없음 -> 제로필 */
		memset(kva, 0, PGSIZE);
		return true;
	}

	for (size_t i = 0; i < SECTORS_PER_PAGE; i++) {
		disk_read(swap_disk, slot * SECTORS_PER_PAGE + i,
				  (uint8_t *)kva + i * DISK_SECTOR_SIZE);
	}

	lock_acquire(&swap_lock);
	bitmap_reset(swap_map, slot);		/* 슬롯 회수 */
	lock_release(&swap_lock);

	page->anon.swap_slot = SIZE_MAX;
	return true;
}

/* 스왑아웃: 메모리 페이지를 스왑 슬롯에 기록하고 슬롯 번호를 보관 */
static bool
anon_swap_out (struct page *page) {
	ASSERT(page->frame && page->frame->kva);
	
	lock_acquire(&swap_lock);
	size_t slot = bitmap_scan_and_flip(swap_map, 0, 1, false);
	lock_release(&swap_lock);
	if (slot == BITMAP_ERROR) PANIC("swap full");

	uint8_t *kva = page->frame->kva;
	for (size_t i = 0; i < SECTORS_PER_PAGE; i++) {
		disk_write(swap_disk, slot * SECTORS_PER_PAGE + i,
				   kva + i * DISK_SECTOR_SIZE);
	}
	page->anon.swap_slot = slot;
	return true;			/* PTE 해제/연결끊기는 evict에서 */
}

/* 파괴(destroy) 시 프레임 분리 + 스왑 슬롯 반납 */
static void
anon_destroy (struct page *page) {
	/* 스왑 슬롯이 남아 있으면 반납 */
    if (page->anon.swap_slot != SIZE_MAX) {
        lock_acquire(&swap_lock);
        bitmap_reset(swap_map, page->anon.swap_slot);
        lock_release(&swap_lock);
        page->anon.swap_slot = SIZE_MAX;
    }

    /* 프레임과의 연결을 정리(사용자 매핑 제거 후 프레임을 '빈 프레임'으로 만들기) */
    if (page->frame) {
        struct frame *fr = page->frame;
        /* pml4 가 아직 살아있을 때만 안전하게 클리어 */
        if (fr->pml4 && page->va)
            pml4_clear_page(fr->pml4, page->va);
        /* 프레임을 빈 상태로 표시(프레임은 frame_table에 남겨 재사용) */
        fr->page = NULL;
        fr->pml4  = NULL;
        page->frame = NULL;
    }
}
