#include "../src/image_headers.hpp"

#include <array>
#include <initializer_list>
#include <stdexcept>
#include <string>

using namespace flockv0::message_attachments;

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string bytes(std::initializer_list<std::uint8_t> data) {
    return {reinterpret_cast<const char*>(data.begin()), data.size()};
}

void appendBigEndian(std::string& data, std::uint32_t value, unsigned bytes) {
    while (bytes) data.push_back(static_cast<char>(value >> (8 * --bytes)));
}

std::string pngHeader(std::uint32_t width, std::uint32_t height, std::uint32_t crc, std::array<std::uint8_t, 5> fields = {8, 2, 0, 0, 0}) {
    auto data = bytes({0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 0x49, 0x48, 0x44, 0x52});
    appendBigEndian(data, width, 4);
    appendBigEndian(data, height, 4);
    data.append(fields.begin(), fields.end());
    appendBigEndian(data, crc, 4);
    return data;
}

std::string jpegFrame(std::uint8_t marker, std::uint32_t width, std::uint32_t height) {
    auto data = bytes({0xff, marker, 0, 11, 8});
    appendBigEndian(data, height, 2);
    appendBigEndian(data, width, 2);
    data += bytes({1, 1, 0x11, 0});
    return data;
}

void checkSize(std::optional<ImageSize> size, std::uint32_t width, std::uint32_t height, const char* message) {
    check(size && size->width == width && size->height == height, message);
}

void checkPng() {
    const auto png = pngHeader(37, 19, 0x98df53e4);
    checkSize(pngHeaderSize(png), 37, 19, "valid PNG header lost its dimensions");
    checkSize(pngHeaderSize(pngHeader(37, 19, 0x5a746666, {1, 3, 0, 0, 1})), 37, 19, "indexed and interlaced PNG header was rejected");
    for (std::size_t size = 0; size < png.size(); ++size)
        check(!pngHeaderSize(std::string_view(png).substr(0, size)), "truncated PNG header was accepted");

    auto invalid = png;
    invalid[0] = 0;
    check(!pngHeaderSize(invalid), "incorrect PNG signature was accepted");
    invalid = png;
    invalid[11] = 12;
    check(!pngHeaderSize(invalid), "incorrect PNG IHDR length was accepted");
    invalid = png;
    invalid.replace(12, 4, "IDAT");
    check(!pngHeaderSize(invalid), "PNG with a different first chunk was accepted");
    invalid = png;
    invalid.back() ^= 1;
    check(!pngHeaderSize(invalid), "incorrect PNG IHDR CRC was accepted");

    check(!pngHeaderSize(pngHeader(37, 19, 0x5d2fbee5, {4, 2, 0, 0, 0})), "4-bit truecolor was accepted");
    check(!pngHeaderSize(pngHeader(37, 19, 0x70f3e8c2, {16, 3, 0, 0, 0})), "16-bit indexed color was accepted");
    check(!pngHeaderSize(pngHeader(37, 19, 0x8a6afc0a, {8, 1, 0, 0, 0})), "reserved PNG color type was accepted");
    check(!pngHeaderSize(pngHeader(37, 19, 0x991d39d3, {8, 2, 1, 0, 0})), "unknown PNG compression method was accepted");
    check(!pngHeaderSize(pngHeader(37, 19, 0x81c462a5, {8, 2, 0, 1, 0})), "unknown PNG filter method was accepted");
    check(!pngHeaderSize(pngHeader(37, 19, 0x76d132c8, {8, 2, 0, 0, 2})), "unknown PNG interlace method was accepted");
    check(!pngHeaderSize(pngHeader(0, 19, 0x31ab9b70)) && !pngHeaderSize(pngHeader(37, 0, 0x1d9d23d1)), "zero-size PNG was accepted");
    check(!pngHeaderSize(pngHeader(8193, 1, 0x16ebdc09)) && !pngHeaderSize(pngHeader(1, 8193, 0x5fca6a42)), "PNG edge limit was exceeded");
    check(!pngHeaderSize(pngHeader(8192, 513, 0x6eb6a61e)), "PNG pixel limit was exceeded");
    checkSize(pngHeaderSize(pngHeader(8192, 512, 0xa5ea75bb)), 8192, 512, "PNG at the edge and pixel limits was rejected");
}

void checkJpeg() {
    const auto soi = bytes({0xff, 0xd8});
    const auto baseline = jpegFrame(0xc0, 300, 27);
    const auto progressive = jpegFrame(0xc2, 31, 290);
    const auto prefix = soi + bytes({0xff, 0xff, 0xe1, 0, 5, 0xff, 0xc0, 0xff, 0xff, 0x01});
    checkSize(jpegHeaderSize(soi + baseline), 300, 27, "valid baseline JPEG frame lost its dimensions");
    checkSize(jpegHeaderSize(prefix + progressive), 31, 290, "progressive JPEG with fill bytes and leading markers was rejected");
    const auto jpeg = prefix + progressive;
    for (std::size_t size = 0; size < jpeg.size(); ++size)
        check(!jpegHeaderSize(std::string_view(jpeg).substr(0, size)), "truncated JPEG header was accepted");

    check(!jpegHeaderSize(baseline), "JPEG without SOI was accepted");
    check(!jpegHeaderSize(prefix + bytes({0xff, 0xd9})), "JPEG without SOF was accepted");
    check(!jpegHeaderSize(soi + bytes({0xff, 0xda, 0, 2}) + baseline), "JPEG with SOS before SOF was accepted");
    check(!jpegHeaderSize(soi + bytes({0xff, 0xd0}) + baseline), "JPEG with a restart marker outside a scan was accepted");
    check(!jpegHeaderSize(soi + bytes({0xff, 0xe1, 0, 1}) + baseline), "JPEG segment length below two was accepted");
    check(!jpegHeaderSize(soi + bytes({0xff, 0xe1, 0xff, 0xff}) + baseline), "JPEG segment extended past the file");
    check(!jpegHeaderSize(soi + bytes({0xff, 0, 0, 2}) + baseline), "stuffed JPEG marker byte was accepted in the header");
    check(!jpegHeaderSize(soi + bytes({0xff, 0xfe, 0, 2, 7}) + baseline), "stray data between JPEG markers was accepted");
    auto invalid = baseline;
    invalid[9] = 2;
    check(!jpegHeaderSize(soi + invalid), "JPEG frame with missing component data was accepted");
    invalid = baseline;
    invalid[11] = 0x10;
    check(!jpegHeaderSize(soi + invalid), "JPEG frame with an invalid sampling factor was accepted");
    invalid = baseline;
    invalid[4] = 12;
    check(!jpegHeaderSize(soi + invalid), "baseline JPEG with 12-bit samples was accepted");

    check(!jpegHeaderSize(soi + jpegFrame(0xc0, 0, 27)) && !jpegHeaderSize(soi + jpegFrame(0xc0, 27, 0)), "zero-size JPEG was accepted");
    check(!jpegHeaderSize(soi + jpegFrame(0xc0, 8193, 1)) && !jpegHeaderSize(soi + jpegFrame(0xc0, 1, 8193)),
          "JPEG edge limit was exceeded");
    check(!jpegHeaderSize(soi + jpegFrame(0xc0, 8192, 513)), "JPEG pixel limit was exceeded");
    checkSize(jpegHeaderSize(soi + jpegFrame(0xc0, 8192, 512)), 8192, 512, "JPEG at the edge and pixel limits was rejected");
}
}

int main() {
    checkPng();
    checkJpeg();
}
