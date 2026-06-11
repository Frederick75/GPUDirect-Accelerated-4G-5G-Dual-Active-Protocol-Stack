#ifndef ACCEL_DRIVER_H
#define ACCEL_DRIVER_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define ACCEL_MAGIC 'g'

struct accel_bar_info {
    __u64 phys_addr;
    __u64 size;
    __u32 bar_index;
    __u32 padding;
};

#define ACCEL_IOCTL_GET_BAR_INFO _IOR(ACCEL_MAGIC, 1, struct accel_bar_info)

#endif /* ACCEL_DRIVER_H */
