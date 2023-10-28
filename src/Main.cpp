﻿// SPDX-FileCopyrightText: 2020-2023 UnionTech Software Technology Co.,Ltd.

#include "Interpreter.h" // for string_view, string, Interpreter, ValueObject,
//	YSLib::PolymorphicAllocatorHolder, default_allocator, YSLib::ifstream,
//	YSLib::istringstream, pmr::new_delete_resource_t;
#include <cstdlib> // for std::getenv;
#include "Evaluation.h" // for Unilang::GetModuleFor, RetainN, ValueToken,
//	AssertSubobjectReferenceTerm, IsTyped, SubpairMetadata, BindParameterObject,
//	RegisterStrict;
#include "BasicReduction.h" // for ReductionStatus, LiftToReturn, LiftOther;
#include "Forms.h" // for RetainN, Forms::CallRawUnary, Forms::CallBinaryFold
//	and other form implementations
#include "Context.h" // for BindingMap, Context, Environment,
//	EnvironmentSwitcher, Unilang::SwitchToFreshEnvironment;
#include "TermAccess.h" // for ResolveTerm, ResolvedTermReferencePtr,
//	ThrowValueCategoryError, IsTypedRegular, Unilang::ResolveRegular,
//	ComposeReferencedTermOp, IsReferenceTerm, IsBoundLValueTerm,
//	IsUncollapsedTerm, IsUniqueTerm, EnvironmentReference, TermNode,
//	IsBranchedList, ThrowInsufficientTermsError;
#include <iterator> // for std::next, std::iterator_traits;
#include "Exception.h" // for ThrowNonmodifiableErrorForAssignee,
//	UnilangException, Unilang::GuardExceptionsForAllocator;
#include <functional> // for std::bind, std::placeholders;
#include <ystdex/functor.hpp> // for ystdex::plus, ystdex::equal_to,
//	ystdex::less, ystdex::less_equal, ystdex::greater, ystdex::greater_equal,
//	ystdex::minus, ystdex::multiplies;
#include <regex> // for std::regex, std::regex_match, std::regex_replace;
#include <YSLib/Core/YModules.h>
#include "Math.h" // for NumberLeaf, NumberNode and other math functions;
#include <ystdex/functional.hpp> // for ystdex::bind1;
#include YFM_YSLib_Core_YShellDefinition // for std::to_string,
//	YSLib::make_string_view, YSLib::to_std::string;
#include <iostream> // for std::ios_base, std::cout, std::endl, std::cin;
#include YFM_YSLib_Adaptor_YAdaptor // for YSLib::ufexists, IO::UniqueFile,
//	uopen, IO::use_openmode_t, YSLib::IO, YSLib::FetchEnvironmentVariable,
//	YSLib::uremove;
#include <ystdex/string.hpp> // for ystdex::sfmt;
#include <sstream> // for complete istringstream;
#include <string> // for std::getline;
#include "TCO.h" // for RefTCOAction;
#include <YSLib/Service/YModules.h>
#include <YSLib/Core/YObject.h>
#include <tuple>
#include YFM_YSLib_Service_FileSystem // for IO::CreateDirectory,
//	EnsureDirectory, IO::Path, IO::IsAbsolute;
#include YFM_YSLib_Core_YCoreUtilities // for YSLib::RandomizeTemplateString;
#include <ystdex/cstdio.h> // for ystdex::fexists;
#include <cerrno> // for errno, EEXIST, EPERM;
#include <ystdex/exception.h> // for ystdex::throw_error;
#include <ystdex/base.h> // for ystdex::noncopyable;
#include <random> // for std::random_device, std::mt19937,
//	std::uniform_int_distribution;
#include <cstdlib> // for std::exit;
#include "UnilangFFI.h"
#include <ffi.h>
#include "JIT.h"
#include <ydef.h> // for YPP_Stringize;
#include <ystdex/string.hpp> // for ystdex::quote, ystdex::write_ntcts;
#include <vector> // for std::vector;
#include <array> // for std::array;
#include YFM_YSLib_Core_YException // for YSLib::LoggedEvent,
//	YSLib::FilterExceptions, YSLib::CommandArguments, YSLib::Alert;
#include YFM_YSLib_Core_YCoreUtilities // for YSLib::LockCommandArguments;
#include "UnilangQt.h"

namespace Unilang
{

namespace
{

const bool Unilang_UseJIT(!std::getenv("UNILANG_NO_JIT"));

template<typename _fCallable, typename... _tParams>
inline void
LoadModuleChecked(BindingMap& m, Context& ctx, string_view module_name,
	_fCallable&& f, _tParams&&... args)
{
	Environment::DefineChecked(m, module_name,
		Unilang::GetModuleFor(ctx, yforward(f), yforward(args)...));
}
template<typename _fCallable, typename... _tParams>
inline void
LoadModuleChecked(Environment& env, Context& ctx, string_view module_name,
	_fCallable&& f, _tParams&&... args)
{
	Unilang::LoadModuleChecked(env.GetMapCheckedRef(), ctx, module_name,
		yforward(f), yforward(args)...);
}
template<typename _fCallable, typename... _tParams>
inline void
LoadModuleChecked(Context& ctx, string_view module_name, _fCallable&& f,
	_tParams&&... args)
{
	Unilang::LoadModuleChecked(ctx.GetRecordRef(), ctx, module_name,
		yforward(f), yforward(args)...);
}

YB_ATTR_nodiscard ReductionStatus
DoMoveOrTransfer(void(&f)(TermNode&, TermNode&, bool), TermNode& term)
{
	RetainN(term);
	ResolveTerm([&](TermNode& nd, ResolvedTermReferencePtr p_ref){
		f(term, nd, !p_ref || p_ref->IsModifiable());
		EnsureValueTags(term.Tags);
	}, *std::next(term.begin()));
	return ReductionStatus::Retained;
}

YB_ATTR_nodiscard ReductionStatus
Qualify(TermNode& term, TermTags tag_add)
{
	return Forms::CallRawUnary([&](TermNode& tm){
		if(const auto p = TryAccessLeafAtom<TermReference>(tm))
			p->AddTags(tag_add);
		LiftTerm(term, tm);
		return ReductionStatus::Retained;
	}, term);
}

YB_ATTR_nodiscard TermNode
MoveResolvedValue(const Context& ctx, string_view id)
{
	auto tm(MoveResolved(ctx, id));

	EnsureValueTags(tm.Tags);
	return tm;
}

void
CheckForAssignment(TermNode& nd, ResolvedTermReferencePtr p_ref)
{
	if(p_ref)
	{
		if(p_ref->IsModifiable())
			return;
		ThrowNonmodifiableErrorForAssignee();
	}
	ThrowValueCategoryError(nd);
}

template<typename _func>
YB_ATTR_nodiscard ValueToken
DoAssign(_func f, bool collapse, bool lift, TermNode& x, TermNode& y)
{
	ResolveTerm([&](TermNode& nd, ResolvedTermReferencePtr p_ref){
		CheckForAssignment(nd, p_ref);
		if(p_ref && IsBranch(x))
		{
			AssertSubobjectReferenceTerm(x);
			if(x.size() == 2)
			{
				auto i(x.begin());
				auto& mterm(*++i);

				assert(IsTyped<SubpairMetadata>(mterm)
					&& "Invalid metadata subterm found.");

				auto& mdata(mterm.Value.GetObject<SubpairMetadata>());
				auto& o(mdata.TermRef.get());
				auto& vo(o.Value);
				auto& j(mdata.First);
				auto& tcon(nd.GetContainerRef());

				i = j;
				if(lift)
					LiftToReturn(y);
				tcon.clear();
				BindParameterObject(p_ref->GetEnvironmentReference(),
					mdata.Sigil).BindSubpairPrefix(tcon, y, y.begin(),
					p_ref->GetTags());
				o.erase(i, o.end());
				Unilang::TransferSubtermsAfter(o, y);
				vo = [&]() -> ValueObject{
					if(collapse)
					{
						if(const auto p = TryAccessLeafAtom<TermReference>(y))
							return Collapse(std::move(*p)).first;
					}
					return std::move(y.Value);
				}();
				nd.Value = vo ? vo.MakeIndirect() : ValueObject();
				return;
			}
		}
		f(nd, y);
	}, x);
	return ValueToken::Unspecified;
}

YB_ATTR_nodiscard ReductionStatus
DoResolve(TermNode(&f)(const Context&, string_view), TermNode& term,
	const Context& c)
{
	Forms::CallRegularUnaryAs<const TokenValue>([&](string_view id){
		term = f(c, id);
		EnsureValueTags(term.Tags);
	}, term);
	return ReductionStatus::Retained;
}

TokenValue
StringToSymbol(const string& s)
{
	return TokenValue(s);
}
TokenValue
StringToSymbol(string&& s)
{
	return TokenValue(std::move(s));
}

const string&
SymbolToString(const TokenValue& s) noexcept
{
	return s;
}


struct LeafPred
{
	template<typename _func>
	YB_ATTR_nodiscard YB_PURE bool
	operator()(const TermNode& nd, _func f) const
		noexcept(noexcept(f(nd.Value)))
	{
		return IsLeaf(nd) && f(nd.Value);
	}
};


void
LoadStandardDerived(Interpreter& intp)
{
	// XXX: The derivations depends on 'std.strings' and core functions
	//	derivations available later in the bodies. Otherwise, they are not
	//	relied on. To avoid cyclic dependencies, the derivation of 'ensigl'
	//	further avoids specific core functions.
	intp.Main.ShareCurrentSource("<root:standard-derived>");
	intp.Perform(R"Unilang(
$def! ensigil $lambda (&s)
	$let/e (derive-current-environment std.strings) ()
		$let ((&str symbol->string s))
			$if (string-empty? str) s
				(string->symbol (++ "&" (symbol->string (desigil s))));
	)Unilang");
}

void
LoadModule_std_continuations(Interpreter& intp)
{
	using namespace Forms;
	auto& m(intp.Main.GetRecordRef().GetMapRef());

	RegisterStrict(m, "call/1cc", Call1CC);
	RegisterStrict(m, "continuation->applicative",
		ContinuationToApplicative);
	intp.Perform(R"Unilang(
$defl! apply-continuation (&k &arg)
	apply (continuation->applicative (forward! k)) (forward! arg);
	)Unilang");
}

void
LoadModule_std_promises(Interpreter& intp)
{
	intp.Perform(R"Unilang(
$provide/let! (promise? memoize $lazy $lazy% $lazy/d $lazy/d% force)
((mods $as-environment (
	$def! (encapsulate% promise? decapsulate) () make-encapsulation-type;
	$defl%! do-force (&prom fwd) $let% ((((&o &env) evf) decapsulate prom))
		$if (null? env) (first (first (decapsulate (fwd prom))))
		(
			$let*% ((&y evf (fwd o) env) (&x decapsulate prom)
				(((&o &env) &evf) x))
				$cond
					((null? env) first (first (decapsulate (fwd prom))))
					((promise? y) $sequence
						($if (eqv? (firstv (rest& (decapsulate y))) eval)
							(assign! evf eval))
						(set-first%! x (first (decapsulate (forward! y))))
						(do-force prom fwd))
					(#t $sequence
						(list% (assign%! o (forward! y)) (assign@! env ()))
						(first (first (decapsulate (fwd prom)))))
		)
)))
(
	$import! mods &promise?,
	$defl/e%! &memoize mods (&x)
		encapsulate% (list (list% (forward! x) ()) #inert),
	$defv/e%! &$lazy mods (.&body) d
		encapsulate% (list (list (forward! body) d) eval),
	$defv/e%! &$lazy% mods (.&body) d
		encapsulate% (list (list (forward! body) d) eval%),
	$defv/e%! &$lazy/d mods (&e .&body) d
		encapsulate%
			(list (list (forward! body) (check-environment (eval e d))) eval),
	$defv/e%! &$lazy/d% mods (&e .&body) d
		encapsulate%
			(list (list (forward! body) (check-environment (eval e d))) eval%),
	$defl/e%! &force mods (&x)
		($lambda% (fwd) $if (promise? x) (do-force x fwd) (fwd x))
			($if ($lvalue-identifier? x) id move!)
);
	)Unilang");
}

void
LoadModule_std_strings(Interpreter& intp)
{
	using namespace Forms;
	auto& m(intp.Main.GetRecordRef().GetMapRef());

	RegisterUnary(m, "string?", [](const TermNode& x) noexcept{
		return IsTypedRegular<string>(ReferenceTerm(x));
	});
	RegisterStrict(m, "++",
		std::bind(CallBinaryFold<string, ystdex::plus<>>, ystdex::plus<>(),
		string(), std::placeholders::_1));
	RegisterUnary<Strict, const string>(m, "string-empty?",
		[](const string& str) noexcept{
			return str.empty();
		});
	RegisterBinary(m, "string<-", [](TermNode& x, TermNode& y){
		ResolveTerm([&](TermNode& nd_x, ResolvedTermReferencePtr p_ref_x){
			if(!p_ref_x || p_ref_x->IsModifiable())
			{
				auto& str_x(AccessRegular<string>(nd_x, p_ref_x));

				ResolveTerm(
					[&](TermNode& nd_y, ResolvedTermReferencePtr p_ref_y){
					auto& str_y(AccessRegular<string>(nd_y, p_ref_y));

					if(Unilang::IsMovable(p_ref_y))
						str_x = std::move(str_y);
					else
						str_x = str_y;
				}, y);
			}
			else
				ThrowNonmodifiableErrorForAssignee();
		}, x);
		return ValueToken::Unspecified;
	});
	RegisterBinary<Strict, const string, const string>(m, "string=?",
		ystdex::equal_to<>());
	RegisterBinary<Strict, const string, const string>(m, "string-contains?",
		[](const string& x, const string& y) noexcept{
		return x.find(y) != string::npos;
	});
	RegisterStrict(m, "string-split", [](TermNode& term){
		return CallBinaryAs<string, const string>(
			[&](string& x, const string& y) -> ReductionStatus{
			if(!x.empty())
			{
				TermNode::Container con(term.get_allocator());
				const auto len(y.length());

				if(len != 0)
				{
					string::size_type pos(0), orig(0);

					while((pos = x.find(y, pos)) != string::npos)
					{
						TermNode::AddValueTo(con, x.substr(orig, pos - orig));
						orig = (pos += len);
					}
					TermNode::AddValueTo(con, x.substr(orig, pos - orig));
				}
				else
					TermNode::AddValueTo(con, std::move(x));
				con.swap(term.GetContainerRef());
				return ReductionStatus::Retained;
			}
			return ReductionStatus::Clean;
		}, term);
	});
	RegisterBinary<Strict, const string, const string>(m, "string-contains?",
		[](const string& x, const string& y){
		return x.find(y) != string::npos;
	});
	RegisterBinary<Strict, string, string>(m, "string-contains-ci?",
		[](string x, string y){
		const auto to_lwr([](string& str) noexcept{
			for(auto& c : str)
				c = ystdex::tolower(c);
		});

		to_lwr(x),
		to_lwr(y);
		return x.find(y) != string::npos;
	});
	RegisterUnary(m, "string->symbol", [](TermNode& term){
		return ResolveTerm([&](TermNode& nd, ResolvedTermReferencePtr p_ref){
			auto& s(AccessRegular<string>(nd, p_ref));

			return Unilang::IsMovable(p_ref) ? StringToSymbol(std::move(s))
				: StringToSymbol(s);
		}, term);
	});
	RegisterUnary<Strict, const TokenValue>(m, "symbol->string",
		SymbolToString);
	RegisterUnary<Strict, const string>(m, "string->regex",
		[](const string& str){
		return std::regex(str);
	});
	RegisterStrict(m, "regex-match?", [](TermNode& term){
		RetainN(term, 2);

		auto i(std::next(term.begin()));
		const auto& str(Unilang::ResolveRegular<const string>(*i));
		const auto& r(Unilang::ResolveRegular<const std::regex>(*++i));

		term.Value = std::regex_match(str, r);
		return ReductionStatus::Clean;
	});
	RegisterStrict(m, "regex-replace", [](TermNode& term){
		RetainN(term, 3);

		auto i(term.begin());
		const auto&
			str(Unilang::ResolveRegular<const string>(Unilang::Deref(++i)));
		const auto&
			re(Unilang::ResolveRegular<const std::regex>(Unilang::Deref(++i)));

		return EmplaceCallResultOrReturn(term, string(std::regex_replace(str,
			re, Unilang::ResolveRegular<const string>(Unilang::Deref(++i)))));
	});
}

void
LoadModule_std_math(Interpreter& intp)
{
	using namespace Forms;
	auto& m(intp.Main.GetRecordRef().GetMapRef());

	RegisterUnary(m, "number?",
		ComposeReferencedTermOp(ystdex::bind1(LeafPred(), IsNumberValue)));
	RegisterUnary(m, "real?",
		ComposeReferencedTermOp(ystdex::bind1(LeafPred(), IsNumberValue)));
	RegisterUnary(m, "rational?",
		ComposeReferencedTermOp(ystdex::bind1(LeafPred(), IsRationalValue)));
	RegisterUnary(m, "integer?",
		ComposeReferencedTermOp(ystdex::bind1(LeafPred(), IsIntegerValue)));
	RegisterUnary(m, "exact-integer?",
		ComposeReferencedTermOp(ystdex::bind1(LeafPred(), IsExactValue)));
	RegisterUnary<Strict, const NumberLeaf>(m, "exact?", IsExactValue);
	RegisterUnary<Strict, const NumberLeaf>(m, "inexact?", IsInexactValue);
	RegisterUnary<Strict, const NumberLeaf>(m, "finite?", IsFinite);
	RegisterUnary<Strict, const NumberLeaf>(m, "infinite?", IsInfinite);
	RegisterUnary<Strict, const NumberLeaf>(m, "nan?", IsNaN);
	RegisterBinary<Strict, const NumberLeaf, const NumberLeaf>(m, "=?",
		Equal);
	RegisterBinary<Strict, const NumberLeaf, const NumberLeaf>(m, "<?", Less);
	RegisterBinary<Strict, const NumberLeaf, const NumberLeaf>(m, ">?",
		Greater);
	RegisterBinary<Strict, const NumberLeaf, const NumberLeaf>(m, "<=?",
		LessEqual);
	RegisterBinary<Strict, const NumberLeaf, const NumberLeaf>(m, ">=?",
		GreaterEqual);
	RegisterUnary<Strict, const NumberLeaf>(m, "zero?", IsZero);
	RegisterUnary<Strict, const NumberLeaf>(m, "positive?", IsPositive);
	RegisterUnary<Strict, const NumberLeaf>(m, "negative?", IsNegative);
	RegisterUnary<Strict, const NumberLeaf>(m, "odd?", IsOdd);
	RegisterUnary<Strict, const NumberLeaf>(m, "even?", IsEven);
	RegisterBinary<Strict, NumberNode, NumberNode>(m, "max", Max);
	RegisterBinary<Strict, NumberNode, NumberNode>(m, "min", Min);
	RegisterUnary<Strict, NumberNode>(m, "add1", Add1);
	RegisterBinary<Strict, NumberNode, NumberNode>(m, "+", Plus);
	RegisterUnary<Strict, NumberNode>(m, "sub1", Sub1);
	RegisterBinary<Strict, NumberNode, NumberNode>(m, "-", Minus);
	RegisterBinary<Strict, NumberNode, NumberNode>(m, "*", Multiplies);
	RegisterBinary<Strict, NumberNode, NumberNode>(m, "/", Divides);
	RegisterUnary<Strict, NumberNode>(m, "abs", Abs);
	RegisterBinary<Strict, NumberNode, NumberNode>(m, "floor/",
		FloorDivides);
	RegisterBinary<Strict, NumberNode, NumberNode>(m, "floor-quotient",
		FloorQuotient);
	RegisterBinary<Strict, NumberNode, NumberNode>(m, "floor-remainder",
		FloorRemainder);
	RegisterBinary<Strict, NumberNode, NumberNode>(m, "truncate/",
		TruncateDivides);
	RegisterBinary<Strict, NumberNode, NumberNode>(m, "truncate-quotient",
		TruncateQuotient);
	RegisterBinary<Strict, NumberNode, NumberNode>(m, "truncate-remainder",
		TruncateRemainder);
	RegisterUnary<Strict, NumberNode>(m, "inexact", Inexact);
	RegisterUnary<Strict, const string>(m, "string->number", StringToNumber);
	RegisterUnary<Strict, const NumberNode>(m, "number->string",
		NumberToString);
	RegisterBinary<Strict, const int, const int>(m, "div",
		[](const int& e1, const int& e2){
		if(e2 != 0)
			return e1 / e2;
		throw std::domain_error("Runtime error: divided by zero.");
	});
	RegisterBinary<Strict, const int, const int>(m, "mod",
		[](const int& e1, const int& e2){
		if(e2 != 0)
			return e1 % e2;
		throw std::domain_error("Runtime error: divided by zero.");
	});
	RegisterUnary<Strict, const int&>(m, "itos", [](const int& x){
		return string(YSLib::make_string_view(std::to_string(int(x))));
	});
	RegisterUnary<Strict, const string>(m, "stoi", [](const string& x){
		return int(std::stoi(YSLib::to_std_string(x)));
	});
}

template<typename _tStream>
using GPortHolder = YSLib::PolymorphicAllocatorHolder<std::ios_base, _tStream,
	default_allocator<byte>>;

void
LoadModule_std_io(Interpreter& intp)
{
	using namespace Forms;
	using YSLib::ifstream;
	using YSLib::istringstream;
	auto& m(intp.Main.GetRecordRef().GetMapRef());

	RegisterUnary<Strict, const string>(m, "readable-file?",
		[](const string& str) noexcept{
		return YSLib::ufexists(str.c_str());
	});
	RegisterUnary<Strict, const string>(m, "readable-nonempty-file?",
		[](const string& str) -> bool{
		using namespace YSLib;

		if(IO::UniqueFile file{uopen(str.c_str(), IO::use_openmode_t(),
			std::ios_base::in)})
			return file->GetSize() > 0;
		return {};
	});
	RegisterUnary<Strict, const string>(m, "writable-file?",
		[](const string& str) noexcept{
		return YSLib::ufexists(str.c_str(), "r+b");
	});
	RegisterUnary<Strict, const string>(m, "open-input-file",
		[](const string& path){
		if(ifstream ifs{path, std::ios_base::in | std::ios_base::binary})
			return ValueObject(std::allocator_arg, path.get_allocator(),
				any_ops::use_holder, in_place_type<GPortHolder<ifstream>>,
				std::move(ifs));
		throw UnilangException(
			ystdex::sfmt("Failed opening file '%s'.", path.c_str()));
	});
	RegisterUnary<Strict, const string>(m, "open-input-string",
		[](const string& str){
		if(istringstream iss{str})
			return ValueObject(std::allocator_arg, str.get_allocator(),
				any_ops::use_holder, in_place_type<GPortHolder<istringstream>>,
				std::move(iss));
		throw UnilangException(
			ystdex::sfmt("Failed opening string '%s'.", str.c_str()));
	});
	RegisterStrict(m, "read-line", [](TermNode& term){
		RetainN(term, 0);

		string line(term.get_allocator());

		std::getline(std::cin, line);
		term.SetValue(line);
	});
	RegisterUnary(m, "write", [&](TermNode& term){
		WriteTermValue(std::cout, term);
		return ValueToken::Unspecified;
	});
	RegisterUnary(m, "display", [&](TermNode& term){
		DisplayTermValue(std::cout, term);
		return ValueToken::Unspecified;
	});
	RegisterStrict(m, "newline", [&](TermNode& term){
		RetainN(term, 0);
		std::cout << std::endl;
		return ReduceReturnUnspecified(term);
	});
	RegisterUnary<Strict, const string>(m, "put", [&](const string& str){
		YSLib::IO::StreamPut(std::cout, str.c_str());
		return ValueToken::Unspecified;
	});
	intp.Main.ShareCurrentSource("<lib:std.io>");
	intp.Perform(R"Unilang(
$defl! puts (&s) $sequence (put s) (() newline);
	)Unilang");
	RegisterStrict(m, "load", [&](TermNode& term, Context& ctx){
		RetainN(term);
		RefTCOAction(ctx).SaveTailSourceName(ctx.CurrentSource,
			std::move(ctx.CurrentSource));
		term = intp.Global.ReadFrom(*intp.OpenUnique(ctx, string(
			Unilang::ResolveRegular<const string>(Unilang::Deref(
			std::next(term.begin()))), term.get_allocator())), ctx);
		intp.Global.Preprocess(term);
		return ctx.ReduceOnce.Handler(term, ctx);
	});
	intp.Main.ShareCurrentSource("<lib:std.io-1>");
	intp.Perform(R"Unilang(
$defl! get-module (&filename .&opt)	
	$let ((env $if (null? opt) (() make-standard-environment)
		($let (((&e .&eopt) opt)) $if (null? eopt)
			($let ((env () make-standard-environment)) $sequence
				($set! env module-parameters (check-environemnt e)) env)
			(raise-invalid-syntax-error "Syntax error in get-module."))))
		$sequence (eval% (list load filename) env) env;
	)Unilang");
}

#define APP_VER_MAJOR 0
#define APP_VER_MINOR 13
#define APP_VER_PATCHLEVEL 0
#define APP_VER YPP_Stringize(APP_VER_MAJOR) "." YPP_Stringize(APP_VER_MINOR) \
	"." YPP_Stringize(APP_VER_PATCHLEVEL)

void
LoadModule_std_system(Interpreter& intp)
{
	using namespace Forms;
	namespace IO = YSLib::IO;
	auto& m(intp.Main.GetRecordRef().GetMapRef());

	Environment::DefineChecked(m, "version-string", string(APP_VER));
	RegisterStrict(m, "eval-string", EvalString);
	RegisterStrict(m, "eval-string%", EvalStringRef);
	RegisterUnary<Strict, const string>(m, "env-get", [](const string& var){
		string res(var.get_allocator());

		YSLib::FetchEnvironmentVariable(res, var.c_str());
		return res;
	});
	RegisterUnary<Strict, const string>(m, "remove-file",
		[](const string& filename){
		return YSLib::uremove(filename.c_str());
	});
	RegisterUnary<Strict, const string>(m, "create-directory",
		[](const string& path){
		return IO::CreateDirectory(path);
	});
	RegisterUnary<Strict, const string>(m, "create-directory*",
		[](const string& path){
		return EnsureDirectory(IO::Path(path));
	});
	RegisterUnary<Strict, const string>(m, "create-parent-directory*",
		[](const string& path) -> bool{
		IO::Path pth(path);

		if(!pth.empty())
		{
			pth.pop_back();
			return EnsureDirectory(pth);
		}
		return {};
	});
	RegisterUnary<Strict, const string>(m, "absolute-path?",
		[](const string& path){
		return IO::IsAbsolute(path);
	});
	RegisterBinary<Strict, const string, const string>(m,
		"make-temporary-filename", [](const string& pfx, const string& sfx){
		const string_view tmpl("0123456789abcdef");

		for(size_t n(16); n != 0; --n)
		{
			string str("%%%%%%", pfx.get_allocator());

			str = YSLib::RandomizeTemplateString(str, '%', tmpl);
			str = pfx + std::move(str) + sfx;
			if(ystdex::fexists(str.c_str(), "w+b"))
				return str;

			const int err(errno);

			if(err != EEXIST && err != EPERM)
				continue;
		}

		int err(errno);

		ystdex::throw_error(err, "Failed opening temporary file with the"
			" prefix '" + YSLib::to_std_string(pfx) + '\'');
	});
}

void
LoadModule_std_modules(Interpreter& intp)
{
	intp.Main.ShareCurrentSource("<lib:std.modules>");
	intp.Perform(R"Unilang(
$provide/let! (registered-requirement? register-requirement!
	unregister-requirement! find-requirement-filename require)
((mods $as-environment (
	$import&! std.strings string-empty? ++ string->symbol;

	$defl! requirement-error ()
		raise-error "Empty requirement name found.",
	(
	$def! registry () make-environment;
	$defl! bound-name? (&req)
		$and (eval (list bound? req) registry)
			(not? (null? (eval (string->symbol req) registry))),
	$defl%! get-cell% (&req) first& (eval% (string->symbol req) registry),
	$defl! set-value! (&req &v)
		eval (list $def! (string->symbol req) $quote (forward! v)) registry
	),
	$def! prom_pathspecs ($remote-eval% $lazy std.promises)
		$let ((spec ($remote-eval% env-get std.system) "UNILANG_PATH"))
			$if (string-empty? spec) (list "./?" "./?.u" "./?.txt")
				(($remote-eval% string-split std.strings) spec ";"),
	(
	$def! placeholder ($remote-eval% string->regex std.strings) "\?",
	$defl! get-requirement-filename (&specs &req)
		$if (null? specs)
			(raise-error (++ "No module for requirement '" req "' found."))
			(
				$let* ((spec first& specs) (path ($remote-eval% regex-replace
					std.strings) spec placeholder req))
					$if (($remote-eval% readable-file? std.io) path) path
						(get-requirement-filename (rest& specs) req)
			)
	)
)))
(
	$defl/e! &registered-requirement? mods (&req)
		$if (string-empty? req) (() requirement-error) (bound-name? req),
	$defl/e! &register-requirement! mods (&req)
		$if (string-empty? req) (() requirement-error)
			($if (bound-name? req) (raise-error
				(++ "Requirement '" req "' is already registered."))
				($let ((env () make-standard-environment))
					$sequence (set-value! req (list () env))
						(weaken-environment (move! env)))),
	$defl/e! &unregister-requirement! mods (&req)
		$if (string-empty? req) (() requirement-error)
			($if (bound-name? req) (set-value! req ()) (raise-error
				(++ "Requirement '" req "' is not registered."))),
	$defl/e! &find-requirement-filename mods (&req)
		get-requirement-filename
			(($remote-eval% force std.promises) prom_pathspecs) req;
	$defl/e%! require mods (&req .&opt)
		$if (registered-requirement? req) (get-cell% (forward! req))
			($let*% ((filename find-requirement-filename req)
				(env register-requirement! req) (&res get-cell% (forward! req)))
				$sequence
					($unless (null? opt)
						($set! env module-parameters $let (((&e .&eopt) opt))
							$if (null? eopt) (check-environemnt e)
								(raise-invalid-syntax-error
									"Syntax error in require.")))
					(assign%! res (eval% (list ($remote-eval% load std.io)
						(move! filename)) (move! env)))
					res)
);
	)Unilang");
}

[[gnu::nonnull(2)]] void
PreloadExternal(Interpreter& intp, const char* filename)
{
	auto& ctx(intp.Main);
	const auto& global(ctx.Global.get());

	try
	{
		auto term(global.ReadFrom(*intp.OpenUnique(ctx,
			string(filename, global.Allocator)), ctx));

		intp.Evaluate(term);
	}
	catch(...)
	{
		std::throw_with_nested(UnilangException(
			ystdex::sfmt("Failed loading external unit '%s'.", filename)));
	}
}

struct NoCopy final : ystdex::noncopyable
{};

#define Unilang_Default_Init_File "init.txt"
const char* init_file = Unilang_Default_Init_File;

void
LoadFunctions(Interpreter& intp, bool jit, int& argc, char* argv[])
{
	using namespace Forms;
	using namespace std::placeholders;
	auto& ctx(intp.Main);
	auto& renv(ctx.GetRecordRef());
	auto& m(renv.GetMapRef());

	if(jit)
		SetupJIT(ctx);
	m["ignore"].Value = ValueToken::Ignore;
	RegisterStrict(m, "eq?", Eq);
	RegisterStrict(m, "eql?", EqLeaf);
	RegisterStrict(m, "eqv?", EqValue);
	RegisterForm(m, "$if", If);
	RegisterUnary(m, "null?", ComposeReferencedTermOp(IsEmpty));
	RegisterUnary(m, "branch?", ComposeReferencedTermOp(IsBranch));
	RegisterUnary(m, "pair?", ComposeReferencedTermOp(IsPair));
	RegisterUnary(m, "symbol?", [](const TermNode& x) noexcept{
		return IsTypedRegular<TokenValue>(ReferenceTerm(x));
	});
	RegisterUnary(m, "list?", ComposeReferencedTermOp(IsList));
	RegisterUnary(m, "reference?", IsReferenceTerm);
	RegisterUnary(m, "unique?", IsUniqueTerm);
	RegisterUnary(m, "modifiable?", IsModifiableTerm);
	RegisterUnary(m, "bound-lvalue?", IsBoundLValueTerm);
	RegisterUnary(m, "uncollapsed?", IsUncollapsedTerm);
	RegisterStrict(m, "as-const",
		ystdex::bind1(Qualify, TermTags::Nonmodifying));
	RegisterStrict(m, "expire",
		ystdex::bind1(Qualify, TermTags::Unique));
	RegisterStrict(m, "move!",
		std::bind(DoMoveOrTransfer, std::ref(LiftOtherOrCopy), _1));
	RegisterStrict(m, "__make-nocopy", [](TermNode& term){
		RetainN(term, 0);
		term.Value = NoCopy();
	});
	RegisterBinary(m, "assign@!", [](TermNode& x, TermNode& y){
		return DoAssign(static_cast<void(&)(TermNode&, TermNode&)>(LiftOther),
			{}, {}, x, y);
	});
	RegisterStrict(m, "cons", Cons);
	RegisterStrict(m, "cons%", ConsRef);
	RegisterStrict(m, "set-rest!", SetRest);
	RegisterStrict(m, "set-rest%!", SetRestRef);
	RegisterUnary<Strict, const TokenValue>(m, "desigil", [](TokenValue s){
		return TokenValue(!s.empty() && (s.front() == '&' || s.front() == '%')
			? s.substr(1) : std::move(s));
	});
	RegisterStrict(m, "eval", Eval);
	RegisterStrict(m, "eval@", EvalAt);
	RegisterStrict(m, "eval%", EvalRef);
	RegisterUnary<Strict, const string>(m, "bound?",
		[](const string& id, Context& c){
		return bool(ResolveName(c, id).first);
	});
	RegisterForm(m, "$resolve-identifier",
		std::bind(DoResolve, std::ref(ResolveIdentifier), _1, _2));
	RegisterUnary<Strict, const EnvironmentReference>(m, "lock-environment",
		[](const EnvironmentReference& wenv) noexcept{
		return wenv.Lock();
	});
	RegisterForm(m, "$move-resolved!",
		std::bind(DoResolve, std::ref(MoveResolvedValue), _1, _2));
	RegisterStrict(m, "make-environment", MakeEnvironment);
	RegisterUnary<Strict, const shared_ptr<Environment>>(m,
		"weaken-environment", [](const shared_ptr<Environment>& p_env) noexcept{
		return EnvironmentReference(p_env);
	});
	RegisterForm(m, "$def!", Define);
	RegisterForm(m, "$vau/e", VauWithEnvironment);
	RegisterForm(m, "$vau/e%", VauWithEnvironmentRef);
	RegisterStrict(m, "wrap", Wrap);
	RegisterStrict(m, "wrap%", WrapRef);
	RegisterStrict(m, "unwrap", Unwrap);
	RegisterUnary<Strict, const string>(m, "raise-error",
		[] (const string& str){
		throw UnilangException(str.c_str());
	});
	RegisterUnary<Strict, const string>(m, "raise-invalid-syntax-error",
		[](const string& str){
		throw InvalidSyntax(str.c_str());
	});
	RegisterStrict(m, "check-list-reference", CheckListReference);
	RegisterStrict(m, "check-pair-reference", CheckPairReference);
	RegisterStrict(m, "make-encapsulation-type", MakeEncapsulationType);
	RegisterStrict(m, "get-current-environment", GetCurrentEnvironment);
	RegisterForm(m, "$vau", Vau);
	RegisterForm(m, "$vau%", VauRef);
	intp.Main.ShareCurrentSource("<root:basic-derived-1>");
	intp.Perform(R"Unilang(
$def! lock-current-environment (wrap ($vau () d lock-environment d));
$def! $quote $vau% (x) #ignore $move-resolved! x;
$def! id wrap ($vau% (%x) #ignore $move-resolved! x);
$def! idv wrap $quote;
	)Unilang");
	RegisterStrict(m, "list", ReduceBranchToListValue);
	RegisterStrict(m, "list%", ReduceBranchToList);
	intp.Main.ShareCurrentSource("<root:basic-derived-2>");
	intp.Perform(R"Unilang(
$def! $lvalue-identifier? $vau (&s) d
	eval (list bound-lvalue? (list $resolve-identifier s)) d;
$def! $expire-rvalue $vau% (&s) d
	$if (eval (list $lvalue-identifier? s) d) id expire;
$def! forward! wrap
	($vau% (%x) #ignore $if ($lvalue-identifier? x) x (move! x));
$def! $remote-eval $vau (&o &e) d eval o (eval e d);
$def! $remote-eval% $vau% (&o &e) d eval% o (eval e d);
$def! $set! $vau (&e &formals .&expr) d
	eval (list $def! formals (unwrap eval) expr d) (eval e d);
$def! $wvau $vau (&formals &ef .&body) d
	wrap (eval (cons $vau (cons% (forward! formals) (cons% ef (forward! body))))
		d);
$def! $wvau% $vau (&formals &ef .&body) d
	wrap (eval
		(cons $vau% (cons% (forward! formals) (cons% ef (forward! body)))) d);
$def! $wvau/e $vau (&p &formals &ef .&body) d
	wrap (eval (cons $vau/e
		(cons p (cons% (forward! formals) (cons% ef (forward! body))))) d);
$def! $wvau/e% $vau (&p &formals &ef .&body) d
	wrap (eval (cons $vau/e%
		(cons p (cons% (forward! formals) (cons% ef (forward! body))))) d);
$def! $lambda $vau (&formals .&body) d
	wrap (eval (cons $vau
		(cons% (forward! formals) (cons% #ignore (forward! body)))) d);
$def! $lambda% $vau (&formals .&body) d
	wrap (eval (cons $vau%
		(cons% (forward! formals) (cons% #ignore (forward! body)))) d);
$def! $lambda/e $vau (&p &formals .&body) d
	wrap (eval (cons $vau/e
		(cons p (cons% (forward! formals) (cons% #ignore (forward! body))))) d);
$def! $lambda/e% $vau (&p &formals .&body) d
	wrap (eval (cons $vau/e%
		(cons p (cons% (forward! formals) (cons% #ignore (forward! body))))) d);
	)Unilang");
	RegisterForm(m, "$sequence", Sequence);
	intp.Main.ShareCurrentSource("<root:basic-derived-3>");
	intp.Perform(R"Unilang(
$def! collapse $lambda% (%x)
	$if (uncollapsed? x) (($if ($lvalue-identifier? x) ($lambda% (%x) x) id)
		($if (modifiable? x) (idv x) (as-const (idv x)))) ($move-resolved! x);
$def! assign%! $lambda (&x &y) assign@! (forward! x) (forward! (collapse y));
$def! assign! $lambda (&x &y) assign@! (forward! x) (idv (collapse y));
$def! (list* list*% apply apply-list) ($lambda (&ce)
(
	$def! mods () ($lambda/e ce ()
	(
		$def! l@ $lambda ((.@xs)) xs,
		$def! fwdl $wvau% (f &x) d
			$if (list? x) (f x) (eval% (cons% $if (forward! x)) d),
		$def! mk-list* $lambda% (cons fwd (&head .&tail))
			$if (null? tail) (idv (fwd head))
				(cons (idv (fwd head)) (mk-list* cons fwd (forward! tail))),
		$def! apply-args $lambda% (c fwd (@appv @arg .&opt))
			eval@ (cons% ($if c ($if (list? arg) () $if) ())
				(cons% (unwrap (idv (fwd appv))) (idv (fwd arg))))
				($if (null? opt) (() make-environment)
					(($lambda ((&e .&eopt))
						$if (null? eopt) e
							(raise-invalid-syntax-error
								"Syntax error in applying form.")) opt));
		() lock-current-environment
	));
	$def! list* $lambda/e mods &args
		mk-list* cons ($expire-rvalue args) (fwdl l@ args),
	$def! list*% $lambda/e% mods &args
		mk-list* cons% ($expire-rvalue args) (fwdl l@ args),
	$def! apply $lambda/e% mods &args
		apply-args #f ($expire-rvalue args) (fwdl forward! args),
	$def! apply-list $lambda/e% mods &args
		apply-args #t ($expire-rvalue args) (fwdl forward! args);
	list% (move! list*) (move! list*%) (move! apply) (move! apply-list)
)) (() get-current-environment);
$def! $defv! $vau (&$f &formals &ef .&body) d
	eval (list*% $def! $f $vau (forward! formals) ef (forward! body)) d;
$defv! $defv%! (&$f &formals &ef .&body) d
	eval (list*% $def! $f $vau% (forward! formals) ef (forward! body)) d;
$defv! $defv/e! (&$f &p &formals &ef .&body) d
	eval (list*% $def! $f $vau/e p (forward! formals) ef (forward! body)) d;
$defv! $defv/e%! (&$f &p &formals &ef .&body) d
	eval (list*% $def! $f $vau/e% p (forward! formals) ef (forward! body)) d;
$defv! $defw! (&f &formals &ef .&body) d
	eval (list*% $def! f $wvau (forward! formals) ef (forward! body)) d;
$defv! $defw%! (&f &formals &ef .&body) d
	eval (list*% $def! f $wvau% (forward! formals) ef (forward! body)) d;
$defv! $defw/e! (&f &p &formals &ef .&body) d
	eval (list*% $def! f $wvau/e p (forward! formals) ef (forward! body)) d;
$defv! $defw/e%! (&f &p &formals &ef .&body) d
	eval (list*% $def! f $wvau/e% p (forward! formals) ef (forward! body)) d;
$defv! $defl! (&f &formals .&body) d
	eval (list*% $def! f $lambda (forward! formals) (forward! body)) d;
$defv! $defl%! (&f &formals .&body) d
	eval (list*% $def! f $lambda% (forward! formals) (forward! body)) d;
$defv! $defl/e! (&f &p &formals .&body) d
	eval (list*% $def! f $lambda/e p (forward! formals) (forward! body)) d;
$defv! $defl/e%! (&f &p &formals .&body) d
	eval (list*% $def! f $lambda/e% p (forward! formals) (forward! body)) d;
$defw%! forward-first% (&appv (&x .)) d
	apply (forward! appv) (list% ($move-resolved! x)) d;
$defl%! first (&pr)
	$if ($lvalue-identifier? pr) (($lambda% ((@x .)) $if (uncollapsed? x)
		($if (modifiable? x) (idv x) (as-const (idv x))) x) pr)
		(forward-first% idv (expire pr));
$defl%! first@ (&pr) ($lambda% ((@x .))
	$if (unique? ($resolve-identifier pr)) (expire x) x)
	($if (unique? ($resolve-identifier pr)) pr
		(check-pair-reference (forward! pr)));
$defl%! first% (&pr) ($lambda (fwd (@x .)) fwd x) ($expire-rvalue pr) pr;
$defl%! first& (&pr)
	($lambda% ((@x .)) $if (uncollapsed? x)
		($if (modifiable? pr) (idv x) (as-const (idv x)))
		($if (unique? ($resolve-identifier pr)) (expire x) x))
	($if (unique? ($resolve-identifier pr)) pr
		(check-pair-reference (forward! pr)));
$defl! firstv ((&x .)) $move-resolved! x;
$defl%! rest% ((#ignore .%xs)) $move-resolved! xs;
$defl%! rest& (&pr)
	($lambda% ((#ignore .&xs)) $if (unique? ($resolve-identifier pr))
		(expire xs) ($resolve-identifier xs))
	($if (unique? ($resolve-identifier pr)) pr
		(check-pair-reference (forward! pr)));
$defl! restv ((#ignore .xs)) $move-resolved! xs;
$defl! set-first! (&pr x) assign@! (first@ (forward! pr)) (move! x);
$defl! set-first%! (&pr &x) assign%! (first@ (forward! pr)) (forward! x);
$defl! equal? (&x &y)
	$if ($if (pair? x) (pair? y) #f)
		($if (equal? (first& x) (first& y)) (equal? (rest& x) (rest& y)) #f)
		(eqv? x y);
$defl%! check-environment (&e) $sequence (eval@ #inert e) (forward! e);
$defv%! $cond &clauses d
	$if (null? clauses) #inert
		(apply-list ($lambda% ((&test .&body) .&clauses)
			$if (eval test d) (eval% (forward! body) d)
			(apply (wrap $cond) (forward! clauses) d)) (forward! clauses));
$defv%! $when (&test .&exprseq) d
	$if (eval test d) (eval% (list* () $sequence (forward! exprseq)) d);
$defv%! $unless (&test .&exprseq) d
	$if (eval test d) #inert (eval% (list* () $sequence (forward! exprseq)) d);
$defv%! $while (&test .&exprseq) d
	$when (eval test d)
		(eval% (list* () $sequence exprseq) d)
		(eval% (list* () $while (forward! test) (forward! exprseq)) d);
$defv%! $until (&test .&exprseq) d
	$unless (eval test d)
		(eval% (list* () $sequence exprseq) d)
		(eval% (list* () $until (forward! test) (forward! exprseq)) d);
$defl! not? (x) eqv? x #f;
$defv%! $and &x d
	$cond
		((null? x) #t)
		((null? (rest& x)) eval% (first (forward! x)) d)
		((eval% (first& x) d) eval% (cons% $and (rest% (forward! x))) d)
		(#t #f);
$defv%! $or &x d
	$cond
		((null? x) #f)
		((null? (rest& x)) eval% (first (forward! x)) d)
		(#t ($lambda% (&r) $if r (forward! r) (eval%
			(cons% $or (rest% (forward! x))) d)) (eval% (move! (first& x)) d));
$defl%! and &x $sequence
	($defl%! and-aux (&h &l) $if (null? l) (forward! h)
		(and-aux ($if h (forward! (first% l)) #f) (forward! (rest% l))))
	(and-aux #t (forward! x));
$defl%! or &x $sequence
	($defl%! or-aux (&h &l) $if (null? l) (forward! h)
		($let% ((&c forward! (first% l)))
			or-aux (forward! ($if c c h)) (forward! (rest% l))))
	(or-aux #f (forward! x));
$defw%! accl (&l &pred? &base &head &tail &sum) d
	$if (apply pred? (list% l) d) (forward! base)
		(apply accl (list% (apply tail (list% l) d) pred?
			(apply sum (list% (apply head (list% l) d)
			(forward! base)) d) head tail sum) d);
$defw%! accr (&l &pred? &base &head &tail &sum) d
	$if (apply pred? (list% l) d) (forward! base)
		(apply sum (list% (apply head (list% l) d)
			(apply accr (list% (apply tail (list% l) d)
			pred? (forward! base) head tail sum) d)) d);
$defw%! foldr1 (&kons &knil &l) d
	apply accr (list% (($lambda ((.@xs)) xs) (check-list-reference l)) null?
		(forward! knil) ($if ($lvalue-identifier? l) ($lambda (&l) first% l)
		($lambda (&l) expire (first% l))) rest% kons) d;
$defw%! map1 (&appv &l) d
	foldr1 ($lambda (%x &xs) cons%
		(apply appv (list% ($move-resolved! x)) d) (move! xs)) () (forward! l);
$defl! list-concat (&x &y) foldr1 cons% (forward! y) (forward! x);
$defl! append (.&ls) foldr1 list-concat () (move! ls);
$defl! filter (&accept? &ls) apply append
	(map1 ($lambda (&x) $if (apply accept? (list x)) (list x) ()) ls);
$defl%! list-extract-first (&l) map1 first (forward! l);
$defl%! list-extract-rest% (&l) map1 rest% (forward! l);
$def! ($let $let% $let/e $let/e% $let* $let*% $letrec $letrec%
	$bindings/p->environment) ($lambda (&ce)
(
	$def! mods () ($lambda/e ce ()
	(
		$defl%! rulist (&l)
			$if ($lvalue-identifier? l)
				(accr (($lambda ((.@xs)) xs) (check-list-reference l)) null? ()
					($lambda% (%l) $sequence ($def! %x idv (first@ l))
						(($if (uncollapsed? x) idv expire) (expire x))) rest%
					($lambda (%x &xs) (cons% ($resolve-identifier x)
						(move! xs))))
				(idv (forward! (check-list-reference l)));
		$defv%! $lqual (&ls) d
			($if (eval (list $lvalue-identifier? ls) d) id rulist) (eval% ls d),
		$defv%! $lqual* (&x) d (eval (list $expire-rvalue x) d) (eval% x d);
		$defl%! mk-let (&$ctor &bindings &body)
			list*% () (list*% $ctor (list-extract-first bindings)
				(list% (forward! body))) (list-extract-rest% bindings),
		$defl%! mk-let/e (&$ctor &p &bindings &body)
			list*% () (list*% $ctor p (list-extract-first bindings)
				(list% (forward! body))) (list-extract-rest% bindings),
		$defl%! mk-let* (&$l &$l* &bindings &body)
			$if (null? bindings) (list*% $l () (forward! body))
				(list% $l (list% (first% ($lqual* bindings)))
				(list*% $l* (rest% ($lqual* bindings)) (forward! body))),
		$defl%! mk-letrec (&$l &bindings &body)
			list% $l () $sequence (list% $def! (list-extract-first bindings)
				(list*% () list% (list-extract-rest% bindings)))
				(forward! body);
		() lock-current-environment
	));
	$defv/e%! $let mods (&bindings .&body) d
		eval% (mk-let $lambda ($lqual bindings) (forward! body)) d,
	$defv/e%! $let% mods (&bindings .&body) d
		eval% (mk-let $lambda% ($lqual bindings) (forward! body)) d,
	$defv/e%! $let* mods (&bindings .&body) d
		eval% (mk-let* $let $let* ($lqual* bindings) (forward! body)) d,
	$defv/e%! $let*% mods (&bindings .&body) d
		eval% (mk-let* $let% $let*% ($lqual* bindings) (forward! body)) d,
	$defv/e%! $let/e mods (&p &bindings .&body) d
		eval% (mk-let/e $lambda/e p ($lqual bindings) (forward! body)) d,
	$defv/e%! $let/e% mods (&p &bindings .&body) d
		eval% (mk-let/e $lambda/e% p ($lqual bindings) (forward! body)) d,
	$defv/e%! $letrec mods (&bindings .&body) d
		eval% (mk-letrec $let ($lqual bindings) (forward! body)) d,
	$defv/e%! $letrec% mods (&bindings .&body) d
		eval% (mk-letrec $let% ($lqual bindings) (forward! body)) d,
	$defv/e! $bindings/p->environment mods (&parents .&bindings) d $sequence
		($def! (res bref) list (apply make-environment
			(map1 ($lambda% (x) eval% x d) parents)) (rulist bindings))
		(eval% (list $set! res (list-extract-first bref)
			(list* () list (list-extract-rest% bref))) d)
		res;
	map1 move! (list% $let $let% $let/e $let/e% $let* $let*% $letrec $letrec%
		$bindings/p->environment)
)) (() get-current-environment);
$defw! derive-current-environment (.&envs) d
	apply make-environment (append envs (list d)) d;
$defl! make-standard-environment () () lock-current-environment;
$def! derive-environment ()
	($vau () d $lambda/e (() lock-current-environment) (.&envs)
		() ($lambda/e (append envs (list d)) () () lock-current-environment));
$defv! $as-environment (.&body) d
	eval (list $let () (list $sequence (forward! body)
		(list () lock-current-environment))) d;
$defv! $bindings->environment (.&bindings) d
	eval (list* $bindings/p->environment () (forward! bindings)) d;
$defl! symbols->imports (&symbols)
	list* () list% (map1 ($lambda (&s) list forward! (desigil s))
		(forward! symbols));
$defv! $provide/let! (&symbols &bindings .&body) d
	eval% (list% $let (forward! bindings) $sequence
		(forward! body) (list% $set! d (append symbols ((unwrap list%) .))
		(symbols->imports symbols)) (list () lock-current-environment)) d;
$defv! $provide! (&symbols .&body) d
	eval (list*% $provide/let! (forward! symbols) () (forward! body)) d;
$defv! $import! (&e .&symbols) d
	eval% (list $set! d (append symbols ((unwrap list%) .))
		(symbols->imports symbols)) (eval e d);
$defv! $import&! (&e .&symbols) d
	eval% (list $set! d (append (map1 ensigil symbols)
		((unwrap list%) .)) (symbols->imports symbols)) (eval e d);
$defl! nonfoldable? (&l)
	$if (null? l) #f ($if (null? (first l)) #t (nonfoldable? (rest& l)));
$defl%! assq (&x &alist) $cond ((null? alist))
	((eq? x (first& (first& alist))) first% alist)
	(#t assq (forward! x) (rest% alist));
$defl%! assv (&x &alist) $cond ((null? alist))
	((eqv? x (first& (first& alist))) first% alist)
	(#t assv (forward! x) (rest% alist));
$defw%! map-reverse (&appv .&ls) d
	accl (forward! (check-list-reference ls)) nonfoldable? () list-extract-first
		list-extract-rest%
		($lambda (&x &xs) cons% (apply appv (forward! x) d) (forward! xs));
$defw! for-each-ltr &ls d $sequence (apply map-reverse (forward! ls) d) #inert;
	)Unilang");
	LoadStandardDerived(intp);
	// NOTE: Supplementary functions.
	RegisterStrict(m, "random.choice", [&](TermNode& term){
		RetainN(term);
		return ResolveTerm([&](TermNode& nd, ResolvedTermReferencePtr p_ref){
			if(IsBranchedList(nd))
			{
				static std::random_device rd;
				static std::mt19937 mt(rd());

				LiftOtherOrCopy(term, *std::next(nd.begin(),
					std::iterator_traits<TermNode::iterator>::difference_type(
					std::uniform_int_distribution<size_t>(0, nd.size() - 1)(mt)
					)), Unilang::IsMovable(p_ref));
				return ReductionStatus::Retained;
			}
			ThrowInsufficientTermsError(nd, p_ref);
		}, *std::next(term.begin()));
	});
	RegisterUnary<Strict, const int>(m, "sys.exit", [&](int status){
		std::exit(status);
	});

	auto& rctx(intp.Main);
	const auto load_std_module([&](string_view module_name,
		void(&load_module)(Interpreter& intp)){
		LoadModuleChecked(rctx, "std." + string(module_name),
			std::bind(load_module, std::ref(intp)));
	});

	load_std_module("continuations", LoadModule_std_continuations);
	load_std_module("promises", LoadModule_std_promises);
	load_std_module("strings", LoadModule_std_strings);
	load_std_module("math", LoadModule_std_math);
	load_std_module("io", LoadModule_std_io);
	load_std_module("system", LoadModule_std_system);
	load_std_module("modules", LoadModule_std_modules);
	// NOTE: Additional standard library initialization.
	PreloadExternal(intp, "std.txt");
	// NOTE: FFI and external libraries support.
	InitializeFFI(intp);
	// NOTE: Qt support.
	InitializeQt(intp, argc, argv);
	// NOTE: Prevent the ground environment from modification.
	renv.Freeze();
	intp.SaveGround();
	// NOTE: User environment initialization.
	if(init_file)
		PreloadExternal(intp, init_file);
}

template<class _tString>
auto
Quote(_tString&& str) -> decltype(ystdex::quote(yforward(str), '\''))
{
	return ystdex::quote(yforward(str), '\'');
}


const struct Option
{
	const char *prefix, *option_arg;
	std::vector<const char*> option_details;

	Option(const char* pfx, const char* opt_arg,
		std::initializer_list<const char*> il)
		: prefix(pfx), option_arg(opt_arg), option_details(il)
	{}
} OptionsTable[]{
	{"-h, --help", "", {"Print this message and exit, without entering"
		" execution modes."}},
	{"-e", " [STRING]", {"Evaluate a string if the following argument exists."
		" This option can occur more than once and combined with SRCPATH.\n"
		"\tAny instance of this option implies the interpreter running in"
		" scripting mode.\n"
		"\tEach instance of this option (with its optional argument) will be"
		" evaluated in order before evaluate the script specified by SRCPATH"
		" (if any)."}},
	{"-q, --no-init-file", "", {"Disable loading the init file. Otherwise, a"
		" file named \"" Unilang_Default_Init_File "\" is loaded at the end"
		" of the initialization and before further evaluations. Currently this"
		" is effective for both execution modes."}}
};

const std::array<const char*, 3> DeEnvs[]{
	{{"ECHO", "", "If set, echo the result after evaluating each unit."}},
	{{"UNILANG_NO_JIT", "", "If set, disable any optimization based on the JIT"
		" (just-in-time) execution."}},
	{{"UNILANG_NO_SRCINFO", "", "If set, disable the source information from"
		" the source code for diagnostics. The source names are used"
		" regardless of this variable."}},
	{{"UNILANG_PATH", "", "Unilang loader path template string."}}
};


void
PrintHelpMessage(const string& prog)
{
	using ystdex::sfmt;
	auto& os(std::cout);

	ystdex::write_ntcts(os, sfmt("Usage: \"%s\" [OPTIONS ...] SRCPATH"
		" [OPTIONS ... [-- [ARGS...]]]\n"
		"  or:  \"%s\" [OPTIONS ... [-- [[SRCPATH] ARGS...]]]\n"
		"\tThis program is an interpreter of Unilang.\n"
		"\tThere are two execution modes, scripting mode and interactive mode,"
		" exclusively. In both modes, the interpreter is initialized before"
		" further execution of the code. In scripting mode, a script file"
		" specified by the command line argument SRCPATH (if any) is run."
		" Otherwise, the program runs in the interactive mode and the REPL"
		" (read-eval-print loop) is entered, see below for details.\n"
		"\tThere are no checks on the encoding of the input code.\n"
		"\tThere are no checks on the values. Any behaviors depending"
		" on the locale-specific values are unspecified.\n"
		"\tCurrently accepted environment variable are:\n\n",
		prog.c_str(), prog.c_str()).c_str());
	for(const auto& env : DeEnvs)
		ystdex::write_ntcts(os, sfmt("  %s\n\t%s Default value is %s.\n\n",
			env[0], env[2], env[1][0] == '\0' ? "empty"
			: Quote(string(env[1])).c_str()).c_str());
	ystdex::write_literal(os,
		"SRCPATH\n"
		"\tThe source path. It is handled if and only if this program runs in"
		" scripting mode. In this case, SRCPATH is the 1st command line"
		" argument not recognized as an option (see below). Otherwise, the"
		" command line argument is treated as an option.\n"
		"\tSRCPATH shall specify a path to a source file, or the special value"
		" '-' which indicates the standard input."
		"\tThe source specified by SRCPATH shall have Unilang source tokens"
		" encoded in a text stream with optional UTF-8 BOM (byte-order mark),"
		" which are to be read and evaluated in the initial environment of the"
		" interpreter. Otherwise, errors are raised to reject the source.\n\n"
		"OPTIONS ...\nOPTIONS ... -- [[SRCPATH] ARGS ...]\n"
		"\tThe options and arguments for the program execution. After '--',"
		" options parsing is turned off and every remained command line"
		" argument (if any) is interpreted as an argument, except that the"
		" 1st command line argument is treated as SRCPATH (implying scripting"
		" mode) when there is no SRCPATH before '--'.\n"
		"\tRecognized options are handled in this program, and the remained"
		" arguments will be passed to the Unilang user program (in the script"
		" or in the interactive environment) which can access the arguments via"
		" appropriate API.\n\t"
		"The recognized options are:\n\n");
	for(const auto& opt : OptionsTable)
	{
		ystdex::write_ntcts(os,
			sfmt("  %s%s\n", opt.prefix, opt.option_arg).c_str());
		for(const auto& des : opt.option_details)
			ystdex::write_ntcts(os, sfmt("\t%s\n", des).c_str());
		os << '\n';
	}
	ystdex::write_literal(os,
		"The exit status is 0 on success and 1 otherwise.\n");
}


#define APP_NAME "Unilang interpreter"
#if YCL_Win32
#	define APP_PLATFORM "[MinGW32]"
#elif YCL_Linux
#	define APP_PLATFORM "[Linux]"
#else
#	define APP_PLATFORM "[unspecified platform]"
#endif
constexpr auto title(APP_NAME " " APP_VER " @ (" __DATE__ ", " __TIME__ ") "
	APP_PLATFORM "[C++"
#if __cplusplus > 202002L
	"2b"
#elif __cplusplus >= 202002L
	"20"
#elif __cplusplus >= 201703L
	"17"
#elif __cplusplus >= 201402L
	"14"
#else
	"11"
#endif
	"] + YSLib");

template<typename _fCallable, typename... _tParams>
inline void
Launch(default_allocator<yimpl(byte)> a, _fCallable&& f, _tParams&&... args)
{
	Interpreter intp{};

	Unilang::GuardExceptionsForAllocator(a, yforward(f), intp,
		yforward(args)...);
}

// XXX: Reference to 'argc' is required for Qt initialization.
void
RunEvalStrings(Interpreter& intp, vector<string>& eval_strs, int& argc,
	char* argv[])
{
	LoadFunctions(intp, Unilang_UseJIT, argc, argv);
	if(Unilang_UseJIT)
		JITMain();
	for(const auto& str : eval_strs)
		intp.RunLine(str);
}

void
RunInteractive(Interpreter& intp, int& argc, char* argv[])
{
	using namespace std;

	cout << title << endl << "Initializing the interpreter "
		<< (Unilang_UseJIT ? "[JIT enabled]" : "[JIT disabled]") << " ..."
		<< endl;
	LoadFunctions(intp, Unilang_UseJIT, argc, argv);
	if(Unilang_UseJIT)
		JITMain();
	cout << "Initialization finished." << endl;
	cout << "Type \"exit\" to exit." << endl << endl;
	intp.Run();
}

} // unnamed namespace;

} // namespace Unilang;

int
main(int argc, char* argv[])
{
	using namespace Unilang;
	using YSLib::LoggedEvent;

	return YSLib::FilterExceptions([&]{
		static pmr::new_delete_resource_t r;
		const YSLib::CommandArguments xargv(argc, argv);
		const auto xargc(xargv.size());

		if(xargc > 1)
		{
			vector<string> args;
			bool opt_trans(true);
			bool requires_eval = {};
			vector<string> eval_strs;

			for(size_t i(1); i < xargc; ++i)
			{
				string arg(xargv[i]);

				if(opt_trans)
				{
					if(YB_UNLIKELY(arg == "--"))
					{
						opt_trans = {};
						continue;
					}
					if(arg == "-h" || arg == "--help")
					{
						PrintHelpMessage(xargv[0]);
						return;
					}
					else if(arg == "-e")
					{
						requires_eval = true;
						continue;
					}
					else if(arg == "-q" || arg == "--no-init-file")
					{
						init_file = {};
						continue;
					}
				}
				if(requires_eval)
				{
					eval_strs.push_back(std::move(arg));
					requires_eval = {};
				}
				else
					args.push_back(std::move(arg));
			}
			if(!args.empty())
			{
				auto src(std::move(args.front()));

				args.erase(args.begin());

				const auto p_cmd_args(YSLib::LockCommandArguments());

				p_cmd_args->Arguments = std::move(args);
				Launch(&r, [&](Interpreter& intp){
					RunEvalStrings(intp, eval_strs, argc, argv);
					intp.RunScript(std::move(src));
				});
			}
			else if(!eval_strs.empty())
				Launch(&r, RunEvalStrings, eval_strs, argc, argv);
			else
				Launch(&r, RunInteractive, argc, argv);
		}
		else if(xargc == 1)
		{
			using namespace std;

			Unilang::Deref(YSLib::LockCommandArguments()).Reset(argc, argv);
			Launch(&r, RunInteractive, argc, argv);
		}
	}, yfsig, YSLib::Alert) ? EXIT_FAILURE : EXIT_SUCCESS;
}

namespace
{
	std::map<platform::strings::string, std::tuple<YSLib::ValueObject, std::function<void(Unilang::BindingMap&)>, std::function<void(Unilang::TermNode&, Unilang::TermNode&, Unilang::Context&)>>> ma_ex;

}

ffi_type* get_ffi_type_by_string(const platform::strings::string& s)
{
	if(s=="int")
		return &ffi_type_sint;
	else if(s=="double")
		return &ffi_type_double;
	else if(s=="bool")
		return &ffi_type_uint8;
	else if(s=="string")
		return &ffi_type_pointer;
	else if(s=="void")
		return &ffi_type_void;
	else if(s=="uintptr_t")
		return &ffi_type_uint64;
	else
		return nullptr;
}

template<typename _type>
int unilang_regist_type_value(const char* objectname, _type* object)
{
	platform::strings::string objectname_s(objectname);
	ma_ex.insert(std::make_pair(objectname_s, std::move(std::forward_as_tuple(object,
	[objectname_s, object](Unilang::BindingMap& m){
		Unilang::Environment::DefineChecked(m, platform::strings::string_view(objectname_s), YSLib::ValueObject(*object, YSLib::OwnershipTag<>()));
	},
	[](Unilang::TermNode& x, Unilang::TermNode& y, Unilang::Context& ctx){
		auto& type_x=x.Value.GetObject<_type>();
		auto& type_y=y.Value.GetObject<_type>();

		type_x=std::move(type_y);
	}))));

	return 0;
}

extern "C" YF_API int
unilang_regist_int_value(const char* objectname,int* object)
{
	return unilang_regist_type_value(objectname, object);
}
extern "C" YF_API int
unilang_regist_double_value(const char* objectname,double* object)
{
	return unilang_regist_type_value(objectname, object);
}
extern "C" YF_API int
unilang_regist_bool_value(const char* objectname,bool* object)
{
	return unilang_regist_type_value(objectname, object);
}
extern "C" YF_API int
unilang_regist_pointer_value(const char* objectname,void** object)
{
	return unilang_regist_type_value(objectname, (int*)(object));
}
extern "C" YF_API int
unilang_regist_uintptr_t_value(const char* objectname,std::uintptr_t* object)
{
	return unilang_regist_type_value(objectname, object);
}

void closureCalled(ffi_cif *cif, void *ret, void **args, void *userdata) {

	printf("address  : %p\n", userdata);

	auto& p_userdata = *(::std::tuple<Unilang::ContextHandler*, platform::containers::vector<platform::strings::string>, platform::strings::string, Unilang::Context*>*)userdata;

	printf("address 0: %p\n", ::std::get<0>(p_userdata));
	printf("address 3: %p\n", ::std::get<3>(p_userdata));

	auto& handler = *::std::get<0>(p_userdata);
	auto& s_param_types = ::std::get<1>(p_userdata);
	auto& s_ret_type = ::std::get<2>(p_userdata);
	auto& ctx = *::std::get<3>(p_userdata);

	printf("size: %d\n", s_param_types.size());

	int i=0;
	Unilang::TermNode node_x;

	Unilang::TermNode node_handler;
	node_handler.SetValue(Unilang::ValueObject(handler));
	node_x.Add(node_handler);

	for(const auto& s_ptype : s_param_types)
	{
		printf("s_ptype: %s\n", s_ptype.c_str());
		Unilang::TermNode node_y;
		if(s_ptype=="int")
		{
			int y;
			node_y.SetValue(Unilang::ValueObject(y));
		}
		else if(s_ptype=="double")
		{
			double y;
			node_y.SetValue(Unilang::ValueObject(y, YSLib::OwnershipTag<>()));
		}
		else if(s_ptype=="bool")
		{
			bool y;
			node_y.SetValue(Unilang::ValueObject(y, YSLib::OwnershipTag<>()));
		}
		else if(s_ptype=="string")
		{
			platform::strings::string y;
			node_y.SetValue(Unilang::ValueObject(y, YSLib::OwnershipTag<>()));
		}
		else if(s_ptype=="uintptr_t")
		{
			std::uintptr_t y;
			node_y.SetValue(Unilang::ValueObject(y, YSLib::OwnershipTag<>()));
		}
		else
		{
			throw Unilang::UnilangException("Unknown parameter type found.");
		}
		node_x.Add(node_y);
		
	}

	printf("s_ret_type: %s\n", s_ret_type.c_str());
	Unilang::TermNode node_r;
	if(s_ret_type=="int")
	{
		int r;
		node_r.SetValue(Unilang::ValueObject(realpath));

		printf("rrr: %p\n", r);

		auto node_result=Unilang::ReduceCombinedBranch(node_x, ctx);

		printf("rrr: %p\n", r);
		ret=new int;
		*((int *)ret)=node_x.Value.GetObject<int>();
	}
	else if(s_ret_type=="double")
	{
		double r;
		node_r.SetValue(Unilang::ValueObject(r, YSLib::OwnershipTag<>()));

		auto node_result=handler(node_x,ctx);

		*((double *)ret)=r;
	}
	else if(s_ret_type=="bool")
	{
		bool r;
		node_r.SetValue(Unilang::ValueObject(r, YSLib::OwnershipTag<>()));

		auto node_result=handler(node_x,ctx);

		*((bool *)ret)=r;
	}
	else if(s_ret_type=="string")
	{
		platform::strings::string r;
		node_r.SetValue(Unilang::ValueObject(r, YSLib::OwnershipTag<>()));

		auto node_result=handler(node_x,ctx);

		ret=new char[r.size()+1];
		strcpy((char *)ret, r.c_str());
	}
	else
	{
		throw Unilang::UnilangException("Unknown parameter type found.");
	}
}

Unilang::ReductionStatus
_ReduceCombining(Unilang::TermNode& term, Unilang::Context& ctx)
{
	assert(IsCombiningTerm(term) && "Invalid term found.");
	AssertValueTags(term);
	if(!IsSingleElementList(term))
	{
		AssertNextTerm(ctx, term);
		ctx.LastStatus = Unilang::ReductionStatus::Neutral;
		if(IsEmpty(AccessFirstSubterm(term)))
			RemoveHead(term);
		assert(IsBranch(term));
		ctx.SetCombiningTermRef(term);
		return ReduceSubsequent(AccessFirstSubterm(term), ctx,
			Unilang::NameTypedReducerHandler(std::bind(Unilang::ReduceCombinedBranch,
			std::ref(term), std::placeholders::_1), "eval-combine-operands"));
	}

	// NOTE: The following is necessary to prevent unbounded overflow in
	//	handling recursive subterms.
	auto term_ref(ystdex::ref(term));

	ystdex::retry_on_cond([&]{
		return IsSingleElementList(term_ref);
	}, [&]{
		term_ref = AccessFirstSubterm(term_ref);
	});
	return ReduceOnceLifted(term, ctx, term_ref);
}

extern "C" YF_API int
unilang_regist_function_var(const char* objectname, void** object, const char* result_type, const int parameters_number, const char **parameters_type)
{
	
	int result = 0; 

	printf("object: %d\n", object);
	printf("*object: %d\n", *object);

	platform::strings::string s_ret_type;
	platform::containers::vector<platform::strings::string> s_param_types(parameters_number);

	s_ret_type = result_type;

	for (int i = 0; i < parameters_number; i++)
	{
		platform::strings::string s_pt(parameters_type[i]);
		s_param_types[i]=s_pt;
	}

	platform::strings::string objectname_s(objectname);
	ma_ex.insert(std::make_pair(objectname_s, std::move(std::forward_as_tuple(object,
	[objectname_s, object, parameters_number, s_param_types, s_ret_type](Unilang::BindingMap& m){
		Unilang::RegisterStrict(m, objectname_s, [object, parameters_number, s_param_types, s_ret_type](Unilang::TermNode& term){
			void* function=*object;
			RetainN(term, parameters_number);

			void** args=new void*[parameters_number];	
			int* nums=new int[parameters_number];		
			
			ffi_type* result_type=get_ffi_type_by_string(s_ret_type);

			platform::containers::vector<ffi_type *> param_types;

			int idx=0;
			auto i(std::next(term.begin()));
			printf("size: %d\n", s_param_types.size());
			for(const auto& s_ptype : s_param_types)
			{
				printf("%s\n", s_ptype.c_str());
				// create ffi_type for each parameter's typename and push it to param_types
				ffi_type* ffi_ptype=get_ffi_type_by_string(s_ptype);
				param_types.push_back(ffi_ptype);
				
				const auto& s_param_term(*i++);

				if(s_ptype=="int")
				{
					args[idx]=new int(s_param_term.Value.GetObject<int>());
					printf("%d\n", s_param_term.Value.GetObject<int>());
					nums[idx]=1;
				}
				else if(s_ptype=="double")
				{
					args[idx]=new double(s_param_term.Value.GetObject<double>());
					nums[idx]=1;
				}
				else if(s_ptype=="bool")
				{
					args[idx]=new bool(s_param_term.Value.GetObject<bool>());
					nums[idx]=1;
				}
				else if(s_ptype=="string")
				{
					auto& s(s_param_term.Value.GetObject<platform::strings::string>());
					char* newchar=new char[s.size()+1];
					strcpy(newchar, s.c_str());
					args[idx]=new char*(newchar);
					nums[idx]=s.size()+1;
				}
				else if(s_ptype=="uintptr_t")
				{
					auto& type=s_param_term.Value.type();
					printf("type: %s\n", type.name());
					printf("uintptr_t size: %d\n", sizeof(std::uintptr_t));
					std::uintptr_t rry;
					Unilang::ResolveTerm([&rry](const Unilang::TermNode& nd){
						rry=nd.Value.GetObject<std::uintptr_t>();
					}, s_param_term);
					args[idx]=new std::uintptr_t(rry);
					printf("%d\n", args[idx]);
					nums[idx]=1;
				}
				else
				{
					throw Unilang::UnilangException("Unknown parameter type found.");
				}

				idx++;
			}
			printf("Switch\n");
			ffi_cif cif;
			switch(::ffi_prep_cif(&cif, ffi_abi::FFI_DEFAULT_ABI, YSLib::CheckUpperBound<
				unsigned>(parameters_number), result_type, param_types.data()))
			{
			case FFI_OK: {
				printf("FFI_OK\n");
				void* result;

				if(s_ret_type=="int")
				{
					result=new int;
				}
				else if(s_ret_type=="double")
				{
					result=new double;
				}
				else if(s_ret_type=="bool")
				{
					result=new bool;
				}
				else if(s_ret_type=="string")
				{
					result = new char[100];
				}
				else
				{
					throw Unilang::UnilangException("Unknown parameter type found.");
				}
				//((int(*)(std::uintptr_t,char*,char*))function)(*(std::uintptr_t*)(args[0]),*(char**)(args[1]),*(char**)(args[2]));
				printf("function: %d\n", function);
				ffi_call(&cif, (void (*)())function, result, args);
				printf("%d\n", *(int*)result);
				

				// delete args
				for(int i=0;i<parameters_number;i++)
				{
					if(s_param_types[i]=="int")
					{
						delete (int*)args[i];
					}
					else if(s_param_types[i]=="double")
					{
						delete (double*)args[i];
					}
					else if(s_param_types[i]=="bool")
					{
						delete (bool*)args[i];
					}
					else if(s_param_types[i]=="string")
					{
						delete[] (*(char**)args[i]);
						delete (char**)args[i];
					}
					else if(s_param_types[i]=="uintptr_t")
					{
						delete (std::uintptr_t*)args[i];
					}
					else
					{
						throw Unilang::UnilangException("Unknown parameter type found.");
					}
				}		
				delete[] args;

				// delete result and set term.Value
				if(s_ret_type=="int")
				{
					term.Value=Unilang::ValueObject(*(int*)result, YSLib::OwnershipTag<>());
					delete (int*)result;
				}
				else if(s_ret_type=="double")
				{
					term.Value=Unilang::ValueObject(*(double*)result, YSLib::OwnershipTag<>());
					delete (double*)result;
				}
				else if(s_ret_type=="bool")
				{
					term.Value=Unilang::ValueObject(*(bool*)result, YSLib::OwnershipTag<>());
					delete (bool*)result;
				}
				else if(s_ret_type=="string")
				{
					term.Value=platform::strings::string((char*)result);
					delete[] (char*)result;
				}
				else
				{
					throw Unilang::UnilangException("Unknown parameter type found.");
				}

				break;
			}
			case FFI_BAD_ABI:
				throw Unilang::UnilangException("FFI_BAD_ABI");
			case FFI_BAD_TYPEDEF:
				throw Unilang::UnilangException("FFI_BAD_TYPEDEF");
			default:
				throw Unilang::UnilangException("Unknown error in '::ffi_prep_cif' found.");
			}

		});
	},
	[objectname_s, &object, parameters_number, s_param_types, s_ret_type](Unilang::TermNode& x, Unilang::TermNode& y, Unilang::Context& ctx){
		printf("object: %d\n", object);
		printf("*object: %d\n", *object);

		auto& y_type=y.Value.type();
		
		printf("y_type: %s\n", y_type.name());

		//unilang function y
		// set object (void**) function pointer pointer to y
		//_ReduceCombining(y,ctx);
		return Unilang::ReduceSubsequent(y, ctx, 
			Unilang::NameTypedReducerHandler(std::bind([&,s_param_types, s_ret_type](Unilang::Context& ctx,
			const Unilang::TermNode& saved, const std::shared_ptr<Unilang::Environment>& p_e){
				//auto handler=Unilang::AccessRegular<Unilang::ContextHandler>(y,false);
				auto& handler=Unilang::AccessRegular<Unilang::ContextHandler>(y,false);
				Unilang::ContextHandler* phandler=new Unilang::ContextHandler(handler);
				printf("handler: %p\n", &handler);

				ffi_type* result_type=get_ffi_type_by_string(s_ret_type);
				platform::containers::vector<ffi_type *> param_types;
				for(const auto& s_ptype : s_param_types)
				{
					printf("%s\n", s_ptype.c_str());
					// create ffi_type for each parameter's typename and push it to param_types
					ffi_type* ffi_ptype=get_ffi_type_by_string(s_ptype);
					param_types.push_back(ffi_ptype);
				}
				ffi_cif cif;
				::ffi_prep_cif(&cif, ffi_abi::FFI_DEFAULT_ABI, YSLib::CheckUpperBound<
						unsigned>(parameters_number), result_type, param_types.data());

				ffi_closure *closure = (ffi_closure *)ffi_closure_alloc(sizeof(ffi_closure), object);

				auto* userdata=new ::std::tuple<Unilang::ContextHandler*, platform::containers::vector<platform::strings::string>, platform::strings::string, Unilang::Context*>(phandler,s_param_types,s_ret_type,&ctx);

				ffi_prep_closure_loc(closure, &cif, closureCalled, userdata, nullptr);

				printf("after ref\n");
				printf("object: %d\n", object);
				printf("*object: %d\n", *object);
				return Unilang::ReductionStatus::Clean;
			}, std::placeholders::_1, x, ctx.GetRecordPtr()),
			"match-ptree"));


	}))));

	return 0;
}

struct reff_F
{
	void* pfn;

	platform::strings::string s_ret_type;
	platform::containers::vector<platform::strings::string> s_param_types;

	ffi_type* result_type;
	platform::containers::vector<ffi_type *> param_types;

	ffi_cif cif;

	Unilang::ReductionStatus operator ()(Unilang::TermNode& term, Unilang::Context& c)
	{
		int parameters_number=s_param_types.size();

		void** args=new void*[parameters_number];	
		int* nums=new int[parameters_number];	

		int idx=0;
		auto i(std::next(term.begin()));
		printf("size: %d\n", s_param_types.size());
		for(const auto& s_ptype : s_param_types)
		{
			printf("%s\n", s_ptype.c_str());
			// create ffi_type for each parameter's typename and push it to param_types
			ffi_type* ffi_ptype=get_ffi_type_by_string(s_ptype);
			param_types.push_back(ffi_ptype);
			
			const auto& s_param_term(*i++);

			if(s_ptype=="int")
			{
				args[idx]=new int(s_param_term.Value.GetObject<int>());
				printf("%d\n", s_param_term.Value.GetObject<int>());
				nums[idx]=1;
			}
			else if(s_ptype=="double")
			{
				args[idx]=new double(s_param_term.Value.GetObject<double>());
				nums[idx]=1;
			}
			else if(s_ptype=="bool")
			{
				args[idx]=new bool(s_param_term.Value.GetObject<bool>());
				nums[idx]=1;
			}
			else if(s_ptype=="string")
			{
				auto& s(s_param_term.Value.GetObject<platform::strings::string>());
				args[idx]=new char[s.size()+1];
				strcpy((char*)(args[idx]), s.c_str());
				nums[idx]=s.size()+1;
			}
			else
			{
				throw Unilang::UnilangException("Unknown parameter type found.");
			}

			idx++;
		}

		switch(::ffi_prep_cif(&cif, ffi_abi::FFI_DEFAULT_ABI, YSLib::CheckUpperBound<
				unsigned>(param_types.size()), result_type, param_types.data()))
			{
			case FFI_OK: {
				printf("FFI_OK\n");
				void* result;

				if(s_ret_type=="int")
				{
					result=new int;
				}
				else if(s_ret_type=="double")
				{
					result=new double;
				}
				else if(s_ret_type=="bool")
				{
					result=new bool;
				}
				else if(s_ret_type=="string")
				{
					result = new char[100];
				}
				else
				{
					throw Unilang::UnilangException("Unknown parameter type found.");
				}

				printf("function: %d\n", pfn);
				ffi_call(const_cast<ffi_cif*>(&cif), (void (*)())pfn, result, args);
				printf("%d\n", *(int*)result);
				

				// delete args
				for(int i=0;i<parameters_number;i++)
				{
					if(s_param_types[i]=="int")
					{
						delete (int*)args[i];
					}
					else if(s_param_types[i]=="double")
					{
						delete (double*)args[i];
					}
					else if(s_param_types[i]=="bool")
					{
						delete (bool*)args[i];
					}
					else if(s_param_types[i]=="string")
					{
						delete[] (char*)args[i];
					}
					else
					{
						throw Unilang::UnilangException("Unknown parameter type found.");
					}
				}		
				delete[] args;

				// delete result and set term.Value
				if(s_ret_type=="int")
				{
					term.Value=Unilang::ValueObject(*(int*)result, YSLib::OwnershipTag<>());
					delete (int*)result;
				}
				else if(s_ret_type=="double")
				{
					term.Value=Unilang::ValueObject(*(double*)result, YSLib::OwnershipTag<>());
					delete (double*)result;
				}
				else if(s_ret_type=="bool")
				{
					term.Value=Unilang::ValueObject(*(bool*)result, YSLib::OwnershipTag<>());
					delete (bool*)result;
				}
				else if(s_ret_type=="string")
				{
					term.Value=platform::strings::string((char*)result);
					delete[] (char*)result;
				}
				else
				{
					throw Unilang::UnilangException("Unknown parameter type found.");
				}

				break;
			}
			case FFI_BAD_ABI:
				throw Unilang::UnilangException("FFI_BAD_ABI");
			case FFI_BAD_TYPEDEF:
				throw Unilang::UnilangException("FFI_BAD_TYPEDEF");
			default:
				throw Unilang::UnilangException("Unknown error in '::ffi_prep_cif' found.");
			}

		return Unilang::ReductionStatus::Retained;
	}
};

extern "C" YF_API int
unilang_regist_function_var__ref(const char* objectname, void** object, const char* result_type, const int parameters_number, const char **parameters_type)
{
		
	int result = 0; 

	printf("object: %d\n", object);
	printf("*object: %d\n", *object);

	platform::strings::string s_ret_type;
	platform::containers::vector<platform::strings::string> s_param_types(parameters_number);

	s_ret_type = result_type;

	for (int i = 0; i < parameters_number; i++)
	{
		platform::strings::string s_pt(parameters_type[i]);
		s_param_types[i]=s_pt;
	}




	platform::strings::string objectname_s(objectname);
	ma_ex.insert(std::make_pair(objectname_s, std::move(std::forward_as_tuple(object, 
		[objectname_s, object, parameters_number, s_param_types, s_ret_type](Unilang::BindingMap& m){
			reff_F f;
			f.pfn=*object;

			Unilang::FormContextHandler handler(f);
			Unilang::ContextHandler ch(handler);

			Unilang::Environment::DefineChecked(m, platform::strings::string_view(objectname_s), YSLib::ValueObject(ch, YSLib::OwnershipTag<>()));
		},
		[objectname_s, &object, parameters_number, s_param_types, s_ret_type](Unilang::TermNode& x, Unilang::TermNode& y, Unilang::Context& ctx){

		}))));

}

extern "C" YF_API int
unilang_load_file(char* filename)
{
	int argc = 1;
	char* argv[] = {filename};
	using namespace Unilang;
	using YSLib::LoggedEvent;

	return YSLib::FilterExceptions([&]{
		static pmr::new_delete_resource_t r;

        string src(filename);

        Launch(&r, [&](Interpreter& intp){
			LoadFunctions(intp, Unilang_UseJIT, argc, argv);

			auto& m(intp.Main.GetRecordRef().GetMapRef());

			string x;

			Unilang::Forms::RegisterBinary<Form, ResolvedArg<>, ResolvedArg<>>(m, "ref<-",
				[&m](ResolvedArg<>&& x, ResolvedArg<>&& y, Context& ctx) -> ValueObject
				{
					if(x.IsModifiable())
					{
						// get variable name of x
						string name = x.first.get().Value.GetObject<TokenValue>();
						
						printf("name: %s\n", name.c_str());

						// find this name from environment
						const auto i(m.find(name));

						printf("i: %d\n", i==m.end());

						if(i == m.end())
							ThrowNonmodifiableErrorForAssignee();
						auto& x_term=i->second;

						// find this name from ma_ex
						auto it = ma_ex.find(name);

						printf("it: %d\n", it==ma_ex.end());

						if(it == ma_ex.end())
							ThrowNonmodifiableErrorForAssignee();

						auto& func = std::get<2>(it->second);
						func(x_term,y.get(),ctx);
					}
					else
						ThrowNonmodifiableErrorForAssignee();
					return ValueToken::Unspecified;
			});

			// Unilang::Forms::RegisterBinary<Strict>(m,"reff<-",[&m](TermNode&& x, TermNode&& y) -> ValueObject{
			// 	reff_F& fun(AccessRegular<reff_F>(x,true));
			// 	fun.pfn=y.Value.GetObject<void*>();
			// });

			// register external variables
			for(auto it=ma_ex.begin();it!=ma_ex.end();it++)
			{
				auto& func = std::get<1>(it->second);
				func(m);
			}

            intp.RunScript(std::move(src));

			// press any key to exit
			getchar();

        });

	}, yfsig, YSLib::Alert) ? EXIT_FAILURE : EXIT_SUCCESS;
}

