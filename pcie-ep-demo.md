# PCIe Endpoint Function Demo

## 1. Overview

This demo implements a bidirectional shared buffer communication channel between a Host and an Endpoint (EP) device over a QEMU-simulated PCIe bus. The EP advertises itself as a custom PCI device (Vendor ID `0x1234`, Device ID `0xDEAD`) and exposes three Base Address Registers (BARs) to the host. The demo application performs case inversion: the host sends a string to the EP, and the EP returns the string with uppercase and lowercase letters swapped.

### 1.1 Architecture

The system consists of four components running across two virtual machines connected by a QEMU-emulated PCIe bus:

| Component | Location | Role |
|-----------|----------|------|
| `elton-host-demo.ko` | Host VM | Standard PCI driver. Maps BARs, sends data via BAR1, reads EP response from BAR2 via IRQ handler. |
| `qemu-epf-demo.ko` | EP VM | EPF function driver. Allocates BAR buffers, polls doorbell, performs case inversion, raises IRQ. |
| `qemu-epc.ko` | EP VM | EPC controller driver. Bridges the Linux `pci_epc` framework to the QEMU virtual hardware via MMIO registers. |
| `epf-bridge` (QEMU) | QEMU | Virtual PCIe hardware. Translates MMIO writes into TLP packets and forwards them between the two VMs via a Unix socket. |

### 1.2 Communication Flow

The end-to-end flow for a single request/response cycle:

```
Host probe
  1. Host copies message into padded buffer
  2. memcpy_toio(BAR1, padded_buf, ALIGN(len, 4))   [TLP MW to EP]
  3. writel(len, BAR0 + HOST_SIZE)                  [TLP MW to EP]
  4. writel(1,   BAR0 + REG_DOORBELL)               [TLP MW to EP]

EP kthread (polling every 5ms)
  5. readl(BAR0 + REG_DOORBELL) == 1 -> process request
  6. Read message from bar_buf[1]  (direct RAM access, no TLP)
  7. Perform case inversion into alphaBuf
  8. writel(size, bar_buf[0] + BAR0_EP_SIZE)
  9. memcpy_toio(bar_buf[2], alphaBuf, ALIGN(size, 4))
 10. writel(0, bar_buf[0] + REG_DOORBELL)           <- clear doorbell
 11. pci_epc_raise_irq(PCI_EPC_IRQ_LEGACY, 1)       [TLP MSG INTA]

Host IRQ handler
 12. readl(BAR0 + EP_SIZE) -> size
 13. memcpy_fromio(buffer, BAR2, ALIGN(size, 4))    [TLP MR from EP]
 14. pr_info("EP message: %s", buffer)
```

> **Note:** All writes to host-side BAR mappings (`priv->bar0/1/2`) travel over the PCIe bus as TLP packets. On the EP side, `bar_buf[]` is ordinary kernel RAM allocated by `pci_epf_alloc_space()` and can be accessed directly via pointer.

### 1.3 BAR Layout

| BAR | Size | Direction | Purpose |
|-----|------|-----------|---------|
| BAR0 | 4 KB | Read/Write | Control registers: `REG_MAGIC` (0x00), `REG_DOORBELL` (0x04), `HOST_SIZE` (0x08), `EP_SIZE` (0x0C) |
| BAR1 | 4 KB | Host → EP | Data buffer: host writes message here before ringing doorbell |
| BAR2 | 4 KB | EP → Host | Response buffer: EP writes processed result here before raising IRQ |
---

## 2. The Linux PCIe Endpoint Framework

Many modern SoCs integrate PCIe controller IPs that can operate in both Root Complex and Endpoint mode. In Endpoint mode, the SoC presents itself as a PCIe device to an external host. Different controller IPs — such as DesignWare, Cadence, or a QEMU virtual controller — expose different internal register interfaces for programming the configuration space, setting up BARs, and raising interrupts.

Without a common framework, each endpoint function driver (NVMe, NIC, NTB, etc.) would need to be rewritten for every controller. A developer porting an NVMe endpoint from a DesignWare-based SoC to a Cadence-based SoC would have to rewrite the hardware programming even though the NVMe protocol logic is identical. This creates unnecessary code duplication and increases maintenance cost.

The Linux PCIe Endpoint Framework solves this by splitting the software
into three components:

**EPC (Endpoint Controller) library**

- Abstracts the hardware. Eachcontroller vendor provides an EPC driver that implements a standard set of operations (`pci_epc_ops`): writing the config header, configuring BARs, raising interrupts, and starting/stopping the link.

**EPF (Endpoint Function) library** 

- Implements the device logic. An EPF driver calls hardware-agnostic APIs like `pci_epc_set_bar()` and `pci_epc_raise_irq()`. The EPC core dispatches these calls through the ops vtable to the correct hardware-specific implementation. The EPF driver works unchanged across any controller.

**Configfs layer**

- Binds EPF devices to EPC controllers at runtime from userspace, without recompiling or modifying kernel code.

In the below section, we will explain briefly about each layers with the code example from our demo.


## 3. EPC Layer - Hardware Abstraction

**The EPC layer/driver only has 2 roles**:

1. Expose the set of standard functions (`pci_epc_ops`) to the EPF_layer
2. Implement the function detail (Hardware Configuration detail)


Before we introduce each aspect of the EPC layer, it would be clearer to introduce 
a bit about the demo underlying hardware: `qemu-epc`, which is the device that `qemu-epc-demo` driver directly talk to. Overall, the `qemu-epc` config space has 4 BARs, which are:
```c
// qemu-epc-demo.ko
#define BAR_CTRL        0
#define BAR_PCI_CFG     1
#define BAR_BAR_CFG     2
#define BAR_OB_WIN      3
```
and each region contains multiple registers at different offset, for example:

```c
/* BAR Register offsets */
#define BAR_CFG_OFF_MASK        0x00
#define BAR_CFG_OFF_NUMBER      0x01
#define BAR_CFG_OFF_FLAG        0x02
#define BAR_CFG_OFF_PHYS_ADDR   0x08
#define BAR_CFG_OFF_SIZE        0x10
```

The base address of each BAR is allocated by the Kernel at runtime. Therefore configuring the hardware here means writing certain value to certain registers in a certain BAR region, for example this is a function to start the `qemu-epc` device (which is called in the `start()`)

```c
static void qemu_epc_ctrl_start(struct qemu_epc *priv)
{
    writel(1, priv->ctrl_region + CTRL_OFF_START);
    
    msleep(100);
    pr_info("qemu-epc-demo: Qemu-EPC started\n");
}
```

Thus the goal for EPC is clear: implement all these hardware details, and abstracting them into a set of standard API `pci_epc_ops` to EPF :)


###  3.1 The `pci_epc_ops` Vtable

`struct pci_epc_ops` is the set of standard functions that EPF drivers could use to configure the hardware. Each EPC driver fills its own implementation.

```c
// pci_epc.h
struct pci_epc_ops {
	int	(*write_header)(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
				struct pci_epf_header *hdr);
	int	(*set_bar)(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			   struct pci_epf_bar *epf_bar);
	void (*clear_bar)(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			     struct pci_epf_bar *epf_bar);
	int	(*map_addr)(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			    phys_addr_t addr, u64 pci_addr, size_t size);
	void (*unmap_addr)(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			      phys_addr_t addr);
	int	(*set_msi)(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			   u8 interrupts);
	int	(*get_msi)(struct pci_epc *epc, u8 func_no, u8 vfunc_no);
	int	(*set_msix)(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			    u16 interrupts, enum pci_barno, u32 offset);
	int	(*get_msix)(struct pci_epc *epc, u8 func_no, u8 vfunc_no);
	int	(*raise_irq)(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			     enum pci_epc_irq_type type, u16 interrupt_num);
	int	(*map_msi_irq)(struct pci_epc *epc, u8 func_no, u8 vfunc_no,
			       phys_addr_t phys_addr, u8 interrupt_num,
			       u32 entry_size, u32 *msi_data,
			       u32 *msi_addr_offset);
	int	(*start)(struct pci_epc *epc);
	void (*stop)(struct pci_epc *epc);
	const struct pci_epc_features* (*get_features)(struct pci_epc *epc,
						       u8 func_no, u8 vfunc_no);
	struct module *owner;
};

```

In the `qemu-epc-demo`, we only implement part of the functions that are used in our EPF layer.
```c
static const struct pci_epc_ops qemu_epc_ops = {
    .write_header   = qemu_epc_write_header,
    .set_bar        = qemu_epc_set_bar,
    .clear_bar      = qemu_epc_clear_bar,
    .start          = qemu_epc_start,
    .stop           = qemu_epc_stop,
    .raise_irq      = qemu_epc_raise_irq,
};
```


### 3.2 EPC Probe - Register as a Controller

Everything in the EPC Layer start with the `.probe` function, which is called when the `device` meets our driver, or vice versa. When we install the epc driver with `insmod qemu-epc-demo.ko`, the kernel load and create a pci_driver instance with the information we provided in the module:
```c
static struct pci_driver qemu_epc_driver = {
    .name     = "qemu-epc",
    .id_table = qemu_epc_ids,
    .probe    = qemu_epc_probe,
    .remove   = qemu_epc_remove,
};
```
The kernel execute bus enumeration, scan the pci_bus to look for a device with name = `qemu-epc`. If the device is founded, our `.probe()` function is eventually called and which allow the driver set up the device.

3 device-Specific functions are called in the `.probe()`:

- `pci_enable_device()`: Enable the device, Interrupt working, make device ready-to-use.

- `pci_set_master()`: Enable device initiate transaction to Host, needed for interrupt signal and outbound dma.

- `pci_request_region()`: Make the device BAR memory private, when set, only this driver could operate on this device BAR memory.

Remember our demo device `qemu-epc` has 4 BAR region. The physical addresses of the region are already allocated, we need them to implement our `pci_ep_ops`.
Therefore we have a private struct `struct qemu-epc` to store these addresses so that we could reference them, and also other important structure such as the `pci_dev` and the `pci_epc`.

```c
struct qemu_epc {
    struct pci_dev *pdev;
    struct pci_epc *epc;

    void __iomem *ctrl_region;
    void __iomem *pci_cfg_region;
    void __iomem *bar_cfg_region;
    void __iomem *ob_window_region;

    phys_addr_t ob_window_phys;
};
```

Now we have a place to store the BAR addresses, but they are physical addresses in the MMIO address space assigned by the kernel during PCI enumeration. We cannot access physical address directly.  

```c
// qemu-epc-demo.c

struct qemu_epc* priv;
priv = devm_kzalloc(&dev->dev, sizeof(*priv), GFP_KERNEL);
...

priv->ctrl_region       = pcim_iomap(dev, BAR_CTRL, 0);
priv->bar_cfg_region    = pcim_iomap(dev, BAR_BAR_CFG, 0);
priv->pci_cfg_region    = pcim_iomap(dev, BAR_PCI_CFG, 0);
priv->ob_window_region  = pcim_iomap(dev, BAR_OB_WIN, 0);
```

`pcim_iomap()` creates a virtual address mapping with the physical addresses, and return the mapped virtual address. We will store them in our private struct.

```c
// qemu-epc-demo.c

epc = devm_pci_epc_create(&dev->dev, &qemu_epc_ops);
...
priv->epc = epc;
epc_set_drvdata(epc, priv);
pci_set_drvdata(dev, priv);

```
This is the key function call, creating the `struct pci_epc` and bind it with our `pci_epc_ops` functions. After the function call, it causes the Configfs creating directory under `../configfs/pci_ep/controllers/qemu-epc-demo`.

At last we will store the `priv` as the epc driver data with `epc_set_drvdata(epc, priv)` and the device driver data with `pci_set_drvdata(dev, priv)`.

### 3.3 Tracing `set_bar()`

When the EPF driver calls `pci_epc_set_bar()`, the EPC core acquires a
mutex, validates the function number, and dispatches to the EPC driver's
`set_bar()` implementation through the ops vtable.

The EPF passes a `struct pci_epf_bar` which contains:
- The BAR number (0, 1, or 2 in our demo)
- The physical address of the backing memory (allocated by
  `pci_epf_alloc_space()`)
- The BAR size
- Flags (memory type, prefetchable, etc.)

The EPC driver's job is to write this information into the hardware so
that the controller knows: "when the host accesses this BAR's PCI
address range, forward the transaction to this physical memory on the
EP side."

In the `qemu-epc-demo`, this means writing the BAR number, flags,
physical address, and size into the `BAR_CFG` region. Notice that the mask register
is written last — it acts as a commit trigger. The hardware does not act
on the other register values until the corresponding mask bit is set.
This is a common hardware pattern: stage all the configuration values
first, then write a single "go" register to apply them atomically.

### 3.4 Tracing `write_header()` and `raise_irq`

**`write_header`** receives a `struct pci_epf_header` containing the vendor_id, driver_id, revision, class code and interrupt pin. The EPC writes these values into the controller's PCI configuration space region. This is what the host see when it enumerate the PCIe bus - the host read the config space to discover the device's identities and capabilities. In the demo, we write vendor ID `0x1234` and device ID `0xDEAD`, which is what the host driver expect to match against.

**`raise_irq`** is called by the EPF driver after it has finished processing a request and wants to notify the host. It receives a IRQ type (legacy, MSI, MSI-X) and an interrupt number - the IRQ type first, then the interrupt number. Writing the interrupt number is the trigger; the hardware sends a TLP message to the host, which cause the host'd IRQ handler to fire.

Both functions follow the same pattern as `set_bar()`: the EPF driver
calls a generic `pci_epc_*()` API, the EPC core dispatches through the
vtable, and the EPC driver translates the call into hardware-specific
register writes. The EPF driver never knows or cares which registers
are being written — it just calls the standard interface.


## 4. EPF Layer - Functional Driver

### 4.1 EPF Bus

The EPF subsystem has its own bus type: `pci_epf_bus_type`, which means that there is also an epf-device. This is specially designed so that the EPF driver talk to the EPF device, exposing the functional detail and configurations which are later exposed to the Host, without knowing what the underlying hardware is. Such design makes writing an EPF driver feels like writing a normal PCI driver.

The EPF device and driver are matched with their name:
```c
// pci-epf-core.c

static int pci_epf_device_match(struct device *dev, struct device_driver *drv)
{
	struct pci_epf *epf = to_pci_epf(dev);
	struct pci_epf_driver *driver = to_pci_epf_driver(drv);

	if (driver->id_table)
		return pci_epf_match_id(driver->id_table, epf);

	return !strcmp(epf->name, drv->name);
}

```

When we install the epf-driver and create a epf-device through `mkdir` in Configfs, the bus matches them and call `.probe()`. This is different from normal PCI matching, which use vendorID and deviceID. The epf-device name comes from configfs directory name rather than hardware.


### 4.2 Probe - Device Discovery
As mentioned, when EPF driver and device are matched, the driver `.probe()` function is called.

```c
# qemu-epf-demo.c

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
```

The demo `.probe()` function does 2 things: 1. Store the epf->header into the struct pci_epf (which represent our epf-device) 2. Allocate memory for the demo private data and save it in the struct pci_epf - so that it could be used in the `.bind()` function when EPF device link with EPC device.

### 4.3 Bind - Where Everything happen

The bind is triggered from userspace by creating a symlink:
```
ln -s /sys/kernel/config/pci_ep/functions/qemu-epf-demo/func1 \
      /sys/kernel/config/pci_ep/controllers/<bus:device:function>/func1
```

This symlink basically tells the framework to connect this EPF device to this EPC controller. The VFS forwards the symlink operation to configfs, whichhas registered an `allow_link` callback on the controller group. This callback is `pci_primary_epc_epf_link` in `pci-ep-cfs.c`:

```c

static int pci_primary_epc_epf_link(struct config_item *epf_item,
				    struct config_item *epc_item)
{
	int ret;
	struct pci_epf_group *epf_group = to_pci_epf_group(epf_item->ci_parent);
	struct pci_epc_group *epc_group = to_pci_epc_group(epc_item);
	struct pci_epc *epc = epc_group->epc;
	struct pci_epf *epf = epf_group->epf;

	ret = pci_epc_add_epf(epc, epf, PRIMARY_INTERFACE);
	if (ret)
		return ret;

	ret = pci_epf_bind(epf);
	if (ret) {
		pci_epc_remove_epf(epc, epf, PRIMARY_INTERFACE);
		return ret;
	}

	return 0;
}

```

On the EPC side, it calls the `pci_epc_add_epf()`. This function checks the type of the EPF function (PRIMARY/SECONDARY) and whether the epf has already bind with an EPC controller already. The key is an EPF device could only bind one EPC as PRIMARY function and one EPC as secondary function before unbind to prevent memory leak.

The EPC allocate a function number to the EPF device for identification (One EPC controller could be binded to multiple EPF device) and lastly the epf device will obtain the reference of the pci_epc and the epf is added to the epf-list of the pci_epc.  

```c
// pci-epc-core.c

int pci_epc_add_epf(struct pci_epc *epc, struct pci_epf *epf,
		    enum pci_epc_interface_type type)
{
...

  if (type == PRIMARY_INTERFACE) {
    epf->func_no = func_no;
    epf->epc = epc;
    list = &epf->list;
  } else {
    epf->sec_epc_func_no = func_no;
    epf->sec_epc = epc;
    list = &epf->sec_epc_list;
  }

  list_add_tail(list, &epc->pci_epf);
}
```

On the EPF side, it does a brunch of sanity checks, ensuring the epf device has driver associated with it, iterate over virtual function (SR-IOV: Not related here). And in the end call the driver registered `.bind()` function.

```c
  ret = epf->driver->ops->bind(epf);
  epf->is_bound = true;
```


At this point, `epf->epc` is set. This is the key moment — the EPF
driver can now reach the hardware through the EPC abstraction. Inside
`epf_demo_bind()`, every hardware configuration call goes through the
linked EPC:
```c
// qemu-epf-demo.c

struct pci_epc *epc = epf->epc;

pci_epc_write_header(epc, epf->func_no, ...);  // configure identity
pci_epc_set_bar(epc, epf->func_no, ...);       // configure BARs
pci_epc_raise_irq(epc, epf->func_no, ...);     // signal the host
```

The EPF driver never touches a hardware register directly. It calls
generic `pci_epc_*()` APIs, passing the `epc` pointer it obtained
through the bind. The EPC core dispatches these through the ops vtable
to the hardware-specific implementation. The EPF driver focuses purely
on the function logic — allocating BAR buffers, writing the magic
value, polling the doorbell, performing case inversion — while the EPC
layer handles all hardware detail underneath.

This is the separation the framework provides: the EPF implements
*what* the endpoint does, the EPC implements *how* the hardware is
programmed.

## 5. Configfs — Runtime Device Composition

Configfs is the userspace interface for creating EPF devices and
linking them to EPC controllers at runtime. After mounting, the
framework exposes the following directory structure:
```
/sys/kernel/config/pci_ep/
├── controllers/          ← one entry per registered EPC
│   └── 0000:00:02.0/    ← created by devm_pci_epc_create()
│       ├── start
│       └── secondary/
└── functions/            ← one entry per registered EPF driver
    └── qemu-epf-demo/   ← created by pci_epf_register_driver()
```

The controller directory appears automatically when the EPC driver
calls `devm_pci_epc_create()` during probe (Section 3.2). The function
driver directory appears when the EPF driver calls
`pci_epf_register_driver()` during module init. Everything else is
driven by the following userspace commands.


### Step 1: Mount configfs
```bash
mount -t configfs configfs /sys/kernel/config
```

Mounts the configfs virtual filesystem. The PCI endpoint subsystem
registered itself with configfs at boot, so the `pci_ep/` directory
appears immediately.


### Step 2: Load the EPF driver
```bash
insmod qemu-epf-demo.ko
```

Calls `pci_epf_register_driver()`, which registers the driver on
`pci_epf_bus_type`. No EPF device exists yet, so probe is not called.
```
[   13.473434] qemu_epf_demo: loading out-of-tree module taints kernel.
[   13.478579] qemu-epf-demo: driver registered
```


### Step 3: Load the EPC driver
```bash
insmod qemu-epc-demo.ko
```

PCI bus matching triggers `qemu_epc_probe()`, which maps the BAR
regions and calls `devm_pci_epc_create()` to register the controller
(see Section 3.2 for detail).
```
[   21.945494] qemu-epc-demo: probe 0000:00:02.0
[   21.945800] qemu-epc 0000:00:02.0: enabling device (0000 -> 0002)
[   21.947140] qemu-epc-demo: ctrl=(____ptrval____) pci_cfg=(____ptrval____) bar_cfg=(____ptrval____) ob_window=(____ptrval____)
[   21.949019] qemu-epc-demo: EPC controller registered!
[   21.949286] qemu-epc-demo: check /sys/kernel/config/pci_ep/controllers/

```


### Step 4: Create the EPF device
```bash
mkdir /sys/kernel/config/pci_ep/functions/qemu-epf-demo/func1
```

Triggers `pci_epf_create("qemu-epf-demo")`, which registers an EPF
device on the bus. Name matching fires and calls `epf_demo_probe()`
(see Section 4.2 for detail). Configfs also populates `func1/` with
attribute files (`vendorid`, `deviceid`, etc.) that can be overridden
before binding.
```
[   41.314392] qemu-epf-demo: probe called
```


### Step 5: Link EPF to EPC
```bash
ln -s /sys/kernel/config/pci_ep/functions/qemu-epf-demo/func1 \
      /sys/kernel/config/pci_ep/controllers/0000:00:02.0/
```

Triggers the configfs `allow_link` callback, which calls
`pci_epc_add_epf()` then `pci_epf_bind()` (see Section 4.3 for the
full call chain). The entire bind sequence is synchronous — the shell
does not return until `epf_demo_bind()` completes.
```
[  118.965942] qemu-epf-demo: bind is called
[  118.966618] qemu-epc-demo: Write Header: VendorId:0x1234 DeviceId: 0xdead
...
[  118.967158] qemu-epc-demo: interrupt_pin value=0x1 offset=0x3d size=4
[  118.967631] qemu-epc-demo: set bar: 0 phys: 0x43519000 size: 0x1000
[  118.968116] qemu-epf-demo: BAR0 size: 4096
...
[  118.968342] qemu-epc-demo: set bar: 1 phys: 0x43569000 size: 0x1000
[  118.968670] qemu-epf-demo: BAR1 size: 4096
...
[  118.968845] qemu-epc-demo: set bar: 2 phys: 0x4355b000 size: 0x1000
[  118.969170] qemu-epf-demo: BAR2 size: 4096
```


### Step 6: Start the EPC
```bash
echo 1 > /sys/kernel/config/pci_ep/controllers/0000:00:02.0/start
```

Triggers `pci_epc_start()` → `qemu_epc_start()`, which writes to the
QEMU control register. QEMU begins listening on the Unix socket for
TLP traffic from the host. The PCIe link is now live.
```
[  275.560711] qemu-epc-demo: start
[  275.665719] qemu-epc-demo: Qemu-EPC started
```

The EP is fully configured. The next step is to boot the host VM and
load the host driver.

## Run the demo

### Tools Requirement
* Linux 6.1.166 
* Qemu 6.2.0 + Backport Commit
* Backport Qemu Virtual PCIE Commit — adds virtual PCIe endpoint controller device to QEMU
  (Ref: https://marc.info/?l=qemu-devel&m=169573607526366)
* Buildroot
* `qemu-epf-demo.ko` + `qemu-epc-demo.ko` for Endpoint (ARM), `epf-host-demo.ko` for Host (x86)

### Linux 6.1 Kernel

```bash
# 1. Build tool  
sudo apt install -y build-essential gcc \
make bc flex bison libssl-dev libelf-dev \
libncurses-dev pahole

# 2. Fetch the Linux 6.1.166 (include all bug-fix)  
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.1.166.tar.xz
# Unzip 
tar -xf linux-6.1.166.tar.xz

# 3. Generate .config and build the Linux Image  
# x86  
make defconfig && make -j$(nproc)  
# arm 
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=../linux-arm64-build defconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=../linux-arm64-build -j$(nproc)
```


### QEMU

``` bash
# 1. Download Qemu-6.2.0 zip
wget https://download.qemu.org/qemu-6.2.0.tar.xz

# 2. Install dependencies (Ubuntu/Debian)
sudo apt update
sudo apt install -y git libglib2.0-dev libfdt-dev libpixman-1-dev zlib1g-dev \
  ninja-build build-essential python3 python3-pip libslirp-dev

# 3 Extract & Condigure the Qemu-6.2.0
tar xf qemu-6.2.0.tar.xz
cd qemu-6.2.0
mkdir build && cd build

# 4. Run configure from inside build/
../qemu-6.2.0/configure --target-list=x86_64-softmmu,aarch64-softmmu
make -j$(nproc)

```

### Buildroot

```bash
# Build rootfs with buildroot (both aarch and x86)

# 1. Clone Buildroot
git clone --depth=1 https://github.com/buildroot/buildroot.git

# 2. Configure Buildroot for both ARM and x86
# Inside the menu, select ext4 as filesystem format
# Also set a larger size for the filesystem size
# Unselect all other option:
#    - Linux Kernel
#    - Host Uitlity

# x86
make O=output-x86 qemu_x86_64_defconfig
make O=output-x86 menuconfig
make O=output-x86 -j$(nproc) 

# ARM
make O=output-arm qemu_aarch64_virt_defconfig
make O=output-arm menuconfig
make O=output-arm -j$(nproc)

```

### Driver Installation
``` bash
# Compile the EPC EPF driver
# copy it into output/target/root/ (aarch64) and output-x86/target/root/ (x86)
# make the rootfs again
cp qemu-epf-demo.ko ~/pcie-demo/br/buildroot/output/target/root/
cp qemu-epc-demo.ko ~/pcie-demo/br/buildroot/output/target/root/

cp epf-host-demo.ko ~/pcie-demo/br/buildroot/output-x86/target/root/

# inside the buildroot folder, build the filesystem again
make O=output -j$(nproc) 
make O=output-x86 -j$(nproc)
```



## Code Execution Flow
- Sequence : First Set up Endpoint(ARM), then run Host(x86)

### 1. Endpoint (ARM)
```bash
# Run Endpoint
> ~/pcie-demo/qemu-src/build/qemu-system-aarch64 \
    -machine virt \
    -cpu cortex-a57 \
    -kernel ~/pcie-demo/linux-source/linux-arm64-build/arch/arm64/boot/Image \
    -drive file=~/pcie-demo/br/buildroot/output/images/rootfs.ext4,format=raw \
    -m 2G \
    -append "root=/dev/vda console=ttyAMA0 rw" \
    -nographic \
    -device qemu-epc

# 1. Mount configfs: For userspace creating and linking EPC EPF
> mount -t configfs configfs /sys/kernel/config

# 2. Load both driver
> insmod /root/qemu-epf-demo.ko

[   13.473434] qemu_epf_demo: loading out-of-tree module taints kernel.
[   13.478579] qemu-epf-demo: driver registered

> insmod /root/qemu-epc-demo.ko

[   21.945494] qemu-epc-demo: probe 0000:00:02.0
[   21.945800] qemu-epc 0000:00:02.0: enabling device (0000 -> 0002)
[   21.947140] qemu-epc-demo: ctrl=(____ptrval____) pci_cfg=(____ptrval____) bar_cfg=(____ptrval____) ob_window=(____ptrval____)
[   21.949019] qemu-epc-demo: EPC controller registered!
[   21.949286] qemu-epc-demo: check /sys/kernel/config/pci_ep/controllers/


# 3. Create EPF device
> mkdir /sys/kernel/config/pci_ep/functions/qemu-epf-demo/func1
[   41.314392] qemu-epf-demo: probe called


# 4. Link EPF to EPC
> ln -s /sys/kernel/config/pci_ep/functions/qemu-epf-demo/func1 \
      /sys/kernel/config/pci_ep/controllers/<bus:device:function>/func1
#                                           ^^^^^^^^^^^^^^^^^^^^^
#                                           lspci to check


[  118.965942] qemu-epf-demo: bind is called
[  118.966618] qemu-epc-demo: Write Header: VendorId:0x1234 DeviceId: 0xdead
...
[  118.967158] qemu-epc-demo: interrupt_pin value=0x1 offset=0x3d size=4
...
[  118.967631] qemu-epc-demo: set bar: 0 phys: 0x43519000 size: 0x1000
[  118.968116] qemu-epf-demo: BAR0 size: 4096
...
[  118.968342] qemu-epc-demo: set bar: 1 phys: 0x43569000 size: 0x1000
[  118.968670] qemu-epf-demo: BAR1 size: 4096
...
[  118.968845] qemu-epc-demo: set bar: 2 phys: 0x4355b000 size: 0x1000
[  118.969170] qemu-epf-demo: BAR2 size: 4096
...
[  118.969369] qemu-epf-demo: bind is completed
[  118.972809] qemu-epf-demo: Doorbell polling thread started

# 5. Enable the QEMU-EPC device
> echo 1 > /sys/kernel/config/pci_ep/controllers/<bus:device:function>/start

[  275.560711] qemu-epc-demo: start
[  275.665719] qemu-epc-demo: Qemu-EPC started



# 6. Receive Host message (after insmod in host)

[  301.993737] qemu-epf-demo: Receive doorbell from Host, size=43
[  301.994080] qemu-epf-demo: Bar1 content:
[  301.994244] Host write message to BAR 1, check it out
[  301.994244] 
[  301.994678] qemu-epf-demo: Send Message size: 43
[  301.995114] qemu-epc-demo: Raise IRQ 1

```

<img src="sc/ep-demo-1.png" width="80%"> <img src="sc/ep-demo-2.png" width="80%">
<img src="sc/ep-demo-3.png" width="80%"> <img src="sc/ep-demo-4.png" width="80%">


### 2. Host (x86)
```bash
# Run Host
> ~/pcie-demo/qemu-src/build/qemu-system-x86_64 \
    -kernel ~/pcie-demo/linux-source/x86bzImage \
    -drive file=~/pcie-demo/br/buildroot/output-x86/images/rootfs.ext4,format=raw \
    -append "root=/dev/sda console=ttyS0" \
    -nographic \
    -m 2G \
    -device epf-bridge

> insmod epf-host-demo.ko

[   10.584118] epf-host-demo: loading out-of-tree module taints kernel.
[   10.589290] epf-host: probe 0000:00:04.0
[   10.673986] ACPI: \_SB_.LNKD: Enabled at IRQ 10
[   10.675135] epf-host: magic value verified
[   10.675629] epf-host: probe is completed
...
[   10.678734] epf-host: Received IRQ 10
[   10.679040] epf-host: Received Message Size: 43
[   10.681190] epf-host: EP message: HOST WRITE MESSAGE TO bar 1, CHECK IT OUT
[   10.681190] 
```
<img src="sc/ep-demo-5.png" width="100%">