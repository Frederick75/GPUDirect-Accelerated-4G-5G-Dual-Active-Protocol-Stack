Build and Execution Instructions of kernel module
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

obj-m += accel_driver.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

Compiling the User-Space Engine (Build & Run Commands)
# Create build artifact folders
mkdir build && cd build

# Generate build definitions via CMake
cmake ..

# Compile target application binaries
make

# Allocate required HugePages allocations for DPDK packet processing
sudo bash -c "echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"

# Launch the execution binary across isolated core maps (e.g., Cores 0 and 1)
# Implements a virtual TAP network device loop for emulation if physical hardware is absent
sudo ./ran_engine -c 0x3 --vdev=net_tap0,iface=tap0
