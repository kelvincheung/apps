/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "log.h"
LOG_MODULE_DECLARE(LOG_MODULE_NAME);

#include <zephyr.h>
#include <misc/printk.h>
#include <shell/shell.h>
#include <logging/sys_log.h>
#include <stdlib.h>
#include <device.h>
#include <gpio.h>
#include <uart.h>
#include <led.h>
#include <irq_nextlevel.h>

#include <uwp_hal.h>
#include <pinmux.h>
#include <flash_map.h>

static int cmd_rand(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	u32_t rand = sys_rand32_get();

	shell_fprintf(shell, SHELL_NORMAL,
			"generate rand: 0x%x\n", rand);
	return 0;
}

#define SOFT_INT_IRQ_NUM	(0x0201)
#define SOFT_INT_FIQ_NUM	(0x0200)
#define SOFT_INT_AON_NUM	(0x0214)
#define	SOFT_IRQ			(0x01)
struct uwp_ictl_data {
	u32_t	base_addr;
};
#define DEV_DATA(dev) \
	((struct uwp_ictl_data * const)(dev)->driver_data)
#define INTC_STRUCT(dev) \
	((volatile struct uwp_intc *)(DEV_DATA(dev))->base_addr)

struct soft_irq {
	char *name;
	u32_t irq_num;
};

struct soft_irq soft_irq[] =
{
	{CONFIG_UWP_ICTL_0_NAME, SOFT_INT_IRQ_NUM},
	{CONFIG_UWP_ICTL_1_NAME, SOFT_INT_FIQ_NUM},
	{CONFIG_UWP_ICTL_2_NAME, SOFT_INT_AON_NUM},
	{NULL, 0}
};

static inline void uwp_ictl_irq_trigger_soft(struct device *dev)
{
	volatile struct uwp_intc *intc = INTC_STRUCT(dev);

	uwp_intc_trigger_soft(intc);
}

static inline void uwp_ictl_irq_clear_soft(struct device *dev)
{
	volatile struct uwp_intc *intc = INTC_STRUCT(dev);

	uwp_intc_clear_soft(intc);
}

static void intc_uwp_soft_irq(void *data)
{
	struct device **dev = (struct device **)data;

	if (!(*dev)) {
		printk("Unknown device: %p.\n", *dev);
		return;
	}

	uwp_ictl_irq_clear_soft(*dev);
	printk("soft irq: %s.\n", (*dev)->config->name);
}

static struct device *intc_irq;
static struct device *intc_fiq;
static struct device *intc_aon;
static int cmd_intc(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);


	intc_irq = device_get_binding(CONFIG_UWP_ICTL_0_NAME);
	if (!intc_irq) {
		printk("Can not find device: %s.\n",
				CONFIG_UWP_ICTL_0_NAME);
		return -1;
	}

	IRQ_CONNECT(SOFT_INT_IRQ_NUM, 0,
				intc_uwp_soft_irq,
				&intc_irq, 0);
	irq_enable_next_level(intc_irq, SOFT_IRQ);
	uwp_ictl_irq_trigger_soft(intc_irq);

	intc_fiq = device_get_binding(CONFIG_UWP_ICTL_1_NAME);
	if (intc_fiq == NULL) {
		printk("Can not find device: %s.\n",
				CONFIG_UWP_ICTL_1_NAME);
		return -1;
	}

	IRQ_CONNECT(SOFT_INT_FIQ_NUM, 0,
				intc_uwp_soft_irq,
				&intc_fiq, 0);
	irq_enable_next_level(intc_fiq, SOFT_IRQ);
	uwp_ictl_irq_trigger_soft(intc_fiq);

	intc_aon = device_get_binding(CONFIG_UWP_ICTL_2_NAME);
	if (intc_aon == NULL) {
		printk("Can not find device: %s.\n",
				CONFIG_UWP_ICTL_2_NAME);
		return -1;
	}

	IRQ_CONNECT(SOFT_INT_AON_NUM, 0,
				intc_uwp_soft_irq,
				&intc_aon, 0);
	irq_enable_next_level(intc_aon, SOFT_IRQ);
	uwp_ictl_irq_trigger_soft(intc_aon);

	return 0;
}

static int cmd_sleep(const struct shell *shell, size_t argc, char **argv)
{
	int time;

	int err = shell_cmd_precheck(shell, (argc == 2), NULL, 0);

	if (err) {
		return err;
	}

	time = atoi(argv[1]);
	shell_fprintf(shell, SHELL_NORMAL,
			"sleep %ims start...\n", time);

	k_sleep(time);

	shell_fprintf(shell, SHELL_NORMAL,
			"sleep stop.\n");

	return 0;
}

#define GPIO_PORT0	DT_GPIO_PO_UWP_NAME
#define UART_2			"UART_2"
static int cmd_uart(const struct shell *shell, size_t argc, char **argv)
{
	struct device *uart;
	char *str = "This is a test message from test command.\r\n";

	uart = device_get_binding(UART_2);
	if (uart == NULL) {
		shell_fprintf(shell, SHELL_ERROR,
				"Can not find device %s.\n", UART_2);
		return -1;
	}

	uart_irq_rx_enable(uart);

	uart_fifo_fill(uart, str, strlen(str));

	shell_fprintf(shell, SHELL_NORMAL,
			"test uart %s finish.\n", UART_2);

	return 0;
}

#ifdef CONFIG_LED_UWP
#define LED_NAME DT_GPIO_LEDS_NAME
#define LED_PIN1 DT_GPIO_LEDS_LED_1_PIN
#define LED_PIN2 DT_GPIO_LEDS_LED_2_PIN
#define LED_PIN3 DT_GPIO_LEDS_LED_3_PIN
static int cmd_led(const struct shell *shell, size_t argc, char **argv)
{
	struct device *led_dev;
	int led_pin, led;
	int on_off;
	int err = shell_cmd_precheck(shell, (argc == 3), NULL, 0);

	if (err) {
		return err;
	}

	led_dev = device_get_binding(LED_NAME);
	if (!led_dev) {
		shell_fprintf(shell, SHELL_ERROR,
				"Can not find device %s.\n", LED_NAME);
		return -1;
	}

	led = atoi(argv[1]);
	on_off = atoi(argv[2]);
	led_pin = LED_PIN1;

	switch (led) {
	case 1:
		led_pin = LED_PIN1; break;
	case 2:
		led_pin = LED_PIN2; break;
	case 3:
		led_pin = LED_PIN3; break;
	default:
		return -1;
	}

	if (on_off) {
		led_on(led_dev, led_pin);
	} else {
		led_off(led_dev, led_pin);
	}

	return 0;
}
#else
static int cmd_led(const struct shell *shell, size_t argc, char **argv)
{
	printk("CONFIG_LED is not enabled!\n");
	return 0;
}
#endif

#if defined(CONFIG_PINMUX_UWP) && defined (CONFIG_GPIO_UWP)
static struct gpio_callback button_cb;
int count;
#define BUTTON_PIN 0
#define GPIO0_REG (BASE_AON_PIN+0x10)
#define PMUX_DEV "pinmux_drv"

static void button_callback(struct device *port, struct gpio_callback *cb,
				 u32_t pin)
{
	printk("button pressed.\n");
}

static int cmd_button(const struct shell *shell, size_t argc, char **argv)
{
	struct device *gpio, *pmx_dev;

	gpio = device_get_binding(GPIO_PORT0);
	if (!gpio) {
		printk("Can not find device %s.\n", GPIO_PORT0);
		return -1;
	}
	pmx_dev = device_get_binding(PMUX_DEV);
	if (!pmx_dev) {
		printk("Can not find device %s.\n", PMUX_DEV);
		return -1;
	}
	count = 0;
	gpio_pin_configure(gpio, BUTTON_PIN, (GPIO_DIR_IN | GPIO_INT
			| GPIO_INT_EDGE | GPIO_PUD_PULL_UP
			| GPIO_INT_ACTIVE_LOW));
	pinmux_pin_pullup(pmx_dev, PIN_GPIO0_REG, PMUX_FPU_EN);

	gpio_init_callback(&button_cb, button_callback, BIT(BUTTON_PIN));
	gpio_add_callback(gpio, &button_cb);
	gpio_pin_enable_callback(gpio, BUTTON_PIN);

	return 0;
}
#else
static int cmd_button(const struct shell *shell, size_t argc, char **argv)
{
	printk("CONFIG_PINMUX_UWP or CONFIG_GPIO_UWP is not enabled!\n");
	return 0;
}
#endif

#if defined(CONFIG_PINMUX_UWP) && defined (CONFIG_GPIO_UWP)
#define PIN10 10
#define PULLUP 1
#define PMUX_DEV "pinmux_drv"
int reset_misc(void)
{
	struct device *pm_dev, *p0_dev;

	pm_dev = device_get_binding(PMUX_DEV);
	if (!pm_dev) {
		printk("Can not find device %s.\n", PMUX_DEV);
		return -1;
	}
	p0_dev = device_get_binding(GPIO_PORT0);
	if (!p0_dev) {
		printk("Can not find device %s.\n", GPIO_PORT0);
		return -1;
	}
	/*set INT as GPIO10*/
	pinmux_pin_set(pm_dev, PIN_INT_REG, PIN_FUNC_3);
	printk("set INT as GPIO10\n");
	/*set GPIO10 DIR out and enable GPIO10 */
	gpio_pin_configure(p0_dev, PIN10, GPIO_DIR_OUT);
	/*pull up GPIO10*/
	gpio_pin_write(p0_dev, PIN10, PULLUP);

	return 0;
}
static int cmd_reset(const struct shell *shell, size_t argc, char **argv)
{
	printk("resetting ...\n");
	k_sleep(50);				/* wait 50 ms */

	reset_misc();

	return 0;
}
#else
static int cmd_reset(const struct shell *shell, size_t argc, char **argv)
{
	printk("CONFIG_PINMUX_UWP or CONFIG_GPIO_UWP is not enabled!\n");
	return 0;
}
#endif

static int cmd_wipe_all_config(const struct shell *shell,
			size_t argc, char *argv[])
{
	const struct flash_area *fap;
	int rc;

	rc = flash_area_open(CONFIG_SETTINGS_FCB_FLASH_AREA, &fap);

	if (rc == 0) {
		rc = flash_area_erase(fap, 0, fap->fa_size);
		flash_area_close(fap);
		printk("\n###################################\n");
		printk("wipe success! please reset system! ..........\n");
		printk("###################################\n\n");
	}

	return rc;
}

SHELL_CREATE_STATIC_SUBCMD_SET(sub_test)
{
	SHELL_CMD(rand, NULL, "Generate random number.", cmd_rand),
	SHELL_CMD(intc, NULL, "Triger soft irq.", cmd_intc),
	SHELL_CMD(uart, NULL, "Fill a message to UART.", cmd_uart),
	SHELL_CMD(sleep, NULL, "Sleep for (n) micro seconds.\n"
			"Usage: sleep <ms>", cmd_sleep),
	SHELL_CMD(led, NULL, "Control led to turn on/off.\n"
			"Usage: led <led's number> <on/off>\n"
			"parameters:\n"
			"led's number: 1/2/3.\n"
			"\t   on/off: 1/0", cmd_led),
	SHELL_CMD(button, NULL, "Push button to control led3",
			cmd_button),
	SHELL_CMD(reset, NULL, "Reset the board",
			cmd_reset),
	SHELL_CMD(wipe, NULL, "wipe all configs in userdata partition.",
			cmd_wipe_all_config),
	SHELL_SUBCMD_SET_END /* Array terminated. */
};
/* Creating root (level 0) command "test" */
SHELL_CMD_REGISTER(test, &sub_test, "Function test commands", NULL);
