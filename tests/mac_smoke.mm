// Loads M602Saver.saver like the system does, runs it for a few seconds and
// writes a frame and the settings sheet to PNG files.
//
//     mac_smoke M602Saver.saver frame.png sheet.png
#import <Cocoa/Cocoa.h>
#import <ScreenSaver/ScreenSaver.h>

static void WritePng(NSView* view, NSString* path) {
    NSBitmapImageRep* rep = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        if (argc != 4) {
            fprintf(stderr, "usage: mac_smoke M602Saver.saver frame.png sheet.png\n");
            return 2;
        }
        [NSApplication sharedApplication];
        NSBundle* bundle = [NSBundle bundleWithPath:@(argv[1])];
        if (![bundle load] || !bundle.principalClass) {
            fprintf(stderr, "cannot load %s\n", argv[1]);
            return 1;
        }
        ScreenSaverView* view = [[bundle.principalClass alloc] initWithFrame:NSMakeRect(0, 0, 1280, 720)
                                                                   isPreview:YES];
        [view startAnimation];
        NSDate* end = [NSDate dateWithTimeIntervalSinceNow:2.5];
        while ([end timeIntervalSinceNow] > 0) {
            [view animateOneFrame];
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1 / 60.0]];
        }
        WritePng(view, @(argv[2]));

        // The boxes are light grey on black: count lit pixels.
        NSBitmapImageRep* rep = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
        [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
        long lit = 0;
        for (NSInteger y = 0; y < rep.pixelsHigh; ++y)
            for (NSInteger x = 0; x < rep.pixelsWide; ++x)
                if ([rep colorAtX:x y:y].redComponent > 0.4) ++lit;
        [view stopAnimation];
        printf("lit pixels: %ld\n", lit);

        NSWindow* sheet = [view configureSheet];
        if (!sheet) {
            fprintf(stderr, "no configure sheet\n");
            return 1;
        }
        WritePng(sheet.contentView, @(argv[3]));
        return lit > 2000 ? 0 : 1;
    }
}
