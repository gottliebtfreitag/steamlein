#pragma once

#include "Relations.h"
#include <vector>

#include <stdexcept>

namespace steamlein
{
struct Relation;

// throw this exception from your execute method to indicate that the current module shall be marked as non-executable
struct StopModuleException : std::exception {
private:
    std::string message;

public:
    StopModuleException(std::string what) : message{std::move(what)} {}
    virtual ~StopModuleException() = default;

    const char* what() const noexcept override {
        return message.c_str();
    }
};

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


