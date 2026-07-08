#include "novaboot/http3/http3_stream.h"

namespace novaboot::http3 {

Http3Stream::Http3Stream(int64_t stream_id)
    : stream_id_(stream_id) {
}

} // namespace novaboot::http3
