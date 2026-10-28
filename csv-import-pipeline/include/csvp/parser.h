#ifndef CSVP_PARSER_H_
#define CSVP_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "csvp/io_config.h"
#include "csvp/row.h"

namespace csvp {

class CsvParser
{
public:
	enum class ReadStatus
	{
		RowReadStatus,
		EofReadStatus,
		ErrorReadStatus,
	};

	struct Config
	{
		char delimiter = ',';
		bool has_header = true;
		bool trim_spaces = true;
		const char *encoding = "UTF-8";
	};

	CsvParser() = default;
	explicit CsvParser(const Config &config);
	~CsvParser();

	CsvParser(const CsvParser &) = delete;
	CsvParser &operator=(const CsvParser &) = delete;

	bool parse(const char *filepath);
	bool readRow(Row &row);
	ReadStatus readNext(Row &row);

	const std::vector<std::string> &header() const
	{
		return m_header;
	}

	std::uint64_t lineNumber() const;
	std::size_t bytesRead() const;
	void getRawLine(std::string &rawLine) const;

	const std::string &lastError() const
	{
		return m_lastError;
	}

private:
	static constexpr std::size_t MAX_LINE_SIZE = static_cast<std::size_t>(16U) * 1024U * 1024U;

	bool readLineFromFile();
	bool discardLineRemainder();
	bool splitLineToFields(const char *line, std::size_t length, Row &row);
	bool handleQuotedField(const char *line, std::size_t &position, std::size_t length,
		std::string &field);
	std::size_t encodedCharacterLength(const char *line, std::size_t position,
		std::size_t length) const;
	bool isGbkEncoding() const;

	Config m_config;
	std::string m_lastError;
	FILE *m_file = nullptr;
	std::vector<char> m_readBuffer = std::vector<char>(INPUT_BUFFER_SIZE);
	std::size_t m_readPosition = 0;
	std::size_t m_readSize = 0;
	std::string m_lineBuffer;
	std::vector<std::string> m_header;
	std::uint64_t m_lineNumber = 0;
	std::size_t m_bytesRead = 0;
	bool m_lineEndedWithNewline = false;
};

} // namespace csvp

#endif // CSVP_PARSER_H_
