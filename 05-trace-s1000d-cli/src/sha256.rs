//! FIPS 180-4 Deterministic SHA-256 Implementation (Zero-Dependency, Safe Rust).

use std::fmt::Write as _;

#[rustfmt::skip]
const K: [u32; 64] = [
    0x428a_2f98, 0x7137_4491, 0xb5c0_fbcf, 0xe9b5_dba5,
    0x3956_c25b, 0x59f1_11f1, 0x923f_82a4, 0xab1c_5ed5,
    0xd807_aa98, 0x1283_5b01, 0x2431_85be, 0x550c_7dc3,
    0x72be_5d74, 0x80de_b1fe, 0x9bdc_06a7, 0xc19b_f174,
    0xe49b_69c1, 0xefbe_4786, 0x0fc1_9dc6, 0x240c_a1cc,
    0x2de9_2c6f, 0x4a74_84aa, 0x5cb0_a9dc, 0x76f9_88da,
    0x983e_5152, 0xa831_c66d, 0xb003_27c8, 0xbf59_7fc7,
    0xc6e0_0bf3, 0xd5a7_9147, 0x06ca_6351, 0x1429_2967,
    0x27b7_0a85, 0x2e1b_2138, 0x4d2c_6dfc, 0x5338_0d13,
    0x650a_7354, 0x766a_0abb, 0x81c2_c92e, 0x9272_2c85,
    0xa2bf_e8a1, 0xa81a_664b, 0xc24b_8b70, 0xc76c_51a3,
    0xd192_e819, 0xd699_0624, 0xf40e_3585, 0x106a_a070,
    0x19a4_c116, 0x1e37_6c08, 0x2748_774c, 0x34b0_bcb5,
    0x391c_0cb3, 0x4ed8_aa4a, 0x5b9c_ca4f, 0x682e_6ff3,
    0x748f_82ee, 0x78a5_636f, 0x84c8_7814, 0x8cc7_0208,
    0x90be_fffa, 0xa450_6ceb, 0xbef9_a3f7, 0xc671_78f2,
];

#[rustfmt::skip]
const H_INIT: [u32; 8] = [
    0x6a09_e667, 0xbb67_ae85, 0x3c6e_f372, 0xa54f_f53a,
    0x510e_527f, 0x9b05_688c, 0x1f83_d9ab, 0x5be0_cd19,
];

/// Computes the FIPS 180-4 SHA-256 digest of `data` as a 32-byte array.
#[must_use]
pub fn sha256_bytes(data: &[u8]) -> [u8; 32] {
    let mut state = H_INIT;
    let bit_len = (data.len() as u64).wrapping_mul(8);

    let mut padded = Vec::with_capacity(data.len() + 72);
    padded.extend_from_slice(data);
    padded.push(0x80);
    while (padded.len() % 64) != 56 {
        padded.push(0x00);
    }
    padded.extend_from_slice(&bit_len.to_be_bytes());

    let num_blocks = padded.len() / 64;
    for b in 0..num_blocks {
        let chunk = &padded[b * 64..(b + 1) * 64];
        let mut schedule = [0u32; 64];
        for (i, slot) in schedule.iter_mut().enumerate().take(16) {
            let offset = i * 4;
            let b0 = u32::from(chunk[offset]);
            let b1 = u32::from(chunk[offset + 1]);
            let b2 = u32::from(chunk[offset + 2]);
            let b3 = u32::from(chunk[offset + 3]);
            *slot = (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
        }
        for i in 16..64 {
            let s0 = schedule[i - 15].rotate_right(7)
                ^ schedule[i - 15].rotate_right(18)
                ^ (schedule[i - 15] >> 3);
            let s1 = schedule[i - 2].rotate_right(17)
                ^ schedule[i - 2].rotate_right(19)
                ^ (schedule[i - 2] >> 10);
            schedule[i] = schedule[i - 16]
                .wrapping_add(s0)
                .wrapping_add(schedule[i - 7])
                .wrapping_add(s1);
        }

        let mut reg_a = state[0];
        let mut reg_b = state[1];
        let mut reg_c = state[2];
        let mut reg_d = state[3];
        let mut reg_e = state[4];
        let mut reg_f = state[5];
        let mut reg_g = state[6];
        let mut reg_h = state[7];

        for i in 0..64 {
            let s1 = reg_e.rotate_right(6) ^ reg_e.rotate_right(11) ^ reg_e.rotate_right(25);
            let ch = (reg_e & reg_f) ^ ((!reg_e) & reg_g);
            let temp1 = reg_h
                .wrapping_add(s1)
                .wrapping_add(ch)
                .wrapping_add(K[i])
                .wrapping_add(schedule[i]);
            let s0 = reg_a.rotate_right(2) ^ reg_a.rotate_right(13) ^ reg_a.rotate_right(22);
            let maj = (reg_a & reg_b) ^ (reg_a & reg_c) ^ (reg_b & reg_c);
            let temp2 = s0.wrapping_add(maj);

            reg_h = reg_g;
            reg_g = reg_f;
            reg_f = reg_e;
            reg_e = reg_d.wrapping_add(temp1);
            reg_d = reg_c;
            reg_c = reg_b;
            reg_b = reg_a;
            reg_a = temp1.wrapping_add(temp2);
        }

        state[0] = state[0].wrapping_add(reg_a);
        state[1] = state[1].wrapping_add(reg_b);
        state[2] = state[2].wrapping_add(reg_c);
        state[3] = state[3].wrapping_add(reg_d);
        state[4] = state[4].wrapping_add(reg_e);
        state[5] = state[5].wrapping_add(reg_f);
        state[6] = state[6].wrapping_add(reg_g);
        state[7] = state[7].wrapping_add(reg_h);
    }

    let mut out = [0u8; 32];
    for (i, word) in state.iter().enumerate() {
        let bytes = word.to_be_bytes();
        out[i * 4] = bytes[0];
        out[i * 4 + 1] = bytes[1];
        out[i * 4 + 2] = bytes[2];
        out[i * 4 + 3] = bytes[3];
    }
    out
}

/// Computes the lowercase hexadecimal SHA-256 digest of `data`.
#[must_use]
pub fn sha256_hex(data: &[u8]) -> String {
    let digest = sha256_bytes(data);
    let mut hex = String::with_capacity(64);
    for byte in digest {
        let _ = write!(hex, "{byte:02x}");
    }
    hex
}
