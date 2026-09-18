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
	struct msi_msg msi_msg;
	struct work_struct irq_work;
	int irq;
};

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
					   rproc_mem_entry_ioremap_wc,
					   rproc_mem_entry_iounmap,
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
	struct fw_rsc_msi *rsc = ptr;

	if (sizeof(*rsc) > avail) {
		dev_err(dev, "MSI resource table entry size is too small\n");
		return -EINVAL;
	}

	rsc->addr = ((u64)priv->msi_msg.address_hi << 32) | priv->msi_msg.address_lo;
	rsc->data = priv->msi_msg.data;

	return 0;
}

static int riscv_sbi_hsm_rproc_start(struct rproc *rproc)
{
	struct riscv_sbi_hsm_rproc *priv = rproc->priv;
	struct device *dev = priv->dev;
	struct sbiret ret;

	dev_info(dev, "Starting a secondary hart 1 at %#llx", rproc->bootaddr);

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

	dev_info(dev, "Stopping secondary hart 1");

	ret = sbi_ecall(SBI_EXT_REMOTE_STOP, SBI_REMOTE_STOP_SYNC,
			1, 1,
			0, 0, 0, 0);
	return sbi_err_map_linux_errno(ret.error);
}

static void riscv_sbi_hsm_rproc_kick(struct rproc *rproc, int vqid)
{
	struct riscv_sbi_hsm_rproc *priv = rproc->priv;
	struct device *dev = priv->dev;
	dev_info(dev, "stub %s(%d)\n", __func__, vqid);
}

static void riscv_sbi_hsm_rproc_write_msi_msg(struct msi_desc *desc, struct msi_msg *msg)
{
	struct device *dev = msi_desc_to_dev(desc);
	struct rproc *rproc = dev_get_drvdata(dev);
	struct riscv_sbi_hsm_rproc *priv = rproc->priv;

	priv->msi_msg = *msg;
}

static const struct rproc_ops riscv_sbi_hsm_rproc_ops = {
	.prepare = riscv_sbi_hsm_rproc_prepare,
	.handle_rsc = riscv_sbi_hsm_rproc_handle_rsc,
	.start = riscv_sbi_hsm_rproc_start,
	.stop = riscv_sbi_hsm_rproc_stop,
	.kick = riscv_sbi_hsm_rproc_kick,
};

static irqreturn_t riscv_sbi_hsm_rproc_irq(int irq, void *dev_id)
{
	struct rproc *rproc = dev_id;
	struct riscv_sbi_hsm_rproc *priv = rproc->priv;

	schedule_work(&priv->irq_work);

	return IRQ_HANDLED;
}

static void riscv_sbi_hsm_rproc_irq_work(struct work_struct *work)
{
	struct riscv_sbi_hsm_rproc *priv;

	priv = container_of(work, struct riscv_sbi_hsm_rproc, irq_work);

	rproc_vq_interrupt(priv->rproc, 0);
	rproc_vq_interrupt(priv->rproc, 1);
}

static void riscv_sbi_hsm_rproc_free_msis(void *data)
{
	struct device *dev = data;

	platform_device_msi_free_irqs_all(dev);
}

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

	ret = devm_request_irq(dev, priv->irq, riscv_sbi_hsm_rproc_irq,
			       IRQF_SHARED, "MSI", rproc);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to set up MSI\n");

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
