#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/vm.h"
struct page;
enum vm_type;

struct anon_page {
    size_t swap_slot;   /* 스왑 슬롯 인덱스; 사용 안하면 SIZE_MAX 등 */
};

void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);

#endif
