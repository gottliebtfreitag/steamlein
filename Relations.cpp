#include "Module.h"
#include "Relations.h"

#include <cxxabi.h>

namespace steamlein
{
namespace detail {
// helper classes to walk type_info class structures
template <typename L>
void walk_tree(L const& l, std::type_info const& b);
template <typename L>
void walk_tree(L const& l, __cxxabiv1::__si_class_type_info const& si) {
	l(*si.__base_type);
	walk_tree(l, *si.__base_type);
}
template <typename L>
void walk_tree(L const& l, __cxxabiv1::__vmi_class_type_info const& mi) {
	for (unsigned int i {0}; i < mi.__base_count; ++i) {
		l(*mi.__base_info[i].__base_type);
	}
}
template <typename L>
void walk_tree(L const& l, std::type_info const& b) {
	if (auto si = dynamic_cast<const __cxxabiv1::__si_class_type_info*>(&b); si) {
		walk_tree(l, *si);
	} else if (auto mi = dynamic_cast<const __cxxabiv1::__vmi_class_type_info*>(&b); mi) {
		walk_tree(l, *mi);
	}
}

bool is_ancestor(std::type_info const& deriv, std::type_info const& base) {
	if (base == deriv) {
		return true;
	}
	bool found{false};
	walk_tree([&](auto const& info) {
		found |= (&info == &base);
	}, deriv);
	return found;
}

void const* manual_up_cast(void const* ptr, std::type_info const& deriv, std::type_info const& base) {
	if (deriv == base) {
		return ptr;
	}

	auto baseInfo =  dynamic_cast<__cxxabiv1::__class_type_info const*>(&base);

	if (not baseInfo) {
		return nullptr;
	}
	deriv.__do_upcast(baseInfo, const_cast<void**>(&ptr));
	return ptr;
}

}

Relation::Relation(Module* owner) {
	owner->addRelation(this);
}

}
