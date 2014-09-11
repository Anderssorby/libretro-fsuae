
#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "threaddep/thread.h"
#include "machdep/rpt.h"
#include "memory.h"
#include "cpuboard.h"
#include "debug.h"
#include "custom.h"
#include "uae.h"
#include "uae/dlopen.h"
#include "uae/log.h"
#include "uae/ppc.h"

#define PPC_SYNC_WRITE 0
#define PPC_ACCESS_LOG 0

#define PPC_DEBUG_ADDR_FROM 0x000000
#define PPC_DEBUG_ADDR_TO   0xffffff

#ifdef WITH_PEARPC_CPU
#include "pearpc/cpu/cpu.h"
#include "pearpc/io/io.h"
#include "pearpc/cpu/cpu_generic/ppc_cpu.h"
#endif

#define TRACE(format, ...) write_log(_T("PPC: ---------------- ") format, ## __VA_ARGS__)


#ifdef FSUAE
#include <glib.h>
static GMutex mutex;
#else
static volatile unsigned int ppc_spinlock, spinlock_cnt;
#endif

void uae_ppc_spinlock_get(void)
{
#ifdef FSUAE
	g_mutex_lock(&mutex);
#else
	int sp = spinlock_cnt;
	if (sp != 0 && sp != 1)
		write_log(_T("uae_ppc_spinlock_get invalid %d\n"),  sp);

	while (InterlockedExchange (&ppc_spinlock, 1));
	if (spinlock_cnt)
		write_log(_T("uae_ppc_spinlock_get %d!\n"), spinlock_cnt);
	spinlock_cnt = 1;
#endif
}
void uae_ppc_spinlock_release(void)
{
#ifdef FSUAE
	g_mutex_unlock(&mutex);
#else
	if (--spinlock_cnt)
		write_log(_T("uae_ppc_spinlock_release %d!\n"), spinlock_cnt);
	InterlockedExchange(&ppc_spinlock, 0);
#endif
}
void uae_ppc_spinlock_reset(void)
{
#ifdef FSUAE
#else
	spinlock_cnt = 0;
#endif
	uae_ppc_spinlock_get();
}

volatile int ppc_state;
static volatile bool ppc_thread_running;
int ppc_cycle_count;
static volatile bool ppc_access;
static volatile int ppc_cpu_lock_state;
static bool ppc_main_thread;
static bool ppc_io_pipe;
static bool ppc_use_spinlock;
static bool ppc_init_done;
static bool ppc_cpu_init_done;
static int ppc_implementation;

#define CSPPC_PVR 0x00090204
#define BLIZZPPC_PVR 0x00070101

#define KB * 1024
#define MB * (1024 * 1024)

/* Dummy PPC implementation */

static bool PPCCALL dummy_ppc_cpu_init(uint32_t pvr) { return false; }
static bool PPCCALL dummy_ppc_cpu_init_with_model(const char *model) { return false; }
static void PPCCALL dummy_ppc_cpu_free(void) { }
static void PPCCALL dummy_ppc_cpu_stop(void) { }
static void PPCCALL dummy_ppc_cpu_atomic_raise_ext_exception(void) { }
static void PPCCALL dummy_ppc_cpu_atomic_cancel_ext_exception(void) { }
static void PPCCALL dummy_ppc_cpu_map_memory(PPCMemoryRegion *regions, int count) { }
static void PPCCALL dummy_ppc_cpu_set_pc(int cpu, uint32_t value) { }
static void PPCCALL dummy_ppc_cpu_run_continuous(void) { }
static void PPCCALL dummy_ppc_cpu_run_single(int count) { }
static uint64_t PPCCALL dummy_ppc_cpu_get_dec(void) { return 0; }
static void PPCCALL dummy_ppc_cpu_do_dec(int value) { }

static void PPCCALL dummy_ppc_cpu_pause(int pause)
{
    UAE_LOG_STUB("pause=%d\n", pause);
}

/* Functions typedefs for PPC implementation */

typedef bool (PPCCALL *ppc_cpu_init_function)(uint32_t pvr);
typedef bool (PPCCALL *ppc_cpu_init_with_model_function)(const char *model);
typedef void (PPCCALL *ppc_cpu_free_function)(void);
typedef void (PPCCALL *ppc_cpu_stop_function)(void);
typedef void (PPCCALL *ppc_cpu_atomic_raise_ext_exception_function)(void);
typedef void (PPCCALL *ppc_cpu_atomic_cancel_ext_exception_function)(void);
typedef void (PPCCALL *ppc_cpu_map_memory_function)(PPCMemoryRegion *regions, int count);
typedef void (PPCCALL *ppc_cpu_set_pc_function)(int cpu, uint32_t value);
typedef void (PPCCALL *ppc_cpu_run_continuous_function)(void);
typedef void (PPCCALL *ppc_cpu_run_single_function)(int count);
typedef uint64_t (PPCCALL *ppc_cpu_get_dec_function)(void);
typedef void (PPCCALL *ppc_cpu_do_dec_function)(int value);
typedef void (PPCCALL *ppc_cpu_pause_function)(int pause);

/* Function pointers to active PPC implementation */

static ppc_cpu_init_function g_ppc_cpu_init;
static ppc_cpu_init_with_model_function g_ppc_cpu_init_with_model;
static ppc_cpu_free_function g_ppc_cpu_free;
static ppc_cpu_stop_function g_ppc_cpu_stop;
static ppc_cpu_atomic_raise_ext_exception_function g_ppc_cpu_atomic_raise_ext_exception;
static ppc_cpu_atomic_cancel_ext_exception_function g_ppc_cpu_atomic_cancel_ext_exception;
static ppc_cpu_map_memory_function g_ppc_cpu_map_memory;
static ppc_cpu_set_pc_function g_ppc_cpu_set_pc;
static ppc_cpu_run_continuous_function g_ppc_cpu_run_continuous;
static ppc_cpu_run_single_function g_ppc_cpu_run_single;
static ppc_cpu_get_dec_function g_ppc_cpu_get_dec;
static ppc_cpu_do_dec_function g_ppc_cpu_do_dec;
static ppc_cpu_pause_function g_ppc_cpu_pause;

static void load_dummy_implementation()
{
	write_log(_T("PPC: Loading dummy implementation\n"));
	g_ppc_cpu_init = dummy_ppc_cpu_init;
	g_ppc_cpu_init_with_model = dummy_ppc_cpu_init_with_model;
	g_ppc_cpu_free = dummy_ppc_cpu_free;
	g_ppc_cpu_stop = dummy_ppc_cpu_stop;
	g_ppc_cpu_atomic_raise_ext_exception = dummy_ppc_cpu_atomic_raise_ext_exception;
	g_ppc_cpu_atomic_cancel_ext_exception = dummy_ppc_cpu_atomic_cancel_ext_exception;
	g_ppc_cpu_map_memory = dummy_ppc_cpu_map_memory;
	g_ppc_cpu_set_pc = dummy_ppc_cpu_set_pc;
	g_ppc_cpu_run_continuous = dummy_ppc_cpu_run_continuous;
	g_ppc_cpu_run_single = dummy_ppc_cpu_run_single;
	g_ppc_cpu_get_dec = dummy_ppc_cpu_get_dec;
	g_ppc_cpu_do_dec = dummy_ppc_cpu_do_dec;
	g_ppc_cpu_pause = dummy_ppc_cpu_pause;
}

#ifdef WITH_QEMU_CPU

static void uae_patch_library_ppc(UAE_DLHANDLE handle)
{
	void *ptr;

	ptr = uae_dlsym(handle, "uae_ppc_io_mem_read");
	if (ptr) *((uae_ppc_io_mem_read_function *) ptr) = &uae_ppc_io_mem_read;
	else write_log(_T("WARNING: uae_ppc_io_mem_read not set\n"));

	ptr = uae_dlsym(handle, "uae_ppc_io_mem_write");
	if (ptr) *((uae_ppc_io_mem_write_function *) ptr) = &uae_ppc_io_mem_write;
	else write_log(_T("WARNING: uae_ppc_io_mem_write not set\n"));

	ptr = uae_dlsym(handle, "uae_ppc_io_mem_read64");
	if (ptr) *((uae_ppc_io_mem_read64_function *) ptr) = &uae_ppc_io_mem_read64;
	else write_log(_T("WARNING: uae_ppc_io_mem_read64 not set\n"));

	ptr = uae_dlsym(handle, "uae_ppc_io_mem_write64");
	if (ptr) *((uae_ppc_io_mem_write64_function *) ptr) = &uae_ppc_io_mem_write64;
	else write_log(_T("WARNING: uae_ppc_io_mem_write64 not set\n"));
}

static bool load_qemu_implementation()
{
	write_log(_T("PPC: Loading QEmu implementation\n"));

	UAE_DLHANDLE handle = uae_dlopen_plugin(_T("qemu-uae"));
	if (!handle) {
		gui_message(_T("PPC: Error loading qemu-uae plugin\n"));
		return false;
	}
	write_log(_T("PPC: Loaded qemu-uae library at %p\n"), handle);

	/* get function pointers */

	g_ppc_cpu_init = (ppc_cpu_init_function) uae_dlsym(handle, "ppc_cpu_init");
	g_ppc_cpu_init_with_model = (ppc_cpu_init_with_model_function) uae_dlsym(handle, "ppc_cpu_init_with_model");
	g_ppc_cpu_free = (ppc_cpu_free_function) uae_dlsym(handle, "ppc_cpu_free");
	g_ppc_cpu_stop = (ppc_cpu_stop_function) uae_dlsym(handle, "ppc_cpu_stop");
	g_ppc_cpu_atomic_raise_ext_exception = (ppc_cpu_atomic_raise_ext_exception_function) uae_dlsym(handle, "ppc_cpu_atomic_raise_ext_exception");
	g_ppc_cpu_atomic_cancel_ext_exception = (ppc_cpu_atomic_cancel_ext_exception_function) uae_dlsym(handle, "ppc_cpu_atomic_cancel_ext_exception");
	g_ppc_cpu_map_memory = (ppc_cpu_map_memory_function) uae_dlsym(handle, "ppc_cpu_map_memory");
	g_ppc_cpu_set_pc = (ppc_cpu_set_pc_function) uae_dlsym(handle, "ppc_cpu_set_pc");
	g_ppc_cpu_run_continuous = (ppc_cpu_run_continuous_function) uae_dlsym(handle, "ppc_cpu_run_continuous");
	g_ppc_cpu_run_single = (ppc_cpu_run_single_function) uae_dlsym(handle, "ppc_cpu_run_single");
	g_ppc_cpu_get_dec = (ppc_cpu_get_dec_function) uae_dlsym(handle, "ppc_cpu_get_dec");
	g_ppc_cpu_do_dec = (ppc_cpu_do_dec_function) uae_dlsym(handle, "ppc_cpu_do_dec");
	g_ppc_cpu_pause = (ppc_cpu_pause_function) uae_dlsym(handle, "ppc_cpu_pause");

#if 0
	/* register callback functions */
#endif

        uae_dlopen_patch_common(handle);
        uae_patch_library_ppc(handle);
        return true;
}

#endif

#ifdef WITH_PEARPC_CPU

static bool load_pearpc_implementation()
{
	write_log(_T("PPC: Loading PearPC implementation\n"));
	g_ppc_cpu_init = ppc_cpu_init;
	g_ppc_cpu_init_with_model = NULL;
	g_ppc_cpu_free = ppc_cpu_free;
	g_ppc_cpu_stop = ppc_cpu_stop;
	g_ppc_cpu_atomic_raise_ext_exception = ppc_cpu_atomic_raise_ext_exception;
	g_ppc_cpu_atomic_cancel_ext_exception = ppc_cpu_atomic_cancel_ext_exception;

	g_ppc_cpu_map_memory = dummy_ppc_cpu_map_memory;

	g_ppc_cpu_set_pc = ppc_cpu_set_pc;
	g_ppc_cpu_run_continuous = ppc_cpu_run_continuous;
	g_ppc_cpu_run_single = ppc_cpu_run_single;
	g_ppc_cpu_get_dec = ppc_cpu_get_dec;
	g_ppc_cpu_do_dec = ppc_cpu_do_dec;

	g_ppc_cpu_pause = dummy_ppc_cpu_pause;

	return true;
}

#endif

static void load_ppc_implementation()
{
	int impl = currprefs.ppc_implementation;
#ifdef WITH_QEMU_CPU
	if (impl == PPC_IMPLEMENTATION_AUTO || impl == PPC_IMPLEMENTATION_QEMU) {
		if (load_qemu_implementation()) {
			ppc_implementation = PPC_IMPLEMENTATION_QEMU;
			return;
		}
	}
#endif
#ifdef WITH_PEARPC_CPU
	if (impl == PPC_IMPLEMENTATION_AUTO || impl == PPC_IMPLEMENTATION_PEARPC) {
		if (load_pearpc_implementation()) {
			ppc_implementation = PPC_IMPLEMENTATION_PEARPC;
			return;
		}
	}
#endif
	load_dummy_implementation();
	ppc_implementation = 0;
}

static void initialize()
{
	static bool initialized = false;
	if (initialized) {
		return;
	}
	initialized = true;
	load_ppc_implementation();
}

static void map_banks(void)
{
	/*
	 * Use NULL for memory to get callbacks for read/write. Use real
	 * memory address for direct access to RAM banks (looks like this
	 * is needed by JIT, or at least more work is needed on QEmu Side
	 * to allow all memory access to go via callbacks).
	 */

	PPCMemoryRegion regions[UAE_MEMORY_REGIONS_MAX];
	UaeMemoryMap map;
	uae_memory_map(&map);

	for (int i = 0; i < map.num_regions; i++) {
		UaeMemoryRegion *r = &map.regions[i];
		regions[i].start = r->start;
		regions[i].size = r->size;
		regions[i].name = ua(r->name);
		regions[i].alias = r->alias;
		regions[i].memory = r->memory;
	}
	g_ppc_cpu_map_memory(regions, map.num_regions);
	for (int i = 0; i < map.num_regions; i++) {
		free(regions[i].name);
	}
}

static void cpu_init()
{
	const TCHAR *model;

	/* Setting default CPU model based on accelerator board */
	if (currprefs.cpuboard_type == BOARD_BLIZZARDPPC) {
		model = _T("603ev");
	} else {
		model = _T("604e");
	}

        /* Override PPC CPU model. See qemu/target-ppc/cpu-models.c for
         * a list of valid CPU model identifiers */
#if 0
	// FIXME: if ppc_model is overridden in currprefs, point to option
	if (currprefs.ppc_model[0]) {
		model = currprefs.ppc_model
	}
#endif

	if (g_ppc_cpu_init_with_model) {
		char *models = ua(model);
		g_ppc_cpu_init_with_model(models);
		free(models);
	} else {
		uint32_t pvr = 0;
		if (_tcsicmp(model, _T("603ev")) == 0) {
			pvr = BLIZZPPC_PVR;
		}
		else if (_tcsicmp(model, _T("604e")) == 0) {
			pvr = CSPPC_PVR;
		}
		else {
			pvr = CSPPC_PVR;
			write_log(_T("PPC: Unrecognized model \"%s\", using PVR 0x%08x\n"), model, pvr);
		}
		write_log(_T("PPC: Calling ppc_cpu_init with PVR 0x%08x\n"), pvr);
		g_ppc_cpu_init(pvr);
	}

	/* Map memory and I/O banks (for QEmu PPC implementation) */
	map_banks();
}

static void uae_ppc_cpu_reset(void)
{
	TRACE(_T("uae_ppc_cpu_reset\n"));
	initialize();
	if (!ppc_cpu_init_done) {
		write_log(_T("PPC: Hard reset\n"));
		cpu_init();
		ppc_cpu_init_done = true;
	}
	write_log(_T("PPC: Init\n"));
	g_ppc_cpu_set_pc(0, 0xfff00100);
	ppc_cycle_count = 2000;
	ppc_state = PPC_STATE_ACTIVE;
	ppc_cpu_lock_state = 0;
}

static void *ppc_thread(void *v)
{
	uae_ppc_cpu_reset();
	g_ppc_cpu_run_continuous();
	if (ppc_state == PPC_STATE_ACTIVE || ppc_state == PPC_STATE_SLEEP)
		ppc_state = PPC_STATE_STOP;
	write_log(_T("ppc_cpu_run() exited.\n"));
	ppc_thread_running = false;
	return NULL;
}

void uae_ppc_execute_quick(int linetype)
{
	if (linetype == 0) {
		uae_ppc_spinlock_release();
		read_processor_time(); // tiny delay..
		read_processor_time();
		uae_ppc_spinlock_get();
	} else {
		uae_ppc_spinlock_release();
		sleep_millis(1);
		uae_ppc_spinlock_get();
	}
}

void uae_ppc_emulate(void)
{
	if (ppc_implementation == PPC_IMPLEMENTATION_QEMU)
		return;

	ppc_interrupt(intlev());

	//TRACE(_T("uae_ppc_emulate\n"));
	if (ppc_state == PPC_STATE_ACTIVE || ppc_state == PPC_STATE_SLEEP)
		g_ppc_cpu_run_single(10);
}

bool uae_ppc_direct_physical_memory_handle(uint32_t addr, uint8_t *&ptr)
{
	ptr = get_real_address(addr);
	if (!ptr)
		gui_message(_T("Executing PPC code at IO address %08x!"), addr);
	return true;
}

STATIC_INLINE bool spinlock_pre(uaecptr addr)
{
	if (ppc_use_spinlock) {
		addrbank *ab = &get_mem_bank(addr);
		if ((ab->flags & ABFLAG_THREADSAFE) == 0) {
			uae_ppc_spinlock_get();
			return true;
		}
	}
	return false;
}

STATIC_INLINE void spinlock_post(bool locked)
{
	if (ppc_use_spinlock && locked)
		uae_ppc_spinlock_release();
}

bool UAECALL uae_ppc_io_mem_write(uint32_t addr, uint32_t data, int size)
{
	bool locked = false;

	while (ppc_thread_running && ppc_cpu_lock_state < 0 && ppc_state);

#if PPC_ACCESS_LOG > 0
	if (!ppc_io_pipe && !valid_address(addr, size)) {
		if (addr >= PPC_DEBUG_ADDR_FROM && addr < PPC_DEBUG_ADDR_TO)
			write_log(_T("PPC io write %08x = %08x %d\n"), addr, data, size);
	}
#endif

	locked = spinlock_pre(addr);
	switch (size)
	{
	case 4:
		put_long(addr, data);
		break;
	case 2:
		put_word(addr, data);
		break;
	case 1:
		put_byte(addr, data);
		break;
	}
	if (ppc_use_spinlock) {
		if (addr == 0xdff09c || addr == 0xdff09a) {
			int lev = intlev();
			ppc_interrupt(lev);
		}
		spinlock_post(locked);
	}

#if PPC_ACCESS_LOG > 2
	write_log(_T("PPC mem write %08x = %08x %d\n"), addr, data, size);
#endif

	return true;
}

bool UAECALL uae_ppc_io_mem_read(uint32_t addr, uint32_t *data, int size)
{
	uint32_t v;
	bool locked = false;

	while (ppc_thread_running && ppc_cpu_lock_state < 0 && ppc_state);

	if (addr >= 0xdff000 && addr < 0xe00000) {
		// shortcuts for common registers
		if (addr == 0xdff01c) { // INTENAR
			*data = intena;
			return true;
		}
		if (addr == 0xdff01e) { // INTREQR
			*data = intreq;
			return true;
		}
	}

	locked = spinlock_pre(addr);
	switch (size)
	{
	case 4:
		v = get_long(addr);
		break;
	case 2:
		v = get_word(addr);
		break;
	case 1:
		v = get_byte(addr);
		break;
	}
	*data = v;
	spinlock_post(locked);

#if PPC_ACCESS_LOG > 0
	if (!ppc_io_pipe && !valid_address(addr, size)) {
		if (addr >= PPC_DEBUG_ADDR_FROM && addr < PPC_DEBUG_ADDR_TO && addr != 0xdff006)
			write_log(_T("PPC io read %08x=%08x %d\n"), addr, v, size);
	}
#endif
#if PPC_ACCESS_LOG > 2
	write_log(_T("PPC mem read %08x=%08x %d\n"), addr, v, size);
#endif
	return true;
}

bool UAECALL uae_ppc_io_mem_write64(uint32_t addr, uint64_t data)
{
	bool locked = false;
	while (ppc_thread_running && ppc_cpu_lock_state < 0 && ppc_state);

	locked = spinlock_pre(addr);
	put_long(addr + 0, data >> 32);
	put_long(addr + 4, data & 0xffffffff);
	spinlock_post(locked);

#if PPC_ACCESS_LOG > 2
	write_log(_T("PPC mem write64 %08x = %08llx\n"), addr, data);
#endif

	return true;
}

bool UAECALL uae_ppc_io_mem_read64(uint32_t addr, uint64_t *data)
{
	bool locked = false;
	uint32_t v1, v2;

	while (ppc_thread_running && ppc_cpu_lock_state < 0 && ppc_state);

	locked = spinlock_pre(addr);
	v1 = get_long(addr + 0);
	v2 = get_long(addr + 4);
	*data = ((uint64_t)v1 << 32) | v2;
	spinlock_post(locked);

#if PPC_ACCESS_LOG > 2
	write_log(_T("PPC mem read64 %08x = %08llx\n"), addr, *data);
#endif

	return true;
}

void uae_ppc_cpu_stop(void)
{
	TRACE(_T("uae_ppc_cpu_stop\n"));
	if (ppc_thread_running && ppc_state) {
		write_log(_T("Stopping PPC.\n"));
		uae_ppc_wakeup();
		g_ppc_cpu_stop();
		while (ppc_state != PPC_STATE_STOP && ppc_state != PPC_STATE_CRASH) {
			uae_ppc_wakeup();
			if (ppc_use_spinlock) {
				uae_ppc_spinlock_release();
				uae_ppc_spinlock_get();
			}
		}
		ppc_state = PPC_STATE_STOP;
		write_log(_T("PPC stopped.\n"));
	}
}

void uae_ppc_cpu_reboot(void)
{
	TRACE(_T("uae_ppc_cpu_reboot\n"));

	uae_ppc_spinlock_reset();
	ppc_io_pipe = false;
	ppc_use_spinlock = true;

	if (ppc_main_thread) {
		uae_ppc_cpu_reset();
	} else {
		write_log(_T("Starting PPC thread.\n"));
		if (!ppc_thread_running) {
			ppc_thread_running = true;
			ppc_main_thread = false;
			uae_start_thread(_T("ppc"), ppc_thread, NULL, NULL);
		}
	}
}

void uae_ppc_reset(bool hardreset)
{
	TRACE(_T("uae_ppc_reset hardreset=%d\n"), hardreset);
	uae_ppc_cpu_stop();
	ppc_main_thread = false;
	if (hardreset) {
		if (ppc_init_done)
			g_ppc_cpu_free();
		ppc_init_done = false;
	}
}

void uae_ppc_cpu_lock(void)
{
	// when called, lock was already set by other CPU
	if (ppc_access) {
		// ppc accessing but m68k already locked
		ppc_cpu_lock_state = -1;
	} else {
		// m68k accessing but ppc already locked
		ppc_cpu_lock_state = 1;
	}
}

bool uae_ppc_cpu_unlock(void)
{
	if (!ppc_cpu_lock_state)
		return true;
	ppc_cpu_lock_state = 0;
	return false;
}

void uae_ppc_wakeup(void)
{
	//TRACE(_T("uae_ppc_wakeup\n"));
	if (ppc_state == PPC_STATE_SLEEP)
		ppc_state = PPC_STATE_ACTIVE;
}

void uae_ppc_interrupt(bool active)
{
	//TRACE(_T("uae_ppc_interrupt\n"));
	if (active) {
		g_ppc_cpu_atomic_raise_ext_exception();
		uae_ppc_wakeup();
	} else {
		g_ppc_cpu_atomic_cancel_ext_exception();
	}
}

// sleep until interrupt (or PPC stopped)
void uae_ppc_doze(void)
{
	//TRACE(_T("uae_ppc_doze\n"));
	if (!ppc_thread_running)
		return;
	ppc_state = PPC_STATE_SLEEP;
	while (ppc_state == PPC_STATE_SLEEP) {
		sleep_millis(2);
	}
}

void uae_ppc_crash(void)
{
	TRACE(_T("uae_ppc_crash\n"));
	ppc_state = PPC_STATE_CRASH;
	g_ppc_cpu_stop();
}

void uae_ppc_hsync_handler(void)
{
	if (ppc_state != PPC_STATE_SLEEP)
		return;
	if (g_ppc_cpu_get_dec() == 0) {
		uae_ppc_wakeup();
	} else {
		g_ppc_cpu_do_dec(ppc_cycle_count);
	}
}

void uae_ppc_pause(int pause)
{
	if (g_ppc_cpu_pause) {
		g_ppc_cpu_pause(pause);
	}
}

#ifdef FSUAE // NL

UAE_EXTERN_C void fsuae_ppc_pause(int pause);
UAE_EXTERN_C void fsuae_ppc_pause(int pause)
{
	uae_ppc_pause(pause);
}

#endif
