#!/usr/bin/env python3
"""Numerical parity check: faceforge core vs. the InsightFace Python reference.

Requires the insightface + onnxruntime + opencv Python packages in the active
environment. The native core is compared stage by stage on CPU execution so
that floating point differences stay small.

    python3 scripts/verify_parity.py --models models --core build/libfaceforge.so
"""
import argparse
import ctypes
import sys
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort

try:
    from insightface.model_zoo.scrfd import SCRFD
    from insightface.model_zoo.arcface_onnx import ArcFaceONNX
    from insightface.model_zoo.inswapper import INSwapper
    from insightface.app.common import Face
except ImportError as exc:
    sys.exit(f"missing insightface reference: {exc}")


class Options(ctypes.Structure):
    _fields_ = [
        ("provider", ctypes.c_int32),
        ("device_id", ctypes.c_int32),
        ("intra_threads", ctypes.c_int32),
        ("sequential", ctypes.c_int32),
        ("det_size", ctypes.c_int32),
        ("det_thresh", ctypes.c_float),
        ("nms_thresh", ctypes.c_float),
        ("max_faces", ctypes.c_int32),
        ("enhance", ctypes.c_int32),
        ("esrgan_tile", ctypes.c_int32),
        ("gpu_mem_limit", ctypes.c_int64),
        ("restore_mouth", ctypes.c_int32),
        ("pad", ctypes.c_int32),
    ]


class FaceC(ctypes.Structure):
    _fields_ = [
        ("bbox", ctypes.c_float * 4),
        ("kps", ctypes.c_float * 10),
        ("det_score", ctypes.c_float),
        ("face_id", ctypes.c_int32),
    ]


def bind(path):
    lib = ctypes.CDLL(str(path))
    lib.ff_options_default.argtypes = [ctypes.POINTER(Options)]
    lib.ff_engine_create.argtypes = [
        ctypes.c_char_p, ctypes.POINTER(Options), ctypes.c_char_p, ctypes.c_int32]
    lib.ff_engine_create.restype = ctypes.c_void_p
    lib.ff_engine_destroy.argtypes = [ctypes.c_void_p]
    lib.ff_engine_provider.argtypes = [ctypes.c_void_p]
    lib.ff_engine_provider.restype = ctypes.c_char_p
    lib.ff_engine_last_error.argtypes = [ctypes.c_void_p]
    lib.ff_engine_last_error.restype = ctypes.c_char_p
    lib.ff_detect.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int32, ctypes.c_int32,
        ctypes.c_int32, ctypes.POINTER(FaceC), ctypes.c_int32,
        ctypes.POINTER(ctypes.c_int32)]
    lib.ff_detect.restype = ctypes.c_int32
    lib.ff_embed.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int32, ctypes.c_int32,
        ctypes.c_int32, ctypes.POINTER(FaceC), ctypes.POINTER(ctypes.c_float)]
    lib.ff_embed.restype = ctypes.c_int32
    lib.ff_swap_face.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int32, ctypes.c_int32,
        ctypes.c_int32, ctypes.POINTER(ctypes.c_float), ctypes.POINTER(FaceC),
        ctypes.c_int32, ctypes.c_void_p]
    lib.ff_swap_face.restype = ctypes.c_int32
    return lib


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", default="models")
    ap.add_argument("--core", default="build/libfaceforge.so")
    ap.add_argument("--target", default="assets/target_t1.jpg")
    ap.add_argument("--source", default="assets/source_lena.jpg")
    args = ap.parse_args()

    target = cv2.imread(args.target)
    source = cv2.imread(args.source)
    if target is None or source is None:
        sys.exit("cannot read test images")

    lib = bind(args.core)
    opt = Options()
    lib.ff_options_default(ctypes.byref(opt))
    opt.provider = 2  # FF_PROVIDER_CPU
    err = ctypes.create_string_buffer(1024)
    engine = lib.ff_engine_create(args.models.encode(), ctypes.byref(opt), err, 1024)
    if not engine:
        sys.exit(f"engine create failed: {err.value.decode()}")
    print(f"faceforge provider: {lib.ff_engine_provider(engine).decode()}")

    # --- detection ---------------------------------------------------------
    ref_det = SCRFD(str(Path(args.models) / "det_10g.onnx"))
    ref_det.prepare(-1, det_thresh=0.5, nms_thresh=0.4)
    ref_boxes, ref_kpss = ref_det.detect(target, input_size=(640, 640))

    faces = (FaceC * 16)()
    count = ctypes.c_int32(0)
    rc = lib.ff_detect(engine, target.ctypes.data, target.shape[1], target.shape[0],
                       target.strides[0], faces, 16, ctypes.byref(count))
    if rc != 0:
        sys.exit(f"ff_detect failed: {lib.ff_engine_last_error(engine).decode()}")
    print(f"detections: native={count.value} reference={len(ref_boxes)}")

    n = min(count.value, len(ref_boxes))
    box_err = np.max(np.abs(
        np.array([faces[i].bbox for i in range(n)]) - ref_boxes[:n, :4])) if n else 0
    kps_err = np.max(np.abs(
        np.array([faces[i].kps for i in range(n)]).reshape(n, 5, 2) -
        ref_kpss[:n])) if n else 0
    print(f"  max bbox error: {box_err:.4f} px")
    print(f"  max kps  error: {kps_err:.4f} px")
    assert count.value == len(ref_boxes), "detection count differs"
    assert box_err < 1.0 and kps_err < 1.0, "detection geometry differs"

    # --- arcface embedding -------------------------------------------------
    ref_arc = ArcFaceONNX(str(Path(args.models) / "w600k_r50.onnx"),
                          session=ort.InferenceSession(
                              str(Path(args.models) / "w600k_r50.onnx"),
                              providers=["CPUExecutionProvider"]))
    ref_arc.prepare(-1)

    src_faces = (FaceC * 16)()
    src_count = ctypes.c_int32(0)
    rc = lib.ff_detect(engine, source.ctypes.data, source.shape[1], source.shape[0],
                       source.strides[0], src_faces, 16, ctypes.byref(src_count))
    assert rc == 0 and src_count.value > 0
    sf = src_faces[0]

    native_emb = np.zeros(512, dtype=np.float32)
    rc = lib.ff_embed(engine, source.ctypes.data, source.shape[1], source.shape[0],
                      source.strides[0], ctypes.byref(sf),
                      native_emb.ctypes.data_as(ctypes.POINTER(ctypes.c_float)))
    if rc != 0:
        sys.exit(f"ff_embed failed: {lib.ff_engine_last_error(engine).decode()}")

    ref_face = Face(kps=np.array(sf.kps, dtype=np.float32).reshape(5, 2),
                    bbox=np.array(sf.bbox, dtype=np.float32), det_score=float(sf.det_score))
    ref_arc.get(source, ref_face)
    ref_emb = ref_face.normed_embedding
    cos = float(np.dot(native_emb, ref_emb))
    print(f"embedding cosine similarity: {cos:.7f}")
    assert cos > 0.9999, "embeddings differ"

    # --- inswapper + paste-back -------------------------------------------
    ref_swap = INSwapper(
        str(Path(args.models) / "inswapper_128.onnx"),
        session=ort.InferenceSession(
            str(Path(args.models) / "inswapper_128.onnx"),
            providers=["CPUExecutionProvider"]))
    target_face = Face(kps=np.array(faces[0].kps, dtype=np.float32).reshape(5, 2),
                       bbox=np.array(faces[0].bbox, dtype=np.float32),
                       det_score=float(faces[0].det_score))
    ref_out = ref_swap.get(target, target_face, ref_face, paste_back=True)

    native_out = np.zeros_like(target)
    rc = lib.ff_swap_face(engine, target.ctypes.data, target.shape[1], target.shape[0],
                          target.strides[0],
                          native_emb.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                          ctypes.byref(faces[0]), 1, native_out.ctypes.data)
    if rc != 0:
        sys.exit(f"ff_swap_face failed: {lib.ff_engine_last_error(engine).decode()}")

    diff = np.abs(native_out.astype(np.float32) - ref_out.astype(np.float32))
    mean_diff = float(diff.mean())
    max_diff = float(diff.max())
    p99 = float(np.percentile(diff, 99))
    print(f"swap difference: mean={mean_diff:.4f}  p99={p99:.3f}  max={max_diff:.1f}")

    Path("bench/results").mkdir(parents=True, exist_ok=True)
    cv2.imwrite("bench/results/parity_reference.png", ref_out)
    cv2.imwrite("bench/results/parity_native.png", native_out)
    cv2.imwrite("bench/results/parity_diff.png",
                np.clip(diff * 8, 0, 255).astype(np.uint8))

    lib.ff_engine_destroy(engine)
    if mean_diff > 1.0 or p99 > 6.0:
        print("PARITY: FAIL")
        return 1
    print("PARITY: PASS (within 8-bit quantization noise)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
