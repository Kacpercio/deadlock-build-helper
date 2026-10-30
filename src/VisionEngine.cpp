#include "VisionEngine.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <fstream>
#include <iostream>
#include <iterator>

namespace fs = std::filesystem;

namespace {

// Six enemies can own the same item; anything beyond that is noise.
constexpr int kMaxHitsPerTemplate = 8;
constexpr double kOverlapIou = 0.3;

double iou(const cv::Rect& a, const cv::Rect& b)
{
    const double inter = (a & b).area();
    const double uni = a.area() + b.area() - inter;
    return uni > 0.0 ? inter / uni : 0.0;
}

// Similar-looking icons can both fire on the same slot; keep only the best-scoring one.
std::vector<Detection> suppressOverlaps(std::vector<Detection> detections)
{
    std::ranges::sort(detections, std::greater{}, &Detection::score);
    std::vector<Detection> kept;
    for (auto& candidate : detections) {
        const bool overlaps = std::ranges::any_of(kept, [&](const Detection& k) {
            return iou(k.box, candidate.box) > kOverlapIou;
        });
        if (!overlaps) {
            kept.push_back(std::move(candidate));
        }
    }
    return kept;
}

}  // namespace

cv::Mat readImage(const fs::path& file, int flags)
{
    std::ifstream in(file, std::ios::binary);
    const std::vector<uchar> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return bytes.empty() ? cv::Mat() : cv::imdecode(bytes, flags);
}

bool writeImage(const fs::path& file, const cv::Mat& image)
{
    std::vector<uchar> bytes;
    if (image.empty() || !cv::imencode(file.extension().string(), image, bytes)) {
        return false;
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

void matchNormed(const cv::Mat& search, const cv::Mat& templ, const cv::Mat& mask, cv::Mat& response)
{
    if (mask.empty()) {
        cv::matchTemplate(search, templ, response, cv::TM_CCOEFF_NORMED);
    } else {
        cv::matchTemplate(search, templ, response, cv::TM_CCOEFF_NORMED, mask);
    }
    // Flat image patches make the normalisation divide by ~0: NaN or +-inf. Those must never
    // count as hits, and a genuine score cannot exceed 1.
    cv::patchNaNs(response, -1.0);
    response.setTo(-1.0, response > 1.0001);
}

std::vector<TemplateFile> readTemplateFolder(const fs::path& dir)
{
    std::vector<TemplateFile> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string ext = entry.path().extension().string();
        std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".png" && ext != ".jpg" && ext != ".bmp") {
            continue;
        }

        cv::Mat raw = readImage(entry.path(), cv::IMREAD_UNCHANGED);
        if (raw.empty()) {
            std::cerr << "[vision] could not read " << entry.path().string() << '\n';
            continue;
        }
        if (raw.depth() != CV_8U) {
            raw.convertTo(raw, CV_8U, 1.0 / 257.0);  // 16-bit PNGs
        }
        cv::Mat image, mask;
        std::vector<cv::Mat> channels;
        cv::split(raw, channels);
        if (raw.channels() == 2 || raw.channels() == 4) {
            cv::threshold(channels.back(), mask, 127, 255, cv::THRESH_BINARY);
            channels.pop_back();
            if (cv::countNonZero(mask) == mask.rows * mask.cols) {
                mask.release();  // fully opaque: no need for masked matching
            }
        }
        if (channels.size() == 1) {
            cv::cvtColor(channels[0], image, cv::COLOR_GRAY2BGR);
        } else {
            cv::merge(channels, image);
        }

        std::string id = entry.path().stem().string();
        if (const auto at = id.find('@'); at != std::string::npos) {
            id.resize(at);
        }
        files.push_back({std::move(id), std::move(image), std::move(mask)});
    }
    if (ec) {
        std::cerr << "[vision] cannot open template dir " << dir.string() << ": " << ec.message() << '\n';
    }
    return files;
}

VisionEngine::VisionEngine(VisionConfig cfg)
    : cfg_(cfg)
{
}

size_t VisionEngine::loadTemplates(const fs::path& dir)
{
    templates_.clear();
    scaledForHeight_ = 0;
    for (auto& file : readTemplateFolder(dir)) {
        templates_.push_back({std::move(file.id), std::move(file.image), std::move(file.mask), {}, {}});
    }
    return templates_.size();
}

void VisionEngine::prepareTemplates(int frameHeight)
{
    if (frameHeight == scaledForHeight_ && cfg_.iconScale == scaledForIconScale_) {
        return;
    }
    const double base =
        (static_cast<double>(frameHeight) / cfg_.referenceHeight) * cfg_.downscale * cfg_.iconScale;
    for (auto& t : templates_) {
        const double scale = cfg_.templateHeight > 0 ? base * cfg_.templateHeight / t.original.rows : base;
        const cv::Size size(std::max(1, static_cast<int>(std::lround(t.original.cols * scale))),
                            std::max(1, static_cast<int>(std::lround(t.original.rows * scale))));
        if (size.width < 4 || size.height < 4) {
            t.scaled.release();  // too small to mean anything; skipped by the search
            continue;
        }
        cv::resize(t.original, t.scaled, size, 0, 0, scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);

        t.scaledMask.release();
        if (!t.originalMask.empty() || cfg_.cornerMask > 0.0) {
            cv::Mat mask(size, CV_8U, cv::Scalar(255));
            if (!t.originalMask.empty()) {
                cv::resize(t.originalMask, mask, size, 0, 0, cv::INTER_NEAREST);
            }
            if (cfg_.cornerMask > 0.0) {
                const int w = size.width, h = size.height;
                const std::vector<cv::Point> corner{{static_cast<int>(w * (1.0 - cfg_.cornerMask)), 0},
                                                    {w, 0},
                                                    {w, static_cast<int>(h * cfg_.cornerMask)}};
                cv::fillConvexPoly(mask, corner, cv::Scalar(0));
            }
            t.scaledMask = mask;
        }
    }
    scaledForHeight_ = frameHeight;
    scaledForIconScale_ = cfg_.iconScale;
}

ScaleTune VisionEngine::tuneScale(const cv::Mat& frameBgr, cv::Rect roi, double from, double to)
{
    const double original = cfg_.iconScale;
    ScaleTune best{original, 0, 0.0};
    double plateauLow = original, plateauHigh = original;

    const auto tryScale = [&](double scale) {
        cfg_.iconScale = scale;
        const std::vector<Detection> hits = detect(frameBgr, roi);
        double mean = 0.0;
        for (const auto& h : hits) {
            mean += h.score;
        }
        mean = hits.empty() ? 0.0 : mean / static_cast<double>(hits.size());
        const int count = static_cast<int>(hits.size());
        const bool same = count == best.detections && std::abs(mean - best.meanScore) < 1e-9;
        if (count > best.detections || (count == best.detections && count > 0 && mean > best.meanScore + 1e-9)) {
            best = {scale, count, mean};
            plateauLow = plateauHigh = scale;
        } else if (same && count > 0) {
            // Neighbouring scales often round to the same template size; remember the plateau.
            plateauLow = std::min(plateauLow, scale);
            plateauHigh = std::max(plateauHigh, scale);
        }
    };

    constexpr double kCoarse = 0.05;
    constexpr double kFine = 0.0125;
    const int coarseSteps = static_cast<int>((to - from) / kCoarse + 0.5);
    for (int i = 0; i <= coarseSteps; ++i) {
        tryScale(from + i * kCoarse);
    }
    const double centre = best.scale;
    for (int i = -3; i <= 3; ++i) {
        if (i != 0) {
            tryScale(centre + i * kFine);
        }
    }

    if (best.detections > 0) {
        best.scale = (plateauLow + plateauHigh) / 2.0;  // middle of the equally good scales
    }
    cfg_.iconScale = best.detections > 0 ? best.scale : original;
    return best;
}

std::vector<Detection> VisionEngine::detect(const cv::Mat& frameBgr, cv::Rect roi)
{
    roi &= cv::Rect(0, 0, frameBgr.cols, frameBgr.rows);
    if (roi.empty() || templates_.empty()) {
        return {};
    }
    prepareTemplates(frameBgr.rows);

    cv::Mat search;
    cv::resize(frameBgr(roi), search, {}, cfg_.downscale, cfg_.downscale, cv::INTER_AREA);

    // One output slot per template so worker threads never share a vector.
    std::vector<std::vector<Detection>> perTemplate(templates_.size());
    cv::parallel_for_(cv::Range(0, static_cast<int>(templates_.size())), [&](const cv::Range& range) {
        cv::Mat response;
        for (int i = range.start; i < range.end; ++i) {
            const Template& t = templates_[i];
            if (t.scaled.empty() || t.scaled.cols > search.cols || t.scaled.rows > search.rows) {
                continue;
            }
            matchNormed(search, t.scaled, t.scaledMask, response);
            collectPeaks(response, t, roi.tl(), perTemplate[i]);
        }
    });

    std::vector<Detection> all;
    for (auto& hits : perTemplate) {
        all.insert(all.end(), std::make_move_iterator(hits.begin()), std::make_move_iterator(hits.end()));
    }
    return suppressOverlaps(std::move(all));
}

void VisionEngine::collectPeaks(cv::Mat& response, const Template& t, cv::Point roiOrigin,
                                std::vector<Detection>& out) const
{
    const double toFrame = 1.0 / cfg_.downscale;
    const cv::Rect bounds(0, 0, response.cols, response.rows);

    for (int n = 0; n < kMaxHitsPerTemplate; ++n) {
        double best = 0.0;
        cv::Point loc;
        cv::minMaxLoc(response, nullptr, &best, nullptr, &loc);
        if (best < cfg_.threshold) {
            break;
        }

        out.push_back({
            t.id,
            cv::Rect(roiOrigin.x + static_cast<int>(loc.x * toFrame),
                     roiOrigin.y + static_cast<int>(loc.y * toFrame),
                     static_cast<int>(t.scaled.cols * toFrame),
                     static_cast<int>(t.scaled.rows * toFrame)),
            best,
        });

        // Blank this peak's neighbourhood so the next minMaxLoc finds a different icon.
        const cv::Rect blank(loc.x - t.scaled.cols / 2, loc.y - t.scaled.rows / 2,
                             t.scaled.cols, t.scaled.rows);
        response(blank & bounds).setTo(-1.0);
    }
}
