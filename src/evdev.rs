//! Kernel input_event record layout. The raw read() ABI differs by word size:
//! 64-bit kernels emit 24-byte records (16-byte timeval), 32-bit kernels
//! (reMarkable 2, armv7) emit 16-byte records (8-byte timeval) regardless of
//! userspace _TIME_BITS.

#[cfg(target_pointer_width = "64")]
pub const EV_REC: usize = 24;
#[cfg(target_pointer_width = "64")]
pub const EV_OFF: usize = 16;

#[cfg(target_pointer_width = "32")]
pub const EV_REC: usize = 16;
#[cfg(target_pointer_width = "32")]
pub const EV_OFF: usize = 8;

/// (type, code, value) from one raw record.
#[inline]
pub fn parse(chunk: &[u8]) -> (u16, u16, i32) {
    (
        u16::from_le_bytes(chunk[EV_OFF..EV_OFF + 2].try_into().unwrap()),
        u16::from_le_bytes(chunk[EV_OFF + 2..EV_OFF + 4].try_into().unwrap()),
        i32::from_le_bytes(chunk[EV_OFF + 4..EV_OFF + 8].try_into().unwrap()),
    )
}
