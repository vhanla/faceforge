#!/usr/bin/env python3
"""Extract the 512x512 `emap` initializer from inswapper_128.onnx.

InsightFace applies this matrix to the ArcFace embedding (and re-normalizes)
before feeding it to the swap model. ONNX Runtime does not expose graph
initializers through its C/C++ API, so the native core reads a raw float32
file produced here.

Requires: python3 -m pip install onnx numpy
"""
import argparse
import sys
from pathlib import Path

try:
    import numpy as np
    import onnx
    from onnx import numpy_helper
except ImportError as exc:  # pragma: no cover
    sys.exit(f"missing dependency: {exc}\n  pip install onnx numpy")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("model", nargs="?", default="models/inswapper_128.onnx")
    ap.add_argument("-o", "--output", default="models/emap.bin")
    args = ap.parse_args()

    model_path = Path(args.model)
    if not model_path.is_file():
        sys.exit(f"model not found: {model_path}")

    model = onnx.load(str(model_path), load_external_data=True)
    inits = model.graph.initializer
    if not inits:
        sys.exit("model has no initializers")

    emap = None
    # InsightFace uses the last initializer. Prefer a name match, fall back to
    # the last 512x512 tensor for robustness across exports.
    last = inits[-1]
    arr = numpy_helper.to_array(last)
    if arr.shape == (512, 512):
        emap = arr
    else:
        for init in reversed(inits):
            if list(init.dims) == [512, 512]:
                emap = numpy_helper.to_array(init)
                break
    if emap is None:
        sys.exit("no 512x512 initializer found; not an inswapper model?")

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    emap.astype("<f4").tofile(out)
    print(f"wrote {out} ({emap.shape[0]}x{emap.shape[1]} float32, "
          f"{out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
