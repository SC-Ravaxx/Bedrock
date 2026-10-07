# The scan panel

Two pictures of the same rock, for work on reading the SCAN RESULTS panel:
whether a given laser can break what is in front of it.

* **`scan_fullscreen_1080.png`** - the whole HUD at 1920x1080, ship mining, a
  Tin rock under a HELIX I. Everything the question needs is in this one
  picture: the rock's figures on the right, the laser's on the left, and the
  two modules fitted.
* **`scan_panel_crop.png`** - the panel on its own, 206x262.

## Where the panel sits

Found by template matching the crop against the full frame, 95% correlation:

| | pixels at 1080p | share of the viewport |
|---|---|---|
| x | 1349 | 0.7026 |
| y | 435 | 0.4028 |
| width | 218 | 0.1135 |
| height | 278 | 0.2574 |

The crop had been scaled to 0.94 of its original size, which is why the match
sits at scale 1.06.

**Not yet known: whether those shares hold at other resolutions.** The HUD may
anchor the panel to an edge rather than scale it with the viewport, and the
signature reader already had to learn that the game scales its HUD with the
height of the picture. A second screenshot at another resolution would settle
it.

## What the panel carries

    TIN (ORE)
    MASS:         22593
    RESISTANCE:   0%
    INSTABILITY:  3.52      ( CHALLENGING )
    COMPOSITION:  30.21 SCU
      10.99%  TIN (ORE)          537
      63.84%  TIN (ORE)          340
       8.36%  COPPER (ORE)       652
      16.79%  INERT MATERIALS      0

Two entries for the same ore is not a mistake in the reading: the composition
presets in the game files are built that way. `commonshipmineables_aluminum`
has two aluminium parts, one at 2.82-6.82% and one at 39.18-83.18%, plus a
third element - exactly the shape above.

## And on the other side of the screen

    HELIX I MINING LASER          Head    +      ~
    FRACTURE PWR                   119   -5%   +25%
    RESISTANCE                      70
    INSTABILITY                    100
    OPTIMAL RATE                   100
    OVERCH. RATE                   100
    WINDOW SIZE                     83   +40%   -1%

    FOCUS III MODULE (passive):  Laser Power -5%,  Optimal Charge Window +4..
    RIEGER-C3 MODULE (passive):  Laser Power +25%, Optimal Charge Window -1..

This is the calibration point for the laser side: the game files give
`DamageEnergy = 1` for the HELIX plus percentage modifiers, and 119 is what
those add up to on screen with these two modules fitted.

## The mechanics, from the game files

`libs/foundry/records/mining/miningglobalparamsship.xml`:

    powerCapacityPerMass   10       decayPerMass          0.2
    resistanceCurveFactor  0.6      optimalWindowSize     0.1
    optimalWindowFactor    0.75     optimalWindowMaxSize  0.5
    optimalWindowThinnessCurveFactor 0.7

    instabilityWavePeriod  3        instabilityWaveVariance 1
    instabilityCurveFactor 1

So a rock's energy capacity is its mass times ten - 225,930 for this one - and
it falls back at 0.2 per mass, 4,518 per second, once the laser stops.
Instability is a wave over time, not a fixed threshold: it moves the window
rather than raising the bar.

There are three sets side by side - `...ParamsShip`, `...ParamsFps`,
`...ParamsGroundVehicle` - so the same model covers all three kinds of mining
with constants of their own.

## The laser formula, solved

Every stat starts at **100** and is multiplied - first by what the laser
itself carries, then by each module fitted. The HUD shows the finished figure
in the first column and what the modules contributed beside it.

The proof is the resistance row. `mining_laser_thcn_helix_s1.xml` carries
`resistance="-30"`, and the screenshot reads RESISTANCE 70. No module touches
it, so 100 x 0.70 = 70. With that settled, the other two rows fall out:

| row | base | laser | modules | computed | on screen |
|---|---|---|---|---|---|
| FRACTURE PWR | 100 | - | x0.95 x1.25 | 118.75 | **119** |
| RESISTANCE | 100 | x0.70 | - | 70.00 | **70** |
| WINDOW SIZE | 100 | x0.60 | x1.40 x0.99 | 83.16 | **83** |

The modules multiply, they do not add: -5% and +25% together give x1.1875,
not x1.20, and 119 is what rounds from 118.75 rather than from 120.

The laser's own modifiers, from `entities/scitem/ships/weapons/`:

    drak_golem_s1     window +40, resistance +25, rate -40   (the Golem's built-in laser, not a model you fit)
    grin_arbor_s1/s2  window +40, resistance +25
    grin_lancet_s1/s2 window -60, rate +40
    shin_hofstede_s1  window +60, resistance -30, rate +20
    shin_klein_s1/s2  window +20, resistance -45
    thcn_helix_s1/s2  window -40, resistance -30
    thcn_impact_s1/s2 window +20, resistance +10, rate -40

## What crafting moves

The blueprint for Helix I has three slots, and the stats they carry are the
answer to "can a crafted laser break more":

    Frame     -> Integrity      0.80 .. 1.20  over quality 0..1000
    Emitter   -> Impact Force    0.80 .. 1.20
    Bus Bars  -> Impact Force    0.80 .. 1.20

**Impact Force is the fracture power** - the database and the HUD call it
different things. Two slots move it, so material quality alone swings a
crafted laser between 0.64 and 1.44 of the stock figure, and a perfect build
carries 144 fracture power where a stock Helix carries 100.

That is the piece that makes the question worth answering: whether a rock is
breakable is not a property of the laser model but of the particular laser,
and the difference between a poor craft and a good one is more than double.

## The absolute power, and the threshold

The percentages above are only half of it - they scale something. The something
is `DamageEnergy` on the laser's fire modes, and the sizes differ:

| laser | size | fracture | extraction | throttle min |
|---|---|---|---|---|
| grin_arbor_s1 | 1 | 2340 | 1850 | 0.05 |
| shin_hofstede_s1 | 1 | 2600 | 1295 | 0.05 |
| thcn_impact_s1 | 1 | 2600 | 2775 | 0.20 |
| grin_lancet_s1 | 1 | 3120 | 1850 | 0.20 |
| shin_klein_s1 | 1 | 3120 | 2220 | 0.15 |
| thcn_helix_s1 | 1 | **3900** | 1850 | 0.20 |
| grin_arbor_s2 | 2 | 2900 | 2590 | 0.05 |
| shin_hofstede_s2 | 2 | 4060 | 1295 | 0.10 |
| thcn_impact_s2 | 2 | 4060 | 3145 | 0.30 |
| grin_lancet_s2 | 2 | 4350 | 2590 | 0.30 |
| shin_klein_s2 | 2 | 4350 | 2775 | 0.20 |
| thcn_helix_s2 | 2 | **4930** | 2590 | 0.30 |

Each laser has two fire modes, `ElectricArc` for breaking and `Extraction` for
drawing the pieces in, and the S0 heads are hand lasers with a fracture value
under 1 - a different scale entirely.

### How breaking actually works

Not an energy account that fills to a total. The charge level rises while the
laser is on and has to be **held inside a window** - too little does nothing,
too much goes into the danger band and the rock explodes. Holding it there
splits the rock into smaller pieces, which are then either drawn straight in
or have to be broken again, depending on their size.

That is what the constants are for, and they read correctly only once the
mechanic is understood this way:

    optimalWindowSize   0.1     the window, as a share of the bar
    optimalWindowFactor 0.75
    optimalWindowMaxSize 0.5
    controlledBreakingFillRate      0.5    inside the window
    controlledBreakingFillRateDanger 0.7
    dangerBreakingFillRate          0.3    and its exponent, 2
    dangerBreakingDecayRate         0.1

    instabilityWavePeriod 3, variance 1    the window moves under you

So `optimalChargeWindowSize` on the laser and WINDOW SIZE in the HUD are the
width of the band you have to stay in - which is why the Lancet at -60 is
described as difficult to fly and the Hofstede at +60 as forgiving.

**An earlier draft of this file had it as a race between laser power and
decay, with a rock taking either 49 seconds or 33 minutes. That was wrong and
has been removed.** `powerCapacityPerMass` and `decayPerMass` are real
constants and quoted correctly above, but they do not combine into a time to
break, and nothing here should be used to compute one until the mechanic is
pinned down properly.

### What this still supports

The laser figures stand on their own regardless: fracture power is
`DamageEnergy` on the ElectricArc mode, scaled by the laser's own modifiers
and then by each module, multiplicatively. A Helix I with Focus III and
Rieger-C3 puts out 4,631 against a stock 3,900, and a crafted one swings
between 0.64x and 1.44x on top of that. Comparing two loadouts against each
other needs none of the rock mechanics.

## Distance, which turns out to matter more than the loadout

Every laser carries `fullDamageRange` and `zeroDamageRange`, and the spread
between models is far wider than the spread in power:

| laser (S1) | full to | zero at | fracture |
|---|---|---|---|
| **thcn_helix_s1** | **15 m** | **45 m** | **3900** |
| grin_lancet_s1 | 30 m | 90 m | 3120 |
| shin_klein_s1 | 45 m | 135 m | 3120 |
| shin_hofstede_s1 | 45 m | 135 m | 2600 |
| thcn_impact_s1 | 45 m | 135 m | 2600 |
| grin_arbor_s1 | 60 m | 180 m | 2340 |
| drak_golem_s1 | 40 m | 45 m | 3900 |

The S2 heads keep the same ordering at roughly double the distances. The
Helix is the strongest S1 head and has by far the shortest reach - full power
only within 15 m, where the Arbor still pulls full at 60 m for a quarter less
power. The Golem's built-in laser is the odd one out: full to 40 m and dead at
45, a cliff rather than a slope.

**The screenshot is a distance problem, not an equipment problem.** The rock
sits at 28.4 m with a Helix I, so:

    (45 - 28.4) / (45 - 15) = 0.553
    4,631 x 0.553 = 2,561 instead of 4,631

Closing to 15 m nearly doubles the output with nothing swapped. Any reader
built on this has to take the range into account before it tells anyone their
laser is too weak - at 28.4 m a Helix is running at half strength by design.

**The shape of the curve is not in the files.** Only the two distances; whether
the fall between them is linear is not stated. The bar changing colour in the
HUD suggests steps rather than a smooth slope, which is something to read off
the screen rather than out of the data.

## The modules, all of them

`entities/scitem/ships/utility/mining/miningarm/`. Laser power sits in the
weapon block as `damageMultiplier`, a plain factor; the other three sit in
`ItemMiningModifierParams` as percentages.

| module | power | window | rate | resistance |
|---|---|---|---|---|
| active_surge | **1.50** | - | - | -15.5 |
| active_brandt | 1.35 | - | - | +15.5 |
| active_stampede | 1.35 | - | - | - |
| passive_rieger_mk3 | 1.25 | -1 | - | - |
| passive_rieger_mk2 | 1.20 | -3 | - | - |
| active_clearcut | 1.15 | +30 | - | - |
| passive_rieger_mk1 | 1.15 | -10 | - | - |
| passive_deluge | 1.15 | - | - | -15.5 |
| passive_overrun | 1.02 | - | - | -24.8 |
| passive_focus_mk3 | 0.95 | **+40** | - | - |
| passive_focus_mk2 | 0.90 | +37 | - | - |
| passive_focus_mk1 | 0.85 | +30 | - | - |
| active_optimum | 0.85 | - | - | - |
| active_rime | 0.85 | - | - | -24.8 |
| passive_xtr_mk3 | 1.00 | +25 | - | - |
| passive_torrent_mk3 | 1.00 | -1 | +45 | - |
| passive_vaux_mk3 | 1.00 | - | -5 | - |
| active_torpid | 1.00 | - | +60 | - |
| active_forel | 1.00 | - | - | +15.5 |
| active_lifeline | 1.00 | - | - | -15.5 |
| vehiclemod_argogeo | 0.15 | - | - | - |

(The mk1/mk2 steps of xtr, torrent and vaux follow the same pattern as their
mk3; fltr moves none of these four and does something else.)

This closes the loop on the screenshot exactly: Focus III is 0.95 power and
+40 window, Rieger-C3 is 1.25 power and -1 window, which is what the HUD
prints beside those rows and what produced 119 and 83.

Note the trade running through the whole list. Nothing gives power and window
together: Focus buys window with power, Rieger buys power with window, and
Surge at 1.50 is the most power available and pays for it in resistance.

## So the full chain, for one laser

    fracture power = DamageEnergy(ElectricArc)        from the laser
                   x damageMultiplier per module       multiplicative
                   x Impact Force from crafting        0.64 .. 1.44
                   x distance falloff                  full..zero range

    window size    = 100 x (1 + laser's own %) x (1 + each module's %)

Everything on the right is now measured out of the game files. The one part
still missing is the shape of that last factor - `fullDamageRange` and
`zeroDamageRange` are given, but not what happens between them, and the HUD
bar changing colour suggests steps rather than a straight line.
