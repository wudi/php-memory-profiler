/*
  +----------------------------------------------------------------------+
  | Memprof                                                              |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2012 Arnaud Le Blanc                              |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Arnaud Le Blanc <arnaud.lb@gmail.com>                        |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "SAPI.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_memprof.h"
#include "zend_extensions.h"
#include <stdint.h>
#include <sys/queue.h>
#include "util.h"
#include <Judy.h>
#if MEMPROF_DEBUG
#	undef NDEBUG
#endif
#include <assert.h>

#ifdef ZTS
	/* TODO: support ZTS builds (without malloc hooks) */
#	error "ZTS build not supported (yet)"
#endif

#if MEMPROF_CONFIGURE_VERSION != 3
#	error Please rebuild configure (run phpize and reconfigure)
#endif

#if HAVE_MALLOC_HOOKS
#	include <malloc.h>

#	if MEMPROF_DEBUG
#		define MALLOC_HOOK_CHECK_NOT_OWN() \
			if (__malloc_hook == malloc_hook) { \
				fprintf(stderr, "__malloc_hook == malloc_hook (set at %s:%d)", malloc_hook_file, malloc_hook_line); \
				abort(); \
			} \

#		define MALLOC_HOOK_CHECK_OWN() \
			if (__malloc_hook != malloc_hook) { \
				fprintf(stderr, "__malloc_hook != malloc_hook"); \
				abort(); \
			} \

#		define MALLOC_HOOK_SET_FILE_LINE() do { \
			malloc_hook_file = __FILE__; \
			malloc_hook_line = __LINE__; \
		} while (0)

#	else /* MEMPROF_DEBUG */
#		define MALLOC_HOOK_CHECK_NOT_OWN()
#		define MALLOC_HOOK_CHECK_OWN()
#		define MALLOC_HOOK_SET_FILE_LINE()
#	endif /* MEMPROF_DEBUG */

#	define WITHOUT_MALLOC_HOOKS \
		do { \
			int ___malloc_hook_restored = 0; \
			if (__malloc_hook == malloc_hook) { \
				MALLOC_HOOK_RESTORE_OLD(); \
				___malloc_hook_restored = 1; \
			} \
			do \

#	define END_WITHOUT_MALLOC_HOOKS \
			while (0); \
			if (___malloc_hook_restored) { \
				MALLOC_HOOK_SAVE_OLD(); \
				MALLOC_HOOK_SET_OWN(); \
			} \
		} while (0)

#	define MALLOC_HOOK_RESTORE_OLD() \
		/* Restore all old hooks */ \
		MALLOC_HOOK_CHECK_OWN(); \
		__malloc_hook = old_malloc_hook; \
		__free_hook = old_free_hook; \
		__realloc_hook = old_realloc_hook; \
		__memalign_hook = old_memalign_hook; \

#	define MALLOC_HOOK_SAVE_OLD() \
		/* Save underlying hooks */ \
		MALLOC_HOOK_CHECK_NOT_OWN(); \
		old_malloc_hook = __malloc_hook; \
		old_free_hook = __free_hook; \
		old_realloc_hook = __realloc_hook; \
		old_memalign_hook = __memalign_hook; \

#	define MALLOC_HOOK_SET_OWN() \
		/* Restore our own hooks */ \
		__malloc_hook = malloc_hook; \
		__free_hook = free_hook; \
		__realloc_hook = realloc_hook; \
		__memalign_hook = memalign_hook; \
		MALLOC_HOOK_SET_FILE_LINE();

#else /* HAVE_MALLOC_HOOKS */

#	warning No support for malloc hooks, this build will not track persistent allocations

#	define MALLOC_HOOK_CHECK_NOT_OWN()
#	define MALLOC_HOOK_IS_SET() (__malloc_hook == malloc_hook)
#	define MALLOC_HOOK_RESTORE_OLD()
#	define MALLOC_HOOK_SAVE_OLD()
#	define MALLOC_HOOK_SET_OWN()
#	define WITHOUT_MALLOC_HOOKS \
		do { \
			do
#	define END_WITHOUT_MALLOC_HOOKS \
			while (0); \
		} while (0);

#endif /* HAVE_MALLOC_HOOKS */

typedef LIST_HEAD(_alloc_list_head, _alloc) alloc_list_head;

/* a call frame */
typedef struct _frame {
	char * name;
	size_t name_len;
	struct _frame * prev;
	size_t calls;
	HashTable next_cache;
	alloc_list_head allocs;
} frame;

/* an allocated block's infos */
typedef struct _alloc {
#if MEMPROF_DEBUG
	size_t canary_a;
#endif
	LIST_ENTRY(_alloc) list;
	size_t size;
#if MEMPROF_DEBUG
	size_t canary_b;
#endif
} alloc;

typedef union _alloc_bucket_item {
	alloc alloc;
	union _alloc_bucket_item * next_free;
} alloc_bucket_item;

typedef struct _alloc_buckets {
	size_t growsize;
	size_t nbuckets;
	alloc_bucket_item * next_free;
	alloc_bucket_item ** buckets;
} alloc_buckets;

#if HAVE_MALLOC_HOOKS
static void * malloc_hook(size_t size, const void *caller);
static void * realloc_hook(void *ptr, size_t size, const void *caller);
static void free_hook(void *ptr, const void *caller);
static void * memalign_hook(size_t alignment, size_t size, const void *caller);
#if MEMPROF_DEBUG
static int malloc_hook_line = 0;
static const char * malloc_hook_file = NULL;
#endif

static void * (*old_malloc_hook) (size_t size, const void *caller) = NULL;
static void * (*old_realloc_hook) (void *ptr, size_t size, const void *caller) = NULL;
static void (*old_free_hook) (void *ptr, const void *caller) = NULL;
static void * (*old_memalign_hook) (size_t alignment, size_t size, const void *caller) = NULL;
#endif /* HAVE_MALLOC_HOOKS */

static void (*old_zend_execute)(zend_op_array *op_array TSRMLS_DC);
static void (*old_zend_execute_internal)(zend_execute_data *execute_data_ptr, int return_value_used TSRMLS_DC);

static int memprof_initialized = 0;
static int memprof_enabled = 0;
static int track_mallocs = 0;

static frame default_frame;
static frame * current_frame = &default_frame;
static alloc_list_head * current_alloc_list = &default_frame.allocs;
static alloc_buckets current_alloc_buckets;

static Pvoid_t allocs_set = (Pvoid_t) NULL;

static const size_t zend_mm_heap_size = 4096;
static zend_mm_heap * zheap = NULL;
static zend_mm_heap * orig_zheap = NULL;

/* ZEND_DECLARE_MODULE_GLOBALS(memprof) */

#define ALLOC_INIT(alloc, size) alloc_init(alloc, size)

#define ALLOC_LIST_INSERT_HEAD(head, elem) alloc_list_insert_head(head, elem)
#define ALLOC_LIST_REMOVE(elem) alloc_list_remove(elem)

static inline size_t alloc_size(size_t size) {
	return ZEND_MM_ALIGNED_SIZE(size) + sizeof(struct _alloc);
}

static inline void alloc_init(alloc * alloc, size_t size) {
	alloc->size = size;
	alloc->list.le_next = NULL;
	alloc->list.le_prev = NULL;
#if MEMPROF_DEBUG
	alloc->canary_a = alloc->canary_b = size ^ 0x5a5a5a5a;
#endif
}

static void alloc_list_insert_head(alloc_list_head * head, alloc * elem) {
	LIST_INSERT_HEAD(head, elem, list);
}

static void list_remove(alloc * elm) {
	LIST_REMOVE(elm, list);
}

static void alloc_list_remove(alloc * elem) {
	if (elem->list.le_prev || elem->list.le_next) {
		list_remove(elem);
		elem->list.le_next = NULL;
		elem->list.le_prev = NULL;
	}
}

#if MEMPROF_DEBUG

static void alloc_check_single(alloc * alloc, const char * function, int line) {
	if (alloc->canary_a != (alloc->size ^ 0x5a5a5a5a) || alloc->canary_a != alloc->canary_b) {
		fprintf(stderr, "canary mismatch for %p at %s:%d\n", alloc, function, line);
		abort();
	}
}

static void alloc_check(alloc * alloc, const char * function, int line) {
	/* fprintf(stderr, "checking %p at %s:%d\n", alloc, function, line); */
	alloc_check_single(alloc, function, line);
	for (alloc = current_alloc_list->lh_first; alloc; alloc = alloc->list.le_next) {
		alloc_check_single(alloc, function, line);
	}
}

#	define ALLOC_CHECK(alloc) alloc_check(alloc, __FUNCTION__, __LINE__);
#else /* MEMPROF_DEBUG */
#	define ALLOC_CHECK(alloc)
#endif /* MEMPROF_DEBUG */

static void alloc_buckets_destroy(alloc_buckets * buckets)
{
	size_t i;

	for (i = 0; i < buckets->nbuckets; ++i) {
		free(buckets->buckets[i]);
	}
	free(buckets->buckets);
}

static void alloc_buckets_grow(alloc_buckets * buckets)
{
	size_t i;
	alloc_bucket_item * bucket;

	buckets->nbuckets++;
	buckets->buckets = realloc(buckets->buckets, sizeof(*buckets->buckets)*buckets->nbuckets);

	buckets->growsize <<= 1;
	bucket = malloc(sizeof(*bucket)*buckets->growsize);
	buckets->buckets[buckets->nbuckets-1] = bucket;

	for (i = 1; i < buckets->growsize; ++i) {
		bucket[i-1].next_free = &bucket[i];
	}
	bucket[buckets->growsize-1].next_free = buckets->next_free;
	buckets->next_free = &bucket[0];
}

static void alloc_buckets_init(alloc_buckets * buckets)
{
	buckets->growsize = 128;
	buckets->nbuckets = 0;
	buckets->buckets = NULL;
	buckets->next_free = NULL;
	alloc_buckets_grow(buckets);
}

static alloc * alloc_buckets_alloc(alloc_buckets * buckets, size_t size)
{
	alloc_bucket_item * item = buckets->next_free;

	if (item == NULL) {
		alloc_buckets_grow(buckets);
		item = buckets->next_free;
	}

	buckets->next_free = item->next_free;

	ALLOC_INIT(&item->alloc, size);

	return &item->alloc;
}

static void alloc_buckets_free(alloc_buckets * buckets, alloc * a)
{
	alloc_bucket_item * item;
	item = (alloc_bucket_item*) a;
	item->next_free = buckets->next_free;
	buckets->next_free = item;
}

static void destroy_frame(frame * f)
{
	alloc * a;

	free(f->name);

	while (f->allocs.lh_first) {
		a = f->allocs.lh_first;
		ALLOC_CHECK(a);
		ALLOC_LIST_REMOVE(a);
	}

	zend_hash_destroy(&f->next_cache);
}

/* HashTable destructor */
static void frame_dtor(void * pDest)
{
	frame * f = * (frame **) pDest;
	destroy_frame(f);
	free(f);
}

static void init_frame(frame * f, frame * prev, char * name, size_t name_len)
{
	zend_hash_init(&f->next_cache, 0, NULL, frame_dtor, 0);
	f->name = malloc(name_len+1);
	memcpy(f->name, name, name_len+1);
	f->name_len = name_len;
	f->calls = 0;
	f->prev = prev;
	LIST_INIT(&f->allocs);
}

static frame * new_frame(frame * prev, char * name, size_t name_len)
{
	frame * f = malloc(sizeof(*f));
	init_frame(f, prev, name, name_len);
	return f;
}

static frame * get_or_create_frame(zend_execute_data * current_execute_data, frame * prev)
{
	frame * f;
	frame ** f_pp;

	char name[256];
	size_t name_len;

	name_len = get_function_name(current_execute_data, name, sizeof(name));

	if (SUCCESS == zend_hash_find(&prev->next_cache, name, name_len+1, (void**) &f_pp)) {
		f = *f_pp;
	} else {
		f = new_frame(prev, name, name_len);
		zend_hash_add(&prev->next_cache, name, name_len+1, &f, sizeof(f), NULL);
	}

	return f;
}

static size_t frame_alloc_size(const frame * f)
{
	size_t size = 0;
	alloc * alloc;

	LIST_FOREACH(alloc, &f->allocs, list) {
		size += alloc->size;
	}

	return size;
}

static int frame_stack_depth(const frame * f)
{
	const frame * prev;
	int depth = 0;

	for (prev = f; prev; prev = prev->prev) {
		depth ++;
	}

	return depth;
}

static void mark_own_alloc(Pvoid_t * set, void * ptr, alloc * a)
{
	Word_t * p;
	JLI(p, *set, (Word_t)ptr);
	*p = (Word_t) a;
}

static void unmark_own_alloc(Pvoid_t * set, void * ptr)
{
	int ret;

	MALLOC_HOOK_CHECK_NOT_OWN();

	JLD(ret, *set, (Word_t)ptr);
}

alloc * is_own_alloc(Pvoid_t * set, void * ptr)
{
	Word_t * p;

	MALLOC_HOOK_CHECK_NOT_OWN();

	JLG(p, *set, (Word_t)ptr);
	if (p != NULL) {
		return (alloc*) *p;
	} else {
		return 0;
	}
}

#if HAVE_MALLOC_HOOKS

static void * malloc_hook(size_t size, const void *caller)
{
	void *result;

	WITHOUT_MALLOC_HOOKS {

		result = malloc(size);
		if (result != NULL) {
			alloc * a = alloc_buckets_alloc(&current_alloc_buckets, size);
			if (track_mallocs) {
				ALLOC_LIST_INSERT_HEAD(current_alloc_list, a);
			}
			mark_own_alloc(&allocs_set, result, a);
			assert(is_own_alloc(&allocs_set, result));
		}

	} END_WITHOUT_MALLOC_HOOKS;

	return result;
}

static void * realloc_hook(void *ptr, size_t size, const void *caller)
{
	void *result;
	alloc *a;

	WITHOUT_MALLOC_HOOKS {

		if (ptr != NULL && !(a = is_own_alloc(&allocs_set, ptr))) {
			result = realloc(ptr, size);
		} else {
			/* ptr may be freed by realloc, so we must remove it from list now */
			if (ptr != NULL) {
				ALLOC_CHECK(a);
				ALLOC_LIST_REMOVE(a);
				unmark_own_alloc(&allocs_set, ptr);
				alloc_buckets_free(&current_alloc_buckets, a);
			}

			result = realloc(ptr, size);
			if (result != NULL) {
				/* succeeded; add result */
				a = alloc_buckets_alloc(&current_alloc_buckets, size);
				if (track_mallocs) {
					ALLOC_LIST_INSERT_HEAD(current_alloc_list, a);
				}
				mark_own_alloc(&allocs_set, result, a);
			} else if (ptr != NULL) {
				/* failed, re-add ptr, since it hasn't been freed */
				a = alloc_buckets_alloc(&current_alloc_buckets, size);
				if (track_mallocs) {
					ALLOC_LIST_INSERT_HEAD(current_alloc_list, a);
				}
				mark_own_alloc(&allocs_set, ptr, a);
			}
		}

	} END_WITHOUT_MALLOC_HOOKS;

	return result;
}

static void free_hook(void *ptr, const void *caller)
{
	WITHOUT_MALLOC_HOOKS {

		if (ptr != NULL) {
			alloc * a;
			if ((a = is_own_alloc(&allocs_set, ptr))) {
				ALLOC_CHECK(a);
				ALLOC_LIST_REMOVE(a);
				free(ptr);
				unmark_own_alloc(&allocs_set, ptr);
				alloc_buckets_free(&current_alloc_buckets, a);
			} else {
				free(ptr);
			}
		}

	} END_WITHOUT_MALLOC_HOOKS;
}

static void * memalign_hook(size_t alignment, size_t size, const void *caller)
{
	void * result;

	WITHOUT_MALLOC_HOOKS {

		result = memalign(alignment, size);
		if (result != NULL) {
			alloc *a = alloc_buckets_alloc(&current_alloc_buckets, size);
			if (track_mallocs) {
				ALLOC_LIST_INSERT_HEAD(current_alloc_list, a);
			}
			mark_own_alloc(&allocs_set, result, a);
		}	

	} END_WITHOUT_MALLOC_HOOKS;

	return result;
}
#endif /* HAVE_MALLOC_HOOKS */

#define WITHOUT_MALLOC_TRACKING do { \
	int ___old_track_mallocs = track_mallocs; \
	track_mallocs = 0; \
	do

#define END_WITHOUT_MALLOC_TRACKING \
	while (0); \
	track_mallocs = ___old_track_mallocs; \
} while (0)

static void * zend_malloc_handler(size_t size)
{
	void *result;

	assert(memprof_enabled);

	WITHOUT_MALLOC_HOOKS {

		result = zend_mm_alloc(orig_zheap, size);
		if (result != NULL) {
			alloc * a = alloc_buckets_alloc(&current_alloc_buckets, size);
			if (track_mallocs) {
				ALLOC_LIST_INSERT_HEAD(current_alloc_list, a);
			}
			mark_own_alloc(&allocs_set, result, a);
			assert(is_own_alloc(&allocs_set, result));
		}

	} END_WITHOUT_MALLOC_HOOKS;

	return result;
}

static void zend_free_handler(void * ptr)
{
	assert(memprof_enabled);

	WITHOUT_MALLOC_HOOKS {

		if (ptr != NULL) {
			alloc * a;
			if ((a = is_own_alloc(&allocs_set, ptr))) {
				ALLOC_CHECK(a);
				ALLOC_LIST_REMOVE(a);
				zend_mm_free(orig_zheap, ptr);
				unmark_own_alloc(&allocs_set, ptr);
				alloc_buckets_free(&current_alloc_buckets, a);
			} else {
				zend_mm_free(orig_zheap, ptr);
			}
		}

	} END_WITHOUT_MALLOC_HOOKS;
}

static void * zend_realloc_handler(void * ptr, size_t size)
{
	void *result;
	alloc *a;

	assert(memprof_enabled);

	WITHOUT_MALLOC_HOOKS {

		if (ptr != NULL && !(a = is_own_alloc(&allocs_set, ptr))) {
			result = zend_mm_realloc(orig_zheap, ptr, size);
		} else {
			/* ptr may be freed by realloc, so we must remove it from list now */
			if (ptr != NULL) {
				ALLOC_CHECK(a);
				ALLOC_LIST_REMOVE(a);
				unmark_own_alloc(&allocs_set, ptr);
				alloc_buckets_free(&current_alloc_buckets, a);
			}

			result = zend_mm_realloc(orig_zheap, ptr, size);
			if (result != NULL) {
				/* succeeded; add result */
				a = alloc_buckets_alloc(&current_alloc_buckets, size);
				if (track_mallocs) {
					ALLOC_LIST_INSERT_HEAD(current_alloc_list, a);
				}
				mark_own_alloc(&allocs_set, result, a);
			} else if (ptr != NULL) {
				/* failed, re-add ptr, since it hasn't been freed */
				a = alloc_buckets_alloc(&current_alloc_buckets, size);
				if (track_mallocs) {
					ALLOC_LIST_INSERT_HEAD(current_alloc_list, a);
				}
				mark_own_alloc(&allocs_set, ptr, a);
			}
		}

	} END_WITHOUT_MALLOC_HOOKS;

	return result;
}

static void memprof_zend_execute(zend_op_array *op_array TSRMLS_DC)
{
	WITHOUT_MALLOC_TRACKING {

		current_frame = get_or_create_frame(EG(current_execute_data), current_frame);
		current_frame->calls++;
		current_alloc_list = &current_frame->allocs;

	} END_WITHOUT_MALLOC_TRACKING;

	old_zend_execute(op_array TSRMLS_CC);

	current_frame = current_frame->prev;
	current_alloc_list = &current_frame->allocs;
}

static void memprof_zend_execute_internal(zend_execute_data *execute_data_ptr, int return_value_used TSRMLS_DC)
{
	int ignore = 0;

	if (execute_data_ptr->function_state.function->common.function_name) {
		const char * name = execute_data_ptr->function_state.function->common.function_name;
		if (0 == memcmp(name, "call_user_func", sizeof("call_user_func"))
				|| 0 == memcmp(name, "call_user_func_array", sizeof("call_user_func_array")))
		{
			ignore = 1;
		}
	}

	WITHOUT_MALLOC_TRACKING {

		if (!ignore) {
			current_frame = get_or_create_frame(execute_data_ptr, current_frame);
			current_frame->calls++;
			current_alloc_list = &current_frame->allocs;
		}

	} END_WITHOUT_MALLOC_TRACKING;

	if (!old_zend_execute_internal) {
		execute_internal(execute_data_ptr, return_value_used);
	} else {
		old_zend_execute_internal(execute_data_ptr, return_value_used);
	}

	if (!ignore) {
		current_frame = current_frame->prev;
		current_alloc_list = &current_frame->allocs;
	}
}

static void memprof_enable()
{
	alloc_buckets_init(&current_alloc_buckets);

	init_frame(&default_frame, NULL, "root", sizeof("root")-1);
	default_frame.calls = 1;

	MALLOC_HOOK_SAVE_OLD();
	MALLOC_HOOK_SET_OWN();

	memprof_enabled = 1;

	if (is_zend_mm()) {
		/* There is no way to completely free a zend_mm_heap with custom
		 * handlers, so we have to allocated it ourselves. We don't know the
		 * actual size of a _zend_mm_heap struct, but this should be enough. */
		zheap = malloc(zend_mm_heap_size);
		memset(zheap, 0, zend_mm_heap_size);
		zend_mm_set_custom_handlers(zheap, zend_malloc_handler, zend_free_handler, zend_realloc_handler);
		orig_zheap = zend_mm_set_heap(zheap);
	} else {
		zheap = NULL;
		orig_zheap = NULL;
	}
}

static void memprof_disable()
{
	if (zheap) {
		zend_mm_set_heap(orig_zheap);
		free(zheap);
	}

	MALLOC_HOOK_RESTORE_OLD();

	memprof_enabled = 0;

	destroy_frame(&default_frame);

	alloc_buckets_destroy(&current_alloc_buckets);

	JudyLFreeArray(&allocs_set, PJE0);
	allocs_set = (Pvoid_t) NULL;
}

ZEND_DLEXPORT int memprof_zend_startup(zend_extension *extension)
{
	int ret;

	memprof_initialized = 1;

	zend_hash_del(CG(function_table), "memory_get_usage", sizeof("memory_get_usage"));
	zend_hash_del(CG(function_table), "memory_get_peak_usage", sizeof("memory_get_peak_usage"));

	ret = zend_startup_module(&memprof_module_entry);

	return ret;
}

#ifndef ZEND_EXT_API
#define ZEND_EXT_API    ZEND_DLEXPORT
#endif
ZEND_EXTENSION();

ZEND_DLEXPORT zend_extension zend_extension_entry = {
	MEMPROF_NAME,
	MEMPROF_VERSION,
	"Arnaud Le Blanc",
	"https://github.com/arnaud-lb/php-memprof",
	"Copyright (c) 2012",
	memprof_zend_startup,
	NULL,
	NULL,           /* activate_func_t */
	NULL,           /* deactivate_func_t */
	NULL,           /* message_handler_func_t */
	NULL,           /* op_array_handler_func_t */
	NULL,           /* statement_handler_func_t */
	NULL,           /* fcall_begin_handler_func_t */
	NULL,           /* fcall_end_handler_func_t */
	NULL,           /* op_array_ctor_func_t */
	NULL,           /* op_array_dtor_func_t */
	STANDARD_ZEND_EXTENSION_PROPERTIES
};

ZEND_BEGIN_ARG_INFO_EX(arginfo_memprof_dump_callgrind, 0, 0, 1)
	ZEND_ARG_INFO(0, handle)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_memprof_dump_pprof, 0, 0, 1)
	ZEND_ARG_INFO(0, handle)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_memprof_dump_array, 0, 0, 0)
ZEND_END_ARG_INFO()

/* {{{ memprof_functions[]
 */
const zend_function_entry memprof_functions[] = {
	PHP_FE(memprof_dump_callgrind, arginfo_memprof_dump_callgrind)
	PHP_FE(memprof_dump_pprof, arginfo_memprof_dump_pprof)
	PHP_FE(memprof_dump_array, arginfo_memprof_dump_array)
	PHP_FE(memprof_memory_get_usage, NULL)
	PHP_FE(memprof_memory_get_peak_usage, NULL)
	PHP_FALIAS(memory_get_usage, memprof_memory_get_usage, NULL)
	PHP_FALIAS(memory_get_peak_usage, memprof_memory_get_peak_usage, NULL)
	PHP_FE_END    /* Must be the last line in memprof_functions[] */
};
/* }}} */

/* {{{ memprof_module_entry
 */
zend_module_entry memprof_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
	STANDARD_MODULE_HEADER,
#endif
	MEMPROF_NAME,
	memprof_functions,
	PHP_MINIT(memprof),
	PHP_MSHUTDOWN(memprof),
	PHP_RINIT(memprof),        /* Replace with NULL if there's nothing to do at request start */
	PHP_RSHUTDOWN(memprof),    /* Replace with NULL if there's nothing to do at request end */
	PHP_MINFO(memprof),
#if ZEND_MODULE_API_NO >= 20010901
	MEMPROF_VERSION,
#endif
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_MEMPROF
ZEND_GET_MODULE(memprof)
#endif

/* {{{ PHP_INI
 */
/* Remove comments and fill if you need to have entries in php.ini
PHP_INI_BEGIN()
	STD_PHP_INI_ENTRY("memprof.global_value",      "42", PHP_INI_ALL, OnUpdateLong, global_value, zend_memprof_globals, memprof_globals)
	STD_PHP_INI_ENTRY("memprof.global_string", "foobar", PHP_INI_ALL, OnUpdateString, global_string, zend_memprof_globals, memprof_globals)
PHP_INI_END()
*/
/* }}} */

/* {{{ php_memprof_init_globals
 */
/*
static void php_memprof_init_globals(zend_memprof_globals *memprof_globals)
{
	memset(memprof_globals, 0, sizeof(*memprof_globals));
}
*/
/* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(memprof)
{
	/* ZEND_INIT_MODULE_GLOBALS(memprof, php_memprof_init_globals, memprof_globals); */
	/* If you have INI entries, uncomment these lines 
	REGISTER_INI_ENTRIES();
	*/

	if (!memprof_initialized) {
		zend_error(E_CORE_ERROR, "memprof must be loaded as a Zend extension (zend_extension=/path/to/memprof.so)");
		return FAILURE;
	}

	old_zend_execute = zend_execute;
	old_zend_execute_internal = zend_execute_internal;
	zend_execute = memprof_zend_execute;
	zend_execute_internal = memprof_zend_execute_internal;

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(memprof)
{
	/* uncomment this line if you have INI entries
	UNREGISTER_INI_ENTRIES();
	*/

	zend_execute_internal = old_zend_execute_internal;
	zend_execute = old_zend_execute;

	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request start */
/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(memprof)
{
	memprof_enable();
	track_mallocs = 1;

	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request end */
/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(memprof)
{
	track_mallocs = 0;

	memprof_disable();

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(memprof)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "memprof support", "enabled");
	php_info_print_table_end();

	/* Remove comments if you have entries in php.ini
	DISPLAY_INI_ENTRIES();
	*/
}
/* }}} */

static void frame_inclusive_cost(frame * f, size_t * inclusive_size, size_t * inclusive_count)
{
	size_t size = 0;
	size_t count = 0;
	alloc * alloc;
	HashPosition pos;
	frame ** next_pp;

	LIST_FOREACH(alloc, &f->allocs, list) {
		size += alloc->size;
		count ++;
	}

	zend_hash_internal_pointer_reset_ex(&f->next_cache, &pos);
	while (zend_hash_get_current_data_ex(&f->next_cache, (void **)&next_pp, &pos) == SUCCESS) {
		char * str_key;
		uint str_key_len;
		ulong num_key;
		size_t call_size;
		size_t call_count;

		if (HASH_KEY_IS_STRING != zend_hash_get_current_key_ex(&f->next_cache, &str_key, &str_key_len, &num_key, 0, &pos)) {
			continue;
		}

		frame_inclusive_cost(*next_pp, &call_size, &call_count);

		size += call_size;
		count += call_count;

		zend_hash_move_forward_ex(&f->next_cache, &pos);
	}

	*inclusive_size = size;
	*inclusive_count = count;
}

static void dump_frame_array(zval * dest, frame * f)
{
	HashPosition pos;
	frame ** next_pp;
	zval * zframe = dest;
	zval * zcalled_functions;
	alloc * alloc;
	size_t alloc_size = 0;
	size_t alloc_count = 0;
	size_t inclusive_size;
	size_t inclusive_count;

	array_init(zframe);

	LIST_FOREACH(alloc, &f->allocs, list) {
		alloc_size += alloc->size;
		alloc_count ++;
	}

	add_assoc_long_ex(zframe, ZEND_STRS("memory_size"), alloc_size);
	add_assoc_long_ex(zframe, ZEND_STRS("blocks_count"), alloc_count);

	frame_inclusive_cost(f, &inclusive_size, &inclusive_count);
	add_assoc_long_ex(zframe, ZEND_STRS("memory_size_inclusive"), inclusive_size);
	add_assoc_long_ex(zframe, ZEND_STRS("blocks_count_inclusive"), inclusive_count);

	add_assoc_long_ex(zframe, ZEND_STRS("calls"), f->calls);

	MAKE_STD_ZVAL(zcalled_functions);
	array_init(zcalled_functions);

	add_assoc_zval_ex(zframe, ZEND_STRS("called_functions"), zcalled_functions);

	zend_hash_internal_pointer_reset_ex(&f->next_cache, &pos);
	while (zend_hash_get_current_data_ex(&f->next_cache, (void **)&next_pp, &pos) == SUCCESS) {

		char * str_key;
		uint str_key_len;
		ulong num_key;
		zval * zcalled_function;

		if (HASH_KEY_IS_STRING != zend_hash_get_current_key_ex(&f->next_cache, &str_key, &str_key_len, &num_key, 0, &pos)) {
			continue;
		}

		MAKE_STD_ZVAL(zcalled_function);
		dump_frame_array(zcalled_function, *next_pp);
		add_assoc_zval_ex(zcalled_functions, str_key, str_key_len, zcalled_function);

		zend_hash_move_forward_ex(&f->next_cache, &pos);
	}
}

static void dump_frame_callgrind(php_stream * stream, frame * f, char * fname, size_t * inclusive_size, size_t * inclusive_count)
{
	size_t size = 0;
	size_t count = 0;
	size_t self_size = 0;
	size_t self_count = 0;
	alloc * alloc;
	HashPosition pos;
	frame ** next_pp;

	zend_hash_internal_pointer_reset_ex(&f->next_cache, &pos);
	while (zend_hash_get_current_data_ex(&f->next_cache, (void **)&next_pp, &pos) == SUCCESS) {
		char * str_key;
		uint str_key_len;
		ulong num_key;
		size_t call_size;
		size_t call_count;

		if (HASH_KEY_IS_STRING != zend_hash_get_current_key_ex(&f->next_cache, &str_key, &str_key_len, &num_key, 0, &pos)) {
			continue;
		}

		dump_frame_callgrind(stream, *next_pp, str_key, &call_size, &call_count);

		size += call_size;
		count += call_count;

		zend_hash_move_forward_ex(&f->next_cache, &pos);
	}

	stream_printf(stream, "fl=/todo.php\n");
	stream_printf(stream, "fn=%s\n", fname);

	LIST_FOREACH(alloc, &f->allocs, list) {
		self_size += alloc->size;
		self_count ++;
	}
	size += self_size;
	count += self_count;

	stream_printf(stream, "1 %zu %zu\n", self_size, self_count);

	zend_hash_internal_pointer_reset_ex(&f->next_cache, &pos);
	while (zend_hash_get_current_data_ex(&f->next_cache, (void **)&next_pp, &pos) == SUCCESS) {
		char * str_key;
		uint str_key_len;
		ulong num_key;
		size_t call_size;
		size_t call_count;

		if (HASH_KEY_IS_STRING != zend_hash_get_current_key_ex(&f->next_cache, &str_key, &str_key_len, &num_key, 0, &pos)) {
			continue;
		}

		frame_inclusive_cost(*next_pp, &call_size, &call_count);

		stream_printf(stream, "cfl=/todo.php\n");
		stream_printf(stream, "cfn=%s\n", str_key);
		stream_printf(stream, "calls=%zu 1\n", (*next_pp)->calls);
		stream_printf(stream, "1 %zu %zu\n", call_size, call_count);

		zend_hash_move_forward_ex(&f->next_cache, &pos);
	}

	stream_printf(stream, "\n");

	if (inclusive_size) {
		*inclusive_size = size;
	}
	if (inclusive_count) {
		*inclusive_count = count;
	}
}

static void dump_frames_pprof(php_stream * stream, HashTable * symbols, frame * f)
{
	HashPosition pos;
	frame * prev;
	frame ** next_pp;
	size_t size = frame_alloc_size(f);
	size_t stack_depth = frame_stack_depth(f);

	if (0 < size) {
		stream_write_word(stream, size);
		stream_write_word(stream, stack_depth);

		for (prev = f; prev; prev = prev->prev) {
			zend_uintptr_t * symaddr_p;
			if (zend_hash_find(symbols, prev->name, prev->name_len+1, (void**) &symaddr_p) != SUCCESS) {
				/* shouldn't happen */
				zend_error(E_CORE_ERROR, "symbol address not found");
				return;
			}
			stream_write_word(stream, *symaddr_p);
		}
	}

	zend_hash_internal_pointer_reset_ex(&f->next_cache, &pos);
	while (zend_hash_get_current_data_ex(&f->next_cache, (void **)&next_pp, &pos) == SUCCESS) {
		char * str_key;
		uint str_key_len;
		ulong num_key;

		if (HASH_KEY_IS_STRING != zend_hash_get_current_key_ex(&f->next_cache, &str_key, &str_key_len, &num_key, 0, &pos)) {
			continue;
		}

		dump_frames_pprof(stream, symbols, *next_pp);

		zend_hash_move_forward_ex(&f->next_cache, &pos);
	}
}

static void dump_frames_pprof_symbols(php_stream * stream, HashTable * symbols, frame * f)
{
	HashPosition pos;
	frame ** next_pp;
	zend_uintptr_t symaddr;

	if (!zend_hash_exists(symbols, f->name, f->name_len+1)) {
		/* addr only has to be unique */
		symaddr = (symbols->nNumOfElements+1)<<3;
		zend_hash_add(symbols, f->name, f->name_len+1, &symaddr, sizeof(symaddr), NULL);
		stream_printf(stream, "0x%0*x %s\n", sizeof(symaddr)*2, symaddr, f->name);
	}

	zend_hash_internal_pointer_reset_ex(&f->next_cache, &pos);
	while (zend_hash_get_current_data_ex(&f->next_cache, (void **)&next_pp, &pos) == SUCCESS) {
		char * str_key;
		uint str_key_len;
		ulong num_key;

		if (HASH_KEY_IS_STRING != zend_hash_get_current_key_ex(&f->next_cache, &str_key, &str_key_len, &num_key, 0, &pos)) {
			continue;
		}

		dump_frames_pprof_symbols(stream, symbols, *next_pp);

		zend_hash_move_forward_ex(&f->next_cache, &pos);
	}
}

/* {{{ proto void memprof_dump_array(void)
   Returns current memory usage as an array */
PHP_FUNCTION(memprof_dump_array)
{
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		return;
	}

	WITHOUT_MALLOC_TRACKING {

		dump_frame_array(return_value, &default_frame);

	} END_WITHOUT_MALLOC_TRACKING;
}
/* }}} */

/* {{{ proto void memprof_dump_callgrind(resource handle)
   Dumps current memory usage in callgrind format to stream $handle */
PHP_FUNCTION(memprof_dump_callgrind)
{
	zval *arg1;
	php_stream *stream;
	size_t total_size;
	size_t total_count;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &arg1) == FAILURE) {
		return;
	}

	php_stream_from_zval(stream, &arg1);

	WITHOUT_MALLOC_TRACKING {

		stream_printf(stream, "version: 1\n");
		stream_printf(stream, "cmd: unknown\n");
		stream_printf(stream, "positions: line\n");
		stream_printf(stream, "events: MemorySize BlocksCount\n");
		stream_printf(stream, "\n");

		dump_frame_callgrind(stream, &default_frame, "root", &total_size, &total_count);

		stream_printf(stream, "total: %zu %zu\n", total_size, total_count);

	} END_WITHOUT_MALLOC_TRACKING;
}
/* }}} */

/* {{{ proto void memprof_dump_pprof(resource handle)
   Dumps current memory usage in pprof heapprofile format to stream $handle */
PHP_FUNCTION(memprof_dump_pprof)
{
	zval *arg1;
	php_stream *stream;
	HashTable symbols;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &arg1) == FAILURE) {
		return;
	}

	php_stream_from_zval(stream, &arg1);

	WITHOUT_MALLOC_TRACKING {

		zend_hash_init(&symbols, 8, NULL, NULL, 0);

		/* symbols */

		stream_printf(stream, "--- symbol\n");
		stream_printf(stream, "binary=todo.php\n");
		dump_frames_pprof_symbols(stream, &symbols, &default_frame);
		stream_printf(stream, "---\n");
		stream_printf(stream, "--- profile\n");

		/* profile header */

		/* header count */
		stream_write_word(stream, 0);
		/* header words after this one */
		stream_write_word(stream, 3);
		/* format version */
		stream_write_word(stream, 0);
		/* sampling period */
		stream_write_word(stream, 0);
		/* unused padding */
		stream_write_word(stream, 0);

		dump_frames_pprof(stream, &symbols, &default_frame);

	} END_WITHOUT_MALLOC_TRACKING;
}
/* }}} */

/* {{{ proto void memprof_memory_get_usage(bool real)
   Returns the current memory usage */
PHP_FUNCTION(memprof_memory_get_usage)
{
	zend_bool real = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|b", &real) == FAILURE) {
		return;
	}

	if (memprof_enabled && orig_zheap) {
		zend_mm_set_heap(orig_zheap);
		RETVAL_LONG(zend_memory_usage(real));
		zend_mm_set_heap(zheap);
	} else {
		RETVAL_LONG(zend_memory_usage(real));
	}
}
/* }}} */

/* {{{ proto void memprof_memory_get_peak_usage(bool real)
   Returns the peak memory usage */
PHP_FUNCTION(memprof_memory_get_peak_usage)
{
	zend_bool real = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|b", &real) == FAILURE) {
		return;
	}

	if (memprof_enabled && orig_zheap) {
		zend_mm_set_heap(orig_zheap);
		RETVAL_LONG(zend_memory_peak_usage(real));
		zend_mm_set_heap(zheap);
	} else {
		RETVAL_LONG(zend_memory_peak_usage(real));
	}
}
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: et sw=4 ts=4 fdm=marker
 * vim<600: et sw=4 ts=4
 */
