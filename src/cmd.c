#include "cmd.h"
#include <loramac-node/src/radio/radio.h>
#include <loramac-node/src/radio/sx1276/sx1276.h>
#include <loramac-node/src/mac/secure-element-nvm.h>
#include <loramac-node/src/mac/LoRaMacTest.h>
#include <LoRaWAN/Utilities/timeServer.h>
#include "lrw.h"
#include "system.h"
#include "config.h"
#include "gpio.h"
#include "log.h"
#include "rtc.h"
#include "nvm.h"
#include "halt.h"

// These are global variables exported by radio.c that store the RSSI and SNR of
// the most recent received packet.
extern int16_t radio_rssi;
extern int8_t radio_snr;


typedef enum cmd_errno {
    ERR_UNKNOWN_CMD   =  -1,  // Unknown command
    ERR_PARAM_NO      =  -2,  // Invalid number of parameters
    ERR_PARAM         =  -3,  // Invalid parameter value(s)
    ERR_FACNEW_FAILED =  -4,  // Factory reset failed
    ERR_NO_JOIN       =  -5,  // Device has not joined LoRaWAN yet
    ERR_JOINED        =  -6,  // Device has already joined LoRaWAN
    ERR_BUSY          =  -7,  // Resource unavailable: LoRa MAC is transmitting
    ERR_VERSION       =  -8,  // New firmware version must be different
    ERR_MISSING_INFO  =  -9,  // Missing firmware information
    ERR_FLASH_ERROR   = -10,  // Flash read/write error
    ERR_UPDATE_FAILED = -11,  // Firmware update failed
    ERR_PAYLOAD_LONG  = -12,  // Payload is too long
    ERR_NO_ABP        = -13,  // Only supported in ABP activation mode
    ERR_NO_OTAA       = -14,  // Only supported in OTAA activation mode
    ERR_BAND          = -15,  // RF band is not supported
    ERR_POWER         = -16,  // Power value too high
    ERR_UNSUPPORTED   = -17,  // Not supported in the current band
    ERR_DUTYCYCLE     = -18,  // Cannot transmit due to duty cycling
    ERR_NO_CHANNEL    = -19,  // Channel unavailable due to LBT or error
    ERR_TOO_MANY      = -20   // Too many link check requests
} cmd_errno_t;


static uint8_t port;
static bool request_confirmation;
static TimerEvent_t payload_timer;

bool schedule_reset = false;


#define abort(num) do {                    \
    atci_printf("+ERR=%d\r\n\r\n", (num)); \
    return;                                \
} while (0)

#define EOL() atci_print("\r\n\n");

#define OK(...) do {                 \
    atci_printf("+OK=" __VA_ARGS__); \
    EOL();                           \
} while (0)

#define OK_() atci_print("+OK\r\n\r\n")


static inline uint32_t ntohl(uint32_t v)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (v & 0xff) << 24 | (v & 0xff00) << 8 | (v & 0xff0000) >> 8 | (v & 0xff000000) >> 24;
#else
    return v;
#endif
}


#define abort_on_error(status) do {  \
    int __rc = status2error(status); \
    if (__rc < 0) abort(__rc);       \
} while (0)


static int status2error(int status)
{
    if (status <= 0) return -status;
    switch ((status)) {
        case LORAMAC_STATUS_BUSY:                  return ERR_BUSY;         break;
        case LORAMAC_STATUS_SERVICE_UNKNOWN:       return ERR_UNKNOWN_CMD;  break;
        case LORAMAC_STATUS_NO_NETWORK_JOINED:     return ERR_NO_JOIN;      break;
        case LORAMAC_STATUS_DUTYCYCLE_RESTRICTED:  return ERR_DUTYCYCLE;    break;
        case LORAMAC_STATUS_REGION_NOT_SUPPORTED:  return ERR_BAND;         break;
        case LORAMAC_STATUS_FREQUENCY_INVALID:     return ERR_UNSUPPORTED;  break;
        case LORAMAC_STATUS_DATARATE_INVALID:      return ERR_UNSUPPORTED;  break;
        case LORAMAC_STATUS_FREQ_AND_DR_INVALID:   return ERR_UNSUPPORTED;  break;
        case LORAMAC_STATUS_LENGTH_ERROR:          return ERR_PAYLOAD_LONG; break;
        case LORAMAC_STATUS_NO_CHANNEL_FOUND:      return ERR_NO_CHANNEL;   break;
        case LORAMAC_STATUS_NO_FREE_CHANNEL_FOUND: return ERR_NO_CHANNEL;   break;
        default:                                   return ERR_PARAM;        break;
    }
}


/*
 * Use this function to parse a single argument that must be either 0 or 1. Note
 * that if the AT command accepts multiple arguments separated by commas, this
 * function cannot be used.
 */
static int parse_enabled(atci_param_t *param)
{
    if (param->offset >= param->length) return -1;
    if (param->length - param->offset != 1) return -1;

    switch (param->txt[param->offset++]) {
        case '0': return 0;
        case '1': return 1;
        default : return -1;
    }
}


static int parse_port(atci_param_t *param)
{
    uint32_t v;

    if (!atci_param_get_uint(param, &v))
        return -1;

    if (v < 1 || v > 223)
        return -1;

    return v;
}


static void get_uart(void)
{
    OK("%d,%d,%d,%d,%d", sysconf.uart_baudrate, 8, 1, 0, 0);
}


static void set_uart(atci_param_t *param)
{
    uint32_t v;
    if (!atci_param_get_uint(param, &v)) abort(ERR_PARAM);

    switch(v) {
        case 4800:  break;
        case 9600:  break;
        case 19200: break;
        case 38400: break;
        default: abort(ERR_PARAM);
    }

    sysconf.uart_baudrate = v;
    sysconf_modified = true;

    OK_();
}


// Backwards compatible implementation of AT+VER
static void get_version_comp(void)
{
    OK("%s,%s", VERSION_COMPAT, BUILD_DATE_COMPAT);
}


// AT$VER with more detailed firmware version and build time
static void get_version(void)
{
    OK("%s [LoRaMac %s],%s", VERSION, LIB_VERSION, BUILD_DATE);
}


static void get_model(void)
{
    OK("ABZ");
}


static void reboot(atci_param_t *param)
{
    (void)param;
    OK_();
    schedule_reset = true;
    atci_flush();
}


static void facnew(atci_param_t *param)
{
    (void)param;

    if (LoRaMacStop() != LORAMAC_STATUS_OK)
        abort(ERR_FACNEW_FAILED);
    OK_();

    if (nvm_erase() == 0) {
        cmd_event(CMD_EVENT_MODULE, CMD_MODULE_FACNEW);
        schedule_reset = true;
        atci_flush();
    }
}


static void get_band(void)
{
    LoRaMacNvmData_t *state = lrw_get_state();
    OK("%d", state->MacGroup2.Region);
}


static void set_band(atci_param_t *param)
{
    uint32_t value;

    if (!atci_param_get_uint(param, &value))
        abort(ERR_PARAM);

    int rv = lrw_set_region(value);
    abort_on_error(rv);

    OK_();
    if (rv == 0) {
        // Emit a factory reset event since we have reset a significant portion
        // of the internal state (this is to match the original firmware which
        // does full factory reset on band change).
        cmd_event(CMD_EVENT_MODULE, CMD_MODULE_FACNEW);
        atci_flush();
        schedule_reset = true;
    }
}


static void get_class(void)
{
    OK("%d", lrw_get_class());
}


static void set_class(atci_param_t *param)
{
    uint32_t v;
    if (!atci_param_get_uint(param, &v)) abort(ERR_PARAM);

    // In original firmware compatiblity mode, only class A (0) and class C (2)
    // can be configured with this command.
    if (v != 0 && v != 2) abort(ERR_PARAM);

    abort_on_error(lrw_set_class(v));
    OK_();
}


static void get_mode(void)
{
    OK("%d", lrw_get_mode());
}


static void set_mode(atci_param_t *param)
{
    uint32_t v;
    if (!atci_param_get_uint(param, &v)) abort(ERR_PARAM);
    if (v > 1) abort(ERR_PARAM);

    abort_on_error(lrw_set_mode(v));
    OK_();
}


static void get_devaddr(void)
{
    MibRequestConfirm_t r = { .Type = MIB_DEV_ADDR };
    abort_on_error(LoRaMacMibGetRequestConfirm(&r));
    OK("%08lX", r.Param.DevAddr);
}


static void set_devaddr(atci_param_t *param)
{
    uint32_t buf;
    if (atci_param_get_buffer_from_hex(param, &buf, sizeof(buf)) != sizeof(buf))
        abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type  = MIB_DEV_ADDR,
        .Param = { .DevAddr = ntohl(buf) }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


static void get_deveui(void)
{
    MibRequestConfirm_t r = { .Type = MIB_DEV_EUI };
    abort_on_error(LoRaMacMibGetRequestConfirm(&r));
    atci_print("+OK=");
    atci_print_buffer_as_hex(r.Param.DevEui, SE_EUI_SIZE);
    EOL();
}


static void set_deveui(atci_param_t *param)
{
    uint8_t eui[SE_EUI_SIZE];
    if (atci_param_get_buffer_from_hex(param, eui, SE_EUI_SIZE) != SE_EUI_SIZE)
        abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type  = MIB_DEV_EUI,
        .Param = { .DevEui = eui }
    };

    abort_on_error(LoRaMacMibSetRequestConfirm(&r));
    OK_();
}


static void get_joineui(void)
{
    MibRequestConfirm_t r = { .Type = MIB_JOIN_EUI };
    abort_on_error(LoRaMacMibGetRequestConfirm(&r));
    atci_print("+OK=");
    atci_print_buffer_as_hex(r.Param.JoinEui, SE_EUI_SIZE);
    EOL();
}


static void set_joineui(atci_param_t *param)
{
    uint8_t eui[SE_EUI_SIZE];
    if (atci_param_get_buffer_from_hex(param, eui, SE_EUI_SIZE) != SE_EUI_SIZE)
        abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type  = MIB_JOIN_EUI,
        .Param = { .JoinEui = eui }
    };

    abort_on_error(LoRaMacMibSetRequestConfirm(&r));
    OK_();
}


static void get_nwkskey(void)
{
    LoRaMacNvmData_t *state = lrw_get_state();
    atci_print("+OK=");

    // We operate in a backwards-compatible 1.0 mode here and in that mode, the
    // various network session keys are the same and the canonical version is in
    // FNwkSIntKey.

    atci_print_buffer_as_hex(&state->SecureElement.KeyList[F_NWK_S_INT_KEY].KeyValue, SE_KEY_SIZE);
    EOL();
}


static void set_nwkskey(atci_param_t *param)
{
    uint8_t key[SE_KEY_SIZE];

    if (atci_param_get_buffer_from_hex(param, key, SE_KEY_SIZE) != SE_KEY_SIZE)
        abort(ERR_PARAM);

    // We implement a mode compatible with the original Type ABZ firmware which
    // only supports LoRaWAN 1.0. Thus, we need to operate in a LoRaWAN 1.0
    // backwards-compatible mode here. In this mode, the NwkSKey becomes
    // FNwkSIntKey (forwarding network session integrity key). The other two
    // network keys required by our 1.1 implementation are set to the same
    // value.

    // Forwarding network session integrity key. This is the network session key
    // for 1.0.x devices.
    MibRequestConfirm_t r = {
        .Type  = MIB_F_NWK_S_INT_KEY,
        .Param = { .FNwkSIntKey = key }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    // Service network session integrity key. This is not used in 1.0.x. Must be
    // the same as the forwarding key above.
    r.Type  = MIB_S_NWK_S_INT_KEY;
    r.Param.SNwkSIntKey = key;
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    // Network session encryption key. Not used by 1.0.x devices. Must be the
    // same as the forwarding key above.
    r.Type  = MIB_NWK_S_ENC_KEY;
    r.Param.NwkSEncKey = key;
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


static void get_appskey(void)
{
    LoRaMacNvmData_t *state = lrw_get_state();
    atci_print("+OK=");
    atci_print_buffer_as_hex(&state->SecureElement.KeyList[APP_S_KEY].KeyValue, SE_KEY_SIZE);
    EOL();
}


static void set_appskey(atci_param_t *param)
{
    uint8_t key[SE_KEY_SIZE];

    if (atci_param_get_buffer_from_hex(param, key, SE_KEY_SIZE) != SE_KEY_SIZE)
        abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type  = MIB_APP_S_KEY,
        .Param = { .AppSKey = key }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


static void get_appkey(void)
{
    LoRaMacNvmData_t *state = lrw_get_state();
    atci_print("+OK=");
    atci_print_buffer_as_hex(&state->SecureElement.KeyList[APP_KEY].KeyValue, SE_KEY_SIZE);
    EOL();
}


static void set_appkey_10(atci_param_t *param)
{
    uint8_t key[SE_KEY_SIZE];

    if (atci_param_get_buffer_from_hex(param, key, SE_KEY_SIZE) != SE_KEY_SIZE)
        abort(ERR_PARAM);

    // The original firmware supports LoRaWAN 1.0 and does not provide an AT
    // command to set the other root key (NwkKey). Hence, we must assume that we
    // will be operating in the backwards-compatible single root key scheme
    // documented in LoRaWAN 1.1 Section 6.1.1.3. In that scheme, AppSKey is
    // derived from NwkKey and not from AppKey. Thus, we need to set the value
    // configured here to both AppKey and NwkKey.

    MibRequestConfirm_t r = {
        .Type  = MIB_NWK_KEY,
        .Param = { .NwkKey = key }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    r.Type = MIB_APP_KEY;
    r.Param.AppKey = key;
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


static void set_appkey_11(atci_param_t *param)
{
    uint8_t key[SE_KEY_SIZE];

    if (atci_param_get_buffer_from_hex(param, key, SE_KEY_SIZE) != SE_KEY_SIZE)
        abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type  = MIB_APP_KEY,
        .Param = { .AppKey = key }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));
    OK_();
}


static void join(atci_param_t *param)
{
    (void)param;

    abort_on_error(lrw_join());
    OK_();
}


static void get_joindc(void)
{
    LoRaMacNvmData_t *state = lrw_get_state();
    OK("%d", state->MacGroup2.JoinDutyCycleOn);
}


static void set_joindc(atci_param_t *param)
{
    int enabled = parse_enabled(param);
    if (enabled == -1) abort(ERR_PARAM);

    LoRaMacTestSetJoinDutyCycleOn(enabled);
    OK_();
}


static void lncheck(atci_param_t *param)
{
    int piggyback = 0;

    if (param != NULL) {
        piggyback = parse_enabled(param);
        if (piggyback == -1) abort(ERR_PARAM);
    }

    abort_on_error(lrw_check_link(piggyback == 1));
    OK_();
}


// static void get_rfparam(void)
// {
//     abort(ERR_UNKNOWN_CMD);
// }


// static void set_rfparam(atci_param_t *param)
// {
//     (void)param;
//     abort(ERR_UNKNOWN_CMD);
// }


// A version compatible with the original Type ABZ firmware
static void get_rfpower_comp(void)
{
    MibRequestConfirm_t r = { .Type  = MIB_CHANNELS_TX_POWER };
    abort_on_error(LoRaMacMibGetRequestConfirm(&r));
    OK("0,%d", r.Param.ChannelsTxPower);
}


// A version compatible with the original Type ABZ firmware
static void set_rfpower_comp(atci_param_t *param)
{
    uint32_t paboost, val;

    if (!atci_param_get_uint(param, &paboost)) abort(ERR_PARAM);
    if (paboost != 0 && paboost != 1)
        abort(ERR_PARAM);

    if (!atci_param_is_comma(param)) abort(ERR_PARAM);

    if (!atci_param_get_uint(param, &val)) abort(ERR_PARAM);
    if (val > 15) abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type  = MIB_CHANNELS_DEFAULT_TX_POWER,
        .Param = { .ChannelsDefaultTxPower = val }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    r.Type = MIB_CHANNELS_TX_POWER;
    r.Param.ChannelsTxPower = val;
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


static void get_nwk(void)
{
    MibRequestConfirm_t r = { .Type = MIB_PUBLIC_NETWORK };
    abort_on_error(LoRaMacMibGetRequestConfirm(&r));
    OK("%d", r.Param.EnablePublicNetwork);
}


static void set_nwk(atci_param_t *param)
{
    int enabled = parse_enabled(param);
    if (enabled == -1) abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type  = MIB_PUBLIC_NETWORK,
        .Param = { .EnablePublicNetwork = enabled }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


static void get_adr(void)
{
    MibRequestConfirm_t r = { .Type = MIB_ADR };
    abort_on_error(LoRaMacMibGetRequestConfirm(&r));
    OK("%d", r.Param.AdrEnable);
}


static void set_adr(atci_param_t *param)
{
    int enabled = parse_enabled(param);
    if (enabled == -1) abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type  = MIB_ADR,
        .Param = { .AdrEnable = enabled }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


// A version compatible with the original Type ABZ firmware
static void get_dr_comp(void)
{
    MibRequestConfirm_t r = { .Type = MIB_CHANNELS_DATARATE };
    abort_on_error(LoRaMacMibGetRequestConfirm(&r));
    OK("%d", r.Param.ChannelsDatarate);
}


// A version compatible with the original Type ABZ firmware
static void set_dr_comp(atci_param_t *param)
{
    uint32_t val;
    if (!atci_param_get_uint(param, &val)) abort(ERR_PARAM);
    if (val > 15) abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type  = MIB_CHANNELS_DEFAULT_DATARATE,
        .Param = { .ChannelsDefaultDatarate = val }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    r.Type = MIB_CHANNELS_DATARATE;
    r.Param.ChannelsDatarate = val;
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


static void get_delay(void)
{
    MibRequestConfirm_t r;

    r.Type = MIB_JOIN_ACCEPT_DELAY_1;
    LoRaMacMibGetRequestConfirm(&r);
    int join1 = r.Param.JoinAcceptDelay1;

    r.Type = MIB_JOIN_ACCEPT_DELAY_2;
    LoRaMacMibGetRequestConfirm(&r);
    int join2 = r.Param.JoinAcceptDelay2;

    r.Type = MIB_RECEIVE_DELAY_1;
    LoRaMacMibGetRequestConfirm(&r);
    int rx1 = r.Param.ReceiveDelay1;

    r.Type = MIB_RECEIVE_DELAY_2;
    LoRaMacMibGetRequestConfirm(&r);
    int rx2 = r.Param.ReceiveDelay2;

    OK("%d,%d,%d,%d", join1, join2, rx1, rx2);
}


static void set_delay(atci_param_t *param)
{
    MibRequestConfirm_t r;
    uint32_t join1, join2, rx1, rx2;

    if (!atci_param_get_uint(param, &join1)) abort(ERR_PARAM);
    if (!atci_param_is_comma(param)) abort(ERR_PARAM);
    if (!atci_param_get_uint(param, &join2)) abort(ERR_PARAM);
    if (!atci_param_is_comma(param)) abort(ERR_PARAM);
    if (!atci_param_get_uint(param, &rx1)) abort(ERR_PARAM);
    if (!atci_param_is_comma(param)) abort(ERR_PARAM);
    if (!atci_param_get_uint(param, &rx2)) abort(ERR_PARAM);

    r.Type = MIB_JOIN_ACCEPT_DELAY_1;
    r.Param.JoinAcceptDelay1 = join1;
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    r.Type = MIB_JOIN_ACCEPT_DELAY_2;
    r.Param.JoinAcceptDelay2 = join2;
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    r.Type = MIB_RECEIVE_DELAY_1;
    r.Param.ReceiveDelay1 = rx1;
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    r.Type = MIB_RECEIVE_DELAY_2;
    r.Param.ReceiveDelay2 = rx2;
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


// static void get_adrack(void)
// {
//     abort(ERR_UNKNOWN_CMD);
// }


// static void set_adrack(atci_param_t *param)
// {
//     (void)param;
//     abort(ERR_UNKNOWN_CMD);
// }


// A version compatible with the original Type ABZ firmware
static void get_rx2_comp(void)
{
    MibRequestConfirm_t r = { .Type = MIB_RX2_CHANNEL };
    abort_on_error(LoRaMacMibGetRequestConfirm(&r));

    OK("%ld,%d", r.Param.Rx2Channel.Frequency, r.Param.Rx2Channel.Datarate);
}


// A version compatible with the original Type ABZ firmware
static void set_rx2_comp(atci_param_t *param)
{
    uint32_t freq, dr;

    if (!atci_param_get_uint(param, &freq)) abort(ERR_PARAM);
    if (!atci_param_is_comma(param)) abort(ERR_PARAM);
    if (!atci_param_get_uint(param, &dr)) abort(ERR_PARAM);
    if (dr > 15) abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type = MIB_RX2_DEFAULT_CHANNEL,
        .Param = {
            .Rx2DefaultChannel = {
                .Frequency = freq,
                .Datarate = dr
            }
        }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    r.Type = MIB_RX2_CHANNEL;
    r.Param.Rx2Channel.Frequency = freq;
    r.Param.Rx2Channel.Datarate = dr;
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


static void get_dutycycle(void)
{
    LoRaMacNvmData_t *state = lrw_get_state();
    OK("%d", state->MacGroup2.DutyCycleOn);
}


static void set_dutycycle(atci_param_t *param)
{
    int enabled = parse_enabled(param);
    if (enabled == -1) abort(ERR_PARAM);

    LoRaMacTestSetDutyCycleOn(enabled);
    OK_();
}


static void get_sleep(void)
{
    OK("%d", sysconf.sleep);
}


static void set_sleep(atci_param_t *param)
{
    uint32_t v;

    if (!atci_param_get_uint(param, &v))
        abort(ERR_PARAM);

    if (v > 1) abort(ERR_PARAM);

    sysconf.sleep = v;
    sysconf_modified = true;
    OK_();
}


static void get_port(void)
{
    OK("%d", sysconf.default_port);
}


static void set_port(atci_param_t *param)
{
    int p = parse_port(param);
    if (p < 0) abort(ERR_PARAM);

    sysconf.default_port = p;
    sysconf_modified = true;
    OK_();
}


static void get_rep(void)
{
    OK("%d", sysconf.unconfirmed_retransmissions);
}


static void set_rep(atci_param_t *param)
{
    uint32_t v;
    if (!atci_param_get_uint(param, &v)) abort(ERR_PARAM);
    if (v < 1 || v > 15) abort(ERR_PARAM);

    sysconf.unconfirmed_retransmissions = v;
    sysconf_modified = true;
    OK_();
}


static void get_dformat(void)
{
    OK("%d", sysconf.data_format);
}


static void set_dformat(atci_param_t *param)
{
    uint32_t v;

    if (!atci_param_get_uint(param, &v))
        abort(ERR_PARAM);

    if (v != 0 && v != 1)
        abort(ERR_PARAM);

    sysconf.data_format = v;
    sysconf_modified = true;

    OK_();
}


static void get_to(void)
{
    OK("%d", sysconf.uart_timeout);
}


static void set_to(atci_param_t *param)
{
    uint32_t v;

    if (!atci_param_get_uint(param, &v))
        abort(ERR_PARAM);

    if (v < 1 || v > 65535)
        abort(ERR_PARAM);

    sysconf.uart_timeout = v;
    sysconf_modified = true;

    OK_();
}


static void payload_timeout(void *ctx)
{
    (void)ctx;
    log_debug("Payload reader timed out after %ld ms", payload_timer.ReloadValue);
    atci_abort_read_next_data();
}


static void transmit(atci_data_status_t status, atci_param_t *param)
{
    TimerStop(&payload_timer);

    if (status == ATCI_DATA_ENCODING_ERROR)
        abort(ERR_PARAM);

    // The original Type ABZ firmware returns an OK if payload submission times
    // out and sends an incomplete message, i.e., whatever has been received
    // before the timer fired. Hence, we don't check for ATCI_DATA_ABORTED here.

    abort_on_error(lrw_send(port, param->txt, param->length, request_confirmation));
    OK_();
}


static void utx(atci_param_t *param)
{
    uint32_t size;
    port = sysconf.default_port;

    if (param == NULL) abort(ERR_PARAM);
    if (!atci_param_get_uint(param, &size))
        abort(ERR_PARAM);

    // The maximum payload size in LoRaWAN seems to be 242 bytes (US region) in
    // the most favorable conditions. If the payload is transmitted hex-encoded
    // by the client, we need to read twice as much data.

    unsigned int mul = sysconf.data_format == 1 ? 2 : 1;
    if (size > 242 * mul)
        abort(ERR_PAYLOAD_LONG);

    TimerInit(&payload_timer, payload_timeout);
    TimerSetValue(&payload_timer, sysconf.uart_timeout);
    TimerStart(&payload_timer);

    request_confirmation = false;
    if (!atci_set_read_next_data(size,
        sysconf.data_format == 1 ? ATCI_ENCODING_HEX : ATCI_ENCODING_BIN, transmit))
        abort(ERR_PAYLOAD_LONG);
}


static void ctx(atci_param_t *param)
{
    (void)param;
    utx(param);
    request_confirmation = true;
}


// static void get_mcast(void)
// {
//     abort(ERR_UNKNOWN_CMD);
// }


// static void set_mcast(atci_param_t *param)
// {
//     (void)param;
//     abort(ERR_UNKNOWN_CMD);
// }


static void putx(atci_param_t *param)
{
    int p;

    if (param == NULL) abort(ERR_PARAM);
    p = parse_port(param);
    if (p < 0) abort(ERR_PARAM);

    if (!atci_param_is_comma(param))
        abort(ERR_PARAM);

    utx(param);
    port = p;
}


static void pctx(atci_param_t *param)
{
    putx(param);
    request_confirmation = true;
}


static void get_frmcnt(void)
{
    uint32_t down;
    LoRaMacNvmData_t *state = lrw_get_state();

    MibRequestConfirm_t r = { .Type = MIB_LORAWAN_VERSION };
    LoRaMacMibGetRequestConfirm(&r);

    if (r.Param.LrWanVersion.LoRaWan.Fields.Minor == 0)
        down = state->Crypto.FCntList.FCntDown;
    else
        down = state->Crypto.FCntList.AFCntDown;

    OK("%lu,%lu", state->Crypto.FCntList.FCntUp, down);
}


static void get_msize(void)
{
    LoRaMacTxInfo_t txi;
    LoRaMacStatus_t rc = LoRaMacQueryTxPossible(0, &txi);
    switch(rc) {
        case LORAMAC_STATUS_OK:
            OK("%d", txi.MaxPossibleApplicationDataSize);
            break;

        case LORAMAC_STATUS_LENGTH_ERROR:
            OK("%d", 0);
            break;

        default:
            abort_on_error(rc);
            break;
    }
}


static void get_rfq(void)
{
    OK("%d,%d", radio_rssi, radio_snr);
}


static void get_dwell(void)
{
    LoRaMacNvmData_t *state = lrw_get_state();
    OK("%d,%d", state->MacGroup2.MacParams.UplinkDwellTime,
        state->MacGroup2.MacParams.DownlinkDwellTime);
}


static void set_dwell(atci_param_t *param)
{
    bool uplink, downlink;

    switch (param->txt[param->offset++]) {
        case '0': uplink = false; break;
        case '1': uplink = true; break;
        default : abort(ERR_PARAM);
    }

    if (!atci_param_is_comma(param))
        abort(ERR_PARAM);

    switch (param->txt[param->offset++]) {
        case '0': downlink = false; break;
        case '1': downlink = true; break;
        default : abort(ERR_PARAM);
    }

    abort_on_error(lrw_set_dwell(uplink, downlink));
    OK_();
}


static void get_maxeirp(void)
{
    LoRaMacNvmData_t *state = lrw_get_state();
    OK("%.0f", state->MacGroup2.MacParams.MaxEirp);
}


static void set_maxeirp(atci_param_t *param)
{
    uint32_t val;

    if (!atci_param_get_uint(param, &val))
        abort(ERR_PARAM);

    lrw_set_maxeirp(val);
    OK_();
}


// static void get_rssith(void)
// {
//     abort(ERR_UNKNOWN_CMD);
// }


// static void set_rssith(atci_param_t *param)
// {
//     (void)param;
//     abort(ERR_UNKNOWN_CMD);
// }


// static void get_cst(void)
// {
//     abort(ERR_UNKNOWN_CMD);
// }


// static void set_cst(atci_param_t *param)
// {
//     (void)param;
//     abort(ERR_UNKNOWN_CMD);
// }


// static void get_backoff(void)
// {
//     abort(ERR_UNKNOWN_CMD);
// }


// A version compatible with the original Type ABZ firmware
static void get_chmask_comp(void)
{
    MibRequestConfirm_t r = { .Type = MIB_CHANNELS_MASK };
    abort_on_error(LoRaMacMibGetRequestConfirm(&r));
    atci_print("+OK=");
    atci_print_buffer_as_hex(r.Param.ChannelsMask, lrw_get_chmask_length() * sizeof(r.Param.ChannelsMask[0]));
    EOL();
}


// A version compatible with the original Type ABZ firmware
static void set_chmask_comp(atci_param_t *param)
{
    uint16_t chmask[REGION_NVM_CHANNELS_MASK_SIZE];

    memset(chmask, 0, sizeof(chmask));
    size_t len = atci_param_get_buffer_from_hex(param, chmask, sizeof(chmask));
    if (len != lrw_get_chmask_length() * sizeof(chmask[0]))
        abort(ERR_PARAM);

    // First set the default channel mask. The default channel mask is the
    // channel mask used before Join or ADR.
    MibRequestConfirm_t r = {
        .Type  = MIB_CHANNELS_DEFAULT_MASK,
        .Param = { .ChannelsDefaultMask = chmask }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    // Then update the channel mask currently in use
    r.Type = MIB_CHANNELS_MASK;
    r.Param.ChannelsMask = chmask;
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


static void get_rtynum(void)
{
    OK("%d", sysconf.confirmed_retransmissions);
}


static void set_rtynum(atci_param_t *param)
{
    uint32_t v;
    if (!atci_param_get_uint(param, &v)) abort(ERR_PARAM);
    if (v < 1 || v > 15) abort(ERR_PARAM);

    sysconf.confirmed_retransmissions = v;
    sysconf_modified = true;
    OK_();
}


static void get_netid(void)
{
    MibRequestConfirm_t r = { .Type = MIB_NET_ID };
    abort_on_error(LoRaMacMibGetRequestConfirm(&r));
    OK("%08lX", r.Param.NetID);
}


static void set_netid(atci_param_t *param)
{
    uint32_t buf;
    if (atci_param_get_buffer_from_hex(param, &buf, sizeof(buf)) != sizeof(buf))
        abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type  = MIB_NET_ID,
        .Param = { .NetID = ntohl(buf) }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


static void get_channels(void)
{
    lrw_channel_list_t list = lrw_get_channel_list();

    // log_debug("%d %d", list.length, list.chmask_length);
    // log_dump(list.chmask, list.chmask_length * 2, "masks");
    // log_dump(list.chmask_default, list.chmask_length * 2, "default_mask");

    for (unsigned int i = 0; i < list.length; i++) {
        if (list.channels[i].Frequency == 0)
            continue;

        int is_enabled = (i / 16) < list.chmask_length ? (list.chmask[i / 16] >> (i % 16)) & 0x01 : 0;

        atci_printf("$CHANNELS: %d,%ld,%ld,%d,%d,%d\r\n",
            is_enabled,
            list.channels[i].Frequency,
            list.channels[i].Rx1Frequency,
            list.channels[i].DrRange.Fields.Min,
            list.channels[i].DrRange.Fields.Max,
            list.channels[i].Band);
    }
    OK_();
}


static void dbg(atci_param_t *param)
{
    (void)param;
    // RF_IDLE = 0,   //!< The radio is idle
    // RF_RX_RUNNING, //!< The radio is in reception state
    // RF_TX_RUNNING, //!< The radio is in transmission state
    // RF_CAD,        //!< The radio is doing channel activity detection
    atci_printf("$DBG: \"stop_mode_mask\",%d\r\n", system_get_stop_mode_mask());
    atci_printf("$DBG: \"radio_state\",%d\r\n", Radio.GetStatus());
    OK_();
}


static void do_halt(atci_param_t *param)
{
    (void)param;
    OK_();
    atci_flush();

    halt(NULL);
}


static void get_nwkkey(void)
{
    LoRaMacNvmData_t *state = lrw_get_state();
    atci_print("+OK=");
    atci_print_buffer_as_hex(&state->SecureElement.KeyList[NWK_KEY].KeyValue, SE_KEY_SIZE);
    EOL();
}


static void set_nwkkey(atci_param_t *param)
{
    uint8_t key[SE_KEY_SIZE];

    if (atci_param_get_buffer_from_hex(param, key, SE_KEY_SIZE) != SE_KEY_SIZE)
        abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type  = MIB_NWK_KEY,
        .Param = { .NwkKey = key }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


static void get_fnwksintkey(void)
{
    LoRaMacNvmData_t *state = lrw_get_state();
    atci_print("+OK=");
    atci_print_buffer_as_hex(&state->SecureElement.KeyList[F_NWK_S_INT_KEY].KeyValue, SE_KEY_SIZE);
    EOL();
}


static void set_fnwksintkey(atci_param_t *param)
{
    uint8_t key[SE_KEY_SIZE];

    if (atci_param_get_buffer_from_hex(param, key, SE_KEY_SIZE) != SE_KEY_SIZE)
        abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type  = MIB_F_NWK_S_INT_KEY,
        .Param = { .FNwkSIntKey = key }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


static void get_snwksintkey(void)
{
    LoRaMacNvmData_t *state = lrw_get_state();
    atci_print("+OK=");
    atci_print_buffer_as_hex(&state->SecureElement.KeyList[S_NWK_S_INT_KEY].KeyValue, SE_KEY_SIZE);
    EOL();
}


static void set_snwksintkey(atci_param_t *param)
{
    uint8_t key[SE_KEY_SIZE];

    if (atci_param_get_buffer_from_hex(param, key, SE_KEY_SIZE) != SE_KEY_SIZE)
        abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type  = MIB_S_NWK_S_INT_KEY,
        .Param = { .SNwkSIntKey = key }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


static void get_nwksenckey(void)
{
    LoRaMacNvmData_t *state = lrw_get_state();
    atci_print("+OK=");
    atci_print_buffer_as_hex(&state->SecureElement.KeyList[NWK_S_ENC_KEY].KeyValue, SE_KEY_SIZE);
    EOL();
}


static void set_nwksenckey(atci_param_t *param)
{
    uint8_t key[SE_KEY_SIZE];

    if (atci_param_get_buffer_from_hex(param, key, SE_KEY_SIZE) != SE_KEY_SIZE)
        abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type  = MIB_NWK_S_ENC_KEY,
        .Param = { .NwkSEncKey = key }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


static void get_chmask(void)
{
    atci_print("+OK=");

    MibRequestConfirm_t r = { .Type = MIB_CHANNELS_MASK };
    LoRaMacMibGetRequestConfirm(&r);
    atci_print_buffer_as_hex(r.Param.ChannelsMask, lrw_get_chmask_length() * sizeof(r.Param.ChannelsMask[0]));

    atci_print(",");

    r.Type = MIB_CHANNELS_DEFAULT_MASK;
    LoRaMacMibGetRequestConfirm(&r);
    atci_print_buffer_as_hex(r.Param.ChannelsDefaultMask, lrw_get_chmask_length() * sizeof(r.Param.ChannelsDefaultMask[0]));

    EOL();
}


static void set_chmask(atci_param_t *param)
{
    uint16_t chmask1[REGION_NVM_CHANNELS_MASK_SIZE];
    uint16_t chmask2[REGION_NVM_CHANNELS_MASK_SIZE];
    unsigned int len = lrw_get_chmask_length() * sizeof(chmask1[0]);
    memset(chmask1, 0, sizeof(chmask1));
    memset(chmask2, 0, sizeof(chmask2));

    size_t len1 = atci_param_get_buffer_from_hex(param, chmask1, sizeof(chmask1));
    if (len1 != len) abort(ERR_PARAM);

    if (!atci_param_is_comma(param)) abort(ERR_PARAM);

    size_t len2 = atci_param_get_buffer_from_hex(param, chmask2, sizeof(chmask2));
    if (len2 != len) abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type  = MIB_CHANNELS_DEFAULT_MASK,
        .Param = { .ChannelsDefaultMask = chmask2 }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    // Then update the channel mask currently in use
    r.Type = MIB_CHANNELS_MASK;
    r.Param.ChannelsMask = chmask1;
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


static void get_rx2(void)
{
    MibRequestConfirm_t r1 = { .Type = MIB_RX2_CHANNEL };
    LoRaMacMibGetRequestConfirm(&r1);

    MibRequestConfirm_t r2 = { .Type = MIB_RX2_DEFAULT_CHANNEL };
    LoRaMacMibGetRequestConfirm(&r2);

    OK("%ld,%d,%ld,%d", r1.Param.Rx2Channel.Frequency, r1.Param.Rx2Channel.Datarate,
        r2.Param.Rx2DefaultChannel.Frequency, r2.Param.Rx2DefaultChannel.Datarate);
}


static void set_rx2(atci_param_t *param)
{
    uint32_t freq1, dr1, freq2, dr2;

    if (!atci_param_get_uint(param, &freq1)) abort(ERR_PARAM);
    if (!atci_param_is_comma(param)) abort(ERR_PARAM);
    if (!atci_param_get_uint(param, &dr1)) abort(ERR_PARAM);

    if (!atci_param_is_comma(param)) abort(ERR_PARAM);

    if (!atci_param_get_uint(param, &freq2)) abort(ERR_PARAM);
    if (!atci_param_is_comma(param)) abort(ERR_PARAM);
    if (!atci_param_get_uint(param, &dr2)) abort(ERR_PARAM);

    if (dr1 > 15 || dr2 > 15) abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type = MIB_RX2_DEFAULT_CHANNEL,
        .Param = {
            .Rx2DefaultChannel = {
                .Frequency = freq2,
                .Datarate = dr2
            }
        }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    r.Type = MIB_RX2_CHANNEL;
    r.Param.Rx2Channel.Frequency = freq1;
    r.Param.Rx2Channel.Datarate = dr1;
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


static void get_dr(void)
{
    MibRequestConfirm_t r1 = { .Type = MIB_CHANNELS_DATARATE };
    LoRaMacMibGetRequestConfirm(&r1);

    MibRequestConfirm_t r2 = { .Type = MIB_CHANNELS_DEFAULT_DATARATE };
    LoRaMacMibGetRequestConfirm(&r2);

    OK("%d,%d", r1.Param.ChannelsDatarate, r2.Param.ChannelsDefaultDatarate);
}


static void set_dr(atci_param_t *param)
{
    uint32_t val1, val2;

    if (!atci_param_get_uint(param, &val1)) abort(ERR_PARAM);
    if (val1 > 15) abort(ERR_PARAM);

    if (!atci_param_is_comma(param)) abort(ERR_PARAM);

    if (!atci_param_get_uint(param, &val2)) abort(ERR_PARAM);
    if (val2 > 15) abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type  = MIB_CHANNELS_DEFAULT_DATARATE,
        .Param = { .ChannelsDefaultDatarate = val2 }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    r.Type = MIB_CHANNELS_DATARATE;
    r.Param.ChannelsDatarate = val1;
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


static void get_rfpower(void)
{
    MibRequestConfirm_t r1 = { .Type  = MIB_CHANNELS_TX_POWER };
    LoRaMacMibGetRequestConfirm(&r1);
    MibRequestConfirm_t r2 = { .Type  = MIB_CHANNELS_DEFAULT_TX_POWER };
    LoRaMacMibGetRequestConfirm(&r2);
    OK("0,%d,0,%d", r1.Param.ChannelsTxPower, r2.Param.ChannelsDefaultTxPower);
}


static void set_rfpower(atci_param_t *param)
{
    uint32_t paboost1, paboost2, val1, val2;

    if (!atci_param_get_uint(param, &paboost1)) abort(ERR_PARAM);
    if (paboost1 != 0) {
        log_warning("PA boost currently unsupported");
        abort(ERR_PARAM);
    }

    if (!atci_param_is_comma(param)) abort(ERR_PARAM);

    if (!atci_param_get_uint(param, &val1)) abort(ERR_PARAM);
    if (val1 > 15) abort(ERR_PARAM);

    if (!atci_param_is_comma(param)) abort(ERR_PARAM);

    if (!atci_param_get_uint(param, &paboost2)) abort(ERR_PARAM);
    if (paboost2 != 0) {
        log_warning("PA boost currently unsupported");
        abort(ERR_PARAM);
    }

    if (!atci_param_is_comma(param)) abort(ERR_PARAM);

    if (!atci_param_get_uint(param, &val2)) abort(ERR_PARAM);
    if (val2 > 15) abort(ERR_PARAM);

    MibRequestConfirm_t r = {
        .Type  = MIB_CHANNELS_DEFAULT_TX_POWER,
        .Param = { .ChannelsDefaultTxPower = val2 }
    };
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    r.Type = MIB_CHANNELS_TX_POWER;
    r.Param.ChannelsTxPower = val1;
    abort_on_error(LoRaMacMibSetRequestConfirm(&r));

    OK_();
}


static void get_loglevel(void)
{
    OK("%d", log_get_level());
}


static void set_loglevel(atci_param_t *param)
{
    (void)param;
    uint32_t level;

    if (!atci_param_get_uint(param, &level))
        abort(ERR_PARAM);

    if (level > 5) abort(ERR_PARAM);

    log_set_level(level);
    OK_();
}

static const atci_command_t cmds[] = {
    {"+UART",        NULL,    set_uart,         get_uart,         NULL, "Configure UART interface"},
    {"+VER",         NULL,    NULL,             get_version_comp, NULL, "Firmware version and build time"},
    {"+DEV",         NULL,    NULL,             get_model,        NULL, "Device model"},
    {"+REBOOT",      reboot,  NULL,             NULL,             NULL, "Reboot"},
    {"+FACNEW",      facnew,  NULL,             NULL,             NULL, "Restore modem to factory"},
    {"+BAND",        NULL,    set_band,         get_band,         NULL, "Configure radio band (region)"},
    {"+CLASS",       NULL,    set_class,        get_class,        NULL, "Configure LoRaWAN class"},
    {"+MODE",        NULL,    set_mode,         get_mode,         NULL, "Configure activation mode (1:OTTA 0:ABP)"},
    {"+DEVADDR",     NULL,    set_devaddr,      get_devaddr,      NULL, "Configure DevAddr"},
    {"+DEVEUI",      NULL,    set_deveui,       get_deveui,       NULL, "Configure DevEUI"},
    {"+APPEUI",      NULL,    set_joineui,      get_joineui,      NULL, "Configure AppEUI (JoinEUI)"},
    {"+NWKSKEY",     NULL,    set_nwkskey,      get_nwkskey,      NULL, "Configure NwkSKey (LoRaWAN 1.0)"},
    {"+APPSKEY",     NULL,    set_appskey,      get_appskey,      NULL, "Configure AppSKey"},
    {"+APPKEY",      NULL,    set_appkey_10,    get_appkey,       NULL, "Configure AppKey (LoRaWAN 1.0)"},
    {"+JOIN",        join,    NULL,             NULL,             NULL, "Send OTAA Join packet"},
    {"+JOINDC",      NULL,    set_joindc,       get_joindc,       NULL, "Configure OTAA Join duty cycling"},
    {"+LNCHECK",     lncheck, lncheck,          NULL,             NULL, "Perform link check"},
    // {"+RFPARAM",     NULL,    set_rfparam,      get_rfparam,      NULL, "Configure RF channel parameters"},
    {"+RFPOWER",     NULL,    set_rfpower_comp, get_rfpower_comp, NULL, "Configure RF power"},
    {"+NWK",         NULL,    set_nwk,          get_nwk,          NULL, "Configure public/private LoRa network setting"},
    {"+ADR",         NULL,    set_adr,          get_adr,          NULL, "Configure adaptive data rate (ADR)"},
    {"+DR",          NULL,    set_dr_comp,      get_dr_comp,      NULL, "Configure data rate (DR)"},
    {"+DELAY",       NULL,    set_delay,        get_delay,        NULL, "Configure receive window offsets"},
    // {"+ADRACK",      NULL,    set_adrack,       get_adrack,       NULL, "Configure ADR ACK parameters"},
    {"+RX2",         NULL,    set_rx2_comp,     get_rx2_comp,     NULL, "Configure RX2 window frequency and data rate"},
    {"+DUTYCYCLE",   NULL,    set_dutycycle,    get_dutycycle,    NULL, "Configure duty cycling in EU868"},
    {"+SLEEP",       NULL,    set_sleep,        get_sleep,        NULL, "Configure low power (sleep) mode"},
    {"+PORT",        NULL,    set_port,         get_port,         NULL, "Configure default port number for uplink messages <1,223>"},
    {"+REP",         NULL,    set_rep,          get_rep,          NULL, "Unconfirmed message repeats [1..15]"},
    {"+DFORMAT",     NULL,    set_dformat,      get_dformat,      NULL, "Configure payload format used by the modem"},
    {"+TO",          NULL,    set_to,           get_to,           NULL, "Configure UART port timeout"},
    {"+UTX",         utx,     NULL,             NULL,             NULL, "Send unconfirmed uplink message"},
    {"+CTX",         ctx,     NULL,             NULL,             NULL, "Send confirmed uplink message"},
    // {"+MCAST",       NULL,    set_mcast,        get_mcast,        NULL, "Configure multicast addresses"},
    {"+PUTX",        putx,    NULL,             NULL,             NULL, "Send unconfirmed uplink message to port"},
    {"+PCTX",        pctx,    NULL,             NULL,             NULL, "Send confirmed uplink message to port"},
    {"+FRMCNT",      NULL,    NULL,             get_frmcnt,       NULL, "Return current values for uplink and downlink counters"},
    {"+MSIZE",       NULL,    NULL,             get_msize,        NULL, "Return maximum payload size for current data rate"},
    {"+RFQ",         NULL,    NULL,             get_rfq,          NULL, "Return RSSI and SNR of the last received message"},
    {"+DWELL",       NULL,    set_dwell,        get_dwell,        NULL, "Configure dwell setting for AS923"},
    {"+MAXEIRP",     NULL,    set_maxeirp,      get_maxeirp,      NULL, "Configure maximum EIRP"},
    // {"+RSSITH",      NULL,    set_rssith,       get_rssith,       NULL, "Configure RSSI threshold for LBT"},
    // {"+CST",         NULL,    set_cst,          get_cst,          NULL, "Configure carrier sensor time (CST) for LBT"},
    // {"+BACKOFF",     NULL,    NULL,             get_backoff,      NULL, "Return duty cycle backoff time for EU868"},
    {"+CHMASK",      NULL,    set_chmask_comp,  get_chmask_comp,  NULL, "Configure channel mask"},
    {"+RTYNUM",      NULL,    set_rtynum,       get_rtynum,       NULL, "Configure number of confirmed uplink message retries"},
    {"+NETID",       NULL,    set_netid,        get_netid,        NULL, "Configure LoRaWAN network identifier"},
    {"$CHANNELS",    NULL,    NULL,             get_channels,     NULL, ""},
    {"$VER",         NULL,    NULL,             get_version,      NULL, "Firmware version and build time"},
    {"$DBG",         dbg,     NULL,             NULL,             NULL, ""},
    {"$HALT",        do_halt, NULL,             NULL,             NULL, "Halt the modem"},
    {"$JOINEUI",     NULL,    set_joineui,      get_joineui,      NULL, "Configure JoinEUI"},
    {"$NWKKEY",      NULL,    set_nwkkey,       get_nwkkey,       NULL, "Configure NwkKey (LoRaWAN 1.1)"},
    {"$APPKEY",      NULL,    set_appkey_11,    get_appkey,       NULL, "Configure AppKey (LoRaWAN 1.1)"},
    {"$FNWKSINTKEY", NULL,    set_fnwksintkey,  get_fnwksintkey,  NULL, "Configure FNwkSIntKey (LoRaWAN 1.1)"},
    {"$SNWKSINTKEY", NULL,    set_snwksintkey,  get_snwksintkey,  NULL, "Configure SNwkSIntKey (LoRaWAN 1.1)"},
    {"$NWKSENCKEY",  NULL,    set_nwksenckey,   get_nwksenckey,   NULL, "Configure NwkSEncKey (LoRaWAN 1.1)"},
    {"$CHMASK",      NULL,    set_chmask,       get_chmask,       NULL, "Configure channel mask"},
    {"$RX2",         NULL,    set_rx2,          get_rx2,          NULL, "Configure RX2 window frequency and data rate"},
    {"$DR",          NULL,    set_dr,           get_dr,           NULL, "Configure data rate (DR)"},
    {"$RFPOWER",     NULL,    set_rfpower,      get_rfpower,      NULL, "Configure RF power"},
    {"$LOGLEVEL",    NULL,    set_loglevel,     get_loglevel,     NULL, "Configure logging on USART port"},
    ATCI_COMMAND_CLAC,
    ATCI_COMMAND_HELP};


void cmd_init(unsigned int baudrate)
{
    atci_init(baudrate, cmds, ATCI_COMMANDS_LENGTH(cmds));
}


void cmd_event(unsigned int type, unsigned int subtype)
{
    atci_printf("+EVENT=%d,%d\r\n\r\n", type, subtype);
}


void cmd_ans(unsigned int margin, unsigned int gwcnt)
{
    atci_printf("+ANS=2,%d,%d\r\n\r\n", margin, gwcnt);
}
