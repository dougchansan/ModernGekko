// A Game Boy Advance screen moved out of the game window into a window of its
// own (right-click a GBA screen, "Move to its own window"): the macOS twin of
// gba_window_win32.cpp. Plain AppKit; runs on the window (main) thread.
#include "gba_screen_internal.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "Core/NetPlay/NetPlayProto.h"

namespace
{
using moderngekko::gba_screen::View;
namespace gba = moderngekko::gba_screen;

void BackToGame(int port)
{
  View view = gba::GetSettings().views[port];
  view.detached = false;
  gba::detail::SetView(port, view, true);
}

NSColor* PlayerColor(int port)
{
  static const CGFloat rgb[4][3] = {
      {222, 58, 58}, {58, 112, 232}, {232, 186, 34}, {52, 176, 88}};
  return [NSColor colorWithSRGBRed:rgb[port][0] / 255.0
                             green:rgb[port][1] / 255.0
                              blue:rgb[port][2] / 255.0
                             alpha:1.0];
}
}  // namespace

// Owns one GBA window: its menu, its close button.
@interface MGGbaWindowController : NSObject <NSWindowDelegate>
@property(assign) int port;
- (void)backToGame:(id)sender;
- (void)reset:(id)sender;
- (void)toggleNumbers:(id)sender;
- (void)setOpacity:(id)sender;
@end

@implementation MGGbaWindowController
- (BOOL)windowShouldClose:(NSWindow*)sender
{
  // Closing the window puts the screen back in the game window.
  BackToGame(self.port);
  return NO;
}
- (void)backToGame:(id)sender
{
  BackToGame(self.port);
}
- (void)reset:(id)sender
{
  gba::ResetGba(self.port);
}
- (void)toggleNumbers:(id)sender
{
  gba::detail::SetShowNumbers(!gba::GetSettings().show_numbers);
}
- (void)setOpacity:(id)sender
{
  View view = gba::GetSettings().views[self.port];
  view.opacity = static_cast<int>([sender tag]);
  gba::detail::SetView(self.port, view, false);
}
@end

// The screen itself; right-click for the window's menu.
@interface MGGbaView : NSImageView
@property(weak) MGGbaWindowController* controller;
@end

@implementation MGGbaView
- (NSMenu*)menuForEvent:(NSEvent*)event
{
  MGGbaWindowController* target = self.controller;
  const gba::Settings settings = gba::GetSettings();
  const View& view = settings.views[target.port];
  NSMenu* menu = [[NSMenu alloc] initWithTitle:@"GBA"];
  [menu setAutoenablesItems:NO];
  NSMenuItem* back = [menu addItemWithTitle:@"Move back into the game window"
                                     action:@selector(backToGame:)
                              keyEquivalent:@""];
  [back setTarget:target];
  NSMenuItem* reset = [menu addItemWithTitle:@"Reset GBA (off and on)"
                                      action:@selector(reset:)
                               keyEquivalent:@""];
  [reset setTarget:target];
  [reset setEnabled:!NetPlay::IsNetPlayRunning()];
  [menu addItem:[NSMenuItem separatorItem]];
  NSMenuItem* numbers = [menu addItemWithTitle:@"Show player numbers"
                                        action:@selector(toggleNumbers:)
                                 keyEquivalent:@""];
  [numbers setTarget:target];
  [numbers setState:settings.show_numbers ? NSControlStateValueOn : NSControlStateValueOff];
  NSMenu* opacity = [[NSMenu alloc] initWithTitle:@"Opacity"];
  for (int percent = 100; percent >= 10; percent -= 10)
  {
    NSMenuItem* item =
        [opacity addItemWithTitle:[NSString stringWithFormat:@"%d%%", percent]
                           action:@selector(setOpacity:)
                    keyEquivalent:@""];
    [item setTarget:target];
    [item setTag:percent];
    [item setState:view.opacity == percent ? NSControlStateValueOn : NSControlStateValueOff];
  }
  NSMenuItem* holder = [menu addItemWithTitle:@"Opacity" action:nil keyEquivalent:@""];
  [holder setSubmenu:opacity];
  return menu;
}
@end

namespace moderngekko::gba_screen::detail
{
namespace
{
struct Window
{
  NSWindow* window = nil;
  MGGbaView* view = nil;
  NSTextField* badge = nil;
  MGGbaWindowController* controller = nil;
  std::vector<std::uint32_t> bgra;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint64_t frame = 0;
  std::uint64_t shown = 0;
};

std::array<Window, 4> s_windows;

void Create(int port, Window& w)
{
  w.controller = [[MGGbaWindowController alloc] init];
  w.controller.port = port;
  const NSRect frame = NSMakeRect(0, 0, 480, 320);  // twice the GBA's size
  w.window = [[NSWindow alloc]
      initWithContentRect:frame
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                          NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
  [w.window setReleasedWhenClosed:NO];
  [w.window setTitle:[NSString stringWithFormat:@"Game Boy Advance - Player %d", port + 1]];
  [w.window setDelegate:w.controller];
  [w.window setContentAspectRatio:NSMakeSize(3, 2)];
  [w.window setBackgroundColor:[NSColor blackColor]];

  w.view = [[MGGbaView alloc] initWithFrame:frame];
  w.view.controller = w.controller;
  [w.view setImageScaling:NSImageScaleProportionallyUpOrDown];
  [w.view setWantsLayer:YES];
  w.view.layer.magnificationFilter = kCAFilterNearest;  // sharp pixels
  [w.view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
  [w.window setContentView:w.view];

  w.badge = [NSTextField labelWithString:[NSString stringWithFormat:@"P%d", port + 1]];
  [w.badge setFont:[NSFont boldSystemFontOfSize:16]];
  [w.badge setTextColor:[NSColor whiteColor]];
  [w.badge setDrawsBackground:YES];
  [w.badge setBackgroundColor:PlayerColor(port)];
  [w.badge setFrameOrigin:NSMakePoint(8, frame.size.height - 30)];
  [w.badge setAutoresizingMask:NSViewMinYMargin];
  [w.view addSubview:w.badge];

  [w.window center];
  [w.window orderFront:nil];
}

void Destroy(Window& w)
{
  [w.window setDelegate:nil];
  [w.window orderOut:nil];
  w = {};
}

NSImage* MakeImage(const Window& w)
{
  // CopyFrame hands out BGRA, which is 32-bit little-endian ARGB.
  NSData* data = [NSData dataWithBytes:w.bgra.data() length:w.bgra.size() * 4];
  CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
  CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
  CGImageRef image =
      CGImageCreate(w.width, w.height, 8, 32, w.width * 4, space,
                    static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst) | kCGBitmapByteOrder32Little, provider, nullptr,
                    false, kCGRenderingIntentDefault);
  NSImage* result = [[NSImage alloc] initWithCGImage:image size:NSMakeSize(w.width, w.height)];
  CGImageRelease(image);
  CGColorSpaceRelease(space);
  CGDataProviderRelease(provider);
  return result;
}
}  // namespace

void TickWindows()
{
  @autoreleasepool
  {
    const Settings settings = GetSettings();
    for (int port = 0; port < 4; ++port)
    {
      Window& w = s_windows[port];
      const bool running = CopyFrame(port, &w.bgra, &w.width, &w.height, &w.frame);
      if (!running || !settings.views[port].detached)
      {
        if (w.window)
          Destroy(w);
        continue;
      }
      if (!w.window)
        Create(port, w);
      [w.window setAlphaValue:settings.views[port].opacity / 100.0];
      [w.badge setHidden:!settings.show_numbers];
      if (w.shown != w.frame && w.width && w.height)
      {
        w.shown = w.frame;
        [w.view setImage:MakeImage(w)];
      }
    }
  }
}
}  // namespace moderngekko::gba_screen::detail
