#include "Steamlein.h"

#include <simplyfile/Event.h>
#include <simplyfile/Epoll.h>

#include <map>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <cxxabi.h>
#include <iostream>
#include <atomic>

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

std::string removeAnonNamespace(std::string s) {
	std::string to_replace="(anonymous namespace)::";
	std::size_t start = 0;
	while((start = s.find(to_replace, start)) != std::string::npos) {
	         s.replace(start, to_replace.length(), "");
	}
	return s;
}

}

struct Dependency {
	Dependency(Module* mod, std::string const& name) : module(mod), moduleName{name} {}
	Dependency(Dependency&& other) noexcept
		: moduleName{std::move(other.moduleName)}
		, event{std::move(other.event)}
	{
		module = other.module;
		modulesAfter = std::move(other.modulesAfter);
		beforeEdges = other.beforeEdges;
		afterEdges  = other.afterEdges;
		edgesToGo   = other.edgesToGo.load();
	}
	Dependency& operator=(Dependency&& other) noexcept {
		module = other.module;
		moduleName = other.moduleName;
		modulesAfter = std::move(other.modulesAfter);
		beforeEdges = other.beforeEdges;
		afterEdges  = other.afterEdges;
		edgesToGo   = other.edgesToGo.load();
		event = std::move(other.event);
		return *this;
	}

	Module* module {};
	std::string moduleName;

	// all modules running after this module
	std::unordered_map<Dependency*, int> modulesAfter;
	std::unordered_map<Dependency*, int> modulesBefore;

	// how many edges are pointing to this module
	int beforeEdges  {0};
	// how many edges are comming from this module
	int afterEdges {0};

	// for the current iteration how many edges need to be fulfilled till the module can be run
	std::atomic<int> edgesToGo {0};

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

		edgesToGo = beforeEdges + afterEdges;
		if (not skipFlag) {
			try {
				module->executeModule();
			} catch (...) {
				// to properly propagate an exception throught the steamlein any module that (require-)depends on this module must not be executed
				// the non-executability must be propagated like the normal execution flow
				for (auto next : modulesAfter) {
					next.first->skipFlag = true;
				}
				try {
					std::throw_with_nested( std::runtime_error("executing " + removeAnonNamespace(demangle(typeid(*module))) + " threw an exception:"));
				} catch (...) {
					eptr = std::current_exception();
				}
			}
		} else {
			for (auto const& next : modulesAfter) {
				next.first->skipFlag = true;
			}
		}
		skipFlag = false;

        auto triggerFunc = [](auto& other) {
            int expected = other.first->edgesToGo.load();
            int desired = expected - other.second;
            while (!other.first->edgesToGo.compare_exchange_weak(expected, desired)) {
                desired = expected - other.second;
            } 
			if (0 == desired) {
				other.first->event.put(1);
			}
        };
        std::for_each(begin(modulesAfter), end(modulesAfter), triggerFunc);
        std::for_each(begin(modulesBefore), end(modulesBefore), triggerFunc);

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
	Pimpl() = default;
	Pimpl(std::map<Module*, std::string> const& modules, Epoll& epoll);
	~Pimpl();
	std::vector<Dependency> dependencies;
	std::vector<Edge> edges;
	Epoll* epoll {nullptr};
};

Steamlein::Pimpl::Pimpl(std::map<Module*, std::string> const& modules, Epoll& _epoll)
	: epoll{&_epoll}
{
	// test for duplicate provides
	std::string dup_provides_error{""};
	for (auto const& mod : modules) {
		for (auto* rel : mod.first->getRelations()) {
			ProvideBase* prov1 = dynamic_cast<ProvideBase*>(rel);
			if (prov1) {
				for (auto const& other_mod : modules) {
					if (other_mod.first != mod.first) {
						for (auto* other_rel : other_mod.first->getRelations()) {
							ProvideBase* prov2 = dynamic_cast<ProvideBase*>(other_rel);
							if (prov2) {
								if (prov1->getName() == prov2->getName() and
										prov1->getType() == prov2->getType()) {
									dup_provides_error += "there are multiple provides with the same type and name!\n" +
											prov1->getName() + "@" + mod.second + " and " +
											prov2->getName() + "@" + other_mod.second + "\n";
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

	for (auto const& mod : modules) {
		dependencies.emplace_back(mod.first, mod.second);
	}

	// hook the modules together
	for (auto& dep : dependencies) {
		for (auto* rel : dep.module->getRelations()) {
			ProvideView* view = dynamic_cast<ProvideView*>(rel);
			if (view) {
				// look for any other module that provides what is needed
				for (auto& providing_dep : dependencies) {
					if (&providing_dep == &dep) {
						continue; // Don't self assign
					}
					for (auto const* providing_relation : providing_dep.module->getRelations()) {
						ProvideBase const* provide = dynamic_cast<ProvideBase const*>(providing_relation);
						if (provide) {
							if (view->setProvide(provide)) {
								// insert an a edge
								Edge edge {providing_dep.module, dep.module, provide, view};

								if (dynamic_cast<BeforeProvideBase*>(view)) {
									dep.addDepAfterThis(&providing_dep);
								} else {
									providing_dep.addDepAfterThis(&dep);
								}
								edges.emplace_back(edge);
							}
						}
					}
				}
			}
		}
	}

	// setup
	for (auto& dep: dependencies) {
		dep.edgesToGo  = dep.beforeEdges;
	}

	for (auto& dep: dependencies) {
		Dependency* d = &dep;
		int fd = d->module->getFD();

		auto executor = [=](int) {
			d->execute();
		};
		auto trampoline = [=](int) {
			epoll->modFD(fd, EPOLLIN|EPOLLONESHOT);
		};
		std::string name = removeAnonNamespace(demangle(typeid(*d->module)));
		if (fd == -1) {
			epoll->addFD(dep.event, executor, EPOLLIN|EPOLLET, name);
		} else {
			epoll->addFD(fd, executor, 0, name);
			epoll->addFD(dep.event, trampoline, EPOLLIN|EPOLLET, name + "_trampoline");
		}
		if (d->edgesToGo == 0) {
			d->event.put(1); // that module can be executed immediately
		}
	}
}

Steamlein::Pimpl::~Pimpl() {
	if (epoll) {
		for (auto& dep: dependencies) {
			epoll->rmFD(dep.event, true);
		}
	}
}

void Steamlein::setModules(std::map<Module*, std::string> const& modules)
{
	pimpl = std::make_unique<Pimpl>(modules, *this);
}

Steamlein::Steamlein()
	: pimpl(new Pimpl)
{}

Steamlein::~Steamlein()
{}

std::vector<Edge> Steamlein::getEdges() const {
	return pimpl->edges;
}

} /* namespace pipeline */
