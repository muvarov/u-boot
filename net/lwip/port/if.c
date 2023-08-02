// SPDX-License-Identifier: GPL-2.0

/*
 * (C) Copyright 2023 Linaro Ltd. <maxim.uvarov@linaro.org>
 */

#include <common.h>
#include <command.h>
#include <net/eth.h>
#include <dm/device.h>
#include <dm/uclass-id.h>
#include <dm/uclass.h>
#include "lwip/debug.h"
#include "lwip/arch.h"
#include "netif/etharp.h"
#include "lwip/stats.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/netif.h"
#include "lwip/ethip6.h"
#include "lwip/timeouts.h"

#include "lwip/ip.h"

/*
 * MAC_ADDR_STRLEN: length of mac address string
 */
#define MAC_ADDR_STRLEN 18

int ulwip_active(void)
{
	struct udevice *udev;
	struct ulwip *ulwip;

	udev = eth_get_dev();
	if (!udev)
		return 0;

	ulwip = eth_lwip_priv(udev);
	return ulwip->active;
}

int ulwip_in_loop(void)
{
	struct udevice *udev;
	struct ulwip *ulwip;

	udev = eth_get_dev();
	if (!udev)
		return 0;

	sys_check_timeouts();

	ulwip = eth_lwip_priv(udev);
	return ulwip->loop;
}

void ulwip_loop_set(int loop)
{
	struct udevice *udev;
	struct ulwip *ulwip;

	udev = eth_get_dev();
	if (!udev)
		return;

	ulwip = eth_lwip_priv(udev);
	ulwip->loop = loop;
}

void ulwip_exit(int err)
{
	struct udevice *udev;
	struct ulwip *ulwip;

	udev = eth_get_dev();
	if (!udev)
		return;

	ulwip = eth_lwip_priv(udev);
	ulwip->loop = 0;
	ulwip->err = err;
}

int ulwip_app_get_err(void)
{
	struct udevice *udev;
	struct ulwip *ulwip;

	udev = eth_get_dev();
	if (!udev)
		return 0;

	ulwip = eth_lwip_priv(udev);
	return ulwip->err;
}

typedef struct {
} ulwip_if_t;

static struct pbuf *low_level_input(uchar *data, int len)
{
	struct pbuf *p, *q;

	/* We allocate a pbuf chain of pbufs from the pool. */
	p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
	if (!p) {
		LINK_STATS_INC(link.memerr);
		LINK_STATS_INC(link.drop);
		return NULL;
	}

	for (q = p; q != NULL; q = q->next) {
		memcpy(q->payload, data, q->len);
		data += q->len;
	}

	LINK_STATS_INC(link.recv);

	return p;
}

void ulwip_poll(uchar *in_packet, int len)
{
	struct pbuf *p;
	int err;
	struct netif *netif;
	int eth_idx;

	eth_idx = eth_get_dev_index();
	if (eth_idx < 0) {
		log_err("no eth idx\n");
		return;
	}

	netif = netif_get_by_index(eth_idx + 1);
	if (!netif) {
		log_err("!netif\n");
		return;
	}

	p = low_level_input(in_packet, len);
	if (!p) {
		log_err("no mem\n");
		return;
	}

	/* ethernet_input always returns ERR_OK */
	err = ethernet_input(p, netif);
	if (err)
		log_err("ip4_input err %d\n", err);
}

static int ethernetif_input(struct pbuf *p, struct netif *netif)
{
	return 0;
}

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
	int err;

	/* switch dev to active state */
	eth_init_state_only();

	err = eth_send(p->payload, p->len);
	if (err) {
		log_err("eth_send error %d\n", err);
		return ERR_ABRT;
	}
	return ERR_OK;
}

err_t ulwip_if_init(struct netif *netif)
{
	ulwip_if_t *uif;
	struct ulwip *ulwip;

	uif = malloc(sizeof(ulwip_if_t));
	if (!uif) {
		log_err("uif: out of memory\n");
		return ERR_MEM;
	}
	netif->state = uif;

#if defined(CONFIG_LWIP_LIB_DEBUG)
	log_info("              MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
		 netif->hwaddr[0], netif->hwaddr[1], netif->hwaddr[2],
		 netif->hwaddr[3], netif->hwaddr[4], netif->hwaddr[5]);
	log_info("             NAME: %s\n", netif->name);
#endif
#if LWIP_IPV4
	netif->output = etharp_output;
#endif
#if LWIP_IPV6
	netif->output_ip6 = ethip6_output;
#endif

	netif->linkoutput = low_level_output;
	netif->mtu = 1500;
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;

	ulwip = eth_lwip_priv(eth_get_dev());
	ulwip->init_done = 1;

	return ERR_OK;
}

int ulwip_init(void)
{
	ip4_addr_t ipaddr, netmask, gw;
	struct netif *unetif;
	struct ulwip *ulwip;
	struct udevice *udev;
	int ret;
	unsigned char env_enetaddr[ARP_HLEN];
	const struct udevice *dev;
	struct uclass *uc;

	eth_set_current();

	udev = eth_get_dev();
	if (!udev) {
		log_err("no active eth device\n");
		return ERR_IF;
	}

	ulwip = eth_lwip_priv(udev);
	if (ulwip->init_done) {
		log_info("init already done for %s\n", udev->name);
		ret = eth_init();
		if (ret)
			return ERR_IF;
		ulwip->active = 1;
		return CMD_RET_SUCCESS;
	}

	eth_init_rings();

	ret = eth_init();
	if (ret) {
		log_err("eth_init error %d\n", ret);
		return ERR_IF;
	}

	uclass_id_foreach_dev(UCLASS_ETH, dev, uc) {
		char ipstr[IP4ADDR_STRLEN_MAX];
		char maskstr[IP4ADDR_STRLEN_MAX];
		char gwstr[IP4ADDR_STRLEN_MAX];
		char hwstr[MAC_ADDR_STRLEN];
		char *env;

		eth_env_get_enetaddr_by_index("eth", dev_seq(dev), env_enetaddr);
		log_info("eth%d: %s %pM %s\n", dev_seq(dev), dev->name, env_enetaddr,
			 udev == dev ? "active" : "");

		unetif = malloc(sizeof(struct netif));
		if (!unetif)
			return ERR_MEM;
		memset(unetif, 0, sizeof(struct netif));

		ip4_addr_set_zero(&gw);
		ip4_addr_set_zero(&ipaddr);
		ip4_addr_set_zero(&netmask);

		if (dev_seq(dev) == 0) {
			snprintf(ipstr, IP4ADDR_STRLEN_MAX, "ipaddr");
			snprintf(maskstr, IP4ADDR_STRLEN_MAX, "netmask");
			snprintf(gwstr, IP4ADDR_STRLEN_MAX, "gw");
		} else {
			snprintf(ipstr, IP4ADDR_STRLEN_MAX, "ipaddr%d", dev_seq(dev));
			snprintf(maskstr, IP4ADDR_STRLEN_MAX, "netmask%d", dev_seq(dev));
			snprintf(gwstr, IP4ADDR_STRLEN_MAX, "gw%d", dev_seq(dev));
		}
		snprintf(hwstr, MAC_ADDR_STRLEN, "%pM",  env_enetaddr);
		snprintf(unetif->name, 2, "%d", dev_seq(dev));

		string_to_enetaddr(hwstr, unetif->hwaddr);
		unetif->hwaddr_len = ETHARP_HWADDR_LEN;

		env = env_get(ipstr);
		if (env)
			ipaddr_aton(env, &ipaddr);

		env = env_get(maskstr);
		if (env)
			ipaddr_aton(env, &netmask);

		if (IS_ENABLED(CONFIG_LWIP_LIB_DEBUG)) {
			log_info("Starting lwIP\n ");
			log_info("	netdev: %s\n", dev->name);
			log_info("	    IP: %s\n", ip4addr_ntoa(&ipaddr));
			log_info("          GW: %s\n", ip4addr_ntoa(&gw));
			log_info("        mask: %s\n", ip4addr_ntoa(&netmask));
		}

#if LWIP_IPV6
#define MAC_FROM_48_BIT 1
		netif_create_ip6_linklocal_address(unetif, MAC_FROM_48_BIT);
		log_info("             IPv6: %s\n", ip6addr_ntoa(netif_ip6_addr(unetif, 0)));
#endif /* LWIP_IPV6 */

		if (!netif_add(unetif, &ipaddr, &netmask, &gw,
			       unetif, ulwip_if_init, ethernetif_input)) {
			log_err("err: netif_add failed!\n");
			free(unetif);
			return ERR_IF;
		}

		if (!netif_is_up(unetif))
			netif_set_up(unetif);
		netif_set_link_up(unetif);
	}

	if (IS_ENABLED(CONFIG_LWIP_LIB_DEBUG)) {
		log_info("Initialized LWIP stack\n");
	}

	ulwip->active = 1;
	return CMD_RET_SUCCESS;
}

/* placeholder, not used now */
void ulwip_destroy(void)
{
}
