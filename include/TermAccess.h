﻿// © 2020 Uniontech Software Technology Co.,Ltd.

#ifndef INC_Unilang_TermAccess_h_
#define INC_Unilang_TermAccess_h_ 1

#include "TermNode.h" // for string, TermNode, ystdex::sfmt;
#include <ystdex/typeinfo.h> // for ystdex::type_info;
#include "Exception.h" // for ListTypeError;
#include <ystdex/functional.hpp> // for ystdex::expand_proxy, ystdex::compose_n;

namespace Unilang
{

// NOTE: The host type of symbol.
// XXX: The destructor is not virtual.
class TokenValue final : public string
{
public:
	using base = string;

	TokenValue() = default;
	using base::base;
	TokenValue(const base& b)
		: base(b)
	{}
	TokenValue(base&& b)
	: base(std::move(b))
	{}
	TokenValue(const TokenValue&) = default;
	TokenValue(TokenValue&&) = default;

	TokenValue&
	operator=(const TokenValue&) = default;
	TokenValue&
	operator=(TokenValue&&) = default;
};

[[nodiscard, gnu::pure]] string
TermToString(const TermNode&);

[[nodiscard, gnu::pure]] string
TermToStringWithReferenceMark(const TermNode&, bool);

[[noreturn]] void
ThrowListTypeErrorForInvalidType(const ystdex::type_info&, const TermNode&,
	bool);

[[noreturn]] void
ThrowListTypeErrorForNonlist(const TermNode&, bool);

template<typename _type>
[[nodiscard, gnu::pure]] inline _type*
TryAccessLeaf(TermNode& term)
{
	return term.Value.type() == ystdex::type_id<_type>()
		? std::addressof(term.Value.GetObject<_type>()) : nullptr; 
}
template<typename _type>
[[nodiscard, gnu::pure]] inline const _type*
TryAccessLeaf(const TermNode& term)
{
	return term.Value.type() == ystdex::type_id<_type>()
		? std::addressof(term.Value.GetObject<_type>()) : nullptr; 
}

template<typename _type>
[[nodiscard, gnu::pure]] inline _type*
TryAccessTerm(TermNode& term)
{
	return IsLeaf(term) ? Unilang::TryAccessLeaf<_type>(term) : nullptr;
}
template<typename _type>
[[nodiscard, gnu::pure]] inline const _type*
TryAccessTerm(const TermNode& term)
{
	return IsLeaf(term) ? Unilang::TryAccessLeaf<_type>(term) : nullptr;
}

[[nodiscard, gnu::pure]] inline const TokenValue*
TermToNamePtr(const TermNode& term)
{
	return Unilang::TryAccessTerm<TokenValue>(term);
}


class Environment;

using AnchorPtr = shared_ptr<const void>;


class EnvironmentReference final
	: private ystdex::equality_comparable<EnvironmentReference>
{
private:
	weak_ptr<Environment> p_weak{};
	AnchorPtr p_anchor{};

public:
	EnvironmentReference() = default;
	EnvironmentReference(const shared_ptr<Environment>&) noexcept;
	template<typename _tParam1, typename _tParam2>
	EnvironmentReference(_tParam1&& arg1, _tParam2&& arg2) noexcept
		: p_weak(yforward(arg1)), p_anchor(yforward(arg2))
	{}
	EnvironmentReference(const EnvironmentReference&) = default;
	EnvironmentReference(EnvironmentReference&&) = default;

	EnvironmentReference&
	operator=(const EnvironmentReference&) = default;
	EnvironmentReference&
	operator=(EnvironmentReference&&) = default;

	[[nodiscard, gnu::pure]] friend bool
	operator==(const EnvironmentReference& x, const EnvironmentReference& y)
		noexcept
	{
		return x.p_weak.lock() == y.p_weak.lock();
	}

	[[nodiscard, gnu::pure]] const AnchorPtr&
	GetAnchorPtr() const noexcept
	{
		return p_anchor;
	}

	[[nodiscard, gnu::pure]] const weak_ptr<Environment>&
	GetPtr() const noexcept
	{
		return p_weak;
	}

	shared_ptr<Environment>
	Lock() const noexcept
	{
		return p_weak.lock();
	}
};


// NOTE: The host type of reference values.
class TermReference final : private ystdex::equality_comparable<TermReference>
{
private:
	lref<TermNode> term_ref;
	EnvironmentReference r_env;

public:
	template<typename _tParam, typename... _tParams>
	inline
	TermReference(TermNode& term, _tParam&& arg, _tParams&&... args) noexcept
		: term_ref(term), r_env(yforward(arg), yforward(args)...)
	{}
	TermReference(const TermReference&) = default;

	TermReference&
	operator=(const TermReference&) = default;

	[[nodiscard, gnu::pure]] friend bool
	operator==(const TermReference& x, const TermReference& y) noexcept
	{
		return &x.term_ref.get() == &y.term_ref.get();
	}

	[[nodiscard, gnu::pure]] explicit
	operator TermNode&() const noexcept
	{
		return term_ref;
	}

	bool
	IsMovable() const noexcept
	{
		// TODO: Support unique reference.
		return false;
	}

	[[nodiscard, gnu::pure]] const EnvironmentReference&
	GetEnvironmentReference() const noexcept
	{
		return r_env;
	}

	[[nodiscard, gnu::pure]] TermNode&
	get() const noexcept
	{
		return term_ref.get();
	}
};


[[nodiscard, gnu::pure]] inline TermNode&
ReferenceTerm(TermNode& term)
{
	if(const auto p = Unilang::TryAccessLeaf<const TermReference>(term))
		return p->get();
	return term;
}
[[nodiscard, gnu::pure]] inline const TermNode&
ReferenceTerm(const TermNode& term)
{
	if(const auto p = Unilang::TryAccessLeaf<const TermReference>(term))
		return p->get();
	return term;
}


using ResolvedTermReferencePtr = const TermReference*;

[[nodiscard, gnu::pure]] constexpr ResolvedTermReferencePtr
ResolveToTermReferencePtr(const TermReference* p) noexcept
{
	return p;
}


[[nodiscard, gnu::pure]] inline bool
IsMovable(const TermReference& ref) noexcept
{
	return ref.IsMovable();
}
template<typename _tPointer>
[[nodiscard, gnu::pure]] inline auto
IsMovable(_tPointer p) noexcept
	-> decltype(!bool(p) || Unilang::IsMovable(*p))
{
	return !bool(p) || Unilang::IsMovable(*p);
}


template<typename _type>
[[nodiscard, gnu::pure]] inline _type*
TryAccessReferencedLeaf(TermNode& term)
{
	return Unilang::TryAccessLeaf<_type>(ReferenceTerm(term));
}
template<typename _type>
[[nodiscard, gnu::pure]] inline const _type*
TryAccessReferencedLeaf(const TermNode& term)
{
	return Unilang::TryAccessLeaf<_type>(ReferenceTerm(term));
}


template<typename _type>
[[nodiscard, gnu::pure]] inline _type*
TryAccessReferencedTerm(TermNode& term)
{
	return Unilang::TryAccessTerm<_type>(ReferenceTerm(term));
}
template<typename _type>
[[nodiscard, gnu::pure]] inline const _type*
TryAccessReferencedTerm(const TermNode& term)
{
	return Unilang::TryAccessTerm<_type>(ReferenceTerm(term));
}

template<typename _func, class _tTerm>
auto
ResolveTerm(_func do_resolve, _tTerm&& term)
	-> decltype(ystdex::expand_proxy<void(_tTerm&&,
	ResolvedTermReferencePtr)>::call(do_resolve, yforward(term),
	ResolvedTermReferencePtr()))
{
	using handler_t = void(_tTerm&&, ResolvedTermReferencePtr);

	if(const auto p = Unilang::TryAccessLeaf<const TermReference>(term))
		return ystdex::expand_proxy<handler_t>::call(do_resolve, p->get(),
			Unilang::ResolveToTermReferencePtr(p));
	return ystdex::expand_proxy<handler_t>::call(do_resolve,
		yforward(term), ResolvedTermReferencePtr());
}

template<typename _type, class _tTerm>
void
CheckRegular(_tTerm& term, bool has_ref)
{
	if(IsList(term))
		ThrowListTypeErrorForInvalidType(ystdex::type_id<_type>(), term,
			has_ref);
}

template<typename _type, class _tTerm>
[[nodiscard, gnu::pure]] inline auto
AccessRegular(_tTerm& term, bool has_ref)
	-> decltype(Unilang::Access<_type>(term))
{
	Unilang::CheckRegular<_type>(term, has_ref);
	return Unilang::Access<_type>(term);
}

template<typename _type, class _tTerm>
[[nodiscard, gnu::pure]] inline auto
ResolveRegular(_tTerm& term) -> decltype(Unilang::Access<_type>(term))
{
	return Unilang::ResolveTerm([&](_tTerm& nd, bool has_ref)
		-> decltype(Unilang::Access<_type>(term)){
		return Unilang::AccessRegular<_type>(nd, has_ref);
	}, term);
}


struct ReferenceTermOp
{
	template<typename _type>
	auto
	operator()(_type&& term) const
		ynoexcept_spec(Unilang::ReferenceTerm(yforward(term)))
		-> decltype(Unilang::ReferenceTerm(yforward(term)))
	{
		return Unilang::ReferenceTerm(yforward(term));
	}
};

template<typename _func>
[[gnu::const]] auto
ComposeReferencedTermOp(_func f)
	-> decltype(ystdex::compose_n(f, ReferenceTermOp()))
{
	return ystdex::compose_n(f, ReferenceTermOp());
}

} // namespace Unilang;

#endif

