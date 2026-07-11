// SPDX-License-Identifier: MIT
#![deny(unsafe_op_in_unsafe_fn)]

use crate::c_size_t;
use crate::gpt;
use crate::println;
use crate::storage;
use alloc::vec::Vec;
use core::ffi::{c_char, c_int, c_void, CStr};
use fatfs::{FileSystem, FsOptions, Read, Seek, SeekFrom};
use uuid::Uuid;

#[derive(Debug)]
pub enum Error {
    FATError(fatfs::Error<storage::Error>),
    GPTError(gpt::Error<storage::Error>),
    BadArgs,
    PartitionNotFound,
    Unknown,
}

impl From<fatfs::Error<storage::Error>> for Error {
    fn from(err: fatfs::Error<storage::Error>) -> Error {
        Error::FATError(err)
    }
}

impl From<gpt::Error<storage::Error>> for Error {
    fn from(err: gpt::Error<storage::Error>) -> Error {
        Error::GPTError(err)
    }
}

fn load_image(spec: &str) -> Result<Vec<u8>, Error> {
    println!("Chainloading {}", spec);

    let mut args = spec.split(';');

    let uuid = Uuid::parse_str(args.next().ok_or(Error::BadArgs)?).or(Err(Error::BadArgs))?;
    let path = args.next().ok_or(Error::BadArgs)?;

    let part = {
        let storage = storage::MainStorage::new(0);
        let mut pt = gpt::GPT::new(storage)?;

        //println!("Partitions:");
        //pt.dump();

        println!("Searching for partition UUID: {}", uuid);
        pt.find_by_partuuid(uuid)?.ok_or(Error::PartitionNotFound)?
    };

    let offset = part.get_starting_lba();

    println!("Partition offset: {}", offset);

    let storage = storage::MainStorage::new(offset);
    let opts = FsOptions::new().update_accessed_date(false);

    let fs = FileSystem::new(storage, opts)?;
    let mut file = fs.root_dir().open_file(path)?;

    let size = file.seek(SeekFrom::End(0))? as usize;
    file.seek(SeekFrom::Start(0))?;

    println!("File size: {}", size);

    let mut buf: Vec<u8> = vec![0; size];
    let mut slice = &mut buf[..];
    while !slice.is_empty() {
        let read = file.read(slice)?;
        slice = &mut slice[read..];
    }
    println!("File read successfully");

    Ok(buf)
}

#[no_mangle]
pub unsafe extern "C" fn rust_load_image(
    raw_spec: *const c_char,
    image: *mut *mut c_void,
    size: *mut c_size_t,
) -> c_int {
    let spec = unsafe { CStr::from_ptr(raw_spec).to_str().unwrap() };

    match load_image(spec) {
        Ok(buf) => {
            unsafe {
                *size = buf.len();
                *image = buf.leak().as_mut_ptr() as *mut c_void;
            }
            0
        }
        Err(err) => {
            println!("Chainload failed: {:?}", err);
            -1
        }
    }
}
