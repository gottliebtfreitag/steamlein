#pragma once

#include "Module.h"
#include <memory>
#include <set>

namespace steamlein
{

struct Steamlein : private Module
{
	Steamlein();
	Steamlein(std::set<Module*> modules);
	Steamlein(Steamlein&&);
	Steamlein& operator=(Steamlein&&);
	virtual ~Steamlein();

	void deinit() override;
	void execute() override { return runOneModule(); }

	/**
	 * execute the next module
	 * can be called from multiple threads for load balancing
	 */
	void runOneModule();

	// get the internally used filedescriptor
	// if this fd is readable a module can be executed
	// use this to hook a Seamlein into an epoll and call runOneModule every time this fd becomes readable
	int getFD() const override;
private:
	struct Pimpl;
	std::unique_ptr<Pimpl> pimpl;
};

} /* namespace pipeline */
