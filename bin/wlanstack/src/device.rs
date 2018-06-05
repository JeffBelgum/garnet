// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use async;
use failure::Error;
use fidl_mlme;
use futures::prelude::*;
use futures::{future, stream};
use futures::channel::mpsc;
use parking_lot::Mutex;
use vfs_watcher;
use watchable_map::{MapWatcher, WatchableMap, WatcherResult};
use wlan;
use wlan_dev;
use zx;

use std::fs::File;
use std::path::Path;
use std::str::FromStr;
use std::sync::Arc;
use std::{thread, time};

struct PhyDevice {
    id: u16,
    proxy: wlan::PhyProxy,
    dev: wlan_dev::Device,
}

impl PhyDevice {
    fn new<P: AsRef<Path>>(id: u16, path: P) -> Result<Self, zx::Status> {
        let dev = wlan_dev::Device::new(path)?;
        let proxy = wlan_dev::connect_wlan_phy(&dev)?;
        Ok(PhyDevice { id, proxy, dev })
    }
}

pub type ClientSmeServer = mpsc::UnboundedSender<async::Channel>;

struct IfaceDevice {
    client_sme_server: Option<ClientSmeServer>,
    _dev: wlan_dev::Device,
}

fn open_iface_device<P: AsRef<Path>>(path: P) -> Result<(async::Channel, wlan_dev::Device), zx::Status> {
    let dev = wlan_dev::Device::new(path)?;
    let channel = wlan_dev::connect_wlan_iface(&dev)?;
    Ok((channel, dev))
}

/// Called by the `DeviceManager` in response to device events.
pub trait EventListener: Send + Sync {
    /// Called when a phy device is added. On error, the listener is removed from the
    /// `DeviceManager`.
    fn on_phy_added(&self, id: u16) -> Result<(), Error>;

    /// Called when a phy device is removed. On error, the listener is removed from the
    /// `DeviceManager`.
    fn on_phy_removed(&self, id: u16) -> Result<(), Error>;

    /// Called when an iface device is added. On error, the listener is removed from the
    /// `DeviceManager`.
    fn on_iface_added(&self, id: u16) -> Result<(), Error>;

    /// Called when an iface device is removed. On error, the listener is removed from
    /// the `DeviceManager`.
    fn on_iface_removed(&self, id: u16) -> Result<(), Error>;
}

pub type DevMgrRef = Arc<Mutex<DeviceManager>>;

struct PhyWatcher(Arc<EventListener>);
impl MapWatcher<u16> for PhyWatcher {
    fn on_add_key(&self, key: &u16) -> WatcherResult {
        handle_notification_error("on_phy_added", self.0.on_phy_added(*key))
    }
    fn on_remove_key(&self, key: &u16) -> WatcherResult {
        handle_notification_error("on_phy_removed", self.0.on_phy_removed(*key))
    }
}

struct IfaceWatcher(Arc<EventListener>);
impl MapWatcher<u16> for IfaceWatcher {
    fn on_add_key(&self, key: &u16) -> WatcherResult {
        handle_notification_error("on_iface_added", self.0.on_iface_added(*key))
    }
    fn on_remove_key(&self, key: &u16) -> WatcherResult {
        handle_notification_error("on_iface_removed", self.0.on_iface_removed(*key))
    }
}

fn handle_notification_error(event_name: &str, r: Result<(), Error>) -> WatcherResult {
    match r {
        Ok(()) => WatcherResult::KeepWatching,
        Err(e) => {
            eprintln!("Failed to notify a watcher of {} event: {}", event_name, e);
            WatcherResult::StopWatching
        }
    }
}

/// Manages the wlan devices used by the wlanstack.
pub struct DeviceManager {
    phys: WatchableMap<u16, PhyDevice, PhyWatcher>,
    ifaces: WatchableMap<u16, IfaceDevice, IfaceWatcher>,
}

impl DeviceManager {
    /// Create a new `DeviceManager`.
    pub fn new() -> Self {
        DeviceManager {
            phys: WatchableMap::new(),
            ifaces: WatchableMap::new(),
        }
    }

    fn add_phy(&mut self, phy: PhyDevice) {
        self.phys.insert(phy.id, phy);
    }

    fn rm_phy(&mut self, id: u16) {
        self.phys.remove(&id);
    }

    fn add_iface(&mut self, id: u16, channel: async::Channel, device: wlan_dev::Device) {
        let proxy = fidl_mlme::MlmeProxy::new(channel);
        // TODO(gbonik): move this code outside of DeviceManager
        let (sender, receiver) = mpsc::unbounded();
        // TODO(gbonik): check the role of the interface instead of assuming it is a station
        async::spawn(super::station::serve_client_sme(proxy, receiver).recover::<Never, _>(|e| {
            eprintln!("Error serving client station: {:?}", e);
        }));
        self.ifaces.insert(id, IfaceDevice {
            client_sme_server: Some(sender),
            _dev: device,
        });
    }

    fn rm_iface(&mut self, id: u16) {
        self.ifaces.remove(&id);
    }

    /// Retrieves information about all the phy devices managed by this `DeviceManager`.
    // TODO(tkilbourn): this should return a simplified view of the Phy compared to query_phy. For
    // now we just return the whole PhyInfo for each device.
    pub fn list_phys(&self) -> impl Stream<Item = wlan::PhyInfo, Error = ()> {
        self.phys
            .iter()
            .map(|(_, phy)| {
                let phy_id = phy.id;
                let phy_path = phy.dev.path().to_string_lossy().into_owned();
                // For now we query each device for every call to `list_phys`. We will need to
                // decide how to handle queries for static vs dynamic data, caching response, etc.
                phy.proxy.query().map_err(|_| ()).map(move |response| {
                    let mut info = response.info;
                    info.id = phy_id;
                    info.dev_path = Some(phy_path);
                    info
                })
            })
            .collect::<stream::FuturesUnordered<_>>()
    }

    pub fn query_phy(&self, id: u16) -> impl Future<Item = wlan::PhyInfo, Error = zx::Status> {
        let phy = match self.phys.get(&id) {
            Some(p) => p,
            None => return future::err(zx::Status::NOT_FOUND).left_future(),
        };
        let phy_id = phy.id;
        let phy_path = phy.dev.path().to_string_lossy().into_owned();
        phy.proxy
            .query()
            .map_err(|_| zx::Status::INTERNAL)
            .and_then(move |response| {
                zx::Status::ok(response.status)
                    .into_future()
                    .map(move |()| {
                        let mut info = response.info;
                        info.id = phy_id;
                        info.dev_path = Some(phy_path);
                        info
                    })
            })
            .right_future()
    }

    /// Creates an interface on the phy with the given id.
    pub fn create_iface(
        &mut self, phy_id: u16, role: wlan::MacRole,
    ) -> impl Future<Item = u16, Error = zx::Status> + Send {
        let phy = match self.phys.get(&phy_id) {
            Some(p) => p,
            None => return future::err(zx::Status::INVALID_ARGS).left_future(),
        };
        let mut req = wlan::CreateIfaceRequest { role };
        phy.proxy
            .create_iface(&mut req)
            .map_err(|_| zx::Status::IO)
            .and_then(|resp| {
                if let Ok(()) = zx::Status::ok(resp.status) {
                    future::ok(resp.info.id)
                } else {
                    future::err(zx::Status::from_raw(resp.status))
                }
            })
            .right_future()
    }

    /// Destroys an interface with the given ids.
    pub fn destroy_iface(
        &mut self, phy_id: u16, iface_id: u16,
    ) -> impl Future<Item = (), Error = zx::Status> {
        let phy = match self.phys.get(&phy_id) {
            Some(p) => p,
            None => return future::err(zx::Status::INVALID_ARGS).left_future(),
        };
        let mut req = wlan::DestroyIfaceRequest { id: iface_id };
        phy.proxy
            .destroy_iface(&mut req)
            .map_err(|_| zx::Status::IO)
            .and_then(|resp| zx::Status::ok(resp.status).into_future())
            .right_future()
    }

    /// Adds an `EventListener`. The event methods will be called for each existing object tracked
    /// by this device manager.
    pub fn add_listener(&mut self, listener: Arc<EventListener>) {
        self.phys.add_watcher(PhyWatcher(listener.clone()));
        self.ifaces.add_watcher(IfaceWatcher(listener));
    }

    pub fn get_client_sme(&mut self, iface_id: u16) -> Option<ClientSmeServer> {
        self.ifaces.get(&iface_id).and_then(|dev| dev.client_sme_server.clone())
    }
}

fn new_watcher<P, OnAdd, OnRm>(
    path: P, devmgr: DevMgrRef, on_add: OnAdd, on_rm: OnRm,
) -> impl Future<Item = (), Error = Error>
where
    OnAdd: Fn(DevMgrRef, &Path),
    OnRm: Fn(DevMgrRef, &Path),
    P: AsRef<Path>,
{
    File::open(&path)
        .into_future()
        .err_into()
        .and_then(|dev| vfs_watcher::Watcher::new(&dev).map_err(Into::into))
        .and_then(|watcher| {
            watcher
                .for_each(move |msg| {
                    let full_path = path.as_ref().join(msg.filename);
                    match msg.event {
                        vfs_watcher::WatchEvent::EXISTING | vfs_watcher::WatchEvent::ADD_FILE => {
                            on_add(devmgr.clone(), &full_path);
                        }
                        vfs_watcher::WatchEvent::REMOVE_FILE => {
                            on_rm(devmgr.clone(), &full_path);
                        }
                        vfs_watcher::WatchEvent::IDLE => debug!("device watcher idle"),
                        e => warn!("unknown watch event: {:?}", e),
                    }
                    Ok(())
                })
                .map(|_s| ())
                .err_into()
        })
}

/// Creates a `futures::Stream` that adds phy devices to the `DeviceManager` as they appear at the
/// given path.
pub fn new_phy_watcher<P: AsRef<Path>>(
    path: P, devmgr: DevMgrRef,
) -> impl Future<Item = (), Error = Error> {
    new_watcher(
        path,
        devmgr,
        |devmgr, path| {
            info!("found phy at {}", path.to_string_lossy());
            // The path was constructed in the new_watcher closure, so filename should not be
            // empty. The file_name comes from devmgr and is an integer, so from_str should not
            // fail.
            let id = u16::from_str(&path.file_name().unwrap().to_string_lossy()).unwrap();
            // This could fail if the device were to go away in between our receiving the watcher
            // message and here. TODO(tkilbourn): handle this case more cleanly.
            let phy = PhyDevice::new(id, path).expect("Failed to open phy device");
            devmgr.lock().add_phy(phy);
        },
        |devmgr, path| {
            info!("removing phy at {}", path.to_string_lossy());
            // The path was constructed in the new_watcher closure, so filename should not be
            // empty. The file_name comes from devmgr and is an integer, so from_str should not
            // fail.
            let id = u16::from_str(&path.file_name().unwrap().to_string_lossy()).unwrap();
            devmgr.lock().rm_phy(id);
        },
    )
}

/// Creates a `futures::Stream` that adds iface devices to the `DeviceManager` as they appear at
/// the given path.
/// TODO(tkilbourn): add the iface to `DeviceManager`
pub fn new_iface_watcher<P: AsRef<Path>>(
    path: P, devmgr: DevMgrRef,
) -> impl Future<Item = (), Error = Error> {
    new_watcher(
        path,
        devmgr,
        |devmgr, path| {
            info!("found iface at {}", path.to_string_lossy());
            let id = u16::from_str(&path.file_name().unwrap().to_string_lossy()).unwrap();

            // Temporarily delay opening the iface since only one service may open a channel to a
            // device at a time. If the legacy wlantack is running, it should take priority. For
            // development of wlanstack2, kill the wlanstack process first to let wlanstack2 take
            // over.
            debug!("sleeping 100ms...");
            let open_delay = time::Duration::from_millis(100);
            thread::sleep(open_delay);

            match open_iface_device(path) {
                Ok((channel, dev)) => devmgr.lock().add_iface(id, channel, dev),
                Err(zx::Status::ALREADY_BOUND) => info!("iface already open, deferring"),
                Err(e) => error!("could not open iface: {:?}", e),
            }
        },
        |devmgr, path| {
            info!("removing iface at {}", path.to_string_lossy());
            let id = u16::from_str(&path.file_name().unwrap().to_string_lossy()).unwrap();
            devmgr.lock().rm_iface(id);
        },
    )
}
