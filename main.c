/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2016 Intel Corporation. All rights reserved.
 *   Copyright(c) 2016-2019 National Institute of Advanced Industrial 
 *                Science and Technology. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/* vim: set noexpandtab ai: */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <math.h>

/*
 * RTE_LIBRTE_RING_DEBUG generates statistics of ring buffers. However, SEGV is occurred. (v16.07）
 * #define RTE_LIBRTE_RING_DEBUG
 */
#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_errno.h>
#include <rte_timer.h>

static int64_t loss_random(const char *loss_rate);
static int64_t loss_random_a(double loss_rate);
static bool loss_event(void);
static bool loss_event_random(uint64_t loss_rate);
static bool loss_event_GE(uint64_t loss_rate_n, uint64_t loss_rate_a, uint64_t st_ch_rate_no2ab, uint64_t st_ch_rate_ab2no);
static bool loss_event_4state( uint64_t p13, uint64_t p14, uint64_t p23, uint64_t p31, uint64_t p32);
static bool dup_event(void);
static uint64_t normal_distribution(uint64_t mean, uint64_t stddev);
#define RANDOM_MAX 1000000000

static volatile bool force_quit;

#define RTE_LOGTYPE_DEMU RTE_LOGTYPE_USER1

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT 128
#define RTE_TEST_TX_DESC_DEFAULT 512
static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

/* ethernet addresses of ports */
static struct ether_addr demu_ports_eth_addr[RTE_MAX_ETHPORTS];

struct rte_mempool * demu_pktmbuf_pool = NULL;

static uint32_t demu_enabled_port_mask = 0;

/* Per-port statistics struct */
struct demu_port_statistics {
	uint64_t tx;
	uint64_t rx;
	uint64_t dropped;
	uint64_t rx_worker_dropped;
	uint64_t worker_tx_dropped;
	uint64_t queue_dropped;
	uint64_t discarded;
} __rte_cache_aligned;
struct demu_port_statistics port_statistics[RTE_MAX_ETHPORTS];

/*
 * Assigment of each thread to a specific CPU core.
 * Currently, each of two threads is running for rx, tx, worker threads.
 */
#define RX_THREAD_CORE 2
#define RX_THREAD_CORE2 3
#define TX_THREAD_CORE 4
#define TX_THREAD_CORE2 5
#define WORKER_THREAD_CORE 6
#define WORKER_THREAD_CORE2 7
#define TIMER_THREAD_CORE 8

/*
 * The maximum number of packets which are processed in burst.
 * Note: do not set PKT_BURST_RX to 1.
 */
#define PKT_BURST_RX 32
#define PKT_BURST_TX 32
#define PKT_BURST_WORKER 32

/*
 * The default mempool size is not enough for bufferijng 64KB of short packets for 1 second.
 * SHORT_PACKET should be enabled in the case of short packet benchmarking.
 * #define SHORT_PACKET
 */
#ifdef SHORT_PACKET
#define DEMU_DELAYED_BUFFER_PKTS 8388608
#define MEMPOOL_BUF_SIZE 1152
#else
#define DEMU_DELAYED_BUFFER_PKTS 4194304
#define MEMPOOL_BUF_SIZE RTE_MBUF_DEFAULT_BUF_SIZE /* 2048 */
#endif

#define MEMPOOL_CACHE_SIZE 512
#define DEMU_SEND_BUFFER_SIZE_PKTS 512

struct rte_ring *rx_to_workers;
struct rte_ring *rx_to_workers2;
struct rte_ring *workers_to_tx;
struct rte_ring *workers_to_tx2;

static uint64_t delayed_value = 0; 
static uint64_t delayed_jitter = 0; 
static uint64_t delayed_time = 0;

enum demu_loss_mode {
	LOSS_MODE_NONE,
	LOSS_MODE_RANDOM,
	LOSS_MODE_GE,
	LOSS_MODE_4STATE,
};
static enum demu_loss_mode loss_mode = LOSS_MODE_NONE;

static uint64_t loss_percent_1 = 0;
static uint64_t loss_percent_2 = 0;
// static uint64_t change_percent_1 = 0;
// static uint64_t change_percent_2 = 0;

static uint64_t dup_rate = 0;

static const struct rte_eth_conf port_conf = {
	.rxmode = {
		.split_hdr_size = 0,
		.header_split   = 0, /**< Header Split disabled */
		.hw_ip_checksum = 0, /**< IP checksum offload disabled */
		.hw_vlan_filter = 0, /**< VLAN filtering disabled */
		.jumbo_frame    = 0, /**< Jumbo Frame Support disabled */
		.hw_strip_crc   = 0, /**< CRC stripped by hardware */
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
};

static struct rte_eth_rxconf rx_conf = {
	.rx_thresh = {                    /**< RX ring threshold registers. */
		.pthresh = 8,             /**< Ring prefetch threshold. */
		.hthresh = 8,             /**< Ring host threshold. */
		.wthresh = 0,             /**< Ring writeback threshold. */
	},
	.rx_free_thresh = 32,             /**< Drives the freeing of RX descriptors. */
	.rx_drop_en = 0,                  /**< Drop packets if no descriptors are available. */
	.rx_deferred_start = 0,           /**< Do not start queue with rte_eth_dev_start(). */
};

static struct rte_eth_txconf tx_conf = {
	.tx_thresh = {                    /**< TX ring threshold registers. */
		.pthresh = 32,
		.hthresh = 0,
		.wthresh = 0,
	},
	.tx_rs_thresh = 32,               /**< Drives the setting of RS bit on TXDs. */
	.tx_free_thresh = 32,             /**< Start freeing TX buffers if there are less free descriptors than this value. */
	.txq_flags = (ETH_TXQ_FLAGS_NOMULTSEGS |
			ETH_TXQ_FLAGS_NOVLANOFFL |
			ETH_TXQ_FLAGS_NOXSUMSCTP |
			ETH_TXQ_FLAGS_NOXSUMUDP |
			ETH_TXQ_FLAGS_NOXSUMTCP),
	.tx_deferred_start = 0,            /**< Do not start queue with rte_eth_dev_start(). */
};

/* #define DEBUG_RX */
/* #define DEBUG_TX */ 

#ifdef DEBUG_RX
#define RX_STAT_BUF_SIZE 3000000
double rx_stat[RX_STAT_BUF_SIZE] = {0};
uint64_t rx_cnt = 0;
#endif

#ifdef DEBUG_TX
#define TX_STAT_BUF_SIZE 3000000
double tx_stat[TX_STAT_BUF_SIZE] = {0};
uint64_t tx_cnt = 0;
#endif


static inline void
pktmbuf_free_bulk(struct rte_mbuf *mbuf_table[], unsigned n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		rte_pktmbuf_free(mbuf_table[i]);
}

static double max_speed = 10000000000.0; /* FIXME: 10Gbps */
static uint64_t limit_speed = 0;
static uint64_t amount_token = 0; /* one token represents capacity of 1 Mbps. */
static uint64_t sub_amount_token = 0;

static void
tx_timer_cb(__attribute__((unused)) struct rte_timer *tmpTime, __attribute__((unused)) void *arg)
{
	double upper_limit_speed = max_speed / 100000;

	if (amount_token >= (uint64_t)upper_limit_speed)
		return;

	if (limit_speed >= 1000000)
		amount_token += (limit_speed / 1000000);
	else {
		sub_amount_token += limit_speed;
		if (sub_amount_token > 1000000) {
			amount_token += sub_amount_token / 1000000;
			sub_amount_token %= 1000000;
		}
	}
}

static void
delay_timer_cb(__attribute__((unused)) struct rte_timer *tmpTime, __attribute__((unused)) void *arg)
{
	//dynamic latency changes latency by normal distribution with delayed jitter as a standard deviation.
	delayed_time = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * normal_distribution(delayed_value, delayed_jitter);
}
static void
demu_timer_loop(void)
{
	unsigned lcore_id;
	uint64_t hz;
	struct rte_timer timer;
	struct rte_timer delay_timer;

	lcore_id = rte_lcore_id();
	hz = rte_get_timer_hz();

	RTE_LOG(INFO, DEMU, "Entering timer loop on lcore %u\n", lcore_id);
	
	if(limit_speed){
		rte_timer_init(&timer);
		rte_timer_reset(&timer, hz / 1000000, PERIODICAL, lcore_id, tx_timer_cb, NULL);

		RTE_LOG(INFO, DEMU, "  Linit speed is %lu bps\n", limit_speed);
	}
	
	if(delayed_jitter){
		rte_timer_init(&delay_timer);
		rte_timer_reset(&delay_timer, hz, PERIODICAL, lcore_id, delay_timer_cb, NULL);

		RTE_LOG(INFO, DEMU, "  Delayed time is %lu us with delay jitter %lu us\n", delayed_value, delayed_jitter);
	}


	while (!force_quit)
		rte_timer_manage();
}

static void
demu_tx_loop(unsigned portid)
{
	struct rte_mbuf *send_buf[PKT_BURST_TX];
	struct rte_ring **cring;
	unsigned lcore_id;
	uint32_t numdeq = 0;
	uint16_t sent;

	lcore_id = rte_lcore_id();

	RTE_LOG(INFO, DEMU, "Entering main tx loop on lcore %u portid %u\n", lcore_id, portid);

	while (!force_quit) {
		if (portid == 0)
			cring = &workers_to_tx;
		else
			cring = &workers_to_tx2;
		
		numdeq = rte_ring_sc_dequeue_burst(*cring,
				(void *)send_buf, PKT_BURST_TX, NULL);

		if (unlikely(numdeq == 0))
			continue;

		sent = 0;
		while (numdeq > sent)
			sent += rte_eth_tx_burst(portid, 0, send_buf + sent, numdeq - sent);

#ifdef DEBUG_TX
		if (tx_cnt < TX_STAT_BUF_SIZE) {
			for (uint32_t i = 0; i < numdeq; i++) {
				tx_stat[tx_cnt] = rte_rdtsc();
				tx_cnt++;
			}
		}
#endif
	}
}

static void
demu_rx_loop(unsigned portid)
{
	struct rte_mbuf *pkts_burst[PKT_BURST_RX], *rx2w_buffer[PKT_BURST_RX];
	unsigned lcore_id;

	unsigned nb_rx, i;
	unsigned nb_loss;
	unsigned nb_dup;
	uint32_t numenq;

	lcore_id = rte_lcore_id();

	RTE_LOG(INFO, DEMU, "Entering main rx loop on lcore %u portid %u\n", lcore_id, portid);

	while (!force_quit) {
		nb_rx = rte_eth_rx_burst((uint8_t) portid, 0, pkts_burst, PKT_BURST_RX);

		if (likely(nb_rx == 0))
			continue;

		nb_loss = 0;
		nb_dup = 0;
		for (i = 0; i < nb_rx; i++) {
			struct rte_mbuf *clone;

			if (portid == 0 && loss_event()) {
				port_statistics[portid].discarded++;
				nb_loss++;
				continue;
			}

			rx2w_buffer[i - nb_loss + nb_dup] = pkts_burst[i];
			rte_prefetch0(rte_pktmbuf_mtod(rx2w_buffer[i - nb_loss + nb_dup], void *));
			rx2w_buffer[i - nb_loss + nb_dup]->udata64 = rte_rdtsc();

			/* FIXME: we do not check the buffer overrun of rx2w_buffer. */
			if (portid == 0 && dup_event()) {
				clone = rte_pktmbuf_clone(rx2w_buffer[i - nb_loss + nb_dup], demu_pktmbuf_pool);
				if (clone == NULL)
					RTE_LOG(ERR, DEMU, "cannot clone a packet\n");
				nb_dup++;
				rx2w_buffer[i - nb_loss + nb_dup] = clone;
			}

#ifdef DEBUG_RX
			if (rx_cnt < RX_STAT_BUF_SIZE) {
				rx_stat[rx_cnt] = rte_rdtsc();
				rx_cnt++;
			}
#endif

		}

		if (portid == 0)
			numenq = rte_ring_sp_enqueue_burst(rx_to_workers,
					(void *)rx2w_buffer, nb_rx - nb_loss + nb_dup, NULL);
		else
			numenq = rte_ring_sp_enqueue_burst(rx_to_workers2,
					(void *)rx2w_buffer, nb_rx - nb_loss + nb_dup, NULL);


		if (unlikely(numenq < (unsigned)(nb_rx - nb_loss + nb_dup))) {
			RTE_LOG(WARNING, DEMU, "Delayed Queue Overflow count: %d\n",
				(nb_rx - nb_loss + nb_dup) - numenq);
			pktmbuf_free_bulk(&pkts_burst[numenq], nb_rx - nb_loss + nb_dup - numenq);
		}
	}
}

static void
worker_thread(unsigned portid)
{
	uint16_t burst_size = 0;
	struct rte_mbuf *burst_buffer[PKT_BURST_WORKER];
	uint64_t diff_tsc;
	int i;
	unsigned lcore_id;
	int status;

	lcore_id = rte_lcore_id();
	RTE_LOG(INFO, DEMU, "Entering main worker on lcore %u\n", lcore_id);

	while (!force_quit) {
		if (portid == 0)
			burst_size = rte_ring_sc_dequeue_burst(rx_to_workers,
					(void *)burst_buffer, PKT_BURST_WORKER, NULL);
		else
			burst_size = rte_ring_sc_dequeue_burst(rx_to_workers2,
					(void *)burst_buffer, PKT_BURST_WORKER, NULL);
		if (unlikely(burst_size == 0))
			continue;

		i = 0;
		while (i != burst_size) {
			/* Add a given delay when a packet comes from the port 0.
			 * FIXME: fix this implementation.
			 */
			if (portid == 0) {

				rte_prefetch0(rte_pktmbuf_mtod(burst_buffer[i], void *));
				diff_tsc = rte_rdtsc() - burst_buffer[i]->udata64;
				if (diff_tsc < delayed_time)
					continue;

				if (limit_speed) {
					uint16_t pkt_size_bit = burst_buffer[i]->pkt_len * 8;

					if (amount_token >= pkt_size_bit)
						amount_token -= pkt_size_bit;
					else
						continue;
				}
				do {
					status = rte_ring_sp_enqueue(workers_to_tx2, burst_buffer[i]);
				} while (status == -ENOBUFS);
				i++;
			} else {
				do {
					status = rte_ring_sp_enqueue(workers_to_tx, burst_buffer[i]);
				} while (status == -ENOBUFS);
				i++; 
			}
		}
	}
}

static int
demu_launch_one_lcore(__attribute__((unused)) void *dummy)
{
	unsigned lcore_id;
	lcore_id = rte_lcore_id();

	if (lcore_id == TX_THREAD_CORE) 
		demu_tx_loop(1);

	else if (lcore_id == TX_THREAD_CORE2)
		demu_tx_loop(0);

	else if (lcore_id == WORKER_THREAD_CORE)
		worker_thread(0);

	else if (lcore_id == WORKER_THREAD_CORE2)
		worker_thread(1);	

	else if (lcore_id == RX_THREAD_CORE)
		demu_rx_loop(1);

	else if (lcore_id == RX_THREAD_CORE2)
		demu_rx_loop(0);

	else if ((limit_speed || delayed_jitter) && lcore_id == TIMER_THREAD_CORE)
		demu_timer_loop();

	if (force_quit)
		return 0;

	return 0;
}

/* display usage */
static void
demu_usage(const char *prgname)
{
	printf("%s [EAL options] -- -d Delayed time [us] (default is 0s)\n"
		" -p PORTMASK: HEXADECIMAL bitmask of ports to configure\n"
		" -r random packet loss %% (default is 0%%)\n"
		" -g XXX\n"
		" -s bandwidth limitation [bps]\n"
		" -D duplicate packet rate\n",
		prgname);
}

static int
demu_parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;

	return pm;
}

static int
demu_parse_delayed(const char *q_arg)
{
	char *end = NULL;
	int n;

	/* parse number string */
	n = strtol(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;

	return n;
}

static int
demu_parse_jitter(const char *q_arg)
{
	char *end = NULL;
	int n;

	/* parse number string */
	n = strtol(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;
	
	return n;
}


static int64_t
demu_parse_speed(const char *arg)
{
	int64_t speed, base = 1;
	char *end = NULL;

	speed = strtoul(arg, &end, 10);
	if (end != NULL) {
		char unit = *end;

		switch (unit) {
			case 'k':
			case 'K':
				base = 1000;
				break;
			case 'm':
			case 'M':
				base = 1000 * 1000;
				break;
			case 'g':
			case 'G':
				if (speed > 10) return -1;
				base = 1000 * 1000 * 1000;
				break;
			default:
				return -1;
		}
		end++;
	}

	if (arg[0] == '\0' || end == NULL || *end != '\0') {
		return -1;
	}

	speed = speed * base;
	if (speed < 1000 && 10000000000 < speed)
		return -1;

	return speed;
}

/* Parse the argument given in the command line of the application */
static int
demu_parse_args(int argc, char **argv)
{
	int opt, ret;
	char **argvopt;
	char *prgname = argv[0];
	const struct option longopts[] = {
		{0, 0, 0, 0}
	};
	int longindex = 0;
	int64_t val;

	argvopt = argv;

	while ((opt = getopt_long(argc, argvopt, "d:g:p:r:s:D:j:",
					longopts, &longindex)) != EOF) {

		switch (opt) {
			/* portmask */
			case 'p':
				demu_enabled_port_mask = demu_parse_portmask(optarg);
				if (demu_enabled_port_mask <= 0) {
					printf("Invalid value: portmask\n");
					demu_usage(prgname);
					return -1;
				}
				break;

			/* delayed packet */
			case 'd':
				val = demu_parse_delayed(optarg);
				if (val < 0) {
					printf("Invalid value: delayed time\n");
					demu_usage(prgname);
					return -1;
				}
				
				/* Global variable for values */
				delayed_value = val;
				delayed_time = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * val;
				break;

			/* random packet loss */
			case 'r':
				val = loss_random(optarg);
				if (val < 0) {
					printf("Invalid value: loss rate\n");
					demu_usage(prgname);
					return -1;
				}
				loss_percent_1 = val;
				loss_mode = LOSS_MODE_RANDOM;
				break;

			case 'g':
				val = loss_random(optarg);
				if (val < 0) {
					printf("Invalid value: loss rate\n");
					demu_usage(prgname);
					return -1;
				}
				loss_percent_2 = val;
				loss_mode = LOSS_MODE_GE;
				break;

			/* duplicate packet */
			case 'D':
				val = loss_random(optarg);
				if (val < 0) {
					printf("Invalid value: loss rate\n");
					demu_usage(prgname);
					return -1;
				}
				dup_rate = val;
				break;

			/* bandwidth limitation */
			case 's':
				val = demu_parse_speed(optarg);
				if (val < 0) {
					RTE_LOG(ERR, DEMU, "Invalid value: speed\n");
					return -1;
				}
				limit_speed = val;
				break;

			/* delayed jitter */
			case 'j':
				val = demu_parse_jitter(optarg);
				if (val < 0) {
					printf("Invalid value: jitter\n");
					demu_usage(prgname);
					return -1;
				}
				
				delayed_jitter = val;
				break;

			/* long options */
			case 0:
				demu_usage(prgname);
				return -1;

			default:
				demu_usage(prgname);
				return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind-1;
	optind = 0; /* reset getopt lib */
	return ret;
}

static void
check_all_ports_link_status(uint8_t port_num, uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint8_t portid, count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	RTE_LOG(INFO, DEMU, "Checking link status\n");
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		if (force_quit)
			return;
		all_ports_up = 1;
		for (portid = 0; portid < port_num; portid++) {
			if (force_quit)
				return;
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(portid, &link);
			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status)
					RTE_LOG(INFO, DEMU, "  Port %d Link Up - speed %u "
						"Mbps - %s\n", (uint8_t)portid,
						(unsigned)link.link_speed,
						(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
						("full-duplex") : ("half-duplex\n"));
				else
					RTE_LOG(INFO, DEMU, "  Port %d Link Down\n",
						(uint8_t)portid);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0)
			rte_delay_ms(CHECK_INTERVAL);

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1))
			print_flag = 1;
	}
}

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		RTE_LOG(NOTICE, DEMU, "Signal %d received, preparing to exit...\n",
				signum);
		force_quit = true;
	}
}

int
main(int argc, char **argv)
{
	int ret;
	uint8_t nb_ports;
	uint8_t portid;
	unsigned lcore_id;

	/* init EAL */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	argc -= ret;
	argv += ret;

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* parse application arguments (after the EAL ones) */
	ret = demu_parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid DEMU arguments\n");

	/* create the mbuf pool */
	demu_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool",
			DEMU_DELAYED_BUFFER_PKTS * 2 + DEMU_SEND_BUFFER_SIZE_PKTS * 2,
			MEMPOOL_CACHE_SIZE, 0, MEMPOOL_BUF_SIZE,
			rte_socket_id());

	if (demu_pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

	nb_ports = rte_eth_dev_count();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");

	if (nb_ports > RTE_MAX_ETHPORTS)
		nb_ports = RTE_MAX_ETHPORTS;

	/* Initialise each port */
	for (portid = 0; portid < nb_ports; portid++) {
		/* init port */
		RTE_LOG(INFO, DEMU, "Initializing port %u\n", (unsigned) portid);
		ret = rte_eth_dev_configure(portid, 1, 1, &port_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
					ret, (unsigned) portid);

		rte_eth_macaddr_get(portid,&demu_ports_eth_addr[portid]);

		/* init one RX queue */
		ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
				rte_eth_dev_socket_id(portid),
				&rx_conf,
				demu_pktmbuf_pool);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
					ret, (unsigned) portid);

		/* init one TX queue on each port */
		ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
				rte_eth_dev_socket_id(portid),
				&tx_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
					ret, (unsigned) portid);

		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
					ret, (unsigned) portid);

		rte_eth_promiscuous_enable(portid);

		RTE_LOG(INFO, DEMU, "  Port %u, MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n",
			(unsigned) portid,
			demu_ports_eth_addr[portid].addr_bytes[0],
			demu_ports_eth_addr[portid].addr_bytes[1],
			demu_ports_eth_addr[portid].addr_bytes[2],
			demu_ports_eth_addr[portid].addr_bytes[3],
			demu_ports_eth_addr[portid].addr_bytes[4],
			demu_ports_eth_addr[portid].addr_bytes[5]);

		/* initialize port stats */
		memset(&port_statistics, 0, sizeof(port_statistics));

	}	

	check_all_ports_link_status(nb_ports, demu_enabled_port_mask);



	rx_to_workers = rte_ring_create("rx_to_workers", DEMU_DELAYED_BUFFER_PKTS,
			rte_socket_id(),   RING_F_SP_ENQ | RING_F_SC_DEQ);
	if (rx_to_workers == NULL)
		rte_exit(EXIT_FAILURE, "%s\n", rte_strerror(rte_errno));

	workers_to_tx = rte_ring_create("workers_to_tx", DEMU_SEND_BUFFER_SIZE_PKTS,
			rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
	if (workers_to_tx == NULL)
		rte_exit(EXIT_FAILURE, "%s\n", rte_strerror(rte_errno));

	rx_to_workers2 = rte_ring_create("rx_to_workers2", DEMU_DELAYED_BUFFER_PKTS,
			rte_socket_id(),   RING_F_SP_ENQ | RING_F_SC_DEQ);

	if (rx_to_workers2 == NULL)
		rte_exit(EXIT_FAILURE, "%s\n", rte_strerror(rte_errno));

	workers_to_tx2 = rte_ring_create("workers_to_tx2", DEMU_SEND_BUFFER_SIZE_PKTS,
			rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
	if (workers_to_tx2 == NULL)
		rte_exit(EXIT_FAILURE, "%s\n", rte_strerror(rte_errno));



	ret = 0;
	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(demu_launch_one_lcore, NULL, CALL_MASTER);
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0) {
			ret = -1;
			break;
		}
	}

	for (portid = 0; portid < nb_ports; portid++) {
		struct rte_eth_stats stats;
		/* if ((demu_enabled_port_mask & (1 << portid)) == 0) */
		/* 	continue; */
		RTE_LOG(INFO, DEMU, "Closing port %d\n", portid);
		rte_eth_stats_get(portid, &stats);
		RTE_LOG(INFO, DEMU, "port %d: in pkt: %ld out pkt: %ld in missed: %ld in errors: %ld out errors: %ld\n",
			portid, stats.ipackets, stats.opackets, stats.imissed, stats.ierrors, stats.oerrors);
		rte_eth_dev_stop(portid);
		rte_eth_dev_close(portid);
	}




#if defined(DEBUG_RX) || defined(DEBUG_TX)
	time_t timer;
	struct tm *timeptr;
	timer = time(NULL);
	timeptr = localtime(&timer);
#endif
#ifdef DEBUG_RX
	char filename1[64] = {'\0'};
	strftime(filename1, 64, "/home/aketa/result/rxtime%m%d%H%M%S", timeptr);
	FILE *rxoutput;
	if ((rxoutput = fopen(filename1, "a+")) == NULL) {
		printf("file open error!!\n");
		exit(EXIT_FAILURE);
	}
	for (uint64_t i = 0; i < rx_cnt - 1 ; i++) {
		fprintf(rxoutput, "%lf\n", rx_stat[i]);
	}
	fclose(rxoutput);
#endif
#ifdef DEBUG_TX
	char filename2[64] = {'\0'}; 
	strftime(filename2, 64, "/home/aketa/result/txtime%m%d%H%M%S", timeptr);
	FILE *txoutput;
	if ((txoutput = fopen(filename2, "a+")) == NULL) {
		printf("file open error!!\n");
		exit(EXIT_FAILURE);
	}
	for (uint64_t i = 0; i < tx_cnt - 1 ; i++) {
		fprintf(txoutput, "%lf\n", tx_stat[i]);
	}
	fclose(txoutput);
#endif


	/* rte_ring_dump(stdout, rx_to_workers); */
	/* rte_ring_dump(stdout, workers_to_tx); */

	RTE_LOG(INFO, DEMU, "Bye...\n");

	return ret;
}

static int64_t
loss_random_a(double loss_rate)
{
	double percent;
	uint64_t percent_u64;

	percent = loss_rate;
	percent *= RANDOM_MAX / 100;
	percent_u64 = (uint64_t)percent;

	return percent_u64;
}

static int64_t
loss_random(const char *loss_rate)
{
	double percent;
	uint64_t percent_u64;

	if (sscanf(loss_rate, "%lf", &percent) == 0)
		return -1;
	percent *= RANDOM_MAX / 100;
	percent_u64 = (uint64_t)percent;

	return percent_u64;
}

static bool
loss_event(void)
{
	bool lost = false;

	switch (loss_mode) {
	case LOSS_MODE_NONE:
		break;

	case LOSS_MODE_RANDOM:
		if (unlikely(loss_event_random(loss_percent_1) == true))
			lost = true;
		break;

	case LOSS_MODE_GE:
		if (unlikely(loss_event_GE(loss_random_a(0), loss_random_a(100),
			loss_percent_1, loss_percent_2) == true))
			lost = true;
		break;

	case LOSS_MODE_4STATE: /* FIX IT */
		if (unlikely(loss_event_4state(loss_random_a(100), loss_random_a(0),
			loss_random_a(100), loss_random_a(0), loss_random_a(1)) == true))
			lost = true;
		break;
	}

	return lost;
}

static bool
loss_event_random(uint64_t loss_rate)
{
	bool flag = false;
	uint64_t temp;

	temp = rte_rand() % (RANDOM_MAX + 1);
	if (loss_rate >= temp)
		flag = true;

	return flag;
}

/*
 * Gilbert Elliott loss model
 * 0: S_NOR (normal state, low loss ratio)
 * 1: S_ABN (abnormal state, high loss ratio)
 */
static bool
loss_event_GE(uint64_t loss_rate_n, uint64_t loss_rate_a, uint64_t st_ch_rate_no2ab, uint64_t st_ch_rate_ab2no)
{
#define S_NOR 0
#define S_ABN 1
	static bool state = S_NOR; 
	uint64_t rnd_loss, rnd_tran;
	uint64_t loss_rate, state_ch_rate;
	bool flag = false;

	if (state == S_NOR) {
		loss_rate = loss_rate_n;
		state_ch_rate = st_ch_rate_no2ab;
	} else { // S_ABN
		loss_rate = loss_rate_a;
		state_ch_rate = st_ch_rate_ab2no;
	}

	rnd_loss = rte_rand() % (RANDOM_MAX + 1);
	if (rnd_loss < loss_rate) {
		flag = true;
	}

	rnd_tran = rte_rand() % (RANDOM_MAX + 1);
	if (rnd_tran < state_ch_rate) {
		state = !state;
	}

	return flag;
}

/*
 * Four-state Markov model
 * State 1 - Packet is received successfully in gap period
 * State 2 - Packet is received within a burst period
 * State 3 - Packet is lost within a burst period
 * State 4 - Isolated packet lost within a gap period
 * p13 is the probability of state change from state1 to state3.
 * https://www.gatesair.com/documents/papers/Parikh-K130115-Network-Modeling-Revised-02-05-2015.pdf
 */
static bool
loss_event_4state( uint64_t p13, uint64_t p14, uint64_t p23, uint64_t p31, uint64_t p32)
{
	static char state = 1; 
	bool flag = false;
	uint64_t rnd = rte_rand() % (RANDOM_MAX + 1);

	switch (state) {
	case 1:
		if (rnd < p13) {
			state = 3;
		} else if (rnd < p13 + p14) {
			state = 4;
		}
		break;

	case 2:
		if (rnd < p23) {
			state = 3;
		}
		break;
 
	case 3:
		if (rnd < p31) {
			state = 1;
		} else if (rnd < p31 + p32) {
			state = 2;
		}
		break;
 
	case 4:
		state = 1;
		break;
	}

	if (state == 2 || state == 4) {
		flag = true;
	}

	return flag;
}

static bool
dup_event(void)
{
	if (unlikely(loss_event_random(dup_rate) == true))
		return true;
	else
		return false;
}

static uint64_t normal_distribution(uint64_t mean, uint64_t stddev)
{
    static double spare;
    static int hasSpare = 0;

    if (hasSpare) {
        hasSpare = 0;
        return spare * stddev + mean;
    } else {
        double u, v, s;
        do {
            u = (rand() / ((double)RAND_MAX)) * 2.0 - 1.0;
            v = (rand() / ((double)RAND_MAX)) * 2.0 - 1.0;
            s = u * u + v * v;
        } while (s >= 1.0 || s == 0.0);
        s = sqrt(-2.0 * log(s) / s);
        spare = v * s;
        hasSpare = 1;
        return mean + stddev * u * s;
    }
}