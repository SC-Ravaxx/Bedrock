#pragma once
// Which gadget to put on this rock, and what a loadout actually puts out.
//
// Only ONE gadget goes on a deposit - the game's own warning is that more
// "can result in a catastrophic explosion" - so this is a choice among seven
// (six and none), not a search through combinations. That matters: it makes
// the question answerable exactly rather than approximately.
//
// The six are designed so that each wins one axis outright and pays for it
// somewhere else. Measured off their records:
//
//     axis           best        worst        (worse = the cost it charges)
//     instability    BoreMax     Sabir
//     window         WaveShift   Stalwart, OptiMax
//     fill rate      Okunis      WaveShift
//     resistance     Sabir       BoreMax
//     yield          OptiMax     Okunis
//
// None of them is free and none is redundant - there is no gadget another one
// beats on every axis. So "the best gadget" has no answer without knowing what
// the miner is short of, which is why advise() is asked for a goal.
#include "MiningHardware.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace SCMiner {

// What a ship is carrying. The loadout does not change between rocks, so it is
// entered once rather than read off the screen every scan.
// One head. A MOLE flies three of these and fits each one separately, which
// is the whole point of the ship: a wide forgiving head for finding the window
// and a hard one for the rock that will not give.
struct MiningLoadout {
    const MiningLaser* laser = nullptr;
    std::vector<const MiningModule*> modules;   // however many the head takes

    // A crafted laser is not the stock one. The game prints the difference on
    // the item itself - "Laser Power: +0.00 % -> +21.72 %" - so this is read
    // off the screen rather than worked out: enter the second figure and the
    // rest follows. Two of a laser's three blueprint slots move Impact Force,
    // which is the fracture power under another name, so the range runs from
    // 0.64x for a poor craft to 1.44x for a good one. That is a wider spread
    // than any module gives, which is why it belongs here and not as a footnote.
    float craftedPower = 0.0f;    // per cent, as the item shows it. 0 = stock.
};

// What the laser does once the modules have had their say. Every figure here
// is the finished one, the way the HUD prints it.
struct LaserOutput {
    float fracture = 0;     // energy per second at full range
    float atRange = 0;      // ...and at the distance asked about
    float window = 100;     // per cent, the HUD's WINDOW SIZE
    float rate = 100;       // per cent
    float resistance = 100; // per cent, the HUD's RESISTANCE
    float instability = 0;  // per cent this laser ADDS to the rock
    float falloff = 1.0f;   // what the distance cost, 0..1
    float stock = 0;        // what it would be without the crafting bonus
};

// Full power out to fullRange, nothing from zeroRange. The shape between them
// is NOT in the game files - this reads it as a straight line, which is the
// simplest thing that fits the two numbers. The HUD bar changes colour in
// steps, so the truth may well be stepped; when that is measured this is the
// one place to change.
inline float rangeFalloff(const MiningLaser& laser, float metres) {
    if (metres <= laser.fullRange) return 1.0f;
    if (metres >= laser.zeroRange) return 0.0f;
    const float span = laser.zeroRange - laser.fullRange;
    if (span <= 0) return 1.0f;
    return (laser.zeroRange - metres) / span;
}

// ...and back: how far out a laser puts out a given share of its power.
// Inside fullRange it is all of it, so a share of 1 or more answers fullRange.
// Whatever rangeFalloff becomes once it is measured, this has to stay its
// inverse.
inline float distanceForFalloff(const MiningLaser& laser, float falloff) {
    if (falloff >= 1.0f) return laser.fullRange;
    if (falloff <= 0.0f) return laser.zeroRange;
    return laser.zeroRange - falloff * (laser.zeroRange - laser.fullRange);
}

// Where to stand when the laser is too strong for the rock up close.
//
// A laser cannot be turned down below its minimum - 20 % on a Helix I - and on
// a small rock that minimum already loads the bar faster than the rock gives
// it back, so the bar climbs on its own. Standing further off costs the laser
// power, and that is the one control left. The distance asked for is where
// the rock would hold at a throttle with room either way: the laser's minimum
// and a quarter more, between 35 and 60 %.
//
// PROVISIONAL with rangeFalloff: the shape of the falloff is not measured yet,
// so the metres are only as good as the straight line between the laser's two
// ranges.
struct DistanceAdvice {
    bool backOff = false;      // up close the rock climbs at the laser's minimum
    float metres = 0;          // about where it holds at `throttle`
    float throttle = 0;        // per cent
    bool tooLittle = false;    // so little is needed that only the last metres of reach would do
};

inline DistanceAdvice distanceFor(const MiningLaser& laser, float shareUpClose) {
    DistanceAdvice out;
    if (shareUpClose <= 0.0f || shareUpClose >= laser.throttleMin) return out;
    out.backOff = true;
    float target = laser.throttleMin + 0.25f;
    if (target < 0.35f) target = 0.35f;
    if (target > 0.60f) target = 0.60f;
    const float falloff = shareUpClose / target;
    out.throttle = target * 100.0f;
    out.metres = distanceForFalloff(laser, falloff);
    // under a tenth of the power is the last few metres before nothing - a
    // hand's breadth of drift there is the difference between holding and not
    out.tooLittle = falloff < 0.10f;
    return out;
}

// Which of the modules on one head act at the same time - see the two
// flags on MiningModule. A passive module always does, and so does an active
// one that neither cuts the others off nor is cut off by them, which is Surge:
// every copy counts. The other actives cut each other off when they fire, so
// of those only one runs at a time, and a second copy of the same one only
// replaces the first - of those the one that does the most for a break
// counts: more power, then less resistance.
//
// None of this is measured directly. It is what the game files say about
// the component that fires a module, and what a community 4.10 Prospector
// guide says: actives interrupt each other except Surge, and two Surge on a
// Helix I break around 44,000 kg - which is 3,900 x 1.5 x 1.5 over 0.2, so
// they multiply. Three Spectrum threads run two Surge at once the same way.
// One check from a MOLE's pilot seat had a second Surge add nothing; that
// stands unexplained.
inline bool countsEveryCopy(const MiningModule& m) { return !m.active || (!m.interrupts && !m.interruptible); }
inline bool cutsOthersOff(const MiningModule& m) { return m.active && !countsEveryCopy(m); }

// The modules on this head that act at once. With `actives` false every
// active module is left out: what the laser does before one is fired, or
// after it has run out.
inline std::vector<const MiningModule*> countedModules(const MiningLoadout& fit, bool actives = true) {
    std::vector<const MiningModule*> out;
    const MiningModule* one = nullptr;   // of the actives that cut each other off, the one that runs
    for (const MiningModule* m : fit.modules) {
        if (!m) continue;
        if (m->active && !actives) continue;
        if (countsEveryCopy(*m)) {
            out.push_back(m);
            continue;
        }
        if (!one || m->power > one->power || (m->power == one->power && m->resistance < one->resistance)) one = m;
    }
    if (one) out.push_back(one);
    return out;
}

// The chain: the laser's own figure, then each module multiplied in. Power is
// a plain factor, the rest are percentages - which is why a Helix I at 3900
// with Focus III (0.95) and Rieger-C3 (1.25) comes out at 4631, and the HUD
// shows the same chain against a base of 100 as 119. Which modules count is
// countedModules' business; `actives` false asks what the laser does with no
// active module running.
inline LaserOutput outputOf(const MiningLoadout& fit, float metres = 0, bool actives = true) {
    LaserOutput out;
    if (!fit.laser) return out;
    const MiningLaser& l = *fit.laser;

    out.fracture = l.fracture;
    out.window = 100.0f * (1.0f + l.window / 100.0f);
    out.rate = 100.0f * (1.0f + l.rate / 100.0f);
    out.resistance = 100.0f * (1.0f + l.resistance / 100.0f);
    out.instability = l.instability;

    for (const MiningModule* m : countedModules(fit, actives)) {
        out.fracture *= m->power;
        out.window *= (1.0f + m->window / 100.0f);
        out.rate *= (1.0f + m->rate / 100.0f);
        out.resistance *= (1.0f + m->resistance / 100.0f);
        // instability adds up rather than multiplying: each piece puts its own
        // share into the same rock, and 0 has to mean "changes nothing"
        out.instability += m->instability;
    }

    // ...and last, what the craft is worth. Kept separate from `stock` so the
    // window can show both and a player can see what their build bought them.
    out.stock = out.fracture;
    if (fit.craftedPower != 0.0f) out.fracture *= (1.0f + fit.craftedPower / 100.0f);

    out.falloff = rangeFalloff(l, metres);
    out.atRange = out.fracture * out.falloff;
    return out;
}

// How much throttle this rock wants.
//
// The charge level is a balance, not a tank: the laser puts energy in while
// the rock gives it back, and where the two meet the bar stands still. That
// gives a figure a miner can act on - "hold it at 84 %" instead of "it should
// work" - and it is the same arithmetic that decides whether the rock is
// possible at all, because a bar that will not rise at full throttle is a rock
// that will not break.
//
//     the rock gives back   mass x decayPerMass          per second
//     the laser puts in     fracture x falloff x throttle
//
// ASSUMED, and this is the one soft spot: that the give-back runs while the
// laser is firing, and that throttle scales the input linearly. Both are the
// simplest reading of the constants and neither is written down. One timed run
// against a rock of known mass would settle them, and this is the only place
// they are used.
struct ThrottleAdvice {
    float toHold = 0;       // per cent: the bar stands still here
    float toRise = 0;       // per cent: a little above, so it climbs
    bool possible = false;  // false when even full throttle cannot hold it
    bool overpowered = false;   // true when the laser's minimum already exceeds
                                // what the rock needs - it will run away
    float headroom = 0;     // per cent of the laser left above toHold
};

// What a rock takes back, per unit of mass, per second.
//
// 0.2 is decayPerMass in miningglobalparamsship.xml, used as is and not
// fitted to measurements: bar readings scatter more than they differ
// between rocks (one rock at INST 24.44 held at 97, 96, 95 and 89 % on four
// tries), so a trend with mass or printed resistance fitted to them is
// fitted to noise.
//
// Instability does not raise the demand. instabilityWavePeriod 3 and
// instabilityWaveVariance 1 make it a wave that moves the balance point over
// time; on a rock at INST 0.00 the point reads within one per cent, while
// the one at 24.44 would not sit still. The window shows it as a throttle
// band in MinerGUI that widens with the rock's instability.
//
// Resistance is handled, on the safe side and not completely, by
// resistanceLetsThrough below: the power handed to throttleFor is what gets
// THROUGH the rock, not what leaves the head.
inline constexpr float BREAK_DECAY_PER_MASS = 0.2f;

// Where "enough power" stops being a yes or a no. Comfortable is enough left
// over to drive the bar up rather than only to hold it; past the edge is
// short. The edge is just past 1 because the decay itself is measured rather
// than exact.
inline constexpr float BREAK_EDGE = 1.05f;
inline constexpr float BREAK_COMFORTABLE = 0.90f;

inline ThrottleAdvice throttleFor(float mass, const MiningLaser* laser,
                                  float powerAtRange) {
    ThrottleAdvice out;
    if (!laser || mass <= 0 || powerAtRange <= 0) return out;

    const float givesBack = mass * BREAK_DECAY_PER_MASS;
    const float share = givesBack / powerAtRange;          // 1.0 = full throttle

    out.toHold = share * 100.0f;
    out.toRise = out.toHold * 1.08f;    // a hand's width above the balance
    out.possible = share <= 1.0f;
    out.overpowered = share < laser->throttleMin;
    out.headroom = out.possible ? (1.0f - share) * 100.0f : 0.0f;
    return out;
}

// What a rock says about itself, off the scan panel.
struct RockStats {
    float mass = 0;
    float resistance = 0;      // per cent, as printed
    float instability = 0;
    float scu = 0;
    std::string ore;
    float distance = 0;        // metres, off the marker under the rock
    // Printed in colour - green where the laser on the rock took some off, red
    // where it put some on. Either way the laser has already had its say.
    // See rockOwnResistance.
    bool resistanceAdjusted = false;
};

// The rock as it is once a gadget is stuck on it. A gadget attaches to the
// ROCK, not to the ship, so what it changes are the rock's own figures.
//
// Only what the gadget's own data states: the resistance and the
// instability, each a per cent on what the panel read. What the resistance
// then does to the power is resistanceLetsThrough's business, below.
inline RockStats withGadget(const RockStats& rock, const MiningGadget* gadget) {
    if (!gadget) return rock;
    RockStats out = rock;
    out.resistance = rock.resistance * (1.0f + gadget->resistance / 100.0f);
    if (out.resistance < 0) out.resistance = 0;
    out.instability = rock.instability * (1.0f + gadget->instability / 100.0f);
    if (out.instability < 0) out.instability = 0;
    return out;
}

// The other way round: the rock's own figures, off a panel that already shows
// what the gadget on it made of them. From the moment a gadget is on, the
// game prints the rock's resistance and instability WITH it - 56 % reads 28 %
// under a Sabir - while the view keeps the rock's own figures and applies
// the gadget itself. So a figure read after the gadget went on is taken
// back through it first, or the gadget would count twice.
inline RockStats withoutGadget(const RockStats& rock, const MiningGadget* gadget) {
    if (!gadget) return rock;
    RockStats out = rock;
    const float r = 1.0f + gadget->resistance / 100.0f;
    const float i = 1.0f + gadget->instability / 100.0f;
    if (rock.resistance > 0 && r > 0.01f) out.resistance = rock.resistance / r;
    if (rock.instability > 0 && i > 0.01f) out.instability = rock.instability / i;
    return out;
}

// A gadget the game's log says was stuck on a rock, and which rock that is as
// far as the panel can tell: by its mass, the one figure a gadget leaves
// alone. The log names the gadget and the rock's kind
//   placed 'Mining_Gadget_SHIN_Sabir_<id>' on entity 'MineableRock_AsteroidLegendary_Savrilium_<id>'
// but no figure of the rock, so the rock on the panel when the gadget went on
// is taken for it - a player scans a rock, gets out and sticks the gadget on
// that one - and with none read, the first one read after, when its ore fits
// the kind the log named. Any other rock is read as it stands.
struct GadgetOnRock {
    const MiningGadget* gadget = nullptr;
    float mass = 0;            // the rock it is on, by the mass read; 0 until one is taken for it
    std::string oreWord;       // "Savrilium", off the rock's kind in the log; "" when it names none
    bool onView = false;       // the rock last read is the one it is on

    // One reading of the panel, as printed. Returns the rock's own figures:
    // taken back through the gadget where this is the rock it is on.
    RockStats take(const RockStats& read) {
        if (!gadget) {
            onView = false;
            return read;
        }
        if (read.mass > 0) {
            if (mass <= 0 && oreFits(read.ore)) mass = read.mass;
            const float slack = mass * 0.005f > 1.0f ? mass * 0.005f : 1.0f;
            onView = mass > 0 && read.mass - mass <= slack && mass - read.mass <= slack;
        }
        return onView ? withoutGadget(read, gadget) : read;
    }

    // "SAVRILIUM (ORE)" against "Savrilium" - by the word's first six
    // letters, which Aluminum and Aluminium share; a name not read, or a
    // kind that names no ore, is no objection
    bool oreFits(const std::string& panelOre) const {
        if (oreWord.empty() || panelOre.empty()) return true;
        auto lower = [](std::string s) {
            for (char& c : s) {
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            }
            return s;
        };
        return lower(panelOre).find(lower(oreWord.substr(0, 6))) != std::string::npos;
    }
};

// How much of a head's power a rock lets through.
//
//     P_eff = P x (1 - (R x M)^1.21)
//
// R is the rock's own resistance. M is what the head does to it - the
// laser's figure and every module's, multiplied: the HUD's RESISTANCE line
// over 100, 70 for a Helix II. R x M is the figure the panel prints in green
// while that head is on the rock. A gadget on the rock multiplies into R. The
// rock takes back 0.2 x mass of what gets through, and with more than one
// head on it, what each lets through adds up.
//
// The straight line 1 - R x M, Mort13's formula from measured breaks as the
// GrimHEX calculator publishes it, asks more throttle than rocks with
// resistance need. The exponent is fitted to four rocks on a Helix I with
// Focus III and Rieger-C3 (4,631 W, all within 13 m), each holding between a
// throttle at which the bar falls and one at which it climbs; "line" and
// "curve" are what the two formulas ask:
//
//     mass  ore          green  falls at  climbs at     line   curve
//   13,147  QUANTAINIUM   47 %    95.4 %      100 %  107.1 %  95.3 %
//   19,930  TITANIUM      12 %    92.6 %     99.7 %   97.8 %  93.4 %
//   15,707  TITANIUM      14 %    75.0 %     82.9 %   78.9 %  74.9 %
//   10,307  ASLARITE      30 %         -     56.5 %   63.6 %  58.2 %
//
// 1.21 lands within 0.1 of the first three and 1.7 over on the last, where
// the line is 7 out on two. It still errs towards too much throttle: two
// torite rocks held at 55.5 and 57 %, the curve asks 59 and 62. A 48,929 kg
// savrilium at 75 % stays impossible for a Helix II with three Rieger-C3 and
// +28.84 % crafted, as in the game: 6,713 through against 9,786.
//
// Four rocks, one at INST 387 wobbling on its wave: the exponent is a
// measurement, not a law. resistanceCurveFactor = 0.6 in
// miningglobalparamsship.xml is unexplained; it is not this 1.21 by any
// obvious road.
inline constexpr float RESISTANCE_CURVE = 1.21f;

inline float resistanceLetsThrough(float rockResistancePercent, float headResistancePercent) {
    if (rockResistancePercent <= 0 || headResistancePercent <= 0) return 1.0f;
    const float held = (rockResistancePercent / 100.0f) * (headResistancePercent / 100.0f);
    if (held >= 1.0f) return 0.0f;
    return 1.0f - std::pow(held, RESISTANCE_CURVE);
}

// The rock's own resistance, from what the panel printed.
//
// The panel prints it in three colours. White is the rock's own figure, as a
// seat that only scans sees it - a MOLE's cockpit - and that is the figure the
// formula wants: each head's own factor goes on top of it, before anything
// is said about whether the rock can be broken. Green and red come from the
// seat whose laser is on the rock, and that laser has already had its say:
// green where it took resistance off, red where it put some on (a rock at
// 75 % white in a MOLE's cockpit reads 52 % green in a turret with a Helix
// II, 75 x 0.70; the MOLE's stock Arbor MH2 adds 25 % and prints red). Put
// through the formula as printed, a coloured figure would have the laser
// counted twice - so it is taken back to the rock's own first, by dividing
// out the factor that coloured it.
//
// Which head lowered it the panel does not say. On a ship with one head it is
// that one; on a MOLE the caller averages the heads' own factors - equal on
// every loadout seen so far, and off by the spread between them where not.
inline float rockOwnResistance(float printedPercent, bool printedAdjusted, float headFactor) {
    if (printedPercent <= 0 || !printedAdjusted || headFactor <= 0) return printedPercent;
    const float own = printedPercent / headFactor;
    return own > 100.0f ? 100.0f : own;
}

// What the miner is short of. Nothing else decides which gadget is right.
enum class MiningGoal {
    Automatic,     // work it out from the rock - see advise()
    Breakable,     // it will not crack: bring the resistance down
    Safe,          // it keeps going up: bring the instability down
    Easier,        // the window is too narrow to hold
    Faster,        // the window fills too slowly
    Yield,         // it breaks fine - keep more of what is in it
};

struct GadgetAdvice {
    const MiningGadget* gadget = nullptr;   // null means "none is worth it"
    MiningGoal reason = MiningGoal::Automatic;
    std::string because;      // one sentence, for the window
    float effect = 0;         // per cent it moves the axis that was chosen
    // From the balance, when one was handed in: whether the rock holds only
    // with the gadget on it, and the laser power to hold it without and with
    bool needed = false;
    float holdWithout = 0;    // per cent; over 100 when it does not hold
    float holdWith = 0;       // per cent
};

namespace detail {

inline const MiningGadget* bestOn(float MiningGadget::*axis, bool higherIsBetter) {
    const MiningGadget* best = nullptr;
    for (int i = 0; i < MINING_GADGET_COUNT; i++) {
        const MiningGadget& g = MINING_GADGETS[i];
        if (g.*axis == 0) continue;
        if (!best || (higherIsBetter ? (g.*axis > best->*axis) : (g.*axis < best->*axis))) {
            best = &g;
        }
    }
    return best;
}

}  // namespace detail

// The gadget for a goal, and why. Each goal is one axis, and each axis has one
// winner - so this is a lookup, not a search.
inline GadgetAdvice adviseFor(MiningGoal goal) {
    GadgetAdvice out;
    out.reason = goal;
    switch (goal) {
        case MiningGoal::Breakable:
            out.gadget = detail::bestOn(&MiningGadget::resistance, false);
            out.because = "brings the rock's resistance down furthest";
            if (out.gadget) out.effect = out.gadget->resistance;
            break;
        case MiningGoal::Safe:
            out.gadget = detail::bestOn(&MiningGadget::instability, false);
            out.because = "takes the most instability out of it";
            if (out.gadget) out.effect = out.gadget->instability;
            break;
        case MiningGoal::Easier:
            out.gadget = detail::bestOn(&MiningGadget::window, true);
            out.because = "widens the charge window the most";
            if (out.gadget) out.effect = out.gadget->window;
            break;
        case MiningGoal::Faster:
            out.gadget = detail::bestOn(&MiningGadget::rate, true);
            out.because = "fills the window fastest";
            if (out.gadget) out.effect = out.gadget->rate;
            break;
        case MiningGoal::Yield:
            out.gadget = detail::bestOn(&MiningGadget::cluster, true);
            out.because = "keeps the most ore together when it shatters";
            if (out.gadget) out.effect = out.gadget->cluster;
            break;
        case MiningGoal::Automatic:
            break;
    }
    return out;
}

// ...and the goal itself, worked out from the rock in front of you.
//
// The order is a priority, not a score, because these are not comparable
// quantities: a rock that will not break is not "worse" than one that breaks
// dangerously, it is a different problem, and adding them up would invent a
// number nobody can check. So the first thing that is actually wrong wins.
//
// The thresholds are the honest weak point. Resistance and instability come
// off the panel as the game prints them, and where "too much" begins is not in
// the files - these are first estimates to be corrected against real rocks,
// and they sit here, alone, so that correcting them is a one-line job.

inline constexpr float RESISTANCE_HARD = 50.0f;     // per cent: needs help
inline constexpr float INSTABILITY_RISKY = 5.0f;    // above this it runs away
inline constexpr float WINDOW_NARROW = 70.0f;       // per cent of the base

// The balance the throttle advice is drawn from, handed in so that "needed"
// and "would help" mean what they say: what the rock gives back, its own
// resistance with nothing on it, and what the heads firing at it put through
// a rock of any given resistance - each through its own factor, so a gadget
// that halves the resistance is weighed the way the game would weigh it.
struct BreakBalance {
    float needs = 0;                 // per second: 0.2 x mass
    float ownResistance = 0;         // per cent, the rock's own, no gadget on it
    std::function<float(float ownResistancePercent)> powerThrough;
};

inline GadgetAdvice advise(const RockStats& rock, const MiningLoadout& fit,
                           const BreakBalance* balance = nullptr) {
    const LaserOutput out = outputOf(fit, rock.distance);

    // Out of range is not a gadget problem, and no gadget fixes it. Say so
    // rather than recommending one that cannot help.
    if (fit.laser && rock.distance > fit.laser->fullRange) {
        GadgetAdvice advice;
        advice.reason = MiningGoal::Automatic;
        advice.because = "you are past this laser's full-power range - closing in "
                         "is worth more than any gadget";
        if (rock.distance >= fit.laser->zeroRange) {
            advice.because = "you are out of range entirely - nothing reaches the rock";
        }
        return advice;
    }

    // With the balance known, whether a gadget is NEEDED is arithmetic, not a
    // threshold: the rock will not hold as it is and will with the gadget on
    // it. One that holds already, but with nothing left to drive the bar up,
    // gets the gadget as help; one that holds comfortably needs none for
    // breaking, whatever its resistance, and the other goals get their turn.
    if (balance && balance->needs > 0 && balance->powerThrough) {
        const auto shareWith = [&](const MiningGadget* g) {
            float own = balance->ownResistance;
            if (g) own = own * (1.0f + g->resistance / 100.0f);
            if (own < 0) own = 0;
            const float through = balance->powerThrough(own);
            return through > 0 ? balance->needs / through : 1e9f;
        };
        const float asIs = shareWith(nullptr);
        if (asIs > BREAK_COMFORTABLE) {
            const MiningGadget* best = nullptr;
            float bestShare = asIs;
            for (int i = 0; i < MINING_GADGET_COUNT; i++) {
                const MiningGadget& g = MINING_GADGETS[i];
                if (g.resistance >= 0) continue;
                const float share = shareWith(&g);
                if (share < bestShare) {
                    bestShare = share;
                    best = &g;
                }
            }
            char text[192];
            GadgetAdvice advice;
            if (asIs > BREAK_EDGE) {
                if (best && bestShare <= BREAK_EDGE) {
                    advice.gadget = best;
                    advice.reason = MiningGoal::Breakable;
                    advice.effect = best->resistance;
                    advice.needed = true;
                    advice.holdWithout = asIs * 100.0f;
                    advice.holdWith = bestShare * 100.0f;
                    std::snprintf(text, sizeof(text),
                                  "needed - without it the rock will not hold at any throttle; "
                                  "with it, hold around %.0f %%", bestShare * 100.0f);
                    advice.because = text;
                    return advice;
                }
                advice.reason = MiningGoal::Automatic;
                advice.because = "no gadget brings this rock within reach - it wants more power";
                return advice;
            }
            if (best && bestShare <= BREAK_COMFORTABLE) {
                advice.gadget = best;
                advice.reason = MiningGoal::Breakable;
                advice.effect = best->resistance;
                advice.holdWithout = asIs * 100.0f;
                advice.holdWith = bestShare * 100.0f;
                std::snprintf(text, sizeof(text),
                              "not needed, but it would help - from %.0f %% down to %.0f %%, "
                              "with room to drive the bar up", asIs * 100.0f, bestShare * 100.0f);
                advice.because = text;
                return advice;
            }
            // at the edge, and no gadget makes it comfortable: the other
            // goals are worth more than a resistance gadget that changes little
        }
        if (rock.instability >= INSTABILITY_RISKY) return adviseFor(MiningGoal::Safe);
        if (out.window < WINDOW_NARROW) return adviseFor(MiningGoal::Easier);
        return adviseFor(MiningGoal::Yield);
    }

    if (rock.resistance >= RESISTANCE_HARD) return adviseFor(MiningGoal::Breakable);
    if (rock.instability >= INSTABILITY_RISKY) return adviseFor(MiningGoal::Safe);
    if (out.window < WINDOW_NARROW) return adviseFor(MiningGoal::Easier);
    return adviseFor(MiningGoal::Yield);
}

// Every gadget ranked for one goal, for a window that wants to show the
// choice rather than just the answer.
inline std::vector<const MiningGadget*> rankFor(MiningGoal goal) {
    std::vector<const MiningGadget*> all;
    for (int i = 0; i < MINING_GADGET_COUNT; i++) all.push_back(&MINING_GADGETS[i]);

    auto score = [goal](const MiningGadget* g) {
        switch (goal) {
            case MiningGoal::Breakable: return -g->resistance;
            case MiningGoal::Safe:      return -g->instability;
            case MiningGoal::Easier:    return g->window;
            case MiningGoal::Faster:    return g->rate;
            case MiningGoal::Yield:     return g->cluster;
            default:                    return 0.0f;
        }
    };
    for (size_t i = 1; i < all.size(); i++) {
        for (size_t j = i; j > 0 && score(all[j]) > score(all[j - 1]); j--) {
            const MiningGadget* t = all[j]; all[j] = all[j - 1]; all[j - 1] = t;
        }
    }
    return all;
}

// ---------------------------------------------------------------------------
// The ships, and what they can carry.
//
// This is the one table here that is not generated: the extract holds the
// game's components but not its vehicle definitions, so the hardpoints are
// written down rather than read. Three facts, and they decide which lasers a
// suggestion is even allowed to name:
//
//   Prospector  one S1 head, swappable
//   MOLE        three S2 heads, swappable, each with its own modules
//   Golem       one head that is part of the ship - the Pitman - but its two
//               module slots are free
//
// A ship not listed here still works: pass the size and count directly.
// ---------------------------------------------------------------------------
struct MiningShip {
    const char* id;
    const char* name;
    int8_t heads;            // how many lasers it flies
    int8_t size;             // the biggest head its hardpoints take
    const char* fixedLaser;  // null when the head can be swapped
};

inline const MiningShip MINING_SHIPS[] = {
    {"prospector", "MISC Prospector", 1, 1, nullptr},
    {"mole",       "Argo MOLE",       3, 2, nullptr},
    {"golem",      "Drake Golem",     1, 1, "drak_golem_s1"},
};
inline constexpr int MINING_SHIP_COUNT = 3;

// A laser that is part of a ship rather than a thing you buy and bolt on.
// The Golem's Pitman is the only one: it has no shop entry and no hardpoint
// anywhere else. As an S1 head it would otherwise pass as a fit for a
// MOLE's S2 mount.
inline const MiningShip* shipBuiltInto(const MiningLaser& laser) {
    for (int i = 0; i < MINING_SHIP_COUNT; i++) {
        const MiningShip& ship = MINING_SHIPS[i];
        if (ship.fixedLaser && std::strcmp(ship.fixedLaser, laser.id) == 0) return &ship;
    }
    return nullptr;
}

// A hardpoint takes its own size and anything smaller - a MOLE can fly an S1
// head on an S2 mount, which is not a corner case but a normal way to fit the
// ship: the S1 Helix out-fractures three of the six S2 heads and gives up
// reach for it. What it will not take is a head welded into another hull.
inline bool laserFits(const MiningLaser& laser, const MiningShip& ship) {
    if (laser.size > ship.size) return false;
    const MiningShip* home = shipBuiltInto(laser);
    return home == nullptr || home == &ship;
}

// One way to fit a ship, and what it gets you against a particular rock.
struct LaserSuggestion {
    const MiningLaser* laser = nullptr;
    std::vector<const MiningModule*> modules;
    float power = 0;        // what gets through the rock at its distance and resistance
    float throttle = 0;     // per cent needed to hold the bar
    bool breaks = false;    // whether it can be held at all
    bool stock = false;     // true when no module was needed
};

namespace detail {

// The modules that get the most through THIS rock on this head, among the
// mixes a head can really run at once: a passive module as often as there
// are slots, Surge too, and of the actives that cut each other off at most
// one (see countedModules; the rule is read off the game's own files).
//
// Power alone does not decide it. Brandt's +35 % comes with +15.5 %
// on the rock's resistance, so on a hard rock a Rieger-C3 gets more through;
// with one Surge allowed, what fills the other slots is exactly that kind of
// trade. Every mix is tried: only modules that help a break - more power,
// or less resistance - and at most three slots, which is a few hundred
// mixes a head. Between two mixes that get the same through, the one with
// fewer active modules wins - they are charges, and run out - and then the
// one with fewer modules.
inline std::vector<const MiningModule*> bestModules(const MiningLoadout& bare, const RockStats& rock) {
    std::vector<const MiningModule*> best;
    if (!bare.laser || bare.laser->slots <= 0) return best;
    const int slots = bare.laser->slots;
    std::vector<const MiningModule*> helps;
    for (int i = 0; i < MINING_MODULE_COUNT; i++) {
        const MiningModule& m = MINING_MODULES[i];
        if (m.power > 1.0f || m.resistance < 0.0f) helps.push_back(&m);
    }
    const auto through = [&](const std::vector<const MiningModule*>& mods) {
        MiningLoadout fit = bare;
        fit.modules = mods;
        const LaserOutput o = outputOf(fit, rock.distance);
        return o.atRange * resistanceLetsThrough(rock.resistance, o.resistance);
    };
    const auto actives = [](const std::vector<const MiningModule*>& mods) {
        int n = 0;
        for (const MiningModule* m : mods) n += m->active ? 1 : 0;
        return n;
    };
    float bestThrough = through(best);
    std::vector<const MiningModule*> mix;
    // every multiset of up to `slots` modules, with at most one of the
    // actives that cut each other off
    bool cutting = false;
    std::function<void(size_t)> walk = [&](size_t from) {
        if (!mix.empty()) {
            const float t = through(mix);
            const bool clearly = t > bestThrough * 1.001f;
            const bool asGood = !clearly && t >= bestThrough * 0.999f;
            if (clearly || (asGood && (actives(mix) < actives(best) ||
                                       (actives(mix) == actives(best) && mix.size() < best.size())))) {
                best = mix;
                bestThrough = (std::max)(bestThrough, t);
            }
        }
        if ((int)mix.size() >= slots) return;
        for (size_t k = from; k < helps.size(); k++) {
            const MiningModule* m = helps[k];
            const bool cuts = cutsOthersOff(*m);
            if (cuts && cutting) continue;
            if (cuts) cutting = true;
            mix.push_back(m);
            walk(k);   // the same one again is allowed where it counts
            mix.pop_back();
            if (cuts) cutting = false;
        }
    };
    walk(0);
    return best;
}

}  // namespace detail

// Every head that fits this ship, ranked by whether it can hold the rock and
// then by how much room it leaves. Each one is reported once: stock when stock
// is enough, with the modules that do most for this rock when not - because
// "it works as it is" is a better answer than "buy three modules".
//
// The rock's resistance goes in as its OWN figure, and each head takes it
// through its own factor: a Klein at -45 % gets more of its power through a
// hard rock than a Helix at -30 %.
inline std::vector<LaserSuggestion> suggestLasers(const RockStats& rock,
                                                  int8_t hardpointSize,
                                                  float craftedPower = 0.0f) {
    std::vector<LaserSuggestion> out;
    const float givesBack = rock.mass * BREAK_DECAY_PER_MASS;
    const auto through = [&](const LaserOutput& o) {
        return o.atRange * resistanceLetsThrough(rock.resistance, o.resistance);
    };

    for (int i = 0; i < MINING_LASER_COUNT; i++) {
        const MiningLaser& l = MINING_LASERS[i];
        // anything up to the mount's size, not only its exact size
        if (l.size > hardpointSize) continue;
        // ...but never a head that is part of another ship. A suggestion is
        // only ever asked for by a ship whose heads can be swapped, so one
        // that cannot be bought is never the answer.
        if (shipBuiltInto(l)) continue;

        MiningLoadout bare;
        bare.laser = &l;
        bare.craftedPower = craftedPower;
        const LaserOutput plain = outputOf(bare, rock.distance);

        LaserSuggestion pick;
        pick.laser = &l;
        pick.power = through(plain);
        pick.stock = true;

        // Stock is enough? Then that is the answer, and no module is named.
        if (pick.power > 0 && givesBack / pick.power <= 1.0f) {
            pick.throttle = givesBack / pick.power * 100.0f;
            pick.breaks = true;
        } else if (l.slots > 0) {
            // Otherwise: the modules that get the most through this rock.
            MiningLoadout fitted = bare;
            fitted.modules = detail::bestModules(bare, rock);
            const LaserOutput best = outputOf(fitted, rock.distance);
            pick.modules = fitted.modules;
            pick.power = through(best);
            pick.stock = false;
            pick.breaks = pick.power > 0 && givesBack / pick.power <= 1.0f;
            pick.throttle = pick.power > 0 ? givesBack / pick.power * 100.0f : 0.0f;
        }
        out.push_back(pick);
    }

    // What holds the rock first, and among those what leaves the most room.
    for (size_t i = 1; i < out.size(); i++) {
        for (size_t j = i; j > 0; j--) {
            const bool better = (out[j].breaks && !out[j - 1].breaks) ||
                                (out[j].breaks == out[j - 1].breaks &&
                                 out[j].power > out[j - 1].power);
            if (!better) break;
            const LaserSuggestion t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    }
    return out;
}

inline const MiningShip* shipById(const char* id) {
    for (int i = 0; i < MINING_SHIP_COUNT; i++)
        if (std::strcmp(MINING_SHIPS[i].id, id) == 0) return &MINING_SHIPS[i];
    return nullptr;
}

inline const MiningLaser* laserById(const char* id) {
    for (int i = 0; i < MINING_LASER_COUNT; i++)
        if (std::strcmp(MINING_LASERS[i].id, id) == 0) return &MINING_LASERS[i];
    return nullptr;
}

inline const MiningModule* moduleById(const char* id) {
    for (int i = 0; i < MINING_MODULE_COUNT; i++)
        if (std::strcmp(MINING_MODULES[i].id, id) == 0) return &MINING_MODULES[i];
    return nullptr;
}

inline const MiningGadget* gadgetById(const char* id) {
    for (int i = 0; i < MINING_GADGET_COUNT; i++)
        if (std::strcmp(MINING_GADGETS[i].id, id) == 0) return &MINING_GADGETS[i];
    return nullptr;
}

}  // namespace SCMiner
