/*
 * Copyright (c) 2010 Broadcom Corporation
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
/* ****************** SDIO CARD Interface Functions **************************/

#include <sync/completion.h>

#include <stdatomic.h>

#include "brcm_hw_ids.h"
#include "brcmu_utils.h"
#include "brcmu_wifi.h"
#include "bus.h"
#include "chip.h"
#include "chipcommon.h"
#include "common.h"
#include "core.h"
#include "debug.h"
#include "defs.h"
#include "device.h"
#include "linuxisms.h"
#include "netbuf.h"
#include "sdio.h"
#include "soc.h"

#define SDIOH_API_ACCESS_RETRY_LIMIT 2

#define DMA_ALIGN_MASK 0x03

#define SDIO_FUNC1_BLOCKSIZE 64
#define SDIO_FUNC2_BLOCKSIZE 512
/* Maximum milliseconds to wait for F2 to come up */
#define SDIO_WAIT_F2RDY 3000

#define BRCMF_DEFAULT_RXGLOM_SIZE 32 /* max rx frames in glom chain */

struct brcmf_sdiod_freezer {
    atomic_int freezing;
    atomic_int thread_count;
    uint32_t frozen_count;
    completion_t thread_freeze;
    completion_t resumed;
};

static irqreturn_t brcmf_sdiod_oob_irqhandler(int irq, void* dev_id) {
    struct brcmf_bus* bus_if = dev_get_drvdata(dev_id);
    struct brcmf_sdio_dev* sdiodev = bus_if->bus_priv.sdio;

    brcmf_dbg(INTR, "OOB intr triggered\n");

    /* out-of-band interrupt is level-triggered which won't
     * be cleared until dpc
     */
    if (sdiodev->irq_en) {
        disable_irq_nosync(irq);
        sdiodev->irq_en = false;
    }

    brcmf_sdio_isr(sdiodev->bus);

    return IRQ_HANDLED;
}

static void brcmf_sdiod_ib_irqhandler(struct sdio_func* func) {
    struct brcmf_bus* bus_if = dev_get_drvdata(&func->dev);
    struct brcmf_sdio_dev* sdiodev = bus_if->bus_priv.sdio;

    brcmf_dbg(INTR, "IB intr triggered\n");

    brcmf_sdio_isr(sdiodev->bus);
}

/* dummy handler for SDIO function 2 interrupt */
static void brcmf_sdiod_dummy_irqhandler(struct sdio_func* func) {}

zx_status_t brcmf_sdiod_intr_register(struct brcmf_sdio_dev* sdiodev) {
    struct brcmfmac_sdio_pd* pdata;
    zx_status_t ret = ZX_OK;
    uint8_t data;
    uint32_t addr, gpiocontrol;

    pdata = &sdiodev->settings->bus.sdio;
    if (pdata->oob_irq_supported) {
        brcmf_dbg(SDIO, "Enter, register OOB IRQ %d\n", pdata->oob_irq_nr);
        //spin_lock_init(&sdiodev->irq_en_lock);
        sdiodev->irq_en = true;

        ret = request_irq(pdata->oob_irq_nr, brcmf_sdiod_oob_irqhandler, pdata->oob_irq_flags,
                          "brcmf_oob_intr", &sdiodev->func1->dev);
        if (ret != ZX_OK) {
            brcmf_err("request_irq failed %d\n", ret);
            return ret;
        }
        sdiodev->oob_irq_requested = true;

        ret = enable_irq_wake(pdata->oob_irq_nr);
        if (ret != ZX_OK) {
            brcmf_err("enable_irq_wake failed %d\n", ret);
            return ret;
        }
        sdiodev->irq_wake = true;

        sdio_claim_host(sdiodev->func1);

        if (sdiodev->bus_if->chip == BRCM_CC_43362_CHIP_ID) {
            /* assign GPIO to SDIO core */
            addr = CORE_CC_REG(SI_ENUM_BASE, gpiocontrol);
            gpiocontrol = brcmf_sdiod_readl(sdiodev, addr, &ret);
            gpiocontrol |= 0x2;
            brcmf_sdiod_writel(sdiodev, addr, gpiocontrol, &ret);

            brcmf_sdiod_writeb(sdiodev, SBSDIO_GPIO_SELECT, 0xf, &ret);
            brcmf_sdiod_writeb(sdiodev, SBSDIO_GPIO_OUT, 0, &ret);
            brcmf_sdiod_writeb(sdiodev, SBSDIO_GPIO_EN, 0x2, &ret);
        }

        /* must configure SDIO_CCCR_IENx to enable irq */
        data = brcmf_sdiod_func0_rb(sdiodev, SDIO_CCCR_IENx, &ret);
        data |= SDIO_CCCR_IEN_FUNC1 | SDIO_CCCR_IEN_FUNC2 | SDIO_CCCR_IEN_FUNC0;
        brcmf_sdiod_func0_wb(sdiodev, SDIO_CCCR_IENx, data, &ret);

        /* redirect, configure and enable io for interrupt signal */
        data = SDIO_CCCR_BRCM_SEPINT_MASK | SDIO_CCCR_BRCM_SEPINT_OE;
        if (pdata->oob_irq_flags & IRQF_TRIGGER_HIGH) {
            data |= SDIO_CCCR_BRCM_SEPINT_ACT_HI;
        }
        brcmf_sdiod_func0_wb(sdiodev, SDIO_CCCR_BRCM_SEPINT, data, &ret);
        sdio_release_host(sdiodev->func1);
    } else {
        brcmf_dbg(SDIO, "Entering\n");
        sdio_claim_host(sdiodev->func1);
        sdio_claim_irq(sdiodev->func1, brcmf_sdiod_ib_irqhandler);
        sdio_claim_irq(sdiodev->func2, brcmf_sdiod_dummy_irqhandler);
        sdio_release_host(sdiodev->func1);
        sdiodev->sd_irq_requested = true;
    }

    return ZX_OK;
}

void brcmf_sdiod_intr_unregister(struct brcmf_sdio_dev* sdiodev) {
    brcmf_dbg(SDIO, "Entering oob=%d sd=%d\n", sdiodev->oob_irq_requested,
              sdiodev->sd_irq_requested);

    if (sdiodev->oob_irq_requested) {
        struct brcmfmac_sdio_pd* pdata;

        pdata = &sdiodev->settings->bus.sdio;
        sdio_claim_host(sdiodev->func1);
        brcmf_sdiod_func0_wb(sdiodev, SDIO_CCCR_BRCM_SEPINT, 0, NULL);
        brcmf_sdiod_func0_wb(sdiodev, SDIO_CCCR_IENx, 0, NULL);
        sdio_release_host(sdiodev->func1);

        sdiodev->oob_irq_requested = false;
        if (sdiodev->irq_wake) {
            disable_irq_wake(pdata->oob_irq_nr);
            sdiodev->irq_wake = false;
        }
        free_irq(pdata->oob_irq_nr, &sdiodev->func1->dev);
        sdiodev->irq_en = false;
        sdiodev->oob_irq_requested = false;
    }

    if (sdiodev->sd_irq_requested) {
        sdio_claim_host(sdiodev->func1);
        sdio_release_irq(sdiodev->func2);
        sdio_release_irq(sdiodev->func1);
        sdio_release_host(sdiodev->func1);
        sdiodev->sd_irq_requested = false;
    }
}

void brcmf_sdiod_change_state(struct brcmf_sdio_dev* sdiodev, enum brcmf_sdiod_state state) {
    if (sdiodev->state == BRCMF_SDIOD_NOMEDIUM || state == sdiodev->state) {
        return;
    }

    brcmf_dbg(TRACE, "%d -> %d\n", sdiodev->state, state);
    switch (sdiodev->state) {
    case BRCMF_SDIOD_DATA:
        /* any other state means bus interface is down */
        brcmf_bus_change_state(sdiodev->bus_if, BRCMF_BUS_DOWN);
        break;
    case BRCMF_SDIOD_DOWN:
        /* transition from DOWN to DATA means bus interface is up */
        if (state == BRCMF_SDIOD_DATA) {
            brcmf_bus_change_state(sdiodev->bus_if, BRCMF_BUS_UP);
        }
        break;
    default:
        break;
    }
    sdiodev->state = state;
}

static zx_status_t brcmf_sdiod_set_backplane_window(struct brcmf_sdio_dev* sdiodev, uint32_t addr) {
    uint32_t v;
    uint32_t bar0 = addr & SBSDIO_SBWINDOW_MASK;
    zx_status_t err = ZX_OK;
    int i;

    if (bar0 == sdiodev->sbwad) {
        return ZX_OK;
    }

    v = bar0 >> 8;

    for (i = 0; i < 3 && err == ZX_OK; i++, v >>= 8) {
        brcmf_sdiod_writeb(sdiodev, SBSDIO_FUNC1_SBADDRLOW + i, v & 0xff, &err);
    }

    if (err == ZX_OK) {
        sdiodev->sbwad = bar0;
    }

    return err;
}

uint32_t brcmf_sdiod_readl(struct brcmf_sdio_dev* sdiodev, uint32_t addr, zx_status_t* ret) {
    uint32_t data = 0;
    zx_status_t retval;

    retval = brcmf_sdiod_set_backplane_window(sdiodev, addr);
    if (retval != ZX_OK) {
        goto out;
    }

    addr &= SBSDIO_SB_OFT_ADDR_MASK;
    addr |= SBSDIO_SB_ACCESS_2_4B_FLAG;

    data = sdio_readl(sdiodev->func1, addr, &retval);

out:
    if (ret) {
        *ret = retval;
    }

    return data;
}

void brcmf_sdiod_writel(struct brcmf_sdio_dev* sdiodev, uint32_t addr, uint32_t data,
                        zx_status_t* ret) {
    zx_status_t retval;

    retval = brcmf_sdiod_set_backplane_window(sdiodev, addr);
    if (retval != ZX_OK) {
        goto out;
    }

    addr &= SBSDIO_SB_OFT_ADDR_MASK;
    addr |= SBSDIO_SB_ACCESS_2_4B_FLAG;

    sdio_writel(sdiodev->func1, data, addr, &retval);

out:
    if (ret) {
        *ret = retval;
    }
}

static zx_status_t brcmf_sdiod_netbuf_read(struct brcmf_sdio_dev* sdiodev, struct sdio_func* func,
                                           uint32_t addr, struct brcmf_netbuf* netbuf) {
    unsigned int req_sz;
    zx_status_t err;

    /* Single netbuf use the standard mmc interface */
    req_sz = netbuf->len + 3;
    req_sz &= (uint)~3;

    switch (func->num) {
    case 1:
        err = sdio_memcpy_fromio(func, ((uint8_t*)(netbuf->data)), addr, req_sz);
        break;
    case 2:
        err = sdio_readsb(func, ((uint8_t*)(netbuf->data)), addr, req_sz);
        break;
    default:
        /* bail out as things are really fishy here */
        WARN(1, "invalid sdio function number\n"); // TODO(cphoenix): %d\n", func->num);
        err = ZX_ERR_IO_REFUSED;
    };

    if (err == ZX_ERR_IO_REFUSED) {
        brcmf_sdiod_change_state(sdiodev, BRCMF_SDIOD_NOMEDIUM);
    }

    return err;
}

static zx_status_t brcmf_sdiod_netbuf_write(struct brcmf_sdio_dev* sdiodev, struct sdio_func* func,
                                            uint32_t addr, struct brcmf_netbuf* netbuf) {
    unsigned int req_sz;
    zx_status_t err;

    /* Single netbuf use the standard mmc interface */
    req_sz = netbuf->len + 3;
    req_sz &= (uint)~3;

    err = sdio_memcpy_toio(func, addr, ((uint8_t*)(netbuf->data)), req_sz);

    if (err == ZX_ERR_IO_REFUSED) {
        brcmf_sdiod_change_state(sdiodev, BRCMF_SDIOD_NOMEDIUM);
    }

    return err;
}

zx_status_t brcmf_sdiod_recv_buf(struct brcmf_sdio_dev* sdiodev, uint8_t* buf, uint nbytes) {
    struct brcmf_netbuf* mypkt;
    zx_status_t err;

    mypkt = brcmu_pkt_buf_get_netbuf(nbytes);
    if (!mypkt) {
        brcmf_err("brcmu_pkt_buf_get_netbuf failed: len %d\n", nbytes);
        return ZX_ERR_NO_MEMORY;
    }

    err = brcmf_sdiod_recv_pkt(sdiodev, mypkt);
    if (err == ZX_OK) {
        memcpy(buf, mypkt->data, nbytes);
    }

    brcmu_pkt_buf_free_netbuf(mypkt);
    return err;
}

zx_status_t brcmf_sdiod_recv_pkt(struct brcmf_sdio_dev* sdiodev, struct brcmf_netbuf* pkt) {
    uint32_t addr = sdiodev->cc_core->base;
    zx_status_t err = ZX_OK;

    brcmf_dbg(SDIO, "addr = 0x%x, size = %d\n", addr, pkt->len);

    err = brcmf_sdiod_set_backplane_window(sdiodev, addr);
    if (err != ZX_OK) {
        goto done;
    }

    addr &= SBSDIO_SB_OFT_ADDR_MASK;
    addr |= SBSDIO_SB_ACCESS_2_4B_FLAG;

    err = brcmf_sdiod_netbuf_read(sdiodev, sdiodev->func2, addr, pkt);

done:
    return err;
}

zx_status_t brcmf_sdiod_recv_chain(struct brcmf_sdio_dev* sdiodev, struct brcmf_netbuf_list* pktq,
                                   uint totlen) {
    struct brcmf_netbuf* glom_netbuf = NULL;
    struct brcmf_netbuf* netbuf;
    uint32_t addr = sdiodev->cc_core->base;
    zx_status_t err = ZX_OK;

    brcmf_dbg(SDIO, "addr = 0x%x, size = %d\n", addr, pktq->qlen);

    err = brcmf_sdiod_set_backplane_window(sdiodev, addr);
    if (err != ZX_OK) {
        goto done;
    }

    addr &= SBSDIO_SB_OFT_ADDR_MASK;
    addr |= SBSDIO_SB_ACCESS_2_4B_FLAG;

    if (pktq->qlen == 1) {
        err = brcmf_sdiod_netbuf_read(sdiodev, sdiodev->func2, addr, pktq->next);
    } else {
        glom_netbuf = brcmu_pkt_buf_get_netbuf(totlen);
        if (!glom_netbuf) {
            return ZX_ERR_NO_MEMORY;
        }
        err = brcmf_sdiod_netbuf_read(sdiodev, sdiodev->func2, addr, glom_netbuf);
        if (err != ZX_OK) {
            goto done;
        }

        brcmf_netbuf_list_for_every(pktq, netbuf) {
            memcpy(netbuf->data, glom_netbuf->data, netbuf->len);
            brcmf_netbuf_shrink_head(glom_netbuf, netbuf->len);
        }
    }

done:
    brcmu_pkt_buf_free_netbuf(glom_netbuf);
    return err;
}

zx_status_t brcmf_sdiod_send_buf(struct brcmf_sdio_dev* sdiodev, uint8_t* buf, uint nbytes) {
    struct brcmf_netbuf* mypkt;
    uint32_t addr = sdiodev->cc_core->base;
    zx_status_t err;

    mypkt = brcmu_pkt_buf_get_netbuf(nbytes);

    if (!mypkt) {
        brcmf_err("brcmu_pkt_buf_get_netbuf failed: len %d\n", nbytes);
        return ZX_ERR_IO;
    }

    memcpy(mypkt->data, buf, nbytes);

    err = brcmf_sdiod_set_backplane_window(sdiodev, addr);
    if (err != ZX_OK) {
        return err;
    }

    addr &= SBSDIO_SB_OFT_ADDR_MASK;
    addr |= SBSDIO_SB_ACCESS_2_4B_FLAG;

    if (err == ZX_OK) {
        err = brcmf_sdiod_netbuf_write(sdiodev, sdiodev->func2, addr, mypkt);
    }

    brcmu_pkt_buf_free_netbuf(mypkt);

    return err;
}

zx_status_t brcmf_sdiod_send_pkt(struct brcmf_sdio_dev* sdiodev, struct brcmf_netbuf_list* pktq) {
    struct brcmf_netbuf* netbuf;
    uint32_t addr = sdiodev->cc_core->base;
    zx_status_t err;

    brcmf_dbg(SDIO, "addr = 0x%x, size = %d\n", addr, pktq->qlen);

    err = brcmf_sdiod_set_backplane_window(sdiodev, addr);
    if (err != ZX_OK) {
        return err;
    }

    addr &= SBSDIO_SB_OFT_ADDR_MASK;
    addr |= SBSDIO_SB_ACCESS_2_4B_FLAG;

    brcmf_netbuf_list_for_every(pktq, netbuf) {
        err = brcmf_sdiod_netbuf_write(sdiodev, sdiodev->func2, addr, netbuf);
        if (err != ZX_OK) {
            break;
        }
    }

    return err;
}

zx_status_t brcmf_sdiod_ramrw(struct brcmf_sdio_dev* sdiodev, bool write, uint32_t address,
                              uint8_t* data, uint size) {
    zx_status_t err = ZX_OK;
    struct brcmf_netbuf* pkt;
    uint32_t sdaddr;
    uint dsize;

    dsize = min_t(uint, SBSDIO_SB_OFT_ADDR_LIMIT, size);
    pkt = brcmf_netbuf_allocate(dsize);
    if (!pkt) {
        brcmf_err("brcmf_netbuf_allocate failed: len %d\n", dsize);
        return ZX_ERR_IO;
    }
    pkt->priority = 0;

    /* Determine initial transfer parameters */
    sdaddr = address & SBSDIO_SB_OFT_ADDR_MASK;
    if ((sdaddr + size) & SBSDIO_SBWINDOW_MASK) {
        dsize = (SBSDIO_SB_OFT_ADDR_LIMIT - sdaddr);
    } else {
        dsize = size;
    }

    sdio_claim_host(sdiodev->func1);

    /* Do the transfer(s) */
    while (size) {
        /* Set the backplane window to include the start address */
        err = brcmf_sdiod_set_backplane_window(sdiodev, address);
        if (err != ZX_OK) {
            break;
        }

        brcmf_dbg(SDIO, "%s %d bytes at offset 0x%08x in window 0x%08x\n", write ? "write" : "read",
                  dsize, sdaddr, address & SBSDIO_SBWINDOW_MASK);

        sdaddr &= SBSDIO_SB_OFT_ADDR_MASK;
        sdaddr |= SBSDIO_SB_ACCESS_2_4B_FLAG;

        brcmf_netbuf_grow_tail(pkt, dsize);

        if (write) {
            memcpy(pkt->data, data, dsize);
            err = brcmf_sdiod_netbuf_write(sdiodev, sdiodev->func1, sdaddr, pkt);
        } else {
            err = brcmf_sdiod_netbuf_read(sdiodev, sdiodev->func1, sdaddr, pkt);
        }

        if (err != ZX_OK) {
            brcmf_err("membytes transfer failed\n");
            break;
        }
        if (!write) {
            memcpy(data, pkt->data, dsize);
        }
        brcmf_netbuf_reduce_length_to(pkt, 0);

        /* Adjust for next transfer (if any) */
        size -= dsize;
        if (size) {
            data += dsize;
            address += dsize;
            sdaddr = 0;
            dsize = min_t(uint, SBSDIO_SB_OFT_ADDR_LIMIT, size);
        }
    }

    brcmf_netbuf_free(pkt);

    sdio_release_host(sdiodev->func1);

    return err;
}

zx_status_t brcmf_sdiod_abort(struct brcmf_sdio_dev* sdiodev, struct sdio_func* func) {
    brcmf_dbg(SDIO, "Enter\n");

    /* Issue abort cmd52 command through F0 */
    brcmf_sdiod_func0_wb(sdiodev, SDIO_CCCR_ABORT, func->num, NULL);

    brcmf_dbg(SDIO, "Exit\n");
    return ZX_OK;
}

#ifdef CONFIG_PM_SLEEP
static zx_status_t brcmf_sdiod_freezer_attach(struct brcmf_sdio_dev* sdiodev) {
    sdiodev->freezer = calloc(1, sizeof(*sdiodev->freezer));
    if (!sdiodev->freezer) {
        return ZX_ERR_NO_MEMORY;
    }
    atomic_store(&sdiodev->freezer->thread_count, 0);
    atomic_store(&sdiodev->freezer->freezing, 0);
    sdiodev->freezer->thread_freeze = COMPLETION_INIT;
    sdiodev->freezer->resumed = COMPLETION_INIT;
    return ZX_OK;
}

static void brcmf_sdiod_freezer_detach(struct brcmf_sdio_dev* sdiodev) {
    if (sdiodev->freezer) {
        WARN_ON(atomic_load(&sdiodev->freezer->freezing));
        free(sdiodev->freezer);
    }
}

static zx_status_t brcmf_sdiod_freezer_on(struct brcmf_sdio_dev* sdiodev) {
    zx_status_t res = ZX_OK;

    sdiodev->freezer->frozen_count = 0;
    completion_reset(&sdiodev->freezer->resumed);
    completion_reset(&sdiodev->freezer->thread_freeze);
    atomic_store(&sdiodev->freezer->freezing, 1);
    brcmf_sdio_trigger_dpc(sdiodev->bus);
    completion_wait(&sdiodev->freezer->thread_freeze, ZX_TIME_INFINITE);
    sdio_claim_host(sdiodev->func1);
    res = brcmf_sdio_sleep(sdiodev->bus, true);
    sdio_release_host(sdiodev->func1);
    return res;
}

static void brcmf_sdiod_freezer_off(struct brcmf_sdio_dev* sdiodev) {
    sdio_claim_host(sdiodev->func1);
    brcmf_sdio_sleep(sdiodev->bus, false);
    sdio_release_host(sdiodev->func1);
    atomic_store(&sdiodev->freezer->freezing, 0);
    completion_signal(&sdiodev->freezer->resumed);
}

bool brcmf_sdiod_freezing(struct brcmf_sdio_dev* sdiodev) {
    return atomic_load(&sdiodev->freezer->freezing);
}

void brcmf_sdiod_try_freeze(struct brcmf_sdio_dev* sdiodev) {
    if (!brcmf_sdiod_freezing(sdiodev)) {
        return;
    }
    sdiodev->freezer->frozen_count++;
    if (atomic_load(&sdiodev->freezer->thread_count) == sdiodev->freezer->frozen_count) {
        completion_signal(&sdiodev->freezer->thread_freeze);
    }
    completion_wait(&sdiodev->freezer->resumed, ZX_TIME_INFINITE);
}

void brcmf_sdiod_freezer_count(struct brcmf_sdio_dev* sdiodev) {
    atomic_fetch_add(&sdiodev->freezer->thread_count, 1);
}

void brcmf_sdiod_freezer_uncount(struct brcmf_sdio_dev* sdiodev) {
    atomic_fetch_sub(&sdiodev->freezer->thread_count, 1);
}
#else
static zx_status_t brcmf_sdiod_freezer_attach(struct brcmf_sdio_dev* sdiodev) {
    return ZX_OK;
}

static void brcmf_sdiod_freezer_detach(struct brcmf_sdio_dev* sdiodev) {}
#endif /* CONFIG_PM_SLEEP */

static zx_status_t brcmf_sdiod_remove(struct brcmf_sdio_dev* sdiodev) {
    sdiodev->state = BRCMF_SDIOD_DOWN;
    if (sdiodev->bus) {
        brcmf_sdio_remove(sdiodev->bus);
        sdiodev->bus = NULL;
    }

    brcmf_sdiod_freezer_detach(sdiodev);

    /* Disable Function 2 */
    sdio_claim_host(sdiodev->func2);
    sdio_disable_func(sdiodev->func2);
    sdio_release_host(sdiodev->func2);

    /* Disable Function 1 */
    sdio_claim_host(sdiodev->func1);
    sdio_disable_func(sdiodev->func1);
    sdio_release_host(sdiodev->func1);

    sdiodev->sbwad = 0;

    pm_runtime_allow(sdiodev->func1->card->host->parent);
    return ZX_OK;
}

static void brcmf_sdiod_host_fixup(struct mmc_host* host) {
    /* runtime-pm powers off the device */
    pm_runtime_forbid(host->parent);
    /* avoid removal detection upon resume */
    host->caps |= MMC_CAP_NONREMOVABLE;
}

static zx_status_t brcmf_sdiod_probe(struct brcmf_sdio_dev* sdiodev) {
    zx_status_t ret = ZX_OK;

    sdio_claim_host(sdiodev->func1);

    ret = sdio_set_block_size(sdiodev->func1, SDIO_FUNC1_BLOCKSIZE);
    if (ret != ZX_OK) {
        brcmf_err("Failed to set F1 blocksize\n");
        sdio_release_host(sdiodev->func1);
        goto out;
    }
    ret = sdio_set_block_size(sdiodev->func2, SDIO_FUNC2_BLOCKSIZE);
    if (ret != ZX_OK) {
        brcmf_err("Failed to set F2 blocksize\n");
        sdio_release_host(sdiodev->func1);
        goto out;
    }

    /* increase F2 timeout */
    sdiodev->func2->enable_timeout = SDIO_WAIT_F2RDY;

    /* Enable Function 1 */
    ret = sdio_enable_func(sdiodev->func1);
    sdio_release_host(sdiodev->func1);
    if (ret != ZX_OK) {
        brcmf_err("Failed to enable F1: err=%d\n", ret);
        goto out;
    }

    ret = brcmf_sdiod_freezer_attach(sdiodev);
    if (ret != ZX_OK) {
        goto out;
    }

    /* try to attach to the target device */
    sdiodev->bus = brcmf_sdio_probe(sdiodev);
    if (!sdiodev->bus) {
        ret = ZX_ERR_IO_NOT_PRESENT;
        goto out;
    }
    brcmf_sdiod_host_fixup(sdiodev->func2->card->host);
out:
    if (ret != ZX_OK) {
        brcmf_sdiod_remove(sdiodev);
    }

    return ret;
}

#define BRCMF_SDIO_DEVICE(dev_id) \
    { SDIO_DEVICE(SDIO_VENDOR_ID_BROADCOM, dev_id) }

/* devices we support, null terminated */
static const struct sdio_device_id brcmf_sdmmc_ids[] = {
    BRCMF_SDIO_DEVICE(SDIO_DEVICE_ID_BROADCOM_43143),
    BRCMF_SDIO_DEVICE(SDIO_DEVICE_ID_BROADCOM_43241),
    BRCMF_SDIO_DEVICE(SDIO_DEVICE_ID_BROADCOM_4329),
    BRCMF_SDIO_DEVICE(SDIO_DEVICE_ID_BROADCOM_4330),
    BRCMF_SDIO_DEVICE(SDIO_DEVICE_ID_BROADCOM_4334),
    BRCMF_SDIO_DEVICE(SDIO_DEVICE_ID_BROADCOM_43340),
    BRCMF_SDIO_DEVICE(SDIO_DEVICE_ID_BROADCOM_43341),
    BRCMF_SDIO_DEVICE(SDIO_DEVICE_ID_BROADCOM_43362),
    BRCMF_SDIO_DEVICE(SDIO_DEVICE_ID_BROADCOM_4335_4339),
    BRCMF_SDIO_DEVICE(SDIO_DEVICE_ID_BROADCOM_4339),
    BRCMF_SDIO_DEVICE(SDIO_DEVICE_ID_BROADCOM_43430),
    BRCMF_SDIO_DEVICE(SDIO_DEVICE_ID_BROADCOM_4345),
    BRCMF_SDIO_DEVICE(SDIO_DEVICE_ID_BROADCOM_43455),
    BRCMF_SDIO_DEVICE(SDIO_DEVICE_ID_BROADCOM_4354),
    BRCMF_SDIO_DEVICE(SDIO_DEVICE_ID_BROADCOM_4356),
    BRCMF_SDIO_DEVICE(SDIO_DEVICE_ID_CYPRESS_4373),
    {/* end: all zeroes */}
};
MODULE_DEVICE_TABLE(sdio, brcmf_sdmmc_ids);

static void brcmf_sdiod_acpi_set_power_manageable(struct brcmf_device* dev, int val) {
#if IS_ENABLED(CONFIG_ACPI)
    struct acpi_device* adev;

    adev = ACPI_COMPANION(dev);
    if (adev) {
        adev->flags.power_manageable = 0;
    }
#endif
}

static zx_status_t brcmf_ops_sdio_probe(struct sdio_func* func, const struct sdio_device_id* id) {
    zx_status_t err;
    struct brcmf_sdio_dev* sdiodev;
    struct brcmf_bus* bus_if;
    struct brcmf_device* dev;

    brcmf_dbg(SDIO, "Enter\n");
    brcmf_dbg(SDIO, "Class=%x\n", func->class);
    brcmf_dbg(SDIO, "sdio vendor ID: 0x%04x\n", func->vendor);
    brcmf_dbg(SDIO, "sdio device ID: 0x%04x\n", func->device);
    brcmf_dbg(SDIO, "Function#: %d\n", func->num);

    dev = &func->dev;

    /* Set MMC_QUIRK_LENIENT_FN0 for this card */
    func->card->quirks |= MMC_QUIRK_LENIENT_FN0;

    /* prohibit ACPI power management for this device */
    brcmf_sdiod_acpi_set_power_manageable(dev, 0);

    /* Consume func num 1 but dont do anything with it. */
    if (func->num == 1) {
        return ZX_OK;
    }

    /* Ignore anything but func 2 */
    if (func->num != 2) {
        return ZX_ERR_IO_NOT_PRESENT;
    }

    bus_if = calloc(1, sizeof(struct brcmf_bus));
    if (!bus_if) {
        return ZX_ERR_NO_MEMORY;
    }
    sdiodev = calloc(1, sizeof(struct brcmf_sdio_dev));
    if (!sdiodev) {
        free(bus_if);
        return ZX_ERR_NO_MEMORY;
    }

    /* store refs to functions used. mmc_card does
     * not hold the F0 function pointer.
     */
    sdiodev->func1 = func->card->sdio_func[0];
    sdiodev->func2 = func;

    sdiodev->bus_if = bus_if;
    bus_if->bus_priv.sdio = sdiodev;
    bus_if->proto_type = BRCMF_PROTO_BCDC;
    dev_set_drvdata(&func->dev, bus_if);
    dev_set_drvdata(&sdiodev->func1->dev, bus_if);
    sdiodev->dev = &sdiodev->func1->dev;

    brcmf_sdiod_change_state(sdiodev, BRCMF_SDIOD_DOWN);

    brcmf_dbg(SDIO, "F2 found, calling brcmf_sdiod_probe...\n");
    err = brcmf_sdiod_probe(sdiodev);
    if (err != ZX_OK) {
        brcmf_err("F2 error, probe failed %d...\n", err);
        goto fail;
    }

    brcmf_dbg(SDIO, "F2 init completed...\n");
    return ZX_OK;

fail:
    dev_set_drvdata(&func->dev, NULL);
    dev_set_drvdata(&sdiodev->func1->dev, NULL);
    free(sdiodev);
    free(bus_if);
    return err;
}

static void brcmf_ops_sdio_remove(struct sdio_func* func) {
    struct brcmf_bus* bus_if;
    struct brcmf_sdio_dev* sdiodev;

    brcmf_dbg(SDIO, "Enter\n");
    brcmf_dbg(SDIO, "sdio vendor ID: 0x%04x\n", func->vendor);
    brcmf_dbg(SDIO, "sdio device ID: 0x%04x\n", func->device);
    brcmf_dbg(SDIO, "Function: %d\n", func->num);

    bus_if = dev_get_drvdata(&func->dev);
    if (bus_if) {
        sdiodev = bus_if->bus_priv.sdio;

        /* start by unregistering irqs */
        brcmf_sdiod_intr_unregister(sdiodev);

        if (func->num != 1) {
            return;
        }

        /* only proceed with rest of cleanup if func 1 */
        brcmf_sdiod_remove(sdiodev);

        dev_set_drvdata(&sdiodev->func1->dev, NULL);
        dev_set_drvdata(&sdiodev->func2->dev, NULL);

        free(bus_if);
        free(sdiodev);
    }

    brcmf_dbg(SDIO, "Exit\n");
}

void brcmf_sdio_wowl_config(struct brcmf_device* dev, bool enabled) {
    struct brcmf_bus* bus_if = dev_get_drvdata(dev);
    struct brcmf_sdio_dev* sdiodev = bus_if->bus_priv.sdio;

    brcmf_dbg(SDIO, "Configuring WOWL, enabled=%d\n", enabled);
    sdiodev->wowl_enabled = enabled;
}

#ifdef CONFIG_PM_SLEEP
static zx_status_t brcmf_ops_sdio_suspend(struct brcmf_device* dev) {
    struct sdio_func* func;
    struct brcmf_bus* bus_if;
    struct brcmf_sdio_dev* sdiodev;
    mmc_pm_flag_t sdio_flags;

    func = containerof(dev, struct sdio_func, dev);
    brcmf_dbg(SDIO, "Enter: F%d\n", func->num);
    if (func->num != 1) {
        return ZX_OK;
    }

    bus_if = dev_get_drvdata(dev);
    sdiodev = bus_if->bus_priv.sdio;

    brcmf_sdiod_freezer_on(sdiodev);
    brcmf_sdio_wd_timer(sdiodev->bus, 0);

    sdio_flags = MMC_PM_KEEP_POWER;
    if (sdiodev->wowl_enabled) {
        if (sdiodev->settings->bus.sdio.oob_irq_supported) {
            enable_irq_wake(sdiodev->settings->bus.sdio.oob_irq_nr);
        } else {
            sdio_flags |= MMC_PM_WAKE_SDIO_IRQ;
        }
    }
    if (sdio_set_host_pm_flags(sdiodev->func1, sdio_flags)) {
        brcmf_err("Failed to set pm_flags %x\n", sdio_flags);
    }
    return ZX_OK;
}

static zx_status_t brcmf_ops_sdio_resume(struct brcmf_device* dev) {
    struct brcmf_bus* bus_if = dev_get_drvdata(dev);
    struct brcmf_sdio_dev* sdiodev = bus_if->bus_priv.sdio;
    struct sdio_func* func = containerof(dev, struct sdio_func, dev);

    brcmf_dbg(SDIO, "Enter: F%d\n", func->num);
    if (func->num != 2) {
        return ZX_OK;
    }

    brcmf_sdiod_freezer_off(sdiodev);
    return ZX_OK;
}

static const struct dev_pm_ops brcmf_sdio_pm_ops = {
    .suspend = brcmf_ops_sdio_suspend,
    .resume = brcmf_ops_sdio_resume,
};
#endif /* CONFIG_PM_SLEEP */

static struct sdio_driver brcmf_sdmmc_driver = {
    .probe = brcmf_ops_sdio_probe,
    .remove = brcmf_ops_sdio_remove,
    .name = KBUILD_MODNAME,
    .id_table = brcmf_sdmmc_ids,
    .drv = {
#ifdef CONFIG_PM_SLEEP
        .pm = &brcmf_sdio_pm_ops,
#endif /* CONFIG_PM_SLEEP */
    },
};

void brcmf_sdio_register(void) {
    zx_status_t ret;

    ret = sdio_register_driver(&brcmf_sdmmc_driver);
    if (ret != ZX_OK) {
        brcmf_err("sdio_register_driver failed: %d\n", ret);
    }
}

void brcmf_sdio_exit(void) {
    brcmf_dbg(SDIO, "Enter\n");

    sdio_unregister_driver(&brcmf_sdmmc_driver);
}
