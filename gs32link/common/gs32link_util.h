#ifndef GS32LINK_UTIL_H
#define GS32LINK_UTIL_H
/* 驱动内部公共辅助: 错误描述、版本字符串 */
#include "gs32link.h"
#include <string>

namespace gs32 {

/* 每句柄错误状态(非线程安全地绑定在句柄结构内部使用) */
class ErrorState {
public:
    void set(int code, const std::string& msg) { code_ = code; msg_ = msg; }
    void clear() { code_ = GS32_OK; msg_.clear(); }
    int code() const { return code_; }
    const char* text() const { return msg_.empty() ? "no error" : msg_.c_str(); }
private:
    int code_ = GS32_OK;
    std::string msg_ = "no error";
};

} // namespace gs32

#endif /* GS32LINK_UTIL_H */
