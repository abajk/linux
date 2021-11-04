// SPDX-License-Identifier: GPL-2.0
/*
 * Lantiq Tantos PMAC tag support
 *
 * Copyright (C) 2021 - 2022 Aleksander Jan Bajkowski <olek2@wp.pl>
 */

#include <linux/bitops.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <net/dsa.h>

#include "tag.h"

#define TANTOS_NAME			"tantos"

#define TANTOS_TX_HEADER_LEN		4

/* special tag in TX path header */
/* Byte 0 */
#define TANTOS_TX_SLPID_SHIFT		0	/* source port ID */
#define  TANTOS_TX_SLPID_CPU		2

/* Byte 3 */
#define TANTOS_TX_DPID_EN		BIT(0)

#define TANTOS_RX_HEADER_LEN	8

/* special tag in RX path header */
/* Byte 2 */
#define TANTOS_RX_SPPID_SHIFT		5
#define TANTOS_RX_SPPID_MASK		GENMASK(7, 5)

static struct sk_buff *tantos_tag_xmit(struct sk_buff *skb,
				       struct net_device *dev)
{
	struct dsa_port *dp = dsa_user_to_port(dev);
	u8 *tantos_tag;

	skb_push(skb, TANTOS_TX_HEADER_LEN);

	tantos_tag = skb->data;
	tantos_tag[0] = TANTOS_TX_SLPID_CPU;
	tantos_tag[1] = dp->index;
	tantos_tag[3] = TANTOS_TX_DPID_EN;

	return skb;
}

static struct sk_buff *tantos_tag_rcv(struct sk_buff *skb,
				      struct net_device *dev)
{
	int port;
	u8 *tantos_tag;

	if (unlikely(!pskb_may_pull(skb, TANTOS_RX_HEADER_LEN)))
		return NULL;

	tantos_tag = skb->data - ETH_HLEN;

	/* Get source port information */
	port = (tantos_tag[2] & TANTOS_RX_SPPID_MASK) >> TANTOS_RX_SPPID_SHIFT;
	skb->dev = dsa_conduit_find_user(dev, 0, port);
	if (!skb->dev)
		return NULL;

	/* remove Tantos tag */
	skb_pull_rcsum(skb, TANTOS_RX_HEADER_LEN);

	return skb;
}

static const struct dsa_device_ops tantos_netdev_ops = {
	.name = TANTOS_NAME,
	.proto	= DSA_TAG_PROTO_TANTOS,
	.xmit = tantos_tag_xmit,
	.rcv = tantos_tag_rcv,
	.needed_headroom = TANTOS_RX_HEADER_LEN,
};

MODULE_DESCRIPTION("DSA tag driver for Lantiq Tantos switches");
MODULE_LICENSE("GPL");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_TANTOS, TANTOS_NAME);

module_dsa_tag_driver(tantos_netdev_ops);
