
/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain 
 * this notice you can do whatever you want with this stuff. If we meet some day, 
 * and you think this stuff is worth it, you can buy me a beer in return. 
 * ----------------------------------------------------------------------------
 */


#include <esp8266.h>
#include "ansi_parser_callbacks.h"
#include "wifimgr.h"
#include "persist.h"
#include "screen.h"
#include "ansi_parser.h"

#define BTNGPIO 0

/** Set to false if GPIO0 stuck low on boot (after flashing) */
static bool enable_ap_button = false;

static ETSTimer resetBtntimer;
static ETSTimer blinkyTimer;
static ETSTimer ansiParserResetTimer;

static void ICACHE_FLASH_ATTR ansiParserResetTimerCb(void *arg) {
	static u32 last_charcnt = 0;
	static int same_charcnt = -1;
	if (termconf->parser_tout_ms == 0) return;

	if (last_charcnt != ansi_parser_char_cnt) {
		last_charcnt = ansi_parser_char_cnt;
		same_charcnt = 0;
	}
	else {
		if (same_charcnt != -1) {
			same_charcnt++;
			if (same_charcnt > termconf->parser_tout_ms) {
				ansi_parser_reset();
				same_charcnt = -1;
			}
		}
	}
}


// Holding BOOT pin triggers AP reset, then Factory Reset.
// Indicate that by blinking the on-board LED.
// -> ESP-01 has at at GPIO1, ESP-01S at GPIO2. We have to use both for compatibility.

static void ICACHE_FLASH_ATTR bootHoldIndicatorTimerCb(void *arg) {
	static bool state = true;

	if (GPIO_INPUT_GET(BTNGPIO)) {
		// if user released, shut up
		state = 1;
	}

	if (state) {
		GPIO_OUTPUT_SET(1, 1);
		GPIO_OUTPUT_SET(2, 1);
	} else {
		GPIO_OUTPUT_SET(1, 0);
		GPIO_OUTPUT_SET(2, 0);
	}

	state = !state;
}

static void ICACHE_FLASH_ATTR resetBtnTimerCb(void *arg) {
	static int resetCnt=0;
	if (enable_ap_button && !GPIO_INPUT_GET(BTNGPIO)) {
		resetCnt++;

		// indicating AP reset
		if (resetCnt == 2) {
			// LED pin as output (Normally UART output)
			PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_GPIO1);
			PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_GPIO2);

			// LED on
			GPIO_OUTPUT_SET(1, 0);
			GPIO_OUTPUT_SET(2, 0);

			os_timer_disarm(&blinkyTimer);
			os_timer_setfn(&blinkyTimer, bootHoldIndicatorTimerCb, NULL);
			os_timer_arm(&blinkyTimer, 500, 1);
		}

		// indicating we'll perform a factory reset
		if (resetCnt == 10) {
			os_timer_disarm(&blinkyTimer);
			os_timer_setfn(&blinkyTimer, bootHoldIndicatorTimerCb, NULL);
			os_timer_arm(&blinkyTimer, 100, 1);
		}
	} else {
		// Switch LED pins back to UART mode
		PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_U0TXD);
        if (sysconf->gpio2_conf == GPIOCONF_OFF) {
            // only if uart is enabled
            PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_U1TXD_BK);
        }

		if (resetCnt>=12) { //6 secs pressed - FR (timer is at 500 ms)
			info("Restoring to default settings via BOOT button!");
			persist_restore_default();
		}
		else if (resetCnt>=2) { //1 sec pressed
			info("BOOT-button triggered reset to AP mode...");

			// Enter "rescue mode".
			wificonf->opmode = STATIONAP_MODE;
			wifimgr_apply_settings();
			persist_store();
		}
		resetCnt=0;
	}
}

void ICACHE_FLASH_ATTR ioInit() {
	// GPIO1, GPIO2, GPIO3 - UARTs.

	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0);
	gpio_output_set(0, 0, 0, (1<<BTNGPIO));

	os_timer_disarm(&resetBtntimer);
	os_timer_setfn(&resetBtntimer, resetBtnTimerCb, NULL);
	os_timer_arm(&resetBtntimer, 500, 1);

	os_timer_disarm(&ansiParserResetTimer);
	os_timer_setfn(&ansiParserResetTimer, ansiParserResetTimerCb, NULL);
	os_timer_arm(&ansiParserResetTimer, 1, 1);

	// One way to enter AP mode - hold GPIO0 low.
	if (GPIO_INPUT_GET(BTNGPIO) == 0) {
		// starting "in BOOT mode" - do not install the AP reset timer
		warn("GPIO0 stuck low - AP reset button disabled.\n");
	} else {
		enable_ap_button = true;
		dbg("Note: Hold GPIO0 low for reset to AP mode.\n");
	}
}

struct pinmapping {
	bool set;
	bool reset;
	bool enable;
	bool disable;
	bool input;
	bool pullup;
};

void ICACHE_FLASH_ATTR userGpioInit(void)
{
	const struct pinmapping pin_mappings[5] = {
	//   S  R  E  D  I  P
		{0, 1, 0, 1, 0, 0}, // OFF
		{0, 1, 1, 0, 0, 0}, // OUT 0
		{1, 0, 1, 0, 0, 0}, // OUT 1
		{0, 0, 0, 1, 1, 1}, // IN PULL
		{0, 0, 0, 1, 1, 0}, // IN NOPULL
	};

	u8 num;
	const struct pinmapping *pm;

	// GPIO2
	num = 2;
	pm = &pin_mappings[sysconf->gpio2_conf];
	gpio_output_set((uint32) (pm->set << num), (uint32) (pm->reset << num), (uint32) (pm->enable << num), (uint32) (pm->disable << num));
	if (pm->pullup) {
		PIN_PULLUP_EN(PERIPHS_IO_MUX_GPIO2_U);
	} else {
		PIN_PULLUP_DIS(PERIPHS_IO_MUX_GPIO2_U);
	}
	if (sysconf->gpio2_conf == GPIOCONF_OFF) {
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_U1TXD_BK);
    } else {
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_GPIO2);
    }

	// GPIO4
	num = 4;
	pm = &pin_mappings[sysconf->gpio4_conf];
	gpio_output_set((uint32) (pm->set << num), (uint32) (pm->reset << num), (uint32) (pm->enable << num), (uint32) (pm->disable << num));
	if (pm->pullup) {
		PIN_PULLUP_EN(PERIPHS_IO_MUX_GPIO4_U);
	} else {
		PIN_PULLUP_DIS(PERIPHS_IO_MUX_GPIO4_U);
	}

	// GPIO5
	num = 5;
	pm = &pin_mappings[sysconf->gpio5_conf];
	gpio_output_set((uint32) (pm->set << num), (uint32) (pm->reset << num), (uint32) (pm->enable << num), (uint32) (pm->disable << num));
	if (pm->pullup) {
		PIN_PULLUP_EN(PERIPHS_IO_MUX_GPIO5_U);
	} else {
		PIN_PULLUP_DIS(PERIPHS_IO_MUX_GPIO5_U);
	}
}
