/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "logging/log.h"
LOG_MODULE_REGISTER(REPEATER);

#include <zephyr.h>
#include <misc/printk.h>
#include <net/net_mgmt.h>
#include <net/sntp.h>
#include <net/wifi_api.h>
#include <time.h>

#define NTP_SERVER	"cn.pool.ntp.org"
static struct net_mgmt_event_callback mgmt_cb;

/* Semaphore to indicate a lease has been acquired. */
static K_SEM_DEFINE(semsync, 0, 1);

extern void blues_init(void);
extern void wifi_repeater(void);

static void wifi_repeater_notify_connect(union wifi_notifier_val val)
{
	int status = val.val_char;

	if (!status)
		k_sem_give(&semsync);
}

static void dhcpv4_handler(struct net_mgmt_event_callback *cb,
			   u32_t mgmt_event,
			   struct net_if *iface)
{
	int i;
	bool notified = false;

	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}

	for (i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		char buf[NET_IPV4_ADDR_LEN];
		char *ipaddr, *netmask, *gateway;

		if (iface->config.ip.ipv4->unicast[i].addr_type !=
		    NET_ADDR_DHCP) {
			continue;
		}

		ipaddr =
		    iface->config.ip.ipv4->unicast[i].address.in_addr.s4_addr;
		netmask = iface->config.ip.ipv4->netmask.s4_addr;
		gateway = iface->config.ip.ipv4->gw.s4_addr;
		wifi_drv_notify_ip(iface, ipaddr, sizeof(struct in_addr));

		printf("IP address: %s\n",
		       net_addr_ntop(AF_INET, ipaddr, buf, sizeof(buf)));
		printf("Lease time: %us\n",
		       iface->config.dhcpv4.lease_time);
		printf("Subnet: %s\n",
		       net_addr_ntop(AF_INET, netmask, buf, sizeof(buf)));
		printf("Router: %s\n",
		       net_addr_ntop(AF_INET, gateway, buf, sizeof(buf)));

		if (!notified) {
			k_sem_give(&semsync);
			notified = true;
		}
		break;
	}
}

/**
 * Start a DHCP client, and wait for a lease to be acquired.
 */
void dhcpv4_startup(void)
{
	/* Wait for connection. */
	k_sem_take(&semsync, K_FOREVER);

	LOG_INF("starting DHCPv4");

	net_mgmt_init_event_callback(&mgmt_cb, dhcpv4_handler,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&mgmt_cb);

	net_dhcpv4_start(net_if_get_default());

	/* Wait for a lease. */
	k_sem_take(&semsync, K_FOREVER);
}

static void show_addrinfo(struct addrinfo *addr)
{
	char hr_addr[NET_IPV6_ADDR_LEN];
	void *a;

top:
	LOG_DBG("  flags   : %d", addr->ai_flags);
	LOG_DBG("  family  : %d", addr->ai_family);
	LOG_DBG("  socktype: %d", addr->ai_socktype);
	LOG_DBG("  protocol: %d", addr->ai_protocol);
	LOG_DBG("  addrlen : %d", addr->ai_addrlen);

	/* Assume two words. */
	LOG_DBG("   addr[0]: 0x%lx", ((uint32_t *)addr->ai_addr)[0]);
	LOG_DBG("   addr[1]: 0x%lx", ((uint32_t *)addr->ai_addr)[1]);

	if (addr->ai_next != 0) {
		addr = addr->ai_next;
		goto top;
	}

	a = &net_sin(addr->ai_addr)->sin_addr;

	LOG_INF("  Got %s",
		log_strdup(net_addr_ntop(addr->ai_family, a,
		hr_addr, sizeof(hr_addr))));

}

void do_sntp(void)
{
	char time_ip[NET_IPV6_ADDR_LEN];
	static struct addrinfo hints;
	struct addrinfo *haddr;
	s64_t time_base;
	struct sntp_ctx ctx;
	int rc;
	s64_t stamp;
	u64_t epoch_time;
	char time_str[sizeof("1970-01-01T00:00:00")];
	int cnt = 0;
	int ret;

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = 0;
	while ((ret = getaddrinfo(NTP_SERVER, "123", &hints,
				  &haddr)) && cnt < 3) {
		LOG_ERR("Unable to get address for NTP server, retrying");
		cnt++;
	}

	if (ret != 0) {
		LOG_ERR("Unable to get address of NTP server, exiting %d", ret);
		return;
	}


	LOG_INF("DNS resolved for %s:123", NTP_SERVER);
	time_base = 0;
	inet_ntop(AF_INET, &net_sin(haddr->ai_addr)->sin_addr, time_ip,
			haddr->ai_addrlen);
	show_addrinfo(haddr);

	LOG_INF("Sending NTP request for current time:");

	/* Initialize sntp */
	rc = sntp_init(&ctx, haddr->ai_addr, sizeof(struct sockaddr_in));
	if (rc < 0) {
		LOG_ERR("Unable to init sntp context: %d", rc);
		return;
	}

	rc = sntp_request(&ctx, K_FOREVER, &epoch_time);
	if (rc == 0) {
		stamp = k_uptime_get();
		time_base = epoch_time * MSEC_PER_SEC - stamp;

		/* Convert time to make sure. */
		time_t now = epoch_time;
		struct tm now_tm;

		gmtime_r(&now, &now_tm);
		strftime(time_str, sizeof(time_str), "%FT%T", &now_tm);
		LOG_INF("  Acquired time: %s", log_strdup(time_str));

	} else {
		LOG_ERR("  Failed to acquire SNTP, code %d\n", rc);
	}

	sntp_close(&ctx);
	freeaddrinfo(haddr);

	/* early return if we failed to acquire time */
	if (time_base == 0) {
		LOG_ERR("Failed to get NTP time");
		return;
	}
}

void main(void)
{
	LOG_INF("   [UNISOC Wi-Fi Repeater]");

#ifdef CONFIG_BT_UWP5661
	blues_init();
#endif
#ifdef CONFIG_WIFIMGR
	wifi_register_connection_notifier(wifi_repeater_notify_connect);
	wifi_repeater();
	dhcpv4_startup();
	do_sntp();
#endif

}
