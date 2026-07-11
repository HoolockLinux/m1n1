/* SPDX-License-Identifier: MIT */
#include "adt.h"
#include "akf.h"
#include "ans1.h"
#include "assert.h"
#include "malloc.h"
#include "pmgr.h"
#include "rtkit.h"
#include "string.h"
#include "types.h"
#include "utils.h"

/* based on syslog of command 0x52 */
#define ANS1_TIMEOUT      3000000
#define ANS1_CMD_HDR_SIZE 0x30

/* Parameter of this ANS1 Client */
#define ANS1_CMD_SIZE     128
#define ANS1_NUM_TAGS     1
#define ANS1_MAX_IO_PAGES ((ANS1_CMD_SIZE - ANS1_CMD_HDR_SIZE) / sizeof(uint32_t))

/* ANS1 commands */
#define ANS1_CMD_OP_IDENTIFY 0x0

/*
 * The main storage area in syslog is known as 'UserArea' in some locations,
 * but 'LBA' in some other locations
 */
#define ANS1_CMD_OP_READ_USERAREA  0x10
#define ANS1_CMD_OP_WRITE_USERAREA 0x11

/* Destructive Commands */
#define ANS1_CMD_OP_FORMAT_ALL      0x15
#define ANS1_CMD_OP_FORMAT_USERAREA 0x16
/*
 * Syslog claims this one is "Format_Neuralize", but considering that after
 * completing this command ANS1 will intentionally crash, this is probably
 * meant to be "Neutralize".
 *
 * After sending this command and rebooting, NAND IO don't work (times
 * out) and the FORMAT_ALL command must be ran before it works again,
 * so this command probably leaves NAND uninitialized after wiping.
 */
#define ANS1_CMD_OP_FORMAT_CLEAR 0x18

/* Allow writes to locations other than NVRAM and PANICLOG */
#define ANS1_CMD_OP_WRITE_UNLOCK 0x19

/*
 * Auxiliary storage
 * For writes, the starting sector must be zero, and reading "unwritten"
 * (according to syslog) sectors would return error 2.
 */
#define ANS1_CMD_OP_READ_LLB      0x30
#define ANS1_CMD_OP_READ_FW       0x31
#define ANS1_CMD_OP_READ_UTILDM   0x32
#define ANS1_CMD_OP_READ_DM       0x33
#define ANS1_CMD_OP_READ_CTRLBITS 0x34
#define ANS1_CMD_OP_READ_EFFACE   0x35
#define ANS1_CMD_OP_READ_NVRAM    0x36
#define ANS1_CMD_OP_READ_SYSCFG   0x37
#define ANS1_CMD_OP_READ_PANICLOG 0x38

#define ANS1_CMD_OP_WRITE_LLB      0x40
#define ANS1_CMD_OP_WRITE_FW       0x41
#define ANS1_CMD_OP_WRITE_UTILDM   0x42
#define ANS1_CMD_OP_WRITE_DM       0x43
#define ANS1_CMD_OP_WRITE_CTRLBITS 0x44
#define ANS1_CMD_OP_WRITE_EFFACE   0x45
#define ANS1_CMD_OP_WRITE_NVRAM    0x46
#define ANS1_CMD_OP_WRITE_SYSCFG   0x47
#define ANS1_CMD_OP_WRITE_PANICLOG 0x48

#define ANS1_CMD_OP_GET_PPN_FW_VERSION 0x79

#define ANS1_CMD_OP_POWER_CONFIG 0x80

/* Sizes from iPad Air 2 J81AP */
/*
 * Something is missing, since the work model documented here could write
 * at most 512 sectors at once, but on the device there are stuff beyond
 * 2097152 byte and iOS's disk2 is 4194304 byte in size.
 */
#define ANS1_FW_NUM_SECTORS 1024
/*
 * Weird number of sectors... maybe some sectors are allocated for UTILDM
 */
#define ANS1_LLB_NUM_SECTORS 254
/*
 * On the device by default there's 1 sector of data already present,
 * but for writes the maximum size is '0', which may be an Apple hack
 * to represent read-only.
 *
 * Based on the number of LLB secors, this is tentatively set to 2.
 */
#define ANS1_UTILDM_NUM_SECTORS 2
/* Need to write at least 12 sectors at once otherwise ANS1 will crash */
#define ANS1_DM_NUM_SECTORS       32
#define ANS1_CTRLBITS_NUM_SECTORS 32
#define ANS1_EFFACE_NUM_SECTORS   32
#define ANS1_NVRAM_NUM_SECTORS    32
#define ANS1_SYSCFG_NUM_SECTORS   256
#define ANS1_PANICLOG_NUM_SECTORS 256

/* mailbox messages */
#define ANS1_MSG_TYPE                GENMASK(3, 0)
#define ANS1_MSG_TYPE_SET_CMDBUF     0
#define ANS1_MSG_TYPE_SET_TAG_CMDBUF 1
#define ANS1_MSG_TYPE_COMPLETE       2
#define ANS1_MSG_TYPE_SQ_DB          3
#define ANS1_MSG_TYPE_UNK4           4

#define ANS1_SET_CMDBUF_BASE      GENMASK(55, 16)
#define ANS1_SET_CMDBUF_SIZE_17_6 GENMASK(15, 4)

#define ANS1_SET_TAG_CMDBUF_OFF_17_6  GENMASK(23, 12)
#define ANS1_SET_TAG_CMDBUF_SIZE_17_6 GENMASK(35, 24)
#define ANS1_SET_TAG_CMDBUF_TAG       GENMASK(7, 4)

#define ANS1_SQ_DB_OP      GENMASK(19, 12)
#define ANS1_SQ_DB_OP_RING 0xff
#define ANS1_SQ_DB_TAG     GENMASK(7, 4)

/*
 * This is wider than the submission tag, since ANS1 will send
 * two special tags during startup:
 *
 * 254 - sent automatically soon after ANS endpoint is enable,
 * possibly ready signal
 *
 * 253 - reply of ANS1_SET_CMDBUF_BASE
 */
#define ANS1_COMPLETION_STATUS    GENMASK(15, 12)
#define ANS1_COMPLETION_STATUS_OK 0
/* Errors from syslog strings */
/* Not exist command */
#define ANS1_COMPLETION_STATUS_IGNORED 1
/* Trying to write while write-protected, write OOB etc */
#define ANS1_COMPLETION_STATUS_ABORT 2
#define ANS1_COMPLETION_STATUS_INIT  0xa /* Seen with the special tags */

#define ANS1_COMPLETION_TAG_READY        254
#define ANS1_COMPLETION_TAG_CMDBUF_READY 253

#define ANS1_COMPLETION_TAG GENMASK(11, 4)

/*
 * Without this flag the operation would still take place but instead of
 * the intended data garbage corresponding to the input data in 128-bit
 * block sizes would be used, so likely encryption.
 */
#define ANS1_CMD_IO_FLAG_NO_ENCRYPTION BIT(3)

struct ans1_cmd_hdr {
    u8 op;
    u8 tag;
    u16 flags;
    u32 slba;
    u32 length;
    u8 pad_io[0x24];
} PACKED;

struct ans1_io {
    u32 sgl[ANS1_MAX_IO_PAGES];
} PACKED;

struct asp_identify {
    u32 num_lba_formatted;
    u32 lba_size;
    u8 unk1[8];
    bool util_formatted;
    u8 unk2[7];
    u32 num_lba_raw;
    u8 unk3[20];
    u8 chip_id_bus0[6];
    u8 chip_id_bus1[6];
    u8 manufacturer_id_bus0[6];
    u8 manufacturer_id_bus1[6];
} PACKED;

struct asp_ppn_fw {
    u32 fw_ver_len;
    char fw_ver[0x10][2];
} PACKED;

static_assert(sizeof(struct ans1_cmd_hdr) == ANS1_CMD_HDR_SIZE, "wrong ans1 command size");
static_assert((ANS1_CMD_SIZE & 0x3f) == 0, "ans1 command must be aligned to cacheline");

static struct ans1_cmd_hdr *cmd;
static akf_dev_t *ans1_akf;
static rtkit_dev_t *ans1_rtkit;
int ans1_ep = -1;
static bool ans1_initialized, ans1_ready;

static bool ans1_epmap_cb(rtkit_dev_t *rtk, u32 base, u32 bitmap)
{
    u8 protocol = rtkit_protocol_version(rtk);
    if (!base && protocol > 10)
        return true;

    if (protocol > 10) {
        ans1_ep = 0x20;
    } else {
        if (bitmap & (1U << 6))
            ans1_ep = 6;
        else if (bitmap & (1U << 5))
            ans1_ep = 5;
    }

    if (ans1_ep == -1) {
        printf("ANS1: Unable to determine ANS endpoint\n");
        return false;
    }

    if (!rtkit_start_ep(rtk, ans1_ep)) {
        printf("ANS1: Unable to start ANS endpoint\n");
        return false;
    }

    return true;
}

static bool ans1_init_recv(rtkit_dev_t *rtk, struct rtkit_message *msg)
{
    UNUSED(rtk);

    if (msg->ep != ans1_ep) {
        printf("ANS1: Unexpected message to endpoint %x during power switch\n", msg->ep);
        return false;
    }

    u64 ready_msg = FIELD_PREP(ANS1_COMPLETION_TAG, ANS1_COMPLETION_TAG_READY) |
                    FIELD_PREP(ANS1_COMPLETION_STATUS, ANS1_COMPLETION_STATUS_INIT) |
                    FIELD_PREP(ANS1_MSG_TYPE, ANS1_MSG_TYPE_COMPLETE);

    u64 unk_msg = FIELD_PREP(ANS1_MSG_TYPE, ANS1_MSG_TYPE_UNK4);

    if (msg->msg == ready_msg) {
        ans1_ready = true;
        return true;
    } else if (msg->msg == unk_msg) {
        return true;
    }

    printf("ANS1: Unexpected ANS endpoint message 0x%lx during power switch\n", msg->msg);
    return false;
}

static bool ans1_wait_for_tag(rtkit_dev_t *rtk, u8 expected_tag)
{
    struct rtkit_message msg;
    int ret;

    u64 timeout = timeout_calculate(ANS1_TIMEOUT);

    while (!timeout_expired(timeout)) {
        if ((ret = rtkit_recv(rtk, &msg)) == 0)
            continue;

        if (ret < 0) {
            printf("ANS1: Receive message failed!\n");
            return false;
        }

        if (msg.ep != ans1_ep) {
            printf("ANS1: Unexpected message to endpoint %x\n", msg.ep);
            return false;
        }

        u8 type = FIELD_GET(ANS1_MSG_TYPE, msg.msg);
        if (type == ANS1_MSG_TYPE_UNK4)
            continue;

        if (type != ANS1_MSG_TYPE_COMPLETE) {
            printf("ANS1: Received unknown message 0x%lx on ANS endpoint\n", msg.msg);
            return false;
        }

        break;
    }

    if (timeout_expired(timeout)) {
        printf("ANS1: Timeout waiting for command completion message\n");
        return false;
    }

    u16 tag = FIELD_GET(ANS1_COMPLETION_TAG, msg.msg);
    u8 status = FIELD_GET(ANS1_COMPLETION_STATUS, msg.msg);
    u8 expected_status = 0;

    if (tag != expected_tag) {
        printf("ANS1: Invalid completion tag %d received, expected %d.\n", tag, expected_tag);
        return false;
    }

    if (tag == ANS1_COMPLETION_TAG_READY || tag == ANS1_COMPLETION_TAG_CMDBUF_READY)
        expected_status = ANS1_COMPLETION_STATUS_INIT;

    if (status != expected_status) {
        printf("ANS1: Command failed with status %d\n", status);
        return false;
    }

    return true;
}

static bool ans1_exec_command(void)
{
    struct rtkit_message rtk_msg;

    dma_wmb();

    rtk_msg.msg = FIELD_PREP(ANS1_SQ_DB_OP, ANS1_SQ_DB_OP_RING) | FIELD_PREP(ANS1_SQ_DB_TAG, 0) |
                  FIELD_PREP(ANS1_MSG_TYPE, ANS1_MSG_TYPE_SQ_DB);
    rtk_msg.ep = ans1_ep;

    if (!rtkit_send(ans1_rtkit, &rtk_msg)) {
        printf("ANS1: failed to send command\n");
        return false;
    }

    if (!ans1_wait_for_tag(ans1_rtkit, 0))
        return false;

    return true;
}

bool ans1_read_main_storage(u64 lba, void *buffer)
{
    if (!ans1_initialized)
        return false;

    if (((u64)buffer & 0xfff) != 0)
        return false;

    memset(cmd, '\0', ANS1_CMD_SIZE);

    cmd->op = ANS1_CMD_OP_READ_USERAREA;
    cmd->flags = ANS1_CMD_IO_FLAG_NO_ENCRYPTION;
    cmd->slba = lba;
    cmd->length = 1;

    struct ans1_io *io = (struct ans1_io *)(cmd + 1);
    io->sgl[0] = (u64)buffer >> 12;

    if (!ans1_exec_command())
        return false;

    dma_rmb();

    return true;
}

bool ans1_init(void)
{
    if (ans1_initialized) {
        printf("ANS1: already initialized\n");
        return false;
    }

    if (pmgr_adt_power_enable("/arm-io/ans"))
        return false;

    ans1_akf = akf_init("/arm-io/ans");
    if (!ans1_akf)
        return false;

    if (!akf_map_preloaded_fw(ans1_akf))
        return false;

    cmd = memalign(SZ_4K, ANS1_CMD_SIZE * ANS1_NUM_TAGS);
    if (!cmd) {
        printf("ANS1: queue allocation failed\n");
        return false;
    }

    ans1_rtkit =
        rtkit_init_akf("ans1", ans1_akf, NULL, NULL, NULL, false, ans1_epmap_cb, ans1_init_recv);
    if (!ans1_rtkit)
        goto out_free_cmd;

    if (!rtkit_boot(ans1_rtkit))
        goto out_rtkit;

    if (!ans1_ready && !ans1_wait_for_tag(ans1_rtkit, ANS1_COMPLETION_TAG_READY))
        goto out_shutdown;

    /* set command buffer */
    struct rtkit_message rtk_msg;
    rtk_msg.msg = FIELD_PREP(ANS1_SET_CMDBUF_BASE, (u64)cmd) |
                  FIELD_PREP(ANS1_SET_CMDBUF_SIZE_17_6, (ANS1_CMD_SIZE * ANS1_NUM_TAGS) >> 6) |
                  FIELD_PREP(ANS1_MSG_TYPE, ANS1_MSG_TYPE_SET_CMDBUF);
    rtk_msg.ep = ans1_ep;

    if (!rtkit_send(ans1_rtkit, &rtk_msg))
        goto out_shutdown;

    if (!ans1_wait_for_tag(ans1_rtkit, ANS1_COMPLETION_TAG_CMDBUF_READY))
        goto out_shutdown;

    for (unsigned int i = 0; i < ANS1_NUM_TAGS; i++) {
        rtk_msg.msg = FIELD_PREP(ANS1_SET_TAG_CMDBUF_TAG, i) |
                      FIELD_PREP(ANS1_SET_TAG_CMDBUF_OFF_17_6, (i * ANS1_CMD_SIZE) >> 6) |
                      FIELD_PREP(ANS1_SET_TAG_CMDBUF_SIZE_17_6, ANS1_CMD_SIZE >> 6) |
                      FIELD_PREP(ANS1_MSG_TYPE, ANS1_MSG_TYPE_SET_TAG_CMDBUF);

        rtk_msg.ep = ans1_ep;
        if (!rtkit_send(ans1_rtkit, &rtk_msg))
            goto out_shutdown;
    }

    ans1_initialized = true;
    printf("ANS1: Initialized\n");

    return true;

out_shutdown:
    rtkit_sleep(ans1_rtkit);
out_rtkit:
    rtkit_free(ans1_rtkit);
out_free_cmd:
    free(cmd);

    return false;
}

void ans1_shutdown(void)
{
    if (!ans1_initialized)
        return;

    // For some reason rtkit_quiesce() would cause crash when startup again
    rtkit_sleep(ans1_rtkit);
    rtkit_free(ans1_rtkit);
    free(cmd);
    free(ans1_akf);
    free(cmd);

    pmgr_adt_power_disable("/arm-io/ans");

    ans1_initialized = false;
    ans1_ready = false;
    printf("ANS1: shutdown done\n");
}
