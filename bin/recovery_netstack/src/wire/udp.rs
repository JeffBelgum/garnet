// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[cfg(test)]
use std::fmt::{self, Debug, Formatter};
use std::num::NonZeroU16;
use std::ops::RangeBounds;

use byteorder::{BigEndian, ByteOrder};
use zerocopy::{AsBytes, ByteSlice, FromBytes, LayoutVerified, Unaligned};

use ip::{Ip, IpAddr, IpProto};
use wire::util::{extract_slice_range, fits_in_u16, fits_in_u32, Checksum};
use wire::{Err, ParseErr};

const HEADER_SIZE: usize = 8;

// Header has the same memory layout (thanks to repr(C, packed)) as a UDP
// header. Thus, we can simply reinterpret the bytes of the UDP header as a
// Header and then safely access its fields. Note the following caveats:
// - We cannot make any guarantees about the alignment of an instance of this
//   struct in memory or of any of its fields. This is true both because
//   repr(packed) removes the padding that would be used to ensure the alignment
//   of individual fields, but also because we are given no guarantees about
//   where within a given memory buffer a particular packet (and thus its
//   header) will be located.
// - Individual fields are all either u8 or [u8; N] rather than u16, u32, etc.
//   This is for two reasons:
//   - u16 and larger have larger-than-1 alignments, which are forbidden as
//     described above
//   - We are not guaranteed that the local platform has the same endianness as
//     network byte order (big endian), so simply treating a sequence of bytes
//     as a u16 or other multi-byte number would not necessarily be correct.
//     Instead, we use the BigEndian type and its reader and writer methods to
//     correctly access these fields.
#[repr(C, packed)]
struct Header {
    src_port: [u8; 2],
    dst_port: [u8; 2],
    length: [u8; 2],
    checksum: [u8; 2],
}

/// The length of a UDP header in bytes.
///
/// When calling `UdpPacket::create`, provide at least `HEADER_LEN` bytes for
/// the header in order to guarantee that `create` will not panic.
pub const HEADER_LEN: usize = 8;

unsafe impl FromBytes for Header {}
unsafe impl AsBytes for Header {}
unsafe impl Unaligned for Header {}

impl Header {
    fn src_port(&self) -> u16 {
        BigEndian::read_u16(&self.src_port)
    }

    fn set_src_port(&mut self, src_port: u16) {
        BigEndian::write_u16(&mut self.src_port, src_port);
    }

    fn dst_port(&self) -> u16 {
        BigEndian::read_u16(&self.dst_port)
    }

    fn set_dst_port(&mut self, dst_port: u16) {
        BigEndian::write_u16(&mut self.dst_port, dst_port);
    }

    fn length(&self) -> u16 {
        BigEndian::read_u16(&self.length)
    }

    fn checksum(&self) -> u16 {
        BigEndian::read_u16(&self.checksum)
    }
}

/// A UDP packet.
///
/// A `UdpPacket` shares its underlying memory with the byte slice it was parsed
/// from or serialized to, meaning that no copying or extra allocation is
/// necessary.
pub struct UdpPacket<B> {
    header: LayoutVerified<B, Header>,
    body: B,
}

impl<B: ByteSlice> UdpPacket<B> {
    /// Parse a UDP packet.
    ///
    /// `parse` parses `bytes` as a UDP packet and validates the checksum.
    ///
    /// `src_ip` is the source address in the IP header. In IPv4, `dst_ip` is
    /// the destination address in the IPv4 header. In IPv6, it's more
    /// complicated:
    /// - If there's no routing header, the destination is the one in the IPv6
    ///   header.
    /// - If there is a routing header, then the sender will compute the
    ///   checksum using the last address in the routing header, while the
    ///   receiver will compute the checksum using the destination address in
    ///   the IPv6 header.
    pub fn parse<A: IpAddr>(bytes: B, src_ip: A, dst_ip: A) -> Result<UdpPacket<B>, impl ParseErr> {
        // See for details: https://en.wikipedia.org/wiki/User_Datagram_Protocol#Packet_structure

        let bytes_len = bytes.len();
        let (header, body) =
            LayoutVerified::<B, Header>::new_unaligned_from_prefix(bytes).ok_or(Err::Format)?;
        let packet = UdpPacket { header, body };
        let len = if packet.header.length() == 0 && A::Version::VERSION.is_v6() {
            // IPv6 supports jumbograms, so a UDP packet may be greater than
            // 2^16 bytes in size. In this case, the size doesn't fit in the
            // 16-bit length field in the header, and so the length field is set
            // to zero to indicate this.
            bytes_len
        } else {
            packet.header.length() as usize
        };
        if len != bytes_len {
            return Err(Err::Format);
        }
        if packet.header.dst_port() == 0 {
            return Err(Err::Format);
        }

        // A 0 checksum indicates that the checksum wasn't computed. In IPv4,
        // this means that it shouldn't be validated. In IPv6, the checksum is
        // mandatory, so this is an error.
        if packet.header.checksum != [0, 0] {
            // When computing the checksum, a checksum of 0 is sent as 0xFFFF.
            let target = if packet.header.checksum == [0xFF, 0xFF] {
                0
            } else {
                BigEndian::read_u16(&packet.header.checksum)
            };
            if packet.compute_checksum(src_ip, dst_ip).ok_or(Err::Format)? != target {
                return Err(Err::Checksum);
            }
        } else if A::Version::VERSION.is_v6() {
            return Err(Err::Checksum);
        }

        Ok(packet)
    }
}

impl<B: ByteSlice> UdpPacket<B> {
    // Compute the UDP checksum, skipping the checksum field itself. Returns
    // None if the packet size is too large.
    fn compute_checksum<A: IpAddr>(&self, src_ip: A, dst_ip: A) -> Option<u16> {
        // See for details: https://en.wikipedia.org/wiki/User_Datagram_Protocol#Checksum_computation
        let mut c = Checksum::new();
        c.add_bytes(src_ip.bytes());
        c.add_bytes(dst_ip.bytes());
        if A::Version::VERSION.is_v4() {
            c.add_bytes(&[0]);
            c.add_bytes(&[IpProto::Udp as u8]);
            c.add_bytes(&self.header.length);
        } else {
            let len = self.header.bytes().len() + self.body.len();
            if !fits_in_u32(len) {
                return None;
            }
            let mut len_bytes = [0; 4];
            BigEndian::write_u32(&mut len_bytes, len as u32);
            c.add_bytes(&len_bytes);
            c.add_bytes(&[0; 3]);
            c.add_bytes(&[IpProto::Udp as u8]);
        }
        c.add_bytes(&self.header.src_port);
        c.add_bytes(&self.header.dst_port);
        c.add_bytes(&self.header.length);
        c.add_bytes(&self.body);
        Some(c.sum())
    }

    /// The packet body.
    pub fn body(&self) -> &[u8] {
        self.body.deref()
    }

    /// The source UDP port, if any.
    ///
    /// The source port is optional, and may have been omitted by the sender.
    pub fn src_port(&self) -> Option<NonZeroU16> {
        NonZeroU16::new(self.header.src_port())
    }

    /// The destination UDP port.
    pub fn dst_port(&self) -> NonZeroU16 {
        NonZeroU16::new(self.header.dst_port()).unwrap()
    }

    /// Did this packet have a checksum?
    ///
    /// On IPv4, the sender may optionally omit the checksum. If this function
    /// returns false, the sender ommitted the checksum, and `parse` will not
    /// have validated it.
    ///
    /// On IPv6, it is guaranteed that `checksummed` will return true because
    /// IPv6 requires a checksum, and so any UDP packet missing one will fail
    /// validation in `parse`.
    pub fn checksummed(&self) -> bool {
        self.header.checksum() != 0
    }
}

// NOTE(joshlf): In order to ensure that the checksum is always valid, we don't
// expose any setters for the fields of the UDP packet; the only way to set them
// is via UdpPacket::create. This, combined with checksum validation performed
// in UdpPacket::parse, provides the invariant that a UdpPacket always has a
// valid checksum.

impl<'a> UdpPacket<&'a mut [u8]> {
    /// Serialize a UDP packet in an existing buffer.
    ///
    /// `create` creates a `UdpPacket` which uses the provided `buffer` for its
    /// storage, initializing all header fields and calculating the checksum. It
    /// treats the range identified by `body` as being the frame body. It uses
    /// the last bytes of `header` before the body to store the header, and
    /// returns the number of remaining prefix bytes to the caller for use in
    /// adding other headers.
    ///
    /// # Examples
    ///
    /// ```rust
    /// let mut buffer = [0u8; 1024];
    /// (&mut buffer[512..]).copy_from_slice(body);
    /// let frame = UdpPacket::create(&mut buffer, 512.., src_ip, dst_ip, src_port, dst_port);
    /// ```
    ///
    /// # Panics
    ///
    /// `create` panics if there is insufficient room preceding the body to
    /// store the UDP header, or if `body` is not in range of `buffer`. The
    /// caller can guarantee that there will be enough room by providing at
    /// least `HEADER_LEN` pre-body bytes.
    ///
    /// When encapsulating in IPv4,
    pub fn create<R: RangeBounds<usize>, A: IpAddr>(
        buffer: &'a mut [u8], body: R, src_ip: A, dst_ip: A, src_port: Option<NonZeroU16>,
        dst_port: NonZeroU16,
    ) -> (UdpPacket<&'a mut [u8]>, usize) {
        // See for details: https://en.wikipedia.org/wiki/User_Datagram_Protocol#Packet_structure

        let (header, body, _) =
            extract_slice_range(buffer, body).expect("body range is out of bounds of buffer");
        let (prefix, header) = LayoutVerified::<_, Header>::new_unaligned_from_suffix(header)
            .expect("too few bytes for UDP header");
        let mut packet = UdpPacket { header, body };

        packet
            .header
            .set_src_port(src_port.map(|port| port.get()).unwrap_or(0));
        packet.header.set_dst_port(dst_port.get());
        let total_len = HEADER_LEN + packet.body.len();
        let len_field = if fits_in_u16(total_len) {
            total_len as u16
        } else if A::Version::VERSION.is_v6() {
            // IPv6 supports jumbograms, so a UDP packet may be greater than
            // 2^16 bytes in size. In this case, the size doesn't fit in the
            // 16-bit length field in the header, and so the length field is set
            // to zero to indicate this.
            0u16
        } else {
            panic!(
                "total UDP packet length of {} bytes overflows 16-bit length field of UDP header",
                total_len
            );
        };
        BigEndian::write_u16(&mut packet.header.length, len_field);

        // This ignores the checksum field in the header, so it's fine that we
        // haven't set it yet, and so it could be filled with arbitrary bytes.
        let c = packet.compute_checksum(src_ip, dst_ip).expect(&format!(
            "total UDP packet length of {} bytes overflow 32-bit length field of pseudo-header",
            total_len
        ));
        BigEndian::write_u16(
            &mut packet.header.checksum,
            if c == 0 {
                // When computing the checksum, a checksum of 0 is sent as 0xFFFF.
                0xFFFF
            } else {
                c
            },
        );
        (packet, prefix.len())
    }
}

// needed by Result::unwrap_err in the tests below
#[cfg(test)]
impl<B> Debug for UdpPacket<B> {
    fn fmt(&self, fmt: &mut Formatter) -> fmt::Result {
        write!(fmt, "UdpPacket")
    }
}

#[cfg(test)]
mod tests {
    use ip::{Ipv4Addr, Ipv6Addr};

    use super::*;

    const TEST_SRC_IPV4: Ipv4Addr = Ipv4Addr::new([1, 2, 3, 4]);
    const TEST_DST_IPV4: Ipv4Addr = Ipv4Addr::new([5, 6, 7, 8]);
    const TEST_SRC_IPV6: Ipv6Addr =
        Ipv6Addr::new([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]);
    const TEST_DST_IPV6: Ipv6Addr = Ipv6Addr::new([
        17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    ]);

    #[test]
    fn test_parse() {
        // source port of 0 (meaning none) is allowed, as is a missing checksum
        let buf = [0, 0, 1, 2, 0, 8, 0, 0];
        let packet = UdpPacket::parse(&buf[..], TEST_SRC_IPV4, TEST_DST_IPV4).unwrap();
        assert!(packet.src_port().is_none());
        assert_eq!(packet.dst_port().get(), BigEndian::read_u16(&[1, 2]));
        assert!(!packet.checksummed());
        assert!(packet.body().is_empty());

        // length of 0 is allowed in IPv6
        let buf = [0, 0, 1, 2, 0, 0, 0xFF, 0xE4];
        let packet = UdpPacket::parse(&buf[..], TEST_SRC_IPV6, TEST_DST_IPV6).unwrap();
        assert!(packet.src_port().is_none());
        assert_eq!(packet.dst_port().get(), BigEndian::read_u16(&[1, 2]));
        assert!(packet.checksummed());
        assert!(packet.body().is_empty());
    }

    #[test]
    fn test_create() {
        let mut buf = [0; 8];
        {
            let (packet, prefix_len) = UdpPacket::create(
                &mut buf,
                8..,
                TEST_SRC_IPV4,
                TEST_DST_IPV4,
                NonZeroU16::new(1),
                NonZeroU16::new(2).unwrap(),
            );
            assert_eq!(prefix_len, 0);
            assert_eq!(packet.src_port().unwrap().get(), 1);
            assert_eq!(packet.dst_port().get(), 2);
            assert!(packet.checksummed());
        }
        assert_eq!(buf, [0, 1, 0, 2, 0, 8, 222, 216]);
    }

    #[test]
    fn test_parse_fail() {
        // Test that while a given byte pattern optionally succeeds, zeroing out
        // certain bytes causes failure. `zero` is a list of byte indices to
        // zero out that should cause failure.
        fn test_zero<I: IpAddr>(
            src: I, dst: I, succeeds: bool, zero: &[usize], err_is_checksum: bool,
        ) {
            // Set checksum to zero so that, in IPV4, it will be ignored. In
            // IPv6, this /is/ the test.
            let mut buf = [1, 2, 3, 4, 0, 8, 0, 0];
            if succeeds {
                assert!(UdpPacket::parse(&buf[..], src, dst).is_ok());
            }
            for idx in zero {
                buf[*idx] = 0;
            }
            assert_eq!(
                UdpPacket::parse(&buf[..], src, dst)
                    .unwrap_err()
                    .is_checksum(),
                err_is_checksum
            );
        }

        // destination port of 0 is disallowed
        test_zero(TEST_SRC_IPV4, TEST_DST_IPV4, true, &[2, 3], false);
        // length of 0 is disallowed in IPv4
        test_zero(TEST_SRC_IPV4, TEST_DST_IPV4, true, &[4, 5], false);
        // missing checksum is disallowed in IPv6; this won't succeed ahead of
        // time because the checksum bytes are already zero
        test_zero(TEST_SRC_IPV6, TEST_DST_IPV6, false, &[], true);

        // Total length of 2^32 or greater is disallowed in IPv6. If we created
        // a 4 gigabyte buffer, this test would take a very long time to run.
        // Instead, we allocate enough space for the header (which may actually
        // be read before the length check fails), and then /very unsafely/
        // pretend that it's 2^32 bytes long.
        let buf = vec![0, 0, 1, 2, 0, 0, 0xFF, 0xE4];
        let buf = unsafe { ::std::slice::from_raw_parts(buf.as_ptr(), 1 << 32) };
        assert!(
            !UdpPacket::parse(&buf[..], TEST_SRC_IPV6, TEST_DST_IPV6)
                .unwrap_err()
                .is_checksum()
        );
    }

    #[test]
    #[should_panic]
    fn test_create_fail_header_too_short() {
        let mut buf = [0; 7];
        UdpPacket::create(
            &mut buf,
            7..,
            TEST_SRC_IPV4,
            TEST_DST_IPV4,
            None,
            NonZeroU16::new(1).unwrap(),
        );
    }

    #[test]
    #[should_panic]
    fn test_create_fail_packet_too_long_ipv4() {
        let mut buf = [0; 1 << 16];
        UdpPacket::create(
            &mut buf,
            8..,
            TEST_SRC_IPV4,
            TEST_DST_IPV4,
            None,
            NonZeroU16::new(1).unwrap(),
        );
    }

    #[test]
    #[should_panic]
    fn test_create_fail_packet_too_long_ipv6() {
        // Total length of 2^32 or greater is disallowed in IPv6. If we created
        // a 4 gigabyte buffer, this test would take a very long time to run.
        // Instead, we allocate enough space for the header (which may actually
        // be written before the length check fails), and then /very unsafely/
        // pretend that it's 2^32 bytes long.
        let mut buf = vec![0, 0, 0, 0, 0, 0, 0, 0];
        let mut buf = unsafe { ::std::slice::from_raw_parts_mut(buf.as_mut_ptr(), 1 << 32) };
        UdpPacket::create(
            &mut buf,
            8..,
            TEST_SRC_IPV4,
            TEST_DST_IPV4,
            None,
            NonZeroU16::new(1).unwrap(),
        );
    }
}
