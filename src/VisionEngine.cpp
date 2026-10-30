#include "VisionEngine.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <utility>

namespace fs = std::filesystem;

namespace {

// Six enemies can own the same item; anything beyond that is noise.
constexpr int kMaxHitsPerTemplate = 8;
constexpr double kOverlapIou = 0.3;

// scan(): how far below the threshold the first pass may score and still get a colour check, and how
// many pixels around its spot that check looks.
constexpr double kCoarseAllowance = 0.20;
constexpr int kConfirmSlack = 3;
constexpr int kCoarsePeaks = 2;  // small, plain icons can peak somewhere else first

// An active item on the scoreboard: a disc, its bottom covered by a label such as "ACTIVE".
constexpr double kActiveLabelStart = 0.72;

// The label holds text, so its brightest pixels stand out from the rest. A hit without any is dark
// art finding a dark patch of the game world. Measured: the real one 99, wrong ones at most 35.
constexpr double kMinLabelContrast = 50.0;

// Where the scoreboard puts items, in pixels at reference_height.
constexpr int kGridLeft = 11;  // of the first icon, from the left edge of the column
constexpr int kGridTop = 334;  // of the first line, from the top of the screen
constexpr int kGridStep = 35;  // from one icon to the next, both ways
constexpr int kGridColumns = 2;
constexpr int kGridSlack = 4;      // how far an icon may be from where it should be
constexpr int kGridTolerance = 8;  // a hit further than this from any place is not on the grid

// Measured on a real frame: icons 0.94-0.98 with the next item at most 0.85 (a close relative), places
// without an item at most 0.78 and usually about 0.5.
constexpr double kGapMinScore = 0.65;
constexpr double kGapMinLead = 0.05;
constexpr double kNextMinScore = 0.75;
constexpr double kNextMinLead = 0.10;
constexpr size_t kCellFinalists = 6;  // items a place is compared with in colour, after a first look in grey

bool showsActiveLabel(const cv::Mat& iconBgr)
{
    cv::Mat label;
    cv::cvtColor(iconBgr.rowRange(static_cast<int>(iconBgr.rows * kActiveLabelStart), iconBgr.rows), label, cv::COLOR_BGR2GRAY);
    double brightest = 0.0;
    cv::minMaxLoc(label, nullptr, &brightest);
    return brightest - cv::mean(label)[0] >= kMinLabelContrast;
}

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
        templates_.push_back({std::move(file.id), std::move(file.image), std::move(file.mask), {}, {}, {}, {}, false});
    }
    return templates_.size();
}

size_t VisionEngine::addActiveItems(const fs::path& dir)
{
    scaledForHeight_ = 0;
    size_t added = 0;
    for (auto& file : readTemplateFolder(dir)) {
        templates_.push_back({std::move(file.id), std::move(file.image), std::move(file.mask), {}, {}, {}, {}, true});
        ++added;
    }
    return added;
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
        if (!t.originalMask.empty() || cfg_.cornerMask > 0.0 || t.activeLook) {
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
            if (t.activeLook) {
                cv::Mat disc(size, CV_8U, cv::Scalar(0));
                cv::circle(disc, {size.width / 2, size.height / 2}, std::min(size.width, size.height) / 2, cv::Scalar(255), cv::FILLED);
                disc.rowRange(static_cast<int>(size.height * kActiveLabelStart), size.height).setTo(0);
                cv::bitwise_and(mask, disc, mask);
            }
            t.scaledMask = mask;
        }

        cv::Mat gray;
        cv::cvtColor(t.scaled, gray, cv::COLOR_BGR2GRAY);
        const cv::Size half(std::max(1, gray.cols / 2), std::max(1, gray.rows / 2));
        cv::resize(gray, t.coarse, half, 0, 0, cv::INTER_AREA);
        t.coarseMask.release();
        if (!t.scaledMask.empty()) {
            cv::resize(t.scaledMask, t.coarseMask, half, 0, 0, cv::INTER_NEAREST);
        }
    }
    scaledForHeight_ = frameHeight;
    scaledForIconScale_ = cfg_.iconScale;
}

ScaleTune VisionEngine::tuneScale(const cv::Mat& frameBgr, const std::vector<cv::Rect>& columns, double from, double to)
{
    const double original = cfg_.iconScale;
    ScaleTune best{original, 0, 0.0};
    double plateauLow = original, plateauHigh = original;

    const auto tryScale = [&](double scale) {
        cfg_.iconScale = scale;
        const std::vector<Detection> hits = scan(frameBgr, columns);
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
            // Active items' art needs the check scan() does: this search is for plain icons.
            if (t.activeLook || t.scaled.empty() || t.scaled.cols > search.cols || t.scaled.rows > search.rows) {
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

std::vector<Detection> VisionEngine::scan(const cv::Mat& frameBgr, const std::vector<cv::Rect>& columns)
{
    if (templates_.empty()) {
        return {};
    }
    prepareTemplates(frameBgr.rows);

    struct Column {
        cv::Rect box;
        cv::Mat search;  // colour, at `downscale`
        cv::Mat coarse;  // grey, half of that
    };
    std::vector<Column> prepared;
    for (const auto& c : columns) {
        const cv::Rect box = c & cv::Rect(0, 0, frameBgr.cols, frameBgr.rows);
        if (box.empty()) {
            continue;
        }
        Column column{box, {}, {}};
        cv::resize(frameBgr(box), column.search, {}, cfg_.downscale, cfg_.downscale, cv::INTER_AREA);
        cv::cvtColor(column.search, column.coarse, cv::COLOR_BGR2GRAY);
        cv::resize(column.coarse, column.coarse, {}, 0.5, 0.5, cv::INTER_AREA);
        prepared.push_back(std::move(column));
    }

    const double toFrame = 1.0 / cfg_.downscale;
    const int total = static_cast<int>(templates_.size() * prepared.size());
    std::vector<std::optional<Detection>> hits(static_cast<size_t>(total));
    cv::parallel_for_(cv::Range(0, total), [&](const cv::Range& range) {
        cv::Mat response, coarseResponse;
        for (int n = range.start; n < range.end; ++n) {
            const Template& t = templates_[static_cast<size_t>(n) % templates_.size()];
            const Column& column = prepared[static_cast<size_t>(n) / templates_.size()];
            if (t.scaled.empty() || t.scaled.cols > column.search.cols || t.scaled.rows > column.search.rows ||
                t.coarse.cols > column.coarse.cols || t.coarse.rows > column.coarse.rows) {
                continue;
            }

            matchNormed(column.coarse, t.coarse, t.coarseMask, coarseResponse);
            for (int peak = 0; peak < kCoarsePeaks; ++peak) {
                double coarseScore = 0.0;
                cv::Point coarseAt;
                cv::minMaxLoc(coarseResponse, nullptr, &coarseScore, nullptr, &coarseAt);
                if (coarseScore < cfg_.threshold - kCoarseAllowance) {
                    break;
                }
                // The next peak must be somewhere else.
                coarseResponse(cv::Rect(coarseAt.x - t.coarse.cols / 2, coarseAt.y - t.coarse.rows / 2, t.coarse.cols, t.coarse.rows) &
                               cv::Rect(0, 0, coarseResponse.cols, coarseResponse.rows))
                    .setTo(-1.0);

                const cv::Rect window = cv::Rect(coarseAt.x * 2 - kConfirmSlack, coarseAt.y * 2 - kConfirmSlack,
                                                 t.scaled.cols + 2 * kConfirmSlack, t.scaled.rows + 2 * kConfirmSlack) &
                                        cv::Rect(0, 0, column.search.cols, column.search.rows);
                if (window.width < t.scaled.cols || window.height < t.scaled.rows) {
                    continue;
                }
                matchNormed(column.search(window), t.scaled, t.scaledMask, response);
                double score = 0.0;
                cv::Point at;
                cv::minMaxLoc(response, nullptr, &score, nullptr, &at);
                const cv::Rect icon(window.x + at.x, window.y + at.y, t.scaled.cols, t.scaled.rows);
                if (score >= cfg_.threshold && (!t.activeLook || showsActiveLabel(column.search(icon))) &&
                    (!hits[static_cast<size_t>(n)] || score > hits[static_cast<size_t>(n)]->score)) {
                    hits[static_cast<size_t>(n)] = Detection{
                        t.id,
                        cv::Rect(column.box.x + static_cast<int>((window.x + at.x) * toFrame),
                                 column.box.y + static_cast<int>((window.y + at.y) * toFrame),
                                 static_cast<int>(t.scaled.cols * toFrame), static_cast<int>(t.scaled.rows * toFrame)),
                        score,
                    };
                }
            }
        }
    });

    std::vector<Detection> all;
    for (auto& hit : hits) {
        if (hit) {
            all.push_back(std::move(*hit));
        }
    }
    return suppressOverlaps(std::move(all));
}

VisionEngine::CellMatch VisionEngine::matchCell(const cv::Mat& frameBgr, cv::Rect window) const
{
    CellMatch match;
    window &= cv::Rect(0, 0, frameBgr.cols, frameBgr.rows);
    if (window.empty()) {
        return match;
    }
    cv::Mat search, coarse;
    cv::resize(frameBgr(window), search, {}, cfg_.downscale, cfg_.downscale, cv::INTER_AREA);
    cv::cvtColor(search, coarse, cv::COLOR_BGR2GRAY);
    cv::resize(coarse, coarse, {}, 0.5, 0.5, cv::INTER_AREA);

    // In grey at half size first, as scan() does: this runs on every column.
    cv::Mat response;
    std::vector<std::pair<double, const Template*>> candidates;
    for (const Template& t : templates_) {
        if (t.scaled.empty() || t.scaled.cols > search.cols || t.scaled.rows > search.rows || t.coarse.cols > coarse.cols ||
            t.coarse.rows > coarse.rows) {
            continue;
        }
        matchNormed(coarse, t.coarse, t.coarseMask, response);
        double score = 0.0;
        cv::minMaxLoc(response, nullptr, &score);
        candidates.emplace_back(score, &t);
    }
    const auto finalists = std::min(kCellFinalists, candidates.size());
    std::ranges::partial_sort(candidates, candidates.begin() + static_cast<std::ptrdiff_t>(finalists), std::greater{},
                              [](const auto& c) { return c.first; });
    candidates.resize(finalists);

    cv::Rect at;
    for (const auto& [coarseScore, t] : candidates) {
        matchNormed(search, t->scaled, t->scaledMask, response);
        double score = 0.0;
        cv::Point loc;
        cv::minMaxLoc(response, nullptr, &score, nullptr, &loc);
        if (score > match.score) {
            if (!match.best || match.best->id != t->id) {
                match.runnerUp = match.score;
            }
            match.score = score;
            match.best = t;
            at = cv::Rect(loc.x, loc.y, t->scaled.cols, t->scaled.rows);
        } else if (score > match.runnerUp && match.best && match.best->id != t->id) {
            match.runnerUp = score;
        }
    }
    if (match.best) {
        const double toFrame = 1.0 / cfg_.downscale;
        match.box = cv::Rect(window.x + static_cast<int>(at.x * toFrame), window.y + static_cast<int>(at.y * toFrame),
                             static_cast<int>(at.width * toFrame), static_cast<int>(at.height * toFrame));
        match.activeLabel = match.best->activeLook && showsActiveLabel(search(at));
    }
    return match;
}

void VisionEngine::fillGaps(const cv::Mat& frameBgr, const std::vector<cv::Rect>& columns, std::vector<Detection>& hits)
{
    if (templates_.empty()) {
        return;
    }
    prepareTemplates(frameBgr.rows);

    const double scale = static_cast<double>(frameBgr.rows) / cfg_.referenceHeight * cfg_.iconScale;
    const int step = static_cast<int>(std::lround(kGridStep * scale));
    const int icon = static_cast<int>(std::lround(cfg_.templateHeight * scale));
    const int slack = static_cast<int>(std::lround(kGridSlack * scale));
    const int tolerance = static_cast<int>(std::lround(kGridTolerance * scale));
    const int gridTop = static_cast<int>(std::lround(kGridTop * scale));
    if (step <= 0 || icon <= 0) {
        return;
    }

    std::vector<Detection> found;
    for (size_t c = 0; c < columns.size(); ++c) {
        const cv::Rect column = columns[c] & cv::Rect(0, 0, frameBgr.cols, frameBgr.rows);
        if (column.empty()) {
            continue;
        }
        const int gridLeft = column.x + static_cast<int>(std::lround(kGridLeft * scale));

        // The grid is where the hits say it is, should the game have moved it.
        int firstLine = gridTop;
        std::vector<Detection> own;
        std::ranges::copy_if(hits, std::back_inserter(own), [&](const Detection& d) { return d.slot == static_cast<int>(c) && !d.isHero; });
        if (!own.empty()) {
            const int topHit = std::ranges::min_element(own, {}, [](const Detection& d) { return d.box.y; })->box.y;
            firstLine = topHit;
            while (firstLine - step >= std::min(gridTop, topHit) - step / 2) {
                firstLine -= step;
            }
        }
        const int lines = (column.br().y - firstLine + step - icon) / step;

        // Which places hold a hit.
        std::vector<cv::Point> offsets;
        std::vector<bool> taken(static_cast<size_t>(std::max(0, lines) * kGridColumns), false);
        int last = -1;
        for (const Detection& d : own) {
            const int across = static_cast<int>(std::lround(static_cast<double>(d.box.x - gridLeft) / step));
            const int down = static_cast<int>(std::lround(static_cast<double>(d.box.y - firstLine) / step));
            const cv::Point off(d.box.x - gridLeft - across * step, d.box.y - firstLine - down * step);
            if (across < 0 || across >= kGridColumns || down < 0 || down >= lines || std::abs(off.x) > tolerance ||
                std::abs(off.y) > tolerance) {
                continue;
            }
            const int place = down * kGridColumns + across;
            taken[static_cast<size_t>(place)] = true;
            last = std::max(last, place);
            offsets.push_back(off);
        }
        cv::Point shift(0, 0);
        if (!offsets.empty()) {
            std::ranges::nth_element(offsets, offsets.begin() + static_cast<std::ptrdiff_t>(offsets.size() / 2), {}, &cv::Point::x);
            shift.x = offsets[offsets.size() / 2].x;
            std::ranges::nth_element(offsets, offsets.begin() + static_cast<std::ptrdiff_t>(offsets.size() / 2), {}, &cv::Point::y);
            shift.y = offsets[offsets.size() / 2].y;
        }
        const auto placeBox = [&](int place) {
            return cv::Rect(gridLeft + shift.x + (place % kGridColumns) * step, firstLine + shift.y + (place / kGridColumns) * step, icon, icon);
        };
        std::vector<Detection> filled;
        const auto alreadyHas = [&](const std::string& id) {
            return std::ranges::find(own, id, &Detection::itemId) != own.end() || std::ranges::find(filled, id, &Detection::itemId) != filled.end();
        };
        const auto look = [&](int place) {
            const cv::Rect box = placeBox(place);
            return matchCell(frameBgr, cv::Rect(box.x - slack, box.y - slack, box.width + 2 * slack, box.height + 2 * slack));
        };

        // Before the last hit: there has to be an item.
        for (int place = 0; place < last; ++place) {
            if (taken[static_cast<size_t>(place)]) {
                continue;
            }
            const CellMatch m = look(place);
            if (m.best && m.score >= kGapMinScore && m.score - m.runnerUp >= kGapMinLead && !alreadyHas(m.best->id)) {
                filled.push_back({m.best->id, m.box, m.score, static_cast<int>(c)});
            } else {
                filled.push_back({kUnknownItem, placeBox(place), m.score, static_cast<int>(c)});
            }
        }
        // More places nothing fits than things recognised: rather a stray hit far down the column, than
        // a player whose items all went unrecognised.
        const auto unknown = std::ranges::count(filled, std::string(kUnknownItem), &Detection::itemId);
        if (unknown > std::ssize(own) + std::ssize(filled) - unknown) {
            std::erase_if(filled, [](const Detection& d) { return d.itemId == kUnknownItem; });
        }
        // After it: only what clearly is an item, and the list ends at the first place that is not.
        for (int place = last + 1; place < static_cast<int>(taken.size()); ++place) {
            const CellMatch m = look(place);
            const bool sure = m.best && m.score >= kNextMinScore && m.score - m.runnerUp >= kNextMinLead &&
                              (!m.best->activeLook || m.activeLabel) && !alreadyHas(m.best->id);
            if (!sure) {
                break;
            }
            filled.push_back({m.best->id, m.box, m.score, static_cast<int>(c)});
        }
        found.insert(found.end(), std::make_move_iterator(filled.begin()), std::make_move_iterator(filled.end()));
    }
    hits.insert(hits.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
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
