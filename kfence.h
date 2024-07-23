#include <nuttx/mm/mm.h>
#include <nuttx/list.h>

// 最大栈深
#define KFENCE_STACK_DEPTH 32

// 获取x的变量类型保障前后变量类型一样
#define ALIGN_DOWN(x, a) (((x) - ((a) - 1)) & ~((typeof(x))(a) - 1)) 

// canary相关定义
#define KFENCE_CANARY_PATTERN(addr) (0xaa ^ ((unsigned long)(addr) & 0x7))

// 元数据状态
enum kfence_object_state {
    KFENCE_OBJECT_UNUSED,       /* 未使用　*/
    KFENCE_OBJECT_ALLOCATED,    /* 已申请　*/
    KFENCE_OBJECT_FREED,        /* 已释放　*/
};

// kfence的记录栈
struct kfence_track
{
    // 记录时间
    uint64_t ts;

    //
    int pid;

    // 分配和释放时的栈深度
    int num_stack_entries;          
    
    // 记录分配和释放信息
    unsigned long stack_entries[KFENCE_STACK_DEPTH];    
};

struct kfence_metadata {
    // 空闲列表节点
    struct list_node list;

    // 记录状态
    enum kfence_object_state state;

    // 分配的对象的地址
    unsigned long addr;

    // 原始分配大小
    size_t size;

    // 申请与释放栈
    struct kfence_track alloc_track;
    struct kfence_track free_track;
};

// kfence异常类型
enum kfence_error_type {
  KFENCE_ERROR_OOB,           /* 越界访问 */
  KFENCE_ERROR_UAF,           /* 释放后再次使用 */
  KFENCE_ERROR_DoubleFree,    /* 二次释放 */
};







