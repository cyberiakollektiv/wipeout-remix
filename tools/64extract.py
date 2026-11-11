import os
import sys
import hashlib
import struct

def check_sha1(file_path, expected_sha1="54e4795aba4fc326f79d0703ffdb51a212dd4b5c"):
    sha1 = hashlib.sha1()
    with open(file_path, "rb") as f:
        while chunk := f.read(8192):
            sha1.update(chunk)
    file_hash = sha1.hexdigest()
    return file_hash == expected_sha1

def extract_entries(file_path, start_address, out_folder):
    with open(file_path, "rb") as f:
        f.seek(start_address)
        entries_count = struct.unpack("<H", f.read(2))[0]
        
        # get entries from headers
        entry_list = []
        for _ in range(entries_count):
            raw_name = f.read(16)
            name = raw_name.split(b"\x00", 1)[0].decode("utf-8")
            sizeCmp, sizeUncmp = struct.unpack("<II", f.read(8))
            padding = f.read(1)
            entry_list.append({"name": name, "size": sizeUncmp})
        
        # extract contents next
        for entry in entry_list:
            name = entry["name"]
            sizeUncmp = entry["size"]
            print(f"Extracting: {name} (size: {sizeUncmp})")
            file_data = f.read(sizeUncmp)
            output_path = os.path.join(out_folder, name)
            with open(output_path, "wb") as out_file:
                out_file.write(file_data)
            
    print("Extraction complete.")


rom = "Wipeout 64 (USA).z64"  # ROM (little-endian dump)
if check_sha1(rom):
    trackno = 1
    for offset in [4152976, 4515360, 4976240, 5548976, 6007760, 6506736, 6945968]:
        track_folder = os.path.join("WIPEOUT64", f"TRACK{trackno:02}")
        os.makedirs(track_folder, exist_ok=True)
        extract_entries(rom, offset, track_folder)
        trackno += 1
