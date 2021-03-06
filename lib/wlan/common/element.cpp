// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <wlan/common/element.h>

namespace wlan {

ElementReader::ElementReader(const uint8_t* buf, size_t len) : buf_(buf), len_(len) {}

bool ElementReader::is_valid() const {
    // Test in order.
    // Test 1. If at least ElementHeader can be safely read: 2 bytes test.
    if (len_ < offset_ + sizeof(ElementHeader)) return false;

    // Test 2. If the full element can be safely read.
    // NextElementLen() needs pass from Test 1.
    if (len_ < offset_ + NextElementLen()) return false;

    // Add more test here.
    return true;
}

const ElementHeader* ElementReader::peek() const {
    if (!is_valid()) return nullptr;
    return reinterpret_cast<const ElementHeader*>(buf_ + offset_);
}

size_t ElementReader::NextElementLen() const {
    auto hdr = reinterpret_cast<const ElementHeader*>(buf_ + offset_);
    return sizeof(ElementHeader) + hdr->len;
}

// TODO(hahnr): Support dot11MultiBSSIDActivated is true.
bool TimElement::traffic_buffered(uint16_t aid) const {
    // Illegal arguments or no partial virtual bitmap. No traffic buffered.
    if (aid >= kMaxLenBmp * 8 || hdr.len < kMinLen) return false;
    if (!tim_hdr.bmp_ctrl.offset() && hdr.len == kMinLen) return false;

    // Safe to use uint8 since offset is 7 bits.
    uint8_t n1 = tim_hdr.bmp_ctrl.offset() << 1;
    uint16_t n2 = (hdr.len - kMinLen) + n1;
    if (n2 > static_cast<uint16_t>(kMaxLenBmp)) return false;

    // No traffic buffered for aid.
    uint8_t octet = aid / 8;
    if (octet < n1 || octet > n2) return false;

    // Traffic might be buffered for aid
    // Bounds are not exceeded since (n2 - n1 + 4) = hdr.len, and
    // n1 <= octet <= n2, and hdr.len >= 4. This simplifies to:
    // 0 <=  octet - n1 <= (hdr.len - 4)
    return bmp[octet - n1] & (1 << (aid % 8));
}

// The macros below assumes that the two data structures being intersected be named lhs and rhs.
// Both of them must be the same sub-class of common::BitField<>.
#define SET_BITFIELD_MIN(element, field) \
    element.set_##field(std::min(lhs.element.field(), rhs.element.field()))
#define SET_BITFIELD_MAX(element, field) \
    element.set_##field(std::max(lhs.element.field(), rhs.element.field()))
#define SET_BITFIELD_AND(element, field) \
    element.set_##field(lhs.element.field() & rhs.element.field())

SupportedMcsSet IntersectMcs(const SupportedMcsSet& lhs, const SupportedMcsSet& rhs) {
    // Find an intersection.
    // Perform bitwise-AND on bitmask fields, which represent MCS
    // Take minimum of numeric values

    auto result = SupportedMcsSet{};
    auto& rx_mcs_head = result.rx_mcs_head;
    SET_BITFIELD_AND(rx_mcs_head, bitmask);

    auto& rx_mcs_tail = result.rx_mcs_tail;
    SET_BITFIELD_AND(rx_mcs_tail, bitmask);
    SET_BITFIELD_MIN(rx_mcs_tail, highest_rate);

    auto& tx_mcs = result.tx_mcs;
    SET_BITFIELD_AND(tx_mcs, set_defined);
    SET_BITFIELD_AND(tx_mcs, rx_diff);
    SET_BITFIELD_MIN(tx_mcs, max_ss);
    SET_BITFIELD_AND(tx_mcs, ueqm);

    return result;
}

// Takes two HtCapabilities/VhtCapabilities, typically, one from the device and the other from the
// air, and find the capabilities supported by both of them.
HtCapabilities IntersectHtCap(const HtCapabilities& lhs, const HtCapabilities& rhs) {
    auto htc = HtCapabilities{};

    auto& ht_cap_info = htc.ht_cap_info;
    SET_BITFIELD_AND(ht_cap_info, ldpc_coding_cap);
    SET_BITFIELD_AND(ht_cap_info, chan_width_set);

    // TODO(NET-1267): Revisit SM power save mode when necessary. IEEE 802.11-2016 11.2.6
    if (lhs.ht_cap_info.sm_power_save() == HtCapabilityInfo::SmPowerSave::DISABLED ||
        rhs.ht_cap_info.sm_power_save() == HtCapabilityInfo::SmPowerSave::DISABLED) {
        ht_cap_info.set_sm_power_save(HtCapabilityInfo::SmPowerSave::DISABLED);
    } else {
        // Assuming a device supporting dynamic power save will support static power save
        SET_BITFIELD_MIN(ht_cap_info, sm_power_save);
    }

    SET_BITFIELD_AND(ht_cap_info, greenfield);
    SET_BITFIELD_AND(ht_cap_info, short_gi_20);
    SET_BITFIELD_AND(ht_cap_info, short_gi_40);
    SET_BITFIELD_AND(ht_cap_info, tx_stbc);

    SET_BITFIELD_MIN(ht_cap_info, rx_stbc);

    SET_BITFIELD_AND(ht_cap_info, delayed_block_ack);
    SET_BITFIELD_AND(ht_cap_info, max_amsdu_len);
    SET_BITFIELD_AND(ht_cap_info, dsss_in_40);
    SET_BITFIELD_AND(ht_cap_info, intolerant_40);
    SET_BITFIELD_AND(ht_cap_info, lsig_txop_protect);

    auto& ampdu_params = htc.ampdu_params;
    SET_BITFIELD_MIN(ampdu_params, exponent);

    SET_BITFIELD_MAX(ampdu_params, min_start_spacing);

    htc.mcs_set = IntersectMcs(lhs.mcs_set, rhs.mcs_set);

    auto& ht_ext_cap = htc.ht_ext_cap;
    SET_BITFIELD_AND(ht_ext_cap, pco);

    if (lhs.ht_ext_cap.pco_transition() == HtExtCapabilities::PcoTransitionTime::PCO_RESERVED ||
        rhs.ht_ext_cap.pco_transition() == HtExtCapabilities::PcoTransitionTime::PCO_RESERVED) {
        ht_ext_cap.set_pco_transition(HtExtCapabilities::PcoTransitionTime::PCO_RESERVED);
    } else {
        SET_BITFIELD_MAX(ht_ext_cap, pco_transition);
    }
    SET_BITFIELD_MIN(ht_ext_cap, mcs_feedback);

    SET_BITFIELD_AND(ht_ext_cap, htc_ht_support);
    SET_BITFIELD_AND(ht_ext_cap, rd_responder);

    auto& txbf_cap = htc.txbf_cap;
    SET_BITFIELD_AND(txbf_cap, implicit_rx);
    SET_BITFIELD_AND(txbf_cap, rx_stag_sounding);
    SET_BITFIELD_AND(txbf_cap, tx_stag_sounding);
    SET_BITFIELD_AND(txbf_cap, rx_ndp);
    SET_BITFIELD_AND(txbf_cap, tx_ndp);
    SET_BITFIELD_AND(txbf_cap, implicit);

    SET_BITFIELD_MIN(txbf_cap, calibration);

    SET_BITFIELD_AND(txbf_cap, csi);

    SET_BITFIELD_AND(txbf_cap, noncomp_steering);
    SET_BITFIELD_AND(txbf_cap, comp_steering);

    // IEEE 802.11-2016 Table 9-166
    // xxx_feedback behaves like bitmask for delayed and immediate feedback
    SET_BITFIELD_AND(txbf_cap, csi_feedback);
    SET_BITFIELD_AND(txbf_cap, noncomp_feedback);
    SET_BITFIELD_AND(txbf_cap, comp_feedback);

    SET_BITFIELD_MIN(txbf_cap, min_grouping);
    SET_BITFIELD_MIN(txbf_cap, csi_antennas);

    SET_BITFIELD_MIN(txbf_cap, noncomp_steering_ants);
    SET_BITFIELD_MIN(txbf_cap, comp_steering_ants);
    SET_BITFIELD_MIN(txbf_cap, csi_rows);
    SET_BITFIELD_MIN(txbf_cap, chan_estimation);

    auto& asel_cap = htc.asel_cap;
    SET_BITFIELD_AND(asel_cap, asel);
    SET_BITFIELD_AND(asel_cap, csi_feedback_tx_asel);
    SET_BITFIELD_AND(asel_cap, ant_idx_feedback_tx_asel);
    SET_BITFIELD_AND(asel_cap, explicit_csi_feedback);
    SET_BITFIELD_AND(asel_cap, antenna_idx_feedback);
    SET_BITFIELD_AND(asel_cap, rx_asel);
    SET_BITFIELD_AND(asel_cap, tx_sounding_ppdu);

    return htc;
}

VhtCapabilities IntersectVhtCap(const VhtCapabilities& lhs, const VhtCapabilities& rhs) {
    auto vhtc = VhtCapabilities{};

    auto& vht_cap_info = vhtc.vht_cap_info;
    SET_BITFIELD_MIN(vht_cap_info, max_mpdu_len);
    // TODO(NET-1267): IEEE 802.11-2016 Table 9-250. Revisit when necessary
    // supported_cbw_set needs to be considered in conjunction with ext_nss_bw below
    SET_BITFIELD_MIN(vht_cap_info, supported_cbw_set);

    SET_BITFIELD_AND(vht_cap_info, rx_ldpc);
    SET_BITFIELD_AND(vht_cap_info, sgi_cbw80);
    SET_BITFIELD_AND(vht_cap_info, sgi_cbw160);
    SET_BITFIELD_AND(vht_cap_info, tx_stbc);

    SET_BITFIELD_MIN(vht_cap_info, rx_stbc);

    SET_BITFIELD_AND(vht_cap_info, su_bfer);
    SET_BITFIELD_AND(vht_cap_info, su_bfee);

    SET_BITFIELD_MIN(vht_cap_info, bfee_sts);
    SET_BITFIELD_MIN(vht_cap_info, num_sounding);

    SET_BITFIELD_AND(vht_cap_info, mu_bfer);
    SET_BITFIELD_AND(vht_cap_info, mu_bfee);
    SET_BITFIELD_AND(vht_cap_info, txop_ps);
    SET_BITFIELD_AND(vht_cap_info, htc_vht);

    SET_BITFIELD_MIN(vht_cap_info, max_ampdu_exp);
    SET_BITFIELD_MIN(vht_cap_info, link_adapt);

    SET_BITFIELD_AND(vht_cap_info, rx_ant_pattern);
    SET_BITFIELD_AND(vht_cap_info, tx_ant_pattern);

    SET_BITFIELD_MIN(vht_cap_info, ext_nss_bw);

    auto& vht_mcs_nss = vhtc.vht_mcs_nss;
    SET_BITFIELD_MIN(vht_mcs_nss, rx_max_mcs_ss1);
    SET_BITFIELD_MIN(vht_mcs_nss, rx_max_mcs_ss2);
    SET_BITFIELD_MIN(vht_mcs_nss, rx_max_mcs_ss3);
    SET_BITFIELD_MIN(vht_mcs_nss, rx_max_mcs_ss4);
    SET_BITFIELD_MIN(vht_mcs_nss, rx_max_mcs_ss5);
    SET_BITFIELD_MIN(vht_mcs_nss, rx_max_mcs_ss6);
    SET_BITFIELD_MIN(vht_mcs_nss, rx_max_mcs_ss7);
    SET_BITFIELD_MIN(vht_mcs_nss, rx_max_mcs_ss8);
    SET_BITFIELD_MIN(vht_mcs_nss, rx_max_data_rate);
    SET_BITFIELD_MIN(vht_mcs_nss, max_nsts);
    SET_BITFIELD_MIN(vht_mcs_nss, tx_max_mcs_ss1);
    SET_BITFIELD_MIN(vht_mcs_nss, tx_max_mcs_ss2);
    SET_BITFIELD_MIN(vht_mcs_nss, tx_max_mcs_ss3);
    SET_BITFIELD_MIN(vht_mcs_nss, tx_max_mcs_ss4);
    SET_BITFIELD_MIN(vht_mcs_nss, tx_max_mcs_ss5);
    SET_BITFIELD_MIN(vht_mcs_nss, tx_max_mcs_ss6);
    SET_BITFIELD_MIN(vht_mcs_nss, tx_max_mcs_ss7);
    SET_BITFIELD_MIN(vht_mcs_nss, tx_max_mcs_ss8);
    SET_BITFIELD_MIN(vht_mcs_nss, tx_max_data_rate);

    SET_BITFIELD_AND(vht_mcs_nss, ext_nss_bw);

    return vhtc;
}

#undef SET_BITFIELD_AND
#undef SET_BITFIELD_MIN
#undef SET_BITFIELD_MAX

std::vector<SupportedRate> IntersectRatesAp(const std::vector<SupportedRate>& ap_rates,
                                            const std::vector<SupportedRate>& client_rates) {
    std::vector<SupportedRate> first(ap_rates);
    std::vector<SupportedRate> second(client_rates);

    std::sort(first.begin(), first.end());
    std::sort(second.begin(), second.end());

    std::vector<SupportedRate> result;
    // C++11 Standard 25.4.5.3 - set_intersection ALWAYS takes elements from the first vector.
    std::set_intersection(first.cbegin(), first.cend(), second.cbegin(), second.cend(),
                          std::back_inserter(result));
    return result;
}

void BssDescToSuppRates(const ::fuchsia::wlan::mlme::BSSDescription& bss,
                        std::vector<SupportedRate>* supp_rates,
                        std::vector<SupportedRate>* ext_rates) {
    std::vector<uint8_t> basic_rates(bss.basic_rate_set);
    std::vector<uint8_t> op_rates(bss.op_rate_set);

    constexpr size_t kMaxSuppRates = SupportedRatesElement::kMaxLen;
    constexpr size_t kMaxExtRates = ExtendedSupportedRatesElement::kMaxLen;

    if (op_rates.size() >= kMaxSuppRates + kMaxExtRates) {
        errorf("op_rates.size() is %lu > max allowed size: %zu\n", op_rates.size(),
               kMaxSuppRates + kMaxExtRates);
        ZX_DEBUG_ASSERT(false);
    }

    std::sort(basic_rates.begin(), basic_rates.end());
    std::sort(op_rates.begin(), op_rates.end());
    size_t count = 0;
    for (auto r : op_rates) {
        bool is_basic = std::binary_search(basic_rates.cbegin(), basic_rates.cend(), r);
        if (count < kMaxSuppRates) {
            supp_rates->emplace_back(SupportedRate(r, is_basic));
        } else {
            ext_rates->emplace_back(SupportedRate(r, is_basic));
        }
        ++count;
    }
    if (ext_rates->size() > kMaxExtRates) {
        ext_rates->resize(kMaxExtRates);
    }
}
}  // namespace wlan
