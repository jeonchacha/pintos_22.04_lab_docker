/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include <string.h>
#include <stdbool.h>
#include "threads/vaddr.h"		// is_user_vaddr, pg_ofs, PGSIZE

/* 스왑 디스크 (후속 단계에서 사용할 예정) */
static struct disk *swap_disk;

/* 앞으로 쓸, "제로로 채우는" init 콜백(UNINIT.initialize가 호출해줌) */
bool anon_init_zero (struct page *page, void *aux) {
	/* vm_do_claim_page()에서 이미 PTE 매핑과 frame 배정이 끝났고,
       지금은 UNINIT.swap_in(=uninit_initialize) 내부에서 불리는 'init' 콜백 단계.
       따라서 page->frame->kva로 바로 쓸 수 있음. */
	memset(page->frame->kva, 0, PGSIZE);
	return true;
}

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
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
	swap_disk = NULL;
}

/* 타입 초기화기: ops만 세팅해 타입을 VM_ANON으로 바꿔준다.
   실제 내용 채우기는 'init 콜백'(위 anon_init_zero)에서 수행. */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	page->operations = &anon_ops;
	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
}
