#ifndef CSVP_ERROR_SINK_H_
#define CSVP_ERROR_SINK_H_

#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

#include "csvp/io_config.h"
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
	std::vector<char> m_writeBuffer = std::vector<char>(ERROR_LOG_BUFFER_SIZE);
	std::ofstream m_output;
	bool m_failed = false;
};

} // namespace csvp

#endif // CSVP_ERROR_SINK_H_
