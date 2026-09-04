#include "cad/math/Vec3.hpp"

#include <sstream>

namespace cad {

std::string Vec3::toString() const {
    std::ostringstream out;
    out << '(' << x << ", " << y << ", " << z << ')';
    return out.str();
}

}  // namespace cad
