// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci-epc.h>
#include <linux/pci-epf.h>
#include <linux/pci_ids.h>
#include <linux/kthread.h>
#include <linux/delay.h>


#define EPF_DEMO_VENDOR_ID 0x1234
#define EPF_DEMO_DEVICE_ID 0xDEAD

#define EPF_DEMO_BAR0       BAR_0
#define EPF_DEMO_BAR1       BAR_1
#define EPF_DEMO_BAR2       BAR_2

#define EPF_DEMO_BAR_SIZE   4096

// ! Represent a PCI Configuration Space
static struct pci_epf_header epf_demo_header = {
    .vendorid = EPF_DEMO_VENDOR_ID,
    .deviceid = EPF_DEMO_DEVICE_ID,
    .baseclass_code = PCI_CLASS_OTHERS,
    .interrupt_pin = PCI_INTERRUPT_INTA,
};

struct epf_demo {
    struct pci_epf* epf;
    struct task_struct* epf_thread;

    void* bar_buf[3];
};

enum EPF_DEMO_BAR_REG_OFF {
    BAR0_REG_MAGIC     = 0x00,
    BAR0_REG_DOORBELL  = 0x04,
    BAR0_HOST_SIZE     = 0x08,
    BAR0_EP_SIZE       = 0x0C,
};

void handle_host_request(struct epf_demo * demo)
{
    u32 size = readl(demo->bar_buf[EPF_DEMO_BAR0] + BAR0_HOST_SIZE);
    pr_info("qemu-epf-demo: Receive doorbell from Host, size=%u\n", size);

    /* Read Bar 1 Content*/
    size = min_t(u32, EPF_DEMO_BAR_SIZE-1, size);
    ((char *)demo->bar_buf[1])[size] = '\0';

    pr_info("qemu-epf-demo: Bar1 content:\n");
    pr_info("%s\n", (char *)demo->bar_buf[1]);


    /* Userspace Logic */
    char *alphaBuf = kmalloc(size + 1, GFP_KERNEL);  // ✅
    if (!alphaBuf) {
        pr_err("kmalloc() failed\n");
        return;
    }
    alphaBuf[size] = '\0';

    for (int i = 0; i < size; i++) {
        char ch = ((char *)demo->bar_buf[EPF_DEMO_BAR1])[i];
        if ( ch >= 'A' && ch <= 'Z')
            alphaBuf[i] = ch + 32;
        else if (ch >= 'a' && ch <= 'z')
            alphaBuf[i] = ch - 32;
        else alphaBuf[i] = ch;
    }


    pr_info("qemu-epf-demo: Send Message size: %u\n", size);
    writel(size, demo->bar_buf[EPF_DEMO_BAR0] + BAR0_EP_SIZE); 
    memcpy_toio(demo->bar_buf[EPF_DEMO_BAR2], alphaBuf, ALIGN(size, 4)); 
    kfree(alphaBuf);
    return;
}

static int epf_demo_thread(void *data)
{
    struct epf_demo* demo = data;
    u32 doorbell;
    u32 size;

    pr_info("qemu-epf-demo: Doorbell polling thread started\n");
    void *bar0_buf = demo->bar_buf[0];

    while (!kthread_should_stop()) {
        doorbell = readl(bar0_buf + BAR0_REG_DOORBELL);
        if (doorbell) {
            handle_host_request(demo);

            /* Raise IRQ */
            writel(0, bar0_buf + BAR0_REG_DOORBELL);

            pci_epc_raise_irq(demo->epf->epc, demo->epf->func_no,
                              demo->epf->vfunc_no, PCI_EPC_IRQ_LEGACY, 1);
            pr_info("epf-demo: IRQ raised!\n");
        }
        msleep(5);
    }
    pr_info("qemu-epf-demo: thread stopped\n");
    return 0;
}

/* Bind - called when the EPF is bounded to an EPC 
   This is where we set up the BAR0
*/
static int epf_demo_bind(struct pci_epf *epf)
{
    struct epf_demo *demo = epf_get_drvdata(epf);
    struct pci_epc *epc = epf->epc;
    struct pci_epf_bar* epf_bar;
    int err;
    int i;

    pr_info("qemu-epf-demo: bind is called\n");

    /* Allocate 4KB Buffer for all Bar buffer */
    for (i = EPF_DEMO_BAR0; i <= EPF_DEMO_BAR2; i++) {
        demo->bar_buf[i] = pci_epf_alloc_space(epf, EPF_DEMO_BAR_SIZE, i, 0, PRIMARY_INTERFACE);
        if (!demo->bar_buf[i]) {
            pr_err("qemu-epf-demo: pci_epf_alloc_space() failed\n");
            goto err_bar_alloc;
        }
    }

    /* Magic Value */
    *(uint32_t *)demo->bar_buf[0] = 0xcafebabe;

    /* EPC Side */
    err = pci_epc_write_header(epc, epf->func_no, epf->vfunc_no, &epf_demo_header);
    if (err) {
        pr_err("qemu-epf-demo: pci_epc_write_header() failed to write header: %d\n", err);
        return err;
    }

    /* Set BAR */
    for (i = EPF_DEMO_BAR0; i <= EPF_DEMO_BAR2; i++) {
        epf_bar = &epf->bar[i];
        err = pci_epc_set_bar(epc, epf->func_no, epf->vfunc_no, epf_bar);
        if (err) {
            pr_err("qemu-epf-demo: pci_epc_set_bar() failed to set BAR: %d\n", err);
            goto err_set_bar;
        }
        pr_info("qemu-epf-demo: BAR%i size: %u\n", i, epf_bar->size);
    }
    pr_info("qemu-epf-demo: bind is completed\n");
    
    /* Polling thread for doorbell mechanism: Host notice EP */
    demo->epf_thread = kthread_run(epf_demo_thread, demo, "epf-demo");
    if (IS_ERR(demo->epf_thread)) {
        pr_err("qemu-epf-demo: kthread_run() failed:\n");
        err = PTR_ERR(demo->epf_thread);
        goto err_set_bar;
    }
    return 0;

err_set_bar:
    i--;
    for (; i >= 0; i--) {
        pci_epc_clear_bar(epc, epf->func_no, epf->vfunc_no, &epf->bar[i]);
        pci_epf_free_space(epf, demo->bar_buf[i], i, PRIMARY_INTERFACE);
    }
    return err;

err_bar_alloc:
    i--;
    for (; i >= 0; i--)
        pci_epf_free_space(epf, demo->bar_buf[i], i, PRIMARY_INTERFACE);
    return -ENOMEM;
}
/* unbind - callled when EPF is unbound from EPC */
static void epf_demo_unbind(struct pci_epf *epf)
{
    struct epf_demo* demo = epf_get_drvdata(epf);
    struct task_struct* qemu_thread = demo->epf_thread;
    struct pci_epc* epc = epf->epc;

    pr_info("qemu-epf-demo: unbind is called\n");
    kthread_stop(demo->epf_thread);
    demo->epf_thread = NULL;

    for (int i = EPF_DEMO_BAR0; i <= EPF_DEMO_BAR2; i++) {
        pci_epc_clear_bar(epc, epf->func_no, epf->vfunc_no, &epf->bar[i]);
        pci_epf_free_space(epf, demo->bar_buf[i], i, PRIMARY_INTERFACE);
    }
}

static struct pci_epf_ops epf_demo_ops = {
    .bind = epf_demo_bind,
    .unbind = epf_demo_unbind,
};

/* Probe : called when device and the driver meet each other.

    1. Create a device through: 
        mkdir /sys/kernel/config/pci_ep/functions/qemu-epf-demo/func1
                                                  ^^^^^^^^^^^^^
                                           this part is the driver name   
    2. Install our driver:
            insmod qemu-epf-demo.ko
            module_init() -> pci_epf_register_driver(&epf_demo_driver) -> driver_register()
    
    Note that both of them trigger a bus scan.
    Device side, it scans whether there is a driver called "qemu-epf-demo"
    Driver side, it scans whether there is a device called "qemu-epf-demo"

*/

static int epf_demo_probe(struct pci_epf* epf)
{
    struct epf_demo* demo;
    pr_info("qemu-epf-demo: probe is called!\n");

    // ! Allocate memory
    // ! devm version : Automatically released when the device is removed
    demo = devm_kzalloc(&epf->dev, sizeof(*demo), GFP_KERNEL);
    if (demo == NULL) {
        pr_info("qemu-epf-demo: devm_kzalloc() failed\n");
        return -ENOMEM;
    }

    // ! Set header & Save driver private info --> for later bind use
    demo->epf = epf;
    epf->header = &epf_demo_header;
    epf_set_drvdata(epf, demo);

    return 0;
}

/* ! Driver Setup */ 

static struct pci_epf_device_id epf_demo_ids[] = {
    {.name = "qemu-epf-demo"},
    {}
};

static struct pci_epf_driver epf_demo_driver = {
    .driver.name = "qemu-epf-demo",
    .probe       = epf_demo_probe,
    .ops         = &epf_demo_ops,
    .owner       = THIS_MODULE,
    .id_table    = epf_demo_ids     // Use id table to match first, if no match then use the name
};

static int __init epf_demo_init(void)
{

    // ! Register the driver 
    int err;
    err = pci_epf_register_driver(&epf_demo_driver);
 
    if (err) {
        pr_info("qemu-epf-demo: pci_epf_register_driver() failed\n");
        return err;
    }

    pr_info("qemu-epf-demo: Driver is registered\n");
    return 0;
}

static void __exit epf_demo_exit(void)
{
    pci_epf_unregister_driver(&epf_demo_driver);
    pr_info("Driver is removed\n");
}

module_init(epf_demo_init);
module_exit(epf_demo_exit);

MODULE_DESCRIPTION("Simple PCI EPF demo driver");
MODULE_AUTHOR("Elton Wong");
MODULE_LICENSE("GPL");