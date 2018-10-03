#pragma once

#include "Module.h"
#include <memory>
#include <set>
#include <simplyfile/Epoll.h>

namespace steamlein
{

struct Steamlein : simplyfile::Epoll
{
	Steamlein();

	virtual ~Steamlein();

	void setModules(std::set<Module*> modules);

	std::string toDotDescription() const;
private:
	struct Pimpl;
	std::unique_ptr<Pimpl> pimpl;
};

} /* namespace pipeline */
