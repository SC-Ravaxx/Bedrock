#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace SCMiner {

// Where the player is and what they fly, read off the game's own log.
//
// Star Citizen writes both, just not where one would look. The system
// stands in every route the ship's navigation calculates -
//   ... |CSCItemNavigation::CalculateRoute|Projected Start Location is Nyx for route to ...
// and in the loading screen at login
// ("Loading screen for Nyx : SC_Frontend closed after 44.25 seconds"). The
// ship stands in the lines where the PLAYER acts on the navigation -
//   <Player Selected Quantum Target - Local> ... | NOT AUTH | MISC_Prospector_824000000606[...]|CSCItemNavigation:: ...
// (the server-replicated route lines name every ship in range and are
// not used for it) - and once more when the pilot leaves the seat:
//   <Vehicle Control Flow> CVehicleMovementBase::ClearDriver: ... releasing control token for 'MISC_Prospector_824000000606'
// Boarding IS logged, as the ship's crew channel: every hull carries one,
// named after the ship and its owner, and the HUD announces it -
//   <SHUDEvent_OnNotification> Added notification "You have joined channel 'MISC Prospector : <owner>'.
// (and "You have left the channel ..." on the way out). That names the
// ship the player is IN, their own or a friend's, with the game's display
// name. Not every boarding raises it (a ship one logs in aboard does not),
// so the quantum-target lines stay as the second source. The ASOP itself
// logs only the vehicle list it fetched, never which ship was called.
//
// The formats are those of 4.10 and change between patches; a line that
// matches nothing changes nothing, and the app then works as it did
// without the log: system by button, font by the picture.
//
// Every route line names the object container the ship is IN as its
// start - "Projected Start Location is Monox" - and the game calculates a
// route from the moment a quantum target is picked until the drive fires.
// After the jump it says nothing until the next target, so the ARRIVAL
// names the place: the target the player selected
// (`Player has selected point pyro2 as their destination`) is where they
// are once `Quantum Drive has arrived at final destination` follows. A
// jump point is the one target that changes the system, and the gateway
// station on the far side is named for the system LEFT - "Nyx Gateway"
// stands in Pyro - so a gateway seen right after a jump point says which
// system the ship is in now. GamePlaces.h turns the names into systems
// and the ore tables' locations.
//
// The navigation lines name their ship by entity id, and the log carries
// the lines of every ship in range: in 200 logs, 163 of 5,781 route lines
// and 62 of 626 arrival lines were other ships'. The ship the player last
// acted in (picked a target, asked for fuel) is the player's; a line about
// any other ship says nothing. A third source of the place, arrival-side
// like the proximity line: every ten minutes the engine lists each planet
// and moon with its loaded terrain cells, and the one with any is the one
// under the ship (at 87 known places: 63 named that body, 24 none in
// orbit, none another).
class GameState {
public:
    void setGameLog(const std::filesystem::path& log);
    const std::filesystem::path& gameLog() const { return log_; }
    // Whatever the log has grown by. True when the system, the ship, the
    // seat or the place changed.
    bool poll();

    const std::string& system() const { return system_; }   // "Stanton", "Pyro", "Nyx" - "" until seen
    const std::string& ship() const { return ship_; }       // "MISC Prospector", "Drake Golem" - "" until seen
    bool inSeat() const { return inSeat_; }
    // How many times a ship has been boarded since the log began: two of one
    // kind boarded one after the other look the same in everything else
    int boardings() const { return boardings_; }
    // WHICH one of its kind: the entity id the log gives the player's hull,
    // "840000000707" - "" until a target is picked in it or its seat is
    // left. Boarding names only the kind, so after it the hull is unknown
    // again until one of the two - it may be another of the same kind. The
    // id stays with a hull for days, until a claim makes a new one, which is
    // enough for the window to keep a fit per hull.
    std::string hull() const;
    // Hulls destroyed with the player in them, by entity id, in the order the
    // log gave them - since the log was taken up, so a caller keeps its own
    // count of the ones it has seen (a new log starts the list over)
    const std::vector<std::string>& lostHulls() const { return lost_; }
    // The mining gadget stuck on a rock, as the log last said - its class,
    // "Mining_Gadget_SHIN_Sabir", and the rock's, "MineableRock_Asteroid
    // Legendary_Savrilium" - or "" once it is back in the player's hand, or
    // when none was seen. gadgetChanges() counts every time that changed;
    // the stamp is the line's own time.
    const std::string& gadgetOnRock() const { return gadget_; }
    const std::string& gadgetRock() const { return gadgetRock_; }
    const std::string& gadgetStamp() const { return gadgetStamp_; }
    int gadgetChanges() const { return gadgetChanges_; }
    std::string shipLabel() const { return label(ship_); }
    // The place as the log spells it - "Monox", "Levski", "Nyx Gateway",
    // "HUR-L1 Green Glade Station"; "" out in space or until seen
    const std::string& place() const { return place_; }
    // True from the moment a route names the system itself as its start -
    // which the game only does out in open space between the bodies -
    // until any place is named again. Ground deposits cannot be where the
    // ship is then.
    bool inSpace() const { return inSpace_; }
    // The last line that said where the ship is, for anyone of a different
    // opinion - the place set by hand, see Application::applyGameState. A
    // place the tables can put in a system, with that system; or "" and the
    // system alone, when a loading screen or a route out in space named
    // only that. The stamp is the line's own ("2026-09-27T09:27:53", UTC),
    // so a setting made at a known time can tell old lines from new.
    const std::string& namedPlace() const { return namedPlace_; }
    const std::string& namedSystem() const { return namedSystem_; }
    const std::string& namedStamp() const { return namedStamp_; }

    // One log line, everything it says
    struct Event {
        std::string stamp;    // "2026-09-27T09:27:53": the line's own time, UTC; "" without one
        std::string system;   // an explicit system name
        bool systemFromRoute = false;   // ...named by a route as its start: out in space
        std::string ship;
        bool leaving = false;
        std::string place;    // the start location of a route, when it is a place
        std::string target;   // a quantum destination the player picked, internal name
        bool arrived = false; // the quantum drive reached its destination
        std::string zone;     // the body the ship is AT, internal name: the one whose object
                              // container just streamed in, or the one whose terrain the
                              // ten-minute statistics list as loaded
        std::string navShip;  // the ship a navigation line is about, by entity id
        bool playerAct = false;   // ...and the player acting in it: their own ship
        std::string hull;     // the player's own hull, by entity id: a target picked in it, or its seat left
        std::string lost;     // a hull destroyed with the player in it, by entity id
        std::string gadget;        // a mining gadget, by class: stuck on a rock...
        std::string gadgetEntity;  // ...which one, by entity id...
        std::string gadgetRock;    // ...and the rock's class
        bool gadgetInHand = false; // or the gadget back in the player's hand
        std::string location;      // a terminal opened at this place: the inventory request's Location[...]
        bool any() const {
            return !system.empty() || !ship.empty() || !place.empty() || !target.empty() ||
                   !zone.empty() || arrived || !location.empty();
        }
    };
    static bool parse(std::string_view line, Event* out);
    // Feeds one event into the state; true when something changed. Shared
    // by the poll and the --gamelog replay so both see the same thing.
    bool apply(const Event& event);

    // "DRAK_Golem" or "Drake Golem" -> "Drake"; an unknown prefix comes back as it is
    static std::string maker(const std::string& ship);
    // "DRAK_Golem" -> "Drake Golem", "AEGS_Idris_P" -> "Aegis Idris P"; a
    // display name ("MISC Prospector") comes back as it is
    static std::string label(const std::string& ship);
    // "Drake Golem Teach" / "DRAK_Golem" -> "Golem", "Argo MOLE Battaglia"
    // -> "MOLE": the model alone, the way the player says it; makers and
    // editions vary, the HUD does not
    static std::string model(const std::string& ship);
    // One log line. True when it said something; only what it said is
    // written to the outputs (a system line leaves `ship` alone).
    static bool parseLine(std::string_view line, std::string* system, std::string* ship, bool* leaving);

private:
    // Lines from `from` to the end; `midLine` says `from` may sit inside a
    // line, whose rest is then skipped. Returns how many changes they made.
    int readFrom(std::uintmax_t from, std::uintmax_t* end, bool midLine);
    // One event of the player's own ship, into the state
    bool applyOwn(const Event& event);
    bool applyGadget(const Event& event);

    std::filesystem::path log_;
    std::uintmax_t offset_ = 0;
    std::string system_;
    std::string ship_;
    bool inSeat_ = false;
    std::string place_;
    bool inSpace_ = false;   // see inSpace()
    std::string target_;   // the last quantum destination picked, internal name
    std::string jumpTo_;   // the far side of the jump point last arrived at, until seen there
    std::string ownShip_;  // entity id of the ship the player last acted in; "" until seen
    std::string hull_;       // see hull(): the last id a line of the player's own named...
    std::string hullShip_;   // ...and the ship it named with it, which has to be ship_ still
    std::vector<std::string> lost_;   // see lostHulls()
    std::string gadget_, gadgetEntity_, gadgetRock_, gadgetStamp_;   // see gadgetOnRock()
    int gadgetChanges_ = 0;
    int boardings_ = 0;
    // A route line of a ship the player has not acted in yet. A new ship's
    // first route is logged a few hundred milliseconds BEFORE the line that
    // says the player picked its target (69 of 1,030 ship changes in 200
    // logs), so it is held until that line names the same ship - or dropped
    // by whatever comes next.
    Event heldRoute_;
    bool holding_ = false;
    std::string namedPlace_, namedSystem_, namedStamp_;   // see namedPlace()
};

}  // namespace SCMiner
