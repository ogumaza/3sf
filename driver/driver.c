// SPDX-License-Identifier: MIT

/*
 * 3SF guest driver for games built on Game Freak's gfl::snd over NintendoWare nw::snd
 * (Pokemon X/Y). It's compiled for the ARM11 (ARMv6K, VFPv2) and stored in every .3sflib;
 * the player boots the game's startup code, and the rip patches nnMain to call this driver.
 *
 * The driver then does what the game's boot would do for sound, using the game's
 * functions (addresses come from the profile in `driver_params`, filled by the ripper):
 *   1. stand-in gfl heaps over the memory the startup code already allocated;
 *   2. nn::fs::MountRom("rom:");
 *   3. the game's sound manager init (nn::snd, nw::snd and the DSP firmware);
 *   4. the console's sound output mode;
 *   5. a sound heap, LoadData(sound), Play(sound);
 *   6. the game's per-frame sound update, forever (about 59.83 times per emulated second).
 * `driver_params.status` tells the player what's happening (see docs/3sf.md).
 *
 * Build with driver/build.py (clang and ld.lld); the result is committed as
 * src/rip/driver_blob.h so building 3SF itself needs no ARM toolchain.
 */

typedef unsigned int u32;
typedef int s32;

#define PARAMS_MAGIC 0x44465333u /* '3SFD' */
#define PARAMS_VERSION 1u

/* Values of Params.status. They're macros because an enumerator can't hold 0x80000000 before C23. */
#define STATUS_BOOTING 0u
#define STATUS_PLAYING 1u
#define STATUS_FINISHED 2u
#define STATUS_ERROR 0x80000000u /* | step number */

struct Params {
    u32 magic;
    u32 version;
    u32 status;          /* written by the driver */
    u32 sound_id;        /* nw::snd item id (sound) to play; patched by each .mini3sf */
    u32 output_mode;     /* 0 mono, 1 stereo, 2 surround */
    u32 handle;          /* gfl sound handle slot */
    u32 data_heap_size;  /* device memory for the sound's data */
    u32 frame_ns;        /* period of the per-frame update */
    /* game profile */
    u32 fs_init;         /* void fs_init(void) */
    u32 rom_size;        /* u32 rom_size(u32 max_files, u32 max_dirs, u32 use_cache) */
    u32 rom_mount;       /* s32 rom_mount(u32 max_files, u32 max_dirs, void* buf, u32 size, u32 use_cache) */
    u32 sound_create;    /* void* sound_create(void* obj, gfl heap, gfl device heap) */
    u32 set_output;      /* void set_output(gfl sound system, u32 mode) */
    u32 heap_create;     /* void* heap_create(void* obj, u32, gfl heap, u32 size) */
    u32 load_data;       /* u32 load_data(sound system, heap obj, u32 id, u32 flags) */
    u32 play;            /* u32 play(sound system, u32 handle, u32 id, u32 fade, void* start_info) */
    u32 update;          /* void update(sound system) */
    u32 heap_manager;    /* gfl heap manager (table at +0xc8, count at +0xcc) */
    u32 system_offset;   /* sound system = ((u32*)obj)[system_offset / 4] */
    u32 handles_offset;  /* handle array = *(u32*)(sound system + handles_offset) */
    u32 heap_base, heap_size;     /* ordinary memory for the stand-in heap */
    u32 device_base, device_size; /* device (linear) memory for the stand-in device heap */
};

__attribute__((section(".params"), used)) volatile struct Params driver_params = {
    .magic = PARAMS_MAGIC,
    .version = PARAMS_VERSION,
};

/* Supervisor calls into the emulated Horizon kernel. */

static void svc_sleep(u32 ns) {
    register u32 r0 __asm__("r0") = ns;
    register u32 r1 __asm__("r1") = 0;
    __asm__ volatile("svc 0x0a" : "+r"(r0), "+r"(r1) : : "r2", "r3", "r12", "lr", "memory");
}

static void svc_debug(const char* s) {
    u32 n = 0;
    while (s[n])
        n++;
    register const char* r0 __asm__("r0") = s;
    register u32 r1 __asm__("r1") = n;
    __asm__ volatile("svc 0x3d" : "+r"(r0), "+r"(r1) : : "r2", "r3", "r12", "lr", "memory");
}

static void fail(u32 step) {
    driver_params.status = STATUS_ERROR | step;
    svc_debug("3SF driver: failed");
    for (;;)
        svc_sleep(1000000000u);
}

/* Stand-in gfl heaps. gfl's allocator (and gfl::snd) call these virtual functions:
 *   +0x18 debug hook, +0x1c default alignment, +0x20 heap id, +0x48 debug hook,
 *   +0x58 allocate(size, alignment), +0x64 debug hook. Memory is never freed. */

struct FakeHeap {
    void* const* vtable;
    u32 id;
    u32 next;
    u32 end;
};

static void heap_nop(struct FakeHeap* h) {
    (void)h;
}

static u32 heap_default_align(struct FakeHeap* h) {
    (void)h;
    return 4;
}

static u32 heap_id(struct FakeHeap* h) {
    return h->id;
}

static void* heap_alloc(struct FakeHeap* h, u32 size, s32 align) {
    u32 a = (u32)(align < 0 ? -align : align);
    if (a < 4)
        a = 4;
    const u32 p = (h->next + a - 1) & ~(a - 1);
    if (p + size > h->end || p + size < p)
        return 0;
    h->next = p + size;
    for (u32* q = (u32*)p; q < (u32*)(p + ((size + 3) & ~3u)); q++)
        *q = 0;
    return (void*)p;
}

static u32 heap_unexpected(struct FakeHeap* h) {
    (void)h;
    svc_debug("3SF driver: unexpected gfl heap call");
    return 0;
}

#define V(f) ((void*)(f))
static void* const heap_vtable[32] = {
    V(heap_unexpected), V(heap_unexpected), V(heap_unexpected), V(heap_unexpected),
    V(heap_unexpected), V(heap_unexpected), V(heap_nop),        V(heap_default_align),
    V(heap_id),         V(heap_unexpected), V(heap_unexpected), V(heap_unexpected),
    V(heap_unexpected), V(heap_unexpected), V(heap_unexpected), V(heap_unexpected),
    V(heap_unexpected), V(heap_unexpected), V(heap_nop),        V(heap_unexpected),
    V(heap_unexpected), V(heap_unexpected), V(heap_alloc),      V(heap_unexpected),
    V(heap_unexpected), V(heap_nop),        V(heap_unexpected), V(heap_unexpected),
    V(heap_unexpected), V(heap_unexpected), V(heap_unexpected), V(heap_unexpected),
};

static struct FakeHeap heap_normal, heap_device;
static u32 heap_table[16][2];

typedef u32 (*fn0)(void);
typedef u32 (*fn1)(u32);
typedef u32 (*fn2)(u32, u32);
typedef u32 (*fn3)(u32, u32, u32);
typedef u32 (*fn4)(u32, u32, u32, u32);
typedef u32 (*fn5)(u32, u32, u32, u32, u32);

__attribute__((noreturn)) void driver_main(void) {
    volatile struct Params* p = &driver_params;
    if (p->magic != PARAMS_MAGIC || p->version != PARAMS_VERSION)
        fail(1);

    heap_normal.vtable = heap_vtable;
    heap_normal.id = 0;
    heap_normal.next = p->heap_base;
    heap_normal.end = p->heap_base + p->heap_size;
    heap_device.vtable = heap_vtable;
    heap_device.id = 1;
    heap_device.next = p->device_base;
    heap_device.end = p->device_base + p->device_size;
    for (u32 i = 0; i < 16; i++) {
        heap_table[i][0] = (u32)(i == 1 ? &heap_device : &heap_normal);
        heap_table[i][1] = 1;
    }
    *(volatile u32*)(p->heap_manager + 0xc8) = (u32)heap_table;
    *(volatile u32*)(p->heap_manager + 0xcc) = 16;

    /* nn::fs::MountRom("rom:", 0x80 files, 0x10 directories, cached metadata) */
    ((fn0)p->fs_init)();
    const u32 rom_size = ((fn3)p->rom_size)(0x80, 0x10, 1);
    const u32 rom_buf = (u32)heap_alloc(&heap_normal, rom_size, 4);
    if (!rom_buf || (s32)((fn5)p->rom_mount)(0x80, 0x10, rom_buf, rom_size, 1) < 0)
        fail(2);

    /* The game's sound manager (nn::snd, nw::snd, DSP component). */
    const u32 obj = (u32)heap_alloc(&heap_normal, 0x40, 8);
    if (!obj)
        fail(3);
    ((fn3)p->sound_create)(obj, (u32)&heap_normal, (u32)&heap_device);
    const u32 ss = *(volatile u32*)(obj + p->system_offset);
    if (!ss)
        fail(4);
    ((fn2)p->set_output)(ss, p->output_mode);

    /* A sound heap for the sound's data, then load and play. */
    const u32 sound_heap = (u32)heap_alloc(&heap_normal, 0x40, 8);
    if (!sound_heap)
        fail(5);
    ((fn4)p->heap_create)(sound_heap, 0, (u32)&heap_device, p->data_heap_size);
    if (!((fn4)p->load_data)(ss, sound_heap, p->sound_id, 0xffffffffu))
        fail(6);
    if (!((fn5)p->play)(ss, p->handle, p->sound_id, 0, 0))
        fail(7);
    p->status = STATUS_PLAYING;

    for (;;) {
        svc_sleep(p->frame_ns);
        ((fn1)p->update)(ss);
        const u32 handles = *(volatile u32*)(ss + p->handles_offset);
        if (p->status == STATUS_PLAYING && *(volatile u32*)(handles + 4 * p->handle) == 0)
            p->status = STATUS_FINISHED;
    }
}

__attribute__((naked, section(".text.entry"))) void driver_entry(void) {
    __asm__ volatile("b driver_main");
}
