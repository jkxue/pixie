#include "src/stirling/utils/byte_format.h"
#include <glog/logging.h>

namespace pl {
namespace stirling {
namespace utils {

// The input bytes are big endian.
// TODO(chengruizhe): Convert to template with [N] to avoid DCHECK.
int BEBytesToInt(const char arr[], size_t size) {
  DCHECK(size <= sizeof(int));
  int result = 0;
  for (size_t i = 0; i < size; i++) {
    result = arr[i] + (result << 8);
  }
  return result;
}

int LEStrToInt(const std::string_view str) {
  DCHECK(str.size() <= sizeof(int));
  int result = 0;
  for (size_t i = 0; i < str.size(); i++) {
    result = str[str.size() - 1 - i] + (result << 8);
  }
  return result;
}

}  // namespace utils
}  // namespace stirling
}  // namespace pl
