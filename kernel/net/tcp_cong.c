/*
 * tcp_cong.c - TCP Reno Congestion Control Implementation
 *
 * Implements TCP Reno congestion control algorithm:
 *   - Slow Start (cwnd doubles each RTT)
 *   - Congestion Avoidance (cwnd increases linearly)
 *   - Fast Retransmit (3 duplicate ACKs trigger retransmission)
 *   - Fast Recovery (RFC 5681)
 *   - RTT estimation using Karn's algorithm
 *   - TCP Window Scale option (RFC 1323)
 */
#include "net.h"
#include "log.h"
#include "string.h"
#include "../smp.h"
#include <stdint.h>

/* ================================================================
 * TCP Congestion Control Constants
 * ================================================================ */
#define TCP_MSS             1460       /* Maximum Segment Size */
#define TCP_INITIAL_CWND    (2 * TCP_MSS)  /* Initial congestion window */
#define TCP_INITIAL_SSTHRESH  65535    /* Initial slow start threshold */

/* RTT estimation constants (RFC 6298) */
#define TCP_RTT_ALPHA       7         /* Alpha = 1/8 (in 1/8 units: 1) */
#define TCP_RTT_BETA        3         /* Beta  = 1/4 (in 1/4 units: 1) */
#define TCP_RTO_INITIAL     1000      /* Initial RTO in ms */
#define TCP_RTO_MIN         200       /* Minimum RTO in ms */
#define TCP_RTO_MAX         60000     /* Maximum RTO in ms */

/* ================================================================
 * TCP Congestion State per Socket
 * ================================================================ */
struct tcp_cong_data {
    uint32_t cwnd;           /* Congestion window (bytes) */
    uint32_t ssthresh;       /* Slow start threshold (bytes) */
    uint32_t rtt;            /* Smoothed round-trip time (ms) */
    uint32_t rttvar;         /* RTT mean deviation (ms) */
    uint32_t srto;           /* Retransmission timeout (ms) */
    uint32_t rtt_seq;        /* Sequence number being timed */
    uint32_t rtt_ts;         /* Timestamp when timed segment was sent */
    int      rtt_measuring;  /* Whether we are measuring RTT */
    int      dup_ack_count;  /* Duplicate ACK counter */
    uint32_t last_ack;       /* Last received ACK number */
    uint32_t recover_seq;    /* Recovery sequence number */
    int      cong_state;     /* Current congestion state */
    int      window_scale;   /* Window scale shift count */
    int      sack_ok;        /* SACK permitted */
    int      timestamp_ok;   /* Timestamp option supported */
    uint32_t recent_ts;      /* Most recent timestamp from peer */
    uint32_t last_ts_sent;   /* Last timestamp we sent */
};

/* ================================================================
 * Congestion data storage
 * ================================================================ */
#define MAX_TCP_CONG_SOCKETS 16
static struct tcp_cong_data tcp_cong_data[MAX_TCP_CONG_SOCKETS];
static spinlock_t tcp_cong_lock;

/* ================================================================
 * Init
 * ================================================================ */
void tcp_cong_init(void) {
    spin_init(&tcp_cong_lock);
    memset(tcp_cong_data, 0, sizeof(tcp_cong_data));

    log_printf(LOG_LEVEL_INFO, "tcp_cong: TCP Reno congestion control initialized\n");
}

/* ================================================================
 * Internal: Initialize congestion data for a new socket
 * ================================================================ */
void tcp_cong_socket_init(int sock_id) {
    spin_lock(&tcp_cong_lock);

    int i;
    int slot = -1;

    /* Find existing or free slot for this socket */
    for (i = 0; i < MAX_TCP_CONG_SOCKETS; i++) {
        if (tcp_cong_data[i].cwnd == 0 && tcp_cong_data[i].ssthresh == 0 &&
            tcp_cong_data[i].srto == 0) {
            if (slot < 0) slot = i;
        }
    }

    if (slot < 0) {
        /* Overwrite oldest */
        slot = sock_id % MAX_TCP_CONG_SOCKETS;
    }

    memset(&tcp_cong_data[slot], 0, sizeof(tcp_cong_data[slot]));
    tcp_cong_data[slot].cwnd = TCP_INITIAL_CWND;
    tcp_cong_data[slot].ssthresh = TCP_INITIAL_SSTHRESH;
    tcp_cong_data[slot].rtt = 0;
    tcp_cong_data[slot].rttvar = 0;
    tcp_cong_data[slot].srto = TCP_RTO_INITIAL;
    tcp_cong_data[slot].rtt_measuring = 0;
    tcp_cong_data[slot].dup_ack_count = 0;
    tcp_cong_data[slot].last_ack = 0;
    tcp_cong_data[slot].cong_state = TCP_CONG_SLOW_START;
    tcp_cong_data[slot].window_scale = 0;
    tcp_cong_data[slot].sack_ok = 0;
    tcp_cong_data[slot].timestamp_ok = 0;

    spin_unlock(&tcp_cong_lock);
}

/* ================================================================
 * Internal: Find congestion data for a socket
 * ================================================================ */
static struct tcp_cong_data *tcp_cong_find_slot(int sock_id) {
    int slot = sock_id % MAX_TCP_CONG_SOCKETS;
    return &tcp_cong_data[slot];
}

/* ================================================================
 * RTT Estimation (Karn's algorithm)
 * ================================================================ */
static void tcp_cong_update_rtt(struct tcp_cong_data *cd, uint32_t measured_rtt) {
    if (measured_rtt == 0) {
        return;  /* RTT=0 is invalid, skip update */
    }
    if (cd->rtt == 0) {
        /* First RTT measurement */
        cd->rtt = measured_rtt;
        cd->rttvar = measured_rtt / 2;
    } else {
        /* RFC 6298: SRTT = (1 - alpha) * SRTT + alpha * R' */
        /* Using alpha = 1/8: SRTT = 7/8 * SRTT + 1/8 * R' */
        int32_t diff = (int32_t)measured_rtt - (int32_t)cd->rtt;
        if (diff < 0) diff = -diff;
        cd->rttvar = (3 * cd->rttvar + (uint32_t)diff) / 4;
        cd->rtt = (7 * cd->rtt + measured_rtt) / 8;
    }

    /* RTO = SRTT + 4 * RTTVAR */
    cd->srto = cd->rtt + 4 * cd->rttvar;
    if (cd->srto < TCP_RTO_MIN) cd->srto = TCP_RTO_MIN;
    if (cd->srto > TCP_RTO_MAX) cd->srto = TCP_RTO_MAX;
}

/* ================================================================
 * Congestion Control on ACK
 * ================================================================ */
void tcp_cong_on_ack(int sock, uint32_t ack_seq, int dup_ack_count) {
    spin_lock(&tcp_cong_lock);
    struct tcp_cong_data *cd = tcp_cong_find_slot(sock);
    if (!cd || cd->cwnd == 0) {
        spin_unlock(&tcp_cong_lock);
        return;
    }

    /* Duplicate ACK detection */
    if (ack_seq == cd->last_ack) {
        cd->dup_ack_count++;
    } else if (ack_seq > cd->last_ack) {
        /* New ACK */
        cd->dup_ack_count = 0;
        cd->last_ack = ack_seq;
    }

    int state = cd->cong_state;

    if (state == TCP_CONG_SLOW_START) {
        /* Slow Start: Each ACK increases cwnd by MSS */
        if (cd->cwnd < cd->ssthresh) {
            if (cd->cwnd <= UINT32_MAX - TCP_MSS) {
                cd->cwnd += TCP_MSS;
            }
            if (cd->cwnd > cd->ssthresh) {
                cd->cwnd = cd->ssthresh;
            }
        } else {
            /* Transition to congestion avoidance */
            cd->cong_state = TCP_CONG_AVOIDANCE;
        }
    }

    if (cd->cong_state == TCP_CONG_AVOIDANCE) {
        /* Congestion Avoidance: cwnd += MSS * MSS / cwnd per ACK */
        /* Approximate: cwnd += MSS / cwnd_fraction */
        uint32_t increment = (uint32_t)(((uint64_t)TCP_MSS * TCP_MSS) / cd->cwnd);
        if (increment == 0) increment = 1;
        cd->cwnd += increment;
    }

    /* Fast Retransmit: 3 duplicate ACKs */
    if (cd->dup_ack_count >= 3 && state != TCP_CONG_RECOVERY) {
        /* Set ssthresh to max(cwnd/2, 2*MSS) */
        cd->ssthresh = cd->cwnd / 2;
        if (cd->ssthresh < 2 * TCP_MSS) {
            cd->ssthresh = 2 * TCP_MSS;
        }

        /* Enter fast recovery */
        cd->cwnd = cd->ssthresh + 3 * TCP_MSS;
        cd->cong_state = TCP_CONG_RECOVERY;
        cd->recover_seq = ack_seq;

        log_printf(LOG_LEVEL_DEBUG,
                   "tcp_cong: fast retransmit sock=%d, cwnd=%u, ssthresh=%u\n",
                   sock, cd->cwnd, cd->ssthresh);
    }

    /* Fast Recovery: additional duplicate ACKs */
    if (state == TCP_CONG_RECOVERY && cd->dup_ack_count >= 3) {
        /* Inflate cwnd by MSS for each additional dup ACK */
        cd->cwnd += TCP_MSS;
    }

    /* Fast Recovery: new ACK covering recover_seq */
    if (state == TCP_CONG_RECOVERY && ack_seq > cd->recover_seq) {
        /* Deflate cwnd and exit recovery */
        cd->cwnd = cd->ssthresh;
        cd->cong_state = TCP_CONG_AVOIDANCE;
        cd->dup_ack_count = 0;
    }

    spin_unlock(&tcp_cong_lock);
}

/* ================================================================
 * Congestion Control on Timeout
 * ================================================================ */
void tcp_cong_on_timeout(int sock) {
    spin_lock(&tcp_cong_lock);
    struct tcp_cong_data *cd = tcp_cong_find_slot(sock);
    if (!cd || cd->cwnd == 0) {
        spin_unlock(&tcp_cong_lock);
        return;
    }

    /* Set ssthresh to max(cwnd/2, 2*MSS) */
    cd->ssthresh = cd->cwnd / 2;
    if (cd->ssthresh < 2 * TCP_MSS) {
        cd->ssthresh = 2 * TCP_MSS;
    }

    /* Reset congestion window to 1 MSS */
    cd->cwnd = TCP_MSS;

    /* Back off RTO (exponential backoff) */
    cd->srto = cd->srto * 2;
    if (cd->srto > TCP_RTO_MAX) {
        cd->srto = TCP_RTO_MAX;
    }

    /* Enter slow start */
    cd->cong_state = TCP_CONG_SLOW_START;
    cd->dup_ack_count = 0;
    cd->rtt_measuring = 0;  /* Karn: don't use RTT from retransmitted segments */

    log_printf(LOG_LEVEL_DEBUG,
               "tcp_cong: timeout sock=%d, cwnd=%u, ssthresh=%u, rto=%u\n",
               sock, cd->cwnd, cd->ssthresh, cd->srto);

    spin_unlock(&tcp_cong_lock);
}

/* ================================================================
 * Window Scale Option (RFC 1323) - Helpers
 * ================================================================ */
void tcp_cong_set_window_scale(int sock, int shift_cnt) {
    spin_lock(&tcp_cong_lock);
    struct tcp_cong_data *cd = tcp_cong_find_slot(sock);
    if (cd) {
        if (shift_cnt >= 0 && shift_cnt <= 14) {
            cd->window_scale = shift_cnt;
        }
    }
    spin_unlock(&tcp_cong_lock);
}

int tcp_cong_get_window_scale(int sock) {
    spin_lock(&tcp_cong_lock);
    struct tcp_cong_data *cd = tcp_cong_find_slot(sock);
    int scale = 0;
    if (cd) {
        scale = cd->window_scale;
    }
    spin_unlock(&tcp_cong_lock);
    return scale;
}

/* ================================================================
 * Get effective send window (min of cwnd and receiver window)
 * ================================================================ */
uint32_t tcp_cong_get_effective_window(int sock, uint16_t rcv_window) {
    spin_lock(&tcp_cong_lock);
    struct tcp_cong_data *cd = tcp_cong_find_slot(sock);
    uint32_t eff_win = TCP_MSS;

    if (cd) {
        /* Scale the receiver window if window scale option is in use */
        uint32_t scaled_rwnd = (uint32_t)rcv_window << cd->window_scale;
        eff_win = cd->cwnd;
        if (scaled_rwnd < eff_win) eff_win = scaled_rwnd;
        if (eff_win < TCP_MSS) eff_win = TCP_MSS;
    }

    spin_unlock(&tcp_cong_lock);
    return eff_win;
}

/* ================================================================
 * Get current RTO for a socket
 * ================================================================ */
uint32_t tcp_cong_get_rto(int sock) {
    spin_lock(&tcp_cong_lock);
    struct tcp_cong_data *cd = tcp_cong_find_slot(sock);
    uint32_t rto = TCP_RTO_INITIAL;
    if (cd) {
        rto = cd->srto;
    }
    spin_unlock(&tcp_cong_lock);
    return rto;
}

/* ================================================================
 * Start RTT measurement for a sequence number
 * ================================================================ */
void tcp_cong_start_rtt_measure(int sock, uint32_t seq) {
    spin_lock(&tcp_cong_lock);
    struct tcp_cong_data *cd = tcp_cong_find_slot(sock);
    if (cd && !cd->rtt_measuring) {
        cd->rtt_seq = seq;
        cd->rtt_measuring = 1;
        /* Use a simple counter as timestamp proxy */
        cd->rtt_ts = 0;
    }
    spin_unlock(&tcp_cong_lock);
}

/* ================================================================
 * Complete RTT measurement when ACK is received
 * ================================================================ */
void tcp_cong_complete_rtt_measure(int sock, uint32_t ack_seq,
                                    uint32_t measured_rtt) {
    spin_lock(&tcp_cong_lock);
    struct tcp_cong_data *cd = tcp_cong_find_slot(sock);
    if (cd && cd->rtt_measuring) {
        /* Karn's algorithm: only update RTT for non-retransmitted segments */
        /* For simplicity, we assume the measurement is valid */
        tcp_cong_update_rtt(cd, measured_rtt);
        cd->rtt_measuring = 0;
    }
    spin_unlock(&tcp_cong_lock);
}