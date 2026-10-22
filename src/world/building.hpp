// A building at run time: Terrain.dll CBuilding (ctor 100324d0) over its agent's control system. Its
// CTLD "items" are devices of the agent (IItemManager = the agent's iid 0x202 device set): type 0xc
// doors (CBuilding +0x5c), 0xb (+0x68, never seen to move here) and 0xd the capture door (+0x74, one per
// building at most). The building tick (message 1 of CBuilding::SendMsg 100335d0) runs the door state
// machines and the capture: an object inside the capture volume (10034d70) opens the capture door, and
// when the door is fully open and that object is still inside and alive, the capture callback fires
// (installed by iron3d 10032ef0 through iid 0x17 slot 3 SetCaptureCallback 100359f0; iron3d
// OnBuildingCaptureCallback 10061540 does the owner change). Spec: notes/buildings.md §2–§3.
//
// Port reductions (inferred where marked):
// - contacts: the original keeps the objects in collision contact with the building (a map filled by
//   the contact callback 10034f00: kind 2 enter, 3 leave). The caller passes every live object whose
//   sphere overlaps the building's bounding sphere (inferred equivalent: the capture volume lies inside);
// - doors (type 0xc) open while an object's sphere touches a door part's sphere (10034f00 kind 1 tests
//   a segment against the part sphere; reduced) and close 5 s after opening once nothing holds them;
// - a device channel moves at its joint rate (t per second; the generic device 10020900 steps
//   0.45·speed per update and waits 0.45·1000/rate ms, i.e. a full stroke in 1000/(speed·rate) ms with
//   the header speed +0x24 = 1.0 in every building).
#pragma once

#include "control/turret.hpp"
#include "world/live_unit.hpp"

#include <optional>

namespace parkan {

struct BuildingItem {
    std::uint32_t device = 0;           // CTLD device index
    std::uint32_t type = 0;             // 0xb, 0xc door, 0xd capture door
    std::vector<std::uint32_t> joints;  // its joint refs (CTLD joint indices)
    // CBuilding item record +0x58. Doors: 0 closed, 3 opening, 1 open, 2 closing. Capture door: 0 armed,
    // 2 opening, 1 open (captured or waiting for the capturer to leave), 3 closing, 4 resting (5 s).
    int state = 0;
    float since = 0;                    // +0x5c: when the state was entered (ms)
    bool held = false;                  // doors: +0x6c, an object keeps it open
    // The device channel (generic CDevice +0x94): 0 closed .. 1 open, moving toward `target`.
    float pos = 0, target = 0;
    float speed = 1;                    // device header +0x24 (1.0 in every building)
    bool busy() const { return pos != target; } // device property 0x600 (state ≠ 0)
};

class Building {
public:
    // The building tick's view of an object in contact (CBuilding +0x108: {object, its bounding sphere}).
    struct Contact {
        std::int32_t id = -1;
        Vec3 centre{};
        float radius = 0;
        bool alive = true; // control clan ≠ 0xfffe (the callback checks it)
    };

    explicit Building(std::unique_ptr<LiveUnit> body, std::uint32_t type_code = 0);
    LiveUnit& body() { return *body_; }
    const LiveUnit& body() const { return *body_; }

    const std::vector<BuildingItem>& items() const { return items_; }
    const BuildingItem* capture_door() const { return capture_ < 0 ? nullptr : &items_[std::size_t(capture_)]; }
    bool can_be_captured() const { return capture_ >= 0; }

    // The capture volume's spheres: the capture door's joint parts' mesh spheres in the world (IJointMesh
    // slot 3, space 2), at the current pose.
    struct Sphere {
        Vec3 centre{};
        float radius = 0;
    };
    std::vector<Sphere> capture_spheres() const;
    // 10034d70: an object sphere is inside when, for one capture sphere {c, R}, |p − c|_xy < 0.75·R and
    // c.z − R − r − 4 < p.z < c.z + R (teleports, type 0x80000200: the xy test only).
    // Port deviation: the capture door is a lift (fr_b_plant: its parts sink ~6 m while it opens) that
    // carries the units standing on it, since Control follows the face a unit stands on, building faces
    // included. The port's units follow only the landscape, so the test uses the spheres of the closed
    // door (where a rider stays relative to the lift) instead of the current ones.
    bool in_capture_volume(Vec3 p, float r) const;
    const std::vector<Sphere>& rest_capture_spheres() const { return rest_spheres_; }

    // One building tick at `now_ms`; returns the capturer when the capture callback fires (the caller
    // does iron3d's part: same clan → "enter" the building, else the owner change).
    std::optional<std::int32_t> update(float now_ms, const std::vector<Contact>& contacts);
    // The door channels as joint frames on the body (LiveUnit::device_frames), for pose_parts().
    void apply_frames();

    // A transform of the body (part base + node) that a door or lift item moves: its joints' nodes and
    // their descendants. Port: these are left out of the walls (the original collides with the posed
    // faces; the port's walls are static), so doors never block.
    bool moving_transform(std::size_t transform) const;

    std::int32_t capturer() const { return capturer_; } // CBuilding +0xc8 (−1 none)
    std::uint32_t type_code = 0;
    bool removed = false; // set by the owner when the object left the game (KillGameObject)

private:
    void set_state(BuildingItem& it, int device_state); // IItemManager slot 6: 1 open, 2 close
    void move(float dt_ms);
    std::unique_ptr<LiveUnit> body_;
    std::vector<CtldJoint> joints_;
    std::vector<BuildingItem> items_;
    std::int32_t capture_ = -1;
    std::int32_t capturer_ = -1;
    std::vector<Sphere> rest_spheres_;
    float last_ms_ = -1;
};

} // namespace parkan
