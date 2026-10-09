// common/hash 的单元测试
#include "fmt/common/hash.hpp"

#include <string>

#include "fmt_test.hpp"

FMT_TEST(Hash, MD5已有向量) {
    // RFC 1321 附录 A.5 的标准向量
    FMT_CHECK_EQ(fmt::md5_hex(""), std::string("d41d8cd98f00b204e9800998ecf8427e"));
    FMT_CHECK_EQ(fmt::md5_hex("a"), std::string("0cc175b9c0f1b6a831c399e269772661"));
    FMT_CHECK_EQ(fmt::md5_hex("abc"), std::string("900150983cd24fb0d6963f7d28e17f72"));
    FMT_CHECK_EQ(fmt::md5_hex("message digest"),
                 std::string("f96b697d7cb7938d525a2f31aaf161d0"));
    FMT_CHECK_EQ(fmt::md5_hex("abcdefghijklmnopqrstuvwxyz"),
                 std::string("c3fcd3d76192e4007dfb496cca67e13b"));
    FMT_CHECK_EQ(fmt::md5_hex(std::string(1000000, 'a')),
                 std::string("7707d6ae4e027c70eea2a935c2296f21"));
}

FMT_TEST(Hash, 分块与一次算结果一致) {
    const std::string text = "上传时是边下载边算的，所以分块必须得到同一个结果。";

    fmt::Md5 chunked;
    for (std::size_t i = 0; i < text.size(); i += 7) {
        FMT_CHECK(chunked.update(text.substr(i, 7)));
    }
    FMT_CHECK_EQ(chunked.finish(), fmt::md5_hex(text));

    // 空块不影响结果
    fmt::Md5 with_empty;
    FMT_CHECK(with_empty.update(text));
    FMT_CHECK(with_empty.update(""));
    FMT_CHECK_EQ(with_empty.finish(), fmt::md5_hex(text));
}
