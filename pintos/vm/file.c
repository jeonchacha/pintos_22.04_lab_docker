/* file.c: Implementation of memory backed file object (mmaped object). */

/* 핵심 포인트
 * 초기 로딩: file_lazy_load()가 aux->page 메타 이관 + 즉시 읽기를 수행(UNINIT.init).
 * 재로딩: file_backed_swap_in()은 page 메타를 사용해 파일에서 다시 읽음.
 * write-back: 축출/munmap 시 dirty만 read_bytes 만큼만 파일로 씀(파일 끝 넘어가면 안 됨).
 * 파일닫기: 페이지 단위가 아닌 region 단위에서 한 번만. */

#include "vm/vm.h"
#include "vm/file.h"
#include "threads/thread.h"
#include "threads/mmu.h"		/* pml4_* */
#include "threads/vaddr.h"		/* pg_round_down */
#include "threads/palloc.h"
#include "threads/synch.h"
#include <round.h>				/* ROUND_UP */

extern struct lock fs_lock;

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
/* 타입 초기화자: UNINIT → FILE 타입으로 전환할 때 호출됨.
   (여기서는 ops만 세팅. 파일/오프셋 정보는 init(aux) 쪽에서 채움) */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;
	/* page->file 필드는 init(aux)에서 채워짐 */
	return true;
}

/* UNINIT.init로 사용할 '처음 로딩' 초기화자: aux 정보를 page->file에 옮기고 즉시 로드 */
static bool
file_lazy_load (struct page *page, void *aux_) {
	struct file_lazy_aux *aux = aux_;
	struct file_page *fp = &page->file;
	void *kva = page->frame->kva;

	/* 1) 페이지 메타 채우기 */
	fp->file = aux->file;
	fp->ofs = aux->ofs;
	fp->read_bytes = aux->read_bytes;
	fp->zero_bytes = aux->zero_bytes;

	/* 2) 파일에서 읽고 나머지 0 채움 */
	if (fp->read_bytes > 0) {
		lock_acquire(&fs_lock);
		file_seek(fp->file, fp->ofs);
		int n = file_read(fp->file, kva, (int)fp->read_bytes);
		lock_release(&fs_lock);

		if (n != (int)fp->read_bytes) {
			// free(aux);
			return false;
		}
	}
	if (fp->zero_bytes > 0) {
		memset((uint8_t *)kva + fp->read_bytes, 0, fp->zero_bytes);
	}

	free(aux); 		/* 1회성 aux는 여기서 수거 */
	return true;
}

/* Swap in the page by read contents from the file. */
/* (이후 재진입 폴트 시) 파일에서 다시 읽어오기 */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *fp = &page->file;
	if (fp->read_bytes > 0) {
		lock_acquire(&fs_lock);
		int n = file_read_at(fp->file, kva, (int)fp->read_bytes, fp->ofs);
		lock_release(&fs_lock);

		if (n != (int)fp->read_bytes) return false;
	}
	if (fp->zero_bytes > 0) {
		memset((uint8_t *)kva + fp->read_bytes, 0, fp->zero_bytes);
	}

	return true;
}

/* Swap out the page by writeback contents to the file. */
/* 축출 시: dirty면 파일로 write-back */
static bool
file_backed_swap_out (struct page *page) {
	
	struct frame *fr = page->frame;
	/* thread_current()->pml4 대신, 프레임 주인의 pml4 사용 */
	uint64_t *owner_pml4 = fr ? fr->pml4 : thread_current()->pml4;

	/* 하드웨어 dirty 비트로 판단 */
	if (pml4_is_dirty(owner_pml4, page->va)) {
		struct file_page *fp = &page->file;
		lock_acquire(&fs_lock);

		/* 파일 끝을 넘어서는 부분은 기록하면 안됨 -> read_bytes 만큼만 write-back */
		/* seek+write  대신 write_at 사용 -> 포지션 공유/실수 차단 */
		(void)file_write_at(fp->file, fr->kva, (int)fp->read_bytes, fp->ofs);
		lock_release(&fs_lock);

		pml4_set_dirty(owner_pml4, page->va, false);
	}
	return true;	/* PTE clear 와 frame 연결해제는 vm_evict_frame()에서 */
}

/* 페이지 제거: region이 파일 닫기를 담당하므로 여기서는 아무 것도 안 함 
 * 파일 백드 페이지도 프레임을 깨끗한 빈 슬롯으로 만들고 남김. 
 * (파일 close/write-back은 이미 do_munmap()/region 레벨에서 처리) */
static void
file_backed_destroy (struct page *page) {
	if (page->frame) {
    	struct frame *fr = page->frame;
        /* pml4 가 아직 유효할 때만 클리어 (do_munmap 쪽에서 이미 클리어했어도 무해) */
        if (fr->pml4 && page->va)
            pml4_clear_page(fr->pml4, page->va);
        /* 프레임을 빈 상태로 */
        fr->page = NULL;
        fr->pml4 = NULL;
        page->frame = NULL;
    }

	/* 파일 핸들/매핑 정리는 상위에서: 
     - 실행 파일: process_cleanup()
     - mmap 파일: do_munmap()  */
}

/* Do the mmap */
/* addr부터 length 바이트를 파일(fd 재오픈 핸들)로 lazy 매핑 */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	
	if (addr == NULL || length == 0) return NULL;
	if ((uintptr_t)addr % PGSIZE != 0) return NULL;
	if (offset % PGSIZE != 0) return NULL;

	/* 1) 시작 주소가 사용자 영역인지 확인 [mmap-kernel.c] */
	if (!is_user_vaddr(addr)) return NULL;

	/* 2) 오버플로 방지 + 끝주소도 사용자 영역인지 확인 */
	size_t rounded = ROUND_UP(length, PGSIZE);
    uintptr_t start = (uintptr_t)addr;
    uintptr_t end   = start + rounded;

    /* 덧셈 오버플로 또는 커널 경계 침범 */
    if (end < start) return NULL;                    		/* overflow */
    if (!is_user_vaddr((void *)(end - 1))) return NULL;  	/* [addr, end) 전부 user? */

	/* 페이지 개수 */
	size_t page_cnt = rounded / PGSIZE;
	/* 겹침 금지: 매핑될 모든 VA가 비어있는지 확인 */
	struct supplemental_page_table *spt = &thread_current()->spt;
	for (size_t i = 0; i < page_cnt; i++) {
		void *va = (uint8_t *)addr + i * PGSIZE;
		if (spt_find_page(spt, va) != NULL) return NULL;
	}

	/* 파일 길이 확인 + region 전용 파일 핸들 준비 */
	lock_acquire(&fs_lock);
	off_t flen = file_length(file);
	struct file *re = file_reopen(file);
	lock_release(&fs_lock);
	if (re == NULL) return NULL;
	if (flen == 0) {
		lock_acquire(&fs_lock);
		file_close(re);
		lock_release(&fs_lock);
		return NULL;
	}

	/* region 객체 생성하여 쓰기 */
	struct mmap_region *region = malloc(sizeof *region);
	if (!region) {
		lock_acquire(&fs_lock);
		file_close(re);
		lock_release(&fs_lock);
		return NULL;
	}
	region->start = addr;
	region->page_cnt = page_cnt;
	region->file = re;
	region->writable = (writable != 0);

	/* 페이지별 SPT 등록 (UNINIT + lazy 로더) */
	size_t remaining = length;
	off_t ofs = offset;

	for (size_t i = 0; i < page_cnt; i++) {
		size_t page_read = 0;
		if (ofs < flen) {
			/* 파일 내 남은 바이트 범위에서 이 페이지가 읽을 크기 산출 */
			off_t left_in_file = flen - ofs;
			page_read = left_in_file < PGSIZE ? (size_t)left_in_file : PGSIZE;
			if (page_read > remaining) page_read = remaining; 	/* mapping length 상한 */
		}
		size_t page_zero = PGSIZE - page_read;

		struct file_lazy_aux *aux = malloc(sizeof *aux);
		if (!aux) { 	/* 롤백 */
			for (size_t j = 0; j < i; j++) {
				void *va = (uint8_t *)addr + j * PGSIZE;
				struct page *p = spt_find_page(spt, va);
				if (p) spt_remove_page(spt, p);
			}
			lock_acquire(&fs_lock);
			file_close(re);
			lock_release(&fs_lock);
			free(region);
			return NULL;
		}
		*aux = (struct file_lazy_aux){
			.file = re,
			.ofs = ofs,
			.read_bytes = page_read,
			.zero_bytes = page_zero
		};

		if (!vm_alloc_page_with_initializer(VM_FILE, (uint8_t *)addr + i * PGSIZE, 
											region->writable, file_lazy_load, aux)) {
			free(aux);
			/* 롤백 (위와 동일) */
			for (size_t j = 0; j < i; j++) {
				void *va = (uint8_t *)addr + j * PGSIZE;
				struct page *p = spt_find_page(spt, va);
				if (p) spt_remove_page(spt, p);
			}
			lock_acquire(&fs_lock);
			file_close(re);
			lock_release(&fs_lock);
			free(region);
			return NULL;
		}

		ofs += PGSIZE;
		if (remaining >= PGSIZE)
			remaining -= PGSIZE;
		else
			remaining = 0;
	}

	/* thread에 region 등록 */
	list_push_back(&thread_current()->mmaps, &region->elem);
	return addr;
}

/* Do the munmap */
/* addr로 시작했던 매핑 하나를 정리: dirty만 write-back, SPT 제거, region 닫기 */
void
do_munmap (void *addr) {
	struct thread *t = thread_current();
	struct mmap_region *region = NULL;

	/* 매핑 시작 주소로 region 찾기 */
	for (struct list_elem *e = list_begin(&t->mmaps); e != list_end(&t->mmaps); e = list_next(e)) {
		struct mmap_region *r = list_entry(e, struct mmap_region, elem);
		if (r->start == addr) {
			region = r;
			break;
		}
	}
	if (region == NULL) return;

	/* 각 페이지에 대해 write-back(필요시) -> 매핑 제거 -> SPT 제거 */
	for (size_t i = 0; i < region->page_cnt; i++) {
		void *va = (uint8_t *)region->start + i * PGSIZE;
		struct page *p = spt_find_page(&t->spt, va);
		if (!p) continue;	/* 이미 제거된 경우 */

		/* 프레임이 있고 dirty면 파일로 write-back (read_bytes 만큼만) */
		if (p->frame && pml4_is_dirty(t->pml4, va)) {
			struct file_page *fp = &p->file;
			lock_acquire(&fs_lock);
			(void) file_write_at(fp->file, p->frame->kva, (int)fp->read_bytes, fp->ofs);
			lock_release(&fs_lock);
			pml4_set_dirty(t->pml4, va, false);
		}

		/* 실제 매핑을 걷고, SPT에서 제거(타입별 destroy 호출 포함) */
		pml4_clear_page(t->pml4, va);

		/* 프레임이 붙어 있으면 프레임도 고아되지 않게 끊어준다. */
	    if (p->frame) {
    		struct frame *fr = p->frame;
     		fr->page = NULL;
      		fr->pml4 = NULL;
      		p->frame = NULL;
    	}

		spt_remove_page(&t->spt, p);
	}

	/* region 마무리: 파일 닫고 리스트에서 제거 */
	lock_acquire(&fs_lock);
    file_close(region->file);
    lock_release(&fs_lock);

    list_remove(&region->elem);
    free(region);
}