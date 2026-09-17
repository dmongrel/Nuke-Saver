// The missile of phase 4 (spec 7.1).
//
// A procedural mesh, like everything else here: a cone nose, a cylinder body, a tapered tail and
// four fins, plus the emissive exhaust plume the spec asks for. It is built once per cycle and
// drawn with a model matrix, because it is the only rigid thing in the project that moves.
//
// Built in local space with the nose at the origin and the body running back along -Z, so the
// model matrix is a rotation that takes +Z to the flight direction and nothing else. The missile
// does not roll or pitch relative to its path: spec 7.1 wants it to read as deliberate.
#ifndef NUKE_SAVER_WORLD_MISSILE_H
#define NUKE_SAVER_WORLD_MISSILE_H

#include "core/math.h"
#include "world/mesh.h"

#include <cstdint>

namespace world {

// The mesh reuses world::Vertex, and repurposes its `rockiness` channel: on the missile it is the
// emissive fraction, 0 on the airframe and 1 at the nozzle, falling to 0 down the plume. There is
// no rock on a missile, and adding a fifth channel to a vertex format used by two hundred thousand
// terrain vertices to carry a flag for four hundred missile ones would be the wrong trade.
Mesh BuildMissileMesh(uint64_t seed, float length, float radius);

// The model matrix that puts the local-space missile at `position` pointing along `direction`.
// `direction` must be a unit vector.
core::Mat4 MissileTransform(const core::Vec3& position, const core::Vec3& direction);

}  // namespace world

#endif
