#ifndef CSVP_ERROR_SINK_H_
#define CSVP_ERROR_SINK_H_

#include <array>
#include <cstddef>
#include <fstream>
#include <string>

#include "csvp/types.h"

namespace csvp {

class ErrorSink
{
public:
	ErrorSink() = default;
	~ErrorSink() = default;

	ErrorSink(const ErrorSink &) = delete;
	ErrorSink &operator=(const ErrorSink &) = delete;

	bool open(const std::string &path, std::string &errorMessage);
	bool write(const RowError &error, std::string &errorMessage);
	bool close(std::string &errorMessage);

private:
	static constexpr std::size_t WRITE_BUFFER_SIZE = static_cast<std::size_t>(64U) * 1024U;

	std::array<char, WRITE_BUFFER_SIZE> m_writeBuffer{};
	std::ofstream m_output;
	bool m_failed = false;
};

} // namespace csvp

#endif // CSVP_ERROR_SINK_H_
