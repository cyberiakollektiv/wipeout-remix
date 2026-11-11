# TODO: Make this automatically look and extract for BIN files matching Redump MD5 checksum
import sys

def extract_iso(bin_path, out_path):
    with open(bin_path, 'rb') as f:
        data = f.read()
    
    sector_size = 2352
    with open(out_path, 'wb') as out:
        for i in range(0, len(data) - sector_size, sector_size):
            out.write(data[i + 24 : i + 24 + 2048])

if __name__ == '__main__':
    extract_iso(sys.argv[1], sys.argv[2])
