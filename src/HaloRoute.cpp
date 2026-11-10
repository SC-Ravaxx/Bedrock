#include "HaloRoute.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace SCMiner {

// The Cornerstone survey's "Detailed Band Distances Data" and "Detailed
// Height and Width Data": km from the Stanton marker; the height measured at
// each band's densest point
const HaloBand HALO_BANDS[HALO_BAND_COUNT] = {
    {1, 19673000, 19715000, 19702000, 615, 635},
    {2, 19815000, 19914000, 19857000, 2063, 2078},
    {3, 19914000, 20071000, 19995000, 4035, 4058},
    {4, 20129000, 20230000, 20168000, 2897, 2926},
    {5, 20230000, 20407000, 20320000, 4990, 5006},
    {6, 20407000, 20540000, 20471000, 4991, 5008},
    {7, 20540000, 20750000, 20662000, 4989, 5007},
    {8, 20793000, 20968000, 20881000, 3475, 3498},
    {9, 21046000, 21132000, 21082000, 2389, 2411},
    {10, 21159000, 21299000, 21207000, 2006, 2010},
};

const HaloPlace HALO_PLACES[HALO_PLACE_COUNT] = {
    {"Hurston", 0},  {"Arial", 0},    {"Aberdeen", 0}, {"Magda", 0},    {"Ita", 0},
    {"Crusader", 1}, {"Cellin", 1},   {"Daymar", 1},   {"Yela", 1},
    {"ArcCorp", 2},  {"Lyria", 2},    {"Wala", 2},
    {"microTech", 3}, {"Calliope", 3}, {"Clio", 3},    {"Euterpe", 3},
    {"HUR-L1", 4},   {"HUR-L2", 4},   {"HUR-L3", 4},   {"HUR-L4", 4},   {"HUR-L5", 4},
    {"CRU-L1", 5},   {"CRU-L2", 5},   {"CRU-L3", 5},   {"CRU-L4", 5},   {"CRU-L5", 5},
    {"ARC-L1", 6},   {"ARC-L2", 6},   {"ARC-L3", 6},   {"ARC-L4", 6},   {"ARC-L5", 6},
    {"MIC-L1", 7},   {"MIC-L2", 7},   {"MIC-L3", 7},   {"MIC-L4", 7},   {"MIC-L5", 7},
};

bool HaloPass::overIt() const {
    return band >= 1 && band <= HALO_BAND_COUNT && heightKm > HALO_BANDS[band - 1].aboveKm;
}

bool HaloPass::underIt() const {
    return band >= 1 && band <= HALO_BAND_COUNT && heightKm < -HALO_BANDS[band - 1].belowKm;
}

namespace {

// The route as a line in km: the point at t (0 the start, 1 the end) is
// from + t * along, and its distance from the marker squared is
// a t^2 + b t + c
struct Line {
    double fx, fy, fz, dx, dy, dz;
    double a, b, c;
    double radiusAt(double t) const {
        const double x = fx + t * dx, y = fy + t * dy, z = fz + t * dz;
        return std::sqrt(x * x + y * y + z * z);
    }
    // Where on the line its distance from the marker is `radius`, coming
    // in and going out again; false where it never comes that near
    bool meets(double radius, double& in, double& out) const {
        const double cc = c - radius * radius;
        const double disc = b * b - 4 * a * cc;
        if (a <= 0 || disc < 0) return false;
        const double root = std::sqrt(disc);
        in = (-b - root) / (2 * a);
        out = (-b + root) / (2 * a);
        return true;
    }
};

}  // namespace

double routeKm(const NavPoint& from, const NavPoint& to) {
    const double dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz) / 1000.0;
}

std::vector<HaloPass> haloPasses(const NavPoint& from, const NavPoint& to) {
    std::vector<HaloPass> out;
    if (from.system != "Stanton" || to.system != "Stanton") return out;
    Line line;
    line.fx = from.x / 1000.0;
    line.fy = from.y / 1000.0;
    line.fz = from.z / 1000.0;
    line.dx = to.x / 1000.0 - line.fx;
    line.dy = to.y / 1000.0 - line.fy;
    line.dz = to.z / 1000.0 - line.fz;
    line.a = line.dx * line.dx + line.dy * line.dy + line.dz * line.dz;
    line.b = 2 * (line.fx * line.dx + line.fy * line.dy + line.fz * line.dz);
    line.c = line.fx * line.fx + line.fy * line.fy + line.fz * line.fz;
    if (line.a <= 0) return out;
    const double length = std::sqrt(line.a);
    for (const HaloBand& band : HALO_BANDS) {
        // Inside the band is inside its outer edge and outside its inner
        // one: one stretch of the line, or two where the line passes
        // through the hole in the middle
        double o1 = 0, o2 = 0;
        if (!line.meets(band.outerKm, o1, o2)) continue;
        double stretches[2][2] = {{o1, o2}, {0, 0}};
        int count = 1;
        double i1 = 0, i2 = 0;
        if (line.meets(band.innerKm, i1, i2)) {
            stretches[0][1] = i1;
            stretches[1][0] = i2;
            stretches[1][1] = o2;
            count = 2;
        }
        // ...the first of them on the route itself
        double t0 = 0, t1 = 0;
        bool on = false;
        for (int k = 0; k < count && !on; k++) {
            t0 = (std::max)(0.0, stretches[k][0]);
            t1 = (std::min)(1.0, stretches[k][1]);
            on = t1 > t0;
        }
        if (!on) continue;
        // Where to leave quantum travel: where the route meets the band's
        // densest distance, the first time - or where it comes nearest to
        // it, at an end of the stretch or where the line passes closest to
        // the marker
        double td = -1, d1 = 0, d2 = 0;
        if (line.meets(band.densestKm, d1, d2)) {
            if (d1 >= t0 && d1 <= t1) td = d1;
            else if (d2 >= t0 && d2 <= t1) td = d2;
        }
        if (td < 0) {
            td = t0;
            double gap = std::abs(line.radiusAt(t0) - band.densestKm);
            for (double t : {t1, std::clamp(-line.b / (2 * line.a), t0, t1)}) {
                const double g = std::abs(line.radiusAt(t) - band.densestKm);
                if (g < gap) {
                    gap = g;
                    td = t;
                }
            }
        }
        HaloPass pass;
        pass.band = band.number;
        pass.enterKm = length * (1 - t0);
        pass.leaveKm = length * (1 - t1);
        pass.exitKm = length * (1 - td);
        pass.heightKm = line.fz + td * line.dz;
        out.push_back(pass);
    }
    return out;
}

bool runsThroughHalo(const NavPoint& from, const NavPoint& to) {
    const std::vector<HaloPass> passes = haloPasses(from, to);
    return bestHaloPass(passes) != nullptr;
}

std::vector<const HaloPlace*> haloDestinations(const std::string& from) {
    std::vector<const HaloPlace*> out;
    NavPoint a;
    if (from.empty() || !navPoint(from, "Stanton", a)) return out;
    for (const HaloPlace& place : HALO_PLACES) {
        NavPoint b;
        if (from == place.name || !navPoint(place.name, "Stanton", b)) continue;
        if (runsThroughHalo(a, b)) out.push_back(&place);
    }
    return out;
}

std::string nearestHaloDestination(const std::string& from) {
    NavPoint a;
    if (from.empty() || !navPoint(from, "Stanton", a)) return std::string();
    const HaloPlace* nearest = nullptr;
    bool densest = false;
    double km = 0;
    for (const HaloPlace* place : haloDestinations(from)) {
        NavPoint b;
        navPoint(place->name, "Stanton", b);
        const std::vector<HaloPass> passes = haloPasses(a, b);
        const bool five = bestHaloPass(passes)->band == HALO_DENSEST_BAND;
        const double way = routeKm(a, b);
        // through the densest band first, then the shortest way
        if (!nearest || (five && !densest) || (five == densest && way < km)) {
            nearest = place;
            densest = five;
            km = way;
        }
    }
    return nearest ? std::string(nearest->name) : std::string();
}

const HaloPass* bestHaloPass(const std::vector<HaloPass>& passes) {
    const HaloPass* best = nullptr;
    for (const HaloPass& pass : passes) {
        if (!pass.inIt()) continue;
        if (pass.band == HALO_DENSEST_BAND) return &pass;
        if (!best || pass.enterKm - pass.leaveKm > best->enterKm - best->leaveKm) best = &pass;
    }
    return best;
}

}  // namespace SCMiner
