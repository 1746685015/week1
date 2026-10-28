#ifndef CSVP_ROW_H_
#define CSVP_ROW_H_

#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "csvp/types.h"

namespace csvp {

class Row
{
public:
	Row() = default;

	explicit Row(std::vector<std::string> fields)
		: m_fields(std::move(fields))
		, m_size(m_fields.size())
	{
	}

	std::size_t size() const
	{
		return m_size;
	}

	bool empty() const
	{
		return m_size == 0U;
	}

	const std::string &operator[](std::size_t index) const
	{
		return m_fields[index];
	}

	std::string &operator[](std::size_t index)
	{
		return m_fields[index];
	}

	const std::string &at(std::size_t index) const
	{
		if (index >= m_size)
		{
			throw std::out_of_range("Row field index is out of range");
		}
		return m_fields[index];
	}

	bool tryToInt(std::size_t index, int &value) const
	{
		if (index >= m_size || m_fields[index].empty())
			return false;

		const std::string &text = m_fields[index];
		const char *begin = text.data();
		const char *end = begin + text.size();
		int parsedValue = 0;
		const std::from_chars_result result = std::from_chars(begin, end, parsedValue);
		if (result.ec != std::errc() || result.ptr != end)
			return false;

		value = parsedValue;
		return true;
	}

	bool tryToDouble(std::size_t index, double &value) const
	{
		if (index >= m_size || m_fields[index].empty())
			return false;

		const std::string &text = m_fields[index];
		if (text.front() == ' ' || text.front() == '\t'
			|| text.back() == ' ' || text.back() == '\t')
			return false;

		errno = 0;
		char *end = nullptr;
		const double parsedValue = std::strtod(text.c_str(), &end);
		if (end != text.c_str() + text.size() || errno == ERANGE || !std::isfinite(parsedValue))
			return false;

		value = parsedValue;
		return true;
	}

	bool tryToBool(std::size_t index, bool &value) const
	{
		if (index >= m_size)
			return false;

		const std::string &text = m_fields[index];
		if (text == "true" || text == "TRUE" || text == "1" || text == "yes")
		{
			value = true;
			return true;
		}
		if (text == "false" || text == "FALSE" || text == "0" || text == "no")
		{
			value = false;
			return true;
		}
		return false;
	}

	// Compatibility accessors retain the original fallback values.
	int toInt(std::size_t index) const
	{
		int value = 0;
		tryToInt(index, value);
		return value;
	}

	double toDouble(std::size_t index) const
	{
		double value = 0.0;
		tryToDouble(index, value);
		return value;
	}

	const std::string &toString(std::size_t index) const
	{
		static const std::string EMPTY_STRING;
		if (index >= m_size)
			return EMPTY_STRING;
		return m_fields[index];
	}

	bool toBool(std::size_t index) const
	{
		bool value = false;
		tryToBool(index, value);
		return value;
	}

	void append(const std::string &field)
	{
		std::string &destination = appendField();
		destination = field;
	}

	std::string &appendField()
	{
		if (m_size < m_fields.size())
		{
			m_fields[m_size].clear();
		}
		else
		{
			m_fields.emplace_back();
		}

		++m_size;
		return m_fields[m_size - 1U];
	}

	void clear()
	{
		m_size = 0U;
		m_isValid = true;
	}

	bool isValid() const
	{
		return m_isValid;
	}

	void setValid(bool value)
	{
		m_isValid = value;
	}

private:
	std::vector<std::string> m_fields;
	std::size_t m_size = 0U;
	bool m_isValid = true;
};

} // namespace csvp

#endif // CSVP_ROW_H_
