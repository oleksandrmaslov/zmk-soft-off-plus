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
int zmk_soft_off_plus_signal_peers_drop(void) { return 0; }

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
    if (!zmk_soft_off_plus_claim_off()) {
        return; /* this half is already powering off (e.g. its own keymap run) */
    }
    LOG_INF("soft-off-plus: peer requested simultaneous off");
    zmk_pm_soft_off();
}
static K_WORK_DEFINE(sop_soft_off_work, sop_soft_off_work_cb);

/* DROP requested by a peer (trigger-on-hold phase 1 on the other half).
 *
 * If this half is holding its own soft-off-plus key (matrix relay, or the
 * sideband half the key is wired to), only blank -- it powers off on its own
 * release, because the held key is also its wake source. If nothing is held
 * here (a passive receiver, e.g. the non-wired half of a sideband press), the
 * user has already committed to power-off by holding past hold-time and there is
 * no held wake source to re-wake us, so just power off now -- no need to wait for
 * a separate release signal to be relayed across the link. */
static void sop_drop_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    if (zmk_soft_off_plus_hold_active()) {
        LOG_INF("soft-off-plus: peer DROP; own key held, blank only");
        zmk_soft_off_plus_drop_components();
        return;
    }
    if (zmk_soft_off_plus_claim_off()) {
        LOG_INF("soft-off-plus: peer DROP; nothing held here, powering off");
        zmk_pm_soft_off();
    }
}
static K_WORK_DEFINE(sop_drop_work, sop_drop_work_cb);

/* Dispatch a received command byte. Runs from a BLE RX callback, so it only
 * submits work -- the actual suspend/power-off happens off the callback. */
static inline void sop_handle_cmd(uint8_t cmd) {
    switch (cmd) {
    case ZMK_SOFT_OFF_PLUS_CMD_OFF:
        k_work_submit(&sop_soft_off_work);
        break;
    case ZMK_SOFT_OFF_PLUS_CMD_DROP:
        k_work_submit(&sop_drop_work);
        break;
    default:
        break;
    }
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* ---------------------------------------------------------------------------
 * Central: GATT client. Discovers the off characteristic on each peripheral,
 * subscribes for notifications, and writes the off command on demand.
 *
 * Discovery is driven from a retrying work item rather than directly from the
 * connected callback: ZMK's own split client kicks off a GATT discovery the
 * instant the link comes up, and only one GATT procedure can be active per
 * connection, so a discovery started from our connected callback races ZMK's
 * and fails with -EBUSY. The work item waits until the link is encrypted and
 * the ATT bearer is free, then retries until it has our handle.
 * ------------------------------------------------------------------------- */

#include <zmk/ble.h>

struct sop_peripheral_slot {
    struct bt_conn *conn;
    uint16_t off_handle;
    bool discovering;
    bool subscribing;
    bool subscribed;
    struct bt_gatt_discover_params discover_params;
    struct bt_gatt_discover_params sub_discover_params;
    struct bt_gatt_subscribe_params subscribe_params;
};

static struct sop_peripheral_slot peripherals[ZMK_SPLIT_BLE_PERIPHERAL_COUNT];

static const struct bt_uuid_128 sop_service_uuid = BT_UUID_INIT_128(ZMK_SOFT_OFF_PLUS_SVC_UUID);

static void sop_discover_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sop_discover_work, sop_discover_work_cb);

/* Long enough for ZMK's own connect-time discovery to finish and free the ATT
 * bearer before we (re)try ours. */
#define SOP_DISCOVER_RETRY_MS 500

static struct sop_peripheral_slot *sop_slot_for_conn(struct bt_conn *conn) {
    if (!conn) {
        return NULL;
    }

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
            peripherals[i].discovering = false;
            peripherals[i].subscribing = false;
            peripherals[i].subscribed = false;
            return &peripherals[i];
        }
    }
    return NULL;
}

static uint8_t sop_notify_cb(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                             const void *data, uint16_t length) {
    if (!data) {
        struct sop_peripheral_slot *slot = sop_slot_for_conn(conn);
        if (slot) {
            slot->subscribing = false;
            slot->subscribed = false;
            /* A NULL notification means that the subscription was removed or
             * its CCC could not be discovered. Keep the value handle for the
             * central->peripheral write path, but rediscover the CCC and retry
             * the peripheral->central notification path. */
            params->ccc_handle = 0U;
            params->value_handle = slot->off_handle;
            params->value = BT_GATT_CCC_NOTIFY;
            k_work_reschedule(&sop_discover_work, K_MSEC(SOP_DISCOVER_RETRY_MS));
        }
        return BT_GATT_ITER_STOP;
    }
    if (length >= 1) {
        sop_handle_cmd(((const uint8_t *)data)[0]);
    }
    return BT_GATT_ITER_CONTINUE;
}

static void sop_subscribe_cb(struct bt_conn *conn, uint8_t err,
                             struct bt_gatt_subscribe_params *params) {
    ARG_UNUSED(params);
    struct sop_peripheral_slot *slot = sop_slot_for_conn(conn);
    if (!slot) {
        return;
    }

    slot->subscribing = false;
    slot->subscribed = (err == 0U);
    if (err) {
        LOG_WRN("soft-off-plus: subscribe response failed (ATT 0x%02x); retrying", err);
        k_work_reschedule(&sop_discover_work, K_MSEC(SOP_DISCOVER_RETRY_MS));
    } else {
        LOG_DBG("soft-off-plus: peripheral notification subscription ready");
    }
}

static void sop_begin_subscription(struct sop_peripheral_slot *slot) {
    slot->subscribe_params.disc_params = &slot->sub_discover_params;
    slot->subscribe_params.end_handle = slot->discover_params.end_handle;
    slot->subscribe_params.value_handle = slot->off_handle;
    slot->subscribe_params.notify = sop_notify_cb;
    slot->subscribe_params.subscribe = sop_subscribe_cb;
    slot->subscribe_params.value = BT_GATT_CCC_NOTIFY;

    /* Re-establish the CCC on every split reconnection. This prevents stale
     * client subscription state from making a peripheral notification appear
     * ready when the server no longer has notifications enabled. */
    atomic_set_bit(slot->subscribe_params.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);
    atomic_clear_bit(slot->subscribe_params.flags, BT_GATT_SUBSCRIBE_FLAG_NO_RESUB);

    slot->subscribing = true;
    int err = bt_gatt_subscribe(slot->conn, &slot->subscribe_params);
    if (err == -EALREADY) {
        /* The same live subscription is already registered. */
        slot->subscribing = false;
        slot->subscribed = true;
    } else if (err) {
        slot->subscribing = false;
        slot->subscribed = false;
        LOG_WRN("soft-off-plus: subscribe failed (%d); retrying", err);
    }
}

static uint8_t sop_chrc_discovery_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                     struct bt_gatt_discover_params *params) {
    ARG_UNUSED(params);
    struct sop_peripheral_slot *slot = sop_slot_for_conn(conn);
    if (!slot) {
        return BT_GATT_ITER_STOP;
    }
    if (!attr || !attr->user_data) {
        /* Walked the whole service without finding our characteristic; let the
         * work item decide whether to retry. */
        slot->discovering = false;
        return BT_GATT_ITER_STOP;
    }

    const struct bt_uuid *chrc_uuid = ((struct bt_gatt_chrc *)attr->user_data)->uuid;
    if (bt_uuid_cmp(chrc_uuid, BT_UUID_DECLARE_128(ZMK_SOFT_OFF_PLUS_CHRC_UUID)) != 0) {
        /* Not ours; keep walking the rest of the service's characteristics. */
        return BT_GATT_ITER_CONTINUE;
    }

    slot->off_handle = bt_gatt_attr_value_handle(attr);
    slot->discovering = false;
    slot->subscribed = false;

    LOG_DBG("soft-off-plus: discovered peripheral off characteristic (handle %u)",
            slot->off_handle);
    sop_begin_subscription(slot);
    return BT_GATT_ITER_STOP;
}

static uint8_t sop_service_discovery_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                        struct bt_gatt_discover_params *params) {
    ARG_UNUSED(params);
    struct sop_peripheral_slot *slot = sop_slot_for_conn(conn);
    if (!slot) {
        return BT_GATT_ITER_STOP;
    }
    if (!attr || !attr->user_data) {
        slot->discovering = false;
        return BT_GATT_ITER_STOP;
    }

    const struct bt_gatt_service_val *svc = attr->user_data;

    /* Constrain the characteristic walk to this service's handle range. A bare
     * 0x0001..0xffff walk would hit the GAP/GATT characteristics first and stop
     * there, so our characteristic would never be found. */
    slot->discover_params.uuid = NULL;
    slot->discover_params.func = sop_chrc_discovery_cb;
    slot->discover_params.start_handle = attr->handle + 1;
    slot->discover_params.end_handle = svc->end_handle;
    slot->discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

    int err = bt_gatt_discover(conn, &slot->discover_params);
    if (err) {
        LOG_WRN("soft-off-plus: characteristic discovery failed (%d)", err);
        slot->discovering = false; /* allow the work item to retry */
    }
    return BT_GATT_ITER_STOP;
}

static void sop_begin_discovery(struct sop_peripheral_slot *slot) {
    slot->discover_params.uuid = &sop_service_uuid.uuid;
    slot->discover_params.func = sop_service_discovery_cb;
    slot->discover_params.start_handle = 0x0001;
    slot->discover_params.end_handle = 0xffff;
    slot->discover_params.type = BT_GATT_DISCOVER_PRIMARY;

    int err = bt_gatt_discover(slot->conn, &slot->discover_params);
    if (err) {
        /* Most likely -EBUSY while ZMK's own discovery runs; the work item
         * reschedules itself and we try again. */
        LOG_DBG("soft-off-plus: service discovery busy (%d), will retry", err);
        return;
    }
    slot->discovering = true;
}

static void sop_discover_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    bool pending = false;

    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        struct sop_peripheral_slot *slot = &peripherals[i];
        if (slot->conn == NULL || slot->subscribed) {
            continue;
        }
        pending = true;
        if (bt_conn_get_security(slot->conn) < BT_SECURITY_L2) {
            continue; /* wait until the inter-half link is encrypted */
        }
        if (slot->off_handle == 0) {
            if (!slot->discovering) {
                sop_begin_discovery(slot);
            }
            continue;
        }
        if (!slot->subscribed && !slot->subscribing) {
            sop_begin_subscription(slot);
        }
    }

    if (pending) {
        k_work_reschedule(&sop_discover_work, K_MSEC(SOP_DISCOVER_RETRY_MS));
    }
}

static void sop_connected(struct bt_conn *conn, uint8_t conn_err) {
    struct bt_conn_info info;
    if (conn_err || bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_CENTRAL) {
        /* Only the inter-half link, on which we are the BLE central, matters. */
        return;
    }
    if (sop_slot_for_conn(conn) != NULL) {
        return; /* already tracked */
    }

    struct sop_peripheral_slot *slot = sop_reserve_slot(conn);
    if (!slot) {
        LOG_WRN("soft-off-plus: no free peripheral slot");
        return;
    }

    /* Defer discovery so it does not race ZMK's connect-time discovery. */
    k_work_reschedule(&sop_discover_work, K_MSEC(SOP_DISCOVER_RETRY_MS));
}

static void sop_security_changed(struct bt_conn *conn, bt_security_t level,
                                 enum bt_security_err err) {
    ARG_UNUSED(level);
    ARG_UNUSED(err);
    /* Once the link is encrypted, discovery can proceed; nudge the work item. */
    if (sop_slot_for_conn(conn) != NULL) {
        k_work_reschedule(&sop_discover_work, K_MSEC(50));
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
    slot->discovering = false;
    slot->subscribing = false;
    slot->subscribed = false;
    slot->subscribe_params.value_handle = 0;
}

static struct bt_conn_cb sop_conn_callbacks = {
    .connected = sop_connected,
    .disconnected = sop_disconnected,
    .security_changed = sop_security_changed,
};

static int sop_central_init(void) {
    bt_conn_cb_register(&sop_conn_callbacks);
    return 0;
}
SYS_INIT(sop_central_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);

static int sop_central_send(uint8_t cmd) {
    int sent = 0;

    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        struct sop_peripheral_slot *slot = &peripherals[i];
        if (slot->conn == NULL || slot->off_handle == 0) {
            continue;
        }
        int err = bt_gatt_write_without_response(slot->conn, slot->off_handle, &cmd, sizeof(cmd),
                                                 false);
        if (err) {
            LOG_WRN("soft-off-plus: cmd 0x%02x write to peripheral %d failed (%d)", cmd, i, err);
        } else {
            sent++;
        }
    }

    return sent > 0 ? 0 : -ENOTCONN;
}

int zmk_soft_off_plus_signal_peers(void) { return sop_central_send(ZMK_SOFT_OFF_PLUS_CMD_OFF); }

int zmk_soft_off_plus_signal_peers_drop(void) {
    return sop_central_send(ZMK_SOFT_OFF_PLUS_CMD_DROP);
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
    if (len >= 1) {
        sop_handle_cmd(((const uint8_t *)buf)[0]);
    }
    return len;
}

BT_GATT_SERVICE_DEFINE(
    sop_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(ZMK_SOFT_OFF_PLUS_SVC_UUID)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZMK_SOFT_OFF_PLUS_CHRC_UUID),
                           BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_WRITE_ENCRYPT, NULL, sop_write_cb, &sop_cmd_value),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT), );

static int sop_peripheral_send(uint8_t cmd) {
    /* attrs[1] is the characteristic declaration; bt_gatt_notify resolves the
     * value attribute from it. */
    int err = bt_gatt_notify(NULL, &sop_svc.attrs[1], &cmd, sizeof(cmd));
    if (err) {
        LOG_WRN("soft-off-plus: notify central (cmd 0x%02x) failed (%d)", cmd, err);
    }
    return err;
}

int zmk_soft_off_plus_signal_peers(void) { return sop_peripheral_send(ZMK_SOFT_OFF_PLUS_CMD_OFF); }

int zmk_soft_off_plus_signal_peers_drop(void) {
    return sop_peripheral_send(ZMK_SOFT_OFF_PLUS_CMD_DROP);
}

#endif /* role */

#endif /* CONFIG_ZMK_SOFT_OFF_PLUS_SPLIT_SYNC */
