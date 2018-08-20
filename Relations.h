#pragma once

#include <string>
#include <regex>

namespace steamlein
{
struct Module;

struct Relation {
	Relation(Module* owner);
	virtual ~Relation() = default;
};

template<typename T, typename T2=void> struct Provide;

struct ProvideBase : Relation {
protected:
	std::string name;
	virtual ~ProvideBase() = default;
public:
	ProvideBase(Module* owner, std::string const& _name) : Relation(owner), name(_name) {}
	auto getName() const -> decltype(name) const& { return name; }

	template<typename T>
	T* as() {
		if constexpr(std::is_class_v<T>) {
			return dynamic_cast<T*>(this);
		} else {
			auto* provide = dynamic_cast<Provide<T>*>(this);
			if (provide) {
				return &(provide->val);
			}
		}
		return nullptr;
	}
	template<typename T>
	T const* as() const {
		if constexpr(std::is_class_v<T>) {
			return dynamic_cast<T const*>(this);
		} else if constexpr(std::is_void<T>::value) {
			return this;
		} else {
			auto const* provide = dynamic_cast<Provide<T> const*>(this);
			if (provide) {
				return &(provide->val);
			}
		}
		return nullptr;
	}

	virtual bool hasSameTypeAs(ProvideBase* other) const = 0;
};


template<>
struct Provide<void> : ProvideBase {
	using ProvideBase::ProvideBase;

	virtual bool hasSameTypeAs(ProvideBase*) const {
		return true;
	};
};

template<typename T>
struct Provide<T, std::enable_if_t<not std::is_class_v<T>>> : ProvideBase {
	T val;
	template<typename... Args>
	Provide(Module* owner, std::string const& _name, Args &&... args)
	: ProvideBase(owner, _name)
	, val(std::forward<Args>(args)...) {}
	operator T&() {
		return val;
	}

	virtual bool hasSameTypeAs(ProvideBase* other) const {
		return dynamic_cast<Provide<T>*>(other);
	};
};

template<typename T>
struct Provide<T, std::enable_if_t<std::is_class_v<T>>> : ProvideBase, T {
	template<typename... Args>
	Provide(Module* owner, std::string const& _name, Args &&... args)
	: ProvideBase(owner, _name)
	, T(std::forward<Args>(args)...) {}
	using T::operator=;

	virtual bool hasSameTypeAs(ProvideBase* other) const {
		return dynamic_cast<Provide<T>*>(other);
	};
};

// a view on a provide
struct ProvideView : Relation {
	ProvideView(Module* owner, std::string const& _regex = ".+")
	: Relation(owner)
	, regex(_regex)
	{}

	virtual bool setProvide(ProvideBase const* provide) = 0;
protected:
	std::regex regex;
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


