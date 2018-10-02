#pragma once

#include "Relations.h"
#include <vector>

namespace steamlein
{
struct Relation;

struct Module {
private:
	std::vector<Relation*> relations;
	bool initialized {false};
	bool shutdownFlag{false};
public:
	virtual ~Module() = default;

	virtual void executeModule() {};

	void addRelation(Relation* rel) {
		relations.emplace_back(rel);
	}

	auto getRelations() const -> decltype(relations) const& {
		return relations;
	}

	/**
	 *  overload this method if the execution of your module depends on a filedescriptor to become readable
	 *  your module will not be executed until this fd is readable
	 *  the returned FD must not change during the lifetime of your module!
	 */
	virtual int getFD() const { return -1; }
};

}


