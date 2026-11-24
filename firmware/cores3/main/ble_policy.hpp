#pragma once
#include <array>
#include <cstddef>

namespace qb {
template<class Peer> class ScopedBondTable {
public:
    struct Bond { bool valid = false; Peer peer{}; };
    std::array<Bond, 2> roles{};
    bool ambiguous = false;
    template<class Equal> bool allows(bool central, const Peer& peer, Equal equal) const {
        const auto& own = roles[central ? 1 : 0];
        const auto& other = roles[central ? 0 : 1];
        return !ambiguous && (!own.valid || equal(own.peer, peer)) &&
            (!other.valid || !equal(other.peer, peer));
    }
    template<class Equal> void reconcile(const Peer* peers, size_t count, bool legacy, Equal equal) {
        if (count > roles.size()) { ambiguous = true; return; }
        if (legacy && count == 1 && !roles[0].valid && !roles[1].valid)
            roles[0] = {true, peers[0]};
        if (roles[0].valid && roles[1].valid && equal(roles[0].peer, roles[1].peer)) ambiguous = true;
        for (size_t i = 0; i < count; ++i) {
            bool known = false;
            for (const auto& role : roles) if (role.valid && equal(role.peer, peers[i])) known = true;
            if (!known) ambiguous = true;
        }
    }
};
inline bool authenticated_link(bool encrypted, bool authenticated, bool bonded, unsigned key_size) {
    return encrypted && authenticated && bonded && key_size == 16;
}
}
