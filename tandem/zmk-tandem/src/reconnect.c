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
 * So every few seconds, and soon after a host disconnects, this checks that
 * the keyboard is advertising while any paired host is away. When ZMK should
 * be advertising and isn't, ZMK restarts it. When only a host on another
 * profile is away, this advertises for it, the same way ZMK does.
 */

#include <errno.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/ble.h>

LOG_MODULE_REGISTER(tandem_reconnect, CONFIG_ZMK_LOG_LEVEL);

/* ZMK's own advertising, from app/src/ble.c. */
static const struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
    BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_ONE_TIME | BT_LE_ADV_OPT_USE_NAME |
        BT_LE_ADV_OPT_FORCE_NAME_IN_AD,
    BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);

static const struct bt_data adv_data[] = {
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, 0xC1, 0x03),
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_SOME, 0x12, 0x18, /* HID Service */
                  0x0f, 0x18                       /* Battery Service */
                  ),
};

/* Whether advertising that's running was started here rather than by ZMK. */
static bool started_here;

/*
 * Returns 0 if advertising is off, and leaves it off. Returns -EALREADY if
 * it's on, or another error if Zephyr refused before looking. Without
 * extended advertising (see Kconfig), a start that fails leaves a running
 * advertiser alone.
 */
static int probe(void) {
    int err = bt_le_adv_start(&adv_param, adv_data, ARRAY_SIZE(adv_data), NULL, 0);

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

static void check(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(check_work, check);

static void check(struct k_work *work) {
    k_work_schedule(&check_work, K_SECONDS(CONFIG_TANDEM_RECONNECT_INTERVAL));

    /* The same rule ZMK uses for when to advertise. */
    bool zmk_wants = zmk_ble_active_profile_is_open() || !zmk_ble_active_profile_is_connected();
    bool away = false;

    if (!zmk_wants) {
        bt_foreach_bond(BT_ID_DEFAULT, find_away, &away);
    }
    if (!zmk_wants && !away) {
        if (started_here) {
            bt_le_adv_stop();
            started_here = false;
        }
        return;
    }

    int err = probe();
    if (err) {
        /*
         * -EALREADY means advertising is on. Some other errors come before
         * Zephyr looks, so advertising may be on then too. Leave it alone.
         */
        if (err != -EALREADY) {
            LOG_WRN("Couldn't check advertising (err %d)", err);
        }
        return;
    }

    if (zmk_wants) {
        /* Hand it back to ZMK, which restarts it its way. */
        started_here = false;
        LOG_WRN("Advertising had stopped, restarting it");
        zmk_ble_set_device_name((char *)bt_get_name());
        return;
    }

    err = bt_le_adv_start(&adv_param, adv_data, ARRAY_SIZE(adv_data), NULL, 0);
    started_here = !err;
    LOG_INF("Advertising for a host on another profile (err %d)", err);
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
        k_work_reschedule(&check_work, K_SECONDS(1));
    }
}

BT_CONN_CB_DEFINE(tandem_reconnect_conn_cb) = {
    .connected = connected,
    .disconnected = disconnected,
};

static int tandem_reconnect_init(void) {
    k_work_schedule(&check_work, K_SECONDS(CONFIG_TANDEM_RECONNECT_INTERVAL));
    return 0;
}

SYS_INIT(tandem_reconnect_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
