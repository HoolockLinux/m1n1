// SPDX-License-Identifier: MIT
use crate::println;
use alloc::boxed::Box;
use core::cmp::min;
use core::ffi::c_void;
use fatfs::SeekFrom;

extern "C" {
    fn main_storage_read(lba: u64, buffer: *mut c_void) -> bool;
}

const SECTOR_SIZE: usize = 4096;

pub type Error = ();

#[repr(C, align(4096))]
struct SectorBuffer([u8; SECTOR_SIZE]);

fn alloc_sector_buf() -> Box<SectorBuffer> {
    let p: Box<SectorBuffer> = unsafe { Box::new_zeroed().assume_init() };
    debug_assert_eq!(0, p.0.as_ptr().align_offset(4096));
    p
}

pub struct MainStorage {
    offset: u64,
    lba: Option<u64>,
    buf: Box<SectorBuffer>,
    pos: u64,
}

impl MainStorage {
    pub fn new(offset: u64) -> MainStorage {
        MainStorage {
            offset: offset,
            lba: None,
            buf: alloc_sector_buf(),
            pos: 0,
        }
    }
}

impl fatfs::IoBase for MainStorage {
    type Error = Error;
}

impl fatfs::Read for MainStorage {
    fn read(&mut self, mut buf: &mut [u8]) -> Result<usize, Self::Error> {
        let mut read = 0;

        while !buf.is_empty() {
            let lba = self.pos / SECTOR_SIZE as u64;
            let off = self.pos as usize % SECTOR_SIZE;

            if Some(lba) != self.lba {
                self.lba = Some(lba);
                let lba = lba + self.offset;
                if !unsafe { main_storage_read(lba, self.buf.0.as_mut_ptr() as *mut c_void) } {
                    println!("main_storage_read({}) failed", lba);
                    return Err(());
                }
            }
            let copy_len = min(SECTOR_SIZE - off, buf.len());
            buf[..copy_len].copy_from_slice(&self.buf.0[off..off + copy_len]);
            buf = &mut buf[copy_len..];
            read += copy_len;
            self.pos += copy_len as u64;
        }
        Ok(read)
    }
}

impl fatfs::Write for MainStorage {
    fn write(&mut self, _buf: &[u8]) -> Result<usize, Self::Error> {
        Err(())
    }
    fn flush(&mut self) -> Result<(), Self::Error> {
        Err(())
    }
}

impl fatfs::Seek for MainStorage {
    fn seek(&mut self, from: SeekFrom) -> Result<u64, Self::Error> {
        self.pos = match from {
            SeekFrom::Start(n) => n,
            SeekFrom::End(_n) => panic!("SeekFrom::End not supported"),
            SeekFrom::Current(n) => self.pos.checked_add_signed(n).ok_or(())?,
        };
        Ok(self.pos)
    }
}
