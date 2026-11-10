"""Generates include/NavPlaces.h: where the places a shop or the player can be
at stand in their system, out of the game's own object containers.

    python gen_nav_places.py <folder> include/NavPlaces.h <build label>

<folder> holds the three systems' containers and Stanton's twenty Lagrange
points', unpacked from Data.p4k with unp4k (searched for under it, at any
depth):

    unp4k.exe <channel>/Data.p4k stantonsystem.socpak
    unp4k.exe <channel>/Data.p4k pyrosystem.socpak
    unp4k.exe <channel>/Data.p4k nyxsystem.socpak
    unp4k.exe <channel>/Data.p4k stanton1_l1.socpak   (... to stanton4_l5)

A system's container lists its bodies, Lagrange points, jump points and
belt segments as a tree of Child elements. A place's position is its
parents' positions with each turned by the rotations above it (Pyro's star
container is turned 85 degrees about z): metres, the star at the origin, the
frame /showlocation reports in. Planets do not orbit.

What is placed where:
- a planet or a moon at its centre, and the cities, outposts and stations in
  low orbit over it there as well (a radius is a thousandth of a Gm);
- a station at one of Stanton's Lagrange points where the point's own
  container puts it: up to 16,712 km from the point and 11,799 km over or
  under the planets' plane (the MIC stations), which the Aaron Halo's thin
  bands cannot leave out (HaloRoute.h); CRU-L2 and CRU-L3 have no station
  and stand at the point;
- Pyro's stops and the gateways at their point's container (the gateways
  40-50 km from it);
- Levski at its Glaciem Ring segment, the People's Service Stations at their
  Keeger Belt segments (Nyx_SocialStation_001..004 = Alpha, Delta, Theta,
  Lambda, by the star map records inside them).

The jump point containers keep the names of an older plan: in 4.10.2 the Nyx
Gateway in Stanton stands in jumppoint_stanton_magnus (star map record
RR_JP_StantonMagnus, "@Stanton_Nyx_JPStation") and the Stanton Gateway in Nyx
in jumppoint_nyx_castra ("@Nyx_Stanton_JPStation"). The gateways are the Nyx
Gateway and the Stanton Gateway, and only those names are written: GamePlaces
reads the log's codes for them as those (leadsTo).
"""
import math
import os
import re
import struct
import sys
import xml.etree.ElementTree as ET
import zipfile
import zlib


def read_socpak_xml(path, name):
    """The container's own XML out of a .socpak: a zip whose local headers
    use backslashes, which zipfile refuses - read from the header offset."""
    z = zipfile.ZipFile(path)
    with open(path, 'rb') as f:
        for i in z.infolist():
            if i.filename.replace('\\', '/').split('/')[-1].lower() != name.lower() + '.xml':
                continue
            f.seek(i.header_offset)
            h = f.read(30)
            n, e = struct.unpack_from('<HH', h, 26)
            f.seek(i.header_offset + 30 + n + e)
            raw = f.read(i.compress_size)
            return raw if i.compress_type == 0 else zlib.decompress(raw, -15)
    raise SystemExit('%s: no %s.xml in it' % (path, name))


def vec(s):
    return tuple(float(v) for v in s.split(','))


def qrot(q, v):
    w, x, y, z = q
    vx, vy, vz = v
    tx = 2 * (y * vz - z * vy)
    ty = 2 * (z * vx - x * vz)
    tz = 2 * (x * vy - y * vx)
    return (vx + w * tx + (y * tz - z * ty), vy + w * ty + (z * tx - x * tz), vz + w * tz + (x * ty - y * tx))


def qmul(a, b):
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return (aw * bw - ax * bx - ay * by - az * bz, aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx, aw * bz + ax * by - ay * bx + az * bw)


def placed(xml, turns=None):
    """Every Child's entityName and its position in the system's frame
    (and, into `turns`, the rotation that frame gives its own children)."""
    root = ET.fromstring(xml)
    out = {}

    def walk(el, p, q):
        for coc in el.findall('ChildObjectContainers'):
            for ch in coc.findall('Child'):
                r = qrot(q, vec(ch.get('pos') or '0,0,0'))
                cp = (p[0] + r[0], p[1] + r[1], p[2] + r[2])
                cq = qmul(q, vec(ch.get('rot') or '1,0,0,0'))
                name = ch.get('entityName') or ''
                if name and name not in out:
                    out[name] = cp
                    if turns is not None:
                        turns[name] = cq
                walk(ch, cp, cq)
    walk(root, (0.0, 0.0, 0.0), (1.0, 0.0, 0.0, 0.0))
    return out


def station(folder, planet, point, at, turn):
    """Stanton's rest stop at a Lagrange point, where the point's own
    container puts it, turned as the point is turned in the system: the child
    made of rs_ext_<hur|cru|arc|mic>-l<n>.socpak - LOC_RR_S<planet>_L<n> by
    name, but ARC-L2's is rs_ext_arc-l001. CRU-L2 and CRU-L3 have none, and
    a route there ends at the point itself."""
    key = 'stanton%d_l%d' % (planet, point)
    root = ET.fromstring(read_socpak_xml(find(folder, key + '.socpak'), key))
    want = 'rs_ext_%s-l%d.socpak' % (['hur', 'cru', 'arc', 'mic'][planet - 1], point)
    for ch in root.iter('Child'):
        if (ch.get('name') or '').replace('\\', '/').split('/')[-1].lower() == want:
            r = qrot(turn, vec(ch.get('pos') or '0,0,0'))
            return (at[0] + r[0], at[1] + r[1], at[2] + r[2])
    return at


# (container entity, names the place goes by). Gateways are named for where
# they lead; their system is the one they stand in.
STANTON = [
    ('OOC_Stanton_1_Hurston', ['Hurston', 'Lorville', 'Everus Harbor']),
    ('OOC_Stanton_1a_Ariel', ['Arial']),
    ('OOC_Stanton_1b_Aberdeen', ['Aberdeen']),
    ('OOC_Stanton_1c_Magda', ['Magda']),
    ('OOC_Stanton_1d_Ita', ['Ita']),
    ('OOC_Stanton_2_Crusader', ['Crusader', 'Orison', 'Seraphim Station']),
    ('OOC_Stanton_2a_Cellin', ['Cellin']),
    ('OOC_Stanton_2b_Daymar', ['Daymar']),
    ('OOC_Stanton_2c_Yela', ['Yela', 'Grim HEX', 'GrimHEX', 'Green Imperial Housing Exchange', 'Yela Asteroid Belt']),
    ('OOC_Stanton_3_ArcCorp', ['ArcCorp', 'Area18', 'Area 18', 'Baijini Point']),
    ('OOC_Stanton_3a_Lyria', ['Lyria']),
    ('OOC_Stanton_3b_Wala', ['Wala']),
    ('OOC_Stanton_4_Microtech', ['microTech', 'New Babbage', 'Port Tressler']),
    ('OOC_Stanton_4a_Calliope', ['Calliope']),
    ('OOC_Stanton_4b_Clio', ['Clio']),
    ('OOC_Stanton_4c_Euterpe', ['Euterpe']),
    ('OOC_JumpPoint_stanton_pyro', ['Pyro Gateway']),
    ('OOC_JumpPoint_stanton_terra', ['Terra Gateway']),
    ('OOC_JumpPoint_stanton_magnus', ['Nyx Gateway']),
] + [('OOC_Stanton%d_L%d' % (p, n), ['%s-L%d' % (code, n)])
     for p, code in enumerate(['HUR', 'CRU', 'ARC', 'MIC'], 1) for n in range(1, 6)]

PYRO_STOPS = {(2, 4): ['Checkmate', 'Checkmate Station'], (3, 1): ['Starlight Service Station'],
              (3, 3): ['Patch City'], (5, 2): ['Gaslight'], (5, 4): ["Rod's Fuel 'N Supplies"],
              (5, 5): ["Rat's Nest"], (6, 3): ['Endgame'], (6, 4): ['Dudley & Daughters'],
              (6, 5): ['Megumi Refueling']}
PYRO = [
    ('pyro1', ['Pyro I']),
    ('pyro2', ['Monox']),
    ('pyro3', ['Bloom', 'Orbituary', "Shepherd's Rest"]),
    ('pyro4', ['Pyro IV']),
    ('pyro5', ['Pyro V']),
    ('pyro5a', ['Ignis']),
    ('pyro5b', ['Vatra']),
    ('pyro5c', ['Adir']),
    ('pyro5d', ['Fairo']),
    ('pyro5e', ['Fuego']),
    ('pyro5f', ['Vuur']),
    ('pyro6', ['Terminus', 'Ruin Station', 'Terminus Ring']),
    ('jumppoint_pyro_stanton', ['Stanton Gateway']),
    ('jumppoint_pyro_nyx', ['Nyx Gateway']),
] + [('P%d_L%d' % (p, n), ['PYR%d L%d' % (p, n)] + PYRO_STOPS.get((p, n), []))
     for p in range(1, 7) for n in range(1, 6)]

NYX = [
    ('glaciemring_segment_levski', ['Levski', 'Delamar']),
    ('keeger_segment_social_001', ["People's Service Station Alpha"]),
    ('keeger_segment_social_002', ["People's Service Station Delta"]),
    ('keeger_segment_social_003', ["People's Service Station Theta"]),
    ('keeger_segment_social_004', ["People's Service Station Lambda"]),
    ('jumppoint_nyx_pyro', ['Pyro Gateway']),
    ('jumppoint_nyx_castra', ['Stanton Gateway']),
]


def find(folder, name):
    for top, _dirs, files in os.walk(folder):
        for f in files:
            if f.lower() == name.lower():
                return os.path.join(top, f)
    raise SystemExit('%s not found under %s - unpack it with unp4k (see the top of this file)' % (name, folder))


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    folder, out, build = sys.argv[1:]
    rows = []
    for system, table in (('Stanton', STANTON), ('Pyro', PYRO), ('Nyx', NYX)):
        key = system.lower() + 'system'
        turns = {}
        where = placed(read_socpak_xml(find(folder, key + '.socpak'), key), turns)
        for entity, names in table:
            if entity not in where:
                # a Lagrange point the system does not have (Pyro IV has none)
                if entity.startswith('P') and '_L' in entity:
                    continue
                raise SystemExit('%s: no %s in the tree' % (system, entity))
            at = where[entity]
            point = re.match(r'^OOC_Stanton([1-4])_L([1-5])$', entity)
            if point:
                at = station(folder, int(point.group(1)), int(point.group(2)), at, turns[entity])
            for name in names:
                rows.append((name, system, at))

    def esc(s):
        return s.replace('\\', '\\\\').replace('"', '\\"')

    def gm(a, b):
        return math.dist(a, b) / 1e9

    at = {(n, s): p for n, s, p in rows}
    lines = [
        '#pragma once',
        '// Where the places a shop or the player can be at stand in their system:',
        '// metres, the star at the origin, the frame /showlocation reports in.',
        '//',
        '// GENERATED by gen_nav_places.py from the game\'s system containers',
        '// (stantonsystem, pyrosystem, nyxsystem .socpak) and Stanton\'s twenty',
        '// Lagrange points\' (stanton1_l1 .. stanton4_l5), game build %s.' % build,
        '// Do not edit by hand - regenerate for a new build.',
        '//',
        '// A city, an outpost or a station in low orbit stands at its body\'s',
        '// centre, Stanton\'s Lagrange stations where their own containers put',
        '// them, Pyro\'s stops and the gateways at their point\'s container. A',
        '// gateway is named for where it leads and stands in `system`. Checks:',
        '// Hurston to ArcCorp %.2f Gm, Hurston to HUR-L1 %.2f Gm, ArcCorp to ARC-L1 %.2f Gm.' % (
            gm(at[('Hurston', 'Stanton')], at[('ArcCorp', 'Stanton')]),
            gm(at[('Hurston', 'Stanton')], at[('HUR-L1', 'Stanton')]), gm(at[('ArcCorp', 'Stanton')], at[('ARC-L1', 'Stanton')])),
        '',
        'namespace SCMiner {',
        '',
        'struct NavPlace {',
        '    const char* name;     // as the game log, GamePlaces or UEX name it',
        '    const char* system;   // Stanton / Pyro / Nyx',
        '    double x, y, z;       // metres',
        '};',
        '',
        '// %d names, %d places' % (len(rows), len(set(p for _n, _s, p in rows))),
        'inline constexpr NavPlace NAV_PLACES[] = {',
    ]
    for name, system, p in rows:
        lines.append('    {"%s", "%s", %.0f, %.0f, %.0f},' % (esc(name), system, p[0], p[1], p[2]))
    lines += ['};', '', 'inline constexpr int NAV_PLACE_COUNT = (int)(sizeof(NAV_PLACES) / sizeof(NAV_PLACES[0]));', '',
              '}  // namespace SCMiner', '']
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))
    print('%s: %d names' % (out, len(rows)))


if __name__ == '__main__':
    main()
