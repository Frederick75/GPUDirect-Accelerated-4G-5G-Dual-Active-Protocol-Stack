#include <iostream>
#include <vector>
#include <string_view>
#include <span>
#include <memory>
#include <cstdint>
#include <atomic>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_lcore.h>

// Include shared header matching driver IOCTL signature layout
extern "C" {
#include "accel_driver.h"
}

constexpr uint16_t NUM_MBUFS = 8191;
constexpr uint16_t MBUF_CACHE_SIZE = 250;
constexpr uint16_t RX_RING_SIZE = 1024;
constexpr uint16_t TX_RING_SIZE = 1024;
constexpr uint16_t BURST_SIZE = 32;

std::atomic<bool> force_quit{false};

namespace ran_engine {

    struct alignas(64) NetworkTelemetry {
        uint64_t rx_packets{0};
        uint64_t tx_packets{0};
        uint64_t dpi_hits{0};
        uint64_t crypto_processed_bytes{0};
    };

    class ProcessingEngine {
    private:
        static constexpr std::string_view DPI_PATTERN = "GTP-U-USER-DATA";
        uint8_t* m_bar_region{nullptr};
        uint64_t m_bar_size{0};

        // Standard user-plane pseudo-cipher transform (XOR stream masking simulation)
        static void xor_crypto_transform(std::span<uint8_t> payload) noexcept {
            constexpr uint8_t cipher_mask = 0xA5; 
            for (auto& byte : payload) {
                byte ^= cipher_mask;
            }
        }

    public:
        ProcessingEngine(uint8_t* bar_region, uint64_t bar_size) 
            : m_bar_region(bar_region), m_bar_size(bar_size) {}

        ~ProcessingEngine() = default;

        void process_packet(struct rte_mbuf* mbuf, NetworkTelemetry& stats) const noexcept {
            uint8_t* data = rte_pktmbuf_mtod(mbuf, uint8_t*);
            uint32_t len = rte_pktmbuf_pkt_len(mbuf);

            if (len < 64) return; // Drop parsing run if frame size is truncated

            // 1. Deep Packet Inspection (DPI) utilizing C++ string_view bounds verification
            std::string_view packet_view(reinterpret_cast<char*>(data), len);
            if (packet_view.find(DPI_PATTERN) != std::string_view::npos) {
                stats.dpi_hits++;
            }

            // 2. Encryption/Decryption logic using safe std::span containers
            // Bypasses local standard network headers (L2/L3/L4 offsets configured to 42 bytes)
            std::span<uint8_t> payload_span(data + 42, len - 42);
            xor_crypto_transform(payload_span);
            stats.crypto_processed_bytes += payload_span.size();

            // 3. Optional Accelerator interaction via mapped BAR space
            if (m_bar_region && m_bar_size >= sizeof(uint32_t)) {
                // Periodically update execution heartbeat inside mapped accelerator space
                volatile auto* heartbeat = reinterpret_cast<volatile uint32_t*>(m_bar_region);
                *heartbeat = *heartbeat + 1;
            }
        }
    };
}

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        std::cout << "\nSignal captured. Gracefully stopping data paths...\n";
        force_quit.store(true);
    }
}

struct LcoreArgs {
    ran_engine::NetworkTelemetry* telemetry;
    ran_engine::ProcessingEngine* engine;
};

static int lcore_network_loop(void* arg) {
    auto* args = static_cast<LcoreArgs*>(arg);
    uint16_t port_id;

    std::cout << "Data Path Processing Engine active on Core Lcore ID: " << rte_lcore_id() << "\n";

    struct rte_mbuf* bufs[BURST_SIZE];

    while (!force_quit.load(std::memory_order_relaxed)) {
        RTE_ETH_FOREACH_DEV(port_id) {
            const uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, bufs, BURST_SIZE);
            if (nb_rx == 0) continue;

            args->telemetry->rx_packets += nb_rx;

            for (uint16_t i = 0; i < nb_rx; ++i) {
                args->engine->process_packet(bufs[i], *(args->telemetry));
            }

            const uint16_t nb_tx = rte_eth_tx_burst(port_id, 0, bufs, nb_rx);
            args->telemetry->tx_packets += nb_tx;

            // Free non-forwarded packets to avoid memory pooling leaks
            if (unlikely(nb_tx < nb_rx)) {
                for (uint16_t buf = nb_tx; buf < nb_rx; ++buf) {
                    rte_pktmbuf_free(bufs[buf]);
                }
            }
        }
    }
    return 0;
}

static inline int init_port(uint16_t port, struct rte_mempool* mbuf_pool) {
    struct rte_eth_conf port_conf = {};
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;

    if (!rte_eth_dev_is_valid_port(port)) return -1;

    int ret = rte_eth_dev_configure(port, 1, 1, &port_conf);
    if (ret < 0) return ret;

    ret = rte_eth_rx_queue_setup(port, 0, RX_RING_SIZE, rte_eth_dev_socket_id(port), nullptr, mbuf_pool);
    if (ret < 0) return ret;

    ret = rte_eth_tx_queue_setup(port, 0, TX_RING_SIZE, rte_eth_dev_socket_id(port), nullptr);
    if (ret < 0) return ret;

    ret = rte_eth_dev_start(port);
    if (ret < 0) return ret;

    rte_eth_promiscuous_enable(port);
    return 0;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 1. Core Accelerator Driver Mapping Sequence
    int dev_fd = open("/dev/accel_gpudirect", O_RDWR);
    uint8_t* bar_ptr = nullptr;
    struct accel_bar_info bar_info = {};

    if (dev_fd < 0) {
        std::cerr << "Warning: Accelerator driver node unavailable. Running in emulation mode.\n";
    } else {
        if (ioctl(dev_fd, ACCEL_IOCTL_GET_BAR_INFO, &bar_info) == 0) {
            std::cout << "PCIe BAR Address resolved: 0x" << std::hex << bar_info.phys_addr 
                      << " (Sizing: " << std::dec << bar_info.size << " bytes)\n";
            
            bar_ptr = static_cast<uint8_t*>(mmap(nullptr, bar_info.size, 
                                            PROT_READ | PROT_WRITE, 
                                            MAP_SHARED, dev_fd, 0));
            if (bar_ptr == MAP_FAILED) {
                std::cerr << "Failed to map device hardware allocation segment.\n";
                bar_ptr = nullptr;
            }
        }
    }

    // 2. Initialize DPDK Environment Abstraction Layer (EAL)
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        if (dev_fd >= 0) close(dev_fd);
        rte_exit(EXIT_FAILURE, "EAL initialization parameters invalid.\n");
    }
    argc -= ret;
    argv += ret;

    uint16_t nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0) {
        std::cerr << "Error: No viable DPDK interfaces bound.\n";
        if (dev_fd >= 0) close(dev_fd);
        return -1;
    }

    struct rte_mempool* mbuf_pool = rte_pktmbuf_pool_create(
        "MBUF_POOL", NUM_MBUFS * nb_ports, MBUF_CACHE_SIZE, 0,
        RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id()
    );

    if (!mbuf_pool) {
        if (dev_fd >= 0) close(dev_fd);
        rte_exit(EXIT_FAILURE, "Mbuf packet pooling construction failed.\n");
    }

    uint16_t port_id;
    RTE_ETH_FOREACH_DEV(port_id) {
        if (init_port(port_id, mbuf_pool) < 0) {
            rte_exit(EXIT_FAILURE, "Failed initialization on interface ID %u\n", port_id);
        }
    }

    // 3. Launch Lcore processing runtime threads
    auto telemetry_metrics = std::make_unique<ran_engine::NetworkTelemetry>();
    auto processing_engine = std::make_unique<ran_engine::ProcessingEngine>(bar_ptr, bar_info.size);

    LcoreArgs core_arguments{
        .telemetry = telemetry_metrics.get(),
        .engine = processing_engine.get()
    };

    rte_eal_remote_launch(lcore_network_loop, &core_arguments, CALL_MAIN);
    rte_eal_mp_wait_lcore();

    // 4. Print Telemetry Metrics Summary
    std::cout << "\n============ RAN Engine Telemetry Performance Summary ============\n"
              << " Received Packet Pipelines:  " << telemetry_metrics->rx_packets << "\n"
              << " Transmitted Packet Streams: " << telemetry_metrics->tx_packets << "\n"
              << " Deep Packet Inspected Hits: " << telemetry_metrics->dpi_hits << "\n"
              << " Cipher Processed Workload:  " << telemetry_metrics->crypto_processed_bytes << " bytes\n"
              << "==================================================================\n";

    // 5. Cleanup
    RTE_ETH_FOREACH_DEV(port_id) {
        rte_eth_dev_stop(port_id);
        rte_eth_dev_close(port_id);
    }

    if (bar_ptr) munmap(bar_ptr, bar_info.size);
    if (dev_fd >= 0) close(dev_fd);
    rte_eal_cleanup();

    return 0;
}
