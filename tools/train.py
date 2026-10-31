"""Builds the board's Trained model from your photo dataset.

Every photo in dataset/<stone>/ is sent to the board, which measures it the same
way it measures the live camera and stores the result. The Live demo samples
are not touched. By default the old trained model is replaced.

    python train.py                        # everything in ./dataset
    python train.py --max 60               # at most 60 photos per stone
    python train.py --only amethyst empty  # just these stones (replaces the rest!)
    python train.py --add                  # add to the trained model instead of replacing it
    python train.py --host 192.168.1.42    # board on your home WiFi

Standard library only, so it runs on any Python 3.
"""

import argparse
import json
import pathlib
import sys
import urllib.error
import urllib.parse
import urllib.request

BOARD_LIMIT = 1000


def call(host, path, body=None):
    req = urllib.request.Request(f"http://{host}{path}", data=body, method="POST")
    if body is not None:
        req.add_header("Content-Type", "image/jpeg")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            msg = json.loads(e.read()).get("error", e.reason)
        except ValueError:
            msg = e.reason
        raise RuntimeError(f"{path.split('?')[0]}: {msg}") from None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dataset", default="dataset", help="folder with one subfolder per stone (default ./dataset)")
    p.add_argument("--host", default="192.168.4.1", help="board address (default: the GemCam hotspot)")
    p.add_argument("--max", type=int, default=0, help="use at most this many photos per stone")
    p.add_argument("--only", nargs="+", metavar="STONE", help="only these stone folders")
    p.add_argument("--add", action="store_true", help="add to the trained model instead of replacing it")
    p.add_argument("--keep-mode", action="store_true", help="don't switch Identify to the trained model")
    args = p.parse_args()

    root = pathlib.Path(args.dataset)
    folders = sorted(f for f in root.iterdir() if f.is_dir()) if root.is_dir() else []
    if args.only:
        folders = [f for f in folders if f.name in args.only]
    plan = []
    for f in folders:
        photos = sorted(f.glob("*.jpg")) + sorted(f.glob("*.jpeg"))
        if args.max:
            # Spread the picks across the folder rather than taking the first N
            step = max(1, len(photos) / args.max)
            photos = [photos[int(i * step)] for i in range(min(args.max, len(photos)))]
        if photos:
            plan.append((f.name, photos))
    if not plan:
        sys.exit(f"No photos found in {root}/<stone>/. Collect some with collect.py first.")

    total = sum(len(ph) for _, ph in plan)
    for name, photos in plan:
        print(f"  {name:<20} {len(photos):>4} photos")
    if total > BOARD_LIMIT:
        sys.exit(f"{total} photos is more than the board holds ({BOARD_LIMIT}). Use --max to cap each stone.")

    try:
        if not args.add:
            call(args.host, "/train/clear")
            print("Cleared the old trained model.")
        done = 0
        for name, photos in plan:
            label = urllib.parse.quote(name)
            for photo in photos:
                call(args.host, f"/train/add?label={label}", photo.read_bytes())
                done += 1
                print(f"\r  sending {done}/{total}  ({name})" + " " * 20, end="", flush=True)
        print()
        result = call(args.host, "/train/commit")
        if not args.keep_mode:
            result = call(args.host, "/mode?set=trained")
    except (RuntimeError, OSError) as e:
        print()
        sys.exit(f"Stopped: {e}\nIs this computer on the board's WiFi ({args.host})?")

    trained = result["trained"]
    print(f"Done: the trained model now knows {len(trained['labels'])} stones from {trained['samples']} photos.")
    if not args.keep_mode:
        print("Identify is now using the trained model.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped part-way. Run it again to finish - the trained model is incomplete until you do.")
