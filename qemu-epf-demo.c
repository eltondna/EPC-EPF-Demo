// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/types.h>
#include <linux/pci-epf.h>
#include <linux/pci-epc.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#define EPF_DEMO_VENDOR_ID 0xd1d1 
#define EPF_DEMO_DEVICE_ID 0x1234
#define EPF_DEMO_MAGIC     0xcafebabe

#define EPF_DEMO_BAR_NUM   3
#define EPF_DEMO_BAR_SIZE  4096

enum BAR0_OFF {
    BAR0_MAGIC    = 0x00,
    BAR0_DOORBELL = 0x04,
    BAR0_HOSTSIZE = 0x08,
    BAR0_EPSIZE   = 0x0C,
};

struct qemu_epf_data {
    struct pci_epf* epf;
    void * bar[EPF_DEMO_BAR_NUM];

    struct task_struct* kthr;
    int kthr_stop;
};

static int doorThread(void* data)
{
    struct qemu_epf_data* priv = (struct qemu_epf_data*) data;
    u32 m_size = 0;
    char* m_buf = devm_kzalloc(&priv->epf->dev, EPF_DEMO_BAR_SIZE, GFP_KERNEL);
    if (!m_buf) {
        pr_err("qemu-epf-demo: devm_kzalloc() failed\n");
        return -ENOMEM;
    }
    memset(m_buf, 0, EPF_DEMO_BAR_SIZE);

    while (!READ_ONCE(priv->kthr_stop)) {
        u32 ring = READ_ONCE(*(u32 *)(priv->bar[BAR_0] + BAR0_DOORBELL));
        if (ring) {
            /* Receive Host DoorBell */
            pr_info("qemu-epf-demo: Receive doorbell\n");
            
            m_size = READ_ONCE(*(u32 *)(priv->bar[BAR_0] + BAR0_HOSTSIZE));
            if (!m_size) {
                pr_err("qemu-epf-demo: Host message Size is 0\n");
                WRITE_ONCE(*(u32*)(priv->bar[BAR_0] + BAR0_DOORBELL), 0);
                continue;
            }

            /* Host Message Sanity Check */
            pr_info("qemu-epf-demo: Host message size: %u", m_size);
            m_size = min_t(u32, m_size, EPF_DEMO_BAR_SIZE);

            memcpy(m_buf, priv->bar[BAR_1], m_size);
            memset(priv->bar[BAR_1], 0, EPF_DEMO_BAR_SIZE);

            m_buf[m_size-1] = '\0'; 
            pr_info("qemu-epf-demo: Host Message:%s\n", m_buf);

            /* Echo Back */
            WRITE_ONCE(*(u32*)(priv->bar[BAR_0] + BAR0_EPSIZE), m_size);

            memcpy(priv->bar[2], m_buf, m_size);
            wmb();

            WRITE_ONCE(*(u32*)(priv->bar[BAR_0] + BAR0_DOORBELL), 0);
            pci_epc_raise_irq(priv->epf->epc, priv->epf->func_no, 
                              priv->epf->vfunc_no, PCI_EPC_IRQ_LEGACY,
                              PCI_INTERRUPT_INTA);
            pr_info("qemu-epf-demo: Raise Interrupt\n");
        }
        msleep(5);
    }

    pr_info("qemu-epf-demo: doorbell thread exited.\n");
    return 0;
}

static struct pci_epf_header qemu_epf_header = {
    .vendorid = EPF_DEMO_VENDOR_ID,
	.deviceid = EPF_DEMO_DEVICE_ID,
    .interrupt_pin = PCI_INTERRUPT_INTA,
};

static int qemu_epf_bind(struct pci_epf *epf)
{
    struct qemu_epf_data* priv = epf_get_drvdata(epf);
    struct pci_epc* epc = epf->epc;
    int i, ret = 0;

    if (!priv) {
        pr_info("qemu-epf-demo: drive data is NULL\n");
        return -ENODATA;
    }

    /* Write Device Header */
    ret = pci_epc_write_header(epc, epf->func_no, epf->vfunc_no, &qemu_epf_header);
    if (ret) {
        pr_err("qemu-epf-demo: write_header() failed\n");
        return ret;
    }

    /* Allocate EPF BAR Space */
    for (i = 0; i < EPF_DEMO_BAR_NUM; i++) {
        priv->bar[i] = pci_epf_alloc_space(epf, EPF_DEMO_BAR_SIZE, i, 0, PRIMARY_INTERFACE);
        if (!priv->bar[i]) {
            ret = -ENOMEM;
            goto err_free_space;
        }
    }

    /* Set Bar */
    *(u32*) priv->bar[0] = EPF_DEMO_MAGIC;
    
    for (i = 0; i < EPF_DEMO_BAR_NUM; i++) {
        struct pci_epf_bar* epf_bar = &epf->bar[i];
        ret = pci_epc_set_bar(epc, epf->func_no, epf->vfunc_no, epf_bar);
        if (ret)
            goto err_free_space;
    }

    /* Run Kernel Thread */
    priv->kthr = kthread_run(doorThread, (void*) priv, "dbThread");
    if (IS_ERR(priv->kthr)) {
        pr_err("qemu-epf-demo: Failed to create thread (error %ld)\n", PTR_ERR(priv->kthr));
        return PTR_ERR(priv->kthr);
    }

    pr_info("qemu-epf-demo: Thread created successfully (PID %d)\n", task_pid_nr(priv->kthr));
    return 0;

err_free_space:
    --i;
    for (; i >=0; i--)
        pci_epf_free_space(epf, priv->bar[i], i, PRIMARY_INTERFACE);
    return ret;
}

static void qemu_epf_unbind(struct pci_epf *epf)
{
    struct qemu_epf_data* priv = epf_get_drvdata(epf);

    WRITE_ONCE(priv->kthr_stop, 1);
    kthread_stop(priv->kthr);

    for (int i = 0; i < EPF_DEMO_BAR_NUM; i++)
        pci_epf_free_space(epf, priv->bar[i], i, PRIMARY_INTERFACE);
}

static struct pci_epf_ops qemu_epf_pci_epf_ops = {
    .bind = qemu_epf_bind,
    .unbind = qemu_epf_unbind,
};

static int qemu_epf_probe(struct pci_epf *epf)
{
    struct qemu_epf_data* priv;
    
    priv = devm_kzalloc(&epf->dev, sizeof(struct qemu_epf_data), GFP_KERNEL);
    if (!priv) {
        pr_info("qemu-epf-demo: devm_kzalloc() failed\n");
        return -ENOMEM;
    }

    priv->epf = epf;
    priv->kthr_stop = 0;
    epf_set_drvdata(epf, priv);

    epf->driver = qemu_epf_header;
    pr_info("qemu-epf-demo: probe() completed\n");
    return 0;
}

static const struct pci_epf_device_id qemu_epf_ids[] = {
    { .name = "qemu-epf-demo" },
    { },
};


static struct pci_epf_driver qemu_epf_driver = {
    .driver.name = "qemu-epf-demo",
    .probe  = qemu_epf_probe,
    .ops = &qemu_epf_pci_epf_ops,
    .owner = THIS_MODULE,
    .id_table = qemu_epf_ids,
};

static int __init qemu_epf_init(void)
{
    int err;
    err = pci_epf_register_driver(&qemu_epf_driver);

    if (err) {
        pr_err("qemu-epf-demo: Register EPF driver failed: %d\n", err);
        return err;
    }

    pr_info("qemu-epf-demo: init() called\n");
    return 0;
}

static void __exit qemu_epf_exit(void)
{
    pci_epf_unregister_driver(&qemu_epf_driver);
    pr_info("qemu-epf-demo: exit() called\n");
}

module_init(qemu_epf_init);
module_exit(qemu_epf_exit);
MODULE_DESCRIPTION("Simple PCI EPF demo driver");
MODULE_AUTHOR("Elton Wong Ming Yan");
MODULE_LICENSE("GPL v2");