#ifndef CSVP_VALIDATOR_H_
#define CSVP_VALIDATOR_H_

#include <cstddef>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "csvp/row.h"
#include "csvp/types.h"

namespace csvp {

class FieldValidator
{
public:
	FieldValidator() = default;

	void addRule(const FieldRule &rule);
	void addRules(const std::vector<FieldRule> &rules);
	std::vector<CellError> validate(const Row &row) const;
	CellError validateField(std::size_t columnIndex, const std::string &rawValue,
		const FieldRule &rule) const;
	bool getConfigurationError(std::string &errorMessage) const;

	const std::vector<FieldRule> &rules() const
	{
		return m_rules;
	}

	std::size_t columnCount() const
	{
		return m_rules.size();
	}

private:
	ErrorCode validateValue(std::size_t columnIndex, const std::string &value,
		const FieldRule &rule, std::string &errorMessage) const;
	bool validateType(const std::string &value, FieldType type, std::string &errorMessage) const;
	bool validateRange(const std::string &value, const FieldRule &rule,
		std::string &errorMessage) const;
	bool validateRegex(std::size_t columnIndex, const std::string &value,
		const FieldRule &rule, std::string &errorMessage) const;
	void compileRegex(const FieldRule &rule);

	std::vector<FieldRule> m_rules;
	std::vector<std::optional<std::regex>> m_compiledRegex;
	std::vector<std::string> m_regexErrors;
};

} // namespace csvp

#endif // CSVP_VALIDATOR_H_
