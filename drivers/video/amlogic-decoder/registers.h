// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef REGISTERS_H_
#define REGISTERS_H_

#include <hwreg/bitfields.h>
#include <hwreg/mmio.h>

template <class RegType>
class TypedRegisterAddr;

// Typed registers can be used only with a specific type of RegisterIo.
template <class MmioType, class DerivedType, class IntType,
          class PrinterState = void>
class TypedRegisterBase
    : public hwreg::RegisterBase<DerivedType, IntType, PrinterState> {
 public:
  using SelfType = DerivedType;
  using ValueType = IntType;
  using Mmio = MmioType;
  using AddrType = TypedRegisterAddr<SelfType>;
  SelfType& ReadFrom(MmioType* reg_io) {
    return hwreg::RegisterBase<DerivedType, IntType, PrinterState>::ReadFrom(
        static_cast<hwreg::RegisterIo*>(reg_io));
  }
  SelfType& WriteTo(MmioType* reg_io) {
    return hwreg::RegisterBase<DerivedType, IntType, PrinterState>::WriteTo(
        static_cast<hwreg::RegisterIo*>(reg_io));
  }
};

template <class RegType>
class TypedRegisterAddr : public hwreg::RegisterAddr<RegType> {
 public:
  TypedRegisterAddr(uint32_t reg_addr)
      : hwreg::RegisterAddr<RegType>(reg_addr) {}

  RegType ReadFrom(typename RegType::Mmio* reg_io) {
    RegType reg;
    reg.set_reg_addr(hwreg::RegisterAddr<RegType>::addr());
    reg.ReadFrom(reg_io);
    return reg;
  }
};

// Cbus does a lot of things, but mainly seems to handle audio and video
// processing.
class CbusRegisterIo : public hwreg::RegisterIo {
 public:
  CbusRegisterIo(volatile void* mmio) : hwreg::RegisterIo(mmio) {}
};

// The DOS bus mainly seems to handle video decoding.
class DosRegisterIo : public hwreg::RegisterIo {
 public:
  DosRegisterIo(volatile void* mmio) : hwreg::RegisterIo(mmio) {}
};

// Aobus communicates with the always-on power management processor.
class AoRegisterIo : public hwreg::RegisterIo {
 public:
  AoRegisterIo(volatile void* mmio) : hwreg::RegisterIo(mmio) {}
};

// Hiubus mainly seems to handle clock control and gating.
class HiuRegisterIo : public hwreg::RegisterIo {
 public:
  HiuRegisterIo(volatile void* mmio) : hwreg::RegisterIo(mmio) {}
};

// The DMC is the DDR memory controller.
class DmcRegisterIo : public hwreg::RegisterIo {
 public:
  DmcRegisterIo(volatile void* mmio) : hwreg::RegisterIo(mmio) {}
};

class ResetRegisterIo : public hwreg::RegisterIo {
 public:
  ResetRegisterIo(volatile void* mmio) : hwreg::RegisterIo(mmio) {}
};

class ParserRegisterIo : public hwreg::RegisterIo {
 public:
  ParserRegisterIo(volatile void* mmio) : hwreg::RegisterIo(mmio) {}
};

class DemuxRegisterIo : public hwreg::RegisterIo {
 public:
  DemuxRegisterIo(volatile void* mmio) : hwreg::RegisterIo(mmio) {}
};

#define DEFINE_REGISTER(name, type, address)                           \
  class name : public TypedRegisterBase<type, name, uint32_t> {        \
   public:                                                             \
    static auto Get() { return TypedRegisterAddr<name>(address * 4); } \
  };

#define REGISTER_NAME(name, type, address)                      \
  class name : public TypedRegisterBase<type, name, uint32_t> { \
   public:                                                      \
    static auto Get() { return AddrType(address * 4); }

// clang-format off
DEFINE_REGISTER(Mpsr, DosRegisterIo, 0x301);
DEFINE_REGISTER(Cpsr, DosRegisterIo, 0x321);
DEFINE_REGISTER(ImemDmaCtrl, DosRegisterIo, 0x340);
DEFINE_REGISTER(ImemDmaAdr, DosRegisterIo, 0x341);
DEFINE_REGISTER(ImemDmaCount, DosRegisterIo, 0x342);
DEFINE_REGISTER(LmemDmaCtrl, DosRegisterIo, 0x0350);
DEFINE_REGISTER(DcacDmaCtrl, DosRegisterIo, 0x0e12);
DEFINE_REGISTER(DosSwReset0, DosRegisterIo, 0x3f00);
DEFINE_REGISTER(DosGclkEn, DosRegisterIo, 0x3f01);
DEFINE_REGISTER(DosMemPdVdec, DosRegisterIo, 0x3f30);
DEFINE_REGISTER(DosVdecMcrccStallCtrl, DosRegisterIo, 0x3f40);

DEFINE_REGISTER(VldMemVififoStartPtr, DosRegisterIo, 0x0c40)
DEFINE_REGISTER(VldMemVififoCurrPtr, DosRegisterIo, 0x0c41)
DEFINE_REGISTER(VldMemVififoEndPtr, DosRegisterIo, 0x0c42)
DEFINE_REGISTER(VldMemVififoBytesAvail, DosRegisterIo, 0x0c43)

REGISTER_NAME(VldMemVififoControl, DosRegisterIo, 0x0c44)
  DEF_FIELD(23, 16, upper)
  DEF_BIT(10, fill_on_level)
  DEF_FIELD(6, 3, endianness)
  DEF_BIT(2, empty_en)
  DEF_BIT(1, fill_en)
  DEF_BIT(0, init)
};

DEFINE_REGISTER(VldMemVififoWP, DosRegisterIo, 0x0c45)
DEFINE_REGISTER(VldMemVififoRP, DosRegisterIo, 0x0c46)
DEFINE_REGISTER(VldMemVififoLevel, DosRegisterIo, 0x0c47)
REGISTER_NAME(VldMemVififoBufCntl, DosRegisterIo, 0x0c48)
    DEF_BIT(1, manual)
    DEF_BIT(0, init)
};
DEFINE_REGISTER(VldMemVififoWrapCount, DosRegisterIo, 0x0c51)
DEFINE_REGISTER(VldMemVififoMemCtl, DosRegisterIo, 0x0c52)

DEFINE_REGISTER(PowerCtlVld, DosRegisterIo, 0x0c08)
DEFINE_REGISTER(DosGenCtrl0, DosRegisterIo, 0x3f02)

DEFINE_REGISTER(McStatus0, DosRegisterIo, 0x0909)
DEFINE_REGISTER(McCtrl1, DosRegisterIo, 0x090b)
DEFINE_REGISTER(DblkCtrl, DosRegisterIo, 0x0951)
DEFINE_REGISTER(DblkStatus, DosRegisterIo, 0x0953)
REGISTER_NAME(MdecPicDcCtrl, DosRegisterIo, 0x098e)
    DEF_BIT(17, nv12_output) // as opposed to 3-plane YUV
    DEF_BIT(31, bit31)
};
DEFINE_REGISTER(MdecPicDcStatus, DosRegisterIo, 0x098f)

DEFINE_REGISTER(MdecSwReset, DosRegisterIo, 0x0984)
DEFINE_REGISTER(MdecPicDcThresh, DosRegisterIo, 0x9b8)

// AvScratch registers are used to communicate with the AMRISC coprocessor.
class AvScratch : public TypedRegisterBase<DosRegisterIo, AvScratch, uint32_t> {
 public:
    static auto Get(uint32_t i) { return AddrType((0x09c0 + i) * 4); }
};

DEFINE_REGISTER(AvScratch0, DosRegisterIo, 0x09c0)
DEFINE_REGISTER(AvScratch1, DosRegisterIo, 0x09c1)
DEFINE_REGISTER(AvScratch2, DosRegisterIo, 0x09c2)
DEFINE_REGISTER(AvScratch3, DosRegisterIo, 0x09c3)
DEFINE_REGISTER(AvScratch4, DosRegisterIo, 0x09c4)
DEFINE_REGISTER(AvScratch5, DosRegisterIo, 0x09c5)
DEFINE_REGISTER(AvScratch6, DosRegisterIo, 0x09c6)
DEFINE_REGISTER(AvScratch7, DosRegisterIo, 0x09c7)
DEFINE_REGISTER(AvScratch8, DosRegisterIo, 0x09c8)
DEFINE_REGISTER(AvScratch9, DosRegisterIo, 0x09c9)
DEFINE_REGISTER(AvScratchA, DosRegisterIo, 0x09ca)
DEFINE_REGISTER(AvScratchB, DosRegisterIo, 0x09cb)
DEFINE_REGISTER(AvScratchC, DosRegisterIo, 0x09cc)
DEFINE_REGISTER(AvScratchD, DosRegisterIo, 0x09cd)
DEFINE_REGISTER(AvScratchE, DosRegisterIo, 0x09ce)
DEFINE_REGISTER(AvScratchF, DosRegisterIo, 0x09cf)
DEFINE_REGISTER(AvScratchG, DosRegisterIo, 0x09d0)
DEFINE_REGISTER(AvScratchH, DosRegisterIo, 0x09d1)
DEFINE_REGISTER(AvScratchI, DosRegisterIo, 0x09d2)
DEFINE_REGISTER(AvScratchJ, DosRegisterIo, 0x09d3)
DEFINE_REGISTER(AvScratchK, DosRegisterIo, 0x09d4)
DEFINE_REGISTER(AvScratchL, DosRegisterIo, 0x09d5)
DEFINE_REGISTER(AvScratchM, DosRegisterIo, 0x09d6)
DEFINE_REGISTER(AvScratchN, DosRegisterIo, 0x09d7)

DEFINE_REGISTER(Mpeg12Reg, DosRegisterIo, 0x0c01)
DEFINE_REGISTER(PscaleCtrl, DosRegisterIo, 0x0911)
DEFINE_REGISTER(PicHeadInfo, DosRegisterIo, 0x0c03)
DEFINE_REGISTER(M4ControlReg, DosRegisterIo, 0x0c29)
DEFINE_REGISTER(VdecAssistMbox1ClrReg, DosRegisterIo, 0x0075)
DEFINE_REGISTER(VdecAssistMbox1Mask, DosRegisterIo, 0x0076)

class AncNCanvasAddr : public TypedRegisterBase<DosRegisterIo, AncNCanvasAddr, uint32_t> {
 public:
    static auto Get(uint32_t i) { return AddrType((0x0990 + i) * 4); }
};

DEFINE_REGISTER(AoRtiGenPwrSleep0, AoRegisterIo, 0x3a);
DEFINE_REGISTER(AoRtiGenPwrIso0, AoRegisterIo, 0x3b);

REGISTER_NAME(HhiGclkMpeg0, HiuRegisterIo, 0x50)
  DEF_BIT(1, dos);
};

REGISTER_NAME(HhiGclkMpeg1, HiuRegisterIo, 0x51)
  DEF_BIT(25, u_parser_top);
  DEF_FIELD(13, 6, aiu);
  DEF_BIT(4, demux);
  DEF_BIT(2, audio_in);
};

REGISTER_NAME(HhiGclkMpeg2, HiuRegisterIo, 0x52)
  DEF_BIT(25, vpu_interrupt);
};

REGISTER_NAME(HhiVdecClkCntl, HiuRegisterIo, 0x78)
  DEF_BIT(8, vdec_en);
  DEF_FIELD(11, 9, vdec_sel);
  DEF_FIELD(6, 0, vdec_div);
};

REGISTER_NAME(DmcReqCtrl, DmcRegisterIo, 0x0)
  DEF_BIT(13, vdec);
};

REGISTER_NAME(DmcCavLutDatal, DmcRegisterIo, 0x12)
    DEF_FIELD(28, 0, addr) // Shifted down by 3
    DEF_FIELD(31, 29, width_lower) // Shifted down by 3
};

REGISTER_NAME(DmcCavLutDatah, DmcRegisterIo, 0x13)
    DEF_FIELD(8, 0, width_upper) // Shifted down by 6
    DEF_FIELD(21, 9, height)
    DEF_BIT(22, wrap_x)
    DEF_BIT(23, wrap_y)
    DEF_FIELD(25, 24, block_mode)
    DEF_FIELD(29, 26, endianness)

    enum {
        kBlockModeLinear = 0,
        kBlockMode32x32 = 1
    };
};

REGISTER_NAME(DmcCavLutAddr, DmcRegisterIo, 0x14)
    DEF_BIT(9, wr_en)
    DEF_BIT(8, rd_en)
    DEF_FIELD(7, 0, index)
};

DEFINE_REGISTER(DmcCavLutRdataL, DmcRegisterIo, 0x15)
DEFINE_REGISTER(DmcCavLutRdataH, DmcRegisterIo, 0x16)

DEFINE_REGISTER(Reset0Register, ResetRegisterIo, 0x1101);
REGISTER_NAME(Reset1Register, ResetRegisterIo, 0x1102)
  DEF_BIT(8, parser)
};
DEFINE_REGISTER(FecInputControl, DemuxRegisterIo, 0x1602)

REGISTER_NAME(TsHiuCtl, DemuxRegisterIo, 0x1625)
  DEF_BIT(7, use_hi_bsf_interface)
};
REGISTER_NAME(TsHiuCtl2, DemuxRegisterIo, 0x1675)
  DEF_BIT(7, use_hi_bsf_interface)
};
REGISTER_NAME(TsHiuCtl3, DemuxRegisterIo, 0x16c5)
  DEF_BIT(7, use_hi_bsf_interface)
};

REGISTER_NAME(TsFileConfig, DemuxRegisterIo, 0x16f2)
  DEF_BIT(5, ts_hiu_enable)
};

REGISTER_NAME(ParserConfig, ParserRegisterIo, 0x2965)
  enum {
    kWidth8 = 0,
    kWidth16 = 1,
    kWidth24 = 2,
    kWidth32 = 3,
  };
  DEF_FIELD(23, 16, pfifo_empty_cnt)
  DEF_FIELD(15, 12, max_es_write_cycle)
  DEF_FIELD(11, 10, startcode_width)
  DEF_FIELD(9, 8, pfifo_access_width)
  DEF_FIELD(7, 0, max_fetch_cycle)
};
DEFINE_REGISTER(PfifoWrPtr, ParserRegisterIo, 0x2966)
DEFINE_REGISTER(PfifoRdPtr, ParserRegisterIo, 0x2967)
DEFINE_REGISTER(ParserSearchPattern, ParserRegisterIo, 0x2969)
DEFINE_REGISTER(ParserSearchMask, ParserRegisterIo, 0x296a)

REGISTER_NAME(ParserControl, ParserRegisterIo, 0x2960)
  enum {
    kSearch = (1 << 1),
    kStart = (1 << 0),
    kAutoSearch = kSearch | kStart,
  };
  DEF_FIELD(31, 8, es_pack_size)
  DEF_FIELD(7, 6, type)
  DEF_BIT(5, write)
  DEF_FIELD(4, 0, command)
};

DEFINE_REGISTER(ParserVideoStartPtr, ParserRegisterIo, 0x2980)
DEFINE_REGISTER(ParserVideoEndPtr, ParserRegisterIo, 0x2981)

REGISTER_NAME(ParserEsControl, ParserRegisterIo, 0x2977)
  DEF_BIT(0, video_manual_read_ptr_update)
};

REGISTER_NAME(ParserIntStatus, ParserRegisterIo, 0x296c)
  DEF_BIT(7, fetch_complete)
};
REGISTER_NAME(ParserIntEnable, ParserRegisterIo, 0x296b)
  DEF_BIT(8, host_en_start_code_found)
  DEF_BIT(15, host_en_fetch_complete)
};

DEFINE_REGISTER(ParserFetchAddr, ParserRegisterIo, 0x2961)
REGISTER_NAME(ParserFetchCmd, ParserRegisterIo, 0x2962)
  DEF_FIELD(29, 27, fetch_endian)
  DEF_FIELD(26, 0, len)
};

// clang-format on

#undef REGISTER_NAME
#undef DEFINE_REGISTER

#endif  // REGISTERS_H_
