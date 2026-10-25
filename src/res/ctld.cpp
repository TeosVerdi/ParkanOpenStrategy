#include "res/ctld.hpp"

#include <cstring>

namespace parkan {

namespace {

template <std::size_t N>
std::array<float, N> floats(Reader& r) {
    std::array<float, N> a{};
    for (auto& v : a) v = r.f32();
    return a;
}

std::string fixed(std::span<const std::uint8_t> b) {
    const auto* p = reinterpret_cast<const char*>(b.data());
    return std::string(p, ::strnlen(p, b.size()));
}

} // namespace

Ctld decode_ctld(std::span<const std::uint8_t> data) {
    Reader r(data);
    Ctld c;
    const std::uint32_t states = r.u32(), contacts = r.u32(), joints = r.u32(), devices = r.u32(), lists = r.u32();
    const std::size_t record = 0x9c + std::size_t(contacts) * 16;
    if (std::size_t(states) * record > r.remaining() || joints * std::size_t(0x24) > r.remaining())
        throw FormatError("CTLD counts exceed the file size");
    c.body = floats<27>(r);

    c.states.resize(states);
    for (auto& s : c.states) {
        s.velocity_mask = r.u32();
        s.flags = r.u32();
        for (auto& b : s.lean) b = r.u8();
        s.track_a = floats<2>(r);
        s.track_b = floats<2>(r);
        s.min_b_weight = r.f32();
        s.duration_ms = r.f32();
        s.lin_min = floats<3>(r);
        s.lin_max = floats<3>(r);
        s.ang_min = floats<3>(r);
        s.ang_max = floats<3>(r);
        s.energy = r.f32();
        s.computed = floats<14>(r);
        s.enter_event_list = r.i32();
        s.uses = r.i32();
        s.mode = r.i32();
        s.contacts.resize(contacts);
        for (auto& k : s.contacts) k = {r.u32(), r.u32(), r.i32(), r.u32()};
    }

    if (std::size_t(states) * states * 4 > r.remaining()) throw FormatError("CTLD weight table truncated");
    c.weights.resize(std::size_t(states) * states);
    for (auto& w : c.weights) w = r.f32();

    c.joints.resize(joints);
    for (auto& j : c.joints) std::memcpy(j.data(), r.bytes(j.size()).data(), j.size());

    c.devices.resize(devices);
    for (auto& d : c.devices) {
        std::memcpy(d.header.data(), r.bytes(d.header.size()).data(), d.header.size());
        std::memcpy(&d.type, d.header.data(), 4);
        std::memcpy(&d.state_override, d.header.data() + 0x18, 4);
        std::uint32_t refs = 0;
        std::memcpy(&refs, d.header.data() + 0xac, 4);
        if (std::size_t(refs) * 4 > r.remaining()) throw FormatError("CTLD device joint list truncated");
        d.joints.resize(refs);
        for (auto& j : d.joints) j = r.u32();
        if (const std::uint32_t len = r.u32()) {
            d.text = r.str(len);
            r.u8(); // NUL
        }
    }

    for (auto& s : c.situations) s = r.i32();

    c.event_lists.resize(lists);
    for (auto& list : c.event_lists) {
        const std::uint32_t n = r.u32();
        if (std::size_t(n) * 0x64 > r.remaining()) throw FormatError("CTLD event list truncated");
        list.resize(n);
        for (auto& e : list) {
            const auto b = r.bytes(0x64);
            std::memcpy(e.raw.data(), b.data(), b.size());
            std::memcpy(&e.flags, b.data(), 4);
            std::memcpy(&e.type, b.data() + 0xc, 4);
            e.lib = fixed(b.subspan(0x24, 32));
            e.entry = fixed(b.subspan(0x44, 32));
        }
    }
    if (!r.at_end()) throw FormatError("trailing bytes after the CTLD event lists");
    return c;
}

} // namespace parkan
