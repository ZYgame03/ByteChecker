#include <stdio.h>
#include <syslog.h>
#include <nuttx/mm/mm.h>
#include <string.h>
#include <nuttx/kmalloc.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>
#include <nuttx/clock.h>
#include <nuttx/timers/timer.h>
#include <nuttx/signal.h>
#include <nuttx/irq.h>
#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <stdint.h>

#include "kfence.h"

#define KFENCE_PAGE_SIZE (4096)
#define KFENCE_POOL_SIZE ((CONFIG_KFENCE_NUM_OBJECTS + 1) * 2 * KFENCE_PAGE_SIZE)

// 确保一次写入操作
#define WRITE_ONCE(x, val) (*(volatile typeof(x) *)&(x) = (val))

// 内存池指针
char *__kfence_pool;

// 用于存放指向meatadata的指针
static struct kfence_metadata kfence_metadata_list[CONFIG_KFENCE_NUM_OBJECTS];

// 初始化空闲列表
static struct list_node kfence_freelist;

// kfence开启状态
static bool kfence_enabled;

static bool is_kfence_address(unsigned long *addr)
{
	return ((unsigned long)((char *)addr - __kfence_pool) < KFENCE_POOL_SIZE && addr);
}

static inline struct kfence_metadata *addr_to_metadata(unsigned long addr)
{
	long index;

	if (!is_kfence_address((void *)addr))
	{
		return NULL;
	}

	index = (addr - (unsigned long)__kfence_pool) / (KFENCE_PAGE_SIZE * 2) - 1;
	if (index < 0 || index >= CONFIG_KFENCE_NUM_OBJECTS)
	{
		return NULL;
	}
	syslog(LOG_ERR, "[ a2m ]addr:%lx,meta->addr:%lx", addr, kfence_metadata_list[index].addr);
	return &kfence_metadata_list[index];
}

static inline unsigned long metadata_to_pageaddr(const struct kfence_metadata *meta)
{
	unsigned long offset = (meta - kfence_metadata_list + 1) * KFENCE_PAGE_SIZE * 2;
	unsigned long pageaddr = (unsigned long)&__kfence_pool[offset];
	return pageaddr;
}

/*===canary================================================*/
static inline bool set_canary_byte(uint8_t *addr)
{
	*addr = KFENCE_CANARY_PATTERN(addr);
	return true;
}

static inline bool check_canary_byte(uint8_t *addr)
{
	if (*addr == KFENCE_CANARY_PATTERN(addr))
	{
		syslog(LOG_INFO, "正确");
		return true;
	}

	// kfence_report error();
	syslog(LOG_ERR, "[ canary ]canary验证失败，可能发生越界");
	return false;
}

static inline void for_each_canary(const struct kfence_metadata *meta, bool (*fn)(uint8_t *))
{

	const unsigned long pageaddr = meta->addr;
	unsigned long addr;

	for (addr = pageaddr; addr < meta->addr; addr++)
	{
		if (!fn((uint8_t *)addr))
			break;
	}

	for (addr = meta->addr + meta->size; addr < pageaddr + KFENCE_PAGE_SIZE; addr++)
	{
		if (!fn((uint8_t *)addr))
			break;
	}
}

/* ===对内存设置/取消保护=====================================*/

// 计划使用MMU进行内存保护
static inline bool kfence_protect_page(unsigned long addr, bool protect)
{
    // 使用ＭＭＵ进行内存设置
	return true;
}

// 设置保护页（调用kfence_project_page)
static bool kfence_protect(unsigned long addr)
{
    return kfence_protect_page(ALIGN_DOWN(addr, KFENCE_PAGE_SIZE), true);
}

// 取消保护页（调用kfence_project_page）
static bool kfence_unprotect(unsigned long addr)
{
    return kfence_protect_page(ALIGN_DOWN(addr, KFENCE_PAGE_SIZE), false);
}

/* ===kfence内存申请与释放=========================================== */

/* 更新metadata状态 */
/* 栈相关操作函数有待研究 */
static void metadata_update_state(struct kfence_metadata *meta, enum kfence_object_state next)
{
	// struct kfence_track *track =
	// 	next == KFENCE_OBJECT_FREED ? &meta->free_track : &meta->alloc_track;

	// track->num_stack_entries = stack_trace_save(track->stack_entries, KFENCE_STACK_DEPTH, 1);
	// track->pid = task_pid_nr(current);

	WRITE_ONCE(meta->state, next);
	// syslog(LOG_INFO, "[ updata ]已更改metadata状态");
}

/* kfence内存分配 */
static void *kfence_guarded_alloc(size_t size)
{
	struct kfence_metadata *meta = NULL;
	void *addr;

	// 从kfence_freelist中取出第一个给元素，并重新初始化该节点
	if (!list_is_empty(&kfence_freelist)) {
		meta = list_entry(kfence_freelist.next, struct kfence_metadata, list);
		list_delete_init(&meta->list);
	}

	// 检查是否获取
	if (!meta) {
		syslog(LOG_ERR, "[ g_alloc ]申请失败，无空闲页面");
		return 0;
	}

	// 如果页面状态为已释放，则需要解除页面保护
	if (meta->state == KFENCE_OBJECT_FREED)
		kfence_unprotect(meta->addr);

	// syslog(LOG_INFO, "地址为0x%lx\n", meta->addr);

	meta->addr = metadata_to_pageaddr(meta);
	addr = (void *)meta->addr;
	
	metadata_update_state(meta, KFENCE_OBJECT_ALLOCATED);
	meta->size = size;

	// for_each_canary(meta, set_canary_byte);
	// can_kfence_alloc = false;

	syslog(LOG_INFO, "[ g_alloc ]申请成功，地址为0x%lx", addr);
	return addr;
}

// kfence内存释放
static void kfence_guarded_free(void *addr, struct kfence_metadata *meta)
{
	if (meta->state != KFENCE_OBJECT_ALLOCATED || meta->addr != (unsigned long)addr) 
	{
		// kfence_report_error((unsigned long)addr, false, NULL, meta, KFENCE_ERROR_INVALID_FREE);
		syslog(LOG_ERR, "非法释放\n");
		return;
	}

	metadata_update_state(meta, KFENCE_OBJECT_FREED);
	for_each_canary(meta, check_canary_byte);


	explicit_bzero(addr, meta->size);
	list_add_tail(&meta->list, &kfence_freelist);

	kfence_protect((unsigned long)addr);
	syslog(LOG_INFO, "[ g_free ]地址0x%lx,释放成功", addr);
}


/* ===功能=========================================== */

// 内存分配
unsigned long *kfence_alloc(size_t size)
{
	syslog(LOG_INFO, "[ alloc ]开始申请内存...");
	// 检查kfence是否开启
	if (!kfence_enabled)
		return NULL;

	// 检查是否可以申请内存（计时器相关）
	// if (!can_kfence_alloc)
	// 	return NULL;

	// 检查申请的内存大小
	if (size > KFENCE_PAGE_SIZE){
		syslog(LOG_INFO, "[ alloc ]申请失败，申请的内存过大");
		return NULL;
	}
	
	return kfence_guarded_alloc(size);
}

// 内存释放
void kfence_free(void *addr)
{
	syslog(LOG_INFO, "[ alloc ]开始释放内存...");
	struct kfence_metadata *meta = addr_to_metadata((unsigned long)addr);
    // if (!is_kfence_address(addr))
    //     return false;

	kfence_guarded_free(addr, meta);
}

/* ===kfence初始化=========================================== */

// 为kfence申请内存池，__kfence_pool存放申请到的内存的首地址
static void kfence_alloc_pool(void)
{
    __kfence_pool = kmm_malloc(KFENCE_POOL_SIZE);
}   

// 初始化内存池，设置保护页和数据页
static bool kfence_init_pool(void)
{
    unsigned long addr = (unsigned long)__kfence_pool;

	// 将偶数页设置为可分配状态（未完成）
    for (int i = 0; i < KFENCE_POOL_SIZE / KFENCE_PAGE_SIZE; i++){
        if (!i || (i % 2))
            continue;
		// 设置为可分配
    }
	
	// 为前两页设置内存保护
    for (int i = 0; i < 2; i++){
        kfence_protect(addr);
        addr += KFENCE_PAGE_SIZE;
    }

	// 初始化metapage
	for (int i = 0; i < CONFIG_KFENCE_NUM_OBJECTS; i++){
		struct kfence_metadata *meta = &kfence_metadata_list[i];

		meta->state = KFENCE_OBJECT_UNUSED;
		meta->addr = addr;

		// 将节点添加到freelist中
		list_initialize(&meta->list);
		list_add_tail(&kfence_freelist, &meta->list);

		// 为下一页设置保护
		kfence_protect(addr + KFENCE_PAGE_SIZE);

		syslog(LOG_INFO, "[ init ]第%d页地址为0x%lx", i + 1, addr);
		addr += 2 * KFENCE_PAGE_SIZE;
	}
	return true;
}

// 初始化kfence
void kfence_init(void)
{
	list_initialize(&kfence_freelist);

    // 申请kfence内存
	kfence_alloc_pool();
    if(!__kfence_pool){
		syslog(LOG_ERR, "[ init ]kfence内存池申请失败\n");
        return;
	}
	syslog(LOG_INFO, "---------------------------------\n");
	syslog(LOG_INFO, "[ init ]内存池申请成功\n");
	syslog(LOG_INFO, "[ init ]内存池地址为0x%lx\n", __kfence_pool);
	syslog(LOG_INFO, "---------------------------------\n");


    // 初始化内存
    if(!kfence_init_pool()){
		syslog(LOG_ERR, "[ init ]kfence内存池初始化失败\n");
        return;
	}
		
    // 表示kfence已使能
    WRITE_ONCE(kfence_enabled, true);
	syslog(LOG_INFO, "---------------------------------\n");
	syslog(LOG_INFO, "[ init ]kfence已就绪\n");
	syslog(LOG_INFO, "---------------------------------\n");
}

/*===运行============================================*/

int main(int argc, FAR char *argv[])
{
	kfence_init();
	printf("---------------测试----------------\n");
	unsigned long a;
	a = kfence_alloc(2048);
	for(int i = 0; i < 8; i++)
		kfence_alloc(2048);
	kfence_alloc(8192);
	syslog(LOG_INFO, "[ test ]a值为0x%lx", a);
	kfence_free(a);
	kfence_alloc(2048);
	return 0;
}