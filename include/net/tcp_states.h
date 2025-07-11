/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the TCP protocol sk_state field.
 */
#ifndef _LINUX_TCP_STATES_H
#define _LINUX_TCP_STATES_H

enum {
	TCP_ESTABLISHED = 1,  // 三次握手完成
	TCP_SYN_SENT,		// 三次握手，客户端发送SYN后
	TCP_SYN_RECV,		// 三次握手，服务端收到SYN后
	TCP_FIN_WAIT1,		// 主动方（客户端）发送FIN后，四次挥手
	TCP_FIN_WAIT2,		// 主动方（客户端）收到服务端发出的ACK指令后
	TCP_TIME_WAIT,		// 主动方（客户端）收到客户端发送的FIN后，四次挥手
	TCP_CLOSE,			
	TCP_CLOSE_WAIT,		// 被动方（服务端）接收到FIN后，并发送ACK后，四次挥手
	TCP_LAST_ACK,		// 被动方（服务端）发送FIN后，四次挥手
	TCP_LISTEN,			// 服务端设置listen后，处于监听状态
	TCP_CLOSING,	/* Now a valid state */
	TCP_NEW_SYN_RECV,

	TCP_MAX_STATES	/* Leave at the end! */
};

#define TCP_STATE_MASK	0xF

#define TCP_ACTION_FIN	(1 << TCP_CLOSE)

enum {
	TCPF_ESTABLISHED = (1 << TCP_ESTABLISHED),
	TCPF_SYN_SENT	 = (1 << TCP_SYN_SENT),
	TCPF_SYN_RECV	 = (1 << TCP_SYN_RECV),
	TCPF_FIN_WAIT1	 = (1 << TCP_FIN_WAIT1),
	TCPF_FIN_WAIT2	 = (1 << TCP_FIN_WAIT2),
	TCPF_TIME_WAIT	 = (1 << TCP_TIME_WAIT),
	TCPF_CLOSE	 = (1 << TCP_CLOSE),
	TCPF_CLOSE_WAIT	 = (1 << TCP_CLOSE_WAIT),
	TCPF_LAST_ACK	 = (1 << TCP_LAST_ACK),
	TCPF_LISTEN	 = (1 << TCP_LISTEN),
	TCPF_CLOSING	 = (1 << TCP_CLOSING),
	TCPF_NEW_SYN_RECV = (1 << TCP_NEW_SYN_RECV),
};

#endif	/* _LINUX_TCP_STATES_H */
