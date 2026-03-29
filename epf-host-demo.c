#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pci-epf.h>

#define VENDOR_ID 0x1234
#define DRIVER_ID 0xDEAD
#define EPF_DEMO_BAR_SIZE 4096

enum BAR0_REG_OFF {
    REG_MAGIC     = 0x00,
    REG_DOORBELL  = 0x04,
    HOST_SIZE     = 0x08,
    EP_SIZE       = 0x0C,
};

struct epf_host {
    struct pci_dev* pdev;
    void __iomem* bar0;
    void __iomem* bar1;
    void __iomem* bar2;
};

irqreturn_t epf_host_irq_handler(int irq, void *data)
{
    struct epf_host *priv = data;
    char *buffer;
    u32 size;

    pr_info("epf-host: Received IRQ %d\n", irq);

    size = readl(priv->bar0 + EP_SIZE);
    size = min_t(u32, size, EPF_DEMO_BAR_SIZE - 1);
    pr_info("epf-host: Received Message Size: %u\n", size);

    if (!size) {
        pr_warn("epf-host: EP sent size=0, ignoring\n");
        return IRQ_HANDLED;
    }

    buffer = kmalloc(ALIGN(size + 1, 4), GFP_ATOMIC);
    if (!buffer) {
        pr_err("epf-host: kmalloc() failed\n");
        return IRQ_HANDLED;
    }
    
    memset(buffer, 0, ALIGN(size + 1, 4));

    memcpy_fromio(buffer, priv->bar2, ALIGN(size, 4));
    buffer[size] = '\0';

    pr_info("epf-host: EP message: %s\n", buffer);

    kfree(buffer);
    return IRQ_HANDLED;
}

static int epf_host_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct epf_host* priv;
    int err;
    u32 magic;

    pr_info("epf-host: probe %s\n", pci_name(pdev));

    /* 1. Enable the device, interrupt, bar, device ready */
    err = pci_enable_device(pdev);
    if (err) {
        pr_err("epf-host: pci_enable_device() failed: %d\n", err);
        return err;
    }

    /* 2. Set Master */
    pci_set_master(pdev);
    
    /* 3. Request Region: So that no other device will use the address region */
    err = pci_request_regions(pdev, "epf-host");
    if (err) {
        pr_err("epf-host: pci_request_region() failed: %d\n", err);
        goto err_disable;
    }

    /* 4. Host Logic */

    /* ! Allocate the host struct */
    priv = devm_kzalloc(&pdev->dev,sizeof(*priv), GFP_KERNEL);
    if (!priv) {
        pr_err("epf_host: devm_kzalloc() failed\n");
        return -ENOMEM;
    }

    priv->pdev = pdev;

    /* map physical address to virtual address */
    priv->bar0 = pcim_iomap(pdev, BAR_0, 0);
    priv->bar1 = pcim_iomap(pdev, BAR_1, 0);
    priv->bar2 = pcim_iomap(pdev, BAR_2, 0);
    if (IS_ERR(priv->bar0) || IS_ERR(priv->bar1) || IS_ERR(priv->bar2)) {
        dev_err(&pdev->dev, "pcim_iomap() failed\n");
        err = -ENOMEM;
        goto err_release;
    }

    /* Extra: Register Interrupt handler */
    err = request_irq(pdev->irq, epf_host_irq_handler, IRQF_SHARED,
                      "epf-host", priv);
    if (err) {
        dev_err(&pdev->dev, "request_irq() failed: %d\n",err);
        goto err_release;
    }


    magic = readl(priv->bar0 + REG_MAGIC);
    if (magic == 0xcafebabe)
        pr_info("epf-host: magic value verified\n");
    else
        pr_warn("epf-host: unexpected magic value\n");
    
    pr_info("epf-host: probe is completed\n");


    /* Test on doorbell */
    char* msg = "Host write message to BAR 1, check it out\n";
    u8 padded[256] = {0};
    size_t len = strlen(msg) + 1;
    memcpy(padded, msg, len);

    memcpy_toio(priv->bar1, padded, ALIGN(len, 4)); 

    writel(strlen(msg) + 1, priv->bar0 + HOST_SIZE);
    writel(1, priv->bar0 + REG_DOORBELL);

    return 0;

err_release:
    pci_release_regions(pdev);
err_disable:
    pci_disable_device(pdev);
    return err;

}

static void epf_host_remove(struct pci_dev *pdev)
{
    struct epf_host* priv = dev_get_drvdata(&pdev->dev);
    pr_info("epf-host: pci device remove\n");

    free_irq(pdev->irq, priv);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
}

static struct pci_device_id epf_host_id_table[] = {
    {PCI_DEVICE(VENDOR_ID, DRIVER_ID)},
    {}
};

static struct pci_driver epf_host_driver = {
    .name       = "epf-host",
    .probe      = epf_host_probe,
    .remove     = epf_host_remove,
    .id_table   = epf_host_id_table,
};

MODULE_DEVICE_TABLE(pci, epf_host_id_table);
module_pci_driver(epf_host_driver);

MODULE_DESCRIPTION("QEMU Host side driver");
MODULE_AUTHOR("Elton Wong");
MODULE_LICENSE("GPL");