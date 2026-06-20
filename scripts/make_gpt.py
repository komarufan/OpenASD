#!/usr/bin/env python3
"""
make_gpt.py — Write a correct GPT (GUID Partition Table) to a raw disk image.

Creates:
  LBA 0    : Protective MBR
  LBA 1    : Primary GPT header
  LBA 2-33 : Partition entry array (128 entries × 128 bytes)
  LBA 2048 : Start of ESP partition (EFI System Partition, FAT32)
  LBA n-33 : Backup partition entry array
  LBA n-1  : Backup GPT header

Usage:
  python3 make_gpt.py <disk_image>

After running, use mtools to format the ESP:
  mformat -i <disk_image>@@1M -F -v "OPENASD" ::
"""
import sys, struct, zlib, uuid, os

# EFI System Partition type GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
# Mixed-endian UEFI format (fields 1-3 are little-endian, 4-5 are big-endian)
ESP_TYPE_GUID = bytes.fromhex('2873A12C1FF8D211BA4B00A0C93EC93B')

SECTOR = 512
GPT_HEADER_SIZE = 92
PART_ENTRY_SIZE = 128
PART_ENTRY_COUNT = 128
PART_TABLE_SECTORS = (PART_ENTRY_SIZE * PART_ENTRY_COUNT) // SECTOR  # 32

FIRST_USABLE_LBA = 2048  # 1 MiB alignment — standard for USB drives


def build_partition_table(first_lba: int, last_lba: int) -> bytes:
    part_guid = uuid.uuid4().bytes_le
    name = 'EFI System'.encode('utf-16-le').ljust(72, b'\x00')[:72]
    entry = struct.pack('<16s16sQQQ',
                       ESP_TYPE_GUID,
                       part_guid,
                       first_lba,
                       last_lba,
                       0)  # attributes
    entry += name
    assert len(entry) == PART_ENTRY_SIZE
    # Pad remaining entries with zeros
    table = entry + b'\x00' * (PART_ENTRY_SIZE * (PART_ENTRY_COUNT - 1))
    assert len(table) == PART_ENTRY_SIZE * PART_ENTRY_COUNT
    return table


def build_header(current_lba: int, backup_lba: int, part_start_lba: int,
                 first_usable: int, last_usable: int,
                 disk_guid: bytes, part_crc: int) -> bytes:
    hdr = struct.pack('<8sIIIIQQQQ16sQIII',
                     b'EFI PART',   # signature
                     0x00010000,    # revision 1.0
                     GPT_HEADER_SIZE,
                     0,             # header CRC — computed below
                     0,             # reserved
                     current_lba,
                     backup_lba,
                     first_usable,
                     last_usable,
                     disk_guid,
                     part_start_lba,
                     PART_ENTRY_COUNT,
                     PART_ENTRY_SIZE,
                     part_crc)
    assert len(hdr) == GPT_HEADER_SIZE
    crc = zlib.crc32(hdr) & 0xFFFFFFFF
    # Patch the CRC field at offset 16
    hdr = hdr[:16] + struct.pack('<I', crc) + hdr[20:]
    return hdr.ljust(SECTOR, b'\x00')


def build_protective_mbr(disk_lba: int) -> bytes:
    mbr = bytearray(SECTOR)
    mbr[510] = 0x55
    mbr[511] = 0xAA
    # Single protective partition entry at byte 446
    mbr[446] = 0x00        # not bootable
    mbr[447] = 0x00        # CHS start — head 0
    mbr[448] = 0x02        # CHS start — sector 2
    mbr[449] = 0x00        # CHS start — cylinder 0
    mbr[450] = 0xEE        # partition type: GPT protective
    mbr[451] = 0xFF        # CHS end (max)
    mbr[452] = 0xFF
    mbr[453] = 0xFF
    struct.pack_into('<I', mbr, 454, 1)                                  # start LBA
    struct.pack_into('<I', mbr, 458, min(disk_lba - 1, 0xFFFFFFFF))     # size LBA
    return bytes(mbr)


def write_gpt(filename: str) -> None:
    size = os.path.getsize(filename)
    if size % SECTOR != 0:
        sys.exit(f"Error: {filename} size {size} is not a multiple of {SECTOR}")

    disk_lba = size // SECTOR
    min_lba = FIRST_USABLE_LBA + PART_TABLE_SECTORS + 2
    if disk_lba < min_lba:
        sys.exit(f"Error: disk too small ({disk_lba} LBAs, need at least {min_lba})")

    # Backup GPT at last sector, backup partition table at last-33 sectors
    backup_header_lba = disk_lba - 1
    backup_parts_lba  = disk_lba - 1 - PART_TABLE_SECTORS  # = disk_lba - 33
    last_usable_lba   = backup_parts_lba - 1

    disk_guid = uuid.uuid4().bytes_le
    part_table = build_partition_table(FIRST_USABLE_LBA, last_usable_lba)
    part_crc   = zlib.crc32(part_table) & 0xFFFFFFFF

    primary_header = build_header(
        current_lba   = 1,
        backup_lba    = backup_header_lba,
        part_start_lba= 2,
        first_usable  = FIRST_USABLE_LBA,
        last_usable   = last_usable_lba,
        disk_guid     = disk_guid,
        part_crc      = part_crc)

    backup_header = build_header(
        current_lba   = backup_header_lba,
        backup_lba    = 1,
        part_start_lba= backup_parts_lba,
        first_usable  = FIRST_USABLE_LBA,
        last_usable   = last_usable_lba,
        disk_guid     = disk_guid,
        part_crc      = part_crc)

    protective_mbr = build_protective_mbr(disk_lba)

    with open(filename, 'r+b') as f:
        f.seek(0);                      f.write(protective_mbr)         # LBA 0
        f.seek(1 * SECTOR);             f.write(primary_header)         # LBA 1
        f.seek(2 * SECTOR);             f.write(part_table)             # LBA 2-33
        f.seek(backup_parts_lba * SECTOR); f.write(part_table)          # LBA n-33
        f.seek(backup_header_lba * SECTOR); f.write(backup_header)      # LBA n-1

    print(f"GPT written to {filename}")
    print(f"  disk:         {disk_lba} sectors ({size // 1024 // 1024} MiB)")
    print(f"  ESP:          LBA {FIRST_USABLE_LBA}–{last_usable_lba}")
    print(f"  Use mtools:   mformat -i {filename}@@1M -F -v OPENASD ::")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(f"Usage: {sys.argv[0]} <disk_image>")
    write_gpt(sys.argv[1])
