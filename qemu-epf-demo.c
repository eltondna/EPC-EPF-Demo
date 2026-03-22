// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/module.h>
#include <linux/pci-epc.h>
#include <linux/pci-epf.h>
#include <linux/io.h>
#include <linux/delay.h>

/* QEMU EPC PCI IDs */
#define VENDOR_ID       0x1b36
#define DEVICE_ID       0x0013

/* BAR indices */
#define BAR_CTRL        0
#define BAR_PCI_CFG     1
#define BAR_BAR_CFG     2
#define BAR_OB_WIN      3

/* CTRL Register offsets */
#define CTRL_OFF_START          0x00
#define CTRL_OFF_WIN_START      0x08
#define CTRL_OFF_WIN_SIZE       0x10
#define CTRL_OFF_IRQ_TYPE       0x18
#define CTRL_OFF_IRQ_NUM        0x1c
#define CTRL_OFF_OB_MAP_MASK    0x20
#define CTRL_OFF_OB_IDX         0x24
#define CTRL_OFF_OB_MAP_PHYS    0x28
#define CTRL_OFF_OB_MAP_PCI     0x30
#define CTRL_OFF_OB_MAP_SIZE    0x38

/* BAR Register offsets */
#define BAR_CFG_OFF_MASK        0x00
#define BAR_CFG_OFF_NUMBER      0x01
#define BAR_CFG_OFF_FLAG        0x02
#define BAR_CFG_OFF_PHYS_ADDR   0x08
#define BAR_CFG_OFF_SIZE        0x10

#define OB_WINDOW_SIZE          0x100000
#define NUM_OB_MAPS             32

struct qemu_epc {
    struct pci_dev *pdev;
    struct pci_epc *epc;

    void __iomem *ctrl_region;
    void __iomem *pci_cfg_region;
    void __iomem *bar_cfg_region;
    void __iomem *ob_window_region;

    phys_addr_t ob_window_phys;
};

/* Internal Function Interface */
static void qemu_epc_ctrl_start(struct qemu_epc *priv);
static void qemu_epc_stop(struct pci_epc* epc);

static void qemu_epc_write_pci_cfg_region(struct qemu_epc * priv, u16 offset,
                                   void* buf, size_t size);
static void qemu_epc_read_pci_cfg_region(struct qemu_epc* priv, u16 offset,
                                  void* buf, size_t size);



/* CTRL Helper */
static void qemu_epc_ctrl_start(struct qemu_epc *priv)
{
    writel(1, priv->ctrl_region + CTRL_OFF_START);
    
    msleep(100);
    pr_info("qemu-epc-demo: Qemu-EPC started\n");
}


static void qemu_epc_ctrl_stop(struct qemu_epc *priv)
{
    writel(0, priv->ctrl_region + CTRL_OFF_START);
    pr_info("qemu-epc-demo: Qemu-EPC stopped\n");
}


/* PCI CFG Helper */

static void qemu_epc_write_pci_cfg_region(struct qemu_epc * priv, u16 offset,
                                   void* buf, size_t size)
{
    /* Copy data to MMIO memory
        The function guarantee that write order is not reordered
        - By compiler
        - By CPU 
        - No cache

        This is important when writing to hardware register since
        write to specific register might trigger the action immediately
        
        1. Write to certain MMIO address 
        - Write physical address -> hardware stores the value, no action
        - Write Size -> hardware triggers BAR Setup

        2. DMA engine
        - Write Src addr -> just store
        - Write Dst addr -> just store
        - Write Size     -> just store
        - Write Start=1  -> Trigger DMA !
    */
    memcpy_toio(priv->pci_cfg_region + offset, buf, size);
}

static void qemu_epc_read_pci_cfg_region(struct qemu_epc* priv, u16 offset,
                                  void* buf, size_t size)
{
    memcpy_fromio(buf, priv->pci_cfg_region + offset, size);
}


/* PCI EPC Ops - called by the EPF Framework */
static int qemu_epc_write_header(struct pci_epc *epc, u8 func_no, 
                                 u8 vfunc_no, struct pci_epf_header *hdr)
{
    struct qemu_epc* priv = epc_get_drvdata(epc);
    u16 vendor_device[2] = {
        hdr->vendorid,
        hdr->deviceid,
    };
    u8 rev_class[3] = {
        hdr->revid,
        hdr->baseclass_code,
        hdr->subclass_code
    };

    pr_info("qemu-epc-demo: Write Header: VendorId:0x%x DeviceId: 0x%lx\n",
            vendor_device[0], vendor_device[1]);
    
    qemu_epc_write_pci_cfg_region(priv, PCI_VENDOR_ID, 
                           vendor_device, sizeof (vendor_device));
    qemu_epc_write_pci_cfg_region(priv, PCI_REVISION_ID,
                           rev_class, sizeof(rev_class));

    pr_info("qemu-epc-demo: interrupt_pin value=0x%x offset=0x%x size=%zu\n",
            hdr->interrupt_pin, PCI_INTERRUPT_PIN, sizeof(hdr->interrupt_pin));

    qemu_epc_write_pci_cfg_region(priv, PCI_INTERRUPT_PIN, 
                               &hdr->interrupt_pin, sizeof(hdr->interrupt_pin));
    
    return 0;
}
static int qemu_epc_set_bar(struct pci_epc *epc, u8 func_no,
                            u8 vfunc_no,struct pci_epf_bar *epf_bar)
{
    struct qemu_epc* priv = epc_get_drvdata(epc);
    u8 mask;
    pr_info("qemu-epc-demo: set bar: %d phys: 0x%llx size: 0x%llx\n",
            epf_bar->barno, epf_bar->phys_addr, epf_bar->size);

    // ! 1. Bar Number
    writeb(epf_bar->barno, priv->bar_cfg_region + BAR_CFG_OFF_NUMBER);
    // wmb();

    // ! 2. Bar flag
    writeb(epf_bar->flags, priv->bar_cfg_region + BAR_CFG_OFF_FLAG);

    // ! 3. BAR physical address (low then high)
    writel((u32)(epf_bar->phys_addr & 0xFFFFFFFF),
           priv->bar_cfg_region + BAR_CFG_OFF_PHYS_ADDR);
    writel((u32)(epf_bar->phys_addr >> 32),
           priv->bar_cfg_region + BAR_CFG_OFF_PHYS_ADDR + 4);

    // 4. BAR size (low then high)
    writel((u32)(epf_bar->size & 0xFFFFFFFF),
           priv->bar_cfg_region + BAR_CFG_OFF_SIZE);
    writel((u32)(epf_bar->size >> 32),
           priv->bar_cfg_region + BAR_CFG_OFF_SIZE + 4);
    
    // wmb(); 
    // ! 5. Update mask - Specifies which BAR region is set
    mask = readb(priv->bar_cfg_region + BAR_CFG_OFF_MASK);
    mask |= (1 << epf_bar->barno);
    writeb(mask, priv->bar_cfg_region + BAR_CFG_OFF_MASK);


    return 0;
}

static int qemu_epc_raise_irq(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			                  enum pci_epc_irq_type type, u16 interrupt_num)
{
    struct qemu_epc* priv = epc_get_drvdata(epc);
    pr_info("qemu-epc-demo: Raise IRQ %d\n", interrupt_num);

    writel(type, priv->ctrl_region + CTRL_OFF_IRQ_TYPE);
    writel(interrupt_num, priv->ctrl_region + CTRL_OFF_IRQ_NUM);
    
    return 0;
}

static int qemu_epc_start(struct pci_epc *epc)
{
    struct qemu_epc* priv = epc_get_drvdata(epc);

    pr_info("qemu-epc-demo: start\n");
    qemu_epc_ctrl_start(priv);
    return 0;
}

static void qemu_epc_stop(struct pci_epc* epc)
{
    struct qemu_epc* priv = epc_get_drvdata(epc);
    pr_info("qemu-epc-demo: stop\n");

    qemu_epc_ctrl_stop(priv);
}

static const struct pci_epc_ops qemu_epc_ops = {
    .start          = qemu_epc_start,
    .stop           = qemu_epc_stop,
    .write_header   = qemu_epc_write_header,
    .set_bar        = qemu_epc_set_bar,
    .raise_irq      = qemu_epc_raise_irq,
};


/* 
    PCI Driver probe / remove 
    Note: This is called when the kernel discover the "Qemu-EPC"
*/
static int qemu_epc_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
    struct qemu_epc* priv;
    struct pci_epc*  epc;
    int err;

    pr_info("qemu-epc-demo: probe %s\n", pci_name(dev));
    
    priv = devm_kzalloc(&dev->dev, sizeof(*priv), GFP_KERNEL);
    if (priv == NULL) {
        pr_debug("qemu-epc-demo: devm_kzalloc() failed\n");
        return -ENOMEM;
    }
    priv->pdev = dev;

    // ! Device is discovered, but everything is not set up in the device
    // ! 1. Therefore first thing is to enable the device
    //      - BARs accessible 
    //      - Interrupt working
    //      - Device ready to use
    err = pci_enable_device(dev);
    if (err) {
        dev_err(&dev->dev, "Failed to enable device: %d\n", err);
        return err;
    }

    // ! 2. Set the Bus Master Enable bit in PCI Config space command register
    // ! This allow the device initiate transaction to Host
    pci_set_master(dev);

    // ! 3. Claim ownership of the device BAR memory
    // ! So that no other driver besides "qemu-epc" could use that addresses
    //   calls request_mem_region() on each BAR
    //   -> register them in /proc/iomem as owned by "qemu-epc"

    err = pci_request_regions(dev, "qemu-epc");
    if (err) {
        dev_err(&dev->dev, "pci_request_regions() failed%d\n", err);
        goto err_disable;
    }

    /* Map all 4 BARS */
    priv->ctrl_region       = pcim_iomap(dev, BAR_CTRL, 0);
    priv->bar_cfg_region    = pcim_iomap(dev, BAR_BAR_CFG, 0);
    priv->pci_cfg_region    = pcim_iomap(dev, BAR_PCI_CFG, 0);
    priv->ob_window_region  = pcim_iomap(dev, BAR_OB_WIN, 0);

    if (!priv->ctrl_region || !priv->bar_cfg_region || !priv->pci_cfg_region
        || !priv->ob_window_region) {
        dev_err(&dev->dev, "Failed to map BAR\n");
        err = -ENOMEM;
        goto err_release;
    }

    // ! Get and store the physical address directly
    //   Since the outbound window is used for dma, 
    //   the dma engine work directly on physical address
    priv->ob_window_phys = pci_resource_start(dev, BAR_OB_WIN);
    pr_info("qemu-epc-demo: ctrl region addr (BAR 0): %p\n", priv->ctrl_region);
    pr_info("qemu-epc-demo: pci config region addr (BAR 1): %p\n", priv->pci_cfg_region);
    pr_info("qemu-epc-demo: bar config region addr (BAR 2): %p\n", priv->bar_cfg_region);
    pr_info("qemu-epc-demo: ob window region addr (BAR 3): %p\n", priv->ob_window_region);


    /* Create EPC Controller */
    epc = devm_pci_epc_create(&dev->dev, &qemu_epc_ops);
    if (IS_ERR(epc)) {
        err = PTR_ERR(epc);
        dev_err(&dev->dev, "devm_pci_epc_create() failed: %d\n", err);
        goto err_release;
    }

    priv->epc = epc;
    epc_set_drvdata(epc, priv);
    pci_set_drvdata(dev, priv);

    /* set EPC capabilities */
    epc->max_functions = 1;
    pr_info("qemu-epc-demo: EPC Controller is registered!\n");
    pr_info("qemu-epc-demo: Check /sys/kernel/config/pci_ep/controllers/\n");
    return 0;

err_release:
    pci_release_regions(dev);
err_disable:
    pci_disable_device(dev);
    return err;
}
static void qemu_epc_remove(struct pci_dev *dev)
{
    pr_info("qemu-epc-demo: Remove\n");
    pci_release_regions(dev);
    pci_disable_device(dev);
}

static struct pci_device_id qemu_epc_ids[] = {
    {PCI_DEVICE(VENDOR_ID, DEVICE_ID)},
    {}
};

// ! This expose the device ID table to userspace so udev can
// automatically load the driver when the device is detected.
// udev -> Hotplug

MODULE_DEVICE_TABLE(pci, qemu_epc_ids);

static struct pci_driver qemu_epc_driver = {
    .name     = "qemu-epc",
    .id_table = qemu_epc_ids,
    .probe    = qemu_epc_probe,
    .remove   = qemu_epc_remove,
};

// module_init / module_exit
module_pci_driver(qemu_epc_driver);

MODULE_DESCRIPTION("QEMU EPC kernel driver");
MODULE_AUTHOR("Elton Wong");
MODULE_LICENSE("GPL");