/* SPDX-License-Identifier: MIT */
#include "adt.h"
#include "akf.h"
#include "ans1.h"
#include "assert.h"
#include "malloc.h"
#include "pmgr.h"
#include "rtkit.h"
#include "soc.h"
#include "string.h"
#include "types.h"
#include "utils.h"

#define ANS1_CMD_SIZE     2240
#define ANS1_MAX_IO_PAGES 512
#define ANS1_NUM_TAGS     8

/* ANS1 commands */
#define ANS1_CMD_OP_IDENTIFY 0x0

#define ANS1_CMD_OP_READ_MAIN  0x10
#define ANS1_CMD_OP_WRITE_MAIN 0x11

/* Destructive Commands */
/*
 * This format the whole NAND. "Format_Util" from syslog.
 * Don't run it! Syscfg is not recoverable with DFU restore.
 * This will also reset all auxiliary storages to "unwritten"
 */
#define ANS1_CMD_OP_FORMAT_UTIL 0x15
/*
 * This may mean 'Format main storage' from syslog wording but I am
 * not going to destroy my NAND again just to test what these commands
 * that won't be used does. (Ran it once when the NAND is already
 * destoryed after running 0x15, so it is not known for sure)
 */
#define ANS1_CMD_OP_FORMAT_LBA 0x16
/*
 * Syslog claims this one is "Format_Neuralize", but considering that after
 * completing this command ANS1 will intentionally crash, this is probably
 * meant to be "Neutralize" (Neutralize/"Brick" the device maybe?).
 */
#define ANS1_CMD_OP_FORMAT_NEUTRALIZE 0x18

#define ANS1_CMD_OP_WRITE_UNLOCK 0x19

/* These returns no error */
#define ANS1_CMD_OP_UNK1A 0x1a
#define ANS1_CMD_OP_UNK1B 0x1b
#define ANS1_CMD_OP_UNK1C 0x1c
#define ANS1_CMD_OP_UNK1D 0x1d
#define ANS1_CMD_OP_UNK29 0x29
#define ANS1_CMD_OP_UNK2B 0x2b

/*
 * Auxiliary storage
 * Depending on which one, there are restrictions on the starting lba,
 * and it seems that reading "unwritten" (according to syslog) would
 * return an error.
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

#define ANS1_CMD_OP_IDENTIFY2    0x72
#define ANS1_CMD_OP_POWER_CONFIG 0x80

/* mailbox messages */
#define ANS1_MSG_TYPE                GENMASK(1, 0)
#define ANS1_MSG_TYPE_SET_CMDBUF     0
#define ANS1_MSG_TYPE_SET_TAG_CMDBUF 1
#define ANS1_MSG_TYPE_SQ_DB          3

#define ANS1_SET_CMDBUF_BASE      GENMASK(55, 16)
#define ANS1_SET_CMDBUF_SIZE_17_4 GENMASK(15, 2)

#define ANS1_SET_TAG_CMDBUF_OFF_13_2  GENMASK(19, 8)
#define ANS1_SET_TAG_CMDBUF_SIZE_13_2 GENMASK(31, 20)
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
#define ANS1_COMPLETION_STATUS      GENMASK(15, 12)
#define ANS1_COMPLETION_STATUS_OK   0
#define ANS1_COMPLETION_STATUS_INIT 0xa /* Seen with the special tags */

#define ANS1_COMPLETION_TAG_READY        254
#define ANS1_COMPLETION_TAG_CMDBUF_READY 253

#define ANS1_COMPLETION_TAG      GENMASK(11, 4)
#define ANS1_REPLY_TYPE          GENMASK(3, 0)
#define ANS1_REPLY_TYPE_COMPLETE 2 /* with completion tag */
#define ANS1_REPLY_TYPE_UNK4     4 /* no completion tag */

/*
 * Without this flag the operation would still take place but instead of
 * the intended data garbage corresponding to the input data in 128-bit
 * block sizes would be used, so likely encryption.
 */
#define ANS1_CMD_IO_FLAG_NO_ENCRYPTION BIT(3)

struct ans1_command {
    u8 op;
    u8 tag;
    union {
        struct {
            u16 flags;
            u32 lba_off;
            u32 num_lba;
            u8 pad0[0x24];
            u32 buf_pages[ANS1_MAX_IO_PAGES];
        } __attribute__((packed)) io;
        struct {
            u8 pad0[0x2e];
            u32 num_lba;
            u32 lba_size;
        } __attribute__((packed)) identify;
        struct {
            u16 pad;
            u32 cdw[523];
        } __attribute__((packed)) common;
    } __attribute__((packed));
    u32 pad1[36];
} __attribute__((packed));

static_assert(sizeof(struct ans1_command) == ANS1_CMD_SIZE, "wrong ans1 command size");

static struct ans1_command *ans1_queue, *cmd;
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
                    FIELD_PREP(ANS1_REPLY_TYPE, ANS1_REPLY_TYPE_COMPLETE);

    u64 unk_msg = FIELD_PREP(ANS1_REPLY_TYPE, ANS1_REPLY_TYPE_UNK4);

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

    u64 timeout = timeout_calculate(1000000);

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

        u8 type = FIELD_GET(ANS1_REPLY_TYPE, msg.msg);
        if (type == ANS1_REPLY_TYPE_UNK4)
            continue;

        if (type != ANS1_REPLY_TYPE_COMPLETE) {
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

static bool ans1_exec_command(struct ans1_command *submit_cmd)
{
    struct rtkit_message rtk_msg;

    memcpy(ans1_queue, submit_cmd, ANS1_CMD_SIZE);
    ans1_queue->tag = 0;

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

    cmd->op = ANS1_CMD_OP_READ_MAIN;
    cmd->io.flags = ANS1_CMD_IO_FLAG_NO_ENCRYPTION;
    cmd->io.num_lba = 1;
    cmd->io.lba_off = lba;
    cmd->io.buf_pages[0] = (u64)buffer >> 12;

    if (!ans1_exec_command(cmd))
        return false;

    dma_rmb();

    return true;
}

static bool ans1_apply_tunables(void)
{
    /* No idea about the details of these commands, so use common */

    memset(cmd, '\0', ANS1_CMD_SIZE);
    cmd->op = ANS1_CMD_OP_POWER_CONFIG;
    cmd->common.cdw[13] = 0x2a;
    cmd->common.cdw[16] = 0x400000;
    cmd->common.cdw[17] = 0x400000;

    if (!ans1_exec_command(cmd))
        return false;

    memset(cmd, '\0', ANS1_CMD_SIZE);
    cmd->op = ANS1_CMD_OP_POWER_CONFIG;
    cmd->common.cdw[13] = 0x13;
    cmd->common.cdw[14] = 0x3;
    cmd->common.cdw[16] = 0x2;
    cmd->common.cdw[17] = 0x2;
    cmd->common.cdw[18] = 0x4;
    cmd->common.cdw[19] = 0x2;
    cmd->common.cdw[20] = 0x4;
    cmd->common.cdw[21] = 0x4;
    cmd->common.cdw[22] = 0x2;
    cmd->common.cdw[23] = 0x2;

    if (!ans1_exec_command(cmd))
        return false;

    memset(cmd, '\0', ANS1_CMD_SIZE);
    cmd->op = ANS1_CMD_OP_POWER_CONFIG;
    cmd->common.cdw[13] = 0x25;
    cmd->common.cdw[14] = 0x3;
    cmd->common.cdw[16] = 0x1;
    cmd->common.cdw[17] = 0x1;
    cmd->common.cdw[18] = 0x1;
    cmd->common.cdw[19] = 0x1;
    cmd->common.cdw[20] = 0x1;
    cmd->common.cdw[21] = 0x1;
    cmd->common.cdw[22] = 0x1;
    cmd->common.cdw[23] = 0x1;

    if (!ans1_exec_command(cmd))
        return false;

    /* This command sets the coprocessor to "high power mode" */
    memset(cmd, '\0', ANS1_CMD_SIZE);
    cmd->op = ANS1_CMD_OP_POWER_CONFIG;
    cmd->common.cdw[13] = 0x26;
    cmd->common.cdw[16] = 0x1;

    if (!ans1_exec_command(cmd))
        return false;

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

    ans1_queue = memalign(SZ_4K, ANS1_CMD_SIZE * ANS1_NUM_TAGS);
    if (!ans1_queue) {
        printf("ANS1: queue allocation failed\n");
        return false;
    }

    cmd = malloc(ANS1_CMD_SIZE * ANS1_NUM_TAGS);
    if (!cmd) {
        printf("ANS1: command allocation failed\n");
        goto out_free_queue;
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
    rtk_msg.msg = FIELD_PREP(ANS1_SET_CMDBUF_BASE, (u64)ans1_queue) |
                  FIELD_PREP(ANS1_SET_CMDBUF_SIZE_17_4, (ANS1_CMD_SIZE * ANS1_NUM_TAGS) >> 4) |
                  FIELD_PREP(ANS1_MSG_TYPE, ANS1_MSG_TYPE_SET_CMDBUF);
    rtk_msg.ep = ans1_ep;

    if (!rtkit_send(ans1_rtkit, &rtk_msg))
        goto out_shutdown;

    if (!ans1_wait_for_tag(ans1_rtkit, ANS1_COMPLETION_TAG_CMDBUF_READY))
        goto out_shutdown;

    for (unsigned int i = 0; i < ANS1_NUM_TAGS; i++) {
        rtk_msg.msg = FIELD_PREP(ANS1_SET_TAG_CMDBUF_TAG, i) |
                      FIELD_PREP(ANS1_SET_TAG_CMDBUF_OFF_13_2, (i * ANS1_CMD_SIZE) >> 2) |
                      FIELD_PREP(ANS1_SET_TAG_CMDBUF_SIZE_13_2, ANS1_CMD_SIZE >> 2) |
                      FIELD_PREP(ANS1_MSG_TYPE, ANS1_MSG_TYPE_SET_TAG_CMDBUF);

        rtk_msg.ep = ans1_ep;
        if (!rtkit_send(ans1_rtkit, &rtk_msg))
            goto out_shutdown;
    }

    /*
     * XXX how to handle A7 properly? Ideally should trace iboot but
     * iboot_tracer does not support iOS 12.
     */
    if (chip_id != S5L8960X && !ans1_apply_tunables())
        goto out_shutdown;

    ans1_initialized = true;
    printf("ANS1: Initialized\n");

    return true;

out_shutdown:
    rtkit_sleep(ans1_rtkit);
out_rtkit:
    rtkit_free(ans1_rtkit);
out_free_queue:
    free(ans1_queue);
out_free_cmd:
    free(cmd);

    return false;
}

void ans1_shutdown(void)
{
    if (!ans1_initialized)
        return;

    // For some reason rtkit_quiesce() would cause crash when startup again
    // iPhone 5s iOS 10.3.3's rtkit firmware would ack power state 0x0 when
    // ap tries to go into state 0x10 (rebooting still works)
    rtkit_sleep_workaround(ans1_rtkit);
    rtkit_free(ans1_rtkit);
    free(ans1_queue);
    free(ans1_akf);
    free(cmd);

    pmgr_adt_power_disable("/arm-io/ans");

    ans1_initialized = false;
    ans1_ready = false;
    printf("ANS1: shutdown done\n");
}
