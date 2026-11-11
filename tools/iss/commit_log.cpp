#include "commit_log.h"

namespace iss {

bool CommitLog::flush() {
    if (len_ > 0 && std::fwrite(buf_, 1, len_, out_) != len_) {
        ok_ = false;
    }
    len_ = 0;
    if (std::fflush(out_) != 0) {
        ok_ = false;
    }
    return ok_;
}

}  // namespace iss
