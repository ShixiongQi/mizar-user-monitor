// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file xdp_user_monitoring_prog.c
 * @author ShixiongQi (@ShixiongQi)
 *
 * @brief Implements the XDP monitoring program (metrics collector)
 *
 * NOTE: Some of the codes are copied from $(LINUX)/tools/bpf/bpftool/map.c and
 * https://github.com/xdp-project/xdp-tutorial/blob/master/tracing02-xdp-monitor/trace_load_and_stats.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <net/if.h>
#include <inttypes.h>

#include <locale.h>
#include <unistd.h>
#include <time.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <net/if.h>
#include <linux/if_link.h> /* depend on kernel-headers installed */
#include <linux/err.h>
#include <linux/perf_event.h>
#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <unistd.h>
#include <sys/syscall.h>   /* For SYS_xxx definitions */
#include <sys/ioctl.h>

#include <ctype.h>

#include "bpf_util.h" /* bpf_num_possible_cpus */
#include "trn_datamodel.h"

#define EXIT_OK 		    0
#define EXIT_FAIL		    1
#define EXIT_FAIL_BPF		40

#define NETDATA_CHART_PRIO_PKTS_PER_SEC 7000
#define NETDATA_CHART_PRIO_TX_PER_SEC 7001
#define NETDATA_CHART_PRIO_PASS_PER_SEC 7002
#define NETDATA_CHART_PRIO_DROP_PER_SEC 7003
#define NETDATA_CHART_PRIO_REDIRECT_PER_SEC 7004
#define NETDATA_CHART_PRIO_ABORT_PER_SEC 7005

// #define PLUGIN_TRANSIT_XDP_NAME "transit_xdp.plugin"

#define unlikely(x)     __builtin_expect(!!(x), 0)

typedef long long collected_number;

static int update_every;
static int freq = 0;
static int debug = 0;
/* struct metrics_record defined in trn_datamodel.h */

/* Userspace structs for collection of stats from maps */
struct record {
	__u64 timestamp;
	struct metrics_record total;
	struct metrics_record *cpu;
};

struct bpf_map {
	char *name;
	int fd;
};

struct stats_to_be_printed {
	double pkts_per_second;
	double tx_per_second; 
	double pass_per_second;
	double drop_per_second;
	double redirect_per_second;
	double abort_per_second;
};

static inline int str2i(const char *s) {
    int n = 0;
    char c, negative = (*s == '-');

    for(c = (negative)?*(++s):*s; c >= '0' && c <= '9' ; c = *(++s)) {
        n *= 10;
        n += c - '0';
    }

    if(unlikely(negative))
        return -n;

    return n;
}

static int __check_map_fd_info(int map_fd, struct bpf_map_info *info,
			       struct bpf_map_info *exp)
{
	/* Please be careful to "info_len". There will a possible error when
	 * executing "bpf_obj_get_info_by_fd()":
	 * 				Arguments list too long 
	 */
	struct bpf_map_info temp = {};
	__u32 info_len = sizeof(temp);
	// __u32 info_len = 40; //sizeof(*info);
	int err;

	if (map_fd < 0)
		return EXIT_FAIL;

        /* BPF-info via bpf-syscall */
	err = bpf_obj_get_info_by_fd(map_fd, info, &info_len);

	if (err) {
		fprintf(stderr, "ERR: %s() can't get info - %s\n",
			__func__,  strerror(errno));
		return EXIT_FAIL_BPF;
	}

	if (exp->key_size && exp->key_size != info->key_size) {
		fprintf(stderr, "ERR: %s() "
			"Map key size(%d) mismatch expected size(%d)\n",
			__func__, info->key_size, exp->key_size);
		return EXIT_FAIL;
	}

	if (exp->value_size && exp->value_size != info->value_size) {
		fprintf(stderr, "ERR: %s() "
			"Map value size(%d) mismatch expected size(%d)\n",
			__func__, info->value_size, exp->value_size);
		return EXIT_FAIL;
	}

	if (exp->max_entries && exp->max_entries != info->max_entries) {
		fprintf(stderr, "ERR: %s() "
			"Map max_entries(%d) mismatch expected size(%d)\n",
			__func__, info->max_entries, exp->max_entries);
		return EXIT_FAIL;
	}

	if (exp->type && exp->type  != info->type) {
		fprintf(stderr, "ERR: %s() "
			"Map type(%d) mismatch expected type(%d)\n",
			__func__, info->type, exp->type);
		return EXIT_FAIL;
	}

	return 0;
}

static int __check_map(int map_fd, struct bpf_map_info *exp)
{
	struct bpf_map_info info;

	return __check_map_fd_info(map_fd, &info, exp);
}

static int check_map(const char *name, int fd)
{
	struct {
		const char          *name;
		struct bpf_map_info  info;
	} maps[] = {
		{
			.name = "metrics_table",
			.info = {
				.type = BPF_MAP_TYPE_PERCPU_ARRAY,
				.key_size = sizeof(__u32),
				.value_size = sizeof(struct metrics_record),
				.max_entries = 1,
			}
		},
		{ }
	};
	int i = 0;

	// fprintf(stdout, "checking map %s\n", name);

	while (maps[i].name) {
		if (!strcmp(maps[i].name, name))
			return __check_map(fd, &maps[i].info);
		i++;
	}

	fprintf(stdout, "ERR: map %s not found\n", name);
	return -1;
}

static int check_maps(struct bpf_map *map)
{
	const char *name;
	int fd;

	// name = bpf_map__name(map);
	name = map ? map->name : NULL;
	// fd   = bpf_map__fd(map);
	fd = map ? map->fd : -EINVAL;

	if (check_map(name, fd)) /* if map exists, return 0 */
		return -1;

	/* return 0 on successful maps check */
	return 0;
}

#define NANOSEC_PER_SEC 1000000000 /* 10^9 */
static __u64 gettime(void)
{
	struct timespec t;
	int res;

	res = clock_gettime(CLOCK_MONOTONIC, &t);
	if (res < 0) {
		fprintf(stderr, "Error with gettimeofday! (%i)\n", res);
		exit(-1);
	}
	return (__u64) t.tv_sec * NANOSEC_PER_SEC + t.tv_nsec;
}

static bool map_collect_record(int fd, __u32 key, struct record *rec)
{
	/* For percpu maps, userspace gets a value per possible CPU */
	unsigned int nr_cpus = bpf_num_possible_cpus();
	struct metrics_record values[nr_cpus];
	__u64 sum_n_pkts = 0;
	__u64 sum_total_bytes_rx = 0;
	__u64 sum_total_bytes_tx = 0;
	__u64 sum_n_tx = 0;
	__u64 sum_n_pass = 0;
	__u64 sum_n_drop = 0;
	__u64 sum_n_redirect = 0;
	__u64 sum_n_aborted = 0;
	int i;

	if ((bpf_map_lookup_elem(fd, &key, values)) != 0) {
		fprintf(stderr,
			"ERR: bpf_map_lookup_elem failed key:0x%X\n", key);
		return false;
	}
	/* Get time as close as possible to reading map contents */
	rec->timestamp = gettime();

	/* Record and sum values from each CPU */
	for (i = 0; i < nr_cpus; i++) {
		rec->cpu[i].n_pkts         =  values[i].n_pkts;
		sum_n_pkts                 += values[i].n_pkts;
		rec->cpu[i].total_bytes_rx =  values[i].total_bytes_rx;
		sum_total_bytes_rx         += values[i].total_bytes_rx;
		rec->cpu[i].total_bytes_tx =  values[i].total_bytes_tx;
		sum_total_bytes_tx         += values[i].total_bytes_tx;
		rec->cpu[i].n_tx           =  values[i].n_tx;
		sum_n_tx                   += values[i].n_tx;
		rec->cpu[i].n_pass         =  values[i].n_pass;
		sum_n_pass                 += values[i].n_pass;
		rec->cpu[i].n_drop         =  values[i].n_drop;
		sum_n_drop                 += values[i].n_drop;
		rec->cpu[i].n_redirect     =  values[i].n_redirect;
		sum_n_redirect             += values[i].n_redirect;
		rec->cpu[i].n_aborted      =  values[i].n_aborted;
		sum_n_aborted              += values[i].n_aborted;		

	}
	rec->total.n_pkts         = sum_n_pkts;
	rec->total.total_bytes_rx = sum_total_bytes_rx;
	rec->total.total_bytes_tx = sum_total_bytes_tx;
	rec->total.n_tx           = sum_n_tx;
	rec->total.n_pass         = sum_n_pass;
	rec->total.n_drop         = sum_n_drop;
	rec->total.n_redirect     = sum_n_redirect;
	rec->total.n_aborted      = sum_n_aborted;

	return true;
}

static double calc_period(struct record *r, struct record *p)
{
	double period_ = 0;
	__u64 period = 0;

	period = r->timestamp - p->timestamp;
	if (period > 0)
		period_ = ((double) period / NANOSEC_PER_SEC);

	return period_;
}

/*
static double calc_pkts_per_second(struct metrics_record *r, struct metrics_record *p, double period)
static double calc_tx_per_second(struct metrics_record *r, struct metrics_record *p, double period)
static double calc_pass_per_second(struct metrics_record *r, struct metrics_record *p, double period)
static double calc_drop_per_second(struct metrics_record *r, struct metrics_record *p, double period)
static double calc_redirect_per_second(struct metrics_record *r, struct metrics_record *p, double period);
static double calc_abort_per_second(struct metrics_record *r, struct metrics_record *p, double period);
*/

/* 
 * So I aggregate all the above calc functions into a single function,
 * these functions take the same operation actually... 
 *
 * An example to call calc_op_per_second() and calculate TX per second:
 *
 * t = calc_period(curr_rec, prev_rec); // calculate the time between two records
 * struct metrics_record *r = &curr_rec->total;
 * struct metrics_record *p = &prev_rec->total;
 *
 * double tx_per_sec = calc_op_per_second(r->n_tx, p->tx, t);
 */
static double calc_op_per_second(__u64 curr, __u64 prev, double period)
{
	__u64 packets = 0;
	double pps = 0;

	if (period > 0) {
		packets = curr - prev;
		pps = ((double)packets / period);
	}
	return pps;
}

// TODO: any other calc_ functions we need?
// static int calc_total_bytes_rx(struct metrics_record *r, struct metrics_record *p, double period);
// static int calc_total_bytes_tx(struct metrics_record *r, struct metrics_record *p, double period);
static void xdp_send_metrics_to_netdata(struct stats_to_be_printed *_s_print) {
// static void xdp_send_metrics_to_netdata(double pkts_per_second) {
    static int // stats counter
               pps_chart_generated = 0,
               tx_chart_generated = 0,
               pass_chart_generated = 0,
               drop_chart_generated = 0,
               redirect_chart_generated = 0,
               abort_chart_generated = 0;

    // ------------------------------------------------------------------------

    if(unlikely(!pps_chart_generated)) {
        pps_chart_generated = 1;

        printf("CHART transit_xdp.pps '' 'Packets per second' 'pkts/s' transitXdpPacketPerSecond.plugin transit_xdp.pps line 14001 %d\n"
		// printf("CHART test.stats '' 'Tests per second' 'tests/s' test.plugin test.stats line 14000 %d\n"
            //    , NETDATA_CHART_PRIO_PKTS_PER_SEC
               , update_every
            //    , PLUGIN_TRANSIT_XDP_NAME
        );
        printf("DIMENSION %s '' absolute 1 1\n", "pps");
    }

    printf(
           "BEGIN transit_xdp.pps\n"
           "SET %s = %lld\n"
           , "pps"
           , (collected_number) _s_print->pkts_per_second
			// , (long long) pkts_per_second
    );
    printf("END\n");
// /*
    // ------------------------------------------------------------------------

    if(unlikely(!tx_chart_generated)) {
        tx_chart_generated = 1;

        printf("CHART transit_xdp.tx '' 'TX per second' 'tx/s' transitXdpTxPerSecond.plugin transit_xdp.tx line 14102 %d\n"
            //    , NETDATA_CHART_PRIO_TX_PER_SEC
               , update_every
            //    , PLUGIN_TRANSIT_XDP_NAME
        );
        printf("DIMENSION %s '' absolute 1 1\n", "tx");
    }

    printf(
           "BEGIN transit_xdp.tx\n"
           "SET %s = %lld\n"
           , "tx"
           , (collected_number) _s_print->tx_per_second
    );
    printf("END\n");

    // ------------------------------------------------------------------------

    if(unlikely(!pass_chart_generated)) {
        pass_chart_generated = 1;

        printf("CHART transit_xdp.pass '' 'Pass per second' 'pass/s' transitXdpPassPerSecond.plugin transit_xdp.pass line 14203 %d\n"
            //    , NETDATA_CHART_PRIO_PASS_PER_SEC
               , update_every
            //    , PLUGIN_TRANSIT_XDP_NAME
        );
        printf("DIMENSION %s '' absolute 1 1\n", "pass");
    }

    printf(
           "BEGIN transit_xdp.pass\n"
           "SET %s = %lld\n"
           , "pass"
           , (collected_number) _s_print->pass_per_second
    );
    printf("END\n");

    // ------------------------------------------------------------------------

    if(unlikely(!drop_chart_generated)) {
        drop_chart_generated = 1;

        printf("CHART transit_xdp.drop '' 'Drop per second' 'drop/s' transitXdpDropPerSecond.plugin transit_xdp.drop line 14304 %d\n"
            //    , NETDATA_CHART_PRIO_DROP_PER_SEC
               , update_every
            //    , PLUGIN_TRANSIT_XDP_NAME
        );
        printf("DIMENSION %s '' absolute 1 1\n", "drop");
    }

    printf(
           "BEGIN transit_xdp.drop\n"
           "SET %s = %lld\n"
           , "drop"
           , (collected_number) _s_print->drop_per_second
    );
    printf("END\n");

    // ------------------------------------------------------------------------

    if(unlikely(!redirect_chart_generated)) {
        redirect_chart_generated = 1;

        printf("CHART transit_xdp.redirect '' 'Packets per second' 'redirect/s' transitXdpRedirectPerSecond.plugin transit_xdp.redirect line 14405 %d\n"
            //    , NETDATA_CHART_PRIO_REDIRECT_PER_SEC
               , update_every
            //    , PLUGIN_TRANSIT_XDP_NAME
        );
        printf("DIMENSION %s '' absolute 1 1\n", "redirect");
    }

    printf(
           "BEGIN transit_xdp.redirect\n"
           "SET %s = %lld\n"
           , "redirect"
           , (collected_number) _s_print->redirect_per_second
    );
    printf("END\n");

    // ------------------------------------------------------------------------

    if(unlikely(!abort_chart_generated)) {
        abort_chart_generated = 1;

        printf("CHART transit_xdp.abort '' 'Abort per second' 'abort/s' transitXdpAbortPerSecond.plugin transit_xdp.abort line 14506 %d\n"
            //    , NETDATA_CHART_PRIO_ABORT_PER_SEC
               , update_every
            //    , PLUGIN_TRANSIT_XDP_NAME
        );
        printf("DIMENSION %s '' absolute 1 1\n", "abort");
    }

    printf(
           "BEGIN transit_xdp.abort\n"
           "SET %s = %lld\n"
           , "abort"
           , (collected_number) _s_print->abort_per_second
    );
    printf("END\n");
// */
}

static void stats_print(struct record *stats_rec,
			struct record *stats_prev)
{
	// unsigned int nr_cpus = bpf_num_possible_cpus();
	double t = 0;
	/* Define the metrics to be printed */
	// double pkts_per_second, tx_per_second, pass_per_second,
		   //drop_per_second, redirect_per_second, abort_per_second;

	t = calc_period(stats_rec, stats_prev);

	struct metrics_record *r = &stats_rec->total;
	struct metrics_record *p = &stats_prev->total;
	// double pkts_per_second = calc_op_per_second(r->n_pkts,     p->n_pkts,     t);
	struct stats_to_be_printed s_print; 
	s_print.pkts_per_second     = calc_op_per_second(r->n_pkts,     p->n_pkts,     t);
	s_print.tx_per_second       = calc_op_per_second(r->n_tx,       p->n_tx,       t);
	s_print.pass_per_second     = calc_op_per_second(r->n_pass,     p->n_pass,     t);
	s_print.drop_per_second     = calc_op_per_second(r->n_drop,     p->n_drop,     t);
	s_print.redirect_per_second = calc_op_per_second(r->n_redirect, p->n_redirect, t);
	s_print.abort_per_second    = calc_op_per_second(r->n_aborted,  p->n_aborted,  t);

	/** 
	 * send the collected data to Netdata 
	 * print BEGIN, SET, END statements
	 */
	// xdp_send_metrics_to_netdata(pkts_per_second);
	xdp_send_metrics_to_netdata(&s_print);

	// printf("[Stats] Packets Per Second: %'-10.2f\n 
	// 				TX Per Second: %'-10.2f\n 
	// 				Pass Per Second: %'-10.2f\n 
	// 				Drop Per Second: %'-10.2f\n 
	// 				Redirect Per Second: %'-10.2f\n 
	// 				Abort Per Second: %'-10.2f\n\n\n",
	// 				s_print.pkts_per_second, s_print.tx_per_second, s_print.pass_per_second,
	// 				s_print.drop_per_second, s_print.redirect_per_second, s_print.abort_per_second);

}

static bool stats_collect(struct bpf_map *map, struct record *rec)
{
	int fd;
	/* 
	 * "key" is used as the key to lookup the bpf map, need to keep 
	 * consistent in the kernel prog (0 in our design).
	 */
	__u32 key = 0;
	fd = map ? map->fd : -EINVAL; //bpf_map__fd(map);
	map_collect_record(fd, key, rec);

	return true;
}

static void *alloc_rec_per_cpu(int record_size)
{
	unsigned int nr_cpus = bpf_num_possible_cpus();
	void *array;
	size_t size;

	size = record_size * nr_cpus;
	array = malloc(size);
	memset(array, 0, size);
	if (!array) {
		fprintf(stderr, "Mem alloc error (nr_cpus:%u)\n", nr_cpus);
		exit(-1);
	}
	return array;
}

static struct record *alloc_stats_record(void)
{
	struct record *rec;
	int rec_sz;

	/* Alloc main record structure */
	rec = malloc(sizeof(*rec));
	memset(rec, 0, sizeof(*rec));
	if (!rec) {
		fprintf(stderr, "Mem alloc error\n");
		exit(-1);
	}

	/* Alloc stats stored per CPU for each record */
	rec_sz = sizeof(struct metrics_record);
	rec->cpu = alloc_rec_per_cpu(rec_sz);

	return rec;
}

static void free_stats_record(struct record *r)
{
	free(r->cpu);
	free(r);
}

/* Pointer swap trick */
static inline void swap(struct record **a, struct record **b)
{
	struct record *tmp;

	tmp = *a;
	*a = *b;
	*b = tmp;
}

/*
static void
send_metrics_to_netdata(long long p) {
    static int stats_counter = 0;

    if(unlikely(!stats_counter)) {
        stats_counter = 1;

        printf("CHART xdp.stats '' 'Tests per second' 'tests/s' transitxdp.plugin xdp.stats line 9000 %d\n"
            //    , NETDATA_CHART_PRIO_PKTS_PER_SEC
               , update_every
            //    , PLUGIN_TRANSIT_XDP_NAME
        );
        printf("DIMENSION %s '' absolute 1 1\n", "tps");
    }

    printf(
           "BEGIN xdp.stats\n"
           "SET %s = %lld\n"
           , "tps"
           , p
    );
    printf("END\n");
}
*/

static void stats_poll(struct bpf_map *map, int interval)
{
	struct record *rec, *prev;

	rec  = alloc_stats_record();
	prev = alloc_stats_record();
	stats_collect(map, rec);

	/* Trick to pretty printf with thousands separators use %' */
	// setlocale(LC_NUMERIC, "en_US");
/*			
			double t = 0;
			struct metrics_record *r;
			struct metrics_record *p;
			double pkts_per_second;
*/
			// long long printout = 0;
	while (1) {
		// printf("map->fd: %d\n", map->fd);
		swap(&prev, &rec);
		stats_collect(map, rec);
		// send_metrics_to_netdata(printout);
		stats_print(rec, prev);


/*
		t = calc_period(rec, prev);
		r = &rec->total;
		p = &prev->total;
		pkts_per_second = calc_op_per_second(r->n_pkts,     p->n_pkts,     t);
		xdp_send_metrics_to_netdata(pkts_per_second);
*/
		fflush(stdout);
		sleep(interval);
		// printout++;
	}

	free_stats_record(rec);
	free_stats_record(prev);
}

static int map_fd_by_name(char *name, int **fds)
{
	unsigned int id = 0;
	int fd, nb_fds = 0;
	void *tmp;
	int err;

	while (true) {
		struct bpf_map_info info = {};
		__u32 len = sizeof(info);

		err = bpf_map_get_next_id(id, &id);
		if (err) {
			if (errno != ENOENT) {
				fprintf(stderr, "%s", strerror(errno));
				goto err_close_fds;
			}
			return nb_fds;
		}

		fd = bpf_map_get_fd_by_id(id);
		if (fd < 0) {
			fprintf(stderr, "can't get map by id (%u): %s", id, strerror(errno));
			goto err_close_fds;
		}

		err = bpf_obj_get_info_by_fd(fd, &info, &len);
		if (err) {
			fprintf(stderr, "can't get map info (%u): %s", id, strerror(errno));
			goto err_close_fd;
		}

		if (strncmp(name, info.name, BPF_OBJ_NAME_LEN)) {
			close(fd);
			continue;
		}

		if (nb_fds > 0) {
			tmp = realloc(*fds, (nb_fds + 1) * sizeof(int));
			if (!tmp) {
				printf("failed to realloc\n");
				goto err_close_fd;
			}
			*fds = tmp;
		}
		(*fds)[nb_fds++] = fd;
		if (nb_fds == 1) // for debugging, need to delete later
			return nb_fds;
	}

err_close_fd:
	close(fd);
err_close_fds:
	while (--nb_fds >= 0)
		close((*fds)[nb_fds]);
	return -1;
}

static int map_parse_fds(char *name, int **fds)
{
	if (strlen(name) > BPF_OBJ_NAME_LEN - 1) {
		printf("can't parse name\n");
		return -1;
	}

	return map_fd_by_name(name, fds);
}

int map_parse_fd(char *name)
{
	int *fds = NULL;
	int nb_fds, fd;

	fds = malloc(sizeof(int));
	if (!fds) {
		printf("mem alloc failed\n");
		return -1;
	}
	nb_fds = map_parse_fds(name, &fds);
	if (nb_fds != 1) {
		if (nb_fds > 1) {
			printf("several maps match this handle\n");
			while (nb_fds--)
				close(fds[nb_fds]);
		}
		fd = -1;
		goto exit_free;
	}

	fd = fds[0];
exit_free:
	free(fds);
	return fd;
}

int map_parse_fd_and_info(char *name, void *info, __u32 *info_len)
{
	int err;
	int fd;

	fd = map_parse_fd(name);
	if (fd < 0)
		return -1;

	err = bpf_obj_get_info_by_fd(fd, info, info_len);
	if (err) {
		fprintf(stderr, "can't get map info: %s", strerror(errno));
		close(fd);
		return err;
	}

	return fd;
}

static int do_bpf_map_lookup_fd(char *name)
{
	struct bpf_map_info info = {};
	__u32 len = sizeof(info);

	int fd = map_parse_fd_and_info(name, &info, &len);
	if (fd < 0)
		return -1;

	return fd;
}

void parse_command_line(int argc, char **argv) {
    int i, plugin_enabled = 0;
    plugin_enabled = 1;
    for(i = 1; i < argc ; i++) {
        if(isdigit(*argv[i]) && !freq) {
            int n = str2i(argv[i]);
            if(n > 0 && n < 86400) {
                freq = n;
                continue;
            }
        }
        else if(strcmp("version", argv[i]) == 0 || strcmp("-version", argv[i]) == 0 || strcmp("--version", argv[i]) == 0 || strcmp("-v", argv[i]) == 0 || strcmp("-V", argv[i]) == 0) {
            printf("transitxdp.plugin %s\n", "1-1");
            exit(0);
        }
        else if(strcmp("debug", argv[i]) == 0) {
            debug = 1;
            continue;
        }
        else if(strcmp("-h", argv[i]) == 0 || strcmp("--help", argv[i]) == 0) {
            fprintf(stderr,
                    "\n"
                    " netdata transit_xdp.plugin %s\n"
                    // " Copyright (C) 2019 Netdata Inc.\n"
                    // " Released under GNU General Public License v3 or later.\n"
                    // " All rights reserved.\n"
                    "\n"
                    " This program is a data collector plugin for netdata.\n"
                    "\n"
                    " Available command line options:\n"
                    "\n"
                    "  COLLECTION_FREQUENCY    data collection frequency in seconds\n"
                    "                          minimum: %d\n"
                    "\n"
                    "  -v\n"
                    "  -V\n"
                    "  --version               print version and exit\n"
                    "\n"
                    "  -h\n"
                    "  --help                  print this message and exit\n"
                    "\n"
                    // " For more information:\n"
                    // " https://github.com/netdata/netdata/tree/master/collectors/perf.plugin\n"
                    // "\n"
                    , "1-1"
                    , update_every
            );
            exit(1);
        }

        printf("ignoring parameter '%s'", argv[i]);
    }

    if(!plugin_enabled){
        printf("no charts enabled - nothing to do.\n");
        printf("DISABLE\n");
        exit(1);
    }
}

int main(int argc, char **argv)
{
	struct bpf_map map;
	/* sampling interval */
	// int interval = 1;
	/* name of BPF map in kernel */
	map.name = "metrics_table";

    parse_command_line(argc, argv);

    if(freq >= update_every)
        update_every = freq;
    else if(freq) {
        printf("update frequency %d seconds is too small for TRANSIT_XDP. Using %d.", freq, update_every);
	}

	/* Lookup the fd of map based on bpf map name */	
	map.fd = do_bpf_map_lookup_fd(map.name);
	if (map.fd == -1) {
		printf("NO MAP FD FOUND\n");
		return EXIT_FAIL_BPF;
	}

	/* Varify the maps */
	if (check_maps(&map)){
		printf("Map check not passed!\n");
	 	return EXIT_FAIL_BPF;
	}
	
	stats_poll(&map, update_every);

	return EXIT_OK;
}


