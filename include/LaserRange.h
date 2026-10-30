#pragma once
// How far the mining laser reaches, off the LASER RANGE gauge of the mining
// HUD.
//
// Left of the crosshair the HUD draws an arc for the laser's range; a white
// triangle rides it, pointing at the arc, with the beam's length in whole
// metres beside it - green, yellow, orange or red with the band it is in,
// the figure turned along the line from the triangle away from the arc. The
// OPTIMAL and MAX marks on the arc are the fitted laser's own ranges and
// move with the laser; the triangle's place does not - the same metres stand
// at the same place for a Helix II and a Hofstede-S2 - but it is placed from
// the scan panel's labels, a long way off, and stands where it does to a few
// metres only. So the figure is what is read, numeral by numeral, and the
// triangle's place only has to agree with it.

#include <opencv2/core.hpp>

#include <string>

namespace SCMiner {

struct ScanPanel;

struct LaserRangeLook {
    int metres = 0;               // 0: not read for sure
    cv::Point2f pointer{-1, -1};  // the triangle's centre, in the frame
    float tilt = 0;               // degrees the figure stands turned, clockwise
    float least = 0;              // the least likeness of its numerals, 0..1
    float lead = 0;               // ...and the least lead over the next likeliest
    std::string note;             // for the tools: what was read, or why not
};

// The gauge of the HUD whose scan panel was found in this frame: placed from
// the panel's labels and their size. Only on HUDs whose gauge is known - see
// LaserRange.cpp; a MOLE's pilot seat has no laser and no gauge.
// toneMapped: the frame is an HDR desktop brought down to eight bits by
// DxgiCapture (ToneMap.h), where the HUD's white sits near 190 instead of
// 255; the gauge is then read off what an SDR capture of it holds.
LaserRangeLook readLaserRange(const cv::Mat& frame, const ScanPanel& panel, bool toneMapped = false);

}  // namespace SCMiner
