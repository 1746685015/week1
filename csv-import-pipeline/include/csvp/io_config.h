#ifndef CSVP_IO_CONFIG_H_
#define CSVP_IO_CONFIG_H_

#include <cstddef>

#ifndef CSVP_INPUT_BUFFER_BYTES
#define CSVP_INPUT_BUFFER_BYTES 65536U
#endif

#ifndef CSVP_ERROR_LOG_BUFFER_BYTES
#define CSVP_ERROR_LOG_BUFFER_BYTES 65536U
#endif

namespace csvp {

inline constexpr std::size_t INPUT_BUFFER_SIZE = CSVP_INPUT_BUFFER_BYTES;
inline constexpr std::size_t ERROR_LOG_BUFFER_SIZE = CSVP_ERROR_LOG_BUFFER_BYTES;

} // namespace csvp

#endif // CSVP_IO_CONFIG_H_
