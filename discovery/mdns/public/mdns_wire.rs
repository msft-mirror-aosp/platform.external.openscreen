// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use simple_dns::rdata::{NsecTypeBitMap, RData, A, AAAA, NSEC, NULL, PTR, SRV, TXT};
use simple_dns::{
    CharacterString, Label, Name, Packet, PacketFlag, Question, ResourceRecord, CLASS, QCLASS,
    QTYPE,
};

#[cxx::bridge(namespace = "openscreen::discovery")]
pub mod ffi {
    #[derive(Debug, Default, Clone)]
    pub struct DnsHeader {
        pub id: u16,
        pub flags: u16,
        pub is_response: bool,
        pub is_truncated: bool,
        pub is_authoritative: bool,
        pub question_count: u16,
        pub answer_count: u16,
        pub authority_count: u16,
        pub additional_count: u16,
    }

    #[derive(Debug, Default, Clone)]
    pub struct DnsQuestion {
        pub name_labels: Vec<String>,
        pub qtype: u16,
        pub qclass: u16,
        pub is_unicast_response: bool,
    }

    #[derive(Debug, Default, Clone)]
    pub struct DnsRecord {
        pub name_labels: Vec<String>,
        pub rtype: u16,
        pub rclass: u16,
        pub is_cache_flush: bool,
        pub ttl_seconds: u32,
        pub rdata_bytes: Vec<u8>,
        pub ptr_target_labels: Vec<String>,
        pub srv_priority: u16,
        pub srv_weight: u16,
        pub srv_port: u16,
        pub srv_target_labels: Vec<String>,
        pub nsec_next_labels: Vec<String>,
    }

    #[derive(Debug, Default, Clone)]
    pub struct DnsMessage {
        pub header: DnsHeader,
        pub questions: Vec<DnsQuestion>,
        pub answers: Vec<DnsRecord>,
        pub authority_records: Vec<DnsRecord>,
        pub additional_records: Vec<DnsRecord>,
    }

    extern "Rust" {
        fn parse_header(buffer: &[u8], header: &mut DnsHeader) -> bool;
        fn parse_message(buffer: &[u8], message: &mut DnsMessage) -> bool;
        fn write_message(message: &DnsMessage, out: &mut Vec<u8>) -> bool;
    }
}

fn name_to_labels(name: &Name) -> Vec<String> {
    name.iter().map(|l| l.to_string()).collect()
}

fn labels_to_name(labels: &[String]) -> Result<Name<'static>, ()> {
    let name_labels: Vec<Label<'static>> =
        labels.iter().map(|l| Label::new_unchecked(l.as_bytes().to_vec())).collect();
    Ok(Name::new_with_labels(&name_labels).into_owned())
}

fn class_to_u16(class: CLASS) -> u16 {
    match class {
        CLASS::IN => 1,
        CLASS::CS => 2,
        CLASS::CH => 3,
        CLASS::HS => 4,
        CLASS::NONE => 254,
    }
}

fn convert_question(q: &Question) -> ffi::DnsQuestion {
    let qclass_raw: u16 = q.qclass.into();
    ffi::DnsQuestion {
        name_labels: name_to_labels(&q.qname),
        qtype: q.qtype.into(),
        qclass: qclass_raw & 0x7FFF,
        is_unicast_response: q.unicast_response,
    }
}

fn convert_record(rr: &ResourceRecord) -> ffi::DnsRecord {
    let rclass_raw = class_to_u16(rr.class);
    let rtype_raw: u16 = rr.rdata.type_code().into();
    let mut record = ffi::DnsRecord {
        name_labels: name_to_labels(&rr.name),
        rtype: rtype_raw,
        rclass: rclass_raw & 0x7FFF,
        is_cache_flush: rr.cache_flush,
        ttl_seconds: rr.ttl,
        ..Default::default()
    };

    match &rr.rdata {
        RData::A(a) => {
            record.rdata_bytes = a.address.to_be_bytes().to_vec();
        }
        RData::AAAA(aaaa) => {
            record.rdata_bytes = aaaa.address.to_be_bytes().to_vec();
        }
        RData::PTR(ptr) => {
            record.ptr_target_labels = name_to_labels(&ptr.0);
        }
        RData::TXT(txt) => {
            for (key, val) in txt.iter_raw() {
                let entry_len = if let Some(v) = val { key.len() + 1 + v.len() } else { key.len() };
                let entry_len: u8 = entry_len.try_into().expect("value doesn't fit in u8");
                record.rdata_bytes.push(entry_len);
                record.rdata_bytes.extend_from_slice(key);
                if let Some(v) = val {
                    record.rdata_bytes.push(b'=');
                    record.rdata_bytes.extend_from_slice(v);
                }
            }
        }
        RData::SRV(srv) => {
            record.srv_priority = srv.priority;
            record.srv_weight = srv.weight;
            record.srv_port = srv.port;
            record.srv_target_labels = name_to_labels(&srv.target);
        }
        RData::NSEC(nsec) => {
            record.nsec_next_labels = name_to_labels(&nsec.next_name);
            for map in &nsec.type_bit_maps {
                record.rdata_bytes.push(map.window_block);
                record.rdata_bytes.push(map.bitmap.len() as u8);
                record.rdata_bytes.extend_from_slice(map.bitmap.as_ref());
            }
        }
        RData::NULL(_, null_data) => {
            record.rdata_bytes = null_data.get_data().to_vec();
        }
        _ => {}
    }

    record
}

const TYPE_A: u16 = 1;
const TYPE_PTR: u16 = 12;
const TYPE_TXT: u16 = 16;
const TYPE_AAAA: u16 = 28;
const TYPE_SRV: u16 = 33;
const TYPE_NSEC: u16 = 47;

fn convert_record_rdata(r: &ffi::DnsRecord) -> Option<RData<'static>> {
    match r.rtype {
        TYPE_A => {
            if r.rdata_bytes.len() != 4 {
                return None;
            }
            let bytes: [u8; 4] = r.rdata_bytes.as_slice().try_into().ok()?;
            Some(RData::A(A { address: u32::from_be_bytes(bytes) }))
        }
        TYPE_AAAA => {
            if r.rdata_bytes.len() != 16 {
                return None;
            }
            let bytes: [u8; 16] = r.rdata_bytes.as_slice().try_into().ok()?;
            Some(RData::AAAA(AAAA { address: u128::from_be_bytes(bytes) }))
        }
        TYPE_PTR => {
            let target = labels_to_name(&r.ptr_target_labels).ok()?;
            Some(RData::PTR(PTR(target)))
        }
        TYPE_SRV => {
            let target = labels_to_name(&r.srv_target_labels).ok()?;
            Some(RData::SRV(SRV {
                priority: r.srv_priority,
                weight: r.srv_weight,
                port: r.srv_port,
                target,
            }))
        }
        TYPE_TXT => {
            let mut txt = TXT::new();
            let mut offset = 0;
            while offset < r.rdata_bytes.len() {
                let len = r.rdata_bytes[offset] as usize;
                offset += 1;
                if offset + len > r.rdata_bytes.len() {
                    return None;
                }
                let cs = CharacterString::new(&r.rdata_bytes[offset..offset + len]).ok()?;
                txt.add_char_string(cs.into_owned());
                offset += len;
            }
            Some(RData::TXT(txt))
        }
        TYPE_NSEC => {
            let next_name = labels_to_name(&r.nsec_next_labels).ok()?;
            let mut type_bit_maps = Vec::new();
            let mut offset = 0;
            while offset + 2 <= r.rdata_bytes.len() {
                let window_block = r.rdata_bytes[offset];
                offset += 1;
                let bitmap_len = r.rdata_bytes[offset] as usize;
                offset += 1;
                if offset + bitmap_len > r.rdata_bytes.len() {
                    return None;
                }
                let bitmap = r.rdata_bytes[offset..offset + bitmap_len].to_vec();
                offset += bitmap_len;
                type_bit_maps.push(NsecTypeBitMap { window_block, bitmap: bitmap.into() });
            }
            Some(RData::NSEC(NSEC { next_name, type_bit_maps }))
        }
        _ => {
            let null_data = NULL::new(&r.rdata_bytes).ok()?;
            Some(RData::NULL(r.rtype, null_data.into_owned()))
        }
    }
}

pub fn parse_header(buffer: &[u8], header: &mut ffi::DnsHeader) -> bool {
    let Ok(packet) = Packet::parse(buffer) else {
        return false;
    };
    *header = ffi::DnsHeader {
        id: packet.id(),
        flags: 0,
        is_response: packet.has_flags(PacketFlag::RESPONSE),
        is_truncated: packet.has_flags(PacketFlag::TRUNCATION),
        is_authoritative: packet.has_flags(PacketFlag::AUTHORITATIVE_ANSWER),
        question_count: packet.questions.len() as u16,
        answer_count: packet.answers.len() as u16,
        authority_count: packet.name_servers.len() as u16,
        additional_count: packet.additional_records.len() as u16,
    };
    true
}

pub fn parse_message(buffer: &[u8], message: &mut ffi::DnsMessage) -> bool {
    let Ok(packet) = Packet::parse(buffer) else {
        return false;
    };

    let mut header = ffi::DnsHeader::default();
    parse_header(buffer, &mut header);

    let questions: Vec<ffi::DnsQuestion> = packet.questions.iter().map(convert_question).collect();
    let answers: Vec<ffi::DnsRecord> = packet.answers.iter().map(convert_record).collect();
    let authority_records: Vec<ffi::DnsRecord> =
        packet.name_servers.iter().map(convert_record).collect();
    let additional_records: Vec<ffi::DnsRecord> =
        packet.additional_records.iter().map(convert_record).collect();

    *message =
        ffi::DnsMessage { header, questions, answers, authority_records, additional_records };

    true
}

pub fn write_message(message: &ffi::DnsMessage, out: &mut Vec<u8>) -> bool {
    let mut packet = if message.header.is_response {
        Packet::new_reply(message.header.id)
    } else {
        Packet::new_query(message.header.id)
    };
    if message.header.is_authoritative {
        packet.set_flags(PacketFlag::AUTHORITATIVE_ANSWER);
    }
    if message.header.is_truncated {
        packet.set_flags(PacketFlag::TRUNCATION);
    }

    for q in &message.questions {
        let Ok(qname) = labels_to_name(&q.name_labels) else {
            return false;
        };
        let Ok(qtype) = QTYPE::try_from(q.qtype) else {
            return false;
        };
        let Ok(qclass) = QCLASS::try_from(q.qclass) else {
            return false;
        };
        packet.questions.push(Question::new(qname, qtype, qclass, q.is_unicast_response));
    }

    let convert_rr = |r: &ffi::DnsRecord| -> Option<ResourceRecord<'static>> {
        let name = labels_to_name(&r.name_labels).ok()?;
        let class = CLASS::try_from(r.rclass).ok()?;
        let rdata = convert_record_rdata(r)?;
        Some(
            ResourceRecord::new(name, class, r.ttl_seconds, rdata)
                .with_cache_flush(r.is_cache_flush),
        )
    };

    let add_records = |src: &[ffi::DnsRecord], dest: &mut Vec<ResourceRecord<'static>>| -> bool {
        dest.reserve(src.len());
        for r in src {
            match convert_rr(r) {
                Some(rr) => dest.push(rr),
                None => return false,
            }
        }
        true
    };

    if !add_records(&message.answers, &mut packet.answers)
        || !add_records(&message.authority_records, &mut packet.name_servers)
        || !add_records(&message.additional_records, &mut packet.additional_records)
    {
        return false;
    }

    let Ok(bytes) = packet.build_bytes_vec_compressed() else {
        return false;
    };
    *out = bytes;
    true
}
