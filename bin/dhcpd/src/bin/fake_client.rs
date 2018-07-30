// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#![deny(warnings)]
#![feature(async_await, await_macro)]

use dhcp::protocol::{CLIENT_PORT, ConfigOption, Message, MessageType, OptionCode, SERVER_PORT};
use failure::{Error, ResultExt};
use fuchsia_async::Executor;
use fuchsia_async::net::UdpSocket;
use std::net::SocketAddr;

const TEST_MAC: [u8; 6] = [0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF];

fn main() -> Result<(), Error> {
    println!("fake_client: starting...");
    let mut exec = Executor::new().context("error creating executor")?;
    let (udp_socket, server) = build_and_bind_socket();

    let disc = build_discover();
    println!("Sending discover message: {:?}", disc);
    let send_msgs = async {
        {
            let serialized = disc.serialize();
            await!(udp_socket.send_to(&serialized, server))?;
        }
        let mut buf = vec![0u8; 1024];
        let (_bytes_rcvd, _addr) = await!(udp_socket.recv_from(&mut buf))?;
        let offer = Message::from_buffer(&buf).unwrap();
        println!("fake_client: msg rcvd {:?}", offer);
        let req = build_request(offer);
        println!("fake_client: sending request msg {:?}", req);
        {
            let serialized = req.serialize();
            await!(udp_socket.send_to(&serialized, server))?;
        }
        let mut buf = vec![0u8; 1024];
        let (_bytes_rcvd, _addr) = await!(udp_socket.recv_from(&mut buf))?;
        let ack = Message::from_buffer(&buf).unwrap();
        println!("fake_client: msg rcvd {:?}", ack);
        Ok::<(), Error>(())
    };

    println!("fake_client: sending messages...");
    exec.run_singlethreaded(send_msgs).context("could not run futures")?;
    println!("fake_client: messages sent...");

    Ok(())
}

fn build_and_bind_socket() -> (UdpSocket, SocketAddr) {
    let addr = SocketAddr::new("127.0.0.1".parse().unwrap(), CLIENT_PORT);
    let server = SocketAddr::new("127.0.0.1".parse().unwrap(), SERVER_PORT);
    let udp_socket = UdpSocket::bind(&addr).context("error binding socket").unwrap();
    (udp_socket, server)
}

fn build_discover() -> Message {
    let mut disc = Message::new();
    disc.xid = 42;
    disc.chaddr = TEST_MAC;
    disc.options.push(ConfigOption {
        code: OptionCode::DhcpMessageType,
        value: vec![MessageType::DHCPDISCOVER as u8],
    });
    disc
}

fn build_request(offer: Message) -> Message {
    let mut req = Message::new();
    req.xid = 42;
    req.ciaddr = offer.yiaddr;
    req.chaddr = TEST_MAC;
    req.options.push(ConfigOption {
        code: OptionCode::DhcpMessageType,
        value: vec![MessageType::DHCPREQUEST as u8],
    });
    let server_id = offer.get_config_option(OptionCode::ServerId).unwrap().clone();
    req.options.push(server_id);
    req
}
