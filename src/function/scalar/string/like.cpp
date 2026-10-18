#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "re2/literal_finder.h"

#include <absl/strings/ascii.h>

namespace duckdb {

static bool GlobMatchesBracket(char s, const char *pattern, idx_t plen, idx_t &pidx, bool &valid_pattern) {
	if (pidx == plen) {
		valid_pattern = false;
		return false;
	}
	// check the first character
	// if it is an exclamation mark we need to invert our logic
	bool invert = false;
	if (pattern[pidx] == '!') {
		invert = true;
		pidx++;
	}
	bool found_match = invert;
	idx_t start_pos = pidx;
	bool found_closing_bracket = false;
	// now check the remainder of the pattern
	while (pidx < plen) {
		auto p = pattern[pidx];
		// if the first character is a closing bracket, we match it literally
		// otherwise it indicates an end of bracket
		if (p == ']' && pidx > start_pos) {
			// end of bracket found: we are done
			found_closing_bracket = true;
			pidx++;
			break;
		}
		if (pidx + 1 == plen) {
			// no next character!
			break;
		}
		bool matches;
		if (pattern[pidx + 1] == '-') {
			// range! find the next character in the range
			if (pidx + 2 == plen) {
				break;
			}
			auto next_char = pattern[pidx + 2];
			// check if the current character is within the range
			matches = s >= p && s <= next_char;
			// shift the pattern forward past the range
			pidx += 3;
		} else {
			// no range! perform a direct match
			matches = p == s;
			// shift the pattern forward past the character
			pidx++;
		}
		if (found_match == invert && matches) {
			// found a match! set the found_matches flag
			// we keep on pattern matching after this until we reach the end bracket
			// however, we don't need to update the found_match flag anymore
			found_match = !invert;
		}
	}
	if (!found_closing_bracket) {
		// no end of bracket: invalid pattern
		valid_pattern = false;
		return false;
	}
	valid_pattern = true;
	return found_match;
}

bool Glob(const char *string, idx_t slen, const char *pattern, idx_t plen, bool allow_question_mark) {
	idx_t sidx = 0;
	idx_t pidx = 0;
	idx_t star_pidx = 0;
	idx_t star_sidx = 0;
	bool has_star = false;
	// main matching loop
	while (sidx < slen) {
		bool matched = false;
		idx_t next_pidx = pidx;
		if (pidx < plen) {
			auto p = pattern[pidx];
			switch (p) {
			case '*': {
				// asterisk: match any set of characters
				pidx++;
				// skip any subsequent asterisks
				while (pidx < plen && pattern[pidx] == '*') {
					pidx++;
				}
				// if the asterisk is the last character, the pattern always matches
				if (pidx == plen) {
					return true;
				}
				has_star = true;
				// remember the pattern position right after '*', and the current string position
				star_pidx = pidx;
				star_sidx = sidx;
				continue;
			}
			case '?':
				// when enabled: matches anything but null
				if (allow_question_mark) {
					matched = true;
				} else {
					matched = string[sidx] == p;
				}
				next_pidx = pidx + 1;
				break;
			case '[': {
				next_pidx = pidx + 1;
				bool valid_pattern;
				matched = GlobMatchesBracket(string[sidx], pattern, plen, next_pidx, valid_pattern);
				if (!valid_pattern) {
					return false;
				}
				break;
			}
			case '\\':
				// escape character, next character needs to match literally
				pidx++;
				// check that we still have a character remaining
				if (pidx == plen) {
					return false;
				}
				matched = string[sidx] == pattern[pidx];
				next_pidx = pidx + 1;
				break;
			default:
				// not a control character: characters need to match literally
				matched = string[sidx] == p;
				next_pidx = pidx + 1;
				break;
			}
		}
		if (matched) {
			sidx++;
			pidx = next_pidx;
			continue;
		}
		if (!has_star) {
			return false;
		}
		// backtrack: the last '*' consumes one more character and we retry from there
		star_sidx++;
		sidx = star_sidx;
		pidx = star_pidx;
	}
	while (pidx < plen && pattern[pidx] == '*') {
		pidx++;
	}
	// we are finished only if we have consumed the full pattern
	return pidx == plen;
}

namespace {
struct StandardCharacterReader {
	static void NextCharacter(const char *sdata, idx_t slen, idx_t &sidx) {
		sidx++;
		while (sidx < slen && !IsCharacter(sdata[sidx])) {
			sidx++;
		}
	}

	static char Operation(const char *data, idx_t pos) {
		return data[pos];
	}
};

struct ASCIILCaseReader {
	static void NextCharacter(const char *sdata, idx_t slen, idx_t &sidx) {
		sidx++;
	}

	static char Operation(const char *data, idx_t pos) {
		return (char)absl::ascii_tolower((uint8_t)data[pos]);
	}
};

template <char PERCENTAGE, char UNDERSCORE, bool HAS_ESCAPE, class READER = StandardCharacterReader>
bool TemplatedLikeOperator(const char *sdata, idx_t slen, const char *pdata, idx_t plen, char escape) {
	idx_t pidx = 0;
	idx_t sidx = 0;
	// position right after the last '%' seen, used to retry the match one character further on mismatch
	bool has_percentage = false;
	idx_t retry_pidx = 0;
	idx_t retry_sidx = 0;
	while (sidx < slen) {
		if (pidx < plen) {
			char pchar = READER::Operation(pdata, pidx);
			char schar = READER::Operation(sdata, sidx);
			if (HAS_ESCAPE && pchar == escape) {
				if (pidx + 1 == plen) {
					throw SyntaxException("Like pattern must not end with escape character!");
				}
				if (pdata[pidx + 1] == schar) {
					pidx += 2;
					sidx++;
					continue;
				}
			} else if (pchar == UNDERSCORE) {
				pidx++;
				READER::NextCharacter(sdata, slen, sidx);
				continue;
			} else if (pchar == PERCENTAGE) {
				pidx++;
				while (pidx < plen && pdata[pidx] == PERCENTAGE) {
					pidx++;
				}
				if (pidx == plen) {
					return true; /* tail is acceptable */
				}
				has_percentage = true;
				retry_pidx = pidx;
				retry_sidx = sidx;
				continue;
			} else if (pchar == schar) {
				pidx++;
				sidx++;
				continue;
			}
		}
		if (!has_percentage) {
			return false;
		}
		pidx = retry_pidx;
		sidx = ++retry_sidx;
	}
	while (pidx < plen && pdata[pidx] == PERCENTAGE && !(HAS_ESCAPE && escape == PERCENTAGE)) {
		pidx++;
	}
	return pidx == plen && sidx == slen;
}

struct LikeSegment {
	explicit LikeSegment(string pattern_p) : pattern(std::move(pattern_p)), finder(pattern) {
	}

	string pattern;
	duckdb_re2::LiteralFinder finder;
};

struct LikeMatcher : public FunctionData {
	LikeMatcher(string like_pattern_p, vector<LikeSegment> segments, bool has_start_percentage, bool has_end_percentage)
	    : like_pattern(std::move(like_pattern_p)), segments(std::move(segments)),
	      has_start_percentage(has_start_percentage), has_end_percentage(has_end_percentage) {
	}

	bool Match(string_t &str) {
		auto str_data = const_uchar_ptr_cast(str.GetData());
		auto str_len = str.GetSize();
		idx_t segment_idx = 0;
		idx_t end_idx = segments.size() - 1;
		if (!has_start_percentage) {
			// no start sample_size: match the first part of the string directly
			auto &segment = segments[0];
			if (str_len < segment.pattern.size()) {
				return false;
			}
			if (memcmp(str_data, segment.pattern.c_str(), segment.pattern.size()) != 0) {
				return false;
			}
			str_data += segment.pattern.size();
			str_len -= segment.pattern.size();
			segment_idx++;
			if (segments.size() == 1) {
				// only one segment, and it matches
				// we have a match if there is an end sample_size, OR if the memcmp was an exact match (remaining str is
				// empty)
				return has_end_percentage || str_len == 0;
			}
		}
		// main match loop: for every segment in the middle, use Contains to find the needle in the haystack
		for (; segment_idx < end_idx; segment_idx++) {
			auto &segment = segments[segment_idx];
			// find the pattern of the current segment
			idx_t next_offset = FindStrInStr(str_data, str_len, const_uchar_ptr_cast(segment.pattern.c_str()),
			                                 segment.pattern.size(), segment.finder);
			if (next_offset == DConstants::INVALID_INDEX) {
				// could not find this pattern in the string: no match
				return false;
			}
			idx_t offset = next_offset + segment.pattern.size();
			str_data += offset;
			str_len -= offset;
		}
		if (!has_end_percentage) {
			end_idx--;
			// no end sample_size: match the final segment now
			auto &segment = segments.back();
			if (str_len < segment.pattern.size()) {
				return false;
			}
			if (memcmp(str_data + str_len - segment.pattern.size(), segment.pattern.c_str(), segment.pattern.size()) !=
			    0) {
				return false;
			}
			return true;
		} else {
			auto &segment = segments.back();
			// find the pattern of the current segment
			idx_t next_offset = FindStrInStr(str_data, str_len, const_uchar_ptr_cast(segment.pattern.c_str()),
			                                 segment.pattern.size(), segment.finder);
			return next_offset != DConstants::INVALID_INDEX;
		}
	}

	static unique_ptr<LikeMatcher> CreateLikeMatcher(string like_pattern, char escape = '\\') {
		vector<LikeSegment> segments;
		idx_t last_non_pattern = 0;
		bool has_start_percentage = false;
		bool has_end_percentage = false;
		for (idx_t i = 0; i < like_pattern.size(); i++) {
			auto ch = like_pattern[i];
			if (ch == escape || ch == '%' || ch == '_') {
				// special character, push a constant pattern
				if (i > last_non_pattern) {
					segments.emplace_back(like_pattern.substr(last_non_pattern, i - last_non_pattern));
				}
				last_non_pattern = i + 1;
				if (ch == escape || ch == '_') {
					// escape or underscore: could not create efficient like matcher
					// FIXME: we could handle escaped percentages here
					return nullptr;
				} else {
					// sample_size
					if (i == 0) {
						has_start_percentage = true;
					}
					if (i + 1 == like_pattern.size()) {
						has_end_percentage = true;
					}
				}
			}
		}
		if (last_non_pattern < like_pattern.size()) {
			segments.emplace_back(like_pattern.substr(last_non_pattern, like_pattern.size() - last_non_pattern));
		}
		if (segments.empty()) {
			return nullptr;
		}
		return make_uniq<LikeMatcher>(std::move(like_pattern), std::move(segments), has_start_percentage,
		                              has_end_percentage);
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<LikeMatcher>(like_pattern, segments, has_start_percentage, has_end_percentage);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<LikeMatcher>();
		return like_pattern == other.like_pattern;
	}

private:
	string like_pattern;
	vector<LikeSegment> segments;
	bool has_start_percentage;
	bool has_end_percentage;
};

unique_ptr<FunctionData> LikeBindFunction(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	// pattern is the second argument. If its constant, we can already prepare the pattern and store it for later.
	D_ASSERT(arguments.size() == 2 || arguments.size() == 3);
	for (auto &arg : arguments) {
		if (arg->GetReturnType().id() == LogicalTypeId::VARCHAR &&
		    !StringType::GetCollation(arg->GetReturnType()).empty()) {
			return nullptr;
		}
	}
	auto pattern_str = input.TryGetConstant(1);
	if (pattern_str) {
		return LikeMatcher::CreateLikeMatcher(pattern_str->ToString());
	}
	return nullptr;
}

bool LikeOperatorFunction(const char *s, idx_t slen, const char *pattern, idx_t plen, char escape) {
	return TemplatedLikeOperator<'%', '_', true>(s, slen, pattern, plen, escape);
}

bool LikeOperatorFunction(const char *s, idx_t slen, const char *pattern, idx_t plen) {
	return TemplatedLikeOperator<'%', '_', false>(s, slen, pattern, plen, '\0');
}

bool LikeOperatorFunction(string_t &s, string_t &pat) {
	return LikeOperatorFunction(s.GetData(), s.GetSize(), pat.GetData(), pat.GetSize());
}

bool LikeOperatorFunction(string_t &s, string_t &pat, char escape) {
	return LikeOperatorFunction(s.GetData(), s.GetSize(), pat.GetData(), pat.GetSize(), escape);
}

char GetEscapeChar(string_t escape) {
	// Only one escape character should be allowed
	if (escape.GetSize() > 1) {
		throw SyntaxException("Invalid escape string. Escape string must be empty or one character.");
	}
	return escape.GetSize() == 0 ? '\0' : *escape.GetData();
}

struct LikeEscapeOperator {
	template <class TA, class TB, class TC>
	static inline bool Operation(TA str, TB pattern, TC escape) {
		char escape_char = GetEscapeChar(escape);
		return escape.GetSize() == 0 ? LikeOperatorFunction(str, pattern)
		                             : LikeOperatorFunction(str, pattern, escape_char);
	}
};

struct NotLikeEscapeOperator {
	template <class TA, class TB, class TC>
	static inline bool Operation(TA str, TB pattern, TC escape) {
		return !LikeEscapeOperator::Operation(str, pattern, escape);
	}
};

struct LikeOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA str, TB pattern) {
		return LikeOperatorFunction(str, pattern, '\\');
	}
};

struct ILikeBuffer {
	char *Reserve(idx_t size) {
		if (size > capacity) {
			data = make_unsafe_uniq_array_uninitialized<char>(size);
			capacity = size;
		}
		return data.get();
	}

	string_t Lower(string_t input) {
		auto lower_size = LowerLength(input.GetData(), input.GetSize());
		auto result = Reserve(lower_size);
		LowerCase(input.GetData(), input.GetSize(), result);
		return string_t(result, UnsafeNumericCast<uint32_t>(lower_size));
	}

	unsafe_unique_array<char> data;
	idx_t capacity = 0;
};

struct ILikePattern {
	bool Match(string_t &str) {
		if (!has_escape) {
			return LikeOperatorFunction(str, pattern);
		}
		return LikeOperatorFunction(str, pattern, escape);
	}

	string_t pattern;
	bool has_escape;
	char escape;
};

struct ILikeMatcher {
	explicit ILikeMatcher(const ILikePattern &lowered_p) : lowered(lowered_p) {
		auto size = lowered.pattern.GetSize();
		if (!lowered.pattern.IsInlined()) {
			auto copy = buffer.Reserve(size);
			memcpy(copy, lowered.pattern.GetData(), size);
			lowered.pattern = string_t(copy, UnsafeNumericCast<uint32_t>(size));
		}
		auto data = lowered.pattern.GetData();
		// the matcher cannot honor escape semantics, so only use it when the escape char never appears in the
		// (lowercased) pattern, in which case escape is irrelevant and the pattern is a plain LIKE pattern
		if (!lowered.has_escape || memchr(data, lowered.escape, size) == nullptr) {
			// Build a case-insensitive matcher from the lowercased pattern. Because both the pattern and each string
			// value are lowercased, a case-sensitive LikeMatcher over lowercased data is equivalent to ILIKE, and it
			// uses the fast SIMD FindStrInStr/memcmp contains path. Returns null for patterns with '_' or only '%';
			// in that case we fall back to the generic recursive matcher on the lowercased values.
			matcher = LikeMatcher::CreateLikeMatcher(string(data, size));
		}
	}

	bool Match(string_t &str) {
		if (matcher) {
			return matcher->Match(str);
		}
		return lowered.Match(str);
	}

	ILikeBuffer buffer;
	ILikePattern lowered;
	unique_ptr<LikeMatcher> matcher;
};

struct ILikeLocalState : public FunctionLocalState {
	ILikePattern LowerPattern(string_t input, string_t escape) {
		char escape_char = GetEscapeChar(escape);
		return {pattern.Lower(input), escape.GetSize() != 0, escape_char};
	}

	// reusable scratch buffer for lowercasing each string value (grown on demand)
	ILikeBuffer str;
	ILikeBuffer pattern;
	string constant_pattern;
	string constant_escape;
	unique_ptr<ILikeMatcher> constant_matcher;
};

unique_ptr<FunctionLocalState> ILikeInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr,
                                                   FunctionData *bind_data) {
	return make_uniq<ILikeLocalState>();
}

bool ILikeOperatorFunction(ILikeLocalState &lstate, string_t str, string_t pattern, string_t escape) {
	// lowercase both the str and the pattern
	auto pat_lcase = lstate.LowerPattern(pattern, escape);
	auto str_lcase = lstate.str.Lower(str);
	return pat_lcase.Match(str_lcase);
}

struct NotLikeOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA str, TB pattern) {
		return !LikeOperatorFunction(str, pattern, '\\');
	}
};

struct ILikeOperatorASCII {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA str, TB pattern) {
		return TemplatedLikeOperator<'%', '_', true, ASCIILCaseReader>(str.GetData(), str.GetSize(), pattern.GetData(),
		                                                               pattern.GetSize(), '\\');
	}
};

struct NotILikeOperatorASCII {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA str, TB pattern) {
		return !ILikeOperatorASCII::Operation<TA, TB, TR>(str, pattern);
	}
};

struct GlobOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA str, TB pattern) {
		return Glob(str.GetData(), str.GetSize(), pattern.GetData(), pattern.GetSize());
	}
};

// This can be moved to the scalar_function class
template <typename FUNC>
void LikeEscapeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &str = args.data[0];
	const auto &pattern = args.data[1];
	const auto &escape = args.data[2];

	TernaryExecutor::Execute<string_t, string_t, string_t, bool>(
	    str, pattern, escape, result, FUNC::template Operation<string_t, string_t, string_t>);
}

template <bool INVERT>
void ILikeConstantPattern(ILikeLocalState &lstate, const Vector &str_vec, string_t pattern, string_t escape,
                          Vector &result) {
	if (!lstate.constant_matcher || pattern != lstate.constant_pattern || escape != lstate.constant_escape) {
		lstate.constant_matcher.reset();
		lstate.constant_pattern.assign(pattern.GetData(), pattern.GetSize());
		lstate.constant_escape.assign(escape.GetData(), escape.GetSize());
		// lowercase the pattern exactly once, up front
		lstate.constant_matcher = make_uniq<ILikeMatcher>(lstate.LowerPattern(pattern, escape));
	}
	auto &matcher = *lstate.constant_matcher;
	UnaryExecutor::Execute<string_t, bool>(str_vec, result, [&](string_t str) {
		auto str_lcase = lstate.str.Lower(str);
		bool match = matcher.Match(str_lcase);
		return INVERT ? !match : match;
	});
}

// Execution function for ILIKE / NOT ILIKE ... ESCAPE. Mirrors ILikeFunction: when the pattern and escape are
// both constant, lowercase the pattern once and reuse a scratch buffer for the per-row string lowercasing. If the
// escape character does not occur in the pattern, escape handling is a no-op, so we build the case-folded
// LikeMatcher and use the fast SIMD contains path; otherwise we fall back to the generic escape-aware matcher on
// the lowercased values. Non-constant pattern/escape falls back to the per-row ternary path.
template <bool INVERT>
void ILikeEscapeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<ILikeLocalState>();
	auto &str_vec = args.data[0];
	auto &pattern_vec = args.data[1];
	auto &escape_vec = args.data[2];

	if (pattern_vec.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(pattern_vec) &&
	    escape_vec.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(escape_vec)) {
		ILikeConstantPattern<INVERT>(lstate, str_vec, *ConstantVector::GetData<string_t>(pattern_vec),
		                             *ConstantVector::GetData<string_t>(escape_vec), result);
		return;
	}
	// non-constant pattern/escape: fall back to the generic per-row implementation
	TernaryExecutor::Execute<string_t, string_t, string_t, bool>(
	    str_vec, pattern_vec, escape_vec, result, [&](string_t str, string_t pattern, string_t escape) {
		    bool match = ILikeOperatorFunction(lstate, str, pattern, escape);
		    return INVERT ? !match : match;
	    });
}

template <class ASCII_OP>
unique_ptr<BaseStatistics> ILikePropagateStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	D_ASSERT(child_stats.size() >= 1);
	// can only propagate stats if the children have stats
	if (!StringStats::CanContainUnicode(child_stats[0])) {
		expr.FunctionMutable().SetFunctionCallback(ScalarFunction::BinaryFunction<string_t, string_t, bool, ASCII_OP>);
	}
	return nullptr;
}

// Execution function for ILIKE / NOT ILIKE on the (possibly) Unicode path.
// When the pattern is constant we lowercase it exactly once instead of once per row, and we reuse a single
// scratch buffer to lowercase each string value instead of heap-allocating per row.
// (The ASCII-only fast path installed by ILikePropagateStats already avoids allocations, so it is unaffected.)
template <bool INVERT>
void ILikeFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<ILikeLocalState>();
	auto &pattern_vec = input.data[1];
	string_t escape("\\", 1);
	if (pattern_vec.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(pattern_vec)) {
		ILikeConstantPattern<INVERT>(lstate, input.data[0], *ConstantVector::GetData<string_t>(pattern_vec), escape,
		                             result);
		return;
	}
	// non-constant pattern: fall back to the generic per-row implementation
	BinaryExecutor::Execute<string_t, string_t, bool>(
	    input.data[0], pattern_vec, result, [&](string_t str, string_t pattern) {
		    bool match = ILikeOperatorFunction(lstate, str, pattern, escape);
		    return INVERT ? !match : match;
	    });
}

template <class OP, bool INVERT>
void RegularLikeFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	if (func_expr.BindInfo()) {
		auto &matcher = func_expr.BindInfo()->Cast<LikeMatcher>();
		// use fast like matcher
		UnaryExecutor::Execute<string_t, bool>(input.data[0], result, [&](string_t input) {
			return INVERT ? !matcher.Match(input) : matcher.Match(input);
		});
	} else {
		// use generic like matcher
		BinaryExecutor::ExecuteStandard<string_t, string_t, bool, OP>(input.data[0], input.data[1], result);
	}
}

} // namespace

ScalarFunction NotLikeFun::GetFunction() {
	ScalarFunction not_like("!~~", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                        RegularLikeFunction<NotLikeOperator, true>, LikeBindFunction);
	not_like.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	not_like.GetSignature().GetParameter(0).SetName("string");
	not_like.GetSignature().GetParameter(1).SetName("pattern");
	return not_like;
}

ScalarFunction GlobPatternFun::GetFunction() {
	ScalarFunction glob("~~~", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                    ScalarFunction::BinaryFunction<string_t, string_t, bool, GlobOperator>);
	glob.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	glob.GetSignature().GetParameter(0).SetName("string");
	glob.GetSignature().GetParameter(1).SetName("pattern");
	return glob;
}

ScalarFunction ILikeFun::GetFunction() {
	ScalarFunction ilike("~~*", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                     ILikeFunction<false>, nullptr, ILikePropagateStats<ILikeOperatorASCII>, ILikeInitLocalState);
	ilike.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	ilike.GetSignature().GetParameter(0).SetName("string");
	ilike.GetSignature().GetParameter(1).SetName("pattern");
	return ilike;
}

ScalarFunction NotILikeFun::GetFunction() {
	ScalarFunction not_ilike("!~~*", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                         ILikeFunction<true>, nullptr, ILikePropagateStats<NotILikeOperatorASCII>,
	                         ILikeInitLocalState);
	not_ilike.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	not_ilike.GetSignature().GetParameter(0).SetName("string");
	not_ilike.GetSignature().GetParameter(1).SetName("pattern");
	return not_ilike;
}

ScalarFunction LikeFun::GetFunction() {
	ScalarFunction like("~~", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                    RegularLikeFunction<LikeOperator, false>, LikeBindFunction);
	like.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	like.GetSignature().GetParameter(0).SetName("string");
	like.GetSignature().GetParameter(1).SetName("pattern");
	return like;
}

ScalarFunction NotLikeEscapeFun::GetFunction() {
	ScalarFunction not_like_escape("not_like_escape", {}, LogicalType::BOOLEAN,
	                               LikeEscapeFunction<NotLikeEscapeOperator>);
	not_like_escape.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("like_specifier", LogicalType::VARCHAR)
	    .AddParameter("escape_character", LogicalType::VARCHAR);
	not_like_escape.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	return not_like_escape;
}

ScalarFunction IlikeEscapeFun::GetFunction() {
	ScalarFunction ilike_escape("ilike_escape", {}, LogicalType::BOOLEAN, ILikeEscapeFunction<false>, nullptr, nullptr,
	                            ILikeInitLocalState);
	ilike_escape.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("like_specifier", LogicalType::VARCHAR)
	    .AddParameter("escape_character", LogicalType::VARCHAR);
	ilike_escape.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	return ilike_escape;
}

ScalarFunction NotIlikeEscapeFun::GetFunction() {
	ScalarFunction not_ilike_escape("not_ilike_escape", {}, LogicalType::BOOLEAN, ILikeEscapeFunction<true>, nullptr,
	                                nullptr, ILikeInitLocalState);
	not_ilike_escape.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("like_specifier", LogicalType::VARCHAR)
	    .AddParameter("escape_character", LogicalType::VARCHAR);
	not_ilike_escape.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	return not_ilike_escape;
}
ScalarFunction LikeEscapeFun::GetFunction() {
	ScalarFunction like_escape("like_escape", {}, LogicalType::BOOLEAN, LikeEscapeFunction<LikeEscapeOperator>);
	like_escape.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("like_specifier", LogicalType::VARCHAR)
	    .AddParameter("escape_character", LogicalType::VARCHAR);
	like_escape.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	return like_escape;
}

//===--------------------------------------------------------------------===//
// similar_to_escape
//===--------------------------------------------------------------------===//
// The parser rewrites `x SIMILAR TO y` into
// `regexp_full_match(x, similar_to_escape(y))`, so this translation has to live
// beside the parser rather than in a server-side function set -- otherwise the
// rewrite emits a call that a plain DuckDB has no way to resolve.
static string SimilarToRegex(const string_t &pattern_str, char escape_char) {
	auto pattern = pattern_str.GetData();
	auto plen = pattern_str.GetSize();

	string result;
	result.reserve(plen + 6);
	result += "^(?:";

	bool after_escape = false;
	idx_t nquotes = 0;
	idx_t bracket_depth = 0;
	idx_t charclass_pos = 0;

	for (idx_t i = 0; i < plen; i++) {
		char pchar = pattern[i];
		if (after_escape) {
			if (pchar == '"' && bracket_depth < 1) {
				// escape-double-quote marks the start/end of the captured substring
				if (nquotes == 0) {
					result += "){1,1}?(";
				} else if (nquotes == 1) {
					result += "){1,1}(?:";
				} else {
					throw InvalidInputException(
					    "SQL regular expression may not contain more than two escape-double-quote separators");
				}
				nquotes++;
			} else {
				result += '\\';
				result += pchar;
				charclass_pos = 3;
			}
			after_escape = false;
		} else if (pchar == escape_char) {
			after_escape = true;
		} else if (bracket_depth > 0) {
			// inside a bracket expression the SIMILAR TO metacharacters are literal
			if (pchar == '\\') {
				result += '\\';
			}
			result += pchar;
			if (pchar == ']' && charclass_pos > 2) {
				bracket_depth--;
			} else if (pchar == '[') {
				bracket_depth++;
				charclass_pos = 3;
			} else if (pchar == '^') {
				charclass_pos++;
			} else {
				charclass_pos = 3;
			}
		} else if (pchar == '[') {
			result += pchar;
			bracket_depth = 1;
			charclass_pos = 1;
		} else if (pchar == '%') {
			result += ".*";
		} else if (pchar == '_') {
			result += '.';
		} else if (pchar == '(') {
			// SIMILAR TO parentheses group without capturing
			result += "(?:";
		} else if (pchar == '\\' || pchar == '.' || pchar == '^' || pchar == '$') {
			result += '\\';
			result += pchar;
		} else {
			result += pchar;
		}
	}
	result += ")$";
	return result;
}

static void SimilarToEscapeFunction(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t pattern) {
		return StringVector::AddString(result, SimilarToRegex(pattern, '\\'));
	});
}

static void SimilarToEscapeWithEscapeFunction(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t pattern, string_t escape) {
		    if (escape.GetSize() != 1) {
			    throw InvalidInputException("invalid escape string: must be one character");
		    }
		    return StringVector::AddString(result, SimilarToRegex(pattern, escape.GetData()[0]));
	    });
}

ScalarFunctionSet SimilarToEscapeFun::GetFunctions() {
	ScalarFunctionSet similar_to_escape("similar_to_escape");
	similar_to_escape.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR}, LogicalType::VARCHAR, SimilarToEscapeFunction));
	similar_to_escape.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                                             SimilarToEscapeWithEscapeFunction));
	return similar_to_escape;
}

} // namespace duckdb
