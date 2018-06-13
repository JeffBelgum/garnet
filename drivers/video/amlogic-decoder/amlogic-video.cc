// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "amlogic-video.h"

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/platform-defs.h>
#include <ddk/protocol/platform-device.h>
#include <hw/reg.h>
#include <hwreg/bitfields.h>
#include <hwreg/mmio.h>
#include <memory.h>
#include <stdint.h>
#include <zircon/errors.h>
#include <zircon/syscalls.h>

#include <chrono>
#include <memory>
#include <thread>

#include "macros.h"
#include "registers.h"

#if ENABLE_DECODER_TESTS
#include "tests/test_support.h"
#endif

constexpr uint64_t kStreamBufferSize = PAGE_SIZE * 1024;

extern "C" {
zx_status_t amlogic_video_bind(void* ctx, zx_device_t* parent);
}

static zx_status_t amlogic_video_ioctl(void* ctx, uint32_t op,
                                       const void* in_buf, size_t in_len,
                                       void* out_buf, size_t out_len,
                                       size_t* out_actual) {
  return ZX_OK;
}

static zx_protocol_device_t amlogic_video_device_ops = {
    DEVICE_OPS_VERSION,
    .ioctl = amlogic_video_ioctl,
};

// Most buffers should be 64-kbyte aligned.
const uint32_t kBufferAlignShift = 4 + 12;

// These match the regions exported when the bus device was added.
enum MmioRegion {
  kCbus,
  kDosbus,
  kHiubus,
  kAobus,
  kDmc,
};

enum Interrupt {
  kDemuxIrq,
  kParserIrq,
  kDosMbox0Irq,
  kDosMbox1Irq,
  kDosMbox2Irq,
};

AmlogicVideo::~AmlogicVideo() {
  if (parser_interrupt_handle_) {
    zx_interrupt_destroy(parser_interrupt_handle_.get());
    if (parser_interrupt_thread_.joinable())
      parser_interrupt_thread_.join();
  }
  DisableVideoPower();
  io_buffer_release(&mmio_cbus_);
  io_buffer_release(&mmio_dosbus_);
  io_buffer_release(&mmio_hiubus_);
  io_buffer_release(&mmio_aobus_);
  io_buffer_release(&mmio_dmc_);
  io_buffer_release(&stream_buffer_);
}

void AmlogicVideo::EnableClockGate() {
  HhiGclkMpeg0::Get()
      .ReadFrom(hiubus_.get())
      .set_dos(true)
      .WriteTo(hiubus_.get());
  HhiGclkMpeg1::Get()
      .ReadFrom(hiubus_.get())
      .set_u_parser_top(true)
      .set_aiu(0xff)
      .set_demux(true)
      .set_audio_in(true)
      .WriteTo(hiubus_.get());
  HhiGclkMpeg2::Get()
      .ReadFrom(hiubus_.get())
      .set_vpu_interrupt(true)
      .WriteTo(hiubus_.get());
}

void AmlogicVideo::DisableClockGate() {
  HhiGclkMpeg2::Get()
      .ReadFrom(hiubus_.get())
      .set_vpu_interrupt(false)
      .WriteTo(hiubus_.get());
  HhiGclkMpeg1::Get()
      .ReadFrom(hiubus_.get())
      .set_u_parser_top(false)
      .set_aiu(0)
      .set_demux(false)
      .set_audio_in(false)
      .WriteTo(hiubus_.get());
  HhiGclkMpeg0::Get()
      .ReadFrom(hiubus_.get())
      .set_dos(false)
      .WriteTo(hiubus_.get());
}

void AmlogicVideo::EnableVideoPower() {
  {
    auto temp = AoRtiGenPwrSleep0::Get().ReadFrom(aobus_.get());
    temp.set_reg_value(temp.reg_value() & ~0xc);
    temp.WriteTo(aobus_.get());
  }
  zx_nanosleep(zx_deadline_after(ZX_USEC(10)));

  DosSwReset0::Get().FromValue(0xfffffffc).WriteTo(dosbus_.get());
  DosSwReset0::Get().FromValue(0).WriteTo(dosbus_.get());

  EnableClockGate();

  HhiVdecClkCntl::Get().FromValue(0).set_vdec_en(true).set_vdec_sel(3).WriteTo(
      hiubus_.get());
  DosGclkEn::Get().FromValue(0x3ff).WriteTo(dosbus_.get());
  DosMemPdVdec::Get().FromValue(0).WriteTo(dosbus_.get());
  {
    auto temp = AoRtiGenPwrIso0::Get().ReadFrom(aobus_.get());
    temp.set_reg_value(temp.reg_value() & ~0xc0);
    temp.WriteTo(aobus_.get());
  }
  DosVdecMcrccStallCtrl::Get().FromValue(0).WriteTo(dosbus_.get());
  DmcReqCtrl::Get().ReadFrom(dmc_.get()).set_vdec(true).WriteTo(dmc_.get());
  video_power_enabled_ = true;
}

void AmlogicVideo::DisableVideoPower() {
  if (!video_power_enabled_)
    return;
  video_power_enabled_ = false;
  DmcReqCtrl::Get().ReadFrom(dmc_.get()).set_vdec(false).WriteTo(dmc_.get());
  zx_nanosleep(zx_deadline_after(ZX_USEC(10)));
  {
    auto temp = AoRtiGenPwrIso0::Get().ReadFrom(aobus_.get());
    temp.set_reg_value(temp.reg_value() | 0xc0);
    temp.WriteTo(aobus_.get());
  }
  DosMemPdVdec::Get().FromValue(~0u).WriteTo(dosbus_.get());
  HhiVdecClkCntl::Get().FromValue(0).set_vdec_en(false).set_vdec_sel(3).WriteTo(
      hiubus_.get());

  {
    auto temp = AoRtiGenPwrSleep0::Get().ReadFrom(aobus_.get());
    temp.set_reg_value(temp.reg_value() | 0xc);
    temp.WriteTo(aobus_.get());
  }
  DisableClockGate();
}

zx_status_t AmlogicVideo::LoadDecoderFirmware(uint8_t* data, uint32_t size) {
  Mpsr::Get().FromValue(0).WriteTo(dosbus_.get());
  Cpsr::Get().FromValue(0).WriteTo(dosbus_.get());
  io_buffer_t firmware_buffer;
  const uint32_t kFirmwareSize = 4 * 4096;
  zx_status_t status = io_buffer_init_aligned(&firmware_buffer, bti_.get(),
                                              kFirmwareSize, kBufferAlignShift,
                                              IO_BUFFER_RW | IO_BUFFER_CONTIG);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed to make firmware buffer");
    return status;
  }

  memcpy(io_buffer_virt(&firmware_buffer), data, std::min(size, kFirmwareSize));
  io_buffer_cache_flush(&firmware_buffer, 0, kFirmwareSize);

  ImemDmaAdr::Get()
      .FromValue(static_cast<uint32_t>(io_buffer_phys(&firmware_buffer)))
      .WriteTo(dosbus_.get());
  ImemDmaCount::Get()
      .FromValue(kFirmwareSize / sizeof(uint32_t))
      .WriteTo(dosbus_.get());
  ImemDmaCtrl::Get().FromValue(0x8000 | (7 << 16)).WriteTo(dosbus_.get());
  auto start = std::chrono::high_resolution_clock::now();
  auto timeout =
      std::chrono::duration_cast<std::chrono::high_resolution_clock::duration>(
          std::chrono::seconds(1));
  while (ImemDmaCtrl::Get().ReadFrom(dosbus_.get()).reg_value() & 0x8000) {
    if (std::chrono::high_resolution_clock::now() - start >= timeout) {
      DECODE_ERROR("Failed to load microcode.");
      return ZX_ERR_TIMED_OUT;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  io_buffer_release(&firmware_buffer);
  return ZX_OK;
}

zx_status_t AmlogicVideo::InitializeStreamBuffer() {
  zx_status_t status = io_buffer_init_aligned(
      &stream_buffer_, bti_.get(), kStreamBufferSize, kBufferAlignShift,
      IO_BUFFER_RW | IO_BUFFER_CONTIG);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed to make video fifo");
    return ZX_ERR_NO_MEMORY;
  }

  io_buffer_cache_flush(&stream_buffer_, 0, kStreamBufferSize);
  VldMemVififoControl::Get().FromValue(0).WriteTo(dosbus_.get());
  VldMemVififoWrapCount::Get().FromValue(0).WriteTo(dosbus_.get());

  DosSwReset0::Get().FromValue(1 << 4).WriteTo(dosbus_.get());
  DosSwReset0::Get().FromValue(0).WriteTo(dosbus_.get());

  Reset0Register::Get().ReadFrom(reset_.get());
  PowerCtlVld::Get().FromValue(1 << 4).WriteTo(dosbus_.get());
  uint32_t buffer_address =
      static_cast<uint32_t>(io_buffer_phys(&stream_buffer_));

  VldMemVififoStartPtr::Get().FromValue(buffer_address).WriteTo(dosbus_.get());
  VldMemVififoCurrPtr::Get().FromValue(buffer_address).WriteTo(dosbus_.get());
  VldMemVififoEndPtr::Get()
      .FromValue(buffer_address + kStreamBufferSize - 8)
      .WriteTo(dosbus_.get());
  VldMemVififoControl::Get().FromValue(0).set_init(true).WriteTo(dosbus_.get());
  VldMemVififoControl::Get().FromValue(0).WriteTo(dosbus_.get());
  VldMemVififoBufCntl::Get().FromValue(0).set_manual(true).WriteTo(
      dosbus_.get());
  VldMemVififoWP::Get().FromValue(buffer_address).WriteTo(dosbus_.get());
  VldMemVififoBufCntl::Get()
      .FromValue(0)
      .set_manual(true)
      .set_init(true)
      .WriteTo(dosbus_.get());
  VldMemVififoBufCntl::Get().FromValue(0).set_manual(true).WriteTo(
      dosbus_.get());
  VldMemVififoControl::Get()
      .FromValue(0)
      .set_upper(0x11)
      .set_fill_on_level(true)
      .set_fill_en(true)
      .set_empty_en(true)
      .WriteTo(dosbus_.get());

  return ZX_OK;
}

// This parser handles MPEG elementary streams.
zx_status_t AmlogicVideo::InitializeEsParser() {
  Reset1Register::Get().FromValue(0).set_parser(true).WriteTo(reset_.get());
  FecInputControl::Get().FromValue(0).WriteTo(demux_.get());
  TsHiuCtl::Get()
      .ReadFrom(demux_.get())
      .set_use_hi_bsf_interface(false)
      .WriteTo(demux_.get());
  TsHiuCtl2::Get()
      .ReadFrom(demux_.get())
      .set_use_hi_bsf_interface(false)
      .WriteTo(demux_.get());
  TsHiuCtl3::Get()
      .ReadFrom(demux_.get())
      .set_use_hi_bsf_interface(false)
      .WriteTo(demux_.get());
  TsFileConfig::Get()
      .ReadFrom(demux_.get())
      .set_ts_hiu_enable(false)
      .WriteTo(demux_.get());
  ParserConfig::Get()
      .FromValue(0)
      .set_pfifo_empty_cnt(10)
      .set_max_es_write_cycle(1)
      .set_max_fetch_cycle(16)
      .WriteTo(parser_.get());
  PfifoRdPtr::Get().FromValue(0).WriteTo(parser_.get());
  PfifoWrPtr::Get().FromValue(0).WriteTo(parser_.get());
  constexpr uint32_t kEsStartCodePattern = 0x00000100;
  constexpr uint32_t kEsStartCodeMask = 0x0000ff00;
  ParserSearchPattern::Get()
      .FromValue(kEsStartCodePattern)
      .WriteTo(parser_.get());
  ParserSearchMask::Get().FromValue(kEsStartCodeMask).WriteTo(parser_.get());

  ParserConfig::Get()
      .FromValue(0)
      .set_pfifo_empty_cnt(10)
      .set_max_es_write_cycle(1)
      .set_max_fetch_cycle(16)
      .set_startcode_width(ParserConfig::kWidth24)
      .set_pfifo_access_width(ParserConfig::kWidth8)
      .WriteTo(parser_.get());

  ParserControl::Get()
      .FromValue(ParserControl::kAutoSearch)
      .WriteTo(parser_.get());

  // Set up output fifo.
  uint32_t buffer_address = truncate_to_32(io_buffer_phys(&stream_buffer_));

  ParserVideoStartPtr::Get().FromValue(buffer_address).WriteTo(parser_.get());
  ParserVideoEndPtr::Get()
      .FromValue(buffer_address + kStreamBufferSize - 8)
      .WriteTo(parser_.get());
  ParserEsControl::Get()
      .ReadFrom(parser_.get())
      .set_video_manual_read_ptr_update(false)
      .WriteTo(parser_.get());
  VldMemVififoBufCntl::Get().FromValue(0).set_init(true).WriteTo(dosbus_.get());
  VldMemVififoBufCntl::Get().FromValue(0).WriteTo(dosbus_.get());

  DosGenCtrl0::Get().FromValue(0).WriteTo(dosbus_.get());

  parser_interrupt_thread_ = std::thread([this]() {
    DLOG("Starting parser thread\n");
    while (true) {
      zx_time_t time;
      zx_status_t zx_status =
          zx_interrupt_wait(parser_interrupt_handle_.get(), &time);
      if (zx_status != ZX_OK)
        return;

      auto status = ParserIntStatus::Get().ReadFrom(parser_.get());
      // Clear interrupt.
      status.WriteTo(parser_.get());
      DLOG("Got Parser interrupt status %x\n", status.reg_value());
      if (status.fetch_complete()) {
        PfifoRdPtr::Get().FromValue(0).WriteTo(parser_.get());
        PfifoWrPtr::Get().FromValue(0).WriteTo(parser_.get());
        parser_finished_promise_.set_value();
      }
    }
  });

  ParserIntStatus::Get().FromValue(0xffff).WriteTo(parser_.get());
  ParserIntEnable::Get().FromValue(0).set_host_en_fetch_complete(true).WriteTo(
      parser_.get());

  return ZX_OK;
}

zx_status_t AmlogicVideo::ParseVideo(void* data, uint32_t len) {
  io_buffer_t input_file;
  zx_status_t status = io_buffer_init(&input_file, bti_.get(), len,
                                      IO_BUFFER_RW | IO_BUFFER_CONTIG);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed to create input file");
    return ZX_ERR_NO_MEMORY;
  }
  PfifoRdPtr::Get().FromValue(0).WriteTo(parser_.get());
  PfifoWrPtr::Get().FromValue(0).WriteTo(parser_.get());

  ParserControl::Get()
      .ReadFrom(parser_.get())
      .set_es_pack_size(len)
      .WriteTo(parser_.get());
  ParserControl::Get()
      .ReadFrom(parser_.get())
      .set_type(0)
      .set_write(true)
      .set_command(ParserControl::kAutoSearch)
      .WriteTo(parser_.get());

  memcpy(io_buffer_virt(&input_file), data, len);
  io_buffer_cache_flush(&input_file, 0, len);

  parser_finished_promise_ = std::promise<void>();
  ParserFetchAddr::Get()
      .FromValue(truncate_to_32(io_buffer_phys(&input_file)))
      .WriteTo(parser_.get());
  ParserFetchCmd::Get().FromValue(0).set_len(len).set_fetch_endian(7).WriteTo(
      parser_.get());

  auto future_status =
      parser_finished_promise_.get_future().wait_for(std::chrono::seconds(1));
  if (future_status != std::future_status::ready) {
    DECODE_ERROR("Parser timed out\n");
    ParserFetchCmd::Get().FromValue(0).WriteTo(parser_.get());
    io_buffer_release(&input_file);
    return ZX_ERR_TIMED_OUT;
  }
  io_buffer_release(&input_file);

  return ZX_OK;
}

zx_status_t AmlogicVideo::InitRegisters(zx_device_t* parent) {
  parent_ = parent;

  zx_status_t status =
      device_get_protocol(parent_, ZX_PROTOCOL_PLATFORM_DEV, &pdev_);

  if (status != ZX_OK) {
    DECODE_ERROR("Failed to get parent protocol");
    return ZX_ERR_NO_MEMORY;
  }
  pdev_device_info_t info;
  status = pdev_get_device_info(&pdev_, &info);
  if (status != ZX_OK) {
    DECODE_ERROR("pdev_get_device_info failed");
    return status;
  }
  switch (info.pid) {
    case PDEV_PID_AMLOGIC_S912:
      device_type_ = DeviceType::kGXM;
      break;
    case PDEV_PID_AMLOGIC_S905D2:
      device_type_ = DeviceType::kG12A;
      break;
    default:
      DECODE_ERROR("Unknown soc pid: %d\n", info.pid);
      return ZX_ERR_INVALID_ARGS;
  }

  status = pdev_map_mmio_buffer(&pdev_, kCbus, ZX_CACHE_POLICY_UNCACHED_DEVICE,
                                &mmio_cbus_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed map cbus");
    return ZX_ERR_NO_MEMORY;
  }
  status = pdev_map_mmio_buffer(&pdev_, kDosbus,
                                ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio_dosbus_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed map dosbus");
    return ZX_ERR_NO_MEMORY;
  }
  status = pdev_map_mmio_buffer(&pdev_, kHiubus,
                                ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio_hiubus_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed map hiubus");
    return ZX_ERR_NO_MEMORY;
  }
  status = pdev_map_mmio_buffer(&pdev_, kAobus, ZX_CACHE_POLICY_UNCACHED_DEVICE,
                                &mmio_aobus_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed map aobus");
    return ZX_ERR_NO_MEMORY;
  }
  status = pdev_map_mmio_buffer(&pdev_, kDmc, ZX_CACHE_POLICY_UNCACHED_DEVICE,
                                &mmio_dmc_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed map dmc");
    return ZX_ERR_NO_MEMORY;
  }
  status = pdev_map_interrupt(&pdev_, kParserIrq,
                              parser_interrupt_handle_.reset_and_get_address());
  if (status != ZX_OK) {
    DECODE_ERROR("Failed get parser interrupt");
    return ZX_ERR_NO_MEMORY;
  }
  status = pdev_map_interrupt(&pdev_, kDosMbox1Irq,
                              vdec1_interrupt_handle_.reset_and_get_address());
  if (status != ZX_OK) {
    DECODE_ERROR("Failed get vdec interrupt");
    return ZX_ERR_NO_MEMORY;
  }
  status = pdev_get_bti(&pdev_, 0, bti_.reset_and_get_address());
  if (status != ZX_OK) {
    DECODE_ERROR("Failed get bti");
    return ZX_ERR_NO_MEMORY;
  }
  cbus_ = std::make_unique<CbusRegisterIo>(io_buffer_virt(&mmio_cbus_));
  dosbus_ = std::make_unique<DosRegisterIo>(io_buffer_virt(&mmio_dosbus_));
  hiubus_ = std::make_unique<HiuRegisterIo>(io_buffer_virt(&mmio_hiubus_));
  aobus_ = std::make_unique<AoRegisterIo>(io_buffer_virt(&mmio_aobus_));
  dmc_ = std::make_unique<DmcRegisterIo>(io_buffer_virt(&mmio_dmc_));

  int64_t reset_register_offset = 0;
  int64_t parser_register_offset = 0;
  int64_t demux_register_offset = 0;
  if (device_type_ == DeviceType::kG12A) {
    // Some portions of the cbus moved in newer versions (TXL and later).
    reset_register_offset = (0x0401 - 0x1101);
    parser_register_offset = 0x3800 - 0x2900;
    demux_register_offset = 0x1800 - 0x1600;
  }
  auto cbus_base = static_cast<volatile uint32_t*>(io_buffer_virt(&mmio_cbus_));
  reset_ = std::make_unique<ResetRegisterIo>(cbus_base + reset_register_offset);
  parser_ =
      std::make_unique<ParserRegisterIo>(cbus_base + parser_register_offset);
  demux_ = std::make_unique<DemuxRegisterIo>(cbus_base + demux_register_offset);

  firmware_ = std::make_unique<FirmwareBlob>();
  status = firmware_->LoadFirmware(parent_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed load firmware\n");
    return status;
  }

  return ZX_OK;
}

zx_status_t AmlogicVideo::Bind() {
  device_add_args_t vc_video_args = {};
  vc_video_args.version = DEVICE_ADD_ARGS_VERSION;
  vc_video_args.name = "amlogic_video";
  vc_video_args.ctx = this;
  vc_video_args.ops = &amlogic_video_device_ops;

  zx_status_t status = device_add(parent_, &vc_video_args, &device_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed to bind device");
    return ZX_ERR_NO_MEMORY;
  }
  return ZX_OK;
}

zx_status_t AmlogicVideo::InitDecoder() {
  EnableVideoPower();
  zx_status_t status = InitializeStreamBuffer();
  if (status != ZX_OK) return status;

  {
    uint8_t* firmware_data;
    uint32_t firmware_size;
    status = firmware_->GetFirmwareData(FirmwareBlob::FirmwareType::kMPEG12,
                                        &firmware_data, &firmware_size);
    if (status != ZX_OK) {
      DECODE_ERROR("Failed load firmware\n");
      return status;
    }
    status = LoadDecoderFirmware(firmware_data, firmware_size);
    if (status != ZX_OK)
      return status;
  }

  return ZX_OK;
}

zx_status_t amlogic_video_bind(void* ctx, zx_device_t* parent) {
#if ENABLE_DECODER_TESTS
  TestSupport::set_parent_device(parent);
  TestSupport::RunAllTests();
#endif

  auto video = std::make_unique<AmlogicVideo>();
  if (!video) {
    DECODE_ERROR("Failed to create AmlogicVideo");
    return ZX_ERR_NO_MEMORY;
  }

  zx_status_t status = video->InitRegisters(parent);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed to initialize registers");
    return status;
  }

  status = video->InitDecoder();
  if (status != ZX_OK) {
    DECODE_ERROR("Failed to initialize decoder");
    return status;
  }

  status = video->Bind();
  if (status != ZX_OK) {
    DECODE_ERROR("Failed to bind device");
    return status;
  }

  video.release();
  zxlogf(INFO, "[amlogic_video_bind] bound\n");
  return ZX_OK;
}
