"""Runs photos through the trained model ON THE BOARD, to check it agrees with Edge Impulse.

Each photo is uploaded to the board's /classify page, which decodes, crops, resizes and
classifies it exactly like a camera frame. If the board gets the same answers as Studio
for the same photos, the firmware is fine and any trouble is in what the camera sees.

    python classify.py dataset/jasper/jasper_0012.jpg      # one photo, all scores
    python classify.py dataset                             # every stone folder + confusion matrix
    python classify.py dataset/jasper dataset/quartz -n 20 # 20 photos from each folder
    python classify.py dataset --save-input seen           # also save what the model saw
    python classify.py dataset --host 192.168.1.42

A folder's name is the expected answer ("lapis_lazuli" matches the label "lapis lazuli").
--save-input writes the 96x96 image the model actually received, as a .bmp, so you can
check colors and cropping by eye.

Standard library only, so it runs on any Python 3.
"""

import argparse
import json
import pathlib
import random
import sys
import urllib.error
import urllib.request

IMAGE_TYPES = {".jpg", ".jpeg"}


def norm(label):
    return label.strip().lower().replace("_", " ").replace("-", " ")


def post(host, data, view_input=False):
    url = f"http://{host}/classify" + ("?view=input" if view_input else "")
    req = urllib.request.Request(url, data=data, method="POST",
                                 headers={"Content-Type": "image/jpeg"})
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors="replace")
        try:
            body = json.loads(body).get("error", body)
        except ValueError:
            pass
        raise RuntimeError(f"board said {e.code}: {body}") from None


def classify(host, path, save_dir):
    data = path.read_bytes()
    result = json.loads(post(host, data))
    if save_dir:
        out = save_dir / path.parent.name / (path.stem + ".bmp")
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(post(host, data, view_input=True))
    return result


def collect(paths, per_folder):
    """Returns [(expected_label_or_None, file), ...]"""
    jobs = []
    for p in map(pathlib.Path, paths):
        if p.is_file():
            jobs.append((norm(p.parent.name), p))
            continue
        if not p.is_dir():
            sys.exit(f"Not found: {p}")
        subdirs = sorted(d for d in p.iterdir() if d.is_dir())
        # A dataset folder holds one folder per stone; a stone folder holds the photos
        folders = subdirs if subdirs else [p]
        for d in folders:
            files = sorted(f for f in d.iterdir() if f.suffix.lower() in IMAGE_TYPES)
            if per_folder and len(files) > per_folder:
                files = sorted(random.Random(0).sample(files, per_folder))
            jobs += [(norm(d.name), f) for f in files]
    return jobs


def print_matrix(rows, labels):
    names = sorted(set(labels) | {e for e, _ in rows})
    width = max(len(n) for n in names) + 2
    print("\nConfusion matrix (rows = folder, columns = board's answer)")
    print(" " * width + "".join(n[:10].rjust(11) for n in names))
    for exp in names:
        got = [p for e, p in rows if e == exp]
        if not got:
            continue
        print(exp.ljust(width) + "".join(str(got.count(n)).rjust(11) for n in names))
    right = sum(e == p for e, p in rows)
    print(f"\nAccuracy: {right}/{len(rows)} = {100 * right / len(rows):.1f}%")
    unknown = sorted({e for e, _ in rows} - set(labels))
    if unknown:
        print(f"Folders with no matching label (all counted wrong): {', '.join(unknown)}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("paths", nargs="+", help="photos, stone folders, or a dataset folder")
    p.add_argument("-n", "--per-folder", type=int, default=0,
                   help="test only this many random photos per folder (default: all)")
    p.add_argument("--host", default="192.168.4.1", help="board address (default: the GemCam hotspot)")
    p.add_argument("--save-input", metavar="DIR", help="save the 96x96 image the model saw, per photo")
    args = p.parse_args()

    jobs = collect(args.paths, args.per_folder)
    if not jobs:
        sys.exit("No .jpg photos found")
    save_dir = pathlib.Path(args.save_input) if args.save_input else None

    if len(jobs) == 1:
        exp, f = jobs[0]
        r = classify(args.host, f, save_dir)
        print(f"{f}: {r['label']} ({r['confidence'] * 100:.1f}%)")
        for s in r["scores"]:
            print(f"  {s['label']:<16}{s['score'] * 100:6.1f}%")
        return

    rows, labels = [], set()
    print(f"Classifying {len(jobs)} photos on {args.host} - Ctrl+C to stop early")
    try:
        for i, (exp, f) in enumerate(jobs, 1):
            try:
                r = classify(args.host, f, save_dir)
            except (RuntimeError, OSError) as e:
                print(f"  {f.name}: {e}")
                continue
            labels.update(norm(s["label"]) for s in r["scores"])
            got = norm(r["label"])
            rows.append((exp, got))
            mark = "  " if got == exp else "XX"
            print(f"{mark} {i}/{len(jobs)}  {f.parent.name}/{f.name}: "
                  f"{r['label']} {r['confidence'] * 100:.0f}%")
    except KeyboardInterrupt:
        print("\nStopped early.")
    if rows:
        print_matrix(rows, labels)


if __name__ == "__main__":
    main()
