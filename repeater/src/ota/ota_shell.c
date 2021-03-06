
/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <logging/log.h>
#define LOG_MODULE_NAME repeater
#define LOG_LEVEL CONFIG_REPEATER_LOG_LEVEL
LOG_MODULE_DECLARE(LOG_MODULE_NAME);

#include <zephyr.h>
#include <misc/printk.h>
#include <shell/shell.h>
#include <stdlib.h>
#include <stdio.h>
#include <version.h>

#include <net/net_core.h>
#include <net/net_ip.h>
#include <net/http_client.h>

#include <flash.h>
#include <device.h>

#include "ota_shell.h"

/*
 * Note that the http_ctx is quite large so be careful if that is
 * allocated from stack.
 */
static struct http_ctx http_ctx;
static struct stc_ota_cfg ota_cfg;
static int ota_req_size;


#define BIN_BUF_SIZE (10 * 1024)
u8_t *bin_buff;

static struct device *flash_device;

#define OTA_IMAGE_OK_OFFS(bank_offs) \
			(bank_offs + DT_FLASH_AREA_IMAGE_0_SIZE - 16 - 8)
#define OTA_MAGIC_OFFS(bank_offs) \
			(bank_offs + DT_FLASH_AREA_IMAGE_0_SIZE - 16)

struct waiter {
	struct http_ctx *ctx;
	struct k_sem wait;
	size_t total_len;
	size_t header_len;
};

/* blank check when erased flash */
static int do_checkblank(off_t offset, size_t len)
{
	u8_t temp[64];
	int count = len;
	int read_size = 0;
	int read_addr = offset;

	OTA_INFO("check Addr=0x%08X, len=%d.\n", (unsigned int)offset, len);
	while (count > 0) {
		if (count > sizeof(temp)) {
			read_size = sizeof(temp);
			count -= read_size;
		} else {
			read_size = count;
			count = 0;
		}

		memset(temp, 0, sizeof(temp));
		flash_read(flash_device, read_addr, temp, read_size);

		for (int i = 0; i < read_size; i++) {
			if (temp[i] != 0xff) {
				OTA_ERR("Blank Check Failed, Addr(0x%08X)=%d",
					(read_addr + i), temp[i]);
				return -1;
			}
		}

		read_addr += read_size;
	}

	return 0;
}

/* Erase area, handling write protection and printing on error. */
static int do_erase(off_t offset, size_t size)
{
	int ret = 0;
	int one_size;
	int count = size;
	int offset_temp = offset;

	while (count) {
		if (count > FLASH_ERASE_ONCE_SIZE) {
			one_size = FLASH_ERASE_ONCE_SIZE;
		} else {
			one_size = count;
		}
		ret = flash_write_protection_set(flash_device, false);
		if (ret) {
			OTA_ERR("Disable flash Protection Err:%d.\n", ret);
			return ret;
		}

		OTA_INFO("Erase Addr=0x%08X, size=%d.\n",
			 (unsigned int)offset_temp, one_size);
		ret = flash_erase(flash_device, offset_temp, one_size);
		if (ret) {
			OTA_ERR("flash_erase failed (err:%d).\n", ret);
			return ret;
		}

		ret = flash_write_protection_set(flash_device, true);
		if (ret) {
			OTA_ERR("Enable flash Protection Err:%d.\n", ret);
		}

		count -= one_size;
		offset_temp += one_size;
	}

	ret = do_checkblank(offset, size);
	return ret;
}

/* Write bytes, handling write protection and printing on error. */
static int do_write(off_t offset, u8_t *buf, size_t len, bool read_back)
{
	int ret = 0;
	u8_t *read_buf = NULL;

	ret = flash_write_protection_set(flash_device, false);
	if (ret) {
		OTA_ERR("Failed to disable flash protection (err: %d).\n", ret);
		return ret;
	}
	OTA_INFO("Write Addr=0x%08X.\n", (unsigned int)offset);

	ret = flash_write(flash_device, offset, buf, len);
	if (ret) {
		OTA_ERR("flash_write failed (err:%d).\n", ret);
		return ret;
	}
	ret = flash_write_protection_set(flash_device, true);
	if (ret) {
		OTA_ERR("Failed to enable flash protection (err: %d).\n", ret);
		return ret;
	}
	if (read_back) {
		read_buf = malloc(len);
		if (read_buf == NULL) {
			OTA_ERR
			    ("Read back check failed! Can't malloc buffer\n");
			return -1;
		}
		OTA_INFO("Reading back written bytes:%d\n", len);
		memset(read_buf, 0, len);
		flash_read(flash_device, offset, read_buf, len);
		for (int i = 0; i < len; i++) {
			if (read_buf[i] != buf[i]) {
				OTA_ERR
				    ("ERR: i=%d,write:%d,read:%d\n",
				     i, buf[i], read_buf[i]);
				ret = -1;
				break;
			}
		}
		free(read_buf);
	}
	return ret;
}

static void http_resp_callback(struct http_ctx *pctx,
					const char *body,
					int body_len,
					enum http_final_call final_data)
{
	static int app_buf_len;

	/** body can't receive commplete*/
	if (final_data == HTTP_DATA_MORE) {
		/** copy the body to app buff*/
		memcpy(&bin_buff[ota_req_size % BIN_BUF_SIZE], body, body_len);
		app_buf_len += body_len;
		printk("Got Http Body (%d) bytes, except (%d) bytes.\n",
			app_buf_len, pctx->rsp.content_length);
	} else {
		/** the body of http response has received complete*/
		if (pctx->rsp.message_complete) {
			app_buf_len = 0;
			printk("*****Response Body Received Complete******\n");
		}
	}
}

static int do_download(struct stc_ota_cfg *p_ota_cfg)
{
	unsigned long int flash_addr = p_ota_cfg->addr;
	int req_count = p_ota_cfg->bin_cfg.file_len;
	char *url = p_ota_cfg->bin_cfg.url_path;
	char *srv_ip = p_ota_cfg->bin_cfg.host_ip;
	char *srv_port = p_ota_cfg->bin_cfg.host_port;
	int req_count_start = 0;
	int req_count_end = 0;
	int repeate_count = 0;
	int restart_count = 0;
	int write_size = 0;
	char temp[200];
	int ret;

restart:
	restart_count++;
	/*init http client */
	ota_req_size = 0;
	ret = http_client_init(&http_ctx, srv_ip, srv_port, false);
	if (ret < 0) {
		OTA_ERR("HTTP init failed (%d)", ret);
		goto out;
	}

	http_set_resp_callback(&http_ctx, http_resp_callback);
	/*request the file from servers and write to flash */
	while (req_count > 0) {
		if (req_count > OTA_COUNT_EACH_ONE) {
			req_count_end =
			    req_count_start + OTA_COUNT_EACH_ONE - 1;
			req_count -= OTA_COUNT_EACH_ONE;
			write_size += OTA_COUNT_EACH_ONE;
		} else {
			req_count_end = req_count_start + req_count - 1;
			write_size += req_count;
			req_count = 0;	/*the last request */
		}

repeate:
		OTA_INFO("req count:req_count_start=%d req_count_end=%d.\n",
			 req_count_start, req_count_end);

		memset(temp, 0, sizeof(temp));
		sprintf(temp, "Range: bytes=%d-%d\r\n",
				req_count_start, req_count_end);
		http_add_header_field(&http_ctx, temp);

		ret = http_client_get(&http_ctx, url, true, NULL);

		if (ret < 0) {
			repeate_count++;
			OTA_ERR("HTTP Get Request failed (%d).\n", ret);
			if (restart_count >= 5) {
				OTA_INFO(" Download Failed!\n");
				goto out;
			}
			if ((repeate_count >= OTA_HTTP_REPEAT_REQ_MAX) ||
				(ret == -1)) {
				OTA_INFO("Disconnect! Restart Http client!\n");
				http_client_close(&http_ctx);
				goto restart;
			}


			k_sleep(500);
			goto repeate;
		}

		/*got BIN_BUF_SIZE, so need to write flash */
		if ((0 == (req_count_end + 1) % BIN_BUF_SIZE)
		    || (req_count == 0)) {
			OTA_INFO("Write Flash[0x%08lX]\n",
			     flash_addr);

			do_write(flash_addr, bin_buff, write_size, true);

			flash_addr = flash_addr + BIN_BUF_SIZE;
			write_size = 0;
			memset(bin_buff, 0, BIN_BUF_SIZE);
		}

		k_sleep(50);
		req_count_start = req_count_end + 1;
		ota_req_size = req_count_start;
		repeate_count = 0;
	}
	ret = 0;
	OTA_INFO(" Download Done!\n");
out:
	http_client_close(&http_ctx);

	return ret;
}

/**
 * Marks the image in slot 1 as pending.  On the next reboot, the system will
 * perform a one-time boot of the slot 1 image.
 *
 * @param permanent         Whether the image should be used permanently or
 *                              only tested once:
 *                                  0=run image once, then confirm or revert.
 *                                  1=run image forever.
 *
 * @return                  0 on success; nonzero on failure.
 */
static int ota_set_kerenl_pending(bool permanent)
{
	u32_t off;
	u8_t buf[8];
	u32_t boot_img_magic_1[4] = {
		0xf395c277,
		0x7fefd260,
		0x0f505235,
		0x8079b62c,
	};

	off = OTA_MAGIC_OFFS(DT_FLASH_AREA_IMAGE_1_OFFSET);
	do_write(off, (u8_t *) boot_img_magic_1, 16, true);

	if (permanent) {
		off = OTA_IMAGE_OK_OFFS(DT_FLASH_AREA_IMAGE_1_OFFSET);
		memset(buf, 0xFF, sizeof(buf));
		buf[0] = 0x01;
		do_write(off, buf, sizeof(buf), true);
	}

	return 0;
}

static int ota_get_modem_upgrade_area(void)
{
	int read_addr = OTA_MODEM_START_ADDR_OFF;
	u8_t buf[4];
	int *p_addr;
	int default_addr = OTA_FA_MODEM_0_ADDR;
	int ret_val = 0;

	flash_read(flash_device, read_addr, buf, sizeof(buf));

	p_addr = (int *)&buf;
	OTA_INFO("Read modem start addr: %08X", *p_addr);

	if (*p_addr == OTA_FA_MODEM_0_ADDR) {
		ret_val = OTA_FA_MODEM_1;

	} else if (*p_addr == OTA_FA_MODEM_1_ADDR) {
		ret_val = OTA_FA_MODEM_0;

	} else {
		OTA_ERR("modem start addr not config, so, set default");

		/*erase flash */
		do_erase(read_addr, OTA_MODEM_START_ADDR_SIZE);

		/*write and check */
		do_write(read_addr, (u8_t *)&default_addr, sizeof(int), true);

		ret_val = OTA_FA_MODEM_1;
	}

	return ret_val;
}

static int ota_check_modem(void)
{
	int modem_addr = ota_cfg.addr + 0x02000000;
	int rc = 0;

	/*check modem bin is ok or not*/

	/*config modem start addr*/
	if (rc == 0) {
		do_erase(OTA_MODEM_START_ADDR_OFF, OTA_MODEM_START_ADDR_SIZE);
		rc = do_write(OTA_MODEM_START_ADDR_OFF,
			(u8_t *)&modem_addr, sizeof(int), true);
		OTA_INFO("Update Modem Start Addrr=0x%08X", modem_addr);

		if (rc) {
			OTA_ERR("Set modem start addr failed! rc=%d", rc);
		}
	} else {
		OTA_ERR("Modem bin file check err! rc=%d", rc);
	}

	return rc;
}

static int ota_init_cfg(u8_t type)
{
	const struct flash_area *fa;
	int flash_area_id;
	int rc = 0;

	ota_cfg.bin_cfg.host_ip = CONFIG_OTA_SVR_ADDR;
	ota_cfg.bin_cfg.host_port = CONFIG_OTA_SVR_PORT;

	switch (type) {
	case OTA_KERNEL:
		ota_cfg.bin_cfg.url_path = OTA_KERNEL_BIN_URL;
		ota_cfg.bin_cfg.file_len = OTA_KERNEL_BIN_SIZE;

		rc = flash_area_open(OTA_FA_SLOT_1, &fa);
		break;
	case OTA_MODEM:
		ota_cfg.bin_cfg.url_path = OTA_MODEM_BIN_URL;
		ota_cfg.bin_cfg.file_len = OTA_MODEM_BIN_SIZE;

		flash_area_id = ota_get_modem_upgrade_area();
		rc = flash_area_open(flash_area_id, &fa);
		break;
	case OTA_OTHER:
	default:
		break;
	}

	OTA_INFO("fa->fa_off=%08lX, fa->fa_size=%08X",
		fa->fa_off, fa->fa_size);
	ota_cfg.addr = fa->fa_off;
	rc = do_erase(fa->fa_off, fa->fa_size);
	return rc;
}

static int ota_download_finished(u8_t type)
{
	switch (type) {
	case OTA_KERNEL:
		ota_set_kerenl_pending(true);
	break;
	case OTA_MODEM:
	/*set modem patition ok*/
		ota_check_modem();
	break;
	case OTA_OTHER:
	default:
	break;
	}
}

static int ota_download(const struct shell *shell, size_t argc, char **argv)
{
	int ret = 0;

	flash_device = device_get_binding(DT_FLASH_DEV_NAME);
	if (flash_device) {
		OTA_INFO("Found flash device %s.\n", DT_FLASH_DEV_NAME);
		OTA_INFO("Flash I/O commands can be run.\n");
	} else {
		OTA_INFO("**No flash device found!**\n");
		return -1;
	}

	shell_fprintf(shell, SHELL_NORMAL,
		      "Got Download parameter is :%s\n", argv[1]);
	if (!strcmp(argv[1], "-k")) {
		ota_cfg.type = OTA_KERNEL;

	} else if (!strcmp(argv[1], "-m")) {
		ota_cfg.type = OTA_MODEM;

	} else if (!strcmp(argv[1], "-t")) {
		ota_cfg.type = OTA_KERNEL;

	} else {
		ota_cfg.type = OTA_OTHER;
		shell_fprintf(shell, SHELL_ERROR,
			      "Invalid input,Parametr is %d.\n", argv[1]);
		return -1;
	}

	ret = ota_init_cfg(ota_cfg.type);
	if (ret != 0) {
		OTA_ERR("OTA Config Init Failed! ret=%d", ret);
		return -1;
	}

	bin_buff = malloc(BIN_BUF_SIZE);
	if (bin_buff == NULL) {
		OTA_ERR("bin_buff malloc failed!\n");
		return -1;
	}

	memset(bin_buff, 0, BIN_BUF_SIZE);
	ret = do_download(&ota_cfg);
	free(bin_buff);

	/* download finished*/
	if (!ret) {
		ota_download_finished(ota_cfg.type);
	} else {
		OTA_INFO("Downlaod Failed! ret=%d\n", ret);
	}

	return 0;
}

static int ota_verify(const struct shell *shell, size_t argc, char **argv)
{
	unsigned long int check_flash_addr1, check_flash_addr2;
	int check_size = 32;
	char buf[check_size];
	int ret;

	/*check the parameter */
	if (!strcmp(argv[1], "-s0")) {
		check_flash_addr1 = DT_FLASH_AREA_IMAGE_0_OFFSET;
		check_flash_addr2 =
		  check_flash_addr1 + DT_FLASH_AREA_IMAGE_0_SIZE - 32;
		OTA_INFO("Check slot0 Partition...\n");
	} else if (!strcmp(argv[1], "-s1")) {
		check_flash_addr1 = DT_FLASH_AREA_IMAGE_1_OFFSET;
		check_flash_addr2 =
		  check_flash_addr1 + DT_FLASH_AREA_IMAGE_1_SIZE - 32;
		OTA_INFO("Check slot1 Partition...\n");
	} else {
		shell_fprintf(shell, SHELL_ERROR, "Invalid input.\n");
		return -1;
	}

	flash_device = device_get_binding(DT_FLASH_DEV_NAME);
	if (flash_device) {
		printk("Found flash device %s.\n", DT_FLASH_DEV_NAME);
		printk("Flash I/O commands can be run.\n");
	} else {
		printk("**No flash device found!**\n");
		return -1;
	}

	memset(buf, 0, sizeof(buf));
	ret = flash_read(flash_device, check_flash_addr1, buf, sizeof(buf));

	OTA_INFO("addr1:");
	for (int i = 0; i < sizeof(buf); i++) {
		OTA_INFO(" %02X", buf[i]);
	}
	OTA_INFO("\n");

	memset(buf, 0, sizeof(buf));
	ret = flash_read(flash_device, check_flash_addr2, buf, sizeof(buf));

	OTA_INFO("addr2:");
	for (int i = 0; i < sizeof(buf); i++) {
		OTA_INFO(" %02X", buf[i]);
	}
	OTA_INFO("\n");

	return 0;
}

static int test_flash(const struct shell *shell, size_t argc, char **argv)
{
	char buf[16];
	u32_t test_flash_addr = (DT_FLASH_AREA_STORAGE_OFFSET + 0x4000);
	int test_flash_size = OTA_TEST_BIN_SIZE;

	/*init flash */
	flash_device = device_get_binding(DT_FLASH_DEV_NAME);
	if (flash_device) {
		printk("Found flash device %s.\n", DT_FLASH_DEV_NAME);
		printk("Flash I/O commands can be run.\n");
	} else {
		printk("**No flash device found!**\n");
		return -1;
	}

	shell_fprintf(shell, SHELL_NORMAL, "test_flash_addr=%08X\n",
		      test_flash_addr);
	/*erase flash */
	do_erase(test_flash_addr, test_flash_size);

	memset(buf, 0xAA, sizeof(buf));
	/*write and check */
	do_write(test_flash_addr, buf, sizeof(buf), true);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ota_cmd,
	SHELL_CMD(download, NULL,
	"download the firmware from internet,include kernel and modem.\n"
	"Usage: download <type> -k:download kernel -m:download modem",
	ota_download),
	SHELL_CMD(verify, NULL,
	"verify the bin code.\n"
	"Usage: download <type> -s0:check slot0 -s1:check slot1",
	ota_verify),
	SHELL_CMD(testflash, NULL,
	"test flash for writing and reading!",
	test_flash),
	SHELL_SUBCMD_SET_END	/* Array terminated. */
);

/* Creating root (level 0) command "app" */
SHELL_CMD_REGISTER(ota, &ota_cmd, "OTA with upgrade firmware", NULL);
