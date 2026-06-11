1. Compiling the Kernel Module
# Compile the driver
make

# Insert into the running kernel
sudo insmod accel_driver.ko

# Inspect kernel ring buffer logs to verify successful tracking initialization
dmesg | tail -n 20

2. Compiling the User-Space Engine
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
