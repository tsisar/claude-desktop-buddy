#pragma once
#include <stdint.h>

// Pixel-art SVG animation player. The manifest lists one .svg PER FRAME:
//
//   "idle": ["idle_0.svg", "idle_1.svg", ...]
//
// Each file is one fully-composed frame; the device steps through them at
// a fixed interval (manifest "svgDelay" ms per frame, default 200) and
// loops. However many files the array lists is how many frames play — a
// single file is simply a static image. Paths with fill-opacity="0" are
// skipped (editors export hidden siblings; only visible content is the
// frame).
//
// Two shape dialects are understood, auto-detected per element:
//   • <path d="M x,y L... Z"> — unions of axis-aligned rects, one per
//     M...Z subpath (the layered-editor export)
//   • <rect x y width height fill> — one rect per pixel cell (the
//     artboard export)
// Everything parses into a few KB of flat tables; scaling uses the tight
// content bounding box across ALL frames (artboards park a small sprite
// on a huge blank canvas), so frames stay aligned and the buddy fills
// the requested box. A frame draw is a handful of fillRect() calls.

void svgAnimSeqBegin();                       // frees any prior anim
bool svgAnimSeqAddFrame(const char* path);    // parse one file as the next frame
bool svgAnimSeqEnd(uint16_t frameDelayMs);    // finalize; false if nothing loaded

void svgAnimClose();                    // free tables; safe to call when not loaded
bool svgAnimLoaded();

// Fit-and-center the content box into this box (canvas pixels). Call after
// load and again if the layout area changes; rendering without a placement
// uses the full canvas.
void svgAnimPlace(int x, int y, int w, int h);

// Milliseconds until the next frame switch (≥1; the idle 200 for static
// single-frame states). Lets the render loop schedule draws exactly on
// frame boundaries instead of polling.
uint32_t svgAnimNextEventMs();

// Draw the frame for the current point in the cycle (time-indexed, so a
// slow render cadence skips frames but keeps the animation pace). The
// home renderer clears the canvas every pass, so this always paints: bg
// fills the placed content rect, then the frame stacks on top.
void svgAnimTick(uint16_t bgColor);
