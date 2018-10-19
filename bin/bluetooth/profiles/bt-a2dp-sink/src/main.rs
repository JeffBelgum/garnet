// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![deny(warnings)]
#![feature(futures_api, pin, async_await, arbitrary_self_types, await_macro)]

#[macro_use]
extern crate failure;

use avdtp;
use failure::{Error, ResultExt};
use fidl_fuchsia_bluetooth_bredr::*;
use fuchsia_async as fasync;
use fuchsia_zircon as zx;
use futures::{future, TryFutureExt, TryStreamExt, StreamExt};
use std::collections::HashSet;

fn get_available_stream_info() -> Result<Vec<avdtp::StreamInformation>, avdtp::Error> {
    // TODO(jamuraa): retrieve available streams from Media
    let s = avdtp::StreamInformation::new(
        1,
        false,
        avdtp::MediaType::Audio,
        avdtp::EndpointType::Sink,
    )?;
    Ok(vec![s])
}

fn get_stream_capabilities(
    _: avdtp::StreamEndpointId,
) -> Result<Vec<avdtp::ServiceCapability>, avdtp::Error> {
    // TODO(jamuraa): get the capabilities of the available stream
    // This is SBC with minimal requirements for a sink.
    Ok(vec![
        avdtp::ServiceCapability::MediaTransport,
        avdtp::ServiceCapability::MediaCodec {
            media_type: avdtp::MediaType::Audio,
            codec_type: avdtp::MediaCodecType::new(0),
            codec_extra: vec![0x3F, 0xFF, 2, 250],
        },
    ])
}

fn start_media_stream(sock: zx::Socket) -> Result<(), zx::Status> {
    let mut stream_sock = fasync::Socket::from_socket(sock)?;
    fuchsia_async::spawn((async move {
        let mut total_bytes = 0;
        while let Some(item) = await!(stream_sock.next()) {
            match item {
                Ok(pkt) => {
                    // TODO(jamuraa): secode the media stream into packets and frames
                    // TODO(jamuraa): deliver audio frames to media
                    total_bytes += pkt.len();
                    eprint!(
                        "Media Packet received: +{} bytes = {} \r",
                        pkt.len(),
                        total_bytes
                    );
                },
                Err(e) => return Err(e),
            }
        };
        eprintln!("Media stream finished");
        Ok(())
    }).unwrap_or_else(|e| eprintln!("Error in media stream: {:?}", e)));
    Ok(())
}

fn handle_request(r: avdtp::Request) -> Result<(), avdtp::Error> {
    eprintln!("Handling {:?} from peer..", r);
    match r {
        avdtp::Request::Discover { responder } => {
            let streams = get_available_stream_info()?;
            responder.send(&streams)
        }
        avdtp::Request::GetCapabilities {
            responder,
            stream_id,
        }
        | avdtp::Request::GetAllCapabilities {
            responder,
            stream_id,
        } => {
            let caps = get_stream_capabilities(stream_id)?;
            responder.send(&caps)
        }
    }
}

async fn init() -> Result<(), Error> {
    let profile_svc = fuchsia_app::client::connect_to_service::<ProfileMarker>()
            .context("Failed to connect to Bluetooth profile service")?;

    let mut service_def = make_profile_service_definition();
    let (status, service_id) = await!(profile_svc.add_service(&mut service_def, SecurityLevel::EncryptionOptional, false))?;

    eprintln!("Registered Service {:} for A2DP Sink", service_id);

    if let Some(e) = status.error {
        return Err(format_err!("Couldn't add A2DP sink service: {:?}", e));
    }

    let mut remotes: HashSet<String> = HashSet::new();
    let mut evt_stream = profile_svc.take_event_stream();
    while let Some(evt) = await!(evt_stream.next()) {
        if evt.is_err() {
            return Err(evt.err().unwrap().into());
        }
        match evt.unwrap() {
            ProfileEvent::OnConnected {
                device_id,
                service_id: _,
                channel,
                protocol,
            } => {
                eprintln!( "Connection from {}: {:?} {:?}!", device_id, channel, protocol);
                if remotes.contains(&device_id) {
                    eprintln!("{} already connected: connecting media channel", device_id);
                    let _ = start_media_stream(channel);
                    continue;
                }
                eprintln!("Adding new peer for {}", device_id);
                let peer = match avdtp::Peer::new(channel) {
                    Ok(peer) => peer,
                    Err(e) => {
                        eprintln!("Error adding peer: {:?}", e);
                        continue;
                    }
                };
                // Start handling the requests
                let stream = peer.take_request_stream();
                fuchsia_async::spawn(async move {
                    let x = await!(stream.try_for_each(|r| future::ready(handle_request(r))));
                    eprintln!("Peer Disconnected: {:?}", x);
                });
                remotes.insert(device_id);
            }
            ProfileEvent::OnDisconnected {
                device_id,
                service_id,
            } => {
                eprintln!("Device {} disconnected from {}", device_id, service_id);
                remotes.remove(&device_id);
            }
        }
    };
    Ok(())
}

fn main() -> Result<(), Error> {
    let mut executor = fuchsia_async::Executor::new().context("error creating executor")?;

    executor.run_singlethreaded(init())
}

fn make_profile_service_definition() -> ServiceDefinition {
    ServiceDefinition {
        service_class_uuids: vec![String::from("110B")], // Audio Sink UUID
        protocol_descriptors: vec![
            ProtocolDescriptor {
                protocol: ProtocolIdentifier::L2Cap,
                params: vec![DataElement {
                    type_: DataElementType::UnsignedInteger,
                    size: 2,
                    data: DataElementData::Integer(PSM_AVDTP),
                }],
            },
            ProtocolDescriptor {
                protocol: ProtocolIdentifier::Avdtp,
                params: vec![DataElement {
                    type_: DataElementType::UnsignedInteger,
                    size: 2,
                    data: DataElementData::Integer(0x0103), // Indicate v1.3
                }],
            },
        ],
        profile_descriptors: vec![ProfileDescriptor {
            profile_id: ServiceClassProfileIdentifier::AdvancedAudioDistribution,
            major_version: 1,
            minor_version: 3,
        }],
        additional_protocol_descriptors: None,
        information: vec![],
        additional_attributes: None,
    }
}
