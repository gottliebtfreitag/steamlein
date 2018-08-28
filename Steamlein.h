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
	Steamlein(std::set<Module*> modules);
	virtual ~Steamlein();

	void deinit();
private:
	struct Pimpl;
	std::unique_ptr<Pimpl> pimpl;
};

} /* namespace pipeline */
