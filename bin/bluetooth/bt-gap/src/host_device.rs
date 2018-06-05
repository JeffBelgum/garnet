// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl;
use fidl_bluetooth::Status;
use fidl_bluetooth_control::AdapterInfo;
use fidl_bluetooth_host::{HostEvent, HostEventStream, HostProxy};
use futures::{FutureExt, StreamExt};
use futures::Future;
use futures::future::ok as fok;
use host_dispatcher::HostDispatcher;
use parking_lot::RwLock;
use std::sync::Arc;
use util::clone_host_state;

pub struct HostDevice {
    host: HostProxy,
    info: AdapterInfo,
}

impl HostDevice {
    pub fn new(host: HostProxy, info: AdapterInfo) -> Self {
        HostDevice {
            host,
            info,
        }
    }

    pub fn get_host(&self) -> &HostProxy {
        &self.host
    }

    pub fn get_info(&self) -> &AdapterInfo {
        &self.info
    }

    pub fn set_name(&self, mut name: String) -> impl Future<Item = Status, Error = fidl::Error> {
        self.host.set_local_name(&mut name)
    }

    pub fn start_discovery(&mut self) -> impl Future<Item = Status, Error = fidl::Error> {
        self.host.start_discovery()
    }

    pub fn close(&self) -> Result<(), fidl::Error> {
        self.host.close()
    }

    pub fn stop_discovery(&self) -> impl Future<Item = Status, Error = fidl::Error> {
        self.host.stop_discovery()
    }

    pub fn set_discoverable(
        &mut self, discoverable: bool
    ) -> impl Future<Item = Status, Error = fidl::Error> {
        self.host.set_discoverable(discoverable)
    }
}

pub fn run(
    hd: Arc<RwLock<HostDispatcher>>, host: Arc<RwLock<HostDevice>>
) -> impl Future<Item = HostEventStream, Error = fidl::Error> {
    make_clones!(host => host_stream, host);
    let stream = host_stream.read().host.take_event_stream();
    stream
        .for_each(move |evt| {
            match evt {
                HostEvent::OnHostStateChanged { ref state } => {
                    host.write().info.state = Some(Box::new(clone_host_state(&state)));
                }
                HostEvent::OnDeviceDiscovered { mut device } => {
                    if let Some(ref events) = hd.write().events {
                        let _res = events
                            .send_on_device_updated(&mut device)
                            .map_err(|e| error!("Failed to send device discovery event: {:?}", e));
                    }
                }
            };
            fok(())
        })
        .err_into()
}
