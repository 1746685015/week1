#include "csvp/validator.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <system_error>
#include <utility>

namespace csvp {

namespace {

bool ParseInteger(const std::string &text, std::int64_t &value)
{
	if (text.empty())
		return false;

	const char *begin = text.data();
	const char *end = begin + text.size();
	std::int64_t parsedValue = 0;
	const std::from_chars_result result = std::from_chars(begin, end, parsedValue);
	if (result.ec != std::errc() || result.ptr != end)
		return false;
	value = parsedValue;
	return true;
}

bool ParseDouble(const std::string &text, double &value)
{
	if (text.empty() || text.front() == ' ' || text.front() == '\t')
		return false;

	errno = 0;
	char *end = nullptr;
	const double parsedValue = std::strtod(text.c_str(), &end);
	if (end != text.c_str() + text.size() || errno == ERANGE || !std::isfinite(parsedValue))
		return false;
	value = parsedValue;
	return true;
}

} // namespace

void FieldValidator::addRule(const FieldRule &rule)
{
	m_rules.push_back(rule);
}

void FieldValidator::addRules(const std::vector<FieldRule> &rules)
{
	m_rules = rules;
}

std::vector<CellError> FieldValidator::validate(const Row &row) const
{
	std::vector<CellError> errors;
	const std::size_t sharedColumnCount = std::min(row.size(), m_rules.size());
	for (std::size_t columnIndex = 0; columnIndex < sharedColumnCount; ++columnIndex)
	{
		CellError error = validateField(columnIndex, row[columnIndex], m_rules[columnIndex]);
		if (error.code != ErrorCode::Ok)
			errors.push_back(std::move(error));
	}

	for (std::size_t columnIndex = sharedColumnCount; columnIndex < row.size(); ++columnIndex)
	{
		CellError error;
		error.column_index = columnIndex;
		error.raw_value = row[columnIndex];
		error.code = ErrorCode::ColumnCountMismatch;
		error.message = "No validation rule for CSV column";
		errors.push_back(std::move(error));
	}

	for (std::size_t columnIndex = sharedColumnCount; columnIndex < m_rules.size(); ++columnIndex)
	{
		CellError error;
		error.column_index = columnIndex;
		error.column_name = m_rules[columnIndex].name;
		error.code = ErrorCode::ColumnCountMismatch;
		error.message = "CSV row is missing a schema column";
		errors.push_back(std::move(error));
	}
	return errors;
}

CellError FieldValidator::validateField(std::size_t columnIndex, const std::string &rawValue,
	const FieldRule &rule) const
{
	CellError error;
	error.column_index = columnIndex;
	error.column_name = rule.name;
	error.raw_value = rawValue;

	if (rule.required && rawValue.empty())
	{
		error.code = ErrorCode::RequiredFieldMissing;
		error.message = "Required field is empty";
		return error;
	}
	if (rawValue.empty())
		return error;

	std::string errorMessage;
	if (!validateType(rawValue, rule.type, errorMessage))
	{
		error.code = ErrorCode::TypeConversionError;
		error.message = std::move(errorMessage);
		return error;
	}
	if (rule.has_range && !validateRange(rawValue, rule, errorMessage))
	{
		error.code = ErrorCode::RangeError;
		error.message = std::move(errorMessage);
		return error;
	}
	if (!rule.regex.empty() && !validateRegex(rawValue, rule.regex, errorMessage))
	{
		error.code = ErrorCode::RegexMismatch;
		error.message = std::move(errorMessage);
	}
	return error;
}

bool FieldValidator::validateType(const std::string &value, FieldType type,
	std::string &errorMessage) const
{
	switch (type)
	{
	case FieldType::Int:
	{
		std::int64_t parsedValue = 0;
		if (!ParseInteger(value, parsedValue))
		{
			errorMessage = "Expected integer, got: " + value;
			return false;
		}
		return true;
	}
	case FieldType::Double:
	{
		double parsedValue = 0.0;
		if (!ParseDouble(value, parsedValue))
		{
			errorMessage = "Expected finite number, got: " + value;
			return false;
		}
		return true;
	}
	case FieldType::Bool:
		if (value != "true" && value != "false"
			&& value != "TRUE" && value != "FALSE"
			&& value != "1" && value != "0"
			&& value != "yes" && value != "no")
		{
			errorMessage = "Expected boolean, got: " + value;
			return false;
		}
		return true;
	case FieldType::String:
	default:
		return true;
	}
}

bool FieldValidator::validateRange(const std::string &value, const FieldRule &rule,
	std::string &errorMessage) const
{
	long double numericValue = 0.0L;
	if (rule.type == FieldType::Int)
	{
		std::int64_t parsedValue = 0;
		if (!ParseInteger(value, parsedValue))
			return false;
		numericValue = static_cast<long double>(parsedValue);
	}
	else if (rule.type == FieldType::Double)
	{
		double parsedValue = 0.0;
		if (!ParseDouble(value, parsedValue))
			return false;
		numericValue = static_cast<long double>(parsedValue);
	}
	else
	{
		return true;
	}

	if (numericValue >= static_cast<long double>(rule.min_val)
		&& numericValue <= static_cast<long double>(rule.max_val))
	{
		return true;
	}

	std::ostringstream message;
	message << "Value " << value << " out of range ["
		<< rule.min_val << ", " << rule.max_val << "]";
	errorMessage = message.str();
	return false;
}

bool FieldValidator::validateRegex(const std::string &value, const std::string &pattern,
	std::string &errorMessage) const
{
	try
	{
		const std::regex expression(pattern);
		if (!std::regex_match(value, expression))
		{
			errorMessage = "Value does not match pattern: " + pattern;
			return false;
		}
	}
	catch (const std::regex_error &error)
	{
		errorMessage = std::string("Invalid regex pattern: ") + error.what();
		return false;
	}
	return true;
}

} // namespace csvp
