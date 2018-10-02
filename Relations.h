#pragma once

#include <string>
#include <regex>
#include <typeinfo>

namespace steamlein
{
namespace detail {
bool is_ancestor(std::type_info const& deriv, std::type_info const& base);
void const* manual_up_cast(void const* ptr, std::type_info const& deriv, std::type_info const& base);
}

struct Module;

struct Relation {
	Relation(Module* owner);
	virtual ~Relation() = default;
};

template<typename T, typename other=void> struct Provide;

struct ProvideBase : Relation {
protected:
	std::string name;
	virtual ~ProvideBase() = default;

	virtual void const* manualCast(std::type_info const& info) const = 0;
public:
	ProvideBase(Module* owner, std::string const& _name) : Relation(owner), name(_name) {}

	auto getName() const -> decltype(name) const& { return name; }
	virtual std::type_info const& getType() const = 0;

	template<typename T>
	T const* as() const {
		if constexpr (not std::is_class_v<T>) {
			if (auto ptr = dynamic_cast<Provide<T> const*>(this)) {
				return static_cast<T const*>(*ptr);
			}
			return nullptr;
		} else {
			return reinterpret_cast<T const*>(manualCast(typeid(T)));
		}
	}
};


template<>
struct Provide<void> final : ProvideBase {
	using ProvideBase::ProvideBase;

	std::type_info const& getType() const override {
		return typeid(void);
	}

private:
	void const* manualCast(std::type_info const&) const {
		return nullptr;
	}
};

template<typename T>
struct Provide<T> : ProvideBase {
private:
	T val;

	void const* manualCast(std::type_info const& info) const {
		if (detail::is_ancestor(getType(), info)) {
			return detail::manual_up_cast(&val, getType(), info);
		}
		return nullptr;
	}
public:

	template<typename... Args>
	Provide(Module* owner, std::string const& _name, Args &&... args)
	: ProvideBase(owner, _name)
	, val(std::forward<Args>(args)...) {}

	T      * operator->()       { return &val; }
	T const* operator->() const { return &val; }

	T      & operator* ()       { return val; }
	T const& operator* () const { return val; }

	operator T      *()       { return &val; }
	operator T const*() const { return &val; }

	std::type_info const& getType() const override {
		return typeid(T);
	}
};

// a view on a provide
struct ProvideView : Relation {
	ProvideView(Module* owner, std::string const& _regex = ".+")
	: Relation(owner)
	, regex(_regex)
	, selector(_regex)
	{}

	virtual bool setProvide(ProvideBase const* provide) = 0;
	virtual std::type_info const& getType() const = 0;
	std::string const& getSelector() const { return selector; }
protected:
	std::regex regex;
	std::string selector;
};

template<typename T>
struct TypedProvideView : ProvideView {
private:
	T const* val;
public:
	using ProvideView::ProvideView;

	bool setProvide(ProvideBase const* provide) override {
		if (val) {
			// only take the first successful assignment
			return false;
		}
		if (std::regex_match(provide->getName(), regex)) {
			T const* castType = provide->as<T>();
			if (castType) {
				val = castType;
			}
			return castType;
		}
		return false;
	}

	bool valid() const { return val; }
	T const* operator->() { return val;    }
	T const& operator*()  { return *val;   }
	operator T const* ()  { return val;    }
	std::type_info const& getType() const override {
		return typeid(T);
	}
};

template<>
struct TypedProvideView<void> : ProvideView {
private:
	ProvideBase const* val;
public:
	using ProvideView::ProvideView;

	bool setProvide(ProvideBase const* provide) override {
		if (val) {
			// only take the first successful assignment
			return false;
		}
		if (std::regex_match(provide->getName(), regex)) {
			val = provide;
			return true;
		}
		return false;
	}
	bool valid() const { return val; }
	std::type_info const& getType() const override {
		return typeid(void);
	}
};

template<typename T>
struct TypedMultiProvideView : ProvideView {
	using ContainerType = std::vector<T const*>;
private:
	ContainerType vals;
public:
	using ProvideView::ProvideView;
	bool setProvide(ProvideBase const* provide) override {
		if (std::regex_match(provide->getName(), regex)) {
			T const* castType = provide->as<T>();
			if (castType) {
				vals.emplace_back(castType);
			}
			return castType;
		}
		return false;
	}

	ContainerType const& get() const {
		return vals;
	}
	std::type_info const& getType() const override {
		return typeid(T);
	}
};
template<>
struct TypedMultiProvideView<void> : ProvideView {
	using ContainerType = std::vector<ProvideBase const*>;
private:
	ContainerType vals;
public:
	using ProvideView::ProvideView;
	bool setProvide(ProvideBase const* provide) override {
		if (std::regex_match(provide->getName(), regex)) {
			vals.emplace_back(provide);
			return true;
		}
		return false;
	}

	ContainerType const& get() const {
		return vals;
	}
	std::type_info const& getType() const override {
		return typeid(void);
	}
};

struct AfterProvideBase {
	virtual ~AfterProvideBase() = default;
};
struct BeforeProvideBase {
	virtual ~BeforeProvideBase() = default;
};

template<typename T>
struct AfterProvide final : TypedProvideView<T>, AfterProvideBase {
	using TypedProvideView<T>::TypedProvideView;
};
template<typename T>
struct BeforeProvide final : TypedProvideView<T>, BeforeProvideBase {
	using TypedProvideView<T>::TypedProvideView;
};

template<typename T>
struct AfterProvides final : TypedMultiProvideView<T>, AfterProvideBase {
	using TypedMultiProvideView<T>::TypedMultiProvideView;
};
template<typename T>
struct BeforeProvides final : TypedMultiProvideView<T>, BeforeProvideBase {
	using TypedMultiProvideView<T>::TypedMultiProvideView;
};


template<typename T> using Require  = AfterProvide<T>;
template<typename T> using Requires = AfterProvides<T>;
template<typename T> using Recycle  = BeforeProvide<T>;
template<typename T> using Recycles = BeforeProvides<T>;

}


