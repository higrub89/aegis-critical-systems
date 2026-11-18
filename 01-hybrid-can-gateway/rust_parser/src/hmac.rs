//! Constant-Time `no_std` FIPS 180-4 SHA-256 and RFC 2104 HMAC-SHA256.
//! Enforces zero heap allocation and zero panic paths.

#[rustfmt::skip]
const SHA256_K: [u32; 64] = [
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
const SHA256_INIT: [u32; 8] = [
    0x6a09_e667, 0xbb67_ae85, 0x3c6e_f372, 0xa54f_f53a,
    0x510e_527f, 0x9b05_688c, 0x1f83_d9ab, 0x5be0_cd19,
];

/// Streaming `no_std` SHA-256 context backed purely by fixed-size stack arrays.
pub struct Sha256 {
    state: [u32; 8],
    block: [u8; 64],
    block_len: usize,
    total_bytes: u64,
}

impl Default for Sha256 {
    fn default() -> Self {
        Self::new()
    }
}

impl Sha256 {
    /// Creates a new initialized SHA-256 state.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            state: SHA256_INIT,
            block: [0u8; 64],
            block_len: 0,
            total_bytes: 0,
        }
    }

    /// Feeds `data` into the SHA-256 compression pipeline without heap allocation.
    pub fn update(&mut self, data: &[u8]) {
        for &byte in data {
            if let Some(slot) = self.block.get_mut(self.block_len) {
                *slot = byte;
                self.block_len = self.block_len.wrapping_add(1);
                self.total_bytes = self.total_bytes.wrapping_add(1);
                if self.block_len == 64 {
                    let full_block = self.block;
                    self.compress(&full_block);
                    self.block_len = 0;
                }
            }
        }
    }

    /// Finalizes the digest and returns the 32-byte SHA-256 hash.
    #[must_use]
    pub fn finalize(mut self) -> [u8; 32] {
        let bit_len = self.total_bytes.wrapping_mul(8);
        self.update(&[0x80]);
        while self.block_len != 56 {
            self.update(&[0x00]);
        }
        self.update(&bit_len.to_be_bytes());

        let mut out = [0u8; 32];
        for (idx, &word) in self.state.iter().enumerate() {
            let bytes = word.to_be_bytes();
            let offset = idx.wrapping_mul(4);
            if offset.wrapping_add(4) <= out.len() {
                out[offset..offset + 4].copy_from_slice(&bytes);
            }
        }
        out
    }

    fn compress(&mut self, chunk: &[u8; 64]) {
        let mut schedule = [0u32; 64];
        for (idx, slot) in schedule.iter_mut().take(16).enumerate() {
            let offset = idx.wrapping_mul(4);
            let b0 = u32::from(chunk.get(offset).copied().unwrap_or(0));
            let b1 = u32::from(chunk.get(offset.wrapping_add(1)).copied().unwrap_or(0));
            let b2 = u32::from(chunk.get(offset.wrapping_add(2)).copied().unwrap_or(0));
            let b3 = u32::from(chunk.get(offset.wrapping_add(3)).copied().unwrap_or(0));
            *slot = (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
        }

        for idx in 16usize..64 {
            let w15 = schedule.get(idx.wrapping_sub(15)).copied().unwrap_or(0);
            let w2 = schedule.get(idx.wrapping_sub(2)).copied().unwrap_or(0);
            let w16 = schedule.get(idx.wrapping_sub(16)).copied().unwrap_or(0);
            let w7 = schedule.get(idx.wrapping_sub(7)).copied().unwrap_or(0);

            let s0 = w15.rotate_right(7) ^ w15.rotate_right(18) ^ (w15 >> 3);
            let s1 = w2.rotate_right(17) ^ w2.rotate_right(19) ^ (w2 >> 10);
            if let Some(slot) = schedule.get_mut(idx) {
                *slot = w16.wrapping_add(s0).wrapping_add(w7).wrapping_add(s1);
            }
        }

        let mut reg_a = self.state[0];
        let mut reg_b = self.state[1];
        let mut reg_c = self.state[2];
        let mut reg_d = self.state[3];
        let mut reg_e = self.state[4];
        let mut reg_f = self.state[5];
        let mut reg_g = self.state[6];
        let mut reg_h = self.state[7];

        for (&k_val, &w_val) in SHA256_K.iter().zip(schedule.iter()) {
            let s1 = reg_e.rotate_right(6) ^ reg_e.rotate_right(11) ^ reg_e.rotate_right(25);
            let ch = (reg_e & reg_f) ^ ((!reg_e) & reg_g);
            let temp1 = reg_h
                .wrapping_add(s1)
                .wrapping_add(ch)
                .wrapping_add(k_val)
                .wrapping_add(w_val);
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

        self.state[0] = self.state[0].wrapping_add(reg_a);
        self.state[1] = self.state[1].wrapping_add(reg_b);
        self.state[2] = self.state[2].wrapping_add(reg_c);
        self.state[3] = self.state[3].wrapping_add(reg_d);
        self.state[4] = self.state[4].wrapping_add(reg_e);
        self.state[5] = self.state[5].wrapping_add(reg_f);
        self.state[6] = self.state[6].wrapping_add(reg_g);
        self.state[7] = self.state[7].wrapping_add(reg_h);
    }
}

/// Computes RFC 2104 HMAC-SHA256 over `msg` using `key` in zero-heap `no_std`.
#[must_use]
pub fn hmac_sha256(key: &[u8], msg: &[u8]) -> [u8; 32] {
    let mut key_block = [0u8; 64];
    if key.len() > 64 {
        let mut hasher = Sha256::new();
        hasher.update(key);
        let hashed_key = hasher.finalize();
        for (dst, &src) in key_block.iter_mut().zip(hashed_key.iter()) {
            *dst = src;
        }
    } else {
        for (dst, &src) in key_block.iter_mut().zip(key.iter()) {
            *dst = src;
        }
    }

    let mut ipad = [0x36u8; 64];
    let mut opad = [0x5cu8; 64];
    for (idx, &k_byte) in key_block.iter().enumerate() {
        if let Some(slot) = ipad.get_mut(idx) {
            *slot ^= k_byte;
        }
        if let Some(slot) = opad.get_mut(idx) {
            *slot ^= k_byte;
        }
    }

    let mut inner = Sha256::new();
    inner.update(&ipad);
    inner.update(msg);
    let inner_digest = inner.finalize();

    let mut outer = Sha256::new();
    outer.update(&opad);
    outer.update(&inner_digest);
    outer.finalize()
}

/// Constant-time 32-byte tag equality check (zero branching on secret bytes).
#[must_use]
pub fn constant_time_eq_32(lhs: &[u8; 32], rhs: &[u8; 32]) -> bool {
    let mut diff: u8 = 0;
    for (&a_byte, &b_byte) in lhs.iter().zip(rhs.iter()) {
        diff |= a_byte ^ b_byte;
    }
    diff == 0
}
