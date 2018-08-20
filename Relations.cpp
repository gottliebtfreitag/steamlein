#include "Module.h"
#include "Relations.h"

namespace steamlein
{


Relation::Relation(Module* owner) {
	owner->addRelation(this);
}

}
