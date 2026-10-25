/*
 * uefi_jump.c - place the UEFI FD at 0xA0000000 and optionally jump into it.
 *
 * init:
 *   1. reads the FD from fd_path (default /data/local/tmp/uefi.fd),
 *   2. best-effort reserves FD_BASE..FD_BASE+FD_SIZE in memblock,
 *   3. memremap()s the physical window and copies the FD into it,
 *   4. flushes the range to the point of coherency,
 *   5. compares CRCs of source and destination,
 *   6. if jump=1, stops secondary CPUs and calls camellia_jump_to(FD_BASE),
 *      which is the exported kernel helper around cpu_soft_restart().
 *
 * jump defaults to 0: the module copies and verifies but stays in Linux.
 *
 * Note: memblock_reserve() after boot does not pull pages back out of the
 * buddy allocator, so the window can still be reused by the kernel. For a
 * copy immediately followed by the jump the race window is tiny; a hard
 * reservation (kernel patch or DT reserved memory) is required before this
 * is used as a long-lived handoff buffer.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <asm/io.h>
#include <linux/crc32.h>
#include <linux/irqflags.h>
#include <linux/vmalloc.h>
#include <linux/platform_device.h>
#include <linux/iommu.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("jiangshihao");
MODULE_DESCRIPTION("UEFI jump - stage 2: place FD at 0xA0000000 and maybe jump");
MODULE_VERSION("0.4-stable-handoff");

#define FD_BASE		0xA0000000UL
#define FD_SIZE		0x200000UL
#define FV_SIG_OFFSET	0x28

#define DISP_OVL0_BASE		0x14005000UL
#define DISP_OVL0_2L_BASE	0x14006000UL
#define DISP_RDMA0_BASE		0x14007000UL
#define DISP_REG_OVL_ADDR	0x0f40
#define DISP_REG_RDMA_MEM_START	0x0f00
#define DISP_REG_OVL_SRC_CON	0x002c
#define DISP_REG_OVL_L0_SRC_SIZE 0x0038
#define DISP_REG_OVL_L0_PITCH	0x0044
#define DISP_REG_OVL_L0_PITCH_MSB 0x0040

#define FB_HANDOFF_ADDR		0xA4000000UL
#define FB_PAGE_MAP_ADDR	0xA4000100UL
#define FB_PAGE_MAP_SIZE	0x680UL
#define FB_PAGE_MAP_MAX_RUNS	100
#define FB_PAGE_MAP_MAGIC	0x314D504649465545ULL /* "UEFIFPM1" LE */
#define PAYLOAD_BASE		0x98000000UL
#define PAYLOAD_MAX		0x4000000UL
#define PAYLOAD_INFO_ADDR	0xA4000780UL
#define PAYLOAD_MAGIC		0x314D415249464555ULL /* "UEFIRAM1" LE */
#define CONTIG_FB_BASE		0x9C000000UL
#define CONTIG_FB_SIZE		0x01000000UL
#define CONTIG_FB_IOVA		0x80000000UL
#define FB_BEACON_ADDR		0xA4000800UL
#define FB_HANDOFF_MAGIC	0x3130424649464555ULL /* "UEFIFB01" LE */
#define JUMP_TRACE_BASE		0x48081800UL
#define JUMP_TRACE_SIZE		0x1000UL
#define JUMP_TRACE_MAGIC	0x3145434152544a55ULL /* "UJTRACE1" LE */

struct fb_handoff {
	__le64 magic;
	__le64 base;
	__le32 width;
	__le32 height;
	__le32 pitch;
	__le32 depth;
};

struct fb_page_map_run {
	__le64 physical;
	__le32 pages;
	__le32 reserved;
};

struct fb_page_map {
	__le64 magic;
	__le64 logical_size;
	__le32 page_size;
	__le32 run_count;
	struct fb_page_map_run runs[FB_PAGE_MAP_MAX_RUNS];
};

struct payload_handoff {
	__le64 magic;
	__le64 base;
	__le64 size;
	__le32 block_size;
	__le32 reserved;
};

struct jump_trace_record {
	__le64 magic;
	__le64 magic_inverse;
	__le64 stage;
	__le64 stage_inverse;
	__le64 detail;
	__le64 counter;
	__le64 reserved[2];
};

typedef int (*memblock_reserve_t)(phys_addr_t base, phys_addr_t size);
typedef void (*flush_dcache_t)(void *addr, size_t size);
typedef void (*smp_send_stop_t)(void);

static __nocfi void do_flush_dcache(flush_dcache_t fn, void *addr,
				    size_t size)
{
	fn(addr, size);
}

static void write_jump_trace(void *trace, unsigned int index,
			     u64 stage, u64 detail,
			     flush_dcache_t flush_dcache)
{
	struct jump_trace_record *record;
	u64 counter;

	if (!trace || index >= JUMP_TRACE_SIZE / sizeof(*record))
		return;
	record = (struct jump_trace_record *)trace + index;
	memset(record, 0, sizeof(*record));
	record->magic = cpu_to_le64(JUMP_TRACE_MAGIC);
	record->magic_inverse = cpu_to_le64(~JUMP_TRACE_MAGIC);
	record->stage = cpu_to_le64(stage);
	record->stage_inverse = cpu_to_le64(~stage);
	record->detail = cpu_to_le64(detail);
	asm volatile("isb; mrs %0, cntpct_el0" : "=r"(counter));
	record->counter = cpu_to_le64(counter);
	asm volatile("dsb sy; isb" : : : "memory");
	if (flush_dcache)
		do_flush_dcache(flush_dcache, record, sizeof(*record));
}

static char *fd_path = "/data/local/tmp/uefi.fd";
module_param(fd_path, charp, 0444);
MODULE_PARM_DESC(fd_path, "Path to the UEFI FD (must be exactly 2 MiB)");

static char *payload_path = "";
module_param(payload_path, charp, 0444);
MODULE_PARM_DESC(payload_path, "Optional RAM-disk payload image");

static uint payload_bytes;
module_param(payload_bytes, uint, 0444);
MODULE_PARM_DESC(payload_bytes,
		 "Read this many bytes when payload_path is a block device");

static bool jump;
module_param(jump, bool, 0644);
MODULE_PARM_DESC(jump, "Actually jump into the UEFI FD (DANGEROUS)");

static bool copy_payload_only;
module_param(copy_payload_only, bool, 0644);
MODULE_PARM_DESC(copy_payload_only,
		 "Copy the RAM payload and write its handoff without jumping");

static bool read_beacon;
module_param(read_beacon, bool, 0444);
MODULE_PARM_DESC(read_beacon, "Only read the UEFI progress beacon and exit");

static bool test_beacon;
module_param(test_beacon, bool, 0644);
MODULE_PARM_DESC(test_beacon, "Write a known value to the beacon page and exit");

static bool hb;
module_param(hb, bool, 0644);
MODULE_PARM_DESC(hb, "Enable the UEFI handoff beacon (5s wait + watchdog reset)");

static bool fb_paint;
module_param(fb_paint, bool, 0644);
MODULE_PARM_DESC(fb_paint, "Paint the top band of the live frame buffer and exit");

static bool fb_bands;
module_param(fb_bands, bool, 0644);
MODULE_PARM_DESC(fb_bands, "Paint four full-width test bands and exit");

static bool mapcheck;
module_param(mapcheck, bool, 0644);
MODULE_PARM_DESC(mapcheck, "Check whether OVL IOVA maps to contiguous physical pages");

static bool mapdump;
module_param(mapdump, bool, 0644);
MODULE_PARM_DESC(mapdump, "Write and dump the compressed framebuffer run map");

static bool contig_fb;
module_param(contig_fb, bool, 0644);
MODULE_PARM_DESC(contig_fb, "Remap the active OVL to a contiguous physical framebuffer");

static uint contig_stage;
module_param(contig_stage, uint, 0444);
MODULE_PARM_DESC(contig_stage, "Return after a contiguous-framebuffer diagnostic stage");

static bool fb_line;
module_param(fb_line, bool, 0644);
MODULE_PARM_DESC(fb_line, "Draw two vertical pitch test lines and exit");

static uint fb_pitch;
module_param(fb_pitch, uint, 0644);
MODULE_PARM_DESC(fb_pitch, "Override frame buffer pitch in bytes (0 = width*4)");

static bool regs;
module_param(regs, bool, 0444);
MODULE_PARM_DESC(regs, "Dump exception level and timer system registers");

static bool disp_test;
module_param(disp_test, bool, 0644);
MODULE_PARM_DESC(disp_test, "Paint red, stop CPUs, paint blue, wait, watchdog reset");

static inline u64 rd_currentel(void)
{
	u64 v;
	asm volatile("mrs %0, currentel" : "=r"(v));
	return v;
}

static inline u64 rd_sys(const char *name) { (void)name; return 0; }

#define RD_SYSREG(fn, reg) \
static inline u64 fn(void) { u64 v; asm volatile("mrs %0, " reg : "=r"(v)); return v; }

RD_SYSREG(rd_cntfrq, "cntfrq_el0")
RD_SYSREG(rd_cntpct, "cntpct_el0")
RD_SYSREG(rd_cntvct, "cntvct_el0")
RD_SYSREG(rd_cntp_ctl, "cntp_ctl_el0")
RD_SYSREG(rd_cntp_cval, "cntp_cval_el0")
RD_SYSREG(rd_cntv_ctl, "cntv_ctl_el0")
RD_SYSREG(rd_cntv_cval, "cntv_cval_el0")
RD_SYSREG(rd_cntkctl, "cntkctl_el1")
RD_SYSREG(rd_hcr, "hcr_el2")
RD_SYSREG(rd_cnthctl, "cnthctl_el2")

static void disable_arch_timers(void)
{
	u64 ctl;

	asm volatile("mrs %0, cntp_ctl_el0" : "=r"(ctl));
	ctl |= 2; /* IMASK */
	ctl &= ~1ULL; /* ENABLE */
	asm volatile("msr cntp_ctl_el0, %0" : : "r"(ctl));

	asm volatile("mrs %0, cntv_ctl_el0" : "=r"(ctl));
	ctl |= 2;
	ctl &= ~1ULL;
	asm volatile("msr cntv_ctl_el0, %0" : : "r"(ctl));
	asm volatile("dsb sy; isb" : : : "memory");
}

static void paint_top_band(u32 color, flush_dcache_t flush_dcache)
{
	void *ho = memremap(FB_HANDOFF_ADDR, 0x1000, MEMREMAP_WB);
	void *fb;
	u32 pitch, lines = 200, i;
	phys_addr_t base;

	if (!ho)
		return;
	{
		struct fb_handoff *s = ho;
		if (le64_to_cpu(s->magic) != FB_HANDOFF_MAGIC) {
			memunmap(ho);
			return;
		}
		base = le64_to_cpu(s->base);
		pitch = le32_to_cpu(s->pitch);
	}
	memunmap(ho);

	fb = memremap(base, (phys_addr_t)pitch * lines, MEMREMAP_WB);
	if (!fb)
		return;
	{
		u32 *px = fb;
		for (i = 0; i < pitch * lines / 4; i++)
			px[i] = color;
	}
	if (flush_dcache)
		do_flush_dcache(flush_dcache, fb, (size_t)pitch * lines);
	memunmap(fb);
}

static void paint_test_bands(flush_dcache_t flush_dcache)
{
	void *ho = memremap(FB_HANDOFF_ADDR, 0x1000, MEMREMAP_WB);
	struct fb_handoff *s;
	void *fb;
	phys_addr_t base;
	u64 total;
	u32 width, height, pitch, y;

	if (!ho)
		return;

	s = ho;
	if (le64_to_cpu(s->magic) != FB_HANDOFF_MAGIC) {
		pr_err("uefi_jump: invalid fb handoff, not painting bands\n");
		memunmap(ho);
		return;
	}

	base = le64_to_cpu(s->base);
	width = le32_to_cpu(s->width);
	height = le32_to_cpu(s->height);
	pitch = le32_to_cpu(s->pitch);
	memunmap(ho);

	if (!base || width == 0 || width > 4096 || height < 475 ||
	    height > 8192 || pitch < width * 4 || pitch > 65536) {
		pr_err("uefi_jump: invalid fb geometry %ux%u pitch %u at 0x%llx\n",
		       width, height, pitch, (unsigned long long)base);
		return;
	}

	total = (u64)pitch * height;
	fb = memremap(base, total, MEMREMAP_WB);
	if (!fb) {
		pr_err("uefi_jump: cannot map full fb 0x%llx size 0x%llx\n",
		       (unsigned long long)base, (unsigned long long)total);
		return;
	}

	for (y = 0; y < height; y++) {
		u32 *row = (u32 *)((u8 *)fb + (u64)y * pitch);
		u32 color;
		u32 x;

		if (y < height / 4)
			color = 0xFF00FF00;
		else if (y < height / 2)
			color = 0xFF0000FF;
		else if (y < (height * 3) / 4)
			color = 0xFFFFFFFF;
		else
			color = 0xFFFFFF00;

		for (x = 0; x < width; x++)
			row[x] = color;

		/* Flush each row so a later fault cannot hide earlier rows. */
		if (flush_dcache)
			do_flush_dcache(flush_dcache, row, (size_t)pitch);
	}

	pr_info("uefi_jump: painted fb bands %ux%u pitch=%u at 0x%llx size=0x%llx\n",
		width, height, pitch, (unsigned long long)base,
		(unsigned long long)total);
	memunmap(fb);
}

static u32 gProbeIova;
static const char *gProbeOvlName;
static int fb_map_error;
static int contig_diag_result;

static void check_fb_contiguity(void)
{
	void *ho = memremap(FB_HANDOFF_ADDR, 0x1000, MEMREMAP_WB);
	struct fb_handoff *s;
	struct device *dev;
	struct iommu_domain *domain;
	phys_addr_t prev_end = 0;
	u64 total, off;
	u32 pitch, height;
	u32 gaps = 0;

	if (!ho || !gProbeIova || !gProbeOvlName) {
		pr_err("uefi_jump: mapcheck has no OVL mapping\n");
		if (ho)
			memunmap(ho);
		return;
	}

	s = ho;
	if (le64_to_cpu(s->magic) != FB_HANDOFF_MAGIC) {
		pr_err("uefi_jump: mapcheck has invalid fb handoff\n");
		memunmap(ho);
		return;
	}
	pitch = le32_to_cpu(s->pitch);
	height = le32_to_cpu(s->height);
	memunmap(ho);
	total = (u64)pitch * height;

	dev = bus_find_device_by_name(&platform_bus_type, NULL, gProbeOvlName);
	if (!dev) {
		pr_err("uefi_jump: mapcheck cannot find %s\n", gProbeOvlName);
		return;
	}
	domain = iommu_get_domain_for_dev(dev);
	if (!domain) {
		pr_err("uefi_jump: mapcheck has no IOMMU domain for %s\n",
		       gProbeOvlName);
		put_device(dev);
		return;
	}

	pr_info("uefi_jump: mapcheck iova=0x%08x size=0x%llx device=%s\n",
		gProbeIova, (unsigned long long)total, gProbeOvlName);
	for (off = 0; off < total; off += PAGE_SIZE) {
		phys_addr_t pa = iommu_iova_to_phys(
			domain, (dma_addr_t)(gProbeIova + off));

		if (!pa) {
			pr_err("uefi_jump: mapcheck iova 0x%08llx has no phys\n",
			       (unsigned long long)(gProbeIova + off));
			put_device(dev);
			return;
		}
		if (prev_end && pa != prev_end) {
			if (gaps < 16)
				pr_info("uefi_jump: mapcheck gap at +0x%llx iova=0x%08llx phys=0x%llx expected=0x%llx\n",
					(unsigned long long)off,
					(unsigned long long)(gProbeIova + off),
					(unsigned long long)pa,
					(unsigned long long)prev_end);
			gaps++;
		}
		prev_end = pa + PAGE_SIZE;
	}
	put_device(dev);

	pr_info("uefi_jump: mapcheck result pages=%llu noncontiguous=%u %s\n",
		(unsigned long long)(total / PAGE_SIZE), gaps,
		gaps ? "SCATTERED" : "CONTIGUOUS");
}

static bool write_fb_page_map(flush_dcache_t flush_dcache)
{
	void *ho = memremap(FB_HANDOFF_ADDR, 0x1000, MEMREMAP_WB);
	void *mo;
	struct fb_handoff *handoff;
	struct fb_page_map *map;
	struct device *dev;
	struct iommu_domain *domain;
	u64 total, aligned_total, expected_pa;
	u32 page_count, run_count, i;
	bool ok = false;

	fb_map_error = 0;
	if (!ho || !gProbeIova || !gProbeOvlName) {
		fb_map_error = 201;
		pr_err("uefi_jump: cannot prepare fb page map\n");
		goto out;
	}

	handoff = ho;
	mo = (u8 *)ho + (FB_PAGE_MAP_ADDR - FB_HANDOFF_ADDR);
	map = mo;
	if (le64_to_cpu(handoff->magic) != FB_HANDOFF_MAGIC) {
		fb_map_error = 202;
		pr_err("uefi_jump: page map has invalid fb handoff\n");
		goto out;
	}

	total = (u64)le32_to_cpu(handoff->pitch) *
		le32_to_cpu(handoff->height);
	aligned_total = ALIGN(total, PAGE_SIZE);
	page_count = aligned_total / PAGE_SIZE;
	if (!total || !page_count) {
		fb_map_error = 203;
		pr_err("uefi_jump: fb is too large for page map: 0x%llx\n",
		       (unsigned long long)total);
		goto out;
	}

	dev = bus_find_device_by_name(&platform_bus_type, NULL, gProbeOvlName);
	if (!dev) {
		fb_map_error = 204;
		pr_err("uefi_jump: page map cannot find %s\n", gProbeOvlName);
		goto out;
	}
	domain = iommu_get_domain_for_dev(dev);
	if (!domain) {
		fb_map_error = 205;
		pr_err("uefi_jump: page map has no IOMMU domain\n");
		put_device(dev);
		goto out;
	}

	memset(mo, 0, FB_PAGE_MAP_SIZE);
	map->page_size = cpu_to_le32(PAGE_SIZE);
	run_count = 0;
	expected_pa = 0;
	for (i = 0; i < page_count; i++) {
		phys_addr_t pa = iommu_iova_to_phys(
			domain, (dma_addr_t)(gProbeIova + (u64)i * PAGE_SIZE));

		if (!pa || !IS_ALIGNED(pa, PAGE_SIZE)) {
			/*
			 * Some DRM mappings omit the final partial page. The panel
			 * uses that page only for the last 1 KiB of pixels; dropping
			 * it is safer than refusing the entire handoff.
			 */
			if ((i + 1 == page_count) &&
			    (!IS_ALIGNED(total, PAGE_SIZE))) {
				pr_warn("uefi_jump: dropping unmapped partial tail page %u\n",
					i);
				page_count = i;
				break;
			}
			fb_map_error = 1000 + (int)i;
			pr_err("uefi_jump: page map missing phys at page %u\n", i);
			put_device(dev);
			goto out;
		}
		if (i == 0 || pa != expected_pa) {
			if (run_count >= FB_PAGE_MAP_MAX_RUNS) {
				fb_map_error = 207;
				pr_err("uefi_jump: too many fb physical runs\n");
				put_device(dev);
				goto out;
			}
			map->runs[run_count].physical = cpu_to_le64(pa);
			map->runs[run_count].pages = cpu_to_le32(1);
			run_count++;
		} else {
			u32 pages = le32_to_cpu(map->runs[run_count - 1].pages);

			map->runs[run_count - 1].pages =
				cpu_to_le32(pages + 1);
		}
		expected_pa = pa + PAGE_SIZE;
	}
	map->logical_size =
		cpu_to_le64((u64)page_count * PAGE_SIZE);
	map->run_count = cpu_to_le32(run_count);
	map->magic = cpu_to_le64(FB_PAGE_MAP_MAGIC);
	put_device(dev);

	if (flush_dcache)
		do_flush_dcache(flush_dcache, ho, 0x1000);
	ok = true;
	pr_info("uefi_jump: wrote fb run map: %u runs, %u pages, size=0x%llx\n",
		run_count, page_count, (unsigned long long)total);

out:
	if (ho)
		memunmap(ho);
	return ok;
}

static bool copy_payload(flush_dcache_t flush_dcache, const void *buffer,
			 size_t size)
{
	void *payload;
	void *ho;
	struct payload_handoff *info;

	payload = memremap(PAYLOAD_BASE, PAYLOAD_MAX, MEMREMAP_WB);
	if (!payload) {
		pr_err("uefi_jump: cannot map payload area\n");
		return false;
	}
	memcpy(payload, buffer, size);
	if (flush_dcache)
		do_flush_dcache(flush_dcache, payload, size);
	memunmap(payload);

	ho = memremap(FB_HANDOFF_ADDR, 0x1000, MEMREMAP_WB);
	if (!ho) {
		pr_err("uefi_jump: cannot map payload handoff page\n");
		return false;
	}
	info = (struct payload_handoff *)((u8 *)ho +
		(PAYLOAD_INFO_ADDR - FB_HANDOFF_ADDR));
	memset(info, 0, sizeof(*info));
	info->magic = cpu_to_le64(PAYLOAD_MAGIC);
	info->base = cpu_to_le64(PAYLOAD_BASE);
	info->size = cpu_to_le64(size);
	info->block_size = cpu_to_le32(512);
	if (flush_dcache)
		do_flush_dcache(flush_dcache, ho, 0x1000);
	pr_info("uefi_jump: payload handoff magic=%llx base=%llx size=%llx block=%u\n",
		(unsigned long long)le64_to_cpu(info->magic),
		(unsigned long long)le64_to_cpu(info->base),
		(unsigned long long)le64_to_cpu(info->size),
		le32_to_cpu(info->block_size));
	memunmap(ho);

	pr_info("uefi_jump: copied payload %zu bytes to 0x%lx\n",
		size, PAYLOAD_BASE);
	return true;
}

static void paint_contig_progress(flush_dcache_t flush_dcache,
				  phys_addr_t framebuffer,
				  u32 pitch, u32 completed)
{
	void *map;
	u32 *pixels;
	u32 rows = 160;
	u32 width;
	u32 bar_width;
	u32 i;
	u32 y;
	u32 x;

	if (!framebuffer || !pitch || !completed)
		return;
	if (completed > 9)
		completed = 9;

	map = memremap(framebuffer, (phys_addr_t)pitch * rows, MEMREMAP_WB);
	if (!map)
		return;
	pixels = map;
	width = pitch / sizeof(u32);

	for (i = 0; i < pitch * rows / sizeof(u32); i++)
		pixels[i] = 0xFF000000;

	bar_width = width / (completed * 4);
	if (bar_width < 6)
		bar_width = 6;
	for (i = 0; i < completed; i++) {
		u32 center = ((i + 1) * width) / (completed + 1);
		u32 start = center > bar_width / 2
			? center - bar_width / 2 : 0;

		for (y = 0; y < rows; y++) {
			for (x = start; x < start + bar_width && x < width; x++)
				pixels[y * width + x] = 0xFFFFFFFF;
		}
	}
	if (flush_dcache)
		do_flush_dcache(flush_dcache, map,
				(size_t)pitch * rows);
	memunmap(map);
}

static bool setup_contiguous_framebuffer(flush_dcache_t flush_dcache,
					 u32 ovl_base, const char *ovl_name,
					 u64 size, phys_addr_t *base,
					 u32 old_pitch)
{
	struct device *dev;
	struct iommu_domain *domain;
	void *cpu;
	void __iomem *ovl;
	u64 aligned_size = ALIGN(size, PAGE_SIZE);
	u64 off;
	int ret;

	if (!contig_fb) {
		return false;
	}
	if (!size || aligned_size > CONTIG_FB_SIZE) {
		pr_err("uefi_jump: contiguous fb size 0x%llx is out of range\n",
		       (unsigned long long)size);
		return false;
	}
	contig_diag_result = 0;
	pr_info("uefi_jump: UEFI_CONTIG_STAGE=1\n");
	paint_contig_progress(flush_dcache, *base, old_pitch, 1);

	dev = bus_find_device_by_name(&platform_bus_type, NULL, ovl_name);
	if (!dev) {
		pr_err("uefi_jump: contiguous fb cannot find %s\n", ovl_name);
		return false;
	}
	pr_info("uefi_jump: UEFI_CONTIG_STAGE=2\n");
	paint_contig_progress(flush_dcache, *base, old_pitch, 2);
	domain = iommu_get_domain_for_dev(dev);
	if (!domain) {
		pr_err("uefi_jump: contiguous fb has no IOMMU domain\n");
		put_device(dev);
		return false;
	}
	pr_info("uefi_jump: UEFI_CONTIG_STAGE=3\n");
	paint_contig_progress(flush_dcache, *base, old_pitch, 3);

	pr_info("uefi_jump: contiguous fb domain aperture 0x%llx..0x%llx\n",
		(unsigned long long)domain->geometry.aperture_start,
		(unsigned long long)domain->geometry.aperture_end);
	if (domain->geometry.force_aperture &&
	    ((CONTIG_FB_IOVA < domain->geometry.aperture_start) ||
	     (CONTIG_FB_IOVA + aligned_size >
	      domain->geometry.aperture_end))) {
		pr_err("uefi_jump: contiguous fb IOVA 0x%lx is outside aperture\n",
		       (unsigned long)CONTIG_FB_IOVA);
		put_device(dev);
		return false;
	}

	if ((gProbeIova == CONTIG_FB_IOVA) &&
	    (iommu_iova_to_phys(domain, CONTIG_FB_IOVA) ==
	     CONTIG_FB_BASE)) {
		*base = CONTIG_FB_BASE;
		put_device(dev);
		return true;
	}

	for (off = 0; off < aligned_size; off += PAGE_SIZE) {
		if (iommu_iova_to_phys(domain,
				       (dma_addr_t)(CONTIG_FB_IOVA + off))) {
			pr_err("uefi_jump: contiguous fb IOVA 0x%lx+0x%llx is in use\n",
			       (unsigned long)CONTIG_FB_IOVA,
			       (unsigned long long)off);
			put_device(dev);
			return false;
		}
	}
	pr_info("uefi_jump: UEFI_CONTIG_STAGE=4\n");
	paint_contig_progress(flush_dcache, *base, old_pitch, 4);
	if (contig_stage == 1) {
		contig_diag_result = 221;
		put_device(dev);
		return false;
	}

	ret = iommu_map(domain, CONTIG_FB_IOVA, CONTIG_FB_BASE, aligned_size,
			IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE);
	if (ret) {
		pr_err("uefi_jump: contiguous fb iommu_map failed %d\n", ret);
		put_device(dev);
		return false;
	}
	pr_info("uefi_jump: UEFI_CONTIG_STAGE=5\n");
	paint_contig_progress(flush_dcache, *base, old_pitch, 5);
	if (contig_stage == 2) {
		(void)iommu_unmap(domain, CONTIG_FB_IOVA, aligned_size);
		iommu_flush_tlb_all(domain);
		contig_diag_result = 222;
		put_device(dev);
		return false;
	}
	iommu_flush_tlb_all(domain);
	pr_info("uefi_jump: UEFI_CONTIG_STAGE=6\n");
	paint_contig_progress(flush_dcache, *base, old_pitch, 6);
	if (contig_stage == 3) {
		(void)iommu_unmap(domain, CONTIG_FB_IOVA, aligned_size);
		iommu_flush_tlb_all(domain);
		contig_diag_result = 223;
		put_device(dev);
		return false;
	}

	cpu = memremap(CONTIG_FB_BASE, aligned_size, MEMREMAP_WB);
	if (!cpu) {
		pr_err("uefi_jump: contiguous fb memremap failed\n");
		iommu_unmap(domain, CONTIG_FB_IOVA, aligned_size);
		iommu_flush_tlb_all(domain);
		put_device(dev);
		return false;
	}
	pr_info("uefi_jump: UEFI_CONTIG_STAGE=7\n");
	paint_contig_progress(flush_dcache, *base, old_pitch, 7);
	if (contig_stage == 4) {
		memunmap(cpu);
		(void)iommu_unmap(domain, CONTIG_FB_IOVA, aligned_size);
		iommu_flush_tlb_all(domain);
		contig_diag_result = 224;
		put_device(dev);
		return false;
	}
	memset(cpu, 0, aligned_size);
	{
		u32 *pixels = cpu;
		u64 count = size / sizeof(u32);
		u64 index;

		for (index = 0; index < count; index++)
			pixels[index] = 0xFF0000FF;
	}
	if (flush_dcache)
		do_flush_dcache(flush_dcache, cpu, aligned_size);
	memunmap(cpu);
	pr_info("uefi_jump: UEFI_CONTIG_STAGE=8\n");
	paint_contig_progress(flush_dcache, *base, old_pitch, 8);
	if (contig_stage == 5) {
		(void)iommu_unmap(domain, CONTIG_FB_IOVA, aligned_size);
		iommu_flush_tlb_all(domain);
		contig_diag_result = 225;
		put_device(dev);
		return false;
	}

	ovl = ioremap(ovl_base, 0x1000);
	if (!ovl) {
		pr_err("uefi_jump: contiguous fb cannot map %s registers\n",
		       ovl_name);
		iommu_unmap(domain, CONTIG_FB_IOVA, aligned_size);
		iommu_flush_tlb_all(domain);
		put_device(dev);
		return false;
	}
	if (contig_stage == 6) {
		iounmap(ovl);
		(void)iommu_unmap(domain, CONTIG_FB_IOVA, aligned_size);
		iommu_flush_tlb_all(domain);
		contig_diag_result = 226;
		put_device(dev);
		return false;
	}
	writel(CONTIG_FB_IOVA, ovl + DISP_REG_OVL_ADDR);
	wmb();
	(void)readl(ovl + DISP_REG_OVL_ADDR);
	iounmap(ovl);
	put_device(dev);
	pr_info("uefi_jump: UEFI_CONTIG_STAGE=9\n");
	paint_contig_progress(flush_dcache, *base, old_pitch, 9);
	if (contig_stage == 7) {
		contig_diag_result = 227;
		return true;
	}

	gProbeIova = CONTIG_FB_IOVA;
	*base = CONTIG_FB_BASE;
	pr_info("uefi_jump: contiguous fb active phys=0x%lx iova=0x%lx size=0x%llx\n",
		(unsigned long)CONTIG_FB_BASE,
		(unsigned long)CONTIG_FB_IOVA,
		(unsigned long long)aligned_size);
	return true;
}

extern void camellia_jump_to(unsigned long entry);

static bool probe_display_regs(flush_dcache_t flush_dcache)
{
	void __iomem *base;
	u32 val = 0;
	u32 src_size = 0;
	u32 pitch_reg = 0;
	u32 pitch_msb = 0;
	u32 con = 0;
	u32 src_con = 0;
	u32 datapath_con = 0;
	u32 ovl_en = 0;
	u32 roi_size = 0;
	u32 roi_bgclr = 0;
	u32 clip = 0;
	u32 layer_offset = 0;
	u32 active_ovl_base = 0;
	bool wrote = false;
	const char *ovl_dev_name = NULL;
	static const struct {
		const char *name;
		const char *dev_name;
		unsigned long base;
	} ovls[] = {
		{ "OVL0_2L", "14006000.disp_ovl0_2l", DISP_OVL0_2L_BASE },
		{ "OVL0", "14005000.disp_ovl0", DISP_OVL0_BASE },
	};
	int i;

	base = ioremap(DISP_RDMA0_BASE, 0x1000);
	if (base) {
		u32 rdma = readl(base + DISP_REG_RDMA_MEM_START);

		pr_info("uefi_jump: RDMA0 mem_start_addr @0x%lx = 0x%08x\n",
			DISP_RDMA0_BASE + DISP_REG_RDMA_MEM_START, rdma);
		iounmap(base);
	} else {
		pr_warn("uefi_jump: cannot map RDMA0\n");
	}

	for (i = 0; i < ARRAY_SIZE(ovls); i++) {
		struct device *candidate_dev;
		struct iommu_domain *candidate_domain;
		phys_addr_t candidate_pa = 0;
		u32 addr, size, pitch, msb, layer_con;
		bool active;

		base = ioremap(ovls[i].base, 0x1000);
		if (!base) {
			pr_warn("uefi_jump: cannot map %s\n", ovls[i].name);
			continue;
		}
		addr = readl(base + DISP_REG_OVL_ADDR);
		size = readl(base + DISP_REG_OVL_L0_SRC_SIZE);
		pitch = readl(base + DISP_REG_OVL_L0_PITCH);
		msb = readl(base + DISP_REG_OVL_L0_PITCH_MSB);
		layer_con = readl(base + DISP_REG_OVL_SRC_CON);
		/* A stale address can survive with FORCE_RELAY_MODE and no layers. */
		active = addr != 0 && (layer_con & 0xf);
		if (active) {
			candidate_dev = bus_find_device_by_name(
				&platform_bus_type, NULL, ovls[i].dev_name);
			if (candidate_dev) {
				candidate_domain =
					iommu_get_domain_for_dev(candidate_dev);
				if (candidate_domain)
					candidate_pa = iommu_iova_to_phys(
						candidate_domain,
						(dma_addr_t)addr);
				put_device(candidate_dev);
			}
			if (!candidate_pa) {
				pr_warn("uefi_jump: %s active addr 0x%08x has no IOMMU page 0\n",
					ovls[i].name, addr);
				active = false;
			}
		}

		pr_info("uefi_jump: %s addr @0x%lx = 0x%08x size=0x%08x pitch=%u msb=0x%x src_con=0x%x\n",
			ovls[i].name, ovls[i].base + DISP_REG_OVL_ADDR,
			addr, size, pitch, msb, layer_con);
		if (active) {
			val = addr;
			src_size = size;
			pitch_reg = pitch;
			pitch_msb = msb;
			con = readl(base + 0x30);
			src_con = layer_con;
			datapath_con = readl(base + 0x24);
			ovl_en = readl(base + 0x0c);
			roi_size = readl(base + 0x20);
			roi_bgclr = readl(base + 0x28);
			clip = readl(base + 0x4c);
			layer_offset = readl(base + 0x3c);
		}
		iounmap(base);
		if (active) {
			ovl_dev_name = ovls[i].dev_name;
			active_ovl_base = ovls[i].base;
			gProbeIova = val;
			gProbeOvlName = ovl_dev_name;
			break;
		}
	}

	if (val) {
		pr_info("uefi_jump: ovl en=0x%x con=0x%08x src_con=0x%x datapath=0x%08x roi=0x%x bg=0x%08x clip=0x%08x offset=0x%08x\n",
			ovl_en, con, src_con, datapath_con, roi_size,
			roi_bgclr, clip, layer_offset);
		pr_info("uefi_jump: ovl decoded fmt=%u byte_swap=%u rgb_swap=%u comp_bit=%u\n",
			(con >> 12) & 0xf, (con >> 24) & 1,
			(con >> 25) & 1, (datapath_con >> 4) & 1);
	}

	/* If OVL0 holds an address, dump what physical memory is there. */
	if (val) {
		void *fb;
		struct device *dev;
		struct iommu_domain *domain;
		phys_addr_t pa = 0;

		fb = memremap((phys_addr_t)val, 0x1000, MEMREMAP_WB);
		if (fb) {
			const unsigned char *p = fb;

			pr_info("uefi_jump: mem@0x%08x: %02x %02x %02x %02x %02x %02x %02x %02x\n",
				val, p[0], p[1], p[2], p[3],
				p[4], p[5], p[6], p[7]);
			pr_info("uefi_jump: mem@+0x100: %02x %02x %02x %02x %02x %02x %02x %02x\n",
				p[0x100], p[0x101], p[0x102], p[0x103],
				p[0x104], p[0x105], p[0x106], p[0x107]);
			memunmap(fb);
		} else {
			pr_warn("uefi_jump: cannot map mem@0x%08x\n", val);
		}

		dev = bus_find_device_by_name(&platform_bus_type, NULL, ovl_dev_name);
		if (dev) {
			domain = iommu_get_domain_for_dev(dev);
			if (domain) {
				pa = iommu_iova_to_phys(domain, (dma_addr_t)val);
				pr_info("uefi_jump: iova 0x%08x -> phys 0x%llx\n",
					val, (unsigned long long)pa);
				if (pa) {
					void *pafb = memremap(pa, 0x10,
							     MEMREMAP_WB);

					if (pafb) {
						const unsigned char *p = pafb;

						pr_info("uefi_jump: phys %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
							p[0], p[1], p[2], p[3],
							p[4], p[5], p[6], p[7],
							p[8], p[9], p[10], p[11],
							p[12], p[13], p[14], p[15]);
						memunmap(pafb);
					}
				}
			} else {
				pr_warn("uefi_jump: no iommu domain for disp_ovl0\n");
			}
			put_device(dev);
		} else {
			pr_warn("uefi_jump: device 14005000.disp_ovl0 not found\n");
		}

		{
			u32 reg_width = src_size & 0xffff;
			u32 reg_height = (src_size >> 16) & 0xffff;
			u32 reg_pitch = (pitch_reg & 0xffff) |
					 ((pitch_msb & 0xf) << 16);
			u32 width = 1080;
			u32 height = 2400;
			u32 pitch;
			void *ho = memremap(FB_HANDOFF_ADDR, 0x1000, MEMREMAP_WB);

			/*
			 * A linear primary plane describes the full panel. The old
			 * compressed Android buffers reported a tiny block height, so
			 * only trust the registers when the height covers the panel.
			 */
			if (reg_height > 1000) {
				if (reg_width && reg_width <= 4096)
					width = reg_width;
				height = reg_height;
			}
			if (fb_pitch) {
				pitch = fb_pitch;
			} else if (reg_height > 1000 &&
				   reg_pitch >= width * 4 && reg_pitch <= 65536) {
				pitch = reg_pitch;
			} else {
				pitch = width * 4;
			}

			pr_info("uefi_jump: reg says %ux%u pitch %u; using %ux%u pitch %u, phys 0x%llx\n",
				reg_width, reg_height, reg_pitch,
				width, height, pitch, (unsigned long long)pa);

			if (setup_contiguous_framebuffer(flush_dcache, active_ovl_base,
						 ovl_dev_name,
						 (u64)pitch * height,
						 &pa, pitch))
				pr_info("uefi_jump: switched display to contiguous framebuffer\n");

			if (ho) {
				struct fb_handoff *s = ho;

				memset(s, 0, sizeof(*s));
				s->magic = cpu_to_le64(FB_HANDOFF_MAGIC);
				s->base  = cpu_to_le64((u64)pa);
				s->width = cpu_to_le32(width);
				s->height = cpu_to_le32(height);
				s->pitch = cpu_to_le32(pitch);
				s->depth = cpu_to_le32(32);
				memunmap(ho);
				wrote = true;
				pr_info("uefi_jump: wrote fb handoff at 0x%lx\n",
					FB_HANDOFF_ADDR);
			} else {
				pr_warn("uefi_jump: cannot map fb handoff area\n");
			}
		}
	}

	if (!wrote) {
		void *ho = memremap(FB_HANDOFF_ADDR, 0x1000, MEMREMAP_WB);
		if (ho) {
			((__le64 *)ho)[0] = 0;
			memunmap(ho);
		}
	}

	return wrote;
}

static int __init uefi_jump_init(void)
{
	memblock_reserve_t mb_reserve;
	flush_dcache_t flush_dcache;
	smp_send_stop_t stop;
	struct file *f;
	loff_t pos = 0;
	loff_t fsize;
	ssize_t n;
	void *buf;
	void *payload_buf = NULL;
	void *map;
	void *jump_trace;
	u32 crc_src, crc_dst;
	const unsigned char *head;
	bool fb_ready;
	bool payload_ready = false;
	loff_t payload_size = 0;

	if (regs) {
		u64 el = rd_currentel();

		pr_info("uefi_jump: CurrentEL=EL%llu\n", (el >> 2) & 3);
		pr_info("uefi_jump: CNTFRQ_EL0=%llu CNTPCT_EL0=%llu CNTVCT_EL0=%llu\n",
			rd_cntfrq(), rd_cntpct(), rd_cntvct());
		pr_info("uefi_jump: CNTP_CTL=%llx CNTP_CVAL=%llx CNTV_CTL=%llx CNTV_CVAL=%llx\n",
			rd_cntp_ctl(), rd_cntp_cval(), rd_cntv_ctl(), rd_cntv_cval());
		pr_info("uefi_jump: CNTKCTL_EL1=%llx\n", rd_cntkctl());
		if (((el >> 2) & 3) == 2)
			pr_info("uefi_jump: HCR_EL2=%llx CNTHCTL_EL2=%llx\n",
				rd_hcr(), rd_cnthctl());
		return 0;
	}

	if (read_beacon) {
		void *b = memremap(FB_BEACON_ADDR, 0x1000, MEMREMAP_WB);
		if (b) {
			u32 *p = b;

			pr_info("uefi_jump: beacon fb  %08x %08x %08x %08x %08x %08x %08x\n",
				p[0], p[1], p[2], p[3], p[4], p[5], p[6]);
			pr_info("uefi_jump: beacon sec %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
				p[8], p[9], p[10], p[11], p[12], p[13],
				p[14], p[15], p[16], p[17], p[18], p[19], p[20]);
			pr_info("uefi_jump: beacon sec2 %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
				p[24], p[25], p[26], p[27], p[28], p[29],
				p[30], p[31], p[32], p[33], p[34], p[35], p[36]);
			pr_info("uefi_jump: beacon dxe  %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
				p[40], p[41], p[42], p[43], p[44], p[45],
				p[46], p[47], p[48], p[49], p[50]);
			pr_info("uefi_jump: beacon drv  %08x %08x %08x %08x %08x\n",
				p[64], p[65], p[66], p[67], p[68]);
			pr_info("uefi_jump: beacon disp %08x %08x %08x guid %08x %08x %08x %08x\n",
				p[72], p[73], p[74], p[76], p[77], p[78], p[79]);
			pr_info("uefi_jump: beacon img  %08x %08x %08x %08x %08x | %08x %08x %08x %08x %08x %08x\n",
				p[80], p[81], p[82], p[83], p[84],
				p[85], p[86], p[87], p[88], p[89], p[90]);
			pr_info("uefi_jump: beacon tmr  %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
				p[92], p[93], p[94], p[95], p[96],
				p[97], p[98], p[99], p[100], p[101]);
			pr_info("uefi_jump: beacon gic  %08x %08x %08x\n",
				p[104], p[105], p[106]);
			pr_info("uefi_jump: beacon hb   %08x %08x %08x\n",
				p[108], p[109], p[110]);
			pr_info("uefi_jump: beacon stp  %08x %08x %08x %08x %08x %08x %08x\n",
				p[112], p[113], p[114], p[115],
				p[116], p[117], p[118]);
			pr_info("uefi_jump: beacon freq %08x %08x\n",
				p[121], p[122]);
			memunmap(b);
		} else {
			pr_warn("uefi_jump: cannot map beacon\n");
		}
		return 0;
	}

	if (test_beacon) {
		void *b = memremap(FB_BEACON_ADDR, 0x1000, MEMREMAP_WB);
		if (b) {
			u64 *p = b;

			p[0] = 0xDEADBEEF12345678ULL;
			p[1] = 0xAA;
			memunmap(b);
			pr_info("uefi_jump: wrote test beacon DEADBEEF/AA\n");
		}
		return 0;
	}

	mb_reserve = (memblock_reserve_t)kallsyms_lookup_name("memblock_reserve");
	if (!mb_reserve) {
		pr_err("uefi_jump: cannot resolve memblock_reserve\n");
		return -ENOENT;
	}

	f = filp_open(fd_path, O_RDONLY, 0);
	if (IS_ERR(f)) {
		pr_err("uefi_jump: cannot open %s: %ld\n",
		       fd_path, PTR_ERR(f));
		return PTR_ERR(f);
	}

	fsize = i_size_read(file_inode(f));
	if (fsize != FD_SIZE) {
		pr_err("uefi_jump: %s is %lld bytes, need %lu\n",
		       fd_path, fsize, FD_SIZE);
		filp_close(f, NULL);
		return -EINVAL;
	}

	buf = vmalloc(FD_SIZE);
	if (!buf) {
		filp_close(f, NULL);
		return -ENOMEM;
	}

	n = kernel_read(f, buf, FD_SIZE, &pos);
	filp_close(f, NULL);
	if (n != FD_SIZE) {
		pr_err("uefi_jump: read %zd of %lu bytes\n", n, FD_SIZE);
		vfree(buf);
		return -EIO;
	}

	flush_dcache = (flush_dcache_t)kallsyms_lookup_name("__flush_dcache_area");
	if (!flush_dcache)
		pr_warn("uefi_jump: __flush_dcache_area missing, cache not flushed\n");

	fb_ready = probe_display_regs(flush_dcache);
	if (contig_stage) {
		vfree(buf);
		return -(contig_diag_result ? contig_diag_result : 230);
	}

	if (jump && fb_ready && !write_fb_page_map(flush_dcache)) {
		pr_err("uefi_jump: cannot prepare fb page map, refusing to jump\n");
		vfree(buf);
		return -(fb_map_error ? fb_map_error : 208);
	}

	if (mapcheck) {
		check_fb_contiguity();
		vfree(buf);
		return 0;
	}

	if (mapdump) {
		void *mo;
		struct fb_page_map *map;
		u32 i;

		if (!write_fb_page_map(flush_dcache)) {
			vfree(buf);
			return -(fb_map_error ? fb_map_error : 208);
		}
		mo = memremap(FB_HANDOFF_ADDR, 0x1000, MEMREMAP_WB);
		if (!mo) {
			vfree(buf);
			return -ENOMEM;
		}
		map = (struct fb_page_map *)((u8 *)mo +
			(FB_PAGE_MAP_ADDR - FB_HANDOFF_ADDR));
		pr_info("uefi_jump: mapdump runs=%u logical=0x%llx page=%u\n",
			le32_to_cpu(map->run_count),
			(unsigned long long)le64_to_cpu(map->logical_size),
			le32_to_cpu(map->page_size));
		for (i = 0; i < le32_to_cpu(map->run_count); i++)
			pr_info("uefi_jump: mapdump %02u phys=0x%llx pages=%u\n",
				i,
				(unsigned long long)le64_to_cpu(
					map->runs[i].physical),
				le32_to_cpu(map->runs[i].pages));
		memunmap(mo);
		vfree(buf);
		return 0;
	}

	if (fb_bands) {
		paint_test_bands(flush_dcache);
		vfree(buf);
		return 0;
	}

	if (disp_test) {
		u64 start, freq;

		paint_top_band(0xFFFF0000, flush_dcache);
		pr_info("uefi_jump: painted color A (live), stopping CPUs\n");

		stop = (smp_send_stop_t)kallsyms_lookup_name("smp_send_stop");
		if (stop)
			stop();

		paint_top_band(0xFF0000FF, flush_dcache);
		pr_info("uefi_jump: painted color B after stop, waiting 4s\n");

		freq = rd_cntfrq();
		if (!freq)
			freq = 13000000;
		start = rd_cntpct();
		while ((rd_cntpct() - start) < freq * 4ULL)
			asm volatile("yield");

		*(volatile u32 *)(uintptr_t)(0x10007000UL + 0x14) = 0x1209;
		return 0;
	}

	if (fb_paint) {
		void *ho = memremap(FB_HANDOFF_ADDR, 0x1000, MEMREMAP_WB);
		if (ho) {
			struct fb_handoff *s = ho;

			if (le64_to_cpu(s->magic) != FB_HANDOFF_MAGIC) {
				pr_err("uefi_jump: invalid fb handoff, not painting\n");
				memunmap(ho);
				return -EINVAL;
			}
			phys_addr_t base = le64_to_cpu(s->base);
			u32 pitch = le32_to_cpu(s->pitch);
			u32 lines = 200;
			void *fb;
			u32 i;

			fb = memremap(base, (phys_addr_t)pitch * lines,
				      MEMREMAP_WB);
			if (fb) {
				u32 *px = fb;

				for (i = 0; i < pitch * lines / 4; i++)
					px[i] = 0xFF00FF00; /* green */
				if (flush_dcache)
					do_flush_dcache(flush_dcache, fb, (size_t)pitch * lines);
				memunmap(fb);
				pr_info("uefi_jump: painted %u lines at 0x%llx\n",
					lines, (unsigned long long)base);
			} else {
				pr_warn("uefi_jump: cannot map fb 0x%llx\n",
					(unsigned long long)base);
			}
			memunmap(ho);
		}
		return 0;
	}

	if (fb_line) {
		void *ho = memremap(FB_HANDOFF_ADDR, 0x1000, MEMREMAP_WB);
		if (ho) {
			struct fb_handoff *s = ho;

			if (le64_to_cpu(s->magic) != FB_HANDOFF_MAGIC ||
			    le64_to_cpu(s->base) == 0) {
				pr_err("uefi_jump: invalid fb handoff, not drawing\n");
				memunmap(ho);
				return -EINVAL;
			}
			phys_addr_t base = le64_to_cpu(s->base);
			void *fb;
			u32 y;

			fb = memremap(base, 0x1000000, MEMREMAP_WB);
			if (fb) {
				u8 *p = fb;

				for (y = 0; y < 1500; y++) {
					u32 *g = (u32 *)(p + (u64)y * 4320 + 200 * 4);
					u32 *r = (u32 *)(p + (u64)y * 4352 + 400 * 4);
					*g = 0xFF00FF00; /* green: pitch 4320 */
					*r = 0xFFFF0000; /* red: pitch 4352 */
				}
				if (flush_dcache)
					do_flush_dcache(flush_dcache, fb, 0x1000000);
				memunmap(fb);
				pr_info("uefi_jump: drew pitch test lines at 0x%llx\n",
					(unsigned long long)base);
			} else {
				pr_warn("uefi_jump: cannot map fb for line test\n");
			}
			memunmap(ho);
		}
		return 0;
	}

	/* best-effort: mark the window in memblock (see caveat above) */
	mb_reserve(FD_BASE, FD_SIZE);
	mb_reserve(PAYLOAD_BASE, PAYLOAD_MAX);
	if (contig_fb)
		mb_reserve(CONTIG_FB_BASE, CONTIG_FB_SIZE);

	map = memremap(FD_BASE, FD_SIZE, MEMREMAP_WB);
	if (!map) {
		pr_err("uefi_jump: memremap 0x%lx failed\n", FD_BASE);
		vfree(buf);
		return -ENOMEM;
	}
	jump_trace = memremap(JUMP_TRACE_BASE, JUMP_TRACE_SIZE, MEMREMAP_WB);
	if (!jump_trace)
		pr_warn("uefi_jump: cannot map the persistent jump trace\n");

	memcpy(map, buf, FD_SIZE);
	crc_src = crc32_le(~0, buf, FD_SIZE);
	crc_dst = crc32_le(~0, map, FD_SIZE);

	pr_info("uefi_jump: stage1 copied %lu bytes to 0x%lx, crc %08x/%08x %s\n",
		FD_SIZE, FD_BASE, crc_src, crc_dst,
		crc_src == crc_dst ? "OK" : "MISMATCH");

	head = (const unsigned char *)map;
	pr_info("uefi_jump: head %02x%02x%02x%02x, FV signature at 0x28: %c%c%c%c\n",
		head[0], head[1], head[2], head[3],
		head[FV_SIG_OFFSET], head[FV_SIG_OFFSET + 1],
		head[FV_SIG_OFFSET + 2], head[FV_SIG_OFFSET + 3]);

	if (flush_dcache)
		do_flush_dcache(flush_dcache, map, FD_SIZE);

	{
		void *ho = memremap(FB_HANDOFF_ADDR, 0x1000, MEMREMAP_WB);
		if (ho) {
			if (flush_dcache)
				do_flush_dcache(flush_dcache, ho, 0x1000);
			memunmap(ho);
		}
	}

	if (!jump) {
		memunmap(map);
		vfree(buf);
		buf = NULL;
	}

	if (!jump && !copy_payload_only) {
		pr_info("uefi_jump: ready at 0x%lx (jump=0, not jumping)\n",
			FD_BASE);
		return 0;
	}

	if (payload_path && payload_path[0]) {
		f = filp_open(payload_path, O_RDONLY, 0);
	}
	if (payload_path && payload_path[0] && !IS_ERR(f)) {
		payload_size = payload_bytes ?
			(loff_t)payload_bytes : i_size_read(file_inode(f));
		if (payload_bytes)
			pr_info("uefi_jump: using fixed payload size %llu\n",
				(unsigned long long)payload_size);
		if ((payload_size > 0) && (payload_size <= PAYLOAD_MAX) &&
		    ((payload_size % 512) == 0)) {
			payload_buf = vmalloc(payload_size);
			if (payload_buf) {
				pos = 0;
				n = kernel_read(f, payload_buf, payload_size, &pos);
				if (n == payload_size) {
					payload_ready = true;
				} else {
					pr_err("uefi_jump: payload read %zd of %lld\n",
					       n, payload_size);
					vfree(payload_buf);
					payload_buf = NULL;
				}
			}
		} else {
			pr_err("uefi_jump: invalid payload size %lld\n",
			       payload_size);
		}
		filp_close(f, NULL);
	} else if (payload_path && payload_path[0]) {
		pr_warn("uefi_jump: no payload at %s (%ld), continuing without it\n",
			payload_path, PTR_ERR(f));
	}

	/*
	 * Copy the large payload while the normal kernel services and all CPUs
	 * are still available. memremap/file-backed allocation after smp_stop()
	 * is fragile and previously left the UEFI-side handoff magic unset.
	 */
	if (payload_ready) {
		if (!copy_payload(flush_dcache, payload_buf, payload_size)) {
			vfree(payload_buf);
			return -EIO;
		}
		pr_info("uefi_jump: payload handoff committed before SMP stop\n");
		payload_ready = false;
	}
	vfree(payload_buf);
	payload_buf = NULL;

	if (copy_payload_only) {
		pr_info("uefi_jump: copy_payload_only complete\n");
		return 0;
	}

	if (!fb_ready) {
		pr_err("uefi_jump: no framebuffer handoff (screen off?), refusing to jump\n");
		memunmap(map);
		vfree(buf);
		return -EAGAIN;
	}

	stop = (smp_send_stop_t)kallsyms_lookup_name("smp_send_stop");
	if (!stop) {
		pr_err("uefi_jump: cannot resolve smp_send_stop\n");
		memunmap(map);
		vfree(buf);
		return -ENOENT;
	}

	{
		void *b = memremap(FB_BEACON_ADDR, 0x1000, MEMREMAP_WB);
		if (b) {
			memset(b, 0, 0x100);
			if (flush_dcache)
				do_flush_dcache(flush_dcache, b, 0x1000);
			memunmap(b);
		}
	}

	{
		void *ho = memremap(FB_HANDOFF_ADDR, 0x1000, MEMREMAP_WB);
		if (ho) {
			((u32 *)ho)[8] = hb ? 0x48424F4E : 0;
			if (flush_dcache)
				do_flush_dcache(flush_dcache, ho, 0x1000);
			memunmap(ho);
		}
	}

	pr_info("uefi_jump: final handoff prepared, entering the jump critical section\n");
	write_jump_trace(jump_trace, 0, 1, FD_BASE, flush_dcache);

	/*
	 * Stop the other CPUs before the last copy.  With the destination mapping
	 * retained above, the remaining work is only memcpy, CRC and cache flush;
	 * no allocator, IOMMU, logging or lock-taking subsystem runs after SMP stop.
	 */
	stop();
	local_irq_disable();
	local_fiq_disable();

	memcpy(map, buf, FD_SIZE);
	crc_dst = crc32_le(~0, map, FD_SIZE);
	head = (const unsigned char *)map;
	if ((crc_dst != crc_src) ||
	    (head[FV_SIG_OFFSET + 0] != '_') ||
	    (head[FV_SIG_OFFSET + 1] != 'F') ||
	    (head[FV_SIG_OFFSET + 2] != 'V') ||
	    (head[FV_SIG_OFFSET + 3] != 'H')) {
		write_jump_trace(jump_trace, 1, 2, crc_dst,
		    flush_dcache);
		for (;;)
			asm volatile("wfe");
	}
	write_jump_trace(jump_trace, 1, 3, crc_dst, flush_dcache);
	if (flush_dcache)
		do_flush_dcache(flush_dcache, map, FD_SIZE);

	disable_arch_timers();
	asm volatile("dsb sy; isb" : : : "memory");
	write_jump_trace(jump_trace, 2, 4, FD_BASE, flush_dcache);
	asm volatile("dsb sy; isb" : : : "memory");
	camellia_jump_to(FD_BASE);

	/* not reached */
	return 0;
}

static void __exit uefi_jump_exit(void)
{
	pr_info("uefi_jump: unloaded\n");
}

module_init(uefi_jump_init);
module_exit(uefi_jump_exit);
