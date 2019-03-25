#ifndef _WIFI_NPI_H_
#define _WIFI_NPI_H_

#include <net/wifi_drv.h>
#include <stdlib.h>

#define WIFINPI_DEV_NAME_STA	"WIFI_STA"
#define WIFINPI_ETH_ALEN	6

int npi_wifi_iface_init(struct device **dev);
int npi_open_station(struct device *dev);
int npi_close_station(struct device *dev);

#endif
