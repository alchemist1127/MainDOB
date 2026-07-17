#include <DobConfig.h>
#include <dob/ipc.h>
#include <dob/reconnect.h>
#include <string.h>

static uint32_t config_port = 0;
#define IPC_CALL(m, r) _dob_call_reconnect(&config_port, "config", 2000, m, r)

int dobconfig_Get(const char *key, char *value_out, uint32_t max_len)
{
    if (!key || !value_out) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = 1;  /* GET */
    msg.payload = (void *)key;
    msg.payload_size = strlen(key) + 1;

    if (IPC_CALL(&msg, &reply) != DOB_OK || reply.code != 0)
        return -1;

    if (reply.payload && reply.payload_size > 0)
    {
        uint32_t copy = reply.payload_size < max_len ? reply.payload_size : max_len - 1;
        memcpy(value_out, reply.payload, copy);
        value_out[copy] = '\0';
    }
    return 0;
}

int dobconfig_Set(const char *key, const char *value)
{
    if (!key || !value) return -1;

    /* Pack "key\0value" into single payload */
    uint32_t klen = strlen(key) + 1;
    uint32_t vlen = strlen(value) + 1;
    char buf[512];
    if (klen + vlen > sizeof(buf)) return -1;
    memcpy(buf, key, klen);
    memcpy(buf + klen, value, vlen);

    dob_msg_t msg = {0}, reply = {0};
    msg.code = 2;  /* SET */
    msg.payload = buf;
    msg.payload_size = klen + vlen;

    return IPC_CALL(&msg, &reply) == DOB_OK ? (int)reply.code : -1;
}

int dobconfig_List(char *buf, uint32_t max_len)
{
    if (!buf) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = 3;  /* LIST */

    if (IPC_CALL(&msg, &reply) != DOB_OK) return -1;

    if (reply.payload && reply.payload_size > 0)
    {
        uint32_t copy = reply.payload_size < max_len ? reply.payload_size : max_len - 1;
        memcpy(buf, reply.payload, copy);
        buf[copy] = '\0';
    }
    return (int)reply.arg0;
}

int dobconfig_GetAssoc(const char *ext, char *prog_out, uint32_t max_len)
{
    if (!ext || !prog_out) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = 6;  /* GET_ASSOC */
    msg.payload = (void *)ext;
    msg.payload_size = strlen(ext) + 1;

    if (IPC_CALL(&msg, &reply) != DOB_OK || reply.code != 0)
        return -1;

    if (reply.payload && reply.payload_size > 0)
    {
        uint32_t copy = reply.payload_size < max_len ? reply.payload_size : max_len - 1;
        memcpy(prog_out, reply.payload, copy);
        prog_out[copy] = '\0';
    }
    return 0;
}

int dobconfig_SetAssoc(const char *ext, const char *prog_path)
{
    if (!ext || !prog_path) return -1;

    uint32_t elen = strlen(ext) + 1;
    uint32_t plen = strlen(prog_path) + 1;
    char buf[512];
    if (elen + plen > sizeof(buf)) return -1;
    memcpy(buf, ext, elen);
    memcpy(buf + elen, prog_path, plen);

    dob_msg_t msg = {0}, reply = {0};
    msg.code = 7;  /* SET_ASSOC */
    msg.payload = buf;
    msg.payload_size = elen + plen;

    return IPC_CALL(&msg, &reply) == DOB_OK ? (int)reply.code : -1;
}

int dobconfig_RemoveAssoc(const char *ext)
{
    if (!ext) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = 8;  /* REMOVE_ASSOC */
    msg.payload = (void *)ext;
    msg.payload_size = strlen(ext) + 1;

    return IPC_CALL(&msg, &reply) == DOB_OK ? (int)reply.code : -1;
}

dobconfig_interface_t dobconfig = {
    .settings = {
        .Get  = dobconfig_Get,
        .Set  = dobconfig_Set,
        .List = dobconfig_List,
    },
    .associations = {
        .Get    = dobconfig_GetAssoc,
        .Set    = dobconfig_SetAssoc,
        .Remove = dobconfig_RemoveAssoc,
    }
};
