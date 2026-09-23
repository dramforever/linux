// SPDX-License-Identifier: GPL-2.0
/*
 */

#include <linux/devm-helpers.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/remoteproc.h>
#include <linux/platform_device.h>
#include <asm/sbi.h>

#include "remoteproc_internal.h"

#define SBI_EXT_REMOTE_STOP 0x08005253
#define SBI_REMOTE_STOP_SYNC 0x1

struct riscv_sbi_hsm_rproc {
	struct device *dev;
	struct rproc *rproc;
	struct work_struct irq_work;
	int msi_rsc_offset;
	int irq;
};

static int rproc_mem_entry_memremap_wb(struct rproc *rproc,
				       struct rproc_mem_entry *mem)
{
	void *va;

	va = memremap(mem->dma, mem->len, MEMREMAP_WB);
	if (!va) {
		dev_err(&rproc->dev, "Unable to map memory region: %pa+%zx\n",
			&mem->dma, mem->len);
		return -ENOMEM;
	}

	mem->va = (void *)va;
	mem->is_iomem = true;

	return 0;
}

static int rproc_mem_entry_memunmap(struct rproc *rproc,
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

struct fw_rsc_msi {
	u32 data;
	u64 addr;
} __packed;

static int riscv_sbi_hsm_rproc_handle_rsc(struct rproc *rproc, u32 rsc_type,
					  void *ptr, int offset, int avail)
{
	struct riscv_sbi_hsm_rproc *priv = rproc->priv;
	struct device *dev = priv->dev;

	if (sizeof(struct fw_rsc_msi) > avail) {
		dev_err(dev, "MSI resource table entry size is too small\n");
		return -EINVAL;
	}

	priv->msi_rsc_offset = offset;

	return 0;
}

static irqreturn_t riscv_sbi_hsm_rproc_irq(int irq, void *dev_id)
{
	struct rproc *rproc = dev_id;
	struct riscv_sbi_hsm_rproc *priv = rproc->priv;

	/* rproc_vq_interrupt() can sleep, so it has to be in a work. */
	schedule_work(&priv->irq_work);

	return IRQ_HANDLED;
}

static void riscv_sbi_hsm_rproc_irq_work(struct work_struct *work)
{
	struct riscv_sbi_hsm_rproc *priv;

	priv = container_of(work, struct riscv_sbi_hsm_rproc, irq_work);

	/* Nothing to handle for the return values here */
	rproc_vq_interrupt(priv->rproc, 0);
	rproc_vq_interrupt(priv->rproc, 1);
}

static int riscv_sbi_hsm_rproc_start(struct rproc *rproc)
{
	struct riscv_sbi_hsm_rproc *priv = rproc->priv;
	struct device *dev = priv->dev;
	struct sbiret srt;
	int ret;

	ret = request_irq(priv->irq, riscv_sbi_hsm_rproc_irq,
			  IRQF_SHARED, "MSI", rproc);
	if (ret < 0) {
		dev_err(dev, "Failed to set up MSI: %pe\n", ERR_PTR(ret));
		goto err_free_irq;
	}

	dev_info(dev, "Starting a secondary hart 3 at %#llx", rproc->bootaddr);

	srt = sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_START,
			3, (unsigned long)rproc->bootaddr, -1,
			0, 0, 0);
	ret = sbi_err_map_linux_errno(srt.error);
	if (ret < 0) {
		dev_err(dev, "Failed to start hart with HSM: %pe\n", ERR_PTR(ret));
		goto err_free_irq;
	}

	return 0;

err_free_irq:
	free_irq(priv->irq, rproc);

	return ret;
}

static int riscv_sbi_hsm_rproc_stop(struct rproc *rproc)
{
	struct riscv_sbi_hsm_rproc *priv = rproc->priv;
	struct device *dev = priv->dev;
	struct sbiret srt;
	int ret;

	dev_info(dev, "Stopping secondary hart 3");

	srt = sbi_ecall(SBI_EXT_REMOTE_STOP, SBI_REMOTE_STOP_SYNC,
			1, 3,
			0, 0, 0, 0);
	ret = sbi_err_map_linux_errno(srt.error);
	if (ret < 0) {
		dev_err(dev, "Failed to stop hart with HSM: %pe\n", ERR_PTR(ret));
		return ret;
	}

	/*
	 * This has to be after stopping, since if the rproc did not stop the
	 * MSI could still be in use.
	 */
	free_irq(priv->irq, rproc);

	return 0;
}

static void riscv_sbi_hsm_rproc_kick(struct rproc *rproc, int vqid)
{
	struct riscv_sbi_hsm_rproc *priv = rproc->priv;
	struct device *dev = priv->dev;
	dev_err(dev, "stub %s(%d)\n", __func__, vqid);
}

static void riscv_sbi_hsm_rproc_write_msi_msg(struct msi_desc *desc, struct msi_msg *msg)
{
	struct device *dev = msi_desc_to_dev(desc);
	struct rproc *rproc = dev_get_drvdata(dev);
	struct riscv_sbi_hsm_rproc *priv = rproc->priv;
	struct fw_rsc_msi *rsc;

	if (!rproc->table_ptr) {
		dev_dbg(dev, "No resource table, no MSI info to update\n");
		return;
	}

	if (priv->msi_rsc_offset <= 0) {
		dev_dbg(dev, "No MSI resource, nothing to update\n");
		return;
	}

	dev_dbg(dev, "Updating MSI information %#x -> %#x%08x\n", msg->data, msg->address_hi, msg->address_lo);
	rsc = (void*)rproc->table_ptr + priv->msi_rsc_offset;
	WRITE_ONCE(rsc->addr, ((u64)msg->address_hi << 32) | msg->address_lo);
	WRITE_ONCE(rsc->data, msg->data);
}

static const struct rproc_ops riscv_sbi_hsm_rproc_ops = {
	.prepare = riscv_sbi_hsm_rproc_prepare,
	.handle_rsc = riscv_sbi_hsm_rproc_handle_rsc,
	.start = riscv_sbi_hsm_rproc_start,
	.stop = riscv_sbi_hsm_rproc_stop,
	.kick = riscv_sbi_hsm_rproc_kick,
};

static void riscv_sbi_hsm_rproc_free_msis(void *data)
{
	struct device *dev = data;

	platform_device_msi_free_irqs_all(dev);
}

static bool __read_mostly auto_boot = true;
module_param(auto_boot, bool, 0444);

static int riscv_sbi_hsm_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct riscv_sbi_hsm_rproc *priv;
	const char *fw_name = NULL;
	struct rproc *rproc;
	int ret;

	of_msi_configure(dev, dev->of_node);

	ret = rproc_of_parse_firmware(dev, 0, &fw_name);
	if (ret < 0 && ret != -EINVAL)
		return dev_err_probe(dev, ret, "Failed to parse firmware-name\n");

	rproc = devm_rproc_alloc(dev, dev_name(dev), &riscv_sbi_hsm_rproc_ops,
				 fw_name, sizeof(*priv));
	if (!rproc)
		return -ENOMEM;

	rproc->auto_boot = auto_boot;

	platform_set_drvdata(pdev, rproc);
	priv = rproc->priv;
	priv->dev = dev;
	priv->rproc = rproc;

	ret = devm_work_autocancel(dev, &priv->irq_work, riscv_sbi_hsm_rproc_irq_work);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to set up irq_work\n");

	ret = platform_device_msi_init_and_alloc_irqs(dev, 1, riscv_sbi_hsm_rproc_write_msi_msg);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to allocate MSIs\n");

	devm_add_action(dev, riscv_sbi_hsm_rproc_free_msis, dev);

	priv->irq = msi_get_virq(dev, 0);
	if (!priv->irq)
		return dev_err_probe(dev, -ENODEV, "Failed to get MSI irq\n");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return dev_err_probe(dev, ret, "Failed to set DMA mask\n");

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
