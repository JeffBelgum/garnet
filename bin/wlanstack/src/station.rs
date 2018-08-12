// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(dead_code)]
#![allow(unused_variables)]

use Never;
use async::temp::TempFutureExt;
use failure::{self, ResultExt};
use fidl::{self, endpoints2::RequestStream, endpoints2::ServerEnd};
use fidl_mlme::{self, MlmeEvent, MlmeEventStream, MlmeProxy};
use fidl_sme::{self, ClientSmeRequest};
use fidl_stats::IfaceStats;
use futures::{prelude::*, stream};
use futures::channel::mpsc;
use stats_scheduler::StatsRequest;
use std::sync::{Arc, Mutex};
use wlan_sme::{client, DeviceInfo, Station, MlmeRequest, MlmeStream};
use wlan_sme::client::{BssInfo, ConnectResult, DiscoveryError, DiscoveryResult, EssInfo};
use zx;

struct ClientTokens;

impl client::Tokens for ClientTokens {
    type ScanToken = fidl_sme::ScanTransactionControlHandle;
    type ConnectToken = Option<fidl_sme::ConnectTransactionControlHandle>;
}

pub type ClientSmeEndpoint = ServerEnd<fidl_sme::ClientSmeMarker>;
type Client = client::ClientSme<ClientTokens>;

pub fn serve_client_sme<S>(proxy: MlmeProxy,
                           device_info: DeviceInfo,
                           event_stream: MlmeEventStream,
                           new_fidl_clients: mpsc::UnboundedReceiver<ClientSmeEndpoint>,
                           stats_requests: S)
    -> impl Future<Output = Result<(), failure::Error>>
    where S: Stream<Item = StatsRequest>
{
    let (client, mlme_stream, user_stream) = Client::new(device_info);
    let client_arc = Arc::new(Mutex::new(client));
    let mlme_sme_fut = serve_mlme_sme(
        proxy, event_stream, client_arc.clone(), mlme_stream, stats_requests);
    let sme_fidl_fut = serve_client_sme_fidl(client_arc, new_fidl_clients, user_stream);
    mlme_sme_fut.select(sme_fidl_fut)
        .map(|e| e.either(|x| Ok(x.context("MLME<->SME future")?),
                          |x| Ok(x.context("SME<->FIDL future")?.into_any())))
}

fn serve_client_sme_fidl(client_arc: Arc<Mutex<Client>>,
                         new_fidl_clients: mpsc::UnboundedReceiver<ClientSmeEndpoint>,
                         user_stream: client::UserStream<ClientTokens>)
    -> impl Future<Output = Result<Never, failure::Error>>
{
    const SERVE_LIMIT: usize = 1000;

    // A future that forwards user events from the station to connected FIDL clients
    let sme_to_fidl = serve_user_stream(user_stream)
        .map(|()| Err(format_err!("SME->FIDL future unexpectedly finished")));
    // A future that handles requests from FIDL clients
    let fidl_to_sme = new_fidl_clients
        .map(Ok)
        .chain(stream::once(
            future::ready(Err(format_err!("new FIDL client stream unexpectedly ended")))))
        .try_for_each_concurrent(SERVE_LIMIT, move |channel| {
            new_client_service(client_arc.clone(), channel).unwrap_or_else(
                |e| error!("Error handling a FIDL request from user: {:?}", e)
            ).map(Ok)
        })
        .map(|x| match x {
            Ok(()) => Err(format_err!("FIDL->SME future unexpectedly finished")),
            Err(e) => Err(e),
        });
    sme_to_fidl.try_join(fidl_to_sme).map_ok(|x: (Never, Never)| x.0)
}

// The returned future successfully terminates when MLME closes the channel
fn serve_mlme_sme<STA, SRS>(proxy: MlmeProxy, event_stream: MlmeEventStream,
                            station: Arc<Mutex<STA>>, mlme_stream: MlmeStream,
                            stats_requests: SRS)
     -> impl Future<Output = Result<(), failure::Error>>
    where STA: Station,
          SRS: Stream<Item = StatsRequest>
{
    let (mut stats_sender, stats_fut) = serve_stats(proxy.clone(), stats_requests);
    let mlme_to_sme_fut = event_stream
        .err_into::<failure::Error>()
        .try_for_each(move |e| future::ready(match e {
            // Handle the stats response separately since it is SME-independent
            MlmeEvent::StatsQueryResp{ resp } => handle_stats_resp(&mut stats_sender, resp),
            other => Ok(station.lock().unwrap().on_mlme_event(other))
        }))
        .map_ok(|_| ());
    let sme_to_mlme_fut = mlme_stream
        .map(Ok)
        .try_for_each(move |e| {
            future::ready(match e {
                MlmeRequest::Scan(mut req) => proxy.start_scan(&mut req),
                MlmeRequest::Join(mut req) => proxy.join_req(&mut req),
                MlmeRequest::Authenticate(mut req) => proxy.authenticate_req(&mut req),
                MlmeRequest::Associate(mut req) => proxy.associate_req(&mut req),
                MlmeRequest::Deauthenticate(mut req) => proxy.deauthenticate_req(&mut req),
                MlmeRequest::Eapol(mut req) => proxy.eapol_req(&mut req),
                MlmeRequest::SetKeys(mut req) => proxy.set_keys_req(&mut req),
            })
        })
        .then(|r| future::ready(match r {
            Ok(_) => Err(format_err!("SME->MLME sender future unexpectedly finished")),
            Err(ref e) if is_peer_closed(e) => {
                // Don't treat closed channel as error; terminate the future peacefully instead
                Ok(())
            },
            Err(other) => Err(other.into()),
        }));
    // Select, not join: terminate as soon as one of the futures terminates
    let mlme_sme_fut = mlme_to_sme_fut.select(sme_to_mlme_fut)
        .map(|e| e.either(|x| Ok(x.context("MLME->SME")?),
                          |x| Ok(x.context("SME->MLME")?)));
    // select3() would be nice
    mlme_sme_fut.select(stats_fut)
        .map(|e| e.either(|x| x,
                          |x| Ok(x.context("Stats server")?.into_any())))
}

fn handle_stats_resp(stats_sender: &mut mpsc::Sender<IfaceStats>,
                     resp: fidl_mlme::StatsQueryResponse) -> Result<(), failure::Error> {
    stats_sender.try_send(resp.stats).or_else(|e| {
        if e.is_full() {
            // We only expect one response from MLME per each request, so the bounded
            // queue of size 1 should always suffice.
            warn!("Received an extra GetStatsResp from MLME, discarding");
            Ok(())
        } else {
            Err(format_err!("Failed to send a message to stats future"))
        }
    })
}

fn serve_stats<S>(proxy: MlmeProxy, stats_requests: S)
    -> (mpsc::Sender<IfaceStats>, impl Future<Output = Result<Never, failure::Error>>)
    where S: Stream<Item = StatsRequest>
{
    let (sender, receiver) = mpsc::channel(1);
    let fut = stats_requests
        .map(move |req| proxy.stats_query_req().map(move |_| req))
        .zip(receiver)
        // Future<Output = (Result<T, E>, IfaceStats)> to
        // Future<Output = Result<(T, IfaceStats), E>>
        .map(|(req, stats)| req.map(move |req| (req, stats)))
        .try_for_each(|(req, stats)| {
            req.reply(stats);
            future::ready(Ok(()))
        })
        .err_into::<failure::Error>()
        .and_then(|_|
            future::ready(Err(format_err!("Stats server future unexpectedly finished"))));
    (sender, fut)
}

fn new_client_service(client: Arc<Mutex<Client>>, endpoint: ClientSmeEndpoint)
    -> impl Future<Output = Result<(), ::fidl::Error>>
{
    future::ready(endpoint.into_stream())
        .and_then(|s| {
            s.try_for_each(move |request| future::ready(match request {
                ClientSmeRequest::Scan { req, txn, control_handle } => {
                    Ok(scan(&client, txn)
                        .unwrap_or_else(|e| error!("Error starting a scan transaction: {:?}", e)))
                },
                ClientSmeRequest::Connect { req, txn, control_handle } => {
                    Ok(connect(&client, req.ssid, req.password, txn)
                        .unwrap_or_else(|e| error!("Error starting a connect transaction: {:?}", e)))
                },
                ClientSmeRequest::Status { responder } => responder.send(&mut status(&client)),
            }))
        })
}

fn scan(client: &Arc<Mutex<Client>>,
        txn: ServerEnd<fidl_sme::ScanTransactionMarker>)
    -> Result<(), failure::Error>
{
    let handle = txn.into_stream()?.control_handle();
    client.lock().unwrap().on_scan_command(handle);
    Ok(())
}

fn connect(client: &Arc<Mutex<Client>>, ssid: Vec<u8>, password: Vec<u8>,
           txn: Option<ServerEnd<fidl_sme::ConnectTransactionMarker>>)
    -> Result<(), failure::Error>
{
    let handle = match txn {
        None => None,
        Some(txn) => Some(txn.into_stream()?.control_handle())
    };
    client.lock().unwrap().on_connect_command(ssid, password, handle);
    Ok(())
}

fn status(client: &Arc<Mutex<Client>>) -> fidl_sme::ClientStatusResponse {
    let status = client.lock().unwrap().status();
    fidl_sme::ClientStatusResponse {
        connected_to: status.connected_to.map(|bss| {
            Box::new(convert_bss_info(bss))
        }),
        connecting_to_ssid: status.connecting_to.unwrap_or(Vec::new()),
    }
}

fn serve_user_stream(stream: client::UserStream<ClientTokens>)
    -> impl Future<Output = ()>
{
    stream
        .for_each(|e| {
            future::ready(match e {
                client::UserEvent::ScanFinished{ token, result } => {
                    send_scan_results(token, result).unwrap_or_else(|e| {
                        if !is_peer_closed(&e) {
                            error!("Error sending scan results to user: {}", e);
                        }
                    })
                },
                client::UserEvent::ConnectFinished { token, result } => {
                    send_connect_result(token, result).unwrap_or_else(|e| {
                        if !is_peer_closed(&e) {
                            error!("Error sending connect result to user: {}", e);
                        }
                    })
                }
            })
        })
}

fn is_peer_closed(e: &fidl::Error) -> bool {
    match e {
        fidl::Error::ServerResponseWrite(zx::Status::PEER_CLOSED)
          | fidl::Error::ServerRequestRead(zx::Status::PEER_CLOSED)
          | fidl::Error::ClientRead(zx::Status::PEER_CLOSED)
          | fidl::Error::ClientWrite(zx::Status::PEER_CLOSED) => true,
        _ => false
    }
}

fn send_scan_results(token: fidl_sme::ScanTransactionControlHandle,
                     result: DiscoveryResult)
    -> Result<(), fidl::Error>
{
    match result {
        Ok(ess_list) => {
            let mut results = ess_list.into_iter().map(convert_ess_info).collect::<Vec<_>>();
            token.send_on_result(&mut results.iter_mut())?;
            token.send_on_finished()?;
        },
        Err(e) => {
            token.send_on_error(&mut fidl_sme::ScanError {
                code: match &e {
                    DiscoveryError::NotSupported => fidl_sme::ScanErrorCode::NotSupported,
                    DiscoveryError::InternalError => fidl_sme::ScanErrorCode::InternalError,
                },
                message: e.to_string()
            })?;
        }
    }
    Ok(())
}

fn convert_ess_info(ess: EssInfo) -> fidl_sme::EssInfo {
    fidl_sme::EssInfo {
        best_bss: convert_bss_info(ess.best_bss)
    }
}

fn convert_bss_info(bss: BssInfo) -> fidl_sme::BssInfo {
    fidl_sme::BssInfo {
        bssid: bss.bssid,
        ssid: bss.ssid,
        rx_dbm: bss.rx_dbm,
        channel: bss.channel,
        protected: bss.protected,
        compatible: bss.compatible
    }
}

fn send_connect_result(token: Option<fidl_sme::ConnectTransactionControlHandle>,
                       result: ConnectResult)
                       -> Result<(), fidl::Error>
{
    if let Some(token) = token {
        let code = match result {
            ConnectResult::Success => fidl_sme::ConnectResultCode::Success,
            ConnectResult::Canceled => fidl_sme::ConnectResultCode::Canceled,
            ConnectResult::Failed => fidl_sme::ConnectResultCode::Failed,
            ConnectResult::BadCredentials => fidl_sme::ConnectResultCode::BadCredentials,
        };
        token.send_on_finished(code)?;
    }
    Ok(())
}

