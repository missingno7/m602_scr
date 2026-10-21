// macOS screen saver shell: one view per screen, each running the saver on
// its own thread, plus the settings sheet.
#import <Cocoa/Cocoa.h>
#import <ScreenSaver/ScreenSaver.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

#include "posixmachine.h"
#include "renderer.h"
#include "saver.h"

namespace {

NSString* const kModule = @"com.missingno7.M602Saver";

struct Settings {
    int speed = 100;  // percent of the original timer rate
    RenderSettings render;
    bool fixed80 = false;
};

ScreenSaverDefaults* Defaults() {
    ScreenSaverDefaults* d = [ScreenSaverDefaults defaultsForModuleWithName:kModule];
    [d registerDefaults:@{@"Speed": @100, @"Color": @(kOriginalTextColor), @"Scanlines": @0, @"Fixed80x25": @NO}];
    return d;
}

Settings LoadSettings() {
    ScreenSaverDefaults* d = Defaults();
    Settings s;
    s.speed = std::clamp(int([d integerForKey:@"Speed"]), 10, 400);
    s.render.color = uint32_t([d integerForKey:@"Color"]) & 0xFFFFFF;
    s.render.scanlines = std::clamp(int([d integerForKey:@"Scanlines"]), 0, 2);
    s.fixed80 = [d boolForKey:@"Fixed80x25"];
    return s;
}

void SaveSettings(const Settings& s) {
    ScreenSaverDefaults* d = Defaults();
    [d setInteger:s.speed forKey:@"Speed"];
    [d setInteger:s.render.color forKey:@"Color"];
    [d setInteger:s.render.scanlines forKey:@"Scanlines"];
    [d setBool:s.fixed80 forKey:@"Fixed80x25"];
    [d synchronize];
}

NSColor* ToColor(uint32_t rgb) {
    return [NSColor colorWithSRGBRed:(rgb >> 16 & 0xFF) / 255.0
                               green:(rgb >> 8 & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1];
}

uint32_t FromColor(NSColor* color) {
    NSColor* c = [color colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    auto byte = [](CGFloat v) { return uint32_t(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
    return byte(c.redComponent) << 16 | byte(c.greenComponent) << 8 | byte(c.blueComponent);
}

}  // namespace

// Double-clicking the speed slider's knob resets it to 100 %.
@interface M602SpeedSlider : NSSlider
@end

@implementation M602SpeedSlider
- (void)mouseDown:(NSEvent*)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSRect knob = [(NSSliderCell*)self.cell knobRectFlipped:self.isFlipped];
    if (event.clickCount == 2 && NSPointInRect(p, knob)) {
        self.integerValue = 100;
        [self sendAction:self.action to:self.target];
        return;
    }
    [super mouseDown:event];
}
@end

@interface M602SaverView : ScreenSaverView
@end

@implementation M602SaverView {
    std::unique_ptr<PosixMachine> _machine;
    std::thread _thread;
    Renderer _renderer;
    std::vector<uint16_t> _cells;
    NSWindow* _sheet;
    M602SpeedSlider* _speed;
    NSTextField* _speedLabel;
    NSColorWell* _color;
    NSPopUpButton* _scanlines;
    NSPopUpButton* _textArea;
}

- (instancetype)initWithFrame:(NSRect)frame isPreview:(BOOL)isPreview {
    if ((self = [super initWithFrame:frame isPreview:isPreview])) {
        self.animationTimeInterval = 1 / 60.0;
        // Since macOS 14 the saver process may keep running after the screen
        // saver is dismissed; stop with it.
        [NSDistributedNotificationCenter.defaultCenter addObserver:self
                                                          selector:@selector(willStop:)
                                                              name:@"com.apple.screensaver.willstop"
                                                            object:nil];
    }
    return self;
}

- (void)dealloc {
    [NSDistributedNotificationCenter.defaultCenter removeObserver:self];
    [self stopSaver];
}

- (void)willStop:(NSNotification*)note {
    [self stopSaver];
    if (!self.isPreview) exit(0);
}

- (NSSize)pixelSize {
    NSRect r = [self convertRectToBacking:self.bounds];
    return NSMakeSize(std::max(1.0, r.size.width), std::max(1.0, r.size.height));
}

- (void)configureRenderer {
    NSSize px = self.pixelSize;
    _renderer.Configure(int(px.width), int(px.height), _machine->screen.ScreenWidth, _machine->screen.ScreenHeight,
                        LoadSettings().render);
    _machine->framePending = true;
}

- (void)startSaver {
    if (_machine) return;
    Settings s = LoadSettings();
    _machine = std::make_unique<PosixMachine>(s.speed);
    NSSize px = self.pixelSize;
    // The wider text buffer is the display adaptation; the saver's own
    // boundary logic (MaxX = ScreenWidth - 11) uses whatever width it gets.
    _machine->screen.ScreenWidth = uint8_t(s.fixed80 ? 80 : TextColumnsForDisplay(int(px.width), int(px.height)));
    _machine->screen.ScreenHeight = 25;
    _cells.resize(size_t(_machine->screen.ScreenWidth) * _machine->screen.ScreenHeight);
    [self configureRenderer];
    PosixMachine* m = _machine.get();
    _thread = std::thread([m] { ScreenSaver(*m); });
}

- (void)stopSaver {
    if (!_machine) return;
    _machine->quit = true;
    if (_thread.joinable()) _thread.join();
    _machine.reset();
}

- (void)startAnimation {
    [super startAnimation];
    [self startSaver];
}

- (void)stopAnimation {
    [self stopSaver];
    [super stopAnimation];
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    if (_machine) [self configureRenderer];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (_machine) [self configureRenderer];
}

- (void)animateOneFrame {
    if (!_machine || !_machine->framePending.exchange(false)) return;
    {
        std::lock_guard<std::mutex> g(_machine->screen.lock);
        std::copy_n(_machine->screen.mem, _cells.size(), _cells.begin());
    }
    _renderer.Update(_cells.data());
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)rect {
    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
    CGContextFillRect(ctx, self.bounds);
    if (!_machine) return;
    size_t w = size_t(_renderer.Width()), h = size_t(_renderer.Height());
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef data = CGDataProviderCreateWithData(nullptr, _renderer.Pixels(), w * h * 4, nullptr);
    CGImageRef image = CGImageCreate(w, h, 8, 32, w * 4, space,
                                     CGBitmapInfo(kCGBitmapByteOrder32Little) | CGBitmapInfo(kCGImageAlphaNoneSkipFirst), data, nullptr,
                                     false, kCGRenderingIntentDefault);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextDrawImage(ctx, self.bounds, image);
    CGImageRelease(image);
    CGDataProviderRelease(data);
    CGColorSpaceRelease(space);
}

// ------------------------------------------------------------ settings sheet

- (BOOL)hasConfigureSheet {
    return YES;
}

- (NSWindow*)configureSheet {
    if (!_sheet) [self buildSheet];
    [self showSettings:LoadSettings()];
    return _sheet;
}

- (void)buildSheet {
    _sheet = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 380, 214)
                                         styleMask:NSWindowStyleMaskTitled
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
    NSView* v = _sheet.contentView;
    auto label = [&](NSString* text, CGFloat y) {
        NSTextField* l = [NSTextField labelWithString:text];
        l.frame = NSMakeRect(20, y, 110, 20);
        [v addSubview:l];
        return l;
    };
    auto popup = [&](NSArray<NSString*>* items, CGFloat y) {
        NSPopUpButton* p = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(136, y - 3, 224, 26) pullsDown:NO];
        [p addItemsWithTitles:items];
        [v addSubview:p];
        return p;
    };
    auto button = [&](NSString* title, CGFloat x, SEL action) {
        NSButton* b = [NSButton buttonWithTitle:title target:self action:action];
        b.frame = NSMakeRect(x, 16, 100, 32);
        [v addSubview:b];
        return b;
    };

    _speedLabel = label(@"Speed:", 170);
    _speed = [[M602SpeedSlider alloc] initWithFrame:NSMakeRect(136, 168, 224, 24)];
    _speed.minValue = 10;
    _speed.maxValue = 400;
    _speed.target = self;
    _speed.action = @selector(speedChanged:);
    [v addSubview:_speed];

    label(@"Color:", 134);
    _color = [[NSColorWell alloc] initWithFrame:NSMakeRect(136, 130, 60, 26)];
    [v addSubview:_color];
    NSButton* original = [NSButton buttonWithTitle:@"Original" target:self action:@selector(originalColor:)];
    original.frame = NSMakeRect(204, 127, 100, 32);
    [v addSubview:original];

    label(@"Scanlines:", 98);
    _scanlines = popup(@[@"Off", @"Subtle", @"Strong"], 98);
    label(@"Text area:", 64);
    _textArea = popup(@[@"Widescreen", @"80×25"], 64);

    button(@"Defaults", 14, @selector(defaults:));
    button(@"Cancel", 176, @selector(cancel:)).keyEquivalent = @"\e";
    button(@"OK", 266, @selector(ok:)).keyEquivalent = @"\r";
}

- (void)showSettings:(const Settings&)s {
    _speed.integerValue = s.speed;
    [self speedChanged:_speed];
    _color.color = ToColor(s.render.color);
    [_scanlines selectItemAtIndex:s.render.scanlines];
    [_textArea selectItemAtIndex:s.fixed80 ? 1 : 0];
}

- (void)speedChanged:(id)sender {
    _speedLabel.stringValue = [NSString stringWithFormat:@"Speed: %ld %%", long(_speed.integerValue)];
}

- (void)originalColor:(id)sender {
    _color.color = ToColor(kOriginalTextColor);
}

- (void)defaults:(id)sender {
    [self showSettings:Settings{}];
}

- (void)closeSheet {
    [NSColorPanel.sharedColorPanel close];
    if (_sheet.sheetParent)
        [_sheet.sheetParent endSheet:_sheet];
    else
        [NSApp endSheet:_sheet];
}

- (void)cancel:(id)sender {
    [self closeSheet];
}

- (void)ok:(id)sender {
    Settings s;
    s.speed = int(_speed.integerValue);
    s.render.color = FromColor(_color.color);
    s.render.scanlines = int(_scanlines.indexOfSelectedItem);
    s.fixed80 = _textArea.indexOfSelectedItem == 1;
    SaveSettings(s);
    [self closeSheet];
    if (_machine) {  // restart with the new settings
        [self stopSaver];
        [self startSaver];
    }
}

@end
