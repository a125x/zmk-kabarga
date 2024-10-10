#include <zmk/events/activity_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zephyr/drivers/led.h>

#include <math.h>
#include <stdlib.h>

#include <zmk/activity.h>
#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

#include <zmk/hid_indicators.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>
#include <zmk/workqueue.h>

#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// timeout
#define LED_BLINK_PROFILE_DELAY 200
#define LED_BLINK_CONN_DELAY 140
#define LED_FADE_DELAY 2
#define LED_BATTERY_SHOW_DELAY 700
#define LED_BATTERY_SLEEP_SHOW 1000
#define LED_BATTERY_BLINK_DELAY 200
#define LED_USB_BLINK_DELAY 200

#define LED_STATUS_ON 100
#define LED_STATUS_OFF 0

// animation options
#define disable_led_sleep_pc
// #define show_led_idle

// led array count
#define CHILD_COUNT(...) +1
#define DT_NUM_CHILD(node_id) (DT_FOREACH_CHILD(node_id, CHILD_COUNT))
#define BACKLIGHT_NUM_LEDS DT_NUM_CHILD(DT_CHOSEN(zmk_backlight))

bool check_conn_working = false;
static enum zmk_usb_conn_state usb_conn_state = ZMK_USB_CONN_NONE;

struct led {
    const struct device *dev;
    uint32_t id;
};

enum {
    BAT_1,
    BAT_2,
    BAT_3,
    STATUS
};

struct led pwm_leds[] = {
    [BAT_1] = {
        .dev = DEVICE_DT_GET(DT_CHOSEN(zmk_backlight)),
        .id = 0,
    },
    [BAT_2] = {
        .dev = DEVICE_DT_GET(DT_CHOSEN(zmk_backlight)),
        .id = 1,
    },
    [BAT_3] = {
        .dev = DEVICE_DT_GET(DT_CHOSEN(zmk_backlight)),
        .id = 2,
    },
    [STATUS] = {
        .dev = DEVICE_DT_GET(DT_CHOSEN(zmk_backlight)),
        .id = 3,
    },
};

static void led_fade_ON(const struct led *led)
{
    for (int brightness = 0; brightness <= LED_STATUS_ON; brightness++)
    {
        led_set_brightness(led->dev, led->id, brightness);
        k_msleep(LED_FADE_DELAY);
    }
    return;
}

static void led_fade_OFF(const struct led *led)
{
    for (int brightness = LED_STATUS_ON; brightness >= LED_STATUS_OFF; brightness--)
    {
        led_set_brightness(led->dev, led->id, brightness);
        k_msleep(LED_FADE_DELAY);
    }
    return;
}

static void led_all_OFF() {
    for (int i = 0; i < BACKLIGHT_NUM_LEDS; i++) {
        const struct led *led = &pwm_leds[i];
        led_off(led->dev, led->id);
    }
    return;
}

void led_fade_blink(const struct led *led, uint32_t sleep_ms, const int count)
{
    for (int i = 0; i < count; i++)
    {
        led_fade_ON(led);
        k_msleep(sleep_ms);
        led_fade_OFF(led);
        k_msleep(sleep_ms);
    }
    return;
}

void usb_animation_work_handler(struct k_work *work)
{
    #ifdef disable_led_sleep_pc
    if (usb_conn_state == ZMK_USB_CONN_POWERED)
    {
        led_all_OFF();
        return;
    }
    #endif
    for (int i = 0; i < 4; i++)
    {
        led_fade_ON(&pwm_leds[i]);
        k_msleep(LED_USB_BLINK_DELAY / 2);
    }

    k_msleep(LED_USB_BLINK_DELAY);

    for (int i = 3; i >= 0; i--)
    {
        led_fade_OFF(&pwm_leds[i]);
        k_msleep(LED_USB_BLINK_DELAY / 2);
    }
}
// Define work for USB animation
K_WORK_DELAYABLE_DEFINE(usb_animation_work, usb_animation_work_handler);

struct k_work_delayable check_ble_conn_work; // This is a workaround for the compiler, don't touch it.

void check_ble_conn_handler(struct k_work *work)
{
    if (!check_conn_working)
    {
        return;
    } 
    else
    {
        if (zmk_ble_active_profile_is_connected() || usb_conn_state != ZMK_USB_CONN_NONE )
        {
            check_conn_working = false;
            return;
        }
        else
        {
            led_fade_blink(&pwm_leds[3], LED_BLINK_CONN_DELAY, 1);
            k_work_schedule(&check_ble_conn_work, K_SECONDS(4)); // Restart work for next status check
            k_msleep(LED_BLINK_PROFILE_DELAY);
            return;
        }
    }
}
K_WORK_DELAYABLE_DEFINE(check_ble_conn_work, check_ble_conn_handler);

void bat_animation_work_handler(struct k_work *work)
{
    uint8_t level = zmk_battery_state_of_charge();
    if (level != 0)
    {
        if (level <= 15)
        {
            led_fade_blink(&pwm_leds[0], LED_BATTERY_BLINK_DELAY, 3);
        }
        else
        {
            led_fade_ON(&pwm_leds[0]);
        }
        if (level > 30 && level <= 50)
        {
            led_fade_blink(&pwm_leds[1], LED_BATTERY_BLINK_DELAY, 3);
        }
        else if (level > 50)
        {
            led_fade_ON(&pwm_leds[1]);
        }
        if (level > 70 && level <= 90)
        {
            led_fade_blink(&pwm_leds[2], LED_BATTERY_BLINK_DELAY, 3);
        }
        else if (level > 90)
        {
            led_fade_ON(&pwm_leds[2]);
        }
        if (level == 100)
        {
            led_fade_OFF(&pwm_leds[0]);
            led_fade_OFF(&pwm_leds[1]);
            led_fade_OFF(&pwm_leds[2]);
            k_msleep(LED_BATTERY_BLINK_DELAY);
            led_fade_ON(&pwm_leds[0]);
            led_fade_ON(&pwm_leds[1]);
            led_fade_ON(&pwm_leds[2]);
            k_msleep(LED_BATTERY_BLINK_DELAY);
        }
        k_msleep(LED_BATTERY_SHOW_DELAY);
        led_all_OFF();
        k_msleep(LED_BATTERY_BLINK_DELAY*2);
        k_work_schedule(&check_ble_conn_work, K_SECONDS(4));
        return;     
    }
}
K_WORK_DELAYABLE_DEFINE(bat_animation_work, bat_animation_work_handler);

static int led_init(const struct device *dev)
{
    led_all_OFF();
    k_work_schedule(&bat_animation_work, K_SECONDS(1));
    return 0;
}

SYS_INIT(led_init, APPLICATION, 32);

int ble_profile_listener(const zmk_event_t *eh)
{
    const struct zmk_ble_active_profile_changed *profile_ev = NULL;
    if ((profile_ev = as_zmk_ble_active_profile_changed(eh)) == NULL)
    {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (profile_ev->index <= 2)
    {
        led_fade_blink(&pwm_leds[profile_ev->index], LED_BLINK_PROFILE_DELAY, 1);
    }

    if (!check_conn_working)
    {
        check_conn_working = true;
        k_work_schedule(&check_ble_conn_work, K_SECONDS(5));
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(ble_profile_listener, ble_profile_listener);
ZMK_SUBSCRIPTION(ble_profile_listener, zmk_ble_active_profile_changed);

int usb_listener(const zmk_event_t *eh)
{
    const struct zmk_usb_conn_state_changed *usb_ev = NULL;
    if ((usb_ev = as_zmk_usb_conn_state_changed(eh)) == NULL)
    {
        return ZMK_EV_EVENT_BUBBLE;
    }

    usb_conn_state = usb_ev->conn_state;

    if (usb_ev->conn_state != ZMK_USB_CONN_NONE)
    {
        k_work_schedule(&usb_animation_work, K_NO_WAIT);
    }
    else
    {
        check_conn_working = true;
        k_work_schedule(&check_ble_conn_work, K_SECONDS(5));
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(usb_listener, usb_listener);
ZMK_SUBSCRIPTION(usb_listener, zmk_usb_conn_state_changed);

void show_battery()
{
    k_work_schedule(&bat_animation_work, K_NO_WAIT);
}

void hide_battery()
{
    led_all_OFF();
}
