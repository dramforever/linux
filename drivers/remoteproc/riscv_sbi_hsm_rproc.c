// SPDX-License-Identifier: GPL-2.0
/*
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/remoteproc.h>
#include <linux/platform_device.h>
#include <asm/sbi.h>

#include "remoteproc_internal.h"

#define SBI_EXT_REMOTE_STOP 0x08005253
#define SBI_REMOTE_STOP_SYNC 0x1

struct riscv_sbi_hsm_rproc {
	struct device *dev;
};

static inline int rproc_mem_entry_memremap_wb(struct rproc *rproc,
					     struct rproc_mem_entry *mem)
{
	mem->va = memremap(mem->dma, mem->len, MEMREMAP_WB);
	if (!mem->va) {
		dev_err(&rproc->dev, "Unable to map memory region: %pa+%zx\n",
			&mem->dma, mem->len);
		return -ENOMEM;
	}

	mem->is_iomem = false;
	return 0;
}

static inline int rproc_mem_entry_memunmap(struct rproc *rproc,
					   struct rproc_mem_entry *mem)
{
	memunmap(mem->va);
	return 0;
}

static int riscv_sbi_hsm_rproc_prepare(struct rproc *rproc)
{
	struct riscv_sbi_hsm_rproc *priv = rproc->priv;
	struct device_node *np = priv->dev->of_node;
	struct rproc_mem_entry *mem;
	struct resource res;
	int ret;
	int i;

	for (i = 0; ; i ++) {
		ret = of_reserved_mem_region_to_resource(np, i, &res);
		if (ret)
			break;

		mem = rproc_mem_entry_init(priv->dev, NULL, (dma_addr_t)res.start,
					resource_size(&res), res.start,
					rproc_mem_entry_memremap_wb,
					rproc_mem_entry_memunmap,
					"%.*s", strchrnul(res.name, '@') - res.name,
					res.name);

		rproc_coredump_add_segment(rproc, res.start, resource_size(&res));
		rproc_add_carveout(rproc, mem);
	}


	return 0;
}

static int riscv_sbi_hsm_rproc_start(struct rproc *rproc)
{
	struct riscv_sbi_hsm_rproc *priv = rproc->priv;
	struct device *dev = priv->dev;
	struct sbiret ret;

	dev_info(dev, "Starting a secondary hart at %#llx", rproc->bootaddr);

	ret = sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_START,
			1, (unsigned long)rproc->bootaddr, -1,
			0, 0, 0);
	return sbi_err_map_linux_errno(ret.error);
}

static int riscv_sbi_hsm_rproc_stop(struct rproc *rproc)
{
	struct riscv_sbi_hsm_rproc *priv = rproc->priv;
	struct device *dev = priv->dev;
	struct sbiret ret;

	dev_info(dev, "Stopping secondary hart");

	ret = sbi_ecall(SBI_EXT_REMOTE_STOP, SBI_REMOTE_STOP_SYNC,
			1, 1,
			0, 0, 0, 0);
	return sbi_err_map_linux_errno(ret.error);
}

static const struct rproc_ops riscv_sbi_hsm_rproc_ops = {
	.prepare	= riscv_sbi_hsm_rproc_prepare,
	.start		= riscv_sbi_hsm_rproc_start,
	.stop		= riscv_sbi_hsm_rproc_stop,
};

static int riscv_sbi_hsm_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct riscv_sbi_hsm_rproc *priv;
	const char *fw_name = NULL;
	struct rproc *rproc;
	int ret;

	ret = rproc_of_parse_firmware(dev, 0, &fw_name);
	if (ret < 0 && ret != -EINVAL)
		return dev_err_probe(dev, ret, "Failed to parse firmware-name\n");

	rproc = devm_rproc_alloc(dev, dev_name(dev), &riscv_sbi_hsm_rproc_ops,
				 fw_name, sizeof(*priv));
	if (!rproc)
		return -ENOMEM;

	priv = rproc->priv;
	priv->dev = dev;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return dev_err_probe(dev, ret, "Failed to set DMA mask\n");

	platform_set_drvdata(pdev, rproc);

	ret = devm_rproc_add(dev, rproc);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add rproc\n");

	return 0;
}

static const struct of_device_id riscv_sbi_hsm_rproc_match[] = {
	{ .compatible = "openruyi,hsm-rproc" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, riscv_sbi_hsm_rproc_match);

static struct platform_driver riscv_sbi_hsm_rproc_driver = {
	.driver = {
		.name = "riscv-sbi-hsm-rproc",
		.of_match_table = riscv_sbi_hsm_rproc_match,
	},
	.probe = riscv_sbi_hsm_rproc_probe,
};
module_platform_driver(riscv_sbi_hsm_rproc_driver);

MODULE_DESCRIPTION("RISC-V SBI HSM remoteproc driver");
MODULE_AUTHOR("Vivian Wang <wangruikang@iscas.ac.cn>");
MODULE_LICENSE("GPL");
