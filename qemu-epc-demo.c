// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/pci-epc.h>
#include <linux/io.h>
#include <linux/types.h>

/* TODO : Register release function for dev, research on the BackPort */

#define VENDOR_ID              0x1b36
#define DEVICE_ID              0x0013
#define QEMU_EPC_DEVICE_NAME   "qemu-epc"

/* BAR Config Space */
#define QEMU_EPC_BAR_CTRL      0
#define QEMU_EPC_BAR_PCI_CFG   1
#define QEMU_EPC_BAR_BAR_CFG   2
#define QEMU_EPC_BAR_OB_WINDOW 3

/* CTRL register offset */
#define QEMU_EPC_CTRL_OFF_START       0x00
#define QEMU_EPC_CTRL_OFF_WIN_START   0x08
#define QEMU_EPC_CTRL_OFF_WIN_SIZE    0x10
#define QEMU_EPC_CTRL_OFF_IRQ_TYPE    0x18
#define QEMU_EPC_CTRL_OFF_IRQ_NUM     0x1c
#define QEMU_EPC_CTRL_OFF_OB_MAP_MASK 0x20
#define QEMU_EPC_CTRL_OFF_OB_IDX      0x24
#define QEMU_EPC_CTRL_OFF_OB_MAP_PHYS 0x28
#define QEMU_EPC_CTRL_OFF_OB_MAP_PCI  0x30
#define QEMU_EPC_CTRL_OFF_OB_MAP_SIZE 0x38

/* BAR_CFG Register offset */
#define QEMU_EPC_BAR_CFG_OFF_MASK      0x00
#define QEMU_EPC_BAR_CFG_OFF_NUMBER    0x01
#define QEMU_EPC_BAR_CFG_OFF_FLAGS     0x02
#define QEMU_EPC_BAR_CFG_OFF_RSV       0x04
#define QEMU_EPC_BAR_CFG_OFF_PHYS_ADDR 0x08
#define QEMU_EPC_BAR_CFG_OFF_SIZE      0x10
#define QEMU_EPC_BAR_CFG_SIZE          0x18

struct qemu_epc_data {
    struct pci_dev* dev;
    struct pci_epc* epc;

    void __iomem *ctrl_region;
    void __iomem *pci_cfg_region;
    void __iomem *bar_cfg_region;
    void __iomem *ob_win_region;
};

static int qemu_epc_start(struct pci_epc *epc)
{
    struct qemu_epc_data* priv = epc_get_drvdata(epc);
    writel(1, priv->ctrl_region + QEMU_EPC_CTRL_OFF_START);

    pr_info("qemu-epc-demo: Device start!\n");
    return 0;
}

static int qemu_epc_write_header(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
                        struct pci_epf_header *hdr)
{
    u16 header[2] = {hdr->vendorid, hdr->deviceid};
    u8 revise[3] = {hdr->revid, hdr->baseclass_code, hdr->subclass_code};
    u8 int_pin = hdr->interrupt_pin;
    struct qemu_epc_data* priv = epc_get_drvdata(epc);

    memcpy_toio(priv->pci_cfg_region + PCI_VENDOR_ID, header, sizeof(header));
    memcpy_toio(priv->pci_cfg_region + PCI_REVISION_ID, revise, sizeof(revise));
    memcpy_toio(priv->pci_cfg_region + PCI_INTERRUPT_PIN, &int_pin, sizeof(int_pin));

    pr_info("qemu-epc-demo: write_header(): vendorId: %x headerId: %x\n", header[0], header[1]);
    return 0;
}


static int qemu_epc_set_bar(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
                   struct pci_epf_bar *epf_bar)
{
    struct qemu_epc_data* priv = epc_get_drvdata(epc);

    writeb(epf_bar->barno, priv->bar_cfg_region + QEMU_EPC_BAR_CFG_OFF_NUMBER);
    writeb(epf_bar->flags, priv->bar_cfg_region + QEMU_EPC_BAR_CFG_OFF_FLAGS);

    writel(lower_32_bits(epf_bar->phys_addr), priv->bar_cfg_region + QEMU_EPC_BAR_CFG_OFF_PHYS_ADDR);
    writel(upper_32_bits(epf_bar->phys_addr), priv->bar_cfg_region + QEMU_EPC_BAR_CFG_OFF_PHYS_ADDR + 4);

    writel(lower_32_bits(epf_bar->size), priv->bar_cfg_region + QEMU_EPC_BAR_CFG_OFF_SIZE);
    writel(upper_32_bits(epf_bar->size), priv->bar_cfg_region + QEMU_EPC_BAR_CFG_OFF_SIZE + 4);
    

    u8 mask = readb(priv->bar_cfg_region + QEMU_EPC_BAR_CFG_OFF_MASK);
    mask|= 1 << epf_bar->barno;
    writeb(mask, priv->bar_cfg_region + QEMU_EPC_BAR_CFG_OFF_MASK);

    pr_info("qemu-epc-demo: set_bar() Bar%d, phys_addr: %pa, size: %lu\n", 
            epf_bar->barno,
            &epf_bar->phys_addr,
            epf_bar->size);

    return 0;
}

static int qemu_epc_raise_irq(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			         enum pci_epc_irq_type type, u16 interrupt_num)
{
    struct qemu_epc_data* priv = epc_get_drvdata(epc);
    
    writel(type, priv->ctrl_region + QEMU_EPC_CTRL_OFF_IRQ_TYPE);
    writel(interrupt_num & 0xffff, priv->ctrl_region + QEMU_EPC_CTRL_OFF_IRQ_NUM);

    pr_info("qemu-epc-demo: raise_irq(): INT %u\n", interrupt_num);
    return 0;
}

static const struct pci_epc_ops qemu_epc_ops = {
    .write_header = qemu_epc_write_header,
    .set_bar      = qemu_epc_set_bar,
    .raise_irq    = qemu_epc_raise_irq,
    .start        = qemu_epc_start
    
};

static int qemu_epc_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
    struct qemu_epc_data* priv;
    int ret;
    void __iomem **iotbl;

    priv = devm_kzalloc(&dev->dev, sizeof(struct qemu_epc_data), GFP_KERNEL);
    if (!priv) {
        pr_err("qemu-epc-demo: devm_kzalloc() failed\n");
        return -ENOMEM;
    }

    if ((ret = pcim_enable_device(dev))) {
        pr_err("qemu-epc-demo: pcim_enable_device() failed\n");
        return ret;
    }

    pci_set_master(dev);
    ret = pcim_iomap_regions(dev,
                            BIT(QEMU_EPC_BAR_CTRL) |
                            BIT(QEMU_EPC_BAR_BAR_CFG) |
                            BIT(QEMU_EPC_BAR_PCI_CFG) |
                            BIT(QEMU_EPC_BAR_OB_WINDOW),
                            QEMU_EPC_DEVICE_NAME);
    if (ret) {
        pr_err("qemu-epc-demo: Failed to map region: %d\n", ret);
        return ret;
    }

    iotbl = pcim_iomap_table(dev);
    priv->ctrl_region = iotbl[QEMU_EPC_BAR_CTRL];
    priv->bar_cfg_region = iotbl[QEMU_EPC_BAR_BAR_CFG];
    priv->pci_cfg_region = iotbl[QEMU_EPC_BAR_PCI_CFG];
    priv->ob_win_region = iotbl[QEMU_EPC_BAR_OB_WINDOW];

    pr_info("qemu-epc-demo: BAR Virtual Address\n"
            "ctrl: %px\nbar_cfg: %px\npci_cfg: %px\nob_win: %px\n",
            priv->ctrl_region,
            priv->bar_cfg_region,
            priv->pci_cfg_region,
            priv->ob_win_region);

    if (!(priv->ctrl_region) || !(priv->bar_cfg_region) ||
        !(priv->pci_cfg_region) || !(priv->ob_win_region)) {
        ret = -ENOMEM;
        return ret;
    }

    priv->epc = devm_pci_epc_create(&dev->dev, &qemu_epc_ops);
    if (IS_ERR(priv->epc)) {
        ret = PTR_ERR(priv->epc);
        pr_err("qemu-epc-demo: Failed to create pci_epc: %d\n", ret);
        return ret;
    }

    priv->epc->max_functions = 1;
    priv->dev = dev;
    epc_set_drvdata(priv->epc, (void*) priv);
    dev_set_drvdata(&dev->dev, (void*) priv);
    
    pr_info("qemu-epc-demo: EPC Driver probe()\n");
    return 0;
}

static void qemu_epc_remove(struct pci_dev *dev)
{
    pr_info("qemu-epc-demo: EPC Driver remove()\n");
}

static struct pci_device_id qemu_epc_id_table[] = {
    { PCI_DEVICE(VENDOR_ID, DEVICE_ID) },
    { },
};

static struct pci_driver qemu_epc_driver = {
    .name   = QEMU_EPC_DEVICE_NAME,
    .probe  = qemu_epc_probe,
    .remove = qemu_epc_remove,
    .id_table = qemu_epc_id_table,
};

module_pci_driver(qemu_epc_driver);
MODULE_DEVICE_TABLE(pci, qemu_epc_id_table);
MODULE_AUTHOR("Elton Wong Ming Yan");
MODULE_DESCRIPTION("EPC driver for Qemu-epc device");
MODULE_LICENSE("GPL v2");