"""Saves training photos from Gemstone Cam into dataset/<label>/.

Turn the stone a little between shots so the photos aren't all identical.

    python collect.py amethyst                 # 50 photos, one every 0.7 s
    python collect.py rose_quartz -n 80 -i 1
    python collect.py empty --host 192.168.1.42

Standard library only, so it runs on any Python 3.
"""

import argparse
import pathlib
import time
import urllib.request


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("label", help="stone name; becomes the folder name")
    p.add_argument("-n", "--count", type=int, default=50, help="photos to take (default 50)")
    p.add_argument("-i", "--interval", type=float, default=0.7, help="seconds between photos (default 0.7)")
    p.add_argument("--host", default="192.168.4.1", help="board address (default: the GemCam hotspot)")
    p.add_argument("--out", default="dataset", help="output folder (default ./dataset)")
    args = p.parse_args()

    label = "".join(c if c.isalnum() or c in "-_" else "_" for c in args.label.strip().lower())
    folder = pathlib.Path(args.out) / label
    folder.mkdir(parents=True, exist_ok=True)

    # Continue numbering after any photos already in the folder
    existing = [int(f.stem.rsplit("_", 1)[-1]) for f in folder.glob(f"{label}_*.jpg")
                if f.stem.rsplit("_", 1)[-1].isdigit()]
    n = max(existing, default=0)

    url = f"http://{args.host}/capture"
    print(f"Saving {args.count} photos of '{label}' to {folder}/ - Ctrl+C to stop early")
    for shot in range(1, args.count + 1):
        try:
            with urllib.request.urlopen(url, timeout=5) as r:
                data = r.read()
        except OSError as e:
            print(f"  couldn't reach {url}: {e}")
            time.sleep(1)
            continue
        n += 1
        (folder / f"{label}_{n:04d}.jpg").write_bytes(data)
        print(f"  {shot}/{args.count}  {label}_{n:04d}.jpg  ({len(data) // 1024} KB)")
        time.sleep(args.interval)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped.")
