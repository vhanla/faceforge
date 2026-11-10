#include "internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ff {

namespace {

struct Candidate {
    float x1, y1, x2, y2;
    float score;
    float kps[10];
};

float iou(const Candidate& a, const Candidate& b) {
    const float xx1 = std::max(a.x1, b.x1);
    const float yy1 = std::max(a.y1, b.y1);
    const float xx2 = std::min(a.x2, b.x2);
    const float yy2 = std::min(a.y2, b.y2);
    const float w = std::max(0.0f, xx2 - xx1 + 1.0f);
    const float h = std::max(0.0f, yy2 - yy1 + 1.0f);
    const float inter = w * h;
    const float aa = (a.x2 - a.x1 + 1.0f) * (a.y2 - a.y1 + 1.0f);
    const float bb = (b.x2 - b.x1 + 1.0f) * (b.y2 - b.y1 + 1.0f);
    return inter / std::max(1e-6f, aa + bb - inter);
}

std::vector<Candidate> nms(std::vector<Candidate> in, float thresh) {
    std::sort(in.begin(), in.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    std::vector<Candidate> keep;
    std::vector<char> removed(in.size(), 0);
    for (size_t i = 0; i < in.size(); ++i) {
        if (removed[i]) continue;
        keep.push_back(in[i]);
        for (size_t j = i + 1; j < in.size(); ++j) {
            if (!removed[j] && iou(in[i], in[j]) > thresh) removed[j] = 1;
        }
    }
    return keep;
}

Ort::Value tensor(ModelSession* s, std::vector<float>& data,
                  std::initializer_list<int64_t> shape) {
    std::vector<int64_t> dims(shape);
    return Ort::Value::CreateTensor<float>(s->mem_info(), data.data(), data.size(),
                                           dims.data(), dims.size());
}

const float kFfhq512[10] = {0.37691676f, 0.46864664f, 0.62285697f, 0.46912813f,
                            0.50123859f, 0.61331904f, 0.39308822f, 0.72541100f,
                            0.61150205f, 0.72490465f};

// Soft mask paste used by the enhancers: warps a white mask back with the same
// affine matrix, feathers it and alpha-blends the patch.
void paste_soft(cv::Mat& canvas, const cv::Mat& patch, const cv::Mat& matrix,
                float blend) {
    cv::Mat inv;
    cv::invertAffineTransform(matrix, inv);

    cv::Mat warped_patch, warped_mask;
    cv::warpAffine(patch, warped_patch, inv, canvas.size(), cv::INTER_LINEAR,
                   cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    cv::Mat white(patch.rows, patch.cols, CV_32F, cv::Scalar(255));
    cv::warpAffine(white, warped_mask, inv, canvas.size(), cv::INTER_LINEAR,
                   cv::BORDER_CONSTANT, cv::Scalar(0));

    warped_mask.setTo(255.0f, warped_mask > 20.0f);
    double mn, mx;
    cv::minMaxLoc(warped_mask, &mn, &mx);
    if (mx < 1.0) return;

    int minx = canvas.cols, miny = canvas.rows, maxx = 0, maxy = 0;
    for (int y = 0; y < warped_mask.rows; ++y) {
        const float* p = warped_mask.ptr<float>(y);
        for (int x = 0; x < warped_mask.cols; ++x) {
            if (p[x] == 255.0f) {
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
                miny = std::min(miny, y);
                maxy = std::max(maxy, y);
            }
        }
    }
    if (maxx <= minx || maxy <= miny) return;

    const int mask_size = static_cast<int>(
        std::sqrt(static_cast<double>(maxy - miny) * static_cast<double>(maxx - minx)));
    int k = std::max(mask_size / 10, 10);
    cv::Mat kernel = cv::Mat::ones(k, k, CV_8U);
    cv::erode(warped_mask, warped_mask, kernel);

    k = std::max(mask_size / 20, 5);
    const int g = 2 * k + 1;
    cv::GaussianBlur(warped_mask, warped_mask, cv::Size(g, g), 0);

    warped_mask /= 255.0f;
    cv::Mat mask3;
    cv::cvtColor(warped_mask, mask3, cv::COLOR_GRAY2BGR);
    cv::Mat canvasf, patchf;
    canvas.convertTo(canvasf, CV_32FC3);
    warped_patch.convertTo(patchf, CV_32FC3);

    cv::Mat merged = mask3.mul(patchf) +
                     (cv::Scalar::all(1.0) - mask3).mul(canvasf);
    if (blend < 0.999f) {
        merged = merged * blend + canvasf * (1.0f - blend);
    }
    merged.convertTo(canvas, CV_8UC3);
}

// Blends the original mouth region of the aligned original crop back over a
// swapped patch. Keeps the real lip texture/teeth instead of the model's
// hallucinated mouth. Coordinates come from the five landmarks: the two mouth
// corners (indices 3,4) and the nose tip (index 2).
void restore_mouth(cv::Mat& patch, const cv::Mat& aligned, const cv::Mat& matrix,
                   const float kps[10]) {
    auto tf = [&](float x, float y, double& ox, double& oy) {
        ox = matrix.at<double>(0, 0) * x + matrix.at<double>(0, 1) * y +
             matrix.at<double>(0, 2);
        oy = matrix.at<double>(1, 0) * x + matrix.at<double>(1, 1) * y +
             matrix.at<double>(1, 2);
    };
    double lx, ly, rx, ry, nx, ny;
    tf(kps[6], kps[7], lx, ly); // left mouth corner
    tf(kps[8], kps[9], rx, ry); // right mouth corner
    tf(kps[4], kps[5], nx, ny); // nose tip

    const double cx = (lx + rx) * 0.5;
    const double cy = (ly + ry) * 0.5;
    double w = std::hypot(rx - lx, ry - ly);
    if (w < 4.0) w = 0.24 * patch.cols;
    const double nose_dist = std::hypot(cx - nx, cy - ny);
    const double h = std::min(w * 0.62, std::max(nose_dist * 0.75, w * 0.35));
    const double ax = std::max(3.0, w * 0.80);
    const double ay = std::max(2.5, h * 0.62);
    const double center_y = cy + h * 0.18;

    cv::Mat mask(patch.rows, patch.cols, CV_32F, cv::Scalar(0));
    cv::ellipse(mask, cv::Point2d(cx, center_y), cv::Size2d(ax, ay), 0.0, 0.0, 360.0,
                cv::Scalar(1.0), cv::FILLED, cv::LINE_AA);
    int k = std::max(3, static_cast<int>(ax * 0.35));
    if (k % 2 == 0) ++k;
    cv::GaussianBlur(mask, mask, cv::Size(k, k), 0);

    cv::Mat patchf, alignedf, mask3;
    patch.convertTo(patchf, CV_32FC3);
    aligned.convertTo(alignedf, CV_32FC3);
    cv::cvtColor(mask, mask3, cv::COLOR_GRAY2BGR);
    cv::Mat merged = mask3.mul(alignedf) + (cv::Scalar::all(1.0) - mask3).mul(patchf);
    merged.convertTo(patch, CV_8UC3);
}

}  // namespace

// ---------------------------------------------------------------------------
// SCRFD
// ---------------------------------------------------------------------------

int detect_faces(Engine& e, const cv::Mat& bgr, std::vector<ff_face>& faces) {
    faces.clear();
    ModelSession* s = e.session("det", "det_10g.onnx");
    if (!s) return FF_ERR_MODEL;
    if (s->output_names().size() != 9) {
        e.set_error("det_10g.onnx must expose 9 outputs (scores/bbox/kps x 3)");
        return FF_ERR_MODEL;
    }

    const int det_w = e.opt.det_size > 0 ? e.opt.det_size : 640;
    const int det_h = det_w;
    const int img_w = bgr.cols, img_h = bgr.rows;

    const float im_ratio = static_cast<float>(img_h) / static_cast<float>(img_w);
    const float model_ratio = static_cast<float>(det_h) / static_cast<float>(det_w);
    int new_h, new_w;
    if (im_ratio > model_ratio) {
        new_h = det_h;
        new_w = static_cast<int>(new_h / im_ratio);
    } else {
        new_w = det_w;
        new_h = static_cast<int>(new_w * im_ratio);
    }
    new_w = std::max(1, std::min(new_w, det_w));
    new_h = std::max(1, std::min(new_h, det_h));
    const float det_scale = static_cast<float>(new_h) / static_cast<float>(img_h);

    cv::Mat resized, det_img(det_h, det_w, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::resize(bgr, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);
    resized.copyTo(det_img(cv::Rect(0, 0, new_w, new_h)));

    std::vector<float> input;
    to_blob(det_img, 127.5f, 128.0f, input);
    std::vector<Ort::Value> in;
    in.push_back(tensor(s, input, {1, 3, det_h, det_w}));
    std::vector<Ort::Value> outs;
    try {
        outs = s->run(in);
    } catch (const std::exception& ex) {
        e.set_error(std::string("SCRFD run failed: ") + ex.what());
        return FF_ERR_RUNTIME;
    }

    const int strides[3] = {8, 16, 32};
    const int num_anchors = 2;
    const float thresh = e.opt.det_thresh > 0.0f ? e.opt.det_thresh : 0.5f;
    const float nms_thresh = e.opt.nms_thresh > 0.0f ? e.opt.nms_thresh : 0.4f;

    std::vector<Candidate> all;
    for (int idx = 0; idx < 3; ++idx) {
        const int stride = strides[idx];
        const int fh = det_h / stride;
        const int fw = det_w / stride;
        const size_t n = static_cast<size_t>(fh) * fw * num_anchors;

        const float* scores = outs[idx].GetTensorData<float>();
        const float* bbox = outs[3 + idx].GetTensorData<float>();
        const float* kps = outs[6 + idx].GetTensorData<float>();

        size_t a = 0;
        for (int gy = 0; gy < fh; ++gy) {
            for (int gx = 0; gx < fw; ++gx) {
                const float cx = static_cast<float>(gx * stride);
                const float cy = static_cast<float>(gy * stride);
                for (int an = 0; an < num_anchors; ++an, ++a) {
                    if (a >= n) break;
                    const float sc = scores[a];
                    if (sc < thresh) continue;
                    Candidate c{};
                    c.score = sc;
                    c.x1 = cx - bbox[a * 4 + 0] * stride;
                    c.y1 = cy - bbox[a * 4 + 1] * stride;
                    c.x2 = cx + bbox[a * 4 + 2] * stride;
                    c.y2 = cy + bbox[a * 4 + 3] * stride;
                    for (int j = 0; j < 5; ++j) {
                        c.kps[j * 2 + 0] = cx + kps[a * 10 + j * 2 + 0] * stride;
                        c.kps[j * 2 + 1] = cy + kps[a * 10 + j * 2 + 1] * stride;
                    }
                    all.push_back(c);
                }
            }
        }
    }

    // InsightFace divides the decoded boxes by det_scale after decoding.
    for (auto& c : all) {
        c.x1 /= det_scale;
        c.y1 /= det_scale;
        c.x2 /= det_scale;
        c.y2 /= det_scale;
        for (int j = 0; j < 5; ++j) {
            c.kps[j * 2 + 0] /= det_scale;
            c.kps[j * 2 + 1] /= det_scale;
        }
    }

    auto kept = nms(std::move(all), nms_thresh);
    const int max_faces = e.opt.max_faces > 0 ? e.opt.max_faces : 10;
    if (static_cast<int>(kept.size()) > max_faces) kept.resize(max_faces);

    faces.reserve(kept.size());
    for (size_t i = 0; i < kept.size(); ++i) {
        ff_face f{};
        f.bbox[0] = kept[i].x1;
        f.bbox[1] = kept[i].y1;
        f.bbox[2] = kept[i].x2;
        f.bbox[3] = kept[i].y2;
        std::memcpy(f.kps, kept[i].kps, sizeof(f.kps));
        f.det_score = kept[i].score;
        f.face_id = static_cast<int32_t>(i);
        faces.push_back(f);
    }
    return FF_OK;
}

// ---------------------------------------------------------------------------
// ArcFace
// ---------------------------------------------------------------------------

int embed_face(Engine& e, const cv::Mat& bgr, const ff_face& face, float* out512) {
    ModelSession* s = e.session("arc", "w600k_r50.onnx");
    if (!s) return FF_ERR_MODEL;

    cv::Mat M = arcface_matrix(face.kps, 112);
    cv::Mat aligned = warp_face(bgr, M, 112, cv::INTER_LINEAR, cv::BORDER_CONSTANT);
    std::vector<float> input;
    to_blob(aligned, 127.5f, 127.5f, input);

    std::vector<Ort::Value> in;
    in.push_back(tensor(s, input, {1, 3, 112, 112}));
    std::vector<Ort::Value> outs;
    try {
        outs = s->run(in);
    } catch (const std::exception& ex) {
        e.set_error(std::string("ArcFace run failed: ") + ex.what());
        return FF_ERR_RUNTIME;
    }
    const float* p = outs[0].GetTensorData<float>();
    const size_t count = outs[0].GetTensorTypeAndShapeInfo().GetElementCount();
    if (count < 512) {
        e.set_error("ArcFace produced fewer than 512 values");
        return FF_ERR_RUNTIME;
    }
    std::memcpy(out512, p, 512 * sizeof(float));
    l2_normalize(out512, 512);
    return FF_OK;
}

// ---------------------------------------------------------------------------
// inswapper
// ---------------------------------------------------------------------------

int swap_patch(Engine& e, const cv::Mat& bgr, const ff_face& face,
               const float* embedding, cv::Mat& fake_bgr, cv::Mat& matrix) {
    ModelSession* s = e.session("swap", "inswapper_128.onnx");
    if (!s) return FF_ERR_MODEL;

    static std::vector<float> emap;
    if (emap.empty()) {
        if (!load_emap(e.model_dir + "/emap.bin", emap)) {
            e.set_error("cannot read " + e.model_dir +
                        "/emap.bin (run scripts/extract_emap.py)");
            return FF_ERR_MODEL;
        }
    }

    matrix = arcface_matrix(face.kps, 128);
    cv::Mat aligned = warp_face(bgr, matrix, 128, cv::INTER_LINEAR, cv::BORDER_CONSTANT);

    std::vector<float> input;
    to_blob(aligned, 0.0f, 255.0f, input);

    std::vector<float> latent(512, 0.0f);
    {
        // Traverse emap row by row so each cache line is consumed once. The
        // summation order per output stays i = 0..511, so the result is
        // bit-identical to the naive column-major loop.
        std::vector<double> acc(512, 0.0);
        for (int i = 0; i < 512; ++i) {
            const float ei = embedding[i];
            const float* row = emap.data() + static_cast<size_t>(i) * 512;
            for (int j = 0; j < 512; ++j) {
                const float prod = ei * row[j];
                acc[j] += static_cast<double>(prod);
            }
        }
        for (int j = 0; j < 512; ++j) latent[j] = static_cast<float>(acc[j]);
    }
    l2_normalize(latent.data(), 512);

    std::vector<Ort::Value> in;
    in.push_back(tensor(s, input, {1, 3, 128, 128}));
    in.push_back(tensor(s, latent, {1, 512}));
    std::vector<Ort::Value> outs;
    try {
        outs = s->run(in);
    } catch (const std::exception& ex) {
        e.set_error(std::string("inswapper run failed: ") + ex.what());
        return FF_ERR_RUNTIME;
    }

    const float* p = outs[0].GetTensorData<float>();
    fake_bgr.create(128, 128, CV_8UC3);
    for (int y = 0; y < 128; ++y) {
        uint8_t* row = fake_bgr.ptr<uint8_t>(y);
        for (int x = 0; x < 128; ++x) {
            const int i = y * 128 + x;
            const float r = p[i] * 255.0f;
            const float g = p[128 * 128 + i] * 255.0f;
            const float b = p[2 * 128 * 128 + i] * 255.0f;
            row[x * 3 + 0] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, b)));
            row[x * 3 + 1] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, g)));
            row[x * 3 + 2] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, r)));
        }
    }
    if (e.opt.restore_mouth) {
        restore_mouth(fake_bgr, aligned, matrix, face.kps);
    }
    return FF_OK;
}

void paste_back(const cv::Mat& target, const cv::Mat& patch, const cv::Mat& matrix,
                cv::Mat& out) {
    target.copyTo(out);
    cv::Mat inv;
    cv::invertAffineTransform(matrix, inv);

    cv::Mat aligned = warp_face(target, matrix, 128, cv::INTER_LINEAR, cv::BORDER_CONSTANT);

    // fake_diff = mean(abs(fake - aligned), axis=2)
    cv::Mat diff(128, 128, CV_32F);
    for (int y = 0; y < 128; ++y) {
        const uint8_t* a = patch.ptr<uint8_t>(y);
        const uint8_t* b = aligned.ptr<uint8_t>(y);
        float* d = diff.ptr<float>(y);
        for (int x = 0; x < 128; ++x) {
            const float db = std::abs(static_cast<float>(a[x * 3 + 0]) - b[x * 3 + 0]);
            const float dg = std::abs(static_cast<float>(a[x * 3 + 1]) - b[x * 3 + 1]);
            const float dr = std::abs(static_cast<float>(a[x * 3 + 2]) - b[x * 3 + 2]);
            d[x] = (db + dg + dr) / 3.0f;
        }
    }
    diff.rowRange(0, 2).setTo(0.0f);
    diff.rowRange(126, 128).setTo(0.0f);
    diff.colRange(0, 2).setTo(0.0f);
    diff.colRange(126, 128).setTo(0.0f);

    cv::Mat warped_fake, warped_mask, warped_diff;
    cv::warpAffine(patch, warped_fake, inv, target.size(), cv::INTER_LINEAR,
                   cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    cv::Mat white(128, 128, CV_32F, cv::Scalar(255));
    cv::warpAffine(white, warped_mask, inv, target.size(), cv::INTER_LINEAR,
                   cv::BORDER_CONSTANT, cv::Scalar(0));
    cv::warpAffine(diff, warped_diff, inv, target.size(), cv::INTER_LINEAR,
                   cv::BORDER_CONSTANT, cv::Scalar(0));

    warped_mask.setTo(255.0f, warped_mask > 20.0f);

    int minx = target.cols, miny = target.rows, maxx = -1, maxy = -1;
    for (int y = 0; y < warped_mask.rows; ++y) {
        const float* p = warped_mask.ptr<float>(y);
        for (int x = 0; x < warped_mask.cols; ++x) {
            if (p[x] == 255.0f) {
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
                miny = std::min(miny, y);
                maxy = std::max(maxy, y);
            }
        }
    }
    if (maxx < minx || maxy < miny) return;

    const int mask_size = static_cast<int>(
        std::sqrt(static_cast<double>(maxy - miny) * static_cast<double>(maxx - minx)));
    int k = std::max(mask_size / 10, 10);
    cv::Mat kernel = cv::Mat::ones(k, k, CV_8U);
    cv::erode(warped_mask, warped_mask, kernel);

    k = std::max(mask_size / 20, 5);
    const int g = 2 * k + 1;
    cv::GaussianBlur(warped_mask, warped_mask, cv::Size(g, g), 0);

    warped_mask /= 255.0f;
    cv::Mat mask3;
    cv::cvtColor(warped_mask, mask3, cv::COLOR_GRAY2BGR);
    cv::Mat targetf, fakef;
    target.convertTo(targetf, CV_32FC3);
    warped_fake.convertTo(fakef, CV_32FC3);
    cv::Mat merged = mask3.mul(fakef) +
                     (cv::Scalar::all(1.0) - mask3).mul(targetf);
    merged.convertTo(out, CV_8UC3);
}

// ---------------------------------------------------------------------------
// GFPGAN
// ---------------------------------------------------------------------------

int enhance_gfpgan(Engine& e, const cv::Mat& bgr, const std::vector<ff_face>& faces,
                   cv::Mat& out, ff_progress_fn cb, void* user) {
    ModelSession* s = e.session("gfpgan", "gfpgan_1.4.onnx");
    if (!s) return FF_ERR_MODEL;
    bgr.copyTo(out);

    if (faces.empty()) return FF_OK;
    for (size_t fi = 0; fi < faces.size(); ++fi) {
        if (cb) cb(FF_STAGE_ENHANCE, static_cast<float>(fi) / faces.size(),
                   "enhancing face", user);
        cv::Mat matrix = template_matrix(faces[fi].kps, kFfhq512, 512, 512);
        cv::Mat crop = warp_face(bgr, matrix, 512, cv::INTER_AREA, cv::BORDER_REPLICATE);

        std::vector<float> input;
        to_blob(crop, 127.5f, 127.5f, input);
        std::vector<Ort::Value> in;
        in.push_back(tensor(s, input, {1, 3, 512, 512}));
        std::vector<Ort::Value> outs;
        try {
            outs = s->run(in);
        } catch (const std::exception& ex) {
            // Split fallback: GFPGAN already works one 512x512 face crop at a
            // time, so a failure on a single face must not abort the whole
            // frame. Keep the swapped pixels for this face and enhance the
            // rest; only give up when there was a single face to begin with.
            e.set_error(std::string("GFPGAN run failed on face ") +
                        std::to_string(fi) + ": " + ex.what());
            if (faces.size() == 1) return FF_ERR_RUNTIME;
            continue;
        }
        const float* p = outs[0].GetTensorData<float>();
        cv::Mat enh(512, 512, CV_8UC3);
        for (int y = 0; y < 512; ++y) {
            uint8_t* row = enh.ptr<uint8_t>(y);
            for (int x = 0; x < 512; ++x) {
                const int i = y * 512 + x;
                const float r = std::min(1.0f, std::max(-1.0f, p[i]));
                const float g = std::min(1.0f, std::max(-1.0f, p[512 * 512 + i]));
                const float b = std::min(1.0f, std::max(-1.0f, p[2 * 512 * 512 + i]));
                row[x * 3 + 0] = static_cast<uint8_t>(std::lround((b + 1.0f) * 127.5f));
                row[x * 3 + 1] = static_cast<uint8_t>(std::lround((g + 1.0f) * 127.5f));
                row[x * 3 + 2] = static_cast<uint8_t>(std::lround((r + 1.0f) * 127.5f));
            }
        }
        paste_soft(out, enh, matrix, 0.8f);
    }
    if (cb) cb(FF_STAGE_ENHANCE, 1.0f, "enhancing face", user);
    return FF_OK;
}

// ---------------------------------------------------------------------------
// Real-ESRGAN (whole frame, tiled). upscale_esrgan keeps the model's native
// output size; enhance_esrgan wraps it and resizes back to the input, which is
// what the ff_enhance contract wants but is not useful as an upscaler.
// ---------------------------------------------------------------------------

int upscale_esrgan(Engine& e, const cv::Mat& bgr, int scale, cv::Mat& out,
                   ff_progress_fn cb, void* user) {
    const std::string file =
        (scale == 4) ? "real_esrgan_x4.onnx" : "real_esrgan_x2.onnx";
    const std::string key = (scale == 4) ? "esrgan4" : "esrgan2";
    ModelSession* s = e.session(key, file);
    if (!s) return FF_ERR_MODEL;

    int tile = e.opt.esrgan_tile > 0 ? e.opt.esrgan_tile : 256;
    tile = std::max(64, std::min(tile, 512));
    tile = (tile / 8) * 8;
    const int pad = 16;

    const int w = bgr.cols, h = bgr.rows;
    const int step = tile - 2 * pad;
    cv::Mat upscaled(h * scale, w * scale, CV_8UC3);
    const int tiles_x = (w + step - 1) / step;
    const int tiles_y = (h + step - 1) / step;
    const int total = std::max(1, tiles_x * tiles_y);
    int done = 0;

    for (int ty = 0; ty < tiles_y; ++ty) {
        for (int tx = 0; tx < tiles_x; ++tx) {
            const int x0 = tx * step;
            const int y0 = ty * step;
            const int x1 = std::min(x0 + step, w);
            const int y1 = std::min(y0 + step, h);
            // The model always sees a `tile`x`tile` window whose top-left is at
            // (x0 - pad, y0 - pad); the step region sits at (pad, pad) inside
            // it. Clamp the window to the frame and replicate-pad whatever
            // falls outside, so `canvas` is exactly `tile`x`tile`.
            const int gx = x0 - pad;
            const int gy = y0 - pad;
            const int sx0 = std::max(0, gx);
            const int sy0 = std::max(0, gy);
            const int sx1 = std::min(w, gx + tile);
            const int sy1 = std::min(h, gy + tile);

            cv::Mat roi = bgr(cv::Rect(sx0, sy0, sx1 - sx0, sy1 - sy0));
            const int left = sx0 - gx;
            const int top = sy0 - gy;
            cv::Mat canvas;
            cv::copyMakeBorder(roi, canvas, top, tile - top - roi.rows, left,
                               tile - left - roi.cols, cv::BORDER_REPLICATE);

            std::vector<float> input;
            to_blob(canvas, 0.0f, 255.0f, input);
            std::vector<Ort::Value> in;
            in.push_back(tensor(s, input, {1, 3, tile, tile}));
            std::vector<Ort::Value> outs;
            try {
                outs = s->run(in);
            } catch (const std::exception& ex) {
                e.set_error(std::string("Real-ESRGAN run failed: ") + ex.what());
                return FF_ERR_RUNTIME;
            }
            const float* p = outs[0].GetTensorData<float>();
            cv::Mat tile_bgr(tile * scale, tile * scale, CV_8UC3);
            const int plane = tile * scale * tile * scale;
            for (int y = 0; y < tile * scale; ++y) {
                uint8_t* row = tile_bgr.ptr<uint8_t>(y);
                for (int x = 0; x < tile * scale; ++x) {
                    const int i = y * (tile * scale) + x;
                    row[x * 3 + 0] = static_cast<uint8_t>(
                        std::min(255.0f, std::max(0.0f, p[2 * plane + i] * 255.0f)));
                    row[x * 3 + 1] = static_cast<uint8_t>(
                        std::min(255.0f, std::max(0.0f, p[plane + i] * 255.0f)));
                    row[x * 3 + 2] = static_cast<uint8_t>(
                        std::min(255.0f, std::max(0.0f, p[i] * 255.0f)));
                }
            }

            const cv::Rect src_rect((x0 - gx) * scale, (y0 - gy) * scale,
                                    (x1 - x0) * scale, (y1 - y0) * scale);
            const cv::Rect dst_rect(x0 * scale, y0 * scale, (x1 - x0) * scale,
                                    (y1 - y0) * scale);
            tile_bgr(src_rect).copyTo(upscaled(dst_rect));

            ++done;
            if (cb) cb(FF_STAGE_ENHANCE, static_cast<float>(done) / total,
                       "enhancing frame", user);
        }
    }

    out = std::move(upscaled);
    return FF_OK;
}

int enhance_esrgan(Engine& e, const cv::Mat& bgr, int scale, cv::Mat& out,
                   ff_progress_fn cb, void* user) {
    cv::Mat up;
    const int rc = upscale_esrgan(e, bgr, scale, up, cb, user);
    if (rc != FF_OK) return rc;
    cv::resize(up, out, bgr.size(), 0, 0, cv::INTER_AREA);
    return FF_OK;
}

}  // namespace ff
