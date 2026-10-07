// The player's refinery work orders - see RefineryOrders.h.
#include "RefineryOrders.h"

#include "RefineryData.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace SCMiner {

namespace {

std::vector<RefineryOrder::Part> partsOf(const RefineryPanel& panel, bool refinedOnly) {
    std::vector<RefineryOrder::Part> parts;
    for (const RefineryRow& row : panel.rows) {
        if (row.inert || row.material.empty()) continue;
        if (refinedOnly && row.yield < 0) continue;
        parts.push_back({row.material, row.quality, row.yield, row.quantity});
    }
    std::sort(parts.begin(), parts.end(), [](const RefineryOrder::Part& a, const RefineryOrder::Part& b) {
        if (a.material != b.material) return a.material < b.material;
        if (a.quality != b.quality) return a.quality < b.quality;
        return a.yield < b.yield;
    });
    return parts;
}

// A part both name: the same material at the same quality
bool overlap(const std::vector<RefineryOrder::Part>& a, const std::vector<RefineryOrder::Part>& b) {
    for (const RefineryOrder::Part& x : a) {
        for (const RefineryOrder::Part& y : b) {
            if (x.material == y.material && x.quality == y.quality) return true;
        }
    }
    return false;
}

bool sameParts(const std::vector<RefineryOrder::Part>& a, const std::vector<RefineryOrder::Part>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].material != b[i].material || a[i].quality != b[i].quality || a[i].yield != b[i].yield) return false;
    }
    return true;
}

}  // namespace

std::string refineryStationAt(const std::string& place, const std::string& system) {
    if (place.empty()) return "";
    for (const Refinery& r : REFINERIES) {
        const std::string name = r.name;
        if (name == place) return name;
        // the gateways stand on both sides of their jump, named for the far one
        const size_t paren = name.find(" (");
        if (paren != std::string::npos && name.compare(0, paren, place) == 0 && paren == place.size() &&
            (system.empty() || system == r.system)) {
            return name;
        }
    }
    return "";
}

namespace {

std::string upperAlnum(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

int distance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); j++) {
            cur[j] = (std::min)({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

}  // namespace

std::string refineryStationNamed(const std::string& text, const std::string& system) {
    const std::string t = upperAlnum(text);
    if (t.size() < 4) return "";
    int best = 1 << 30;
    std::string name;
    for (const Refinery& r : REFINERIES) {
        std::string bare = r.name;
        const size_t paren = bare.find(" (");
        if (paren != std::string::npos) {
            // "Pyro Gateway (Nyx)": the header says "PYRO GATEWAY" on both
            // sides of the jump, and the system says which side this is
            if (!system.empty() && system != r.system) continue;
            bare = bare.substr(0, paren);
        }
        const int d = distance(t, upperAlnum(bare));
        if (d < best) {
            best = d;
            name = r.name;
        }
    }
    return best <= (std::max)(1, static_cast<int>(t.size()) / 5) ? name : "";
}

int RefineryOrder::yieldTotal() const {
    int total = 0;
    for (const Part& p : parts) total += (std::max)(0, p.yield);
    return total;
}

bool RefineryOrderBook::usable(const RefineryPanel& panel) {
    const bool running = panel.kind == RefineryBar::Band::Processing;
    const bool done = panel.kind == RefineryBar::Band::Completed;
    if (!running && !done) return false;
    if (running && (panel.order < 0 || panel.seconds < 0)) return false;
    int named = 0;
    for (const RefineryRow& row : panel.rows) {
        if (row.inert) continue;
        if (row.material.empty() || row.quality < 0 || row.yield < 0) return false;
        named++;
    }
    return named > 0;
}

bool RefineryOrderBook::samePanel(const RefineryPanel& a, long long aTaken, const RefineryPanel& b, long long bTaken,
                                  int slack) {
    if (a.kind != b.kind || a.order != b.order) return false;
    if (!sameParts(partsOf(a, false), partsOf(b, false))) return false;
    if (a.kind == RefineryBar::Band::Completed) return true;   // done: no clock on it
    // A quote: the method, the bill and the time alike as well
    if (a.kind != RefineryBar::Band::Processing) {
        return a.method == b.method && a.cost == b.cost && a.seconds == b.seconds;
    }
    const long long dueA = aTaken + a.seconds, dueB = bTaken + b.seconds;
    return std::llabs(dueA - dueB) <= slack;
}

// A quote: the method, the bill and the time, and which rows it refines -
// the ones the quote gave a yield, as far as the list shows them. The same
// quote seen again only moves its time on: once it went to an order, it
// does not go to a second one.
void RefineryOrderBook::noteQuote(const std::string& station, const RefineryPanel& panel, long long now) {
    if (panel.method.empty() || panel.cost < 0 || panel.seconds <= 0) return;
    std::vector<RefineryOrder::Part> parts = partsOf(panel, true);
    const bool same = quote_.station == station && quote_.method == panel.method && quote_.cost == panel.cost &&
                      quote_.seconds == panel.seconds;
    if (same) {
        // the same quote looked at again - scrolled, it may show other rows:
        // they all go into the job
        quote_.taken = now;
        for (const RefineryOrder::Part& p : parts) {
            auto at = std::find_if(quote_.parts.begin(), quote_.parts.end(), [&](const RefineryOrder::Part& q) {
                return q.material == p.material && q.quality == p.quality;
            });
            if (at == quote_.parts.end()) {
                quote_.parts.push_back(p);
            } else {
                if (p.yield >= 0) at->yield = p.yield;
                if (p.quantity >= 0) at->quantity = p.quantity;
            }
        }
        return;
    }
    quote_ = Quote();
    quote_.station = station;
    quote_.method = panel.method;
    quote_.cost = panel.cost;
    quote_.seconds = panel.seconds;
    quote_.parts = parts;
    quote_.taken = now;
}

bool RefineryOrderBook::apply(const std::string& station, const RefineryScreen& screen, long long now) {
    bool changed = false;
    for (const RefineryPanel& panel : screen.panels) {
        if (panel.kind == RefineryBar::Band::Setup) {
            noteQuote(station, panel, now);
            continue;
        }
        if (!usable(panel)) continue;
        const std::vector<RefineryOrder::Part> parts = partsOf(panel, false);
        if (panel.kind == RefineryBar::Band::Completed) {
            // Done: the order with these parts is done now at the latest. Its number
            // may have moved up, as the one before it was collected (work order 2
            // becomes work order 1).
            RefineryOrder* known = nullptr;
            for (RefineryOrder& order : orders_) {
                if (order.station != station || !overlap(order.parts, parts)) continue;
                if (!known || sameParts(order.parts, parts)) known = &order;
            }
            if (!known) {
                RefineryOrder order;
                order.station = station;
                order.number = panel.order;
                order.parts = parts;
                order.due = now;
                order.seen = now;
                order.completed = now;
                orders_.push_back(order);
                changed = true;
                continue;
            }
            if (known->completed == 0) {
                known->completed = now;
                changed = true;
            }
            if (known->due > now) {
                known->due = now;
                changed = true;
            }
            if (panel.order >= 0 && known->number != panel.order) {
                known->number = panel.order;
                changed = true;
            }
            for (const RefineryOrder::Part& p : parts) {
                auto at = std::find_if(known->parts.begin(), known->parts.end(), [&](const RefineryOrder::Part& k) {
                    return k.material == p.material && k.quality == p.quality;
                });
                if (at == known->parts.end()) {
                    known->parts.push_back(p);
                    changed = true;
                }
            }
            known->seen = now;
            continue;
        }
        const long long due = now + panel.seconds;
        // The order already in the book. A long list scrolls, so one look
        // may show only some of an order's parts and the next look others;
        // what makes it the same order is its station and when it is done -
        // with its number or a part in common to back that up - or, with
        // the clock off, its number and a part in common.
        RefineryOrder* known = nullptr;
        for (RefineryOrder& order : orders_) {
            if (order.station != station) continue;
            const bool sameTime = std::llabs(order.due - due) <= 10;
            const bool sameNumber = order.number >= 0 && order.number == panel.order;
            const bool common = overlap(order.parts, parts);
            if ((sameTime && (sameNumber || common)) || (sameNumber && common)) {
                known = &order;
                break;
            }
        }
        if (known) {
            if (std::llabs(known->due - due) > 2) {
                known->due = due;
                changed = true;
            }
            if (known->number != panel.order) {
                known->number = panel.order;
                changed = true;
            }
            // the parts this look shows, added to the ones seen before
            for (const RefineryOrder::Part& p : parts) {
                auto at = std::find_if(known->parts.begin(), known->parts.end(), [&](const RefineryOrder::Part& k) {
                    return k.material == p.material && k.quality == p.quality;
                });
                if (at == known->parts.end()) {
                    known->parts.push_back(p);
                    changed = true;
                } else if (at->yield != p.yield) {
                    at->yield = p.yield;
                    changed = true;
                }
            }
            known->seen = now;
            continue;
        }
        RefineryOrder order;
        order.station = station;
        order.number = panel.order;
        order.parts = parts;
        order.due = due;
        order.seen = now;
        // Set up here a moment ago: the quote last seen less than a minute
        // before this new order, and the order running no longer than it
        // said and not much less than it said minus the time since. Not by
        // its rows: a long list scrolls, and the quote's view of it and the
        // order's need not have a row in common.
        const long long since = now - quote_.taken;
        if (!quote_.used && quote_.station == station && since >= 0 && since <= 60 &&
            panel.seconds <= quote_.seconds + 2 && panel.seconds >= quote_.seconds - since - 15) {
            order.method = quote_.method;
            order.cost = quote_.cost;
            order.seconds = quote_.seconds;
            order.placed = due - quote_.seconds;
            quote_.used = true;
            // ...and what went in, which only the quote shows: the rows it
            // refines are the order's parts, the ones this look missed too
            for (const RefineryOrder::Part& q : quote_.parts) {
                auto at = std::find_if(order.parts.begin(), order.parts.end(), [&](const RefineryOrder::Part& p) {
                    return p.material == q.material && p.quality == q.quality;
                });
                if (at == order.parts.end()) {
                    order.parts.push_back(q);
                } else if (q.quantity >= 0) {
                    at->quantity = q.quantity;
                }
            }
        }
        orders_.push_back(order);
        changed = true;
    }
    return changed;
}

bool RefineryOrderBook::anyDone(long long now) const {
    for (const RefineryOrder& order : orders_) {
        if (order.completed > 0 || order.due <= now) return true;
    }
    return false;
}

// Collected: after COLLECT is confirmed, "WORK ORDER DELIVERED" comes up
// over the list; the collected order's panel is gone at once and the next
// moves up into its place (work order 2 becomes 1), or the list is empty.
// Each time the notice comes up, one order was collected: the done order of
// the station that two looks in a row do not show, the panels on the list
// each standing for one order, the one with their parts first. Only where
// that is one order - a panel not read may hide another - and not with four
// panels or more on the list, which may hold more than the terminal shows,
// nor on a look away from the list (signing out shows neither panel nor
// notice).
bool RefineryOrderBook::noteCollected(const std::string& station, const RefineryScreen& read, long long now) {
    if (station.empty()) return false;
    if (read.delivered) {
        // up again after a look without it: another order collected
        if (station != deliveredStation_ || !noticeUp_ || now - noticeSeen_ > 3) {
            deliveredStation_ = station;
            deliveredAt_ = now;
            toCollect_ = 1;
            for (RefineryOrder& order : orders_) order.missing = 0;
        }
        noticeSeen_ = now;
    }
    if (station == deliveredStation_) noticeUp_ = read.delivered;
    if (station != deliveredStation_ || now - deliveredAt_ > 30 || toCollect_ <= 0) return false;
    std::vector<const RefineryPanel*> shown;
    for (const RefineryPanel& panel : read.panels) {
        if (usable(panel)) shown.push_back(&panel);
    }
    if (shown.empty() ? !read.delivered : shown.size() >= 4) return false;
    std::vector<bool> matched(orders_.size(), false);
    for (const RefineryPanel* panel : shown) {
        const std::vector<RefineryOrder::Part> parts = partsOf(*panel, false);
        int best = -1;
        for (size_t i = 0; i < orders_.size(); i++) {
            if (matched[i] || orders_[i].station != station) continue;
            if (sameParts(orders_[i].parts, parts)) {
                best = static_cast<int>(i);
                break;
            }
            if (best < 0 && overlap(orders_[i].parts, parts)) best = static_cast<int>(i);
        }
        if (best >= 0) matched[static_cast<size_t>(best)] = true;
    }
    int gone = -1, candidates = 0;
    for (size_t i = 0; i < orders_.size(); i++) {
        RefineryOrder& order = orders_[i];
        if (order.station != station) continue;
        if (matched[i] || (order.completed == 0 && order.due > now)) {
            order.missing = 0;
            continue;
        }
        if (++order.missing >= 2) {
            gone = static_cast<int>(i);
            candidates++;
        }
    }
    if (candidates != 1) return false;   // none yet, or no telling which
    orders_.erase(orders_.begin() + gone);
    toCollect_--;
    return true;
}

bool RefineryOrderBook::take(const std::string& station, const RefineryScreen& read, long long taken) {
    for (const RefineryPanel& panel : read.panels) {
        if (panel.kind == RefineryBar::Band::Setup) noteQuote(station, panel, taken);
    }
    RefineryScreen steady;
    if (station == lastStation_ && taken - lastTaken_ <= 10) {
        for (const RefineryPanel& panel : read.panels) {
            for (const RefineryPanel& before : last_.panels) {
                if (samePanel(panel, taken, before, lastTaken_)) {
                    steady.panels.push_back(panel);
                    break;
                }
            }
        }
    }
    last_ = read;
    lastStation_ = station;
    lastTaken_ = taken;
    const bool applied = !steady.panels.empty() && apply(station, steady, taken);
    const bool collected = noteCollected(station, read, taken);
    return applied || collected;
}

bool RefineryOrder::complete() const {
    if (method.empty() || cost < 0 || seconds <= 0 || placed <= 0 || parts.empty()) return false;
    for (const Part& p : parts) {
        if (p.quantity < 0 || p.yield < 0) return false;
    }
    return true;
}

bool RefineryOrderBook::markHanded(size_t index, long long when) {
    if (index >= orders_.size() || orders_[index].handed > 0) return false;
    orders_[index].handed = when;
    return true;
}

bool RefineryOrderBook::remove(size_t index) {
    if (index >= orders_.size()) return false;
    orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

std::vector<RefineryOrder> RefineryOrderBook::sorted() const {
    std::vector<RefineryOrder> out = orders_;
    std::stable_sort(out.begin(), out.end(), [](const RefineryOrder& a, const RefineryOrder& b) { return a.due < b.due; });
    return out;
}

// One order a line, tab-separated, its parts as material|quality|yield|quantity
// joined by semicolons, then when it was seen done and when it went to UEX:
//   order  Levski  1  1759775562  1759776004  1759775600  Pyrometric Chromalysis  194.00  442  Iron (Ore)|874|235|484  0  0
bool RefineryOrderBook::save(const std::filesystem::path& file) const {
    std::ofstream out(file, std::ios::trunc);
    if (!out) return false;
    out << "bedrock refinery orders 1\n";
    for (const RefineryOrder& o : orders_) {
        char cost[32];
        std::snprintf(cost, sizeof(cost), "%.2f", o.cost);
        out << "order\t" << o.station << "\t" << o.number << "\t" << o.placed << "\t" << o.due << "\t" << o.seen << "\t"
            << o.method << "\t" << cost << "\t" << o.seconds << "\t";
        for (size_t i = 0; i < o.parts.size(); i++) {
            if (i) out << ";";
            out << o.parts[i].material << "|" << o.parts[i].quality << "|" << o.parts[i].yield << "|"
                << o.parts[i].quantity;
        }
        out << "\t" << o.completed << "\t" << o.handed << "\n";
    }
    return static_cast<bool>(out);
}

bool RefineryOrderBook::load(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return false;
    std::vector<RefineryOrder> read;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, '\t')) f.push_back(cell);
        if (f.size() < 10 || f[0] != "order") continue;
        RefineryOrder o;
        o.station = f[1];
        o.number = std::atoi(f[2].c_str());
        o.placed = std::atoll(f[3].c_str());
        o.due = std::atoll(f[4].c_str());
        o.seen = std::atoll(f[5].c_str());
        o.method = f[6];
        o.cost = std::atof(f[7].c_str());
        o.seconds = std::atoi(f[8].c_str());
        std::stringstream parts(f[9]);
        std::string part;
        while (std::getline(parts, part, ';')) {
            // material|quality|yield, and since 2.6.5 |quantity
            std::vector<std::string> p;
            std::stringstream pieces(part);
            std::string piece;
            while (std::getline(pieces, piece, '|')) p.push_back(piece);
            if (p.size() < 3 || p[0].empty()) continue;
            o.parts.push_back({p[0], std::atoi(p[1].c_str()), std::atoi(p[2].c_str()),
                               p.size() > 3 ? std::atoi(p[3].c_str()) : -1});
        }
        if (f.size() > 10) o.completed = std::atoll(f[10].c_str());
        if (f.size() > 11) o.handed = std::atoll(f[11].c_str());
        if (o.station.empty() || o.parts.empty() || o.due <= 0) continue;
        read.push_back(o);
    }
    orders_ = read;
    return true;
}

}  // namespace SCMiner
