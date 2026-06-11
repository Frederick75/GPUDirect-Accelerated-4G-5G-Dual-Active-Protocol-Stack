#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/pci.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include "accel_driver.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("AI Collaboration Core");
MODULE_DESCRIPTION("Production PCIe RDMA GPUDirect Accelerator Driver Framework");
MODULE_VERSION("1.0");

#define DEVICE_NAME "accel_gpudirect"
#define CLASS_NAME  "accel_proto"
#define VENDOR_ID   0x10EE // Example Vendor ID (Xilinx/AMD)
#define DEVICE_ID   0x903F // Example Accelerator Device ID

static int major_number;
static struct class *accel_class = NULL;
static struct cdev accel_cdev;
static dev_t dev_num;

struct accel_dev_context {
    struct pci_dev *pdev;
    resource_size_t bar_phys;
    resource_size_t bar_len;
    void __iomem *bar_virt;
};

static struct accel_dev_context g_ctx;

static int accel_open(struct inode *inod, struct file *fil) {
    fil->private_data = &g_ctx;
    return 0;
}

static long accel_ioctl(struct file *fil, unsigned int cmd, unsigned long arg) {
    struct accel_dev_context *ctx = fil->private_data;
    struct accel_bar_info info = {0};

    switch (cmd) {
        case ACCEL_IOCTL_GET_BAR_INFO:
            if (!ctx->pdev) {
                return -ENODEV;
            }
            info.phys_addr = (__u64)ctx->bar_phys;
            info.size = (__u64)ctx->bar_len;
            info.bar_index = 0;

            if (copy_to_user((struct accel_bar_info __user *)arg, &info, sizeof(info))) {
                return -EFAULT;
            }
            break;
        default:
            return -EINVAL;
    }
    return 0;
}

static int accel_mmap(struct file *fil, struct vm_area_struct *vma) {
    struct accel_dev_context *ctx = fil->private_data;
    unsigned long size = vma->vm_end - vma->vm_start;

    if (size > ctx->bar_len) {
        return -EINVAL;
    }

    // Set page attributes to non-cached to guarantee write-through/I/O consistency
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);

    if (remap_pfn_range(vma, vma->vm_start, ctx->bar_phys >> PAGE_SHIFT, size, vma->vm_page_prot)) {
        dev_err(&ctx->pdev->dev, "Failed to remap BAR0 physical range to user-space\n");
        return -EAGAIN;
    }

    return 0;
}

static int accel_release(struct inode *inod, struct file *fil) {
    (void)inod;
    (void)fil;
    return 0;
}

static const struct file_operations fops = {
    .owner          = THIS_MODULE,
    .open           = accel_open,
    .unlocked_ioctl = accel_ioctl,
    .mmap           = accel_mmap,
    .release        = accel_release,
};

static int accel_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id) {
    int err;
    (void)id;

    dev_info(&pdev->dev, "Initializing PCIe RDMA Accelerator Device hardware\n");

    err = pci_enable_device(pdev);
    if (err) {
        dev_err(&pdev->dev, "pci_enable_device failed\n");
        return err;
    }

    err = pci_request_regions(pdev, DEVICE_NAME);
    if (err) {
        dev_err(&pdev->dev, "pci_request_regions failed\n");
        goto disable_device;
    }

    pci_set_master(pdev);

    g_ctx.bar_phys = pci_resource_start(pdev, 0);
    g_ctx.bar_len  = pci_resource_len(pdev, 0);
    g_ctx.bar_virt = pci_iomap(pdev, 0, g_ctx.bar_len);

    if (!g_ctx.bar_virt) {
        dev_err(&pdev->dev, "pci_iomap for BAR0 mapping failed\n");
        err = -ENOMEM;
        goto release_regions;
    }

    g_ctx.pdev = pdev;
    pci_set_drvdata(pdev, &g_ctx);

    dev_info(&pdev->dev, "BAR0 registered successfully: Phys=0x%llx, Size=%llu\n",
             (unsigned long long)g_ctx.bar_phys, (unsigned long long)g_ctx.bar_len);

    return 0;

release_regions:
    pci_release_regions(pdev);
disable_device:
    pci_disable_device(pdev);
    return err;
}

static void accel_pci_remove(struct pci_dev *pdev) {
    struct accel_dev_context *ctx = pci_get_drvdata(pdev);

    if (ctx) {
        if (ctx->bar_virt) {
            pci_iounmap(pdev, ctx->bar_virt);
        }
        pci_release_regions(pdev);
        pci_disable_device(pdev);
        ctx->pdev = NULL;
    }
    dev_info(&pdev->dev, "PCIe Device removed from subsystem hooks\n");
}

static const struct pci_device_id accel_pci_ids[] = {
    { PCI_DEVICE(VENDOR_ID, DEVICE_ID) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, accel_pci_ids);

static struct pci_driver accel_pci_driver = {
    .name     = DEVICE_NAME,
    .id_table = accel_pci_ids,
    .probe    = accel_pci_probe,
    .remove   = accel_pci_remove,
};

static int __init accel_driver_init(void) {
    int result;

    result = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (result < 0) {
        pr_err("accel_driver: Failed to allocate major number\n");
        return result;
    }
    major_number = MAJOR(dev_num);

    accel_class = class_create(CLASS_NAME);
    if (IS_ERR(accel_class)) {
        unregister_chrdev_region(dev_num, 1);
        pr_err("accel_driver: Failed to register device class\n");
        return PTR_ERR(accel_class);
    }

    cdev_init(&accel_cdev, &fops);
    accel_cdev.owner = THIS_MODULE;

    result = cdev_add(&accel_cdev, dev_num, 1);
    if (result < 0) {
        class_destroy(accel_class);
        unregister_chrdev_region(dev_num, 1);
        pr_err("accel_driver: Failed to add cdev mapping\n");
        return result;
    }

    device_create(accel_class, NULL, dev_num, NULL, DEVICE_NAME);

    result = pci_register_driver(&accel_pci_driver);
    if (result < 0) {
        device_destroy(accel_class, dev_num);
        class_destroy(accel_class);
        cdev_del(&accel_cdev);
        unregister_chrdev_region(dev_num, 1);
        pr_err("accel_driver: PCIe driver registration failed\n");
        return result;
    }

    pr_info("accel_driver: Framework initialized character layout successfully\n");
    return 0;
}

static void __exit accel_driver_exit(void) {
    pci_unregister_driver(&accel_pci_driver);
    device_destroy(accel_class, dev_num);
    class_destroy(accel_class);
    cdev_del(&accel_cdev);
    unregister_chrdev_region(dev_num, 1);
    pr_info("accel_driver: Module framework unloaded smoothly\n");
}

module_init(accel_driver_init);
module_exit(accel_driver_exit);
