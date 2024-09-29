#include <nuttx/list.h>
#include <stdint.h>
#include <malloc.h>
#include <stdlib.h>
#include <strings.h>

#ifndef __ASSEMBLY__

FAR void *bytechecker_alloc(FAR const char *file, int line, size_t size);
void bytechecker_free(FAR const char *file, int line, FAR const void *ptr);

static void *(*real_malloc)(size_t size) = NULL;
static void (*real_free)(void *ptr) = NULL;

static void init_real_func(void)
{
    real_malloc = malloc;
    real_free = free;
}

#endif

#define malloc(s) bytechecker_alloc(__FILE__, __LINE__, s)
#define free(p) bytechecker_free(__FILE__, __LINE__, p)

// 最大栈深
#define BYTECHECKER_STACK_DEPTH 32

// 获取x的变量类型保障前后变量类型一样
#define ALIGN_DOWN(x, a) (((x) - ((a) - 1)) & ~((typeof(x))(a) - 1)) 

// canary相关定义
#define BYTECHECKER_CANARY_PATTERN(addr) ((uint8_t)0xa3 ^ (uint8_t)((unsigned long)(addr) & 0x7))

// 元数据状态
enum bytechecker_object_state {
    BYTECHECKER_OBJECT_UNUSED,       /* 未使用　*/
    BYTECHECKER_OBJECT_ALLOCATED,    /* 已申请　*/
    BYTECHECKER_OBJECT_FREED,        /* 已释放　*/
};

// bytechecker异常类型
enum bytechecker_error_type {
    BYTECHECKER_ERROR_OOB,   /* 越界访问 */
    BYTECHECKER_ERROR_UAF,   /* 释放后再次使用 */
    BYTECHECKER_ERROR_DF,    /* 二次释放 */
    BYTECHECKER_ERROR_INVALID_FREE, 
};


// bytechecker的记录栈
struct bytechecker_track
{
    // 记录时间
    uint64_t ts;

    //
    int pid;

    // 分配和释放时的栈深度
    int num_stack_entries;          
    
    // 记录分配和释放信息
    unsigned long stack_entries[BYTECHECKER_STACK_DEPTH];    
};

struct bytechecker_metadata {
    // 空闲列表节点
    struct list_node list;

    // 记录状态
    enum bytechecker_object_state state;

    // 分配的对象的地址
    unsigned long addr;

    // 原始分配大小
    size_t size;

    // 申请与释放栈
    struct bytechecker_track alloc_track;
    struct bytechecker_track free_track;
};








