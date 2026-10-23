#include "economy/distributor.hpp"

#include "game/world.hpp"

#include <algorithm>

namespace parkan::economy {

namespace {

bool generator_like(std::uint32_t t) {
    return t == game::kTypeGenerator || t == game::kTypeBunkerSmall || t == game::kTypeBunkerMedium || t == game::kTypeBunkerLarge;
}
bool giver(std::uint32_t t) { return t == game::kTypeMine || t == game::kTypeStorage; }

// The three regimes of Step's ratio block: shares partly served / shares in full and the rest pro rata /
// everyone full and the givers give less.
struct Ratios {
    float a = 0, b = 0, take = 0;
};
Ratios ratios(float g, float w, float ws) {
    if (w <= 0 || g <= 0) return {};
    if (g <= ws) return {g / ws, 0, 1};
    if (g < w) return {1, (g - ws) / (w - ws), 1};
    return {1, 1, w / g};
}

} // namespace

DistributorRecord* Distributor::rec(std::uint32_t lid) {
    for (auto& r : recs_)
        if (r.logic_id == lid) return &r;
    return nullptr;
}

const DistributorRecord* Distributor::find(std::uint32_t lid) const {
    for (const auto& r : recs_)
        if (r.logic_id == lid) return &r;
    return nullptr;
}

void Distributor::normalize(std::uint32_t keep) {
    float fixed = 0, free = 0;
    std::size_t n_free = 0;
    for (const auto& r : recs_) {
        if (r.logic_id == keep || r.locked) fixed += r.share;
        else free += r.share, ++n_free;
    }
    for (auto& r : recs_) {
        if (r.logic_id == keep || r.locked) continue;
        if (free > 0) r.share *= (1 - fixed) / free;
        else r.share = (1 - fixed) / float(n_free);
    }
}

void Distributor::set_share(std::uint32_t lid, float s, int lock) {
    float other_locked = 0;
    for (const auto& r : recs_)
        if (r.logic_id != lid && r.locked) other_locked += r.share;
    s = std::clamp(s, 0.f, 1 - other_locked);
    DistributorRecord* r = rec(lid);
    if (!r) {
        recs_.push_back({lid, 0, false});
        r = &recs_.back();
    }
    r->share = s;
    if (lock != -1) r->locked = lock != 0;
    normalize(lid);
}

void Distributor::add_equal_share(std::uint32_t lid) {
    if (DistributorRecord* r = rec(lid)) {
        r->share = 1.f / float(recs_.size());
    } else {
        recs_.push_back({lid, 1.f / float(recs_.size() + 1), false});
    }
    normalize(lid);
}

void Distributor::remove(std::uint32_t lid) {
    std::erase_if(recs_, [&](const DistributorRecord& r) { return r.logic_id == lid; });
    normalize(0xFFFFFFFF);
}

void Distributor::reset_shares() {
    for (auto& r : recs_) r.share = 1.f / float(recs_.size()), r.locked = false;
}

bool Distributor::takt(std::uint32_t now_ms, const ai::RandomSource& rand) {
    if (!primed_) {
        primed_ = true;
        last_ = now_ms;
        timer_.rearm(now_ms, rand);
        return false;
    }
    if (!timer_.check_and_rearm(now_ms, rand)) return false;
    const float dt = float(now_ms - last_) * 0.001f;
    last_ = now_ms;
    step(dt);
    return true;
}

void Distributor::step(float dt) {
    stepped_ = true;
    float gp = 0, wp = 0, wsp = 0, go = 0, wo = 0, wso = 0, buffer = 0;
    std::vector<EcoState*> objs(recs_.size(), nullptr);
    for (std::size_t i = 0; i < recs_.size(); ++i) {
        DistributorRecord& r = recs_[i];
        r.power_want = r.ore_want = 0;
        EcoState* o = objs[i] = resolve_ ? resolve_(r.logic_id) : nullptr;
        if (!o) continue; // no QI(0x16)/QI(0x204), or clan 0xfffe: the record contributes zeros
        float give_p = 0, want_p = 0, give_o = 0, want_o = 0;
        if (generator_like(o->type)) {
            give_p = o->power_out * dt; // "Generator can give"
        } else {
            const float cap = std::max(o->battery_cap, 0.f);
            want_p = cap * (1 - o->battery_frac); // "Non-Generator want": a deficit, no dt
        }
        if (giver(o->type)) {
            give_o = std::min(o->ore, o->ore_give_rate() * dt);
            buffer += o->ore;
        } else {
            want_o = o->ore_max - o->ore; // "Non-Mine/Storage want"
            if (want_o < 0.01f) want_o = 0, o->ore = o->ore_max;
        }
        r.power_want = want_p, r.ore_want = want_o;
        gp += give_p, wp += want_p, wsp += want_p * r.share;
        go += give_o, wo += want_o, wso += want_o * r.share;
    }
    summary_ = {gp, go, buffer, wp, wo};
    const Ratios p = ratios(gp, wp, wsp), q = ratios(go, wo, wso);
    for (std::size_t i = 0; i < recs_.size(); ++i) {
        DistributorRecord& r = recs_[i];
        EcoState* o = objs[i];
        r.power_recv = r.ore_recv = 0;
        if (!o) continue;
        if (generator_like(o->type)) {
            o->battery_frac = 1.0f; // its batteries are kept full
        } else if (o->battery_cap > 0) {
            const float cap = o->battery_cap, frac = o->battery_frac;
            const float recv = (cap - cap * frac) * (r.share * p.a + (1 - r.share) * p.b);
            o->battery_frac = (cap * frac + recv) / cap; // "Non-Generator received"
            r.power_recv = recv;
        }
        if (giver(o->type)) {
            const float gave = std::min(o->ore, o->ore_give_rate() * dt) * q.take; // "Mine/Storage gave"
            o->ore -= gave;
        } else {
            const float recv = (o->ore_max - o->ore) * (r.share * q.a + (1 - r.share) * q.b);
            o->ore += recv;
            r.ore_recv = recv;
        }
    }
}

} // namespace parkan::economy
