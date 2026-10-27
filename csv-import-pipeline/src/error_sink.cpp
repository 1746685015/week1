// error_sink.cpp
// Persists complete row-error details as bounded-buffer JSON Lines records.

#include "csvp/error_sink.h"

#include <ostream>

namespace csvp {

namespace {

constexpr char HEX_DIGITS[] = "0123456789ABCDEF";

const char *ErrorCodeName(ErrorCode code)
{
	switch (code)
	{
	case ErrorCode::Ok:
		return "Ok";
	case ErrorCode::ParseError:
		return "ParseError";
	case ErrorCode::ColumnCountMismatch:
		return "ColumnCountMismatch";
	case ErrorCode::TypeConversionError:
		return "TypeConversionError";
	case ErrorCode::RequiredFieldMissing:
		return "RequiredFieldMissing";
	case ErrorCode::RangeError:
		return "RangeError";
	case ErrorCode::RegexMismatch:
		return "RegexMismatch";
	case ErrorCode::EncodingError:
		return "EncodingError";
	case ErrorCode::FileNotOpen:
		return "FileNotOpen";
	case ErrorCode::InternalError:
		return "InternalError";
	}
	return "Unknown";
}

void WriteJsonString(std::ostream &output, const std::string &value)
{
	output.put('"');
	for (const unsigned char character : value)
	{
		switch (character)
		{
		case '"':
			output << "\\\"";
			break;
		case '\\':
			output << "\\\\";
			break;
		case '\b':
			output << "\\b";
			break;
		case '\f':
			output << "\\f";
			break;
		case '\n':
			output << "\\n";
			break;
		case '\r':
			output << "\\r";
			break;
		case '\t':
			output << "\\t";
			break;
		default:
			if (character >= 0x20U && character <= 0x7EU)
				output.put(static_cast<char>(character));
			else
			{
				const unsigned int byte = static_cast<unsigned int>(character);
				output << "\\u00"
					<< HEX_DIGITS[(byte >> 4U) & 0x0FU]
					<< HEX_DIGITS[byte & 0x0FU];
			}
			break;
		}
	}
	output.put('"');
}

void WriteHex(std::ostream &output, const std::string &value)
{
	for (const unsigned char character : value)
	{
		const unsigned int byte = static_cast<unsigned int>(character);
		output.put(HEX_DIGITS[(byte >> 4U) & 0x0FU]);
		output.put(HEX_DIGITS[byte & 0x0FU]);
	}
}

} // namespace

bool ErrorSink::open(const std::string &path, std::string &errorMessage)
{
	errorMessage.clear();
	m_failed = false;
	m_output.clear();
	m_output.rdbuf()->pubsetbuf(m_writeBuffer.data(),
		static_cast<std::streamsize>(m_writeBuffer.size()));
	// Standard openmode flags are combined with their required bitmask operator.
	m_output.open(path, std::ios::binary | std::ios::out | std::ios::trunc); // NOLINT(bugprone-signed-bitwise)
	if (!m_output.is_open())
	{
		errorMessage = "Failed to open error log: " + path;
		m_failed = true;
		return false;
	}
	return true;
}

bool ErrorSink::write(const RowError &error, std::string &errorMessage)
{
	errorMessage.clear();
	if (!m_output.is_open() || m_failed)
	{
		errorMessage = "Error log is not writable";
		m_failed = true;
		return false;
	}

	m_output << "{\"line_number\":" << error.line_number << ",\"raw_line_hex\":\"";
	WriteHex(m_output, error.raw_line);
	m_output << "\",\"cell_errors\":[";
	for (std::size_t index = 0; index < error.cell_errors.size(); ++index)
	{
		if (index != 0U)
			m_output.put(',');
		const CellError &cellError = error.cell_errors[index];
		m_output << "{\"column_index\":" << cellError.column_index
			<< ",\"column_name\":";
		WriteJsonString(m_output, cellError.column_name);
		m_output << ",\"raw_value_hex\":\"";
		WriteHex(m_output, cellError.raw_value);
		m_output << "\",\"code\":";
		WriteJsonString(m_output, ErrorCodeName(cellError.code));
		m_output << ",\"message\":";
		WriteJsonString(m_output, cellError.message);
		m_output.put('}');
	}
	m_output << "]}\n";
	if (!m_output.good())
	{
		errorMessage = "Failed to write error log";
		m_failed = true;
		return false;
	}
	return true;
}

bool ErrorSink::close(std::string &errorMessage)
{
	errorMessage.clear();
	if (!m_output.is_open())
	{
		if (m_failed)
			errorMessage = "Error log was not completed";
		return !m_failed;
	}

	m_output.flush();
	if (!m_output.good())
	{
		errorMessage = "Failed to flush error log";
		m_failed = true;
	}
	m_output.close();
	if (m_output.fail())
	{
		if (errorMessage.empty())
			errorMessage = "Failed to close error log";
		m_failed = true;
	}
	return !m_failed;
}

} // namespace csvp
