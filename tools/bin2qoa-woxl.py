import os
import shutil
import subprocess
from pathlib import Path

def check_dependencies():
    missing = []
    
    if not shutil.which("ffmpeg"):
        missing.append("ffmpeg")
        
    if not shutil.which("qoaconv") and not Path("./qoaconv").exists():
        missing.append("qoaconv")
        
    if missing:
        print(f"Error: Required tool(s) not found: {', '.join(missing)}")
        print("Please ensure they are installed and available in your PATH or current directory.")
        return False
    return True

def main():
    if not check_dependencies():
        return

    print("Starting Wipeout XL soundtrack conversion")

    output_index = 1

    for i in range(2, 13):
        bin_name = f"WipEout XL (Track {i:02d}).bin"
        bin_path = Path(bin_name)

        if bin_path.exists():
            wav_path = Path(f"track_{i:02d}.wav")
            qoa_path = Path(f"track{output_index:02d}.qoa")

            print(f"Processing {bin_name} -> {qoa_path.name}...")

            ffmpeg_cmd = [
                "ffmpeg", "-f", "s16le", "-ar", "44100", "-ac", "2",
                "-i", str(bin_path), "-y", str(wav_path)
            ]
            
            try:
                subprocess.run(
                    ffmpeg_cmd, 
                    check=True, 
                    stdout=subprocess.DEVNULL, 
                    stderr=subprocess.DEVNULL
                )
            except subprocess.CalledProcessError:
                print(f"Error: Failed to process {bin_name} with ffmpeg.")
                continue

            qoaconv_executable = "qoaconv" if shutil.which("qoaconv") else "./qoaconv"
            qoaconv_cmd = [qoaconv_executable, str(wav_path), str(qoa_path)]
            
            try:
                subprocess.run(qoaconv_cmd, check=True)
                print(f"Successfully created {qoa_path.name}")
                output_index += 1
            except subprocess.CalledProcessError:
                print(f"Error: Failed to encode {wav_path.name} with qoaconv.")
            finally:
                if wav_path.exists():
                    wav_path.unlink()
        else:
            print(f"Warning: {bin_name} not found, skipping.")

    print("\nAll available soundtrack tracks have been successfully converted!")

if __name__ == "__main__":
    main()
