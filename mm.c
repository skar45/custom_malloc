/*
 * mm.c - Memory allocator that uses segregated free lists
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "mm.h"
#include "memlib.h"

team_t team = {
    "a",
    "b",
    "c",
    "d",
    "e"
};

#define PTR_SIZE 8
#define RESERVED (PTR_SIZE * 2)
#define PAGE_SIZE (1<<12)
#define GET_CHUNK(size) (PAGE_SIZE / size)
// 3 reserved for length, footer, list header
// 1 for alignment
#define ALLOC_LEN(size) (GET_CHUNK(size) - (PTR_SIZE * 4))
#define MODULO_2(a, b) ((unsigned long)a & (b - 1))
#define ALIGN(addr, size) ((char *)(MODULO_2(addr, size) == 0 ? (unsigned long)addr: ((unsigned long)addr & ~(size - 1)) + size))
#define PUT(p, val) (*(unsigned long *)(p) = ((unsigned long)(val)))
#define GET(p) (*(unsigned long *)(p))

// Macros for the sized lists
#define GET_LEN(list) ((size_t)GET(list))
#define PUT_LEN(list, length) (PUT(list, length))
#define GET_START(list) ((char *)GET(list + PTR_SIZE))
#define PUT_START(list, ptr) (PUT(list + PTR_SIZE, ptr))
#define GET_FTR(size, list)  (list + (ALLOC_LEN(size) * size) + (PTR_SIZE * 2))

// Macros for the big list
#define HEADER_BIG(p, size) (PUT(p, (size  | 0x1)))
#define FOOTER_BIG(p, size, next) (PUT(p + size + PTR_SIZE, next))
#define GET_SIZE(p) ((GET(p) | 0x1) - 0x1)
#define GET_NEXT(p, size) (char *)(GET(p + size + PTR_SIZE))
#define CHECK_ALLOC(p) (GET(p) & 0x1)
#define SET_ALLOC(p) (PUT(p, GET(p) | 0x1))
#define SET_FREE(p) (PUT(p, GET(p) - 0x1))
#define GET_BLOCK_ADDR(p) ((void *)(p + PTR_SIZE))

static char *class8;
static char *class16;
static char *class32;
static char *class48;
static char *class64;
static char *classbig;

void mm_check(void)
{
    size_t heap_size = mem_heapsize();
    size_t page_size = mem_pagesize();
    char* start = (char *)mem_heap_lo();
    char* end = (char *)mem_heap_hi();
    printf("heap size: %d, page size: %d, heap start: %x, heap end: %x \n", heap_size, page_size, start, end);
}

static size_t get_class(size_t size)
{
    if (size <= 8) return 8;
    if (size <= 16) return 16;
    if (size <= 32) return 32;
    if (size <= 48) return 48;
    if (size <= 64) return 64;
    return size;
}

static char** get_class_ptr(size_t class_size)
{
    switch (class_size) {
	case 8:
	    return &class8;
	case 16:
	    return &class16;
	case 32:
	    return &class32;
	case 48:
	    return &class48;
	case 64:
	    return &class64;
	default:
	    return &classbig;
    }
}

static void expand_list(char** list, size_t size) {
    char* alloc = (char *)(mem_sbrk(PAGE_SIZE));
    size_t alloc_len = ALLOC_LEN(size);
    alloc = ALIGN(alloc, 8);
    char* footer = GET_FTR(size, *list);
    PUT(footer, alloc);
    PUT(alloc, alloc_len);
    footer = GET_FTR(size, alloc);
    PUT(footer, 0x00);
    char* alloc_start = alloc + PTR_SIZE;
    for (size_t i = 0; i < alloc_len; i++) {
	char* curr = alloc_start + (size * i);
	char* next = curr + size;
	PUT(curr, next);
    }
}

static void create_list(char** list, size_t size) {
    char* alloc = (char *)(mem_sbrk(PAGE_SIZE));
    size_t alloc_len = ALLOC_LEN(size);
    alloc = ALIGN(alloc, 8);
    *list = alloc;
    PUT(*list, alloc_len);
    char* footer = GET_FTR(size, *list);
    PUT(footer, 0x00);
    char* alloc_start = *list + PTR_SIZE;
    for (size_t i = 0; i < alloc_len; i++) {
	char* curr = alloc_start + (size * i);
	char* next = curr + size;
	PUT(curr, next);
    }
}

static char* expand_biglist(char** block_ptr, size_t size) {
    size_t new_size = (size_t)ALIGN(size, 8);
    char* new_block = (char *)(mem_sbrk(new_size + RESERVED));
    new_block = ALIGN(new_block, 8);
    FOOTER_BIG(*block_ptr, new_size, new_block);
    HEADER_BIG(new_block, new_size);
    FOOTER_BIG(new_block, new_size, 0x00);
    return new_block;
}

static char* create_biglist(char** block_ptr, size_t size) {
    size_t new_size = (size_t)ALIGN(size, 8);
    char* new_block = (char *)(mem_sbrk(new_size + RESERVED));
    new_block = ALIGN(new_block, 8);
    HEADER_BIG(new_block, new_size);
    FOOTER_BIG(new_block, new_size, 0x00);
    *block_ptr = new_block;
    return *block_ptr;
}

static char* allocate_block(size_t class_size) {
    if (classbig == NULL) {
	char* block_ptr = create_biglist(&classbig, class_size);
	return block_ptr;
    } else {
	char* block_ptr = classbig;
	while (1) {
	    size_t block_size = GET_SIZE(block_ptr);
	    if ((block_size >= class_size) && !CHECK_ALLOC(block_ptr)) {
		SET_ALLOC(block_ptr);
		return block_ptr;
	    } else {
		char* next = GET_NEXT(block_ptr, block_size);
		if (next == NULL) break;
		block_ptr = next;
	    }
	}
	char* addr = expand_biglist(&block_ptr, class_size);
	SET_ALLOC(addr);
	return addr;
    }
}

static char* allocate_free_list(size_t class_size) {
    char** class_ptr = get_class_ptr(class_size);
    char* addr = *class_ptr;
    char* prev;
    if (addr != NULL) {
	size_t free_length = (size_t)GET(addr);
	while (addr) {
	    if (free_length != 0) {
		 return addr;
	    }
	    prev = addr;
	    char* footer = (char *)GET(GET_FTR(class_size, addr));
	    if (footer == NULL) break;
	    addr = footer;
	    free_length = (size_t)GET(addr);
	}
	expand_list(&prev, class_size);
	addr = (char *)GET(GET_FTR(class_size, prev));
	return addr;
    } else {
	create_list(class_ptr, class_size);
	addr = *class_ptr;
	return addr;
    }
}

/**
 * Mark block as allocated
 */
static void* alloc(char* listptr) {
    char* alloc_addr = GET_START(listptr);
    size_t length = GET_LEN(listptr);
    length--;
    PUT_LEN(listptr, length);
    if (length == 0) {
	PUT_START(listptr, 0x00);
	return (void *) alloc_addr;
    }
    char* next_addr = (char *)GET(alloc_addr);
    PUT_START(listptr, next_addr);
    return (void *) alloc_addr;
}


/* 
 * mm_init -
 */
int mm_init(void)
{
    mem_init();
    return 0;
}


/* 
 * mm_malloc -
 */
void* mm_malloc(size_t size)
{
    size_t class_size = get_class(size);
    if (class_size > 64) {
	char* addr = allocate_block(class_size);
	return GET_BLOCK_ADDR(addr);
    } else {
	char* addr = allocate_free_list(class_size);
	return alloc(addr);
    }
}

/*
 * mm_free -
 */
void mm_free(void *ptr)
{
    char* block = (char *)ptr;
    char* class_ptrs[] = { class8, class16, class32, class48, class64 };
    size_t class_sizes[] = { 8, 16, 32, 48, 64 };
    for (size_t i = 0; i < 5; i++) {
	char* list = class_ptrs[i];
	if (list == NULL) continue;
	size_t size = class_sizes[i];
	while (list) {
	    size_t length = GET_LEN(list);
	    char* footer = GET_FTR(size, list);
	    if (length == ALLOC_LEN(size)) {
		list = (char *)GET(footer);
		continue;
	    };
	    char* min_ptr = list + (PTR_SIZE * 2);
	    if ((block >= min_ptr) && (block < footer)) {
		char* start = GET_START(list);
		PUT(block, start);
		PUT_START(list, block);
		length++;
		PUT_LEN(list, length);
		return;
	    }
	    list = (char *)GET(footer);
	}
    }
    char* list = classbig;
    while (list) {
	if (!CHECK_ALLOC(list)) {
	    list = GET_NEXT(list, GET_SIZE(list));
	    continue;
	}
	char* start = list + PTR_SIZE;
	if (block == start) {
	    SET_FREE(list);
	    return;
	}
	list = GET_NEXT(list, GET_SIZE(list));
    }
}

/**
 * Copies byte data from old location to a new one
 * The new location must have a higher allocation capacity then the old one.
 * `size` must be a multiple of 8
**/
static void copy_data(char* old, char* new, size_t size) {
    for (size_t i = 0; i < size; i += 8) {
	PUT(new + i, GET(old + i));
    }
}

/*
 * mm_realloc -
 */
void *mm_realloc(void *ptr, size_t size)
{
    char* block = (char *)ptr;
    char* found_list = NULL;
    size_t prev_class_size = 0;

    // Search lists for addr
    char* class_ptrs[] = { class8, class16, class32, class48, class64 };
    size_t class_sizes[] = { 8, 16, 32, 48, 64 };
    for (size_t i = 0; i < 5; i++) {
	char* list = class_ptrs[i];
	if (list == NULL) continue;
	prev_class_size = class_sizes[i];
	while (list) {
	    size_t length = GET_LEN(list);
	    char* footer = GET_FTR(prev_class_size, list);
	    if (length == ALLOC_LEN(prev_class_size)) {
		list = (char *)GET(footer);
		continue
	    };
	    char* min_ptr = list + (PTR_SIZE * 2);
	    if ((block >= min_ptr) && (block < footer)) {
		// Return the same address if the class size hasn't increased
		if (prev_class_size >= size) return (void *)block;
		found_list = list;
		length++;
		PUT_LEN(found_list, length);
		break;
	    }
	    list = (char *)GET(footer);
	}
    }

    char* old_block = classbig;
    while (!found_list) {
	if (!CHECK_ALLOC(old_block)) continue;
	char* start = old_block + PTR_SIZE;
	if (block == start) {
	    prev_class_size = GET_SIZE(old_block);
	    // Return the same address if the class size hasn't increased
	    if (prev_class_size >= size) return (void *)block;
	    break;
	}
	old_block = GET_NEXT(old_block, GET_SIZE(old_block));
    }

    // New allocation
    char* new_list;
    size_t new_class_size = get_class(size);
    if (new_class_size > 64) {
	new_list = allocate_block(new_class_size);
    } else {
	new_list = allocate_free_list(new_class_size);
    }

    if (found_list != NULL) {
	copy_data(block, GET_START(new_list), prev_class_size);
	char* start = GET_START(found_list);
	PUT(block, start);
	PUT_START(found_list, block);
	return alloc(new_list);
    } else {
	copy_data(block, new_list + PTR_SIZE, prev_class_size);
	SET_FREE(old_block);
	return GET_BLOCK_ADDR(new_list);
    }
}


