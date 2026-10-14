/*
 * Copyright (C) 2009 - 2019 Xilinx, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 */

#include <stdio.h>
#include <string.h>

#include "lwip/err.h"
#include "lwip/tcp.h"
#include "amp_shared.h"
#include "xil_io.h"
#include "xpseudo_asm.h"
#if defined (__arm__) || defined (__aarch64__)
#include "xil_printf.h"
#endif

static struct tcp_pcb *conn;
static u32 sent;        /* tx_seq of the last answer handed to lwIP */
static u32 fill;        /* bytes of the current request already in RECEIVE */

/* Runs on every pass of the main loop: once CPU0 has finished an answer, send
 * SEND back to the laptop. */
int transfer_data() {
	u32 tx, t0, t1;

	tx = XCHG->tx_seq;
	if (conn == NULL || tx == sent) {
		return 0;
	}

	t0 = Xil_In32(GT_CNT_LO);
	if (tcp_write(conn, XCHG->send, sizeof(XCHG->send), TCP_WRITE_FLAG_COPY) == ERR_OK) {
		t1 = Xil_In32(GT_CNT_LO);
		tcp_output(conn);
		sent = tx;

		AMP_SYNC->x_wait += t0 - XCHG->tx_ts;
		AMP_SYNC->x_send += t1 - t0;
		AMP_SYNC->x_n++;
	}
	return 0;
}

void print_app_header()
{
#if (LWIP_IPV6==0)
	xil_printf("\n\r\n\r-----lwIP TCP echo server ------\n\r");
#else
	xil_printf("\n\r\n\r-----lwIPv6 TCP echo server ------\n\r");
#endif
	xil_printf("TCP packets sent to port 6001 will be echoed back\n\r");
}

err_t recv_callback(void *arg, struct tcp_pcb *tpcb,
                               struct pbuf *p, err_t err)
{
	u32 t0;

	if (!p) {
		conn = NULL;
		tcp_close(tpcb);
		tcp_recv(tpcb, NULL);
		return ERR_OK;
	}

	/* A request is exactly sizeof(RECEIVE) bytes, but TCP may deliver it in
	 * pieces, so collect until RECEIVE is full and only then tell CPU0. */
	t0 = Xil_In32(GT_CNT_LO);
	fill += pbuf_copy_partial(p, (u8 *)XCHG->receive + fill,
	                          sizeof(XCHG->receive) - fill, 0);
	AMP_SYNC->x_fill += Xil_In32(GT_CNT_LO) - t0;

	if (fill == sizeof(XCHG->receive)) {
		fill = 0u;
		XCHG->rx_ts = Xil_In32(GT_CNT_LO);
		dmb();                  /* RECEIVE and rx_ts must be in place before rx_seq moves */
		XCHG->rx_seq++;
	}

	tcp_recved(tpcb, p->tot_len);
	pbuf_free(p);
	return ERR_OK;
}

static void err_callback(void *arg, err_t err)
{
	conn = NULL;    /* lwIP has already freed the pcb */
}

err_t accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{
	static int connection = 1;

	/* set the receive callback for this connection */
	conn = newpcb;
	sent = XCHG->tx_seq;    /* an answer to an earlier connection is not for this one */
	fill = 0u;
	tcp_nagle_disable(newpcb);
	tcp_recv(newpcb, recv_callback);
	tcp_err(newpcb, err_callback);

	/* just use an integer number indicating the connection id as the
	   callback argument */
	tcp_arg(newpcb, (void*)(UINTPTR)connection);

	/* increment for subsequent accepted connections */
	connection++;

	return ERR_OK;
}


int start_application()
{
	struct tcp_pcb *pcb;
	err_t err;
	unsigned port = 7;

	/* create new TCP PCB structure */
	pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
	if (!pcb) {
		xil_printf("Error creating PCB. Out of Memory\n\r");
		return -1;
	}

	/* bind to specified @port */
	err = tcp_bind(pcb, IP_ANY_TYPE, port);
	if (err != ERR_OK) {
		xil_printf("Unable to bind to port %d: err = %d\n\r", port, err);
		return -2;
	}

	/* we do not need any arguments to callback functions */
	tcp_arg(pcb, NULL);

	/* listen for connections */
	pcb = tcp_listen(pcb);
	if (!pcb) {
		xil_printf("Out of memory while tcp_listen\n\r");
		return -3;
	}

	/* specify callback to use for incoming connections */
	tcp_accept(pcb, accept_callback);

	xil_printf("TCP echo server started @ port %d\n\r", port);

	return 0;
}
