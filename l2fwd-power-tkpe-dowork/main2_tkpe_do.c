/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2016 Intel Corporation. All rights reserved.
 *   All rights reserved.
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
#include <rte_mempool.h>
#include <rte_mbuf.h>

//#define RTE_LIBRTE_POWER_DEBUG

#include <rte_power.h>
#include <rte_timer.h>
#include <rte_spinlock.h>

static volatile bool force_quit;

#define RTE_LOGTYPE_L3FWD_POWER RTE_LOGTYPE_USER1

/* MAC updating disabled by default */
static int mac_updating = 0;

#define RTE_LOGTYPE_L2FWD RTE_LOGTYPE_USER1

#define NB_MBUF   8192

#define MAX_PKT_BURST 32
#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */
#define MEMPOOL_CACHE_SIZE 256
#define MIN_ZERO_POLL_COUNT 10

#define TEST_POWER_FREQS_NUM_MAX ((unsigned)RTE_MAX_LCORE_FREQS)
static uint32_t total_freq_num;
static uint32_t freqs[TEST_POWER_FREQS_NUM_MAX];

static int timer_per_second;
/* can change according to the realistic traffic and sample interval */
static int timer_per_epoch = 4;
/* the resolution of hardware tsc timer */
static uint64_t timerhz;

#define CIO            45
#define CCALL          43
#define CV             24
#define CLIFF          0.8
/* Similar to hysteresis loop, avoid frequent adjustments */
/* due to the use of a single threshold */
#define ADDJUST_FREQ_THRESHOLD 0.75 
#define MAX_TIMER_PERIOD 86400 /* 1 day max */
/* max arrival rate in kpps for a 10Gbps ethernet link */
#define MAX_ARRIVAL_RATE 14881


/*
 * Configurable number of RX/TX ring descriptors
 */
//#define RTE_TEST_RX_DESC_DEFAULT 128
#define RTE_TEST_RX_DESC_DEFAULT 512
#define RTE_TEST_TX_DESC_DEFAULT 512
static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

/* ethernet addresses of ports */
static struct ether_addr l2fwd_ports_eth_addr[RTE_MAX_ETHPORTS];
/* ethernet addresses of ports */
static rte_spinlock_t locks[RTE_MAX_ETHPORTS];

/* mask of enabled ports */
static uint32_t l2fwd_enabled_port_mask = 0;

/* list of enabled ports */
static uint32_t l2fwd_dst_ports[RTE_MAX_ETHPORTS];

static unsigned int l2fwd_rx_queue_per_lcore = 1;


struct lcore_rx_queue {
    uint8_t port_id;
    uint8_t queue_id;
} __rte_cache_aligned;


static struct{
    char* traffic_table_name;
    int sample_interval;
    double avgBatch;
    int app_cycles;
    double ma;
    int num_traffic;
    double *traffic;
    int start_epoch;
} tkpe_config;

#define MAX_RX_QUEUE_PER_LCORE 16
#define MAX_TX_QUEUE_PER_PORT 16

//static uint64_t n_freqchange=0;
//static uint64_t sleepnum = 0;

struct lcore_queue_conf {
    unsigned n_rx_port;
	struct lcore_rx_queue rx_port_list[MAX_RX_QUEUE_PER_LCORE];
} __rte_cache_aligned;
struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE] __rte_cache_aligned;

static struct rte_eth_dev_tx_buffer *tx_buffer[RTE_MAX_ETHPORTS];

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

#define CMD_LINE_OPT_MAC_UPDATING "mac-updating"
#define CMD_LINE_OPT_NO_MAC_UPDATING "no-mac-updating"
#define OPTION_TRAFFIC_TABLE	"history_traffic"
#define OPTION_SAMPLE_INTERVAL  "sample_interval"
#define OPTION_AVG_BARRIVAL_SIZE "avg_batch_size"
#define OPTION_START_EPOCH "start_epoch"
#define OPTION_APP_CYCLES "app_cycles"
#define OPTION_MA "moving_average"


struct rte_mempool * l2fwd_pktmbuf_pool = NULL;


struct lcore_stats {
    double rho;
    uint16_t Nap_Cycles;
    uint64_t nb_idle_looped;
    /* total packet processed recently */
    uint64_t nb_rx_processed;
    /* total iterations looped recently */
    uint64_t nb_iteration_looped;
    /* current epoch; total epoch: tkpe_config.num_traffic */
    uint32_t epoch;
    uint32_t round;
} __rte_cache_aligned;
static struct lcore_stats stats[RTE_MAX_LCORE] __rte_cache_aligned;

static struct rte_timer power_timers[RTE_MAX_LCORE];


/* Per-port statistics struct */
struct l2fwd_port_statistics {
	uint64_t tx;
	uint64_t rx;
	uint64_t dropped;
} __rte_cache_aligned;
struct l2fwd_port_statistics port_statistics[RTE_MAX_ETHPORTS];

#define MAX_TIMER_PERIOD 86400 /* 1 day max */


static
inline void do_cycles(uint32_t cycles) {
    __asm__ __volatile__ (" movl %0, %%ecx \n\t"
                      " lb%=: decl %%ecx \n\t"
                      " cmpl $0, %%ecx \n\t"
                      " jnz lb%=" : : "r"(cycles):"%ecx" ) ;
}


static inline void
l2fwd_mac_updating(struct rte_mbuf *m, unsigned dst_portid)
{
	struct ether_hdr *eth;
	eth = rte_pktmbuf_mtod(m, struct ether_hdr *);
    /* ether_addr_copy(from_addr, to_addr) */
    ether_addr_copy(&eth->d_addr, &eth->s_addr);
	ether_addr_copy(&l2fwd_ports_eth_addr[dst_portid], &eth->d_addr);
}

static void
do_some_work(struct rte_mbuf *m, unsigned portid)
{
	unsigned dst_port;
    struct rte_eth_dev_tx_buffer *buffer;
	dst_port = l2fwd_dst_ports[portid];

	if (mac_updating)           /* mac_updating will cost about 17 cycles */
		l2fwd_mac_updating(m, dst_port);

    //do_cycles(tkpe_config.app_cycles-25); /* do_cycles itself will cost about 25cycles, still do not know the reason */
    do_cycles(tkpe_config.app_cycles);
	buffer = tx_buffer[dst_port];
    rte_eth_tx_buffer(dst_port, 0, buffer, m);
}


/* __SSE2__ may be supported or not; deceided by __SSE2__ defined in gcc */
static inline void nap(void){
#ifdef __SSE2__
    __asm__ __volatile__("pause\n\t" ::: "memory");
#else
    __asm__ __volatile__("rep;nop\n\t": : : "memory"); 
#endif
}


static inline void
nap_cycles(uint64_t cycles)
{
    while( cycles-- > 0 ){
        nap();
    }
}

/* main processing loop */
static void
l2fwd_main_loop(void)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	struct rte_mbuf *m;

	unsigned lcore_id;
	unsigned i, j, portid, destportid, nb_rx, nb_tx, queueid;
	uint64_t prev_tsc_power = 0, cur_tsc_power, diff_tsc_power;
	
	struct lcore_rx_queue *rx_queue;
	struct lcore_queue_conf *qconf;
	struct rte_eth_dev_tx_buffer *buffer;


	lcore_id = rte_lcore_id();
	qconf = &lcore_queue_conf[lcore_id];

	memset(freqs,0,sizeof(freqs));
    if( rte_power_freqs){
        RTE_LOG(INFO, L2FWD, "test rte_power_freqs\n" );
        total_freq_num = rte_power_freqs(lcore_id,freqs,TEST_POWER_FREQS_NUM_MAX);
        /* for(i = 0; i < total_freq_num; i++){ */
        /*     RTE_LOG(INFO, L2FWD, "Freq %u: %u\n",i,freqs[i] ); */
        /* } */
    }
	

    int recv = 0;
	int mini_period = 14881;    /* 0.1 percent of line rate, adjusting according to the real traffic and will be set to 1/10 of real traffic arrival rate*/
	uint64_t idle_num_for_timer = 100000;
	int new_mini_period = 0;


	if (qconf->n_rx_port == 0) {
		RTE_LOG(INFO, L2FWD, "lcore %u has nothing to do\n", lcore_id);
		return;
	}

	RTE_LOG(INFO, L2FWD, "entering main loop on lcore %u\n", lcore_id);

	for (i = 0; i < qconf->n_rx_port; i++) {
		portid = qconf->rx_port_list[i].port_id;
		RTE_LOG(INFO, L2FWD, " -- lcoreid=%u portid=%u\n", lcore_id,
                portid);

	}

    uint64_t timer_resolution_cycles = (uint64_t)(timerhz/timer_per_second);
    printf("tiemr resolution cycles: %llu\n", timer_resolution_cycles);
    
	while (!force_quit) {

        /* we let the mini_period be about 1/10 of the timer resolution cycles,
           then we just call about 10 times rte_rdtsc() per timer period;
           as a result of the above design for timer wake-up, the CPU may keep running 
           with a high frequency if there's no packet arriving (the rte_timer_manage() will no be invoked),
           then we add the idle num counter;
         */
        if( unlikely( recv >= mini_period  || stats[lcore_id].nb_idle_looped > idle_num_for_timer )){
            new_mini_period += recv;
            cur_tsc_power = rte_rdtsc();
            diff_tsc_power = cur_tsc_power - prev_tsc_power;
            if (diff_tsc_power >  timer_resolution_cycles ) {
                mini_period = new_mini_period/20;
                new_mini_period = 0;
                idle_num_for_timer = stats[lcore_id].nb_idle_looped*0.7;
                rte_timer_manage();
                prev_tsc_power = cur_tsc_power;
            }
            idle_num_for_timer = stats[lcore_id].nb_idle_looped + 100000;
            recv = 0;
        }
        
        for (i = 0; i < qconf->n_rx_port; i++) {
	    
            rx_queue = &(qconf->rx_port_list[i]);
            portid = rx_queue->port_id;
            queueid = rx_queue->queue_id;

            nb_rx = rte_eth_rx_burst((uint8_t) portid, 0, pkts_burst, MAX_PKT_BURST);
            
            if (unlikely(nb_rx == 0)){
                /*no packet received from rx queue, try to Nap */
                stats[lcore_id].nb_idle_looped++;
                if( stats[lcore_id].Nap_Cycles > 0)
                    nap_cycles(stats[lcore_id].Nap_Cycles);
                continue;
            }

            stats[lcore_id].nb_rx_processed += nb_rx;
            recv += nb_rx;
	    
            for (j = 0; j < nb_rx; j++) {
                m = pkts_burst[j];
                rte_prefetch0(rte_pktmbuf_mtod(m, void *));
                do_some_work(m, portid);
            }

            destportid = l2fwd_dst_ports[portid];
            buffer = tx_buffer[destportid];
            rte_eth_tx_buffer_flush(destportid, 0, buffer);
            
        }//for
        
        /* for (i = 0; i < qconf->n_rx_port; i++) { */
        /*     portid = l2fwd_dst_ports[qconf->rx_port_list[i].port_id]; */
        /*     buffer = tx_buffer[portid]; */
        /*     rte_eth_tx_buffer_flush(portid, 0, buffer); */
        /* } */
        
	}//while
}//func

static int
l2fwd_launch_one_lcore(__attribute__((unused)) void *dummy)
{
	l2fwd_main_loop();
	return 0;
}


static int
add_traffic_table(const char *file_path)
{
	char buff[LINE_MAX];
	FILE *fh = fopen(file_path, "rb");
	unsigned int i = 0;
    double* traffic;
	if (fh == NULL)
		rte_exit(EXIT_FAILURE, "%s: Open %s failed\n", __func__,
                 file_path);

	while ((fgets(buff, LINE_MAX, fh) != NULL)) i++;

    tkpe_config.num_traffic = i;
    //printf("*********** num_traffic: %d *************\n",tkpe_config.num_traffic);
    traffic = (double*)calloc(i, sizeof(double));

    if (NULL == traffic)
		rte_exit(EXIT_FAILURE, "%s: failed to calloc memory\n",
                 __func__);

    
	fseek(fh, 0, SEEK_SET);
	i = 0;
	while (fgets(buff, LINE_MAX, fh) != NULL) {
        char *end = NULL;
        double t;
        t = strtod(buff, &end);
        if ((buff[0] == '\0') || (end == NULL) || ( (*end != '\n') &&  (*end != '\0') ) ){
            rte_exit(EXIT_FAILURE,
                     "%s Line %u: historical traffic value illegal:%s\n",
                     file_path, i, buff);
        }
        traffic[i] = t;
        i++;
	}
    tkpe_config.traffic = traffic;
	fclose(fh);
	return 0;
}


static void
dump_tkpe_config(void)
{
	printf("TKPE-DPDK option are:\n");
	printf(OPTION_TRAFFIC_TABLE": %s\n", tkpe_config.traffic_table_name);
	printf(OPTION_SAMPLE_INTERVAL": %d\n", tkpe_config.sample_interval);
    printf(OPTION_AVG_BARRIVAL_SIZE": %f\n", tkpe_config.avgBatch);
    printf(OPTION_START_EPOCH": %d\n", tkpe_config.start_epoch);
    printf(OPTION_APP_CYCLES": %d\n", tkpe_config.app_cycles);
    printf(OPTION_MA": %f\n", tkpe_config.ma);
}

static void
dump_tkpe_traffic(void)
{
    int i;
	printf("TKPE-DPDK historical traffic, total %d items: \n", tkpe_config.num_traffic);
	for(i=0; i < tkpe_config.num_traffic; i ++){
        printf("%-8.4f ", tkpe_config.traffic[i]);
        if(i % 5 == 4 ) printf("\n");
    }
}

static int
check_tkpe_config(void)
{
    /* const char *traffic_table_name; */
    /* int sample_interval; */
    /* double avgBatch; */
    
	if (tkpe_config.traffic_table_name == NULL) {
		printf("TKPE-DPDK historical traffic file not specified\n");
		return -1;
	} else if (tkpe_config.sample_interval <= 0) {
		printf("TKPE-DPDK historical traffic sample interval not specified or invalid\n");
		return -1;
	} else if (tkpe_config.avgBatch <= 0) {
		printf("TKPE-DPDK average arrival batch size not specified or invalid\n");
		return -1;
	}else if(tkpe_config.num_traffic < tkpe_config.start_epoch || tkpe_config.start_epoch < 0 ){
        printf("TKPE-DPDK start epcoh exceeding the num of sample traffic or less than f zero\n");
		return -1;
    }else if (tkpe_config.app_cycles <= 0) {
		printf("TKPE-DPDK app cycles not specified or invalid\n");
		return -1;
    }
	return 0;
}

static int
app_tkpe_init(void)
{
    add_traffic_table(tkpe_config.traffic_table_name);
    dump_tkpe_traffic();
    
    if (check_tkpe_config() != 0)
		rte_exit(EXIT_FAILURE, "Failed to get valid TKPE-DPDK options\n");
    
	dump_tkpe_config();
    return 0;
}

/* display usage */
static void
l2fwd_usage(const char *prgname)
{
	printf("%s [EAL options] -- -p PORTMASK [-q NQ]\n"
	       "  -p PORTMASK: hexadecimal bitmask of ports to configure\n"
	       "  -q NQ: number of queue (=ports) per lcore (default is 1)\n"
		   "  -T PERIOD: statistics will be refreshed each PERIOD seconds (0 to disable, 10 default, 86400 maximum)\n"
		   "  --[no-]mac-updating: Enable or disable MAC addresses updating (enabled by default)\n"
		   "      When enabled:\n"
		   "       - The source MAC address is replaced by the TX port MAC address\n"
		   "       - The destination MAC address is replaced by the source MAC addres\n"
           "       - as we just send the packets back to the source.\n"
           "  --"OPTION_TRAFFIC_TABLE"=FILE: file stored the historical traffic load information \n"
           "  --"OPTION_SAMPLE_INTERVAL"=Interger: historical traffic sampling interval in seconds\n"
           "  --"OPTION_AVG_BARRIVAL_SIZE"=Double: average batch arrival size\n"
           "  --"OPTION_MA"=Double: moving average param to estimate traffic load\n",
	       prgname);
}

static int
l2fwd_parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;

	if (pm == 0)
		return -1;

	return pm;
}

static unsigned int
l2fwd_parse_nqueue(const char *q_arg)
{
	char *end = NULL;
	unsigned long n;

	/* parse hexadecimal string */
	n = strtoul(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;
	if (n == 0)
		return 0;
	if (n >= MAX_RX_QUEUE_PER_LCORE)
		return 0;

	return n;
}

static int
parse_timer_per_epoch(const char *t_arg)
{
	char *end = NULL;
    int n;

	/* parse number string */
    n = strtol(t_arg, &end, 10);
	if ((t_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;
	if (n <= 0)
		return -1;
	return n;
}


static double
parse_ma(const char *q_arg)
{
	char *end = NULL;
	double ma;

	/* parse hexadecimal string */
	ma = strtod(q_arg, &end);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;
    if(ma > 1 || ma < 0)
        return -1;
	return ma ;
}

static double
parse_avgBatch(const char *avgBatch_arg)
{
	char *end = NULL;
	double avgBatch;

	/* parse hexadecimal string */
	avgBatch = strtod(avgBatch_arg, &end);
	if ((avgBatch_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;
    if(avgBatch <= 0 )
        return -1;
	return avgBatch;
}

static int
parse_sample_interval(const char *s_arg)
{
	char *end = NULL;
	int si;

	/* parse decimal string */
	si = strtol(s_arg, &end, 10);
	if ((s_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1; 
	if (si == 0 || si >= MAX_TIMER_PERIOD )
		return -1;

	return si;
}

static int
parse_app_cycles(const char *s_arg)
{
	char *end = NULL;
	int ac;

	/* parse decimal string */
	ac = strtol(s_arg, &end, 10);
	if ((s_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1; 
	return ac;
}

static int
parse_start_epoch(const char *s_arg)
{
	char *end = NULL;
	int se;

	/* parse decimal string */
	se = strtol(s_arg, &end, 10);
	if ((s_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;
    
	return se;
}

/* static int */
/* l2fwd_parse_timer_period(const char *q_arg) */
/* { */
/* 	char *end = NULL; */
/* 	int n; */

/* 	/\* parse number string *\/ */
/* 	n = strtol(q_arg, &end, 10); */
/* 	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0')) */
/* 		return -1; */
/* 	if (n >= MAX_TIMER_PERIOD) */
/* 		return -1; */

/* 	return n; */
/* } */


static const char short_options[] =
	"p:"  /* portmask */
	"q:"  /* number of queues */
    "E:"  /* timer per epoch */
	;


enum {
	/* long options mapped to a short option */

	/* first long only option value must be >= 256, so that we won't
	 * conflict with short options */
	CMD_LINE_OPT_MIN_NUM = 256,
};

static const struct option lgopts[] = {
	{ CMD_LINE_OPT_MAC_UPDATING, no_argument, &mac_updating, 1},
	{ CMD_LINE_OPT_NO_MAC_UPDATING, no_argument, &mac_updating, 0},
    { OPTION_TRAFFIC_TABLE, required_argument, 0, 0},
    { OPTION_SAMPLE_INTERVAL, required_argument, 0, 0},
    { OPTION_AVG_BARRIVAL_SIZE, required_argument, 0, 0},
    { OPTION_START_EPOCH, required_argument, 0, 0},
    { OPTION_MA, required_argument, 0, 0},
    { OPTION_APP_CYCLES, required_argument, 0, 0},
	{NULL, 0, 0, 0}
};

/* Parse the argument given in the command line of the application */
static int
l2fwd_parse_args(int argc, char **argv)
{
	int opt, ret;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];

	argvopt = argv;

	while ((opt = getopt_long(argc, argvopt, short_options,
                              lgopts, &option_index)) != EOF) {

		switch (opt) {
            /* portmask */
		case 'p':
			l2fwd_enabled_port_mask = l2fwd_parse_portmask(optarg);
			if (l2fwd_enabled_port_mask == 0) {
				printf("invalid portmask\n");
				l2fwd_usage(prgname);
				return -1;
			}
			break;

            /* nqueue */
		case 'q':
			l2fwd_rx_queue_per_lcore = l2fwd_parse_nqueue(optarg);
			if (l2fwd_rx_queue_per_lcore == 0) {
				printf("invalid queue number\n");
				l2fwd_usage(prgname);
				return -1;
			}
			break;

 
        case 'E':
			timer_per_epoch = parse_timer_per_epoch(optarg);
			if (timer_per_epoch <= 0) {
				printf("invalid timer per epoch\n");
                l2fwd_usage(prgname);
				return -1;
			}
			break;
        
            /* long options */
		case 0:
            /* Add TKPE-DPDK Params */
            if (!strncmp(lgopts[option_index].name,
                         OPTION_TRAFFIC_TABLE,
                         sizeof(OPTION_TRAFFIC_TABLE))){
				tkpe_config.traffic_table_name = optarg;
            }

            if (!strncmp(lgopts[option_index].name,
                         OPTION_SAMPLE_INTERVAL,
                         sizeof(OPTION_SAMPLE_INTERVAL))){
                double si = parse_sample_interval(optarg);
                if ( si < 0 ){
                    printf("invalid sample interval.\n");
                    return -1;
                }
				tkpe_config.sample_interval = si;
            }
            
            if (!strncmp(lgopts[option_index].name,
                         OPTION_AVG_BARRIVAL_SIZE,
                         sizeof(OPTION_AVG_BARRIVAL_SIZE))){
                double ab = parse_avgBatch(optarg);
                if ( ab < 0 ){
                    printf("invalid average arrival batch size.\n");
                    return -1;
                }
                tkpe_config.avgBatch = ab;
            }

            if (!strncmp(lgopts[option_index].name,
                         OPTION_MA,
                         sizeof(OPTION_MA))){
                double ma = parse_ma(optarg);
                if ( ma < 0 ){
                    printf("invalid moving average.\n");
                    return -1;
                }
				tkpe_config.ma = ma;
            }

            if (!strncmp(lgopts[option_index].name,
                         OPTION_START_EPOCH,
                         sizeof(OPTION_START_EPOCH))){
                int se =  parse_start_epoch(optarg);
                if( se < 0 ){
                    printf("invalid start epoch.\n");
                    return -1;
                }
                tkpe_config.start_epoch = se;
            }

            if (!strncmp(lgopts[option_index].name,
                         OPTION_APP_CYCLES,
                         sizeof(OPTION_APP_CYCLES))){
                int ac =  parse_app_cycles(optarg);
                if( ac <= 0 ){
                    printf("invalid app cycles.\n");
                    return -1;
                }
                tkpe_config.app_cycles = ac;
            }
            
			break;

		default:
			l2fwd_usage(prgname);
			return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind-1;
	optind = 0; /* reset getopt lib */
	return ret;
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(uint8_t port_num, uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint8_t portid, count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	printf("\nChecking link status");
	fflush(stdout);
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
					printf("Port %d Link Up - speed %u "
                           "Mbps - %s\n", (uint8_t)portid,
                           (unsigned)link.link_speed,
                           (link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
                           ("full-duplex") : ("half-duplex\n"));
				else
					printf("Port %d Link Down\n",
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

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}
}

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		printf("\n\nSignal %d received, preparing to exit...\n",
               signum);
		force_quit = true;
	}
}



/*  Freqency scale down timer callback */
static void
power_timer_cb(__attribute__((unused)) struct rte_timer *tim,
               __attribute__((unused)) void *arg)
{
        unsigned lcore_id = rte_lcore_id();
    rte_timer_reset(&power_timers[lcore_id], timerhz/timer_per_second-5000,
                    SINGLE, lcore_id, power_timer_cb, NULL);
        
    uint32_t cpuhzindex = rte_power_get_freq(lcore_id);
        
    /* The unit of values in freqs is in KHz  */
    double cpuhz = freqs[cpuhzindex]*1e3; /* info */
    
    stats[lcore_id].round++;
    stats[lcore_id].epoch =  (stats[lcore_id].round/timer_per_epoch) % tkpe_config.num_traffic;
    
    double history_traffic =  tkpe_config.traffic[stats[lcore_id].epoch]; /* history_traffic is in kpps */

    /* estimation the traffic load (in pps) by moving average method */
    double traffic_load = history_traffic*tkpe_config.ma*1e3 +          \
        (1-tkpe_config.ma)*stats[lcore_id].nb_rx_processed*timer_per_second;

    uint64_t freq1 = (uint64_t)( traffic_load * (tkpe_config.avgBatch*(CIO + tkpe_config.app_cycles) + CCALL)/tkpe_config.avgBatch/CLIFF );
    /* round to an available freq just a liitle higer than freq1, get its index */
    int afreq1_index = cpuhzindex - ( ceil(freq1/1e8) - (int)(cpuhz/1e8) );
    uint64_t freq2 = (uint64_t)( traffic_load * (tkpe_config.avgBatch*(CIO + tkpe_config.app_cycles) + CCALL)/tkpe_config.avgBatch/ADDJUST_FREQ_THRESHOLD );
    /* round to an available freq just a liitle higer than freq2, get its index */
    int afreq2_index = cpuhzindex - ( ceil(freq2/1e8) - (int)(cpuhz/1e8) );

    if( afreq1_index < 0 )
        afreq1_index = 0;
    else if( afreq1_index > total_freq_num - 1 )
        afreq1_index = total_freq_num - 1;

    if( afreq2_index < 0 )
        afreq2_index = 0;
    else if( afreq2_index > total_freq_num - 1 )
        afreq2_index = total_freq_num - 1;

    /* hysteresis loop: avoid frequent freq adjustments */
    if( afreq1_index != cpuhzindex && afreq2_index != cpuhzindex ){
        rte_power_set_freq(lcore_id, afreq1_index);
    }

    if( traffic_load == 0 ){
        stats[lcore_id].Nap_Cycles = cpuhz/1e6;
    }else{
        int Nap_Cycles = cpuhz/traffic_load/tkpe_config.avgBatch;
        stats[lcore_id].Nap_Cycles = (Nap_Cycles > cpuhz/1e6)?cpuhz/1e6:Nap_Cycles;
    }

    stats[lcore_id].nb_rx_processed = 0;
    stats[lcore_id].nb_idle_looped = 0;
}

int
main(int argc, char **argv)
{
	struct lcore_queue_conf *qconf;
	//struct rte_eth_dev_info dev_info;
	int ret;
	uint8_t nb_ports;
	uint8_t nb_ports_available;
	uint8_t portid;
	unsigned lcore_id, rx_lcore_id;
    
	/* unsigned nb_ports_in_mask = 0; */
	/* uint64_t hz; */
	/* init EAL */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	argc -= ret;
	argv += ret;

	/* init RTE timer library to be used late */
    rte_timer_subsystem_init();

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
    
	/* parse application arguments (after the EAL ones) */
	ret = l2fwd_parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid L2FWD arguments\n");

	printf("MAC updating %s\n", mac_updating ? "enabled" : "disabled");


    /* Add tkpe historical traffic entries */
    if (app_tkpe_init() < 0)
        rte_exit(EXIT_FAILURE, "app_tkpe_init failed\n");

    timerhz = rte_get_timer_hz();
    printf("Hardware Timer Resolution: %lu \n",timerhz);
    timer_per_second = timer_per_epoch/tkpe_config.sample_interval;
    printf("Timer Per Epoch: %d ****** Timer Per Second: %d\n",timer_per_epoch,timer_per_second);
    

	/* create the mbuf pool */
	l2fwd_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", NB_MBUF,
                                                 MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                                 rte_socket_id());
	if (l2fwd_pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

	nb_ports = rte_eth_dev_count();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");

	/* reset l2fwd_dst_ports */
	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++)
		l2fwd_dst_ports[portid] = portid;

	rx_lcore_id = 0;
	qconf = NULL;

	/* Initialize the port/queue configuration of each logical core */
	for (portid = 0; portid < nb_ports; portid++) {
		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;

		/* get the lcore_id for this port */
		while (rte_lcore_is_enabled(rx_lcore_id) == 0 ||
		       lcore_queue_conf[rx_lcore_id].n_rx_port ==
		       l2fwd_rx_queue_per_lcore) {
			rx_lcore_id++;
			if (rx_lcore_id >= RTE_MAX_LCORE)
				rte_exit(EXIT_FAILURE, "Not enough cores\n");
		}

		if (qconf != &lcore_queue_conf[rx_lcore_id])
			/* Assigned a new logical core in the loop above. */
			qconf = &lcore_queue_conf[rx_lcore_id];

		qconf->rx_port_list[qconf->n_rx_port].port_id = portid;
		qconf->rx_port_list[qconf->n_rx_port].queue_id = 0;
		qconf->n_rx_port++;
		printf("Lcore %u: RX port %u\n", rx_lcore_id, (unsigned) portid);
	}

	nb_ports_available = nb_ports;

	
    for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
        if (rte_lcore_is_enabled(lcore_id) == 0)
            continue;

        /* init power management library */
        ret = rte_power_init(lcore_id);
        if (ret)
            RTE_LOG(ERR, POWER,
                    "Library initialization failed on core %u\n", lcore_id);

        /* init timer structures for each enabled lcore */
        rte_timer_init(&power_timers[lcore_id]);
        stats[lcore_id].epoch = tkpe_config.start_epoch;
        rte_timer_reset(&power_timers[lcore_id],
                        timerhz/timer_per_second, SINGLE, lcore_id,
                        power_timer_cb, NULL);

	}


	/* Initialise each port */
	for (portid = 0; portid < nb_ports; portid++) {
		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) {
			printf("Skipping disabled port %u\n", (unsigned) portid);
			nb_ports_available--;
			continue;
		}
		/* init port */
		printf("Initializing port %u... ", (unsigned) portid);
		fflush(stdout);
		ret = rte_eth_dev_configure(portid, 1, 1, &port_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
                     ret, (unsigned) portid);

		rte_eth_macaddr_get(portid,&l2fwd_ports_eth_addr[portid]);

		/* init one RX queue */
		fflush(stdout);
		ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
                                     rte_eth_dev_socket_id(portid),
                                     NULL,
                                     l2fwd_pktmbuf_pool);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
                     ret, (unsigned) portid);

		/* init one TX queue on each port */
		fflush(stdout);
		ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
                                     rte_eth_dev_socket_id(portid),
                                     NULL);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
                     ret, (unsigned) portid);

		/* Initialize TX buffers */
		tx_buffer[portid] = rte_zmalloc_socket("tx_buffer",
                                               RTE_ETH_TX_BUFFER_SIZE(MAX_PKT_BURST), 0,
                                               rte_eth_dev_socket_id(portid));
		if (tx_buffer[portid] == NULL)
			rte_exit(EXIT_FAILURE, "Cannot allocate buffer for tx on port %u\n",
                     (unsigned) portid);

		rte_eth_tx_buffer_init(tx_buffer[portid], MAX_PKT_BURST);

		ret = rte_eth_tx_buffer_set_err_callback(tx_buffer[portid],
                                                 rte_eth_tx_buffer_count_callback,
                                                 &port_statistics[portid].dropped);
		if (ret < 0)
            rte_exit(EXIT_FAILURE, "Cannot set error callback for "
                     "tx buffer on port %u\n", (unsigned) portid);

		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
                     ret, (unsigned) portid);

		printf("done: \n");

		rte_eth_promiscuous_enable(portid);

		rte_spinlock_init( &(locks[portid]) );

		printf("Port %u, MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n\n",
               (unsigned) portid,
               l2fwd_ports_eth_addr[portid].addr_bytes[0],
               l2fwd_ports_eth_addr[portid].addr_bytes[1],
               l2fwd_ports_eth_addr[portid].addr_bytes[2],
               l2fwd_ports_eth_addr[portid].addr_bytes[3],
               l2fwd_ports_eth_addr[portid].addr_bytes[4],
               l2fwd_ports_eth_addr[portid].addr_bytes[5]);

		/* initialize port stats */
		memset(&port_statistics, 0, sizeof(port_statistics));
	}

	if (!nb_ports_available) {
		rte_exit(EXIT_FAILURE,
                 "All available ports are disabled. Please set portmask.\n");
	}

	check_all_ports_link_status(nb_ports, l2fwd_enabled_port_mask);

	ret = 0;
	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(l2fwd_launch_one_lcore, NULL, CALL_MASTER);

	for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
        if (rte_lcore_is_enabled(lcore_id) == 0)
            continue;
        /* init power management library */
        ret = rte_power_exit(lcore_id);
        if (ret)
            rte_exit(EXIT_FAILURE, "Power management "
                     "library de-initialization failed on "
                     "core%u\n", lcore_id);
	}
	
	for (portid = 0; portid < nb_ports; portid++) {
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;
		printf("Closing port %d...", portid);
		rte_eth_dev_stop(portid);
		rte_eth_dev_close(portid);
		printf(" Done\n");
	}

    RTE_LCORE_FOREACH_SLAVE(lcore_id) {
        if (rte_eal_wait_lcore(lcore_id) < 0) {
			ret = -1;
			break;
		}
	}
	//print_stats();
	
	printf("Bye...\n");
	rte_exit(EXIT_SUCCESS, "User forced exit\n");
	return ret;
}
