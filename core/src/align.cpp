#include "internal.hpp"

#include <cmath>
#include <cstring>
#include <fstream>

namespace ff {

cv::Mat wrap_bgr(const uint8_t* data, int width, int height, int stride) {
    if (stride <= 0) stride = width * 3;
    return cv::Mat(height, width, CV_8UC3, const_cast<uint8_t*>(data),
                   static_cast<size_t>(stride));
}

// ---------------------------------------------------------------------------
// Umeyama similarity fit, matching skimage.transform.SimilarityTransform
// ---------------------------------------------------------------------------

static cv::Mat umeyama(const cv::Mat& src, const cv::Mat& dst) {
    const int num = src.rows;
    const int dim = 2;

    cv::Mat src_mean, dst_mean;
    cv::reduce(src, src_mean, 0, cv::REDUCE_AVG);
    cv::reduce(dst, dst_mean, 0, cv::REDUCE_AVG);
    cv::Mat src_d = src - cv::repeat(src_mean, num, 1);
    cv::Mat dst_d = dst - cv::repeat(dst_mean, num, 1);

    cv::Mat A = (dst_d.t() * src_d) / static_cast<double>(num);

    cv::Mat d = cv::Mat::ones(dim, 1, CV_64F);
    if (cv::determinant(A) < 0.0) d.at<double>(dim - 1, 0) = -1.0;

    cv::Mat w, u, vt;
    cv::SVD::compute(A, w, u, vt);

    // Numerical rank, same idea as numpy.linalg.matrix_rank.
    const double tol = w.at<double>(0) * std::max(2, 2) * 2.220446049250313e-16;
    int rank = 0;
    for (int i = 0; i < dim; ++i)
        if (w.at<double>(i) > tol) ++rank;

    cv::Mat R = cv::Mat::eye(dim, dim, CV_64F);
    if (rank == 0) {
        return cv::Mat::zeros(2, 3, CV_64F);
    } else if (rank == dim - 1) {
        if (cv::determinant(u) * cv::determinant(vt) > 0.0) {
            R = u * vt;
        } else {
            const double s = d.at<double>(dim - 1, 0);
            d.at<double>(dim - 1, 0) = -1.0;
            R = u * cv::Mat::diag(d) * vt;
            d.at<double>(dim - 1, 0) = s;
        }
    } else {
        R = u * cv::Mat::diag(d) * vt;
    }

    const double vsum = cv::sum(src_d.mul(src_d))[0] / static_cast<double>(num);
    const double dot = w.at<double>(0) * d.at<double>(0, 0) +
                       w.at<double>(1) * d.at<double>(1, 0);
    const double scale = (vsum > 0.0) ? dot / vsum : 1.0;

    // T[:dim, dim] = dst_mean - scale * (R @ src_mean)
    cv::Mat t = dst_mean.t() - scale * (R * src_mean.t());

    cv::Mat M(2, 3, CV_64F);
    cv::Mat R2 = M(cv::Rect(0, 0, 2, 2));
    R.convertTo(R2, CV_64F, scale);
    t.copyTo(M.col(2));
    return M;
}

cv::Mat arcface_matrix(const float kps[10], int size) {
    static const double tmpl[5][2] = {
        {38.2946, 51.6963}, {73.5318, 51.5014}, {56.0252, 71.7366},
        {41.5493, 92.3655}, {70.7299, 92.2041}};

    double ratio, diff_x;
    if (size % 112 == 0) {
        ratio = static_cast<double>(size) / 112.0;
        diff_x = 0.0;
    } else {
        ratio = static_cast<double>(size) / 128.0;
        diff_x = 8.0 * ratio;
    }

    cv::Mat src(5, 2, CV_64F), dst(5, 2, CV_64F);
    for (int i = 0; i < 5; ++i) {
        src.at<double>(i, 0) = kps[i * 2 + 0];
        src.at<double>(i, 1) = kps[i * 2 + 1];
        dst.at<double>(i, 0) = tmpl[i][0] * ratio + diff_x;
        dst.at<double>(i, 1) = tmpl[i][1] * ratio;
    }
    return umeyama(src, dst);
}

cv::Mat template_matrix(const float kps[10], const float tmpl[10], int width, int height) {
    cv::Mat src(5, 2, CV_32F), dst(5, 2, CV_32F);
    for (int i = 0; i < 5; ++i) {
        src.at<float>(i, 0) = kps[i * 2 + 0];
        src.at<float>(i, 1) = kps[i * 2 + 1];
        dst.at<float>(i, 0) = tmpl[i * 2 + 0] * static_cast<float>(width);
        dst.at<float>(i, 1) = tmpl[i * 2 + 1] * static_cast<float>(height);
    }
    cv::Mat m = cv::estimateAffinePartial2D(src, dst, cv::noArray(), cv::RANSAC, 100.0);
    if (m.empty()) m = arcface_matrix(kps, std::max(width, height));
    return m;
}

cv::Mat warp_face(const cv::Mat& image, const cv::Mat& matrix, int size,
                  int interpolation, int border_mode) {
    cv::Mat out;
    cv::warpAffine(image, out, matrix, cv::Size(size, size), interpolation,
                   border_mode, cv::Scalar(0, 0, 0));
    return out;
}

void to_blob(const cv::Mat& bgr, float mean, float scale, std::vector<float>& out) {
    to_blob_rect(bgr, 0, 0, bgr.cols, bgr.rows, mean, scale, out);
}

void to_blob_rect(const cv::Mat& bgr, int x, int y, int w, int h,
                  float mean, float scale, std::vector<float>& out) {
    out.resize(static_cast<size_t>(w) * h * 3);
    const float inv = 1.0f / scale;
    float* pr = out.data();
    float* pg = pr + static_cast<size_t>(w) * h;
    float* pb = pg + static_cast<size_t>(w) * h;
    for (int row = 0; row < h; ++row) {
        const uint8_t* p = bgr.ptr<uint8_t>(y + row) + static_cast<size_t>(x) * 3;
        for (int col = 0; col < w; ++col) {
            const float b = p[0], g = p[1], r = p[2];
            *pr++ = (r - mean) * inv;
            *pg++ = (g - mean) * inv;
            *pb++ = (b - mean) * inv;
            p += 3;
        }
    }
}

void l2_normalize(float* v, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; ++i) sum += static_cast<double>(v[i]) * v[i];
    const float norm = static_cast<float>(std::sqrt(sum));
    if (norm > 1e-12f) {
        const float inv = 1.0f / norm;
        for (int i = 0; i < n; ++i) v[i] *= inv;
    }
}

bool load_emap(const std::string& path, std::vector<float>& emap) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize n = f.tellg();
    if (n != static_cast<std::streamsize>(512 * 512 * sizeof(float))) return false;
    f.seekg(0);
    emap.resize(512 * 512);
    return static_cast<bool>(f.read(reinterpret_cast<char*>(emap.data()), n));
}

}  // namespace ff
