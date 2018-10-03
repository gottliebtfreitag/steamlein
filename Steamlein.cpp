#include "Steamlein.h"

#include <simplyfile/Event.h>
#include <simplyfile/Epoll.h>

#include <map>
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
	Dependency(Module* mod) : module(mod) {}
	Dependency(Dependency&& other) noexcept
		: event{std::move(other.event)}
	{
		module = other.module;
		modulesAfter = std::move(other.modulesAfter);
		beforeEdges = other.beforeEdges;
		beforeEdgesToGo = other.beforeEdgesToGo;
		afterEdges = other.afterEdges;
		afterEdgesToGo = other.afterEdgesToGo;
	}
	Dependency& operator=(Dependency&& other) noexcept {
		module = other.module;
		modulesAfter = std::move(other.modulesAfter);
		beforeEdges = other.beforeEdges;
		beforeEdgesToGo = other.beforeEdgesToGo;
		afterEdges = other.afterEdges;
		afterEdgesToGo = other.afterEdgesToGo;
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

struct Edge {
	Dependency* from {nullptr};
	Dependency* to   {nullptr};
	Relation* fromRelation {nullptr};
	Relation* toRelation {nullptr};
	bool inverted {false}; // for recycles
};

struct Steamlein::Pimpl {
	Pimpl() = default;
	Pimpl(std::set<Module*> modules, Epoll& epoll);
	~Pimpl();
	std::vector<Dependency> dependencies;
	std::vector<Edge> edges;
	Epoll* epoll {nullptr};
};

Steamlein::Pimpl::Pimpl(std::set<Module*> modules, Epoll& _epoll)
	: epoll{&_epoll}
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
											prov1->getName() + "@" + removeAnonNamespace(demangle(typeid(*mod))) + " and " +
											prov2->getName() + "@" + removeAnonNamespace(demangle(typeid(*other_mod))) + "\n";
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
								Edge edge {&other_dep, &dep, provide, view, false};

								if (dynamic_cast<BeforeProvideBase*>(view)) {
									std::swap(edge.from, edge.to);
									std::swap(edge.fromRelation, edge.toRelation);
									edge.inverted = true;
								}
								if (edge.from and edge.to) {
									edge.from->addDepAfterThis(edge.to);
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
		dep.beforeEdgesToGo  = dep.beforeEdges;
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
		if (d->beforeEdges == 0) {
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

void Steamlein::setModules(std::set<Module*> modules)
{
	pimpl = std::make_unique<Pimpl>(modules, *this);
}

Steamlein::Steamlein()
	: pimpl(new Pimpl)
{}

Steamlein::~Steamlein()
{}


std::string Steamlein::toDotDescription() const {
	std::stringstream description;
	description << "digraph steamlein {\n";
    description << "node [shape=record];\n";
    description << "graph [pad=\"0.5\", nodesep=\"1\", ranksep=\"2\"];\n";
    description << "rankdir=LR;\n";

	for (auto const& dep : pimpl->dependencies) {
		auto nodeName = removeAnonNamespace(demangle(typeid(*dep.module)));
		description << "\"" << nodeName << "\" [label=\"" << nodeName << " | {";

		std::vector<ProvideBase const*> provides;
		std::vector<ProvideView const*> befores;
		std::vector<ProvideView const*> afters;
		for (auto const& relation : dep.module->getRelations()) {
			if (auto provCast = dynamic_cast<ProvideBase const*>(relation)) {
				provides.emplace_back(provCast);
			} else {
				auto viewCast = dynamic_cast<ProvideView const*>(relation);
				if (dynamic_cast<BeforeProvideBase const*>(relation)) {
					befores.emplace_back(viewCast);
				} else if (dynamic_cast<AfterProvideBase const*>(relation)) {
					afters.emplace_back(viewCast);
				}
			}
		}
		auto renderView = [&](ProvideView const* view) {
			description << "<" << reinterpret_cast<uint64_t>(dynamic_cast<void const*>(view)) << "> " << view->getSelector();
			description << "\\n[" << removeAnonNamespace(demangle(view->getType())) << "] ";
		};
		auto renderProvide = [&](ProvideBase const* prov) {
			description << "<" << reinterpret_cast<uint64_t>(dynamic_cast<void const*>(prov)) << "> " << prov->getName() << " ";
			description << "\\n[	" << removeAnonNamespace(demangle(prov->getType())) << "] ";
		};
		if (not afters.empty()) {
			description << "{ ";
			renderView(afters[0]);
			for (std::size_t i{1}; i < afters.size(); ++i) {
				description << "| ";
				renderView(afters[i]);
			}
			description << "}";
		}
		if (not afters.empty() and (not provides.empty() or not befores.empty())) {
			description << " |";
		}
		if (not provides.empty() or not befores.empty()) {
			description << "{ ";
			if (not provides.empty()) {
				renderProvide(provides[0]);
				for (std::size_t i{1}; i < provides.size(); ++i ) {
					description << "| ";
					renderProvide(provides[i]);
				}
				if (not befores.empty()) {
					description << "| ";
				}
			}
			if (not befores.empty()) {
				renderView(befores[0]);
				for (std::size_t i{1}; i < befores.size(); ++i ) {
					description << "| ";
					renderView(befores[i]);
				}
			}
			description << "}";
		}

		description << "}\"];\n";
	}

	description << "\n";

	for (auto const& edge : pimpl->edges) {
		auto fromNodeName = removeAnonNamespace(demangle(typeid(*edge.from->module)));
		auto toNodeName   = removeAnonNamespace(demangle(typeid(*edge.to->module)));

		description << "\"" << fromNodeName << "\":" << reinterpret_cast<uint64_t>(dynamic_cast<void const*>(edge.fromRelation)) << " -> ";
		description << "\"" << toNodeName << "\":" << reinterpret_cast<uint64_t>(dynamic_cast<void const*>(edge.toRelation));
		if (edge.inverted) {
			description << "[dir=back]";
		}
		description << ";\n";
	}

	description << "}\n";
	return description.str();
}

} /* namespace pipeline */
