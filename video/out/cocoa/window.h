/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#import <Cocoa/Cocoa.h>
#import "video/out/cocoa/mpvadapter.h"

@interface MpvVideoWindow : NSWindow <NSWindowDelegate>
@property(nonatomic, retain) MpvCocoaAdapter *adapter;
- (void)setFullScreen:(BOOL)willBeFullscreen;
- (BOOL)canBecomeKeyWindow;
- (BOOL)canBecomeMainWindow;
- (void)mulSize:(float)multiplier;
- (void)setCenteredContentSize:(NSSize)newSize;

- (void)queueNewVideoSize:(NSSize)newSize;
- (void)dispatchNewVideoSize;

// This really needs to use KVO
- (void)didChangeFullScreenState;
@end