#include <stdio.h>
#include <syslog.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <nuttx/mm/mm.h>
#include <nuttx/kmalloc.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>
#include <nuttx/clock.h>
#include <nuttx/timers/timer.h>
#include <nuttx/signal.h>
#include <nuttx/irq.h>
#include <nuttx/config.h>
#include <nuttx/arch.h>


#define BYTECHECKER_PAGE_SIZE CONFIG_BYTECHECKER_PAGE_SIZE
#define BYTECHECKER_POOL_SIZE ((CONFIG_BYTECHECKER_NUM_OBJECTS * 2 + 1)* BYTECHECKER_PAGE_SIZE)
#define ALLOC_GATE (CONFIG_BYTECHECKER_ALLOC_GATE / 1000)

// 确保一次写入操作
#define WRITE_ONCE(x, val) (*(volatile typeof(x) *)&(x) = (val))

// 内存池指针
char *__bytechecker_pool;

// 用于存放指向metadata的指针
static struct bytechecker_metadata bytechecker_metadata_list[CONFIG_BYTECHECKER_NUM_OBJECTS];

// 初始化空闲列表
static struct list_node bytechecker_freelist;

// bytechecker开启状态
static bool bytechecker_enabled;
static bool running = false;

// 门
static bool bytechecker_can_alloc = false;

timer_t g_timerid;

// 打印内存数据，用于测试输出
static void print(unsigned char *addr, int l)
{
	int count = 0;
	for (int i = 0; i < l; i++){
		printf("%02x", addr[i]);
		count++;
		if(count == 16){
			count = 0;
			printf("\n");
		}
	}
}

const int bytechecker_info()
{
	print(__bytechecker_pool, BYTECHECKER_POOL_SIZE);
	return -1;
}

// 检查地址是否合法
static bool is_bytechecker_address(unsigned long *addr)
{
	return ((unsigned long)((char *)addr - __bytechecker_pool) < BYTECHECKER_POOL_SIZE && addr);
}

// 物理地址到元数据页的转换
static inline struct bytechecker_metadata *addr_to_metadata(unsigned long addr)
{
	int index;

	if (!is_bytechecker_address((void *)addr)){
		return NULL;
	}

	// 计算索引
	index = (addr - (unsigned long)__bytechecker_pool) / (BYTECHECKER_PAGE_SIZE * 2);
	if (index < 0 || index >= CONFIG_BYTECHECKER_NUM_OBJECTS){
		return NULL;
	}

	return &bytechecker_metadata_list[index];
}

// 元数据页到物理地址转换
static inline unsigned long metadata_to_pageaddr(const struct bytechecker_metadata *meta)
{
	unsigned long offset = ((meta - bytechecker_metadata_list) * 2 + 1) * BYTECHECKER_PAGE_SIZE;
	unsigned long pageaddr = (unsigned long)&__bytechecker_pool[offset];
	return pageaddr;
}

/*===报告============================================*/

void bytechecker_report_error(unsigned long addr, const struct bytechecker_metadata *meta, enum bytechecker_error_type type)
{
	const int object_index = meta ? meta - bytechecker_metadata_list + 1 : -1;

	syslog(LOG_ERR, "==================================================\n");
	switch (type)
	{
	case BYTECHECKER_ERROR_OOB:
	{	
		const bool left_of_object = addr < meta->addr;

		syslog(LOG_ERR, "问题：越界访问\n");
		syslog(LOG_ERR, "在位置%d处，地址0x%lx的%s侧\n", object_index, addr, left_of_object ? "左" : "右");
		break;
	}
	case BYTECHECKER_ERROR_UAF:
	{
		syslog(LOG_ERR, "问题：释放后使用\n");
		syslog(LOG_ERR, "在位置%d处,地址0x%lx\n",object_index, addr);
		break;
	}
	case BYTECHECKER_ERROR_DF:
	{
		syslog(LOG_ERR, "问题：重复释放\n");
		syslog(LOG_ERR, "在位置%d处,地址0x%lx\n",object_index, addr);
		break;
	}
	case BYTECHECKER_ERROR_INVALID_FREE:
	{
		syslog(LOG_ERR, "问题：非法释放\n");
		syslog(LOG_ERR, "在位置%d处,地址0x%lx\n",object_index, addr);
		break;
	}
	}

	syslog(LOG_ERR, "==================================================\n");
}

/*===canary================================================*/

// 设置canary字节
static inline bool set_canary_byte(uint8_t *addr)
{
	*addr = BYTECHECKER_CANARY_PATTERN(addr);
	return true;
}

// 检查canary字节
static inline bool check_canary_byte(uint8_t *addr)
{
	if (*addr == BYTECHECKER_CANARY_PATTERN(addr))
		return true;

	return false;
}

// 按位操作
static inline void for_each_canary(const struct bytechecker_metadata *meta, bool (*fn)(uint8_t *))
{
	unsigned long addr;
	enum bytechecker_error_type error_type;

	if (meta->state == BYTECHECKER_OBJECT_ALLOCATED)
		error_type = 0;
	else if (meta->state == BYTECHECKER_OBJECT_FREED)
		error_type = 1;
	else
		error_type = 3;
	
	for (addr = meta->addr - BYTECHECKER_PAGE_SIZE / 2; addr < meta->addr; addr++){
		if (!fn((uint8_t *)addr)){
			bytechecker_report_error(addr, meta, error_type);
			break;
		}
	}

	for (addr = meta->addr + meta->size; addr < meta->addr + BYTECHECKER_PAGE_SIZE * 3 / 2; addr++){
		if (!fn((uint8_t *)addr)){
			bytechecker_report_error(addr, meta, error_type);
			break;
		}
	}
}

/* ===计时任务===================================================== */

static void alloc_gate_callback(int sig)
{
	bytechecker_can_alloc = true;
}

static void start_timer(void)
{
	struct sigevent sev;
	struct itimerspec its;
	struct sigaction sa;

	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = alloc_gate_callback;
	sigemptyset(&sa.sa_mask);
	
	if(sigaction(SIGRTMIN, &sa, NULL) == -1){
		return 0;
	}

	sev.sigev_notify = SIGEV_SIGNAL;
	sev.sigev_signo = SIGRTMIN;
	sev.sigev_value.sival_ptr = &g_timerid;

	if(timer_create(CLOCK_REALTIME, &sev, &g_timerid) == -1){
		return 0;
	}

	its.it_value.tv_sec = ALLOC_GATE;
	its.it_value.tv_nsec = 0;
	its.it_interval.tv_sec = ALLOC_GATE;
	its.it_interval.tv_nsec = 0;

	if (timer_settime(g_timerid, 0, &its, NULL) == -1){
		return 0;
	}
}

/* ===bytechecker内存申请与释放=========================================== */

/* 更新metadata状态 */
static void metadata_update_state(struct bytechecker_metadata *meta, enum bytechecker_object_state next)
{
	WRITE_ONCE(meta->state, next);
}

/* bytechecker内存分配 */
static void *bytechecker_guarded_alloc(size_t size)
{
	struct bytechecker_metadata *meta = NULL;
	void *addr;

	// 从bytechecker_freelist中取出第一个给元素，并重新初始化该节点
	if (!list_is_empty(&bytechecker_freelist)) {
		meta = list_entry(bytechecker_freelist.next, struct bytechecker_metadata, list);
		list_delete_init(&meta->list);
	}

	// 检查是否获取
	if (!meta)
		return real_malloc(size);

	// 如果页面状态为已释放，则需要检查页面
	if (meta->state == BYTECHECKER_OBJECT_FREED)
		for_each_canary(meta, check_canary_byte);

	meta->addr = metadata_to_pageaddr(meta);
	addr = (void *)meta->addr;
	
	metadata_update_state(meta, BYTECHECKER_OBJECT_ALLOCATED);
	meta->size = size;

	for_each_canary(meta, set_canary_byte);
	bytechecker_can_alloc = false;

	return addr;
}

// bytechecker内存释放
static void bytechecker_guarded_free(unsigned long *addr, struct bytechecker_metadata *meta)
{
	if (meta->state != BYTECHECKER_OBJECT_ALLOCATED || meta->addr != (unsigned long)addr){
		bytechecker_report_error((unsigned long)addr, meta, BYTECHECKER_ERROR_INVALID_FREE);
		return;
	}

	for_each_canary(meta, check_canary_byte);
	metadata_update_state(meta, BYTECHECKER_OBJECT_FREED);
	
	meta->size = 0;
	for_each_canary(meta, set_canary_byte);

	list_add_tail(&meta->list, &bytechecker_freelist);
}

/* ===功能=========================================== */

// 内存分配
void *bytechecker_alloc(FAR const char *file, int line, size_t size)
{
	// 检查bytechecker是否开启
	if (!bytechecker_enabled)
		return real_malloc(size);

	// 检查是否可以申请内存（计时器相关）
	if (!bytechecker_can_alloc)
		return real_malloc(size);

	// 检查申请的内存大小
	if (size > BYTECHECKER_PAGE_SIZE)
		return real_malloc(size);
	
	return bytechecker_guarded_alloc(size);
}

// 内存释放
void bytechecker_free(FAR const char *file, int line, FAR const void *ptr)
{
	struct bytechecker_metadata *meta = addr_to_metadata((unsigned long)ptr);
    if (!is_bytechecker_address((unsigned long *)ptr)){
        return;
	}

	bytechecker_guarded_free((unsigned long *)ptr, meta);
}

/* ===bytechecker初始化=========================================== */

// 为bytechecker申请内存池，__bytechecker_pool存放申请到的内存的首地址
static void bytechecker_alloc_pool(void)
{
    __bytechecker_pool = real_malloc(BYTECHECKER_POOL_SIZE);
}   

// 初始化内存池，设置保护页和数据页
static bool bytechecker_init_pool(void)
{
    unsigned long addr = (unsigned long)__bytechecker_pool + BYTECHECKER_PAGE_SIZE;
	
	// 初始化metapage
	for (int i = 0; i < CONFIG_BYTECHECKER_NUM_OBJECTS; i++){
		struct bytechecker_metadata *meta = &bytechecker_metadata_list[i];

		WRITE_ONCE(meta->state, BYTECHECKER_OBJECT_UNUSED);
		meta->addr = addr;

		// 将节点添加到freelist中
		list_initialize(&meta->list);
		list_add_tail(&bytechecker_freelist, &meta->list);

		addr += 2 * BYTECHECKER_PAGE_SIZE;
	}
	return true;
}

// 初始化bytechecker
void bytechecker_init(void)
{
	syslog(LOG_INFO, "---------------------------------\n");
	syslog(LOG_INFO, "[ bytechecker ]bytechecker启动中\n");
	init_real_func();
	list_initialize(&bytechecker_freelist);

    // 申请bytechecker内存
	bytechecker_alloc_pool();
    if(!__bytechecker_pool){
		syslog(LOG_ERR, "[ bytechecker ]bytechecker内存池申请失败\n");
        return;
	}
	
	syslog(LOG_INFO, "[ bytechecker ]内存池申请成功\n");

    // 初始化内存
    if(!bytechecker_init_pool()){
		syslog(LOG_ERR, "[ bytechecker ]内存池初始化失败\n");
        return;
	}

	start_timer();
		
    // 表示bytechecker已使能
    WRITE_ONCE(bytechecker_enabled, true);
	syslog(LOG_INFO, "[ bytechecker ]内存池初始化成功\n");
	syslog(LOG_INFO, "[ bytechecker ]bytechecker已就绪\n");
	syslog(LOG_INFO, "---------------------------------\n");
}

/*===程序============================================*/

void *bytechecker_task(int argc, char *argv)
{

	bytechecker_init();
	while (running){
	}

	real_free(__bytechecker_pool);
	syslog(LOG_INFO, "[ bytechecker ]bytechecker已停止\n");
	
	running = 0;
	return NULL;
}

int start_bytechecker_task(void)
{
	if (running){
		syslog(LOG_INFO, "[ bytechecker ]bytechecker正在运行\n");
		return -1;
	}

	running = true;

	int ret = task_create("bytechecker", 50, 1000, (main_t)bytechecker_task, NULL);
	if (ret == 0){
		syslog(LOG_ERR, "[ bytechecker ]bytechecker启动失败\n");
		running = 0;
		return -1;
	}

	sleep(1);
	return 0;
}

int stop_bytechecker_task(void)
{
	if (!running){
		syslog(LOG_INFO, "[ bytechecker ]bytechecker未运行\n");
		return -1;
	}

	running = 0;
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc < 2){
		syslog(LOG_INFO, "[ bytechecker ]输入bytechecker start启动bytechecker\n");
		syslog(LOG_INFO, "[ bytechecker ]输入bytechecker stop关闭bytechecker\n");
		return -1;
	}

	if (strcmp(argv[1], "start")==0)
		return start_bytechecker_task();
	else if (strcmp(argv[1], "stop")==0)
		return stop_bytechecker_task();
	else
		return bytechecker_info();
}