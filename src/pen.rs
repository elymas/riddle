//! Raw evdev pen input: the full digitizer, bypassing Qt's filtered view.
//! Gives us 0-4096 pressure, tilt, hover, and the eraser tip (BTN_TOOL_RUBBER),
//! at the hardware event rate.
//!
//! The device is grabbed (EVIOCGRAB) while the diary is open so xochitl
//! doesn't also react to the pen; released automatically on close/exit.

use std::io;
use std::os::fd::RawFd;

use crate::evdev::{parse, EV_REC};
use crate::fb::{SCREEN_H, SCREEN_W};

// Digitizer axis ranges (EVIOCGABS-measured).
// Paper Pro "Elan marker input": axes aligned with the portrait screen.
#[cfg(not(target_arch = "arm"))]
const DIGI_MAX_X: i32 = 11180;
#[cfg(not(target_arch = "arm"))]
const DIGI_MAX_Y: i32 = 15340;
// reMarkable 2 "Wacom I2C Digitizer": rotated 90° vs the portrait screen —
// raw X (the long axis) runs along screen Y, raw Y along screen X.
#[cfg(target_arch = "arm")]
const DIGI_MAX_X: i32 = 20966;
#[cfg(target_arch = "arm")]
const DIGI_MAX_Y: i32 = 15725;
pub const MAX_PRESSURE: i32 = 4096;

const EV_SYN: u16 = 0;
const EV_KEY: u16 = 1;
const EV_ABS: u16 = 3;
const SYN_REPORT: u16 = 0;
const ABS_X: u16 = 0;
const ABS_Y: u16 = 1;
const ABS_PRESSURE: u16 = 24;
const BTN_TOOL_PEN: u16 = 320;
const BTN_TOOL_RUBBER: u16 = 321;
const BTN_TOUCH: u16 = 330;

const EVIOCGRAB: libc::c_ulong = 0x40044590;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Tool {
    Pen,
    Eraser,
}

#[derive(Debug, Clone, Copy)]
pub struct PenSample {
    /// Screen coordinates.
    pub x: i32,
    pub y: i32,
    /// 0..4096
    pub pressure: i32,
    pub tool: Tool,
    pub touching: bool,
    /// True from tool-in-range until the pen leaves the digitizer.
    pub proximity: bool,
}

pub struct PenDevice {
    fd: RawFd,
    // Accumulated state between SYN_REPORTs.
    raw_x: i32,
    raw_y: i32,
    pressure: i32,
    tool: Tool,
    touching: bool,
    pen_in_range: bool,
    rubber_in_range: bool,
    proximity: bool,
    dirty: bool,
}

impl PenDevice {
    /// Find and grab the marker input device.
    pub fn open() -> io::Result<Self> {
        let path = find_marker_device()?;
        let cpath = std::ffi::CString::new(path.clone()).unwrap();
        let fd = unsafe { libc::open(cpath.as_ptr(), libc::O_RDONLY | libc::O_NONBLOCK) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        let grab = unsafe { libc::ioctl(fd, EVIOCGRAB, 1i32) };
        if grab != 0 {
            eprintln!(
                "riddle: warning: EVIOCGRAB failed ({}) — xochitl will also see the pen",
                io::Error::last_os_error()
            );
        }
        eprintln!("riddle: pen device {path} opened (grabbed: {})", grab == 0);
        Ok(Self {
            fd,
            raw_x: 0,
            raw_y: 0,
            pressure: 0,
            tool: Tool::Pen,
            touching: false,
            pen_in_range: false,
            rubber_in_range: false,
            proximity: false,
            dirty: false,
        })
    }

    pub fn raw_fd(&self) -> RawFd {
        self.fd
    }

    /// Drain all pending events; returns one sample per SYN_REPORT frame
    /// that changed state.
    pub fn drain(&mut self) -> Vec<PenSample> {
        let mut out = Vec::new();
        let mut buf = [0u8; EV_REC * 64];
        loop {
            let n =
                unsafe { libc::read(self.fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len()) };
            if n <= 0 {
                break;
            }
            for chunk in buf[..n as usize].chunks_exact(EV_REC) {
                let (etype, code, value) = parse(chunk);
                match (etype, code) {
                    (EV_ABS, ABS_X) => {
                        self.raw_x = value;
                        self.dirty = true;
                    }
                    (EV_ABS, ABS_Y) => {
                        self.raw_y = value;
                        self.dirty = true;
                    }
                    (EV_ABS, ABS_PRESSURE) => {
                        self.pressure = value;
                        self.dirty = true;
                    }
                    (EV_KEY, BTN_TOOL_PEN) => {
                        self.pen_in_range = value == 1;
                        if self.pen_in_range {
                            self.tool = Tool::Pen;
                        }
                        self.proximity = self.pen_in_range || self.rubber_in_range;
                        self.dirty = true;
                    }
                    (EV_KEY, BTN_TOOL_RUBBER) => {
                        self.rubber_in_range = value == 1;
                        if self.rubber_in_range {
                            self.tool = Tool::Eraser;
                        }
                        self.proximity = self.pen_in_range || self.rubber_in_range;
                        self.dirty = true;
                    }
                    (EV_KEY, BTN_TOUCH) => {
                        self.touching = value == 1;
                        self.dirty = true;
                    }
                    (EV_SYN, SYN_REPORT) => {
                        if self.dirty {
                            self.dirty = false;
                            let (x, y) = to_screen(self.raw_x, self.raw_y);
                            out.push(PenSample {
                                x,
                                y,
                                pressure: self.pressure,
                                tool: self.tool,
                                touching: self.touching,
                                proximity: self.proximity,
                            });
                        }
                    }
                    _ => {}
                }
            }
        }
        out
    }
}

impl Drop for PenDevice {
    fn drop(&mut self) {
        unsafe {
            libc::ioctl(self.fd, EVIOCGRAB, 0i32);
            libc::close(self.fd);
        }
    }
}

/// Digitizer-to-screen transform.
#[cfg(not(target_arch = "arm"))]
#[inline]
fn to_screen(raw_x: i32, raw_y: i32) -> (i32, i32) {
    (
        raw_x * (SCREEN_W as i32 - 1) / DIGI_MAX_X,
        raw_y * (SCREEN_H as i32 - 1) / DIGI_MAX_Y,
    )
}

/// rM2: swap axes (digitizer rotated 90°) and flip to portrait orientation.
/// ponytail: polarity from libremarkable's rM2 mapping; if strokes come out
/// mirrored on real hardware set RIDDLE_PEN_INVX/RIDDLE_PEN_INVY=1 to correct
/// without rebuilding.
#[cfg(target_arch = "arm")]
#[inline]
fn to_screen(raw_x: i32, raw_y: i32) -> (i32, i32) {
    use std::sync::OnceLock;
    static INV: OnceLock<(bool, bool)> = OnceLock::new();
    let (invx, invy) = *INV.get_or_init(|| {
        (
            std::env::var("RIDDLE_PEN_INVX").is_ok_and(|v| v == "1"),
            std::env::var("RIDDLE_PEN_INVY").is_ok_and(|v| v == "1"),
        )
    });
    let mut x = raw_y * (SCREEN_W as i32 - 1) / DIGI_MAX_Y;
    let mut y = (SCREEN_H as i32 - 1) - raw_x * (SCREEN_H as i32 - 1) / DIGI_MAX_X;
    if invx {
        x = (SCREEN_W as i32 - 1) - x;
    }
    if invy {
        y = (SCREEN_H as i32 - 1) - y;
    }
    (x, y)
}

fn find_marker_device() -> io::Result<String> {
    for i in 0..8 {
        let name_path = format!("/sys/class/input/event{i}/device/name");
        if let Ok(name) = std::fs::read_to_string(&name_path) {
            let name = name.to_lowercase();
            // Paper Pro: "Elan marker input"; reMarkable 2: "Wacom I2C Digitizer".
            if name.contains("marker") || name.contains("wacom") {
                return Ok(format!("/dev/input/event{i}"));
            }
        }
    }
    Err(io::Error::new(
        io::ErrorKind::NotFound,
        "no marker input device found",
    ))
}
