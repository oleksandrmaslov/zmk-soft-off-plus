/*
 * SPDX-License-Identifier: MIT
 *
 * Self-contained cross-half soft-off signalling.
 *
 * A dedicated GATT service carries a one-byte "power off now" command between
 * the split halves so that triggering soft-off-plus on either half powers off
 * both. The central writes the command to each peripheral; a peripheral
 * notifies the central. The receiving side defers the actual power-off to a work
 * item so it never runs inside a Bluetooth callback.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include <zmk/soft_off_plus/split_sync.h>

LOG_MODULE_DECLARE(zmk_soft_off_plus, CONFIG_ZMK_SOFT_OFF_PLUS_LOG_LEVEL);

#if !IS_ENABLED(CONFIG_ZMK_SOFT_OFF_PLUS_SPLIT_SYNC)

int zmk_soft_off_plus_signal_peers(void) { return 0; }

#else /* CONFIG_ZMK_SOFT_OFF_PLUS_SPLIT_SYNC */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/atomic.h>

#include <zmk/pm.h>

#include <zmk/soft_off_plus/uuid.h>

/* Defer the power-off so it never runs in a BLE RX callback context. */
static void sop_soft_off_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    LOG_INF("soft-off-plus: peer requested simultaneous off");
    zmk_pm_soft_off();
}
static K_WORK_DEFINE(sop_soft_off_work, sop_soft_off_work_cb);

static inline void sop_request_local_soft_off(void) { k_work_submit(&sop_soft_off_work); }

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* ---------------------------------------------------------------------------
 * Central: GATT client. Discovers the off characteristic on each peripheral,
 * subscribes for notifications, and writes the off command on demand.
 * ------------------------------------------------------------------------- */

#include <zmk/ble.h>

struct sop_peripheral_slot {
    struct bt_conn *conn;
    uint16_t off_handle;
    struct bt_gatt_discover_params discover_params;
    struct bt_gatt_discover_params sub_discover_params;
    struct bt_gatt_subscribe_params subscribe_params;
};

static struct sop_peripheral_slot peripherals[ZMK_SPLIT_BLE_PERIPHERAL_COUNT];

static const struct bt_uuid_128 sop_service_uuid = BT_UUID_INIT_128(ZMK_SOFT_OFF_PLUS_SVC_UUID);

static struct sop_peripheral_slot *sop_slot_for_conn(struct bt_conn *conn) {
    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        if (peripherals[i].conn == conn) {
            return &peripherals[i];
        }
    }
    return NULL;
}

static struct sop_peripheral_slot *sop_reserve_slot(struct bt_conn *conn) {
    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        if (peripherals[i].conn == NULL) {
            peripherals[i].conn = conn;
            peripherals[i].off_handle = 0;
            return &peripherals[i];
        }
    }
    return NULL;
}

static uint8_t sop_notify_cb(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                             const void *data, uint16_t length) {
    ARG_UNUSED(conn);
    if (!data) {
        params->value_handle = 0U;
        return BT_GATT_ITER_STOP;
    }
    if (length >= 1 && ((const uint8_t *)data)[0] == ZMK_SOFT_OFF_PLUS_CMD_OFF) {
        sop_request_local_soft_off();
    }
    return BT_GATT_ITER_CONTINUE;
}

static uint8_t sop_chrc_discovery_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                     struct bt_gatt_discover_params *params) {
    ARG_UNUSED(params);
    if (!attr || !attr->user_data) {
        return BT_GATT_ITER_STOP;
    }
    struct sop_peripheral_slot *slot = sop_slot_for_conn(conn);
    if (!slot) {
        return BT_GATT_ITER_STOP;
    }

    const struct bt_uuid *chrc_uuid = ((struct bt_gatt_chrc *)attr->user_data)->uuid;
    if (bt_uuid_cmp(chrc_uuid, BT_UUID_DECLARE_128(ZMK_SOFT_OFF_PLUS_CHRC_UUID)) == 0) {
        slot->off_handle = bt_gatt_attr_value_handle(attr);
        slot->subscribe_params.disc_params = &slot->sub_discover_params;
        slot->subscribe_params.end_handle = slot->discover_params.end_handle;
        slot->subscribe_params.value_handle = slot->off_handle;
        slot->subscribe_params.notify = sop_notify_cb;
        slot->subscribe_params.value = BT_GATT_CCC_NOTIFY;
        atomic_set(slot->subscribe_params.flags, BT_GATT_SUBSCRIBE_FLAG_NO_RESUB);

        int err = bt_gatt_subscribe(conn, &slot->subscribe_params);
        if (err && err != -EALREADY) {
            LOG_ERR("soft-off-plus: subscribe failed (%d)", err);
        } else {
            LOG_DBG("soft-off-plus: subscribed to peripheral off characteristic");
        }
    }
    return BT_GATT_ITER_STOP;
}

static uint8_t sop_service_discovery_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                        struct bt_gatt_discover_params *params) {
    ARG_UNUSED(params);
    if (!attr) {
        return BT_GATT_ITER_STOP;
    }
    struct sop_peripheral_slot *slot = sop_slot_for_conn(conn);
    if (!slot) {
        return BT_GATT_ITER_STOP;
    }

    slot->discover_params.uuid = NULL;
    slot->discover_params.func = sop_chrc_discovery_cb;
    slot->discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
    int err = bt_gatt_discover(conn, &slot->discover_params);
    if (err) {
        LOG_ERR("soft-off-plus: characteristic discovery failed (%d)", err);
    }
    return BT_GATT_ITER_STOP;
}

static void sop_connected(struct bt_conn *conn, uint8_t conn_err) {
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_CENTRAL) {
        /* Only the inter-half link, on which we are the BLE central, matters. */
        return;
    }
    if (conn_err) {
        return;
    }

    struct sop_peripheral_slot *slot = sop_reserve_slot(conn);
    if (!slot) {
        LOG_WRN("soft-off-plus: no free peripheral slot");
        return;
    }

    slot->discover_params.uuid = &sop_service_uuid.uuid;
    slot->discover_params.func = sop_service_discovery_cb;
    slot->discover_params.start_handle = 0x0001;
    slot->discover_params.end_handle = 0xffff;
    slot->discover_params.type = BT_GATT_DISCOVER_PRIMARY;
    int err = bt_gatt_discover(conn, &slot->discover_params);
    if (err) {
        LOG_ERR("soft-off-plus: service discovery failed (%d)", err);
    }
}

static void sop_disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);
    struct sop_peripheral_slot *slot = sop_slot_for_conn(conn);
    if (!slot) {
        return;
    }
    slot->conn = NULL;
    slot->off_handle = 0;
    slot->subscribe_params.value_handle = 0;
}

static struct bt_conn_cb sop_conn_callbacks = {
    .connected = sop_connected,
    .disconnected = sop_disconnected,
};

static int sop_central_init(void) {
    bt_conn_cb_register(&sop_conn_callbacks);
    return 0;
}
SYS_INIT(sop_central_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);

int zmk_soft_off_plus_signal_peers(void) {
    uint8_t cmd = ZMK_SOFT_OFF_PLUS_CMD_OFF;
    int sent = 0;

    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        struct sop_peripheral_slot *slot = &peripherals[i];
        if (slot->conn == NULL || slot->off_handle == 0) {
            continue;
        }
        int err = bt_gatt_write_without_response(slot->conn, slot->off_handle, &cmd, sizeof(cmd),
                                                 false);
        if (err) {
            LOG_WRN("soft-off-plus: off write to peripheral %d failed (%d)", i, err);
        } else {
            sent++;
        }
    }

    return sent > 0 ? 0 : -ENOTCONN;
}

#else /* split peripheral */

/* ---------------------------------------------------------------------------
 * Peripheral: GATT server. The central writes the off command; we notify the
 * central when our own soft-off-plus is triggered.
 * ------------------------------------------------------------------------- */

static uint8_t sop_cmd_value;

static ssize_t sop_write_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                            uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);
    if (len >= 1 && ((const uint8_t *)buf)[0] == ZMK_SOFT_OFF_PLUS_CMD_OFF) {
        sop_request_local_soft_off();
    }
    return len;
}

BT_GATT_SERVICE_DEFINE(
    sop_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(ZMK_SOFT_OFF_PLUS_SVC_UUID)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZMK_SOFT_OFF_PLUS_CHRC_UUID),
                           BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_WRITE_ENCRYPT, NULL, sop_write_cb, &sop_cmd_value),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT), );

int zmk_soft_off_plus_signal_peers(void) {
    uint8_t cmd = ZMK_SOFT_OFF_PLUS_CMD_OFF;
    /* attrs[1] is the characteristic declaration; bt_gatt_notify resolves the
     * value attribute from it. */
    int err = bt_gatt_notify(NULL, &sop_svc.attrs[1], &cmd, sizeof(cmd));
    if (err) {
        LOG_WRN("soft-off-plus: notify central failed (%d)", err);
    }
    return err;
}

#endif /* role */

#endif /* CONFIG_ZMK_SOFT_OFF_PLUS_SPLIT_SYNC */
