#pragma once
// Sharing what refinery terminals show (server/refinery in this repository):
// a station's workload - CURRENT CAPACITY and the game's word for it - and a
// work order's quote as it was set up. The game sets a new order's cost by
// that workload, and only the terminal shows it, so with readings from many
// players everyone sees how busy each refinery is. Sent only with the
// player's yes, LIVE's refineries only, and nothing of the player goes along.

#include <map>
#include <string>
#include <vector>

namespace SCMiner {
namespace RefineryShare {

struct Item {
    std::string material;    // as the refinery tables name it
    int quantity = -1;       // cSCU put in
    int quality = -1;
    int yield = -1;          // cSCU it comes out at
};

struct Reading {
    std::string station;     // as the refinery tables name it
    long long observed = 0;  // UTC seconds: when the terminal showed it
    int capacity = -1;       // per cent; -1 not read
    int workload = -1;       // 0 light .. 3 extreme; -1 not read
    // ...and a quote, where an order was set up and read whole
    std::string method;      // the game's key, "DINYX_SOLVENTATION"; "" for none
    double cost = -1;        // aUEC
    int seconds = -1;
    std::vector<Item> items;
};

// The report as the service takes it (server/refinery/README.md). The game's
// version as Game.log's header gives it: 41002 is 4.10.2, 0 leaves it out.
std::string body(const Reading& r, const std::string& bedrockVersion, int gameVersion, const std::string& channel);

// A station's state as the service hands it back
struct StationState {
    int capacity = -1;
    int workload = -1;
    long long observed = 0;
    int sources = 0;         // senders behind it now; 0 for an older reading
    bool confirmed = false;  // two senders agree on it now
};
// The service's /v1/refinery/stations, read; false and `why` for an answer
// that is not one
bool readStations(const std::string& json, std::map<std::string, StationState>& out, std::string& why);

// Whether a station's workload, read again, goes to the service: when it is
// not what was sent last, and otherwise every RESEND_SECONDS
bool worthSending(int capacity, int workload, long long now, int lastCapacity, int lastWorkload, long long lastSent);
constexpr long long RESEND_SECONDS = 600;

}  // namespace RefineryShare
}  // namespace SCMiner
