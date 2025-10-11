#ifndef VM_FILE_H
#define VM_FILE_H
#include "filesys/file.h"
#include "vm/vm.h"
#include "list.h"            /* struct list_elem */

struct page;
enum vm_type;

/* 각 VM_FILE 페이지가 알아야 할 최소 메타 */
struct file_page {
	struct file *file;	/* 이 페이지가 읽고/되쓸 파일 핸들(매핑마다 reopen으로 독립 참조) */
	off_t ofs;			/* 파일 내 이 페이지의 시작 오프셋 */
	size_t read_bytes;	/* 파일에서 실제로 읽어올 바이트 수 (마지막 페이지는 PGSIZE보다 작을 수 있음) */
	size_t zero_bytes;	/* 나머지 0으로 채울 바이트 수 = PGSIZE - read_bytes */
};

/* UNINIT 페이지의 init(aux)로 넘겨줄 1회성 정보 패킷 */
/* 실행 파일/파일-백드 페이지 하나를 채우는 데 필요한 메타데이터 */
struct file_lazy_aux {
	struct file *file;		/* 실행 파일(또는 mmap 파일) 핸들 */
	off_t ofs;				/* 이 페이지의 파일 오프셋 */
	size_t read_bytes;		/* 이 페이지에서 파일에서 실제로 읽을 바이트 수 */
	size_t zero_bytes;		/* 이어서 0으로 채울 바이트 수 */
};

/* 한 번의 mmap 호출로 생기는 '메핑 덩어리'를 추적하기 위한 구조체 */
struct mmap_region {
	void *start;			/* 매핑 시작 VA (반환값) */
	size_t page_cnt;		/* 매핑된 페이지 수 */
	struct file *file;		/* 이 매핑 전용으로 reopen한 파일 핸들 */
	bool writable;			/* 페이지 쓰기 가능 여부 */
	struct list_elem elem;	/* thread->mmaps 에 들어갈 리스트 엘리먼트 */
};

/* 초기화/스왑 인/아웃/정리 루틴 */
void vm_file_init (void);
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva);

/* mmap/munmap 본체 (syscall에서 호출) */
void *do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset);
void do_munmap (void *va);
#endif

/* file_page는 페이지 단위 메타(어느 파일/어디서/얼마나 읽을지)를 담음.
 * mmap_region은 매핑 덩어리 단위(여러 페이지) 관리용. file_close()는 region 쪽에서 한 번만!
 * file_lazy_aux는 UNINIT → 첫 폴트 시 로더에 전달할 1회성 정보. */