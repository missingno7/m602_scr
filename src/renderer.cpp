#include "renderer.h"

#include <algorithm>
#include <cmath>

#include "font.h"

namespace {

// Default VGA text palette: DAC levels 0, 21, 42, 63 = 00h, 55h, AAh, FFh.
const uint32_t kPalette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

// 720x400 shown as 4:3: each raster pixel is (4/720) / (3/400) = 20/27 as wide as tall.
constexpr double kPixelAspect = 20.0 / 27.0;
constexpr int kCellW = 9, kCellH = 16;

// Scanline depth at the boundary between two raster lines (off, subtle, strong).
constexpr double kScanlineDepth[3] = {0.0, 0.3, 0.6};

float ToLinear(uint32_t rgb, int channel) {
    double v = ((rgb >> (16 - 8 * channel)) & 0xFF) / 255.0;
    return float(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
}

// Linear light -> 8-bit sRGB.
constexpr int kLutSize = 16384;
struct EncodeLut {
    uint8_t v[kLutSize + 1];
    EncodeLut() {
        for (int i = 0; i <= kLutSize; ++i) {
            double x = double(i) / kLutSize;
            double s = x <= 0.0031308 ? x * 12.92 : 1.055 * std::pow(x, 1 / 2.4) - 0.055;
            v[i] = uint8_t(std::lround(std::clamp(s, 0.0, 1.0) * 255));
        }
    }
    uint32_t operator()(float x) const { return v[std::clamp(int(x * kLutSize + 0.5f), 0, kLutSize)]; }
};
const EncodeLut kEncode;

// Integral from 0 to y (in raster lines) of the scanline profile
// f(y) = 1 - d * (1 + cos(2 pi y)) / 2: dimmest between lines, full at their centres.
double ScanlineIntegral(double y, double d) {
    const double pi = 3.14159265358979323846;
    return y * (1 - d / 2) - d / 2 * std::sin(2 * pi * y) / (2 * pi);
}

}  // namespace

int TextColumnsForDisplay(int widthPx, int heightPx) {
    if (widthPx <= 0 || heightPx <= 0) return 80;
    // 25 rows of 16 lines at 20:27 pixels: cols * 9 * 20 / (400 * 27) = w / h.
    int cols = int(std::floor(60.0 * widthPx / heightPx + 0.5));
    // ScreenWidth is a Byte; below 41 columns the fixed start column 30 would
    // lie outside the movement range.
    return std::clamp(cols, 41, 255);
}

void Renderer::Configure(int outWidth, int outHeight, int cols, int rows, const RenderSettings& settings) {
    outW_ = std::max(outWidth, 1);
    outH_ = std::max(outHeight, 1);
    cols_ = cols;
    rows_ = rows;
    srcW_ = cols * kCellW;
    srcH_ = rows * kCellH;
    out_.assign(size_t(outW_) * outH_, 0);
    raster_.assign(size_t(srcW_) * srcH_, 0);
    cells_.assign(size_t(cols) * rows, 0);
    valid_ = false;
    for (int i = 0; i < 16; ++i)
        for (int c = 0; c < 3; ++c) palette_[i][c] = ToLinear(i == 7 ? settings.color : kPalette[i], c);

    // Largest centred rectangle with the original physical proportions.
    double aspect = srcW_ * kPixelAspect / srcH_;
    if (double(outW_) / outH_ > aspect) {
        ch_ = outH_;
        cw_ = std::clamp(int(std::lround(outH_ * aspect)), 1, outW_);
    } else {
        cw_ = outW_;
        ch_ = std::clamp(int(std::lround(outW_ / aspect)), 1, outH_);
    }
    cx_ = (outW_ - cw_) / 2;
    cy_ = (outH_ - ch_) / 2;
    BuildTaps(xTaps_, cw_, srcW_, 0.0);
    BuildTaps(yTaps_, ch_, srcH_, kScanlineDepth[std::clamp(settings.scanlines, 0, 2)]);
}

// Area sampling: each output pixel takes every raster pixel it overlaps,
// weighted by the overlap, so raster pixels keep equal size on screen at any
// fractional scale.  Vertical weights also integrate the scanline profile.
void Renderer::BuildTaps(Taps& taps, int outSize, int srcSize, double scanlineDepth) {
    taps.first.assign(1, 0);
    taps.index.clear();
    taps.weight.clear();
    double scale = double(srcSize) / outSize;
    for (int o = 0; o < outSize; ++o) {
        double a = o * scale, b = (o + 1) * scale;
        for (int i = int(a); i < srcSize && i < b; ++i) {
            double lo = std::max(a, double(i)), hi = std::min(b, double(i + 1));
            if (hi <= lo) continue;
            taps.index.push_back(i);
            taps.weight.push_back(
                float((ScanlineIntegral(hi, scanlineDepth) - ScanlineIntegral(lo, scanlineDepth)) / scale));
        }
        taps.first.push_back(int(taps.index.size()));
    }
}

// One character cell of a 9-dot VGA text mode: 8 columns from the font, and
// a 9th that repeats the 8th for C0h..DFh (line graphics) and is blank
// otherwise.  Attribute: foreground bits 0-3, background bits 4-6 (bit 7 is
// blink, unused here).
void Renderer::RasterizeCell(int col, int row, uint16_t cell) {
    uint8_t ch = uint8_t(cell), attr = uint8_t(cell >> 8);
    uint8_t fg = attr & 0x0F, bg = (attr >> 4) & 0x07;
    bool lineGraphics = ch >= 0xC0 && ch <= 0xDF;
    for (int y = 0; y < kCellH; ++y) {
        uint8_t bits = kVgaFont[ch * 16 + y];
        uint8_t* p = &raster_[size_t(row * kCellH + y) * srcW_ + col * kCellW];
        for (int x = 0; x < 8; ++x) p[x] = (bits & (0x80 >> x)) ? fg : bg;
        p[8] = (lineGraphics && (bits & 1)) ? fg : bg;
    }
}

void Renderer::RenderRect(int ox0, int oy0, int ox1, int oy1) {
    for (int oy = oy0; oy < oy1; ++oy) {
        uint32_t* dst = &out_[size_t(cy_ + oy) * outW_ + cx_];
        for (int ox = ox0; ox < ox1; ++ox) {
            float r = 0, g = 0, b = 0;
            for (int ty = yTaps_.first[oy]; ty < yTaps_.first[oy + 1]; ++ty) {
                const uint8_t* line = &raster_[size_t(yTaps_.index[ty]) * srcW_];
                for (int tx = xTaps_.first[ox]; tx < xTaps_.first[ox + 1]; ++tx) {
                    const float* c = palette_[line[xTaps_.index[tx]]];
                    float w = yTaps_.weight[ty] * xTaps_.weight[tx];
                    r += w * c[0];
                    g += w * c[1];
                    b += w * c[2];
                }
            }
            dst[ox] = kEncode(r) << 16 | kEncode(g) << 8 | kEncode(b);
        }
    }
}

std::vector<PixelRect> Renderer::Update(const uint16_t* cells) {
    std::vector<PixelRect> dirty;
    double sx = double(srcW_) / cw_, sy = double(srcH_) / ch_;
    for (int row = 0; row < rows_; ++row) {
        const uint16_t* in = cells + size_t(row) * cols_;
        uint16_t* old = &cells_[size_t(row) * cols_];
        for (int col = 0; col < cols_;) {
            if (valid_ && old[col] == in[col]) {
                ++col;
                continue;
            }
            // A run of changed cells: rasterize it, then redraw every output
            // pixel that can sample it (plus one pixel for rounding).
            int first = col;
            for (; col < cols_ && !(valid_ && old[col] == in[col]); ++col) {
                old[col] = in[col];
                RasterizeCell(col, row, in[col]);
            }
            int ox0 = std::max(int(first * kCellW / sx) - 1, 0);
            int ox1 = std::min(int(std::ceil(col * kCellW / sx)) + 1, cw_);
            int oy0 = std::max(int(row * kCellH / sy) - 1, 0);
            int oy1 = std::min(int(std::ceil((row + 1) * kCellH / sy)) + 1, ch_);
            RenderRect(ox0, oy0, ox1, oy1);
            dirty.push_back({cx_ + ox0, cy_ + oy0, cx_ + ox1, cy_ + oy1});
        }
    }
    valid_ = true;
    return dirty;
}
