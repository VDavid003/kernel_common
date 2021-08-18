#if defined(CONFIG_OF_RESERVED_MEM) && defined(CONFIG_DMA_CMA)

#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/memblock.h>
#include <linux/dma-contiguous.h>
#include <linux/cma.h>
#include <linux/dma-mapping.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/kref.h>
#include <linux/genalloc.h>
#include <linux/exynos_ion.h>

#include <asm/dma-contiguous.h>

#include <linux/exynos_ion.h>

#include "../ion.h"
#include "../ion_priv.h"

struct ion_device *ion_exynos;

struct ion_heap *system_heap;

static struct ion_platform_heap ion_noncontig_heap __initdata = {
	.name = "ion_noncontig_heap",
	.type = ION_HEAP_TYPE_SYSTEM,
	.id = EXYNOS_ION_HEAP_SYSTEM_ID,
};

struct ion_client *exynos_ion_client_create(const char *name)
{
	return ion_client_create(ion_exynos, name);
}

static int __init exynos_ion_populate_heaps(struct ion_device *ion_dev)
{
	int ret;

	system_heap = ion_heap_create(&ion_noncontig_heap);
	if (IS_ERR(system_heap)) {
		pr_err("%s: failed to create heap %s\n", __func__,
				ion_noncontig_heap.name);
		ret = PTR_ERR(system_heap);
		goto err;
	}

	ion_device_add_heap(ion_exynos, system_heap);

	return 0;

err:
	ion_heap_destroy(system_heap);

	return ret;
}

static int __init exynos_ion_init(void)
{
	ion_exynos = ion_device_create(NULL);
	if (IS_ERR(ion_exynos)) {
		pr_err("%s: failed to create ion device\n", __func__);
		return PTR_ERR(ion_exynos);
	}

	return exynos_ion_populate_heaps(ion_exynos);
}

subsys_initcall(exynos_ion_init);
#endif
