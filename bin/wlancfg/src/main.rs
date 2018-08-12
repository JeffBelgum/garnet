// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![feature(futures_api, pin, arbitrary_self_types, transpose_result)]
#![deny(warnings)]

#[macro_use]
extern crate failure;
extern crate fidl;
extern crate fidl_fuchsia_wlan_mlme as fidl_mlme;
extern crate fidl_fuchsia_wlan_service as legacy;
extern crate fidl_fuchsia_wlan_sme as fidl_sme;
extern crate fidl_fuchsia_wlan_stats as fidl_wlan_stats;
extern crate fidl_fuchsia_wlan_device as wlan;
extern crate fidl_fuchsia_wlan_device_service as wlan_service;
extern crate fuchsia_app as app;
#[macro_use]
extern crate fuchsia_async as async;
extern crate fuchsia_zircon as zx;
#[macro_use]
extern crate futures;
#[macro_use]
extern crate log;
extern crate parking_lot;
extern crate serde;
#[macro_use]
extern crate serde_derive;
extern crate serde_json;

#[cfg(test)]
extern crate tempdir;

mod config;
mod client;
mod device;
mod known_ess_store;
mod future_util;
mod shim;
mod state_machine;

use app::server::ServicesServer;
use config::Config;
use known_ess_store::KnownEssStore;
use failure::{Error, ResultExt};
use futures::prelude::*;
use std::sync::Arc;
use wlan_service::DeviceServiceMarker;

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum Never {}
impl Never {
    pub fn never_into<T>(self) -> T { match self {} }
}

fn serve_fidl(_client_ref: shim::ClientRef)
    -> impl Future<Output = Result<Never, Error>>
{
    future::ready(ServicesServer::new()
        // To test the legacy API server, change
        //     "fuchsia.wlan.service.Wlan": "wlanstack"
        // to
        //     "fuchsia.wlan.service.Wlan": "wlancfg"
        // in 'bin/sysmgr/config/services.config'
        //
        // Also, comment out the following lines in 'bin/wlancfg/BUILD.gn':
        //    {
        //      dest = "sysmgr/wlancfg.config"
        //      path = rebase_path("wlancfg.config")
        //    },
        //
        // Then, uncomment the following code:
        /*
        .add_service((<legacy::WlanMarker as ::fidl::endpoints2::ServiceMarker>::NAME, move |channel| {
            let stream = <legacy::WlanRequestStream as ::fidl::endpoints2::RequestStream>::from_channel(channel);
            let fut = shim::serve_legacy(stream, _client_ref.clone())
                .unwrap_or_else(|e| eprintln!("error serving legacy wlan API: {}", e));
            async::spawn(fut)
        }))
        */
        .start())
        .and_then(|fut| fut)
        .and_then(|()| future::ready(Err(format_err!("FIDL server future exited unexpectedly"))))
}

fn main() -> Result<(), Error> {
    let cfg = Config::load_from_file()?;

    let mut executor = async::Executor::new().context("error creating event loop")?;
    let wlan_svc = app::client::connect_to_service::<DeviceServiceMarker>()
        .context("failed to connect to device service")?;

    let legacy_client = shim::ClientRef::new();
    let fidl_fut = serve_fidl(legacy_client.clone());

    let (watcher_proxy, watcher_server_end) = fidl::endpoints2::create_endpoints()?;
    wlan_svc.watch_devices(watcher_server_end)?;
    let listener = device::Listener::new(wlan_svc, cfg, legacy_client);
    let ess_store = Arc::new(KnownEssStore::new()?);
    let fut = watcher_proxy.take_event_stream()
        .try_for_each(move |evt| device::handle_event(&listener, evt, &ess_store).map(Ok))
        .err_into()
        .and_then(|_| future::ready(Err(format_err!("Device watcher future exited unexpectedly"))));

    executor
        .run_singlethreaded(fidl_fut.try_join(fut))
        .map(|_: (Never, Never)| ())
}
