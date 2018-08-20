#include "Steamlein.h"

#include <simplyfile/Event.h>
#include <simplyfile/Epoll.h>

#include <map>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <cxxabi.h>
#include <iostream>

namespace steamlein
{
namespace
{

std::string demangle(std::type_info const& ti) {
	int status;
	char* name_ = abi::__cxa_demangle(ti.name(), 0, 0, &status);
	if (status) {
		throw std::runtime_error("cannot demangle a type!");
	}
	std::string demangledName{ name_ };
	free(name_); // need to use free here :/
	return demangledName;
}

}

struct Dependency {
	Dependency(Module* mod) : module(mod) {}
	Dependency(Dependency&& other) noexcept
		: event{std::move(other.event)}
	{
		module = other.module;
		modulesAfter = std::move(other.modulesAfter);
		beforeEdges = other.beforeEdges;
		beforeEdgesToGo = other.beforeEdgesToGo;
	}
	Dependency& operator=(Dependency&& other) noexcept {
		module = other.module;
		modulesAfter = std::move(other.modulesAfter);
		beforeEdges = other.beforeEdges;
		beforeEdgesToGo = other.beforeEdgesToGo;
		event = std::move(other.event);
		return *this;
	}

	// all modules running after this module
	Module* module {};

	std::map<Dependency*, int> modulesAfter;
	std::map<Dependency*, int> modulesBefore;

	std::mutex edgesMutex;
	// how many edges are pointing to this module
	int beforeEdges  {0};
	// how many edges are comming from this module
	int afterEdges {0};

	// for the current iteration how many edges need to be fulfilled till the module can be run
	int beforeEdgesToGo {0};
	int afterEdgesToGo {0};

	bool skipFlag {false};

	simplyfile::Event event{EFD_SEMAPHORE|EFD_NONBLOCK};

	void addDepAfterThis(Dependency* dep) {
		modulesAfter[dep] += 1;
		afterEdges++;

		dep->modulesBefore[this] += 1;
		dep->beforeEdges++;
	}

	void execute() {
		std::exception_ptr eptr;

		beforeEdgesToGo = beforeEdges;
		afterEdgesToGo  = afterEdges;
		if (not skipFlag) {
			try {
				module->execute_();
			} catch (...) {
				// mark all following modules as nonexecutable
				std::function<void(Dependency*)> helper;
				helper = [&](Dependency* dep) {
					dep->skipFlag = true;
					for (auto next : dep->modulesAfter) {
						helper(next.first);
					}
				};
				helper(this);
				try {
					std::throw_with_nested( std::runtime_error("executing " + demangle(typeid(*module)) + " threw an exception:"));
				} catch (...) {
					eptr = std::current_exception();
				}
			}
		}

		skipFlag = false;

		afterEdgesToGo = afterEdges;
		for (auto const& next : modulesAfter) {
			std::unique_lock lock{next.first->edgesMutex};
			next.first->beforeEdgesToGo -= next.second;
			if (0 == next.first->beforeEdgesToGo and 0 == next.first->afterEdgesToGo) {
				next.first->event.put(1);
			}
		}
		for (auto const& before : modulesBefore) {
			std::unique_lock lock{before.first->edgesMutex};
			before.first->afterEdgesToGo -= before.second;
			if (0 == before.first->afterEdgesToGo and 0 == before.first->beforeEdgesToGo) {
				before.first->event.put(1);
			}
		}
		event.get();

		// a module that runs on its own can set itself off immediately
		if (modulesAfter.empty() and modulesBefore.empty()) {
			event.put(1);
		}

		if (eptr) {
			std::rethrow_exception(eptr);
		}
	}
};

struct Steamlein::Pimpl {
	simplyfile::Epoll epoll;

	Pimpl() = default;
	Pimpl(std::set<Module*> modules);

	std::vector<Dependency> dependencies;
};

Steamlein::Pimpl::Pimpl(std::set<Module*> modules)
{
	// test for duplicate provides
	std::string dup_provides_error{""};
	for (auto* mod : modules) {
		for (auto* rel : mod->getRelations()) {
			ProvideBase* prov1 = dynamic_cast<ProvideBase*>(rel);
			if (prov1) {
				for (auto* other_mod : modules) {
					if (other_mod != mod) {
						for (auto* other_rel : other_mod->getRelations()) {
							ProvideBase* prov2 = dynamic_cast<ProvideBase*>(other_rel);
							if (prov2) {
								if (prov1->getName() == prov2->getName() and
										prov1->hasSameTypeAs(prov2)) {
									dup_provides_error += "there are multiple provides with the same type and name!\n" +
											prov1->getName() + "@" + demangle(typeid(*mod)) + " and " +
											prov2->getName() + "@" + demangle(typeid(*other_mod)) + "\n";
								}
							}
						}
					}
				}
			}
		}
	}
	if (dup_provides_error != "") {
		throw std::runtime_error(dup_provides_error);
	}

	for (auto* mod : modules) {
		dependencies.emplace_back(mod);
	}

	// hook the modules together
	for (auto& dep : dependencies) {
		for (auto* rel : dep.module->getRelations()) {
			ProvideView* view = dynamic_cast<ProvideView*>(rel);
			if (view) {
				// look for any other module that provides what is needed
				for (auto& other_dep : dependencies) {
					if (&other_dep == &dep) {
						continue; // Don't self assign
					}
					for (auto* otherRel : other_dep.module->getRelations()) {
						ProvideBase* provide = dynamic_cast<ProvideBase*>(otherRel);
						if (provide) {
							if (view->setProvide(provide)) {
								// insert an a edge
								Dependency* fromDep {&dep};
								Dependency* toDep   {&other_dep};

								if (dynamic_cast<AfterProvideBase*>(view)) {
									std::swap(fromDep, toDep);
								}
								if (fromDep and toDep) {
									fromDep->addDepAfterThis(toDep);
								}
							}
						}
					}
				}
			}
		}
	}

	// setup
	for (auto& dep: dependencies) {
		dep.beforeEdgesToGo  = dep.beforeEdges;
	}

	for (auto& dep: dependencies) {
		Dependency* d = &dep;
		int fd = d->module->getFD();

		auto executor = [=](int) {
			d->execute();
		};
		auto trampoline = [=](int) {
			epoll.modFD(fd, EPOLLIN|EPOLLONESHOT);
		};

		if (fd == -1) {
			epoll.addFD(dep.event, executor, EPOLLIN|EPOLLET);
		} else {
			epoll.addFD(fd, executor, 0);
			epoll.addFD(dep.event, trampoline, EPOLLIN|EPOLLET);
		}
		if (d->beforeEdges == 0) {
			d->event.put(1); // that module can be executed immediately
		}
	}
}

Steamlein::Steamlein(std::set<Module*> modules)
	: Module()
	, pimpl(new Pimpl(modules))
{}

Steamlein::Steamlein()
	: Module()
	, pimpl(new Pimpl)
{}

Steamlein::Steamlein(Steamlein&& other)
	: Module(std::move(other))
{
	std::swap(pimpl, other.pimpl);
}
Steamlein& Steamlein::operator=(Steamlein&& other) {
	Module::operator=(std::move(other));
	pimpl = std::move(other.pimpl);
	return *this;
}

void Steamlein::runOneModule() {
	pimpl->epoll.work(1);
}

int Steamlein::getFD() const {
	return pimpl->epoll;
}

void Steamlein::deinit() {
	for (auto& dep : pimpl->dependencies) {
		dep.module->deinit();
	}
}

Steamlein::~Steamlein()
{}

} /* namespace pipeline */
