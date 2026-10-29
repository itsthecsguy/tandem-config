/*
 * Tandem reconnects.
 *
 * ZMK advertises only while the active profile's host is away. A host paired
 * on another profile that loses its connection, say a laptop carried out of
 * range, can't reconnect while the active host stays connected. At boot, the
 * first host to connect can lock the other out the same way. ZMK also starts
 * advertising only when something changes, and if the radio refuses then,
 * ZMK gives up.
 *
 * So every few seconds, and soon after a host disconnects or the profile
 * changes, this checks that the keyboard is advertising while any paired
 * host is away. When ZMK should be advertising and isn't, ZMK restarts it.
 * When only a host on another profile is away, this advertises for it: as
 * fast as ZMK does at first, then about once a second to save the battery.
 * That host gets no keys until its profile is selected, and selecting it
 * makes ZMK advertise fast again, so it only has to reconnect by then.
 */

#include <errno.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>

LOG_MODULE_REGISTER(tandem_reconnect, CONFIG_ZMK_LOG_LEVEL);

#define ADV_OPTIONS                                                                                \
    (BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_ONE_TIME | BT_LE_ADV_OPT_USE_NAME |                 \
     BT_LE_ADV_OPT_FORCE_NAME_IN_AD)

/* ZMK's own advertising, from app/src/ble.c, every 100 to 150 ms. */
static const struct bt_le_adv_param fast_param =
    BT_LE_ADV_PARAM_INIT(ADV_OPTIONS, BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);

/* The same every 1 to 1.2 s. */
static const struct bt_le_adv_param slow_param =
    BT_LE_ADV_PARAM_INIT(ADV_OPTIONS, BT_GAP_ADV_SLOW_INT_MIN, BT_GAP_ADV_SLOW_INT_MAX, NULL);

static const struct bt_data adv_data[] = {
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, 0xC1, 0x03),
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_SOME, 0x12, 0x18, /* HID Service */
                  0x0f, 0x18                       /* Battery Service */
                  ),
};

/* Whether advertising that's running was started here rather than by ZMK, and at which pace. */
static bool started_here;
static bool started_slow;

/* When a host on another profile was first found away, or 0 while none is. */
static int64_t away_since;

/*
 * Returns 0 if advertising is off, and leaves it off. Returns -EALREADY if
 * it's on, or another error if Zephyr refused before looking. Without
 * extended advertising (see Kconfig), a start that fails leaves a running
 * advertiser alone.
 */
static int probe(void) {
    int err = bt_le_adv_start(&fast_param, adv_data, ARRAY_SIZE(adv_data), NULL, 0);

    if (!err) {
        bt_le_adv_stop();
    }
    return err;
}

static bool is_connected(const bt_addr_le_t *addr) {
    struct bt_conn *conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr);
    struct bt_conn_info info;
    bool connected = false;

    if (conn) {
        connected = bt_conn_get_info(conn, &info) == 0 && info.state == BT_CONN_STATE_CONNECTED;
        bt_conn_unref(conn);
    }
    return connected;
}

static void find_away(const struct bt_bond_info *bond, void *away) {
    /* The right half is paired too, but isn't on a profile. */
    if (zmk_ble_profile_index(&bond->addr) >= 0 && !is_connected(&bond->addr)) {
        *(bool *)away = true;
    }
}

static void stop_here(void) {
    if (started_here) {
        bt_le_adv_stop();
        started_here = false;
    }
}

static void check(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(check_work, check);

static void check(struct k_work *work) {
    k_work_schedule(&check_work, K_SECONDS(CONFIG_TANDEM_RECONNECT_INTERVAL));

    /* The same rule ZMK uses for when to advertise. */
    if (zmk_ble_active_profile_is_open() || !zmk_ble_active_profile_is_connected()) {
        away_since = 0;
        if (started_here) {
            /*
             * Advertising from here was running, so ZMK's own start failed.
             * Stop it, and let ZMK advertise at its own pace.
             */
            stop_here();
        } else {
            int err = probe();
            if (err) {
                /*
                 * -EALREADY means advertising is on. Some other errors come
                 * before Zephyr looks, so it may be on then too.
                 */
                if (err != -EALREADY) {
                    LOG_WRN("Couldn't check advertising (err %d)", err);
                }
                return;
            }
            LOG_WRN("Advertising had stopped, restarting it");
        }
        zmk_ble_set_device_name((char *)bt_get_name());
        return;
    }

    bool away = false;
    bt_foreach_bond(BT_ID_DEFAULT, find_away, &away);
    if (!away) {
        away_since = 0;
        stop_here();
        return;
    }

    int64_t now = k_uptime_get();
    if (!away_since) {
        away_since = now;
    }
    bool slow = now - away_since >= CONFIG_TANDEM_RECONNECT_FAST_SECONDS * MSEC_PER_SEC;
    if (started_here && started_slow != slow) {
        stop_here();
    }

    int err = probe();
    if (err) {
        if (err != -EALREADY) {
            LOG_WRN("Couldn't check advertising (err %d)", err);
        }
        return;
    }

    err = bt_le_adv_start(slow ? &slow_param : &fast_param, adv_data, ARRAY_SIZE(adv_data), NULL,
                          0);
    started_here = !err;
    started_slow = slow;
    LOG_INF("Advertising %s for a host on another profile (err %d)", slow ? "slowly" : "fast",
            err);
}

static void connected(struct bt_conn *conn, uint8_t err) {
    struct bt_conn_info info;

    /* One-time advertising stops when a host connects. */
    if (!err && bt_conn_get_info(conn, &info) == 0 && info.role == BT_CONN_ROLE_PERIPHERAL) {
        started_here = false;
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    struct bt_conn_info info;

    /* Check soon after a host disconnects, once ZMK's own restart has run. */
    if (bt_conn_get_info(conn, &info) == 0 && info.role == BT_CONN_ROLE_PERIPHERAL) {
        k_work_reschedule(&check_work, K_MSEC(100));
    }
}

BT_CONN_CB_DEFINE(tandem_reconnect_conn_cb) = {
    .connected = connected,
    .disconnected = disconnected,
};

/* Selecting another profile may need advertising, or ZMK's own instead of this. */
static int profile_changed(const zmk_event_t *eh) {
    k_work_reschedule(&check_work, K_MSEC(100));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(tandem_reconnect, profile_changed);
ZMK_SUBSCRIPTION(tandem_reconnect, zmk_ble_active_profile_changed);

static int tandem_reconnect_init(void) {
    k_work_schedule(&check_work, K_SECONDS(CONFIG_TANDEM_RECONNECT_INTERVAL));
    return 0;
}

SYS_INIT(tandem_reconnect_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
