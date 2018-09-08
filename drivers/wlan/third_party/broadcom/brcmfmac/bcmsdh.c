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

// TODO(cphoenix): Do we need sdio, completion, status, stdatomic, threads?
#include <ddk/protocol/sdio.h>
#include <lib/sync/completion.h>
#include <zircon/status.h>

#include <stdatomic.h>
#ifndef _ALL_SOURCE
#define _ALL_SOURCE // Enables thrd_create_with_name in <threads.h>.
#endif
#include <threads.h>

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
    sync_completion_t thread_freeze;
    sync_completion_t resumed;
};
bool print_sdio = false;
int last_here;
uint16_t checksum(void* buf, int len) {
  char* cbuf = buf;
  int i;
  uint16_t sum = 0;
  for (i = 0; i < len; i++) {
    sum = (sum << 1) ^ (sum << 5) ^ cbuf[i] ^ cbuf[i] << 7;
  }
  return sum;
}

extern thrd_t gl_interrupt_thread;
static int brcmf_sdiod_oob_irqhandler(void* cookie) {
    struct brcmf_sdio_dev* sdiodev = cookie;
    zx_status_t status;

    gl_interrupt_thread = thrd_current();
    while ((status = zx_interrupt_wait(sdiodev->irq_handle, NULL)) == ZX_OK) {
        //brcmf_dbg(INTR, "OOB intr triggered\n");

        brcmf_sdio_isr(sdiodev->bus);
    }

    brcmf_err("ISR exiting with status %s\n", zx_status_get_string(status));
    return (int)status;
}

static void brcmf_sdiod_ib_irqhandler(struct brcmf_sdio_dev* sdiodev) {
    brcmf_dbg(INTR, "IB intr triggered\n");

    brcmf_sdio_isr(sdiodev->bus);
}

/* dummy handler for SDIO function 2 interrupt */
static void brcmf_sdiod_dummy_irqhandler(struct brcmf_sdio_dev* sdiodev) {}
bool gl_intr_print;
extern atomic_int dw_state;
extern atomic_int dw_count;
extern bool gl_print_sdio;
void remember_sdio(uint8_t func, uint32_t addr, bool write, void* data, size_t size,
                                           bool fifo);

zx_status_t brcmf_sdiod_intr_register(struct brcmf_sdio_dev* sdiodev) {
    struct brcmfmac_sdio_pd* pdata;
    zx_status_t ret = ZX_OK;
    uint8_t data;
    uint32_t addr, gpiocontrol;

    //gl_print_sdio = true;
    pdata = &sdiodev->settings->bus.sdio;
    // TODO(cphoenix): Always?
    remember_sdio(11, 0xffff, 1, &ret, 4, 0);
    pdata->oob_irq_supported = true;
    if (pdata->oob_irq_supported) {
        brcmf_dbg(SDIO, "Enter, register OOB IRQ in 100 msec, %p %p", &sdiodev->sdio_proto,
                  &sdiodev->irq_handle); PAUSE; PAUSE;
        // TODO(cphoenix): Add error handling for sdio_get_oob_irq, thrd_create_with_name, and
        // thrd_detach. Note that the thrd_ functions don't return zx_status_t; check for
        // thrd_success and maybe thrd_nomem. See zircon/third_party/ulib/musl/include/threads.h

        ret = enable_irq_wake(sdiodev->irq_handle);
        brcmf_dbg(TEMP, "ddpc state %d, times %d", atomic_load(&dw_state), atomic_load(&dw_count));
        remember_sdio(15, 0xffff, 1, &ret, 4, 0);
        brcmf_dbg(SDIO, "4Enter, register OOB IRQ\n"); PAUSE; PAUSE;
        brcmf_dbg(TEMP, "edpc state %d, times %d", atomic_load(&dw_state), atomic_load(&dw_count));
        remember_sdio(16, 0xffff, 1, &ret, 4, 0);
        if (ret != ZX_OK) {
            brcmf_err("enable_irq_wake failed %d\n", ret);
            return ret;
        }
        sdiodev->irq_wake = true;

        if (sdiodev->bus_if->chip == BRCM_CC_43362_CHIP_ID) {
        brcmf_dbg(SDIO, "5Enter, register OOB IRQ\n"); PAUSE; PAUSE;
            /* assign GPIO to SDIO core */
            addr = CORE_CC_REG(SI_ENUM_BASE, gpiocontrol);
        brcmf_dbg(SDIO, "6Enter, register OOB IRQ\n"); PAUSE; PAUSE;
            gpiocontrol = brcmf_sdiod_func1_rl(sdiodev, addr, &ret);
        brcmf_dbg(SDIO, "7Enter, register OOB IRQ\n"); PAUSE; PAUSE;
            gpiocontrol |= 0x2;
            brcmf_sdiod_func1_wl(sdiodev, addr, gpiocontrol, &ret);

            brcmf_sdiod_func1_wb(sdiodev, SBSDIO_GPIO_SELECT, 0xf, &ret);
            brcmf_sdiod_func1_wb(sdiodev, SBSDIO_GPIO_OUT, 0, &ret);
            brcmf_sdiod_func1_wb(sdiodev, SBSDIO_GPIO_EN, 0x2, &ret);
        brcmf_dbg(SDIO, "8Enter, register OOB IRQ\n"); PAUSE; PAUSE;
        }

        brcmf_dbg(SDIO, "9Enter, register OOB IRQ\n"); PAUSE; PAUSE;
        /* must configure SDIO_CCCR_INT_ENABLE to enable irq */
        brcmf_dbg(TEMP, "fdpc state %d, times %d", atomic_load(&dw_state), atomic_load(&dw_count));
        remember_sdio(17, 0xffff, 1, &ret, 4, 0);
        data = brcmf_sdiod_func0_rb(sdiodev, SDIO_CCCR_INT_ENABLE, &ret);
        brcmf_dbg(TEMP, "gdpc state %d, times %d", atomic_load(&dw_state), atomic_load(&dw_count));
        remember_sdio(18, 0xffff, 1, &ret, 4, 0);
        data |= SDIO_CCCR_IEN_FUNC1 | SDIO_CCCR_IEN_FUNC2 | SDIO_CCCR_IEN_FUNC0;
        brcmf_sdiod_func0_wb(sdiodev, SDIO_CCCR_INT_ENABLE, data, &ret);

        /* redirect, configure and enable io for interrupt signal */
        data = SDIO_CCCR_BRCM_SEPINT_MASK | SDIO_CCCR_BRCM_SEPINT_OE;
        pdata->oob_irq_flags = IRQ_FLAG_LEVEL_HIGH;
        if (pdata->oob_irq_flags & IRQ_FLAG_LEVEL_HIGH) {
            data |= SDIO_CCCR_BRCM_SEPINT_ACT_HI;
        }
        gl_intr_print = true;
        brcmf_sdiod_func0_wb(sdiodev, SDIO_CCCR_BRCM_SEPINT, data, &ret);
        brcmf_dbg(SDIO, "aEnter, register OOB IRQ\n"); PAUSE; PAUSE;


        sdio_get_oob_irq(&sdiodev->sdio_proto, &sdiodev->irq_handle);
        PAUSE; PAUSE; brcmf_dbg(SDIO, "Did get OOB IRQ 100 msec ago");
        thrd_create_with_name(&sdiodev->isr_thread, brcmf_sdiod_oob_irqhandler, sdiodev,
                              "brcmf-sdio-isr");
        remember_sdio(12, 0xffff, 1, &ret, 4, 0);
        brcmf_dbg(TEMP, "adpc state %d, times %d", atomic_load(&dw_state), atomic_load(&dw_count));
        brcmf_dbg(TEMP, "bdpc state %d, times %d", atomic_load(&dw_state), atomic_load(&dw_count));
        brcmf_dbg(SDIO, "2Enter, register OOB IRQ\n"); PAUSE; PAUSE;
        brcmf_dbg(TEMP, "cdpc state %d, times %d", atomic_load(&dw_state), atomic_load(&dw_count));
        thrd_detach(sdiodev->isr_thread);
        remember_sdio(13, 0xffff, 1, &ret, 4, 0);
        brcmf_dbg(SDIO, "3Enter, register OOB IRQ\n"); PAUSE; PAUSE;
        brcmf_dbg(TEMP, "ddpc state %d, times %d", atomic_load(&dw_state), atomic_load(&dw_count));
        remember_sdio(21, 0xffff, 1, &ret, 4, 0); PAUSE; PAUSE;
        brcmf_dbg(TEMP, "gdpc state %d, times %d", atomic_load(&dw_state), atomic_load(&dw_count));
        remember_sdio(22, 0xffff, 1, &ret, 4, 0); PAUSE; PAUSE;
        brcmf_dbg(TEMP, "hdpc state %d, times %d", atomic_load(&dw_state), atomic_load(&dw_count));
        remember_sdio(23, 0xffff, 1, &ret, 4, 0); PAUSE; PAUSE;
        brcmf_dbg(TEMP, "idpc state %d, times %d", atomic_load(&dw_state), atomic_load(&dw_count));
        remember_sdio(24, 0xffff, 1, &ret, 4, 0); PAUSE; PAUSE; PAUSE; PAUSE; PAUSE; PAUSE;
        brcmf_dbg(TEMP, "jdpc state %d, times %d", atomic_load(&dw_state), atomic_load(&dw_count));
        remember_sdio(25, 0xffff, 1, &ret, 4, 0);
        sdiodev->oob_irq_requested = true;
        remember_sdio(24, 0xffff, 1, &ret, 4, 0);
        brcmf_dbg(TEMP, "jdpc state %d, times %d", atomic_load(&dw_state), atomic_load(&dw_count));
        PAUSE; PAUSE;
        remember_sdio(24, 0xffff, 1, &ret, 4, 0);
        brcmf_dbg(TEMP, "jdpc state %d, times %d", atomic_load(&dw_state), atomic_load(&dw_count));


    } else {
        brcmf_dbg(SDIO, "Entering\n");
        sdio_enable_fn_intr(&sdiodev->sdio_proto, SDIO_FN_1);
        (void)brcmf_sdiod_ib_irqhandler; // TODO(cphoenix): If we use these, plug them in later.
        sdio_enable_fn_intr(&sdiodev->sdio_proto, SDIO_FN_2);
        (void)brcmf_sdiod_dummy_irqhandler;
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
        brcmf_sdiod_func0_wb(sdiodev, SDIO_CCCR_BRCM_SEPINT, 0, NULL);
        brcmf_sdiod_func0_wb(sdiodev, SDIO_CCCR_INT_ENABLE, 0, NULL);

        sdiodev->oob_irq_requested = false;
        if (sdiodev->irq_wake) {
            disable_irq_wake(sdiodev->irq_handle);
            sdiodev->irq_wake = false;
        }
        zx_handle_close(sdiodev->irq_handle);
        sdiodev->oob_irq_requested = false;
    }

    if (sdiodev->sd_irq_requested) {
        sdio_disable_fn_intr(&sdiodev->sdio_proto, SDIO_FN_2);
        sdio_disable_fn_intr(&sdiodev->sdio_proto, SDIO_FN_1);
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
extern bool gl_compare_now;

#define SR_DATA_SIZE 8
#define SR_DATA_RECORD_COUNT 10000
static atomic_int current_sr_rec;
typedef struct sdio_record {
    char data[SR_DATA_SIZE];
    bool write;
    bool fifo;
    uint8_t func;
    uint32_t addr;
    uint32_t size;
    int dw_state;
    uint32_t checksum;
    thrd_t thread;
} sdio_record_t;
thrd_t gl_interrupt_thread, gl_main_thread, gl_worker_thread, gl_watchdog_thread;

static sdio_record_t sdio_records[SR_DATA_RECORD_COUNT];

extern atomic_int dw_state;

static void pr(int32_t i, sdio_record_t* rec) {
        char namebuf[64];
        char* name;
        if (rec->thread == gl_interrupt_thread) {
            name = "intr";
        } else if (rec->thread == gl_worker_thread) {
            name = "work";
        } else if (rec->thread == gl_watchdog_thread) {
            name = "watch";
        } else if (rec->thread == gl_main_thread) {
            name = "";
        } else {
            sprintf(namebuf, "%p", rec->thread);
            name = namebuf;
        }
        brcmf_dbg(TEMP, "sdio %d: F%d%s%d%s %x %lx (%x) %s %d", i, rec->func, rec->write ? "w" : "r",
                  rec->size, rec->fifo ? "f" : "", rec->addr, *(uint64_t*)rec->data, rec->checksum, name,
                  rec->dw_state);
}
bool gl_print_sdio;
void remember_sdio(uint8_t func, uint32_t addr, bool write, void* data, size_t size,
                                           bool fifo) {
    if (current_sr_rec >= SR_DATA_RECORD_COUNT) {
        return;
    }
    sdio_record_t* rec = &sdio_records[atomic_fetch_add(&current_sr_rec, 1)];
    memcpy(rec->data, data, min(size, SR_DATA_SIZE));
    rec->write = write;
    rec->fifo = fifo;
    rec->func = func;
    rec->addr = addr;
    rec->size = (uint32_t)size;
    rec->dw_state = atomic_load(&dw_state);
    rec->checksum = checksum(data, size);
    rec->thread = thrd_current();
    if (gl_print_sdio && rec->thread == gl_main_thread) {
        pr(12345, rec);
    }
}

void psr() {
    int32_t i;
    atomic_thread_fence(memory_order_seq_cst);
    int32_t last_rec = atomic_load(&current_sr_rec);
    for (i = 0; i < last_rec; i++) {
        sdio_record_t* rec = &sdio_records[i];
        pr(i, rec);
        zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));
    }
}
atomic_bool gl_writing_firmware;
static zx_status_t brcmf_sdiod_transfer(struct brcmf_sdio_dev* sdiodev, uint8_t func,
                                           uint32_t addr, bool write, void* data, size_t size,
                                           bool fifo) {
    sdio_rw_txn_t txn;

    txn.addr = addr;
    txn.write = write;
    txn.virt = data;
    txn.data_size = size;
    txn.incr = !fifo;
    txn.fifo = fifo;
    txn.use_dma = false; // TODO(cphoenix): Decide when to use DMA
    txn.buf_offset = 0;
    zx_status_t result;

    result = sdio_do_rw_txn(&sdiodev->sdio_proto, func, &txn);
    if (result != ZX_OK) {
        brcmf_dbg(TEMP, "Why did this fail?? result %d", result);
        return result;
    }
    if (!(addr == 0xd020 && *(uint32_t*)data == 0x80000000 && atomic_load(&dw_state) == 0) &&
      !atomic_load(&gl_writing_firmware)) {
        remember_sdio(func, addr, write, data, size, fifo);
    }
    return result;
}

static uint8_t brcmf_sdiod_func_rb(struct brcmf_sdio_dev* sdiodev, uint8_t func, uint32_t addr,
                             zx_status_t* result_out) {
    uint8_t data;
    zx_status_t result;
    result = brcmf_sdiod_transfer(sdiodev, func, addr, false, &data, sizeof(data), false);
    if (result_out != NULL) {
        *result_out = result;
    }
    return data;
}

uint8_t brcmf_sdiod_func0_rb(struct brcmf_sdio_dev* sdiodev, uint32_t addr,
                             zx_status_t* result_out) {
    return brcmf_sdiod_func_rb(sdiodev, SDIO_FN_0, addr, result_out);
}

uint8_t brcmf_sdiod_func1_rb(struct brcmf_sdio_dev* sdiodev, uint32_t addr,
                             zx_status_t* result_out) {
    return brcmf_sdiod_func_rb(sdiodev, SDIO_FN_1, addr, result_out);
}

void brcmf_sdiod_func0_wb(struct brcmf_sdio_dev* sdiodev, uint32_t addr, uint8_t data,
                          zx_status_t* result_out) {
    zx_status_t result;
    result = brcmf_sdiod_transfer(sdiodev, SDIO_FN_0, addr, true, &data, sizeof(data), false);
    if (result_out != NULL) {
        *result_out = result;
    }
}

void brcmf_sdiod_func1_wb(struct brcmf_sdio_dev* sdiodev, uint32_t addr, uint8_t data,
                          zx_status_t* result_out) {
    zx_status_t result;
    result = brcmf_sdiod_transfer(sdiodev, SDIO_FN_1, addr, true, &data, sizeof(data), false);
    if (result_out != NULL) {
        *result_out = result;
    }
}

static zx_status_t brcmf_sdiod_set_backplane_window(struct brcmf_sdio_dev* sdiodev, uint32_t addr) {
    uint32_t v;
    uint32_t bar0 = addr & SBSDIO_SBWINDOW_MASK;
    zx_status_t err = ZX_OK;
    int i;

    if (bar0 == sdiodev->sbwad) {
        if (gl_compare_now) {
            brcmf_dbg(TEMP, "bar0 equal, addr was 0x%x", addr);
        }
        return ZX_OK;
    }
    v = bar0 >> 8;

    for (i = 0; i < 3 && err == ZX_OK; i++, v >>= 8) {
        if (err == ZX_OK) {
            brcmf_sdiod_func1_wb(sdiodev, SBSDIO_FUNC1_SBADDRLOW + i, v & 0xff, &err);
        }
    }

    if (err == ZX_OK) {
        sdiodev->sbwad = bar0;
    }

    return err;
}

#ifdef NICE_TRY
static zx_status_t brcmf_sdiod_set_backplane_window_RW(struct brcmf_sdio_dev* sdiodev, uint32_t addr) {
    uint32_t v;
    uint32_t bar0 = addr & SBSDIO_SBWINDOW_MASK_RW;
    zx_status_t err = ZX_OK;
    int i;

    if (bar0 == sdiodev->sbwad) {
        if (gl_compare_now) {
            brcmf_dbg(TEMP, "bar0 equal, addr was 0x%x", addr);
        }
        return ZX_OK;
    }
    v = bar0 >> 8;

    for (i = 0; i < 3 && err == ZX_OK; i++, v >>= 8) {
        if (err == ZX_OK) {
            brcmf_sdiod_func1_wb(sdiodev, SBSDIO_FUNC1_SBADDRLOW + i, v & 0xff, &err);
        }
    }

    if (err == ZX_OK) {
        sdiodev->sbwad = bar0;
    }

    return err;
}
#endif // NICE_TRY

mtx_t sdio_mutex = {};

uint32_t brcmf_sdiod_func1_rl(struct brcmf_sdio_dev* sdiodev, uint32_t addr, zx_status_t* ret) {
    uint32_t data = 0;
    zx_status_t retval;

    retval = brcmf_sdiod_set_backplane_window(sdiodev, addr);
    if (retval == ZX_OK) {
        addr &= SBSDIO_SB_OFT_ADDR_MASK;
        addr |= SBSDIO_SB_ACCESS_2_4B_FLAG;
        retval = brcmf_sdiod_transfer(sdiodev, SDIO_FN_1, addr, false, &data, sizeof(data), false);
    }
    if (ret) {
        *ret = retval;
    }

    return data;
}

void brcmf_sdiod_func1_wl(struct brcmf_sdio_dev* sdiodev, uint32_t addr, uint32_t data,
                          zx_status_t* ret) {
    zx_status_t retval;

    retval = brcmf_sdiod_set_backplane_window(sdiodev, addr);
    if (retval == ZX_OK) {
        addr &= SBSDIO_SB_OFT_ADDR_MASK;
        addr |= SBSDIO_SB_ACCESS_2_4B_FLAG;
        retval = brcmf_sdiod_transfer(sdiodev, SDIO_FN_1, addr, true, &data, sizeof(data), false);
    }
    if (ret) {
        *ret = retval;
    }
}

zx_status_t brcmf_sdiod_read(struct brcmf_sdio_dev* sdiodev, uint8_t func, uint32_t addr,
                             void* data, size_t size) {
    return brcmf_sdiod_transfer(sdiodev, func, addr, false, data, size, false);
}

zx_status_t brcmf_sdiod_write(struct brcmf_sdio_dev* sdiodev, uint8_t func, uint32_t addr,
                              void* data, size_t size) {
    return brcmf_sdiod_transfer(sdiodev, func, addr, true, data, size, false);
}

zx_status_t brcmf_sdiod_read_fifo(struct brcmf_sdio_dev* sdiodev, uint8_t func, uint32_t addr,
                                  void* data, size_t size) {
    return brcmf_sdiod_transfer(sdiodev, func, addr, false, data, size, true);
}

static zx_status_t brcmf_sdiod_netbuf_read(struct brcmf_sdio_dev* sdiodev, uint32_t func,
                                           uint32_t addr, struct brcmf_netbuf* netbuf) {
    unsigned int req_sz;
    zx_status_t err;

    /* Single netbuf use the standard mmc interface */
    req_sz = netbuf->len + 3;
    req_sz &= (uint)~3;

    switch (func) {
    case SDIO_FN_1:
        err = brcmf_sdiod_read(sdiodev, func, addr, netbuf->data, req_sz);
        break;
    case SDIO_FN_2:
        err = brcmf_sdiod_read_fifo(sdiodev, func, addr, netbuf->data, req_sz);
        break;
    default:
        /* bail out as things are really fishy here */
        WARN(1, "invalid sdio function number %d\n");
        err = ZX_ERR_IO_REFUSED;
    };

    if (err == ZX_ERR_IO_REFUSED) {
        brcmf_sdiod_change_state(sdiodev, BRCMF_SDIOD_NOMEDIUM);
    }

    return err;
}

static zx_status_t brcmf_sdiod_netbuf_write(struct brcmf_sdio_dev* sdiodev, uint32_t func,
                                            uint32_t addr, struct brcmf_netbuf* netbuf) {
    unsigned int req_sz;
    zx_status_t err;

    /* Single netbuf use the standard mmc interface */
    req_sz = netbuf->len + 3;
    req_sz &= (uint)~3;

    err = brcmf_sdiod_write(sdiodev, func, addr, netbuf->data, req_sz);

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

    err = brcmf_sdiod_netbuf_read(sdiodev, SDIO_FN_2, addr, pkt);

done:
    return err;
}

zx_status_t brcmf_sdiod_recv_chain(struct brcmf_sdio_dev* sdiodev, struct brcmf_netbuf_list* pktq,
                                   uint totlen) {
    struct brcmf_netbuf* glom_netbuf = NULL;
    struct brcmf_netbuf* netbuf;
    uint32_t addr = sdiodev->cc_core->base;
    zx_status_t err = ZX_OK;

    brcmf_dbg(SDIO, "addr = 0x%x, size = %d\n", addr, brcmf_netbuf_list_length(pktq));

    err = brcmf_sdiod_set_backplane_window(sdiodev, addr);
    if (err != ZX_OK) {
        goto done;
    }

    addr &= SBSDIO_SB_OFT_ADDR_MASK;
    addr |= SBSDIO_SB_ACCESS_2_4B_FLAG;

    if (brcmf_netbuf_list_length(pktq) == 1) {
        err = brcmf_sdiod_netbuf_read(sdiodev, SDIO_FN_2, addr, brcmf_netbuf_list_peek_head(pktq));
    } else {
        glom_netbuf = brcmu_pkt_buf_get_netbuf(totlen);
        if (!glom_netbuf) {
            return ZX_ERR_NO_MEMORY;
        }
        err = brcmf_sdiod_netbuf_read(sdiodev, SDIO_FN_2, addr, glom_netbuf);
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
        err = brcmf_sdiod_netbuf_write(sdiodev, SDIO_FN_2, addr, mypkt);
    }

    brcmu_pkt_buf_free_netbuf(mypkt);

    return err;
}

zx_status_t brcmf_sdiod_send_pkt(struct brcmf_sdio_dev* sdiodev, struct brcmf_netbuf_list* pktq) {
    struct brcmf_netbuf* netbuf;
    uint32_t addr = sdiodev->cc_core->base;
    zx_status_t err;

    brcmf_dbg(SDIO, "addr = 0x%x, size = %d\n", addr, brcmf_netbuf_list_length(pktq));

    err = brcmf_sdiod_set_backplane_window(sdiodev, addr);
    if (err != ZX_OK) {
        return err;
    }

    addr &= SBSDIO_SB_OFT_ADDR_MASK;
    addr |= SBSDIO_SB_ACCESS_2_4B_FLAG;

    brcmf_netbuf_list_for_every(pktq, netbuf) {
        err = brcmf_sdiod_netbuf_write(sdiodev, SDIO_FN_2, addr, netbuf);
        if (err != ZX_OK) {
            break;
        }
    }

    return err;
}
extern bool gl_compare_now;
bool brcmf_sdio_verifymemory(struct brcmf_sdio_dev* sdiodev, uint32_t ram_addr,
                                    uint8_t* ram_data, uint ram_sz);
zx_status_t brcmf_sdiod_ramrw(struct brcmf_sdio_dev* sdiodev, bool write, uint32_t address,
                              uint8_t* data, uint size) {
    zx_status_t err = ZX_OK;
    struct brcmf_netbuf* pkt;
    uint32_t this_transfer_address;
    uint this_transfer_size;

#define MAX_XFER_SIZE 0x100

    uint packet_size = min_t(uint, MAX_XFER_SIZE, size);
    pkt = brcmf_netbuf_allocate(packet_size);
    if (!pkt) {
        brcmf_err("brcmf_netbuf_allocate failed: len %d\n", packet_size);
        return ZX_ERR_IO;
    }
    pkt->priority = 0;
    /* Determine initial transfer parameters */
    this_transfer_address = address & SBSDIO_SB_OFT_ADDR_MASK;
    uint32_t low_address_bits = this_transfer_address & (MAX_XFER_SIZE - 1);
    if (low_address_bits) {
        this_transfer_size = min(packet_size, MAX_XFER_SIZE - low_address_bits);
    } else {
        this_transfer_size = packet_size;
    }

    /* Do the transfer(s) */
    while (size) {

        /* Set the backplane window to include the start address */
        err = brcmf_sdiod_set_backplane_window(sdiodev, address);

        if (err != ZX_OK) {
            break;
        }

        this_transfer_address &= SBSDIO_SB_OFT_ADDR_MASK;
        this_transfer_address |= SBSDIO_SB_ACCESS_2_4B_FLAG;

        brcmf_netbuf_grow_tail(pkt, this_transfer_size);

        if (write) {
            memcpy(pkt->data, data, this_transfer_size);
            err = brcmf_sdiod_netbuf_write(sdiodev, SDIO_FN_1, this_transfer_address, pkt);
        } else {
            err = brcmf_sdiod_netbuf_read(sdiodev, SDIO_FN_1, this_transfer_address, pkt);
        }

        if (err != ZX_OK) {
            brcmf_err("membytes transfer failed\n");
            break;
        }
        if (!write) {
            memcpy(data, pkt->data, this_transfer_size);
        }
        brcmf_netbuf_reduce_length_to(pkt, 0);

        /* Adjust for next transfer (if any) */
        size -= this_transfer_size;
        if (size) {
            data += this_transfer_size;
            address += this_transfer_size;
            this_transfer_address += this_transfer_size;
            this_transfer_size = min_t(uint32_t, MAX_XFER_SIZE, size);
        }
    }

    brcmf_netbuf_free(pkt);

    return err;
}

zx_status_t brcmf_sdiod_abort(struct brcmf_sdio_dev* sdiodev, uint32_t func) {
    brcmf_dbg(SDIO, "Enter\n");

    /* Issue abort cmd52 command through F0 */
    brcmf_sdiod_func0_wb(sdiodev, SDIO_CCCR_ABORT_RESET, func, NULL);

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
    sdiodev->freezer->thread_freeze = SYNC_COMPLETION_INIT;
    sdiodev->freezer->resumed = SYNC_COMPLETION_INIT;
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
    sync_completion_reset(&sdiodev->freezer->resumed);
    sync_completion_reset(&sdiodev->freezer->thread_freeze);
    atomic_store(&sdiodev->freezer->freezing, 1);
    brcmf_sdio_trigger_dpc(sdiodev->bus);
    sync_completion_wait(&sdiodev->freezer->thread_freeze, ZX_TIME_INFINITE);
    res = brcmf_sdio_sleep(sdiodev->bus, true);
    return res;
}

static void brcmf_sdiod_freezer_off(struct brcmf_sdio_dev* sdiodev) {
    brcmf_sdio_sleep(sdiodev->bus, false);
    atomic_store(&sdiodev->freezer->freezing, 0);
    sync_completion_signal(&sdiodev->freezer->resumed);
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
        sync_completion_signal(&sdiodev->freezer->thread_freeze);
    }
    sync_completion_wait(&sdiodev->freezer->resumed, ZX_TIME_INFINITE);
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
    sdio_disable_fn(&sdiodev->sdio_proto, SDIO_FN_2);

    /* Disable Function 1 */
    sdio_disable_fn(&sdiodev->sdio_proto, SDIO_FN_1);

    sdiodev->sbwad = 0;

    // TODO(cphoenix): Power management stuff
    //pm_runtime_allow(sdiodev->func1->card->host->parent);
    return ZX_OK;
}

// TODO(cphoenix): Power management stuff
#ifdef POWER_MANAGEMENT
static void brcmf_sdiod_host_fixup(struct mmc_host* host) {
    /* runtime-pm powers off the device */
    pm_runtime_forbid(host->parent);
    /* avoid removal detection upon resume */
    host->caps |= MMC_CAP_NONREMOVABLE; // Defined outside this driver's codebase
}
#endif // POWER_MANAGEMENT

static zx_status_t brcmf_sdiod_probe(struct brcmf_sdio_dev* sdiodev) {
    zx_status_t ret = ZX_OK;

    ret = sdio_update_block_size(&sdiodev->sdio_proto, SDIO_FN_1, SDIO_FUNC1_BLOCKSIZE, false);
    if (ret != ZX_OK) {
        brcmf_err("Failed to set F1 blocksize\n");
        goto out;
    }
    ret = sdio_update_block_size(&sdiodev->sdio_proto, SDIO_FN_2, SDIO_FUNC2_BLOCKSIZE, false);
    if (ret != ZX_OK) {
        brcmf_err("Failed to set F2 blocksize\n");
        goto out;
    }

    /* increase F2 timeout */
    // TODO(cphoenix): SDIO doesn't use timeout yet
    //sdiodev->func2->enable_timeout = SDIO_WAIT_F2RDY;

    /* Enable Function 1 */
    ret = sdio_enable_fn(&sdiodev->sdio_proto, SDIO_FN_1);
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
    //brcmf_sdiod_host_fixup(sdiodev->func2->card->host);
out:
    if (ret != ZX_OK) {
        brcmf_sdiod_remove(sdiodev);
    }

    return ret;
}

#ifdef TODO_ADD_SDIO_IDS // Put some of these into binding.c
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
    BRCMF_SDIO_DEVICE(0x4359),
    {/* end: all zeroes */}
};
#endif // TODO_ADD_SDIO_IDS

static void brcmf_sdiod_acpi_set_power_manageable(struct brcmf_device* dev, int val) {
#if IS_ENABLED(CONFIG_ACPI)
    struct acpi_device* adev;

    adev = ACPI_COMPANION(dev);
    if (adev) {
        adev->flags.power_manageable = 0;
    }
#endif
}

zx_status_t brcmf_sdio_register(zx_device_t* zxdev, sdio_protocol_t* sdio_proto) {
    zx_status_t err;
    struct brcmf_sdio_dev* sdiodev;
    struct brcmf_bus* bus_if;
    struct brcmf_device* dev;

    brcmf_dbg(SDIO, "Enter\n");
    sdio_hw_info_t devinfo;
    sdio_get_dev_hw_info(sdio_proto, &devinfo);
    if (devinfo.dev_hw_info.num_funcs < 3) {
        brcmf_err("Not enough SDIO funcs (need 3, have %d)", devinfo.dev_hw_info.num_funcs);
        return ZX_ERR_IO;
    }

    // We don't get "class" info in the current API.
    //brcmf_dbg(SDIO, "Class=%x\n", func->class);
    brcmf_dbg(SDIO, "sdio vendor ID: 0x%04x\n", devinfo.funcs_hw_info[SDIO_FN_1].manufacturer_id);
    brcmf_dbg(SDIO, "sdio device ID: 0x%04x\n", devinfo.funcs_hw_info[SDIO_FN_1].product_id);
    // Linux calls sdio_register once per SDIO function; we only get called once per chipset.
    //brcmf_dbg(SDIO, "Function#: %d\n", func);

    // TODO(cphoenix): Reexamine this when SDIO is more mature - do we need to support "quirks" in
    // Fuchsia? (MMC_QUIRK_LENIENT_FN0 is defined outside this driver.)
    /* Set MMC_QUIRK_LENIENT_FN0 for this card */
    //func->card->quirks |= MMC_QUIRK_LENIENT_FN0;

    /* prohibit ACPI power management for this device */
    // TODO(cphoenix): Linux power management stuff
    brcmf_sdiod_acpi_set_power_manageable(NULL, 0);

    bus_if = calloc(1, sizeof(struct brcmf_bus));
    if (!bus_if) {
        return ZX_ERR_NO_MEMORY;
    }
    sdiodev = calloc(1, sizeof(struct brcmf_sdio_dev));
    if (!sdiodev) {
        free(bus_if);
        return ZX_ERR_NO_MEMORY;
    }
    dev = &sdiodev->dev;
    dev->zxdev = zxdev;
    memcpy(&sdiodev->sdio_proto, sdio_proto, sizeof(sdiodev->sdio_proto));

    sdiodev->bus_if = bus_if;
    bus_if->bus_priv.sdio = sdiodev;
    bus_if->proto_type = BRCMF_PROTO_BCDC;
    dev->bus = bus_if;

    sdiodev->manufacturer_id = devinfo.funcs_hw_info[SDIO_FN_1].manufacturer_id;
    sdiodev->product_id = devinfo.funcs_hw_info[SDIO_FN_1].product_id;

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
    dev->bus = NULL;
    free(sdiodev);
    free(bus_if);
    return err;
}

static void brcmf_ops_sdio_remove(struct brcmf_sdio_dev* sdiodev) {
    struct brcmf_bus* bus_if;

    brcmf_dbg(SDIO, "Enter\n");
    if (sdiodev == NULL) {
        return;
    }
    brcmf_dbg(SDIO, "sdio vendor ID: 0x%04x\n", sdiodev->manufacturer_id);
    brcmf_dbg(SDIO, "sdio device ID: 0x%04x\n", sdiodev->product_id);

    bus_if = dev_to_bus(&sdiodev->dev);
    if (bus_if) {
        /* start by unregistering irqs */
        brcmf_sdiod_intr_unregister(sdiodev);

        brcmf_sdiod_remove(sdiodev);

        free(bus_if);
        free(sdiodev);
    }

    brcmf_dbg(SDIO, "Exit\n");
}

void brcmf_sdio_wowl_config(struct brcmf_device* dev, bool enabled) {
    struct brcmf_bus* bus_if = dev_to_bus(dev);
    struct brcmf_sdio_dev* sdiodev = bus_if->bus_priv.sdio;

    brcmf_dbg(SDIO, "Configuring WOWL, enabled=%d\n", enabled);
    sdiodev->wowl_enabled = enabled;
}

#ifdef CONFIG_PM_SLEEP
static zx_status_t brcmf_ops_sdio_suspend(struct brcmf_sdio_dev* sdiodev, uint32_t func) {
    struct brcmf_bus* bus_if;
    mmc_pm_flag_t sdio_flags;
    struct brcmf_device* dev = sdiodev->dev;

    brcmf_dbg(SDIO, "Enter: F%d\n", func);
    if (func != SDIO_FN_1) {
        return ZX_OK;
    }

    bus_if = dev_to_bus(dev);
    sdiodev = bus_if->bus_priv.sdio;

    brcmf_sdiod_freezer_on(sdiodev);
    brcmf_sdio_wd_timer(sdiodev->bus, false);

    sdio_flags = MMC_PM_KEEP_POWER;
    if (sdiodev->wowl_enabled) {
        if (sdiodev->settings->bus.sdio.oob_irq_supported) {
            enable_irq_wake(sdiodev->irq_handle);
        } else {
            sdio_flags |= MMC_PM_WAKE_SDIO_IRQ;
        }
    }
    if (sdio_set_host_pm_flags(sdiodev, SDIO_FN_1, sdio_flags)) {
        brcmf_err("Failed to set pm_flags %x\n", sdio_flags);
    }
    return ZX_OK;
}

static zx_status_t brcmf_ops_sdio_resume(struct brcmf_device* dev) {
    struct brcmf_bus* bus_if = dev_to_bus(dev);
    struct brcmf_sdio_dev* sdiodev = bus_if->bus_priv.sdio;

    brcmf_dbg(SDIO, "Enter");

    brcmf_sdiod_freezer_off(sdiodev);
    return ZX_OK;
}

static const struct dev_pm_ops brcmf_sdio_pm_ops = {
    .suspend = brcmf_ops_sdio_suspend,
    .resume = brcmf_ops_sdio_resume,
};
#endif /* CONFIG_PM_SLEEP */

void brcmf_sdio_exit(void) {
    brcmf_dbg(SDIO, "Enter\n");

    // TODO(cphoenix): Hook up the actual remove pathway.
    brcmf_ops_sdio_remove(NULL);
}
