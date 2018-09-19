// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl_fuchsia_cobalt::HistogramBucket;
use fidl_fuchsia_wlan_stats as fidl_stats;
use fidl_fuchsia_wlan_stats::MlmeStats::{ApMlmeStats, ClientMlmeStats};
use fuchsia_async as fasync;
use fuchsia_zircon::DurationNum;
use futures::prelude::*;
use futures::stream::FuturesUnordered;
use futures::StreamExt;
use log::{error, log};
use parking_lot::Mutex;
use std::cmp::PartialOrd;
use std::collections::HashMap;
use std::default::Default;
use std::ops::Sub;
use std::sync::Arc;

use crate::cobalt_reporter::CobaltSender;
use crate::device::IfaceMap;

type StatsRef = Arc<Mutex<fidl_stats::IfaceStats>>;

const REPORT_PERIOD_MINUTES: i64 = 1;

// These IDs must match the Cobalt config from //third_party/cobalt_config/fuchsia/wlan/config.yaml
enum CobaltMetricId {
    DispatcherPacketCounter = 5,
    ClientAssocDataRssi = 6,
    ClientBeaconRssi = 7,
    ConnectionTime = 8,
}

// Export MLME stats to Cobalt every REPORT_PERIOD_MINUTES.
pub async fn report_telemetry_periodically(ifaces_map: Arc<IfaceMap>, mut sender: CobaltSender) {
    // TODO(NET-1386): Make this module resilient to Wlanstack2 downtime.

    let mut last_reported_stats: HashMap<u16, StatsRef> = HashMap::new();
    let mut interval_stream = fasync::Interval::new(REPORT_PERIOD_MINUTES.minutes());
    while let Some(_) = await!(interval_stream.next()) {
        let mut futures = FuturesUnordered::new();
        for (id, iface) in ifaces_map.get_snapshot().iter() {
            let id = *id;
            let iface = Arc::clone(iface);
            let fut = iface.stats_sched.get_stats().map(move |r| (id, iface, r));
            futures.push(fut);
        }

        while let Some((id, iface, stats_result)) = await!(futures.next()) {
            match stats_result {
                Ok(current_stats) => {
                    let last_stats_opt = last_reported_stats.get(&id);
                    if let Some(last_stats) = last_stats_opt {
                        let last_stats = last_stats.lock();
                        let current_stats = current_stats.lock();
                        report_stats(&last_stats, &current_stats, &mut sender);
                    }

                    last_reported_stats.insert(id, current_stats);
                }
                Err(e) => {
                    last_reported_stats.remove(&id);
                    error!(
                        "Failed to get the stats for iface '{}': {}",
                        iface.device.path().display(),
                        e
                    );
                }
            };
        }
    }
}

fn report_stats(
    last_stats: &fidl_stats::IfaceStats, current_stats: &fidl_stats::IfaceStats,
    sender: &mut CobaltSender,
) {
    report_mlme_stats(&last_stats.mlme_stats, &current_stats.mlme_stats, sender);

    report_dispatcher_stats(
        &last_stats.dispatcher_stats,
        &current_stats.dispatcher_stats,
        sender,
    );
}

fn report_dispatcher_stats(
    last_stats: &fidl_stats::DispatcherStats, current_stats: &fidl_stats::DispatcherStats,
    sender: &mut CobaltSender,
) {
    // These indexes must match the Cobalt config from
    // //third_party/cobalt_config/fuchsia/wlan/config.yaml
    const DISPATCHER_IN_PACKET_COUNT_INDEX: u32 = 0;
    const DISPATCHER_OUT_PACKET_COUNT_INDEX: u32 = 1;
    const DISPATCHER_DROP_PACKET_COUNT_INDEX: u32 = 2;

    report_dispatcher_packets(
        DISPATCHER_IN_PACKET_COUNT_INDEX,
        get_diff(
            last_stats.any_packet.in_.count,
            current_stats.any_packet.in_.count,
        ),
        sender,
    );
    report_dispatcher_packets(
        DISPATCHER_OUT_PACKET_COUNT_INDEX,
        get_diff(
            last_stats.any_packet.out.count,
            current_stats.any_packet.out.count,
        ),
        sender,
    );
    report_dispatcher_packets(
        DISPATCHER_DROP_PACKET_COUNT_INDEX,
        get_diff(
            last_stats.any_packet.drop.count,
            current_stats.any_packet.drop.count,
        ),
        sender,
    );
}

fn report_dispatcher_packets(packet_type_index: u32, packet_count: u64, sender: &mut CobaltSender) {
    sender.log_event_count(
        CobaltMetricId::DispatcherPacketCounter as u32,
        packet_type_index,
        packet_count as i64,
    );
}

fn report_mlme_stats(
    last: &Option<Box<fidl_stats::MlmeStats>>, current: &Option<Box<fidl_stats::MlmeStats>>,
    sender: &mut CobaltSender,
) {
    if let (Some(ref last), Some(ref current)) = (last, current) {
        match (last.as_ref(), current.as_ref()) {
            (ClientMlmeStats(last), ClientMlmeStats(current)) => {
                report_client_mlme_stats(&last, &current, sender)
            }
            (ApMlmeStats(_), ApMlmeStats(_)) => {}
            _ => error!("Current MLME stats type is different from the last MLME stats type"),
        };
    }
}

fn report_client_mlme_stats(
    last_stats: &fidl_stats::ClientMlmeStats, current_stats: &fidl_stats::ClientMlmeStats,
    sender: &mut CobaltSender,
) {
    report_rssi_stats(
        CobaltMetricId::ClientAssocDataRssi as u32,
        &last_stats.assoc_data_rssi,
        &current_stats.assoc_data_rssi,
        sender,
    );
    report_rssi_stats(
        CobaltMetricId::ClientBeaconRssi as u32,
        &last_stats.beacon_rssi,
        &current_stats.beacon_rssi,
        sender,
    );
}

fn report_rssi_stats(
    rssi_metric_id: u32, last_stats: &fidl_stats::RssiStats, current_stats: &fidl_stats::RssiStats,
    sender: &mut CobaltSender,
) {
    // In the internal stats histogram, hist[x] represents the number of frames
    // with RSSI -x. For the Cobalt representation, buckets from -128 to 0 are
    // used. When data is sent to Cobalt, the concept of index is utilized.
    //
    // Shortly, for Cobalt:
    // Bucket -128 -> index   0
    // Bucket -127 -> index   1
    // ...
    // Bucket    0 -> index 128
    //
    // The for loop below converts the stats internal representation to the
    // Cobalt representation and prepares the histogram that will be sent.

    let mut histogram = Vec::new();
    for bin in 0..current_stats.hist.len() {
        let diff = get_diff(last_stats.hist[bin], current_stats.hist[bin]);
        if diff > 0 {
            let entry = HistogramBucket {
                index: (fidl_stats::RSSI_BINS - (bin as u8) - 1).into(),
                count: diff.into(),
            };
            histogram.push(entry);
        }
    }

    if !histogram.is_empty() {
        sender.log_int_histogram(rssi_metric_id, histogram);
    }
}

pub fn report_connection_time(sender: &mut CobaltSender, time: i64, result_index: u32) {
    sender.log_elapsed_time(CobaltMetricId::ConnectionTime as u32, result_index, time);
}

fn get_diff<T>(last_stat: T, current_stat: T) -> T
where
    T: Sub<Output = T> + PartialOrd + Default,
{
    if current_stat >= last_stat {
        current_stat - last_stat
    } else {
        Default::default()
    }
}
