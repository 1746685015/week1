#include "csvp/parser.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace csvp {

namespace {

bool EqualsIgnoringCase(const char *left, const char *right)
{
	if (!left || !right)
		return false;

	while (*left != '\0' && *right != '\0')
	{
		const unsigned char leftByte = static_cast<unsigned char>(*left);
		const unsigned char rightByte = static_cast<unsigned char>(*right);
		if (std::toupper(leftByte) != std::toupper(rightByte))
			return false;
		++left;
		++right;
	}
	return *left == *right;
}

} // namespace

CsvParser::CsvParser(const Config &config)
	: m_config(config)
{
}

CsvParser::~CsvParser()
{
	if (m_file)
	{
		fclose(m_file);
		m_file = nullptr;
	}
}

bool CsvParser::parse(const char *filepath)
{
	if (m_file)
	{
		fclose(m_file);
		m_file = nullptr;
	}

	m_lastError.clear();
	m_readPosition = 0;
	m_readSize = 0;
	m_lineBuffer.clear();
	m_header.clear();
	m_lineNumber = 0;
	m_bytesRead = 0;
	m_lineEndedWithNewline = false;
	if (!filepath || filepath[0] == '\0')
	{
		m_lastError = "CSV file path is null or empty";
		return false;
	}

	if (!EqualsIgnoringCase(m_config.encoding, "UTF-8")
		&& !EqualsIgnoringCase(m_config.encoding, "UTF8")
		&& !EqualsIgnoringCase(m_config.encoding, "GBK"))
	{
		m_lastError = "Unsupported CSV encoding";
		return false;
	}

#ifdef _WIN32
	fopen_s(&m_file, filepath, "rb");
#else
	m_file = fopen(filepath, "rb");
#endif
	if (!m_file)
	{
		m_lastError = std::string("Failed to open file: ") + filepath;
		return false;
	}

	if (m_config.has_header)
	{
		Row headerRow;
		if (readNext(headerRow) != ReadStatus::RowReadStatus)
		{
			if (m_lastError.empty())
				m_lastError = "Failed to read header row";
			else
				m_lastError = "Failed to read header row: " + m_lastError;
			fclose(m_file);
			m_file = nullptr;
			return false;
		}
		for (std::size_t index = 0; index < headerRow.size(); ++index)
			m_header.push_back(headerRow[index]);
	}

	return true;
}

bool CsvParser::readRow(Row &row)
{
	return readNext(row) == ReadStatus::RowReadStatus;
}

CsvParser::ReadStatus CsvParser::readNext(Row &row)
{
	if (!m_file)
	{
		m_lastError = "No file opened";
		return ReadStatus::ErrorReadStatus;
	}

	row.clear();
	m_lastError.clear();
	if (!readLineFromFile())
	{
		return m_lastError.empty()
			? ReadStatus::EofReadStatus
			: ReadStatus::ErrorReadStatus;
	}

	if (m_lineEndedWithNewline && !m_lineBuffer.empty() && m_lineBuffer.back() == '\r')
		m_lineBuffer.pop_back();

	if (m_lineBuffer.empty())
	{
		++m_lineNumber;
		return readNext(row);
	}

	if (!splitLineToFields(m_lineBuffer.data(), m_lineBuffer.size(), row))
	{
		++m_lineNumber;
		return ReadStatus::ErrorReadStatus;
	}

	++m_lineNumber;
	return ReadStatus::RowReadStatus;
}

bool CsvParser::readLineFromFile()
{
	m_lineBuffer.clear();
	m_lineEndedWithNewline = false;
	while (true)
	{
		if (m_readPosition >= m_readSize)
		{
			m_readSize = fread(m_readBuffer.data(), 1, m_readBuffer.size(), m_file);
			m_readPosition = 0;
			if (m_readSize == 0)
			{
				if (ferror(m_file))
				{
					m_lastError = "Failed to read CSV file";
					return false;
				}
				return !m_lineBuffer.empty();
			}
		}

		const char *begin = m_readBuffer.data() + m_readPosition;
		const std::size_t available = m_readSize - m_readPosition;
		const char *newline = static_cast<const char *>(std::memchr(begin, '\n', available));
		const std::size_t segmentLength = newline
			? static_cast<std::size_t>(newline - begin)
			: available;
		if (m_lineBuffer.size() + segmentLength > MAX_LINE_SIZE)
		{
			m_lastError = "CSV line exceeds the 16 MB limit";
			return false;
		}

		m_lineBuffer.append(begin, segmentLength);
		m_readPosition += segmentLength;
		m_bytesRead += segmentLength;
		if (newline)
		{
			++m_readPosition;
			++m_bytesRead;
			m_lineEndedWithNewline = true;
			return true;
		}
	}
}

bool CsvParser::splitLineToFields(const char *line, std::size_t length, Row &row)
{
	row.clear();
	std::size_t position = 0;
	std::string field;
	field.reserve(256);

	while (true)
	{
		while (position < length && m_config.trim_spaces
			&& (line[position] == ' ' || line[position] == '\t'))
		{
			++position;
		}

		field.clear();
		if (position < length && line[position] == '"')
		{
			if (!handleQuotedField(line, position, length, field))
				return false;
			while (position < length && m_config.trim_spaces
				&& (line[position] == ' ' || line[position] == '\t'))
			{
				++position;
			}
			if (position < length && line[position] != m_config.delimiter)
			{
				m_lastError = "Unexpected character after quoted field";
				return false;
			}
			row.append(field);
		}
		else
		{
			while (position < length && line[position] != m_config.delimiter)
			{
				if (line[position] == '"')
				{
					m_lastError = "Unexpected quote in unquoted field";
					return false;
				}

				const std::size_t characterLength = encodedCharacterLength(line, position, length);
				if (characterLength == 0)
				{
					m_lastError = isGbkEncoding()
						? "Invalid GBK byte sequence"
						: "Invalid UTF-8 byte sequence";
					return false;
				}
				field.append(line + position, characterLength);
				position += characterLength;
			}

			if (m_config.trim_spaces)
			{
				while (!field.empty() && (field.back() == ' ' || field.back() == '\t'))
					field.pop_back();
			}
			row.append(field);
		}

		if (position == length)
			return true;

		++position;
		if (position == length)
		{
			row.append("");
			return true;
		}
	}
}

bool CsvParser::handleQuotedField(const char *line, std::size_t &position, std::size_t length,
	std::string &field)
{
	++position;
	while (position < length)
	{
		const char character = line[position];
		if (character == '"')
		{
			if (position + 1 < length && line[position + 1] == '"')
			{
				field.push_back('"');
				position += 2;
				continue;
			}
			++position;
			return true;
		}

		const std::size_t characterLength = encodedCharacterLength(line, position, length);
		if (characterLength == 0)
		{
			m_lastError = isGbkEncoding() ? "Invalid GBK byte sequence" : "Invalid UTF-8 byte sequence";
			return false;
		}
		field.append(line + position, characterLength);
		position += characterLength;
	}

	m_lastError = "Unclosed quote in CSV field";
	return false;
}

bool CsvParser::isGbkEncoding() const
{
	return EqualsIgnoringCase(m_config.encoding, "GBK");
}

std::size_t CsvParser::encodedCharacterLength(const char *line, std::size_t position,
	std::size_t length) const
{
	const unsigned char firstByte = static_cast<unsigned char>(line[position]);
	if (firstByte <= 0x7F)
		return 1;

	if (isGbkEncoding())
	{
		if (firstByte == 0x80)
			return 1;
		if (firstByte < 0x81 || firstByte > 0xFE || position + 1 >= length)
			return 0;
		const unsigned char secondByte = static_cast<unsigned char>(line[position + 1]);
		return secondByte >= 0x40 && secondByte <= 0xFE && secondByte != 0x7F ? 2 : 0;
	}

	std::size_t sequenceLength = 0;
	if (firstByte >= 0xC2 && firstByte <= 0xDF)
		sequenceLength = 2;
	else if (firstByte >= 0xE0 && firstByte <= 0xEF)
		sequenceLength = 3;
	else if (firstByte >= 0xF0 && firstByte <= 0xF4)
		sequenceLength = 4;
	else
		return 0;

	if (position + sequenceLength > length)
		return 0;
	for (std::size_t index = 1; index < sequenceLength; ++index)
	{
		const unsigned char continuationByte = static_cast<unsigned char>(line[position + index]);
		if (continuationByte < 0x80 || continuationByte > 0xBF)
			return 0;
	}

	const unsigned char secondByte = static_cast<unsigned char>(line[position + 1]);
	if ((firstByte == 0xE0 && secondByte < 0xA0)
		|| (firstByte == 0xED && secondByte > 0x9F)
		|| (firstByte == 0xF0 && secondByte < 0x90)
		|| (firstByte == 0xF4 && secondByte > 0x8F))
		return 0;
	return sequenceLength;
}

std::uint64_t CsvParser::lineNumber() const
{
	return m_lineNumber;
}

std::size_t CsvParser::bytesRead() const
{
	return m_bytesRead;
}

void CsvParser::getRawLine(std::string &rawLine) const
{
	rawLine = m_lineBuffer;
}

} // namespace csvp
