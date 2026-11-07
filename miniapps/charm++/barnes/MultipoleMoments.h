#ifndef MULTIPOLEMOMENTS_H
#define MULTIPOLEMOMENTS_H

#include "Vector3D.h"
#include "defines.h"

class FullTreeNode;
class Particle;

/// A representation of a multipole expansion.
class MultipoleMoments {
public:
	/// A physical size for this multipole expansion, calculated by an external function using some other information
	Real rsq;
	/// The total mass represented by this expansion
	Real totalMass;
	/// The center of mass (zeroth order multipole)
	Vector3D<Real> cm;
        MultipoleMoments() {
          clear();
        }
	
	/// Reset this expansion to nothing
        void clear() {
          rsq = 0;
          totalMass = 0;
          cm.x = cm.y = cm.z = 0;
        }
};

#include "pup.h"
inline void operator|(PUP::er& p, MultipoleMoments& m) {
	p | m.rsq;
	p | m.totalMass;
	p | m.cm;
}
#endif //MULTIPOLEMOMENTS_H
