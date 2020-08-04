#pragma once

#include "Module.h"
#include <memory>
#include <set>
#include <simplyfile/Epoll.h>

namespace steamlein
{

struct Edge {
    Module const* from {nullptr};
    Module const* to   {nullptr};
    Relation const* fromRelation {nullptr};
    Relation const* toRelation {nullptr};
};

struct Steamlein 
{
    Steamlein(std::map<Module*, std::string> const& modules, simplyfile::Epoll& epoll);
    ~Steamlein();

    std::vector<Edge> getEdges() const;
private:
    struct Pimpl;
    std::unique_ptr<Pimpl> pimpl;
};

} /* namespace pipeline */
