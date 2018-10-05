// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::host_dispatcher::HostDispatcher;
use fidl;
use fidl::endpoints::RequestStream;
use fidl_fuchsia_bluetooth_control::*;
use fuchsia_async as fasync;
use fuchsia_syslog::{fx_log, fx_log_warn};
use futures::{Future, TryStreamExt};

// Number of concurrent requests allowed to the pairing delegate at a single time
const MAX_CONCURRENT_REQUESTS: usize = 100;

async fn handler(
    pd: Option<PairingDelegateProxy>, event: PairingDelegateRequest,
) -> fidl::Result<()> {
    match pd {
        Some(pd) => match event {
            PairingDelegateRequest::OnPairingRequest { device, method, displayed_passkey, responder } =>
                await!(handle_pairing_request( pd, device, method, displayed_passkey, responder)),
            PairingDelegateRequest::OnPairingComplete { device_id, status, control_handle: _ } =>
                handle_pairing_complete(pd, device_id, status),
            PairingDelegateRequest::OnRemoteKeypress { device_id, keypress, control_handle: _, } =>
                handle_remote_keypress(pd, device_id, keypress),
        },
        None => match event {
            PairingDelegateRequest::OnPairingRequest { device: _, method: _, displayed_passkey: _, responder } => {
                fx_log_warn!("Rejected pairing due to no upstream pairing delegate");
                let _ = responder.send(false, None);
                Ok(())
            }
            PairingDelegateRequest::OnPairingComplete { device_id, .. } => {
                fx_log_warn!("Unhandled OnPairingComplete for device '{:?}': No PairingDelegate", device_id);
                Ok(())
            },
            PairingDelegateRequest::OnRemoteKeypress { device_id, .. } => {
                fx_log_warn!("Unhandled OnRemoteKeypress for device '{:?}': No PairingDelegate", device_id);
                Ok(())
            },
        },
    }
}

async fn handle_pairing_request(
    pd: PairingDelegateProxy, mut device: RemoteDevice, method: PairingMethod,
    displayed_passkey: Option<String>, responder: PairingDelegateOnPairingRequestResponder,
) -> fidl::Result<()> {
    let passkey_ref = displayed_passkey.as_ref().map(|x| &**x);
    let (status, passkey) = await!(pd.on_pairing_request(&mut device, method, passkey_ref))?;
    let _ = responder.send(status, passkey.as_ref().map(String::as_str));
    Ok(())
}

fn handle_pairing_complete(
    pd: PairingDelegateProxy, device_id: String, mut status: fidl_fuchsia_bluetooth::Status,
) -> fidl::Result<()> {
    if pd.on_pairing_complete(device_id.as_str(), &mut status).is_err() {
        fx_log_warn!("Failed to propagate pairing cancelled upstream");
    };
    Ok(())
}

fn handle_remote_keypress(
    pd: PairingDelegateProxy, device_id: String, keypress: PairingKeypressType,
) -> fidl::Result<()> {
    if pd.on_remote_keypress(device_id.as_str(), keypress).is_err() {
        fx_log_warn!("Failed to propagate pairing cancelled upstream");
    };
    Ok(())
}

pub fn start_pairing_delegate(
    mut hd: HostDispatcher, channel: fasync::Channel,
) -> impl Future<Output = fidl::Result<()>> {
    let stream = PairingDelegateRequestStream::from_channel(channel);
    stream.try_for_each_concurrent(MAX_CONCURRENT_REQUESTS, move |event| {
        handler(hd.pairing_delegate(), event)
    })
}
