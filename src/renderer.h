// Displays the emulated text screen: 9x16 VGA character cells (720x400 for
// 80 columns) with the 4:3 geometry of the original CRT.  Purely
// presentational; nothing here feeds back into the simulation.
#pragma once

#include <cstdint>
#include <vector>

// Attribute 7 (light grey) in the default VGA palette.
constexpr uint32_t kOriginalTextColor = 0xAAAAAA;

struct RenderSettings {
    int scanlines = 0;                    // 0 off, 1 subtle, 2 strong
    uint32_t color = kOriginalTextColor;  // 0xRRGGBB used for palette entry 7
};

struct PixelRect {
    int left, top, right, bottom;
};

// Columns that let 25 rows fill a display of this size with the original
// character proportions: 80 on 4:3, about 60 * aspect ratio in general.
int TextColumnsForDisplay(int widthPx, int heightPx);

class Renderer {
public:
    // Output size and text screen size; invalidates everything.
    void Configure(int outWidth, int outHeight, int cols, int rows, const RenderSettings& settings);
    // Redraws the cells (cols*rows words, char | attr << 8) that changed since
    // the last call and returns the output rectangles that changed.
    std::vector<PixelRect> Update(const uint16_t* cells);

    const uint32_t* Pixels() const { return out_.data(); }  // 0x00RRGGBB, top-down
    int Width() const { return outW_; }
    int Height() const { return outH_; }
    PixelRect Content() const { return {cx_, cy_, cx_ + cw_, cy_ + ch_}; }

private:
    struct Taps {  // resampling weights, output pixel -> raster pixels
        std::vector<int> first;  // per output pixel: index of its first tap
        std::vector<int> index;  // raster coordinate
        std::vector<float> weight;
    };
    static void BuildTaps(Taps& taps, int outSize, int srcSize, double scanlineDepth);
    void RasterizeCell(int col, int row, uint16_t cell);
    void RenderRect(int ox0, int oy0, int ox1, int oy1);

    int outW_ = 0, outH_ = 0, cols_ = 0, rows_ = 0;
    int srcW_ = 0, srcH_ = 0;
    int cx_ = 0, cy_ = 0, cw_ = 0, ch_ = 0;  // content rectangle in the output
    float palette_[16][3] = {};  // linear light
    bool valid_ = false;
    std::vector<uint32_t> out_;
    std::vector<uint8_t> raster_;  // colour index per raster pixel
    std::vector<uint16_t> cells_;  // cells currently rasterized
    Taps xTaps_, yTaps_;
};
