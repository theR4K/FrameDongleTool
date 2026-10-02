// set_region.cpp — show or set the Steam Frame 6 GHz adapter country code.
//
// Usage:  set_region          show the current country code
//         set_region <CC>     set the country code (ISO 3166-1 alpha-2: PL, US, DE, ...)
//
// Build (x64 Native Tools Command Prompt for VS 2022):
//   cl /std:c++latest /EHsc /O2 /W4 /permissive- /utf-8 set_region.cpp
//      /link /MANIFESTUAC:"level='requireAdministrator'"
//
// All pure logic is constexpr and verified at compile time (see `tests` below).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <setupapi.h>
#include <wlanapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "wlanapi.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

static_assert(sizeof(void*) == 8,
              "build for x64: a 32-bit process is redirected from System32\\drivers to SysWOW64");

using Byte = std::uint8_t;
using Bytes = std::span<const Byte>;

// ── ASCII string helpers ────────────────────────────────────────────────
namespace text {

[[nodiscard]] constexpr auto lower(char c) noexcept -> char {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
[[nodiscard]] constexpr auto upper(char c) noexcept -> char {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}
[[nodiscard]] constexpr auto to_lower(std::string_view s) -> std::string {
    std::string out(s);
    std::ranges::transform(out, out.begin(), lower);
    return out;
}
[[nodiscard]] constexpr auto iequals(std::string_view a, std::string_view b) noexcept -> bool {
    return std::ranges::equal(a, b, {}, lower, lower);
}
[[nodiscard]] constexpr auto trim(std::string_view s) noexcept -> std::string_view {
    constexpr std::string_view ws = " \t\r\n";
    const auto first = s.find_first_not_of(ws);
    if (first == std::string_view::npos) return {};
    return s.substr(first, s.find_last_not_of(ws) - first + 1);
}
[[nodiscard]] constexpr auto hex_digit(char c) noexcept -> std::optional<unsigned> {
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
    return std::nullopt;
}
[[nodiscard]] constexpr auto to_hex(Bytes bytes) -> std::string {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0xF]);
    }
    return out;
}
[[nodiscard]] constexpr auto is_printable(char c) noexcept -> bool { return c >= 32 && c <= 126; }

} // namespace text

// ── vendor protocol (mirrors protocol.rs) ───────────────────────────────
namespace proto {

constexpr void put_u32(std::span<Byte> out, std::size_t at, std::uint32_t value) noexcept {
    for (std::size_t i = 0; i < 4; ++i) out[at + i] = static_cast<Byte>(value >> (8 * i));
}
[[nodiscard]] constexpr auto get_u32(Bytes in, std::size_t at) noexcept -> std::uint32_t {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) value |= std::uint32_t{in[at + i]} << (8 * i);
    return value;
}

enum class Oid : std::uint32_t {
    country    = 0xff81521f,
    diagnostic = 0xff818040,
    done       = 0xff818041,
    output     = 0xff818042,
};

inline constexpr std::size_t header_size = 40;
inline constexpr std::size_t plain_body_size = 300;
inline constexpr std::size_t diag_body_size = 336;
inline constexpr std::size_t token_size = 16;

using PlainPacket = std::array<Byte, header_size + plain_body_size>;
using DiagPacket = std::array<Byte, header_size + diag_body_size>;

// Header: 10 little-endian u32 words; body follows.
template <std::size_t BodySize>
[[nodiscard]] constexpr auto make_header(Oid oid) noexcept {
    std::array<Byte, header_size + BodySize> packet{};
    constexpr auto n = static_cast<std::uint32_t>(BodySize);
    const std::array<std::uint32_t, 10> words{
        n + 36, 0x814ce000, 4, 1, 7, n + 40, std::to_underlying(oid), n + 24, n + 24, n};
    for (std::size_t i = 0; i < words.size(); ++i) put_u32(packet, i * 4, words[i]);
    return packet;
}

// Diagnostic body: [0, 0, subcommand, 3] + NUL-padded 16-byte tokens.
[[nodiscard]] constexpr auto make_diag(std::uint32_t subcommand,
                                       std::initializer_list<std::string_view> tokens) -> DiagPacket {
    auto packet = make_header<diag_body_size>(Oid::diagnostic);
    put_u32(packet, header_size + 8, subcommand);
    put_u32(packet, header_size + 12, 3);
    auto at = header_size + 16;
    for (const auto token : tokens) {
        if (token.size() >= token_size) throw std::length_error("diagnostic token too long");
        std::ranges::transform(token, packet.begin() + static_cast<std::ptrdiff_t>(at),
                               [](char c) { return static_cast<Byte>(c); });
        at += token_size;
    }
    return packet;
}

inline constexpr PlainPacket country_request = make_header<plain_body_size>(Oid::country);
inline constexpr PlainPacket done_request = make_header<plain_body_size>(Oid::done);
inline constexpr PlainPacket output_request = make_header<plain_body_size>(Oid::output);
inline constexpr DiagPacket info_request = make_diag(0, {"echo", "core", "6g_info"});

// User-supplied target: exactly two ASCII letters, stored upper-case.
class CountryCode {
public:
    [[nodiscard]] static constexpr auto parse(std::string_view s) noexcept -> std::optional<CountryCode> {
        if (s.size() != 2) return std::nullopt;
        std::array<char, 2> chars{};
        for (std::size_t i = 0; i < 2; ++i) {
            chars[i] = text::upper(s[i]);
            if (chars[i] < 'A' || chars[i] > 'Z') return std::nullopt;
        }
        return CountryCode{chars};
    }
    [[nodiscard]] constexpr auto view() const noexcept -> std::string_view {
        return {chars_.data(), chars_.size()};
    }

private:
    constexpr explicit CountryCode(std::array<char, 2> chars) noexcept : chars_{chars} {}
    std::array<char, 2> chars_;
};

[[nodiscard]] constexpr auto set_request(CountryCode code) -> DiagPacket {
    return make_diag(29, {"67", code.view()});
}

// Validates the driver's echo of our header; returns the payload view into `response`.
[[nodiscard]] constexpr auto decode(Bytes request, Bytes response, std::size_t returned) noexcept
    -> std::expected<Bytes, std::string_view> {
    if (request.size() < header_size || response.size() != request.size() || returned < header_size ||
        returned > response.size())
        return std::unexpected("invalid response length");
    if (!std::ranges::equal(request.first(28), response.first(28)) ||
        !std::ranges::equal(request.subspan(36, 4), response.subspan(36, 4)))
        return std::unexpected("response header mismatch");
    const std::size_t written = get_u32(response, 28);
    if (get_u32(response, 32) != 0 || written > get_u32(response, 36) || header_size + written > returned)
        return std::unexpected("driver did not confirm a valid response length");
    return response.subspan(header_size, written);
}

// Country as reported by the driver. 00 00 (observed on real hardware) means "not set".
struct Country {
    std::array<char, 2> chars{};

    [[nodiscard]] constexpr auto known() const noexcept -> bool { return chars != std::array<char, 2>{}; }
    [[nodiscard]] constexpr auto view() const noexcept -> std::string_view {
        return known() ? std::string_view{chars.data(), chars.size()} : "unset (00 00)";
    }
    [[nodiscard]] constexpr auto is(CountryCode code) const noexcept -> bool {
        return known() && view() == code.view();
    }
};

[[nodiscard]] constexpr auto parse_country(Bytes payload) noexcept -> std::expected<Country, std::string_view> {
    if (payload.size() != 2) return std::unexpected("invalid country response (expected 2 bytes)");
    const Country country{{static_cast<char>(payload[0]), static_cast<char>(payload[1])}};
    if (!country.known()) return country;
    if (!std::ranges::all_of(country.chars, text::is_printable))
        return std::unexpected("invalid country response (expected 2 printable ASCII bytes)");
    return country;
}

[[nodiscard]] constexpr auto parse_done(Bytes payload) noexcept -> std::expected<bool, std::string_view> {
    if (payload.size() != 4 || get_u32(payload, 0) > 1) return std::unexpected("invalid completion flag");
    return get_u32(payload, 0) == 1;
}

// Reassembles the diagnostic text from output blocks: [mode, status, sequence, len] + data.
class OutputAssembler {
public:
    // Returns true once the text is complete (NUL seen or final block).
    [[nodiscard]] constexpr auto push(Bytes block) -> std::expected<bool, std::string_view> {
        if (block.size() < 16) return std::unexpected("malformed output block");
        const auto mode = get_u32(block, 0);
        const auto status = get_u32(block, 4);
        const auto sequence = get_u32(block, 8);
        const std::size_t len = get_u32(block, 12);
        if (mode != 0 || (status != 0 && status != 2) || len > 200 || block.size() != 16 + len)
            return std::unexpected("malformed output block");
        if (sequence_ && *sequence_ != sequence)
            return std::unexpected("output sequence changed: another diagnostic tool is using the driver");
        sequence_ = sequence;
        for (const auto b : block.subspan(16)) {
            if (b == 0) return true;
            text_.push_back(static_cast<char>(b));
        }
        return status == 0;
    }

    [[nodiscard]] constexpr auto take() && -> std::expected<std::string, std::string_view> {
        if (!std::ranges::all_of(text_, [](char c) { return static_cast<unsigned char>(c) < 0x80; }))
            return std::unexpected("output contains non-ASCII data");
        std::erase(text_, '\r');
        return std::move(text_);
    }

private:
    std::optional<std::uint32_t> sequence_;
    std::string text_;
};

[[nodiscard]] constexpr auto lines(std::string_view s) {
    return s | std::views::split('\n') |
           std::views::transform([](auto&& r) { return text::trim(std::string_view{r.begin(), r.end()}); });
}

// Mirrors Status::manual_us_supported, minus the country part.
[[nodiscard]] constexpr auto manual_6ghz_supported(std::string_view info) -> bool {
    return std::ranges::any_of(lines(info), [](std::string_view l) {
        return l.starts_with("6G Support (domain:") && !l.contains("domain:00") &&
               l.contains("REGU_RSN_MANUAL");
    });
}

struct Status {
    Country country;
    std::string info;

    [[nodiscard]] constexpr auto active(CountryCode code) const -> bool {
        return country.is(code) && manual_6ghz_supported(info);
    }
};

} // namespace proto

// ── hardware / driver identification (mirrors backend.rs) ──────────────
namespace hw {

inline constexpr std::string_view driver_service = "rtwlanuval";
inline constexpr std::string_view verified_sha256 =
    "378ffaee3b782b9a7f5382c294a846e7c4b3b952717991fd1fd93795c5a85612";
inline constexpr std::string_view verified_version = "5.32.908.2026";

// Exact VID_/PID_ tokens in the first component of a USB instance ID.
[[nodiscard]] constexpr auto is_steam_frame(std::string_view instance_id) -> bool {
    constexpr std::string_view prefix = "USB\\";
    if (instance_id.size() < prefix.size() || !text::iequals(instance_id.substr(0, prefix.size()), prefix))
        return false;
    auto component = instance_id.substr(prefix.size());
    component = component.substr(0, component.find('\\'));
    bool vid = false;
    bool pid = false;
    for (auto&& part : component | std::views::split('&')) {
        const std::string_view token{part.begin(), part.end()};
        vid = vid || text::iequals(token, "VID_28DE");
        pid = pid || text::iequals(token, "PID_2432");
    }
    return vid && pid;
}

[[nodiscard]] constexpr auto supported(std::string_view instance_id, std::string_view service) -> bool {
    return is_steam_frame(instance_id) && text::iequals(service, driver_service);
}

// Resolves a service ImagePath to a lower-case absolute path, the way the kernel loader does.
[[nodiscard]] constexpr auto resolve_image_path(std::string_view image, std::string_view windows_dir)
    -> std::string {
    auto path = text::to_lower(text::trim(image));
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
    const auto windir = text::to_lower(windows_dir);
    if (path.starts_with("\\??\\"))
        path.erase(0, 4);
    else if (path.starts_with("\\systemroot\\"))
        path.replace(0, 12, windir + "\\");
    else if (path.starts_with("system32\\")) // relative ImagePath is relative to %SystemRoot%
        path.insert(0, windir + "\\");
    return path;
}

[[nodiscard]] constexpr auto expected_driver_path(std::string_view system_dir) -> std::string {
    return text::to_lower(system_dir) + "\\drivers\\" + std::string(driver_service) + ".sys";
}

[[nodiscard]] constexpr auto parse_guid(std::string_view s) noexcept -> std::optional<GUID> {
    s = text::trim(s);
    if (s.size() == 38 && s.front() == '{' && s.back() == '}') s = s.substr(1, 36);
    if (s.size() != 36 || s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return std::nullopt;
    bool ok = true;
    const auto hex = [&](std::size_t at, std::size_t len) {
        std::uint32_t v = 0;
        for (const char c : s.substr(at, len)) {
            const auto d = text::hex_digit(c);
            ok = ok && d.has_value();
            v = (v << 4) | d.value_or(0);
        }
        return v;
    };
    GUID g{};
    g.Data1 = hex(0, 8);
    g.Data2 = static_cast<unsigned short>(hex(9, 4));
    g.Data3 = static_cast<unsigned short>(hex(14, 4));
    g.Data4[0] = static_cast<unsigned char>(hex(19, 2));
    g.Data4[1] = static_cast<unsigned char>(hex(21, 2));
    for (std::size_t i = 0; i < 6; ++i) g.Data4[2 + i] = static_cast<unsigned char>(hex(24 + 2 * i, 2));
    if (!ok) return std::nullopt;
    return g;
}

[[nodiscard]] constexpr auto same_guid(const GUID& a, const GUID& b) noexcept -> bool {
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
           std::ranges::equal(a.Data4, b.Data4);
}

// {4d36e972-e325-11ce-bfc1-08002be10318}: network adapter device setup class.
inline constexpr GUID net_class{0x4d36e972, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

} // namespace hw

// ── compile-time tests ───────────────
namespace tests {

constexpr auto code(std::string_view s) { return proto::CountryCode::parse(s).value(); }

static_assert(proto::CountryCode::parse("pl").has_value() && code("pl").view() == "PL");
static_assert(!proto::CountryCode::parse("P").has_value());
static_assert(!proto::CountryCode::parse("P1").has_value());
static_assert(!proto::CountryCode::parse("PLX").has_value());

static_assert([] {
    const auto p = proto::set_request(code("US"));
    constexpr std::array<Byte, 16> head{0, 0, 0, 0, 0, 0, 0, 0, 29, 0, 0, 0, 3, 0, 0, 0};
    const auto body = std::span{p}.subspan(proto::header_size);
    return p.size() == 376 && proto::get_u32(p, 24) == 0xff818040 && proto::get_u32(p, 0) == 336 + 36 &&
           std::ranges::equal(body.first(16), head) && body[16] == '6' && body[17] == '7' &&
           std::ranges::all_of(body.subspan(18, 14), [](Byte b) { return b == 0; }) && body[32] == 'U' &&
           body[33] == 'S' && std::ranges::all_of(body.subspan(34), [](Byte b) { return b == 0; });
}());
static_assert(proto::country_request.size() == 340 && proto::get_u32(proto::country_request, 24) == 0xff81521f);
static_assert(proto::get_u32(proto::info_request, 48) == 0 && proto::info_request[56] == 'e' &&
              proto::info_request[72] == 'c' && proto::info_request[88] == '6');

static_assert([] {
    const auto& request = proto::country_request;
    auto response = request;
    proto::put_u32(response, 28, 2);
    proto::put_u32(response, 32, 0);
    response[40] = 'U';
    response[41] = 'S';
    const auto ok = proto::decode(request, response, response.size());
    if (!ok || ok->size() != 2 || (*ok)[0] != 'U' || (*ok)[1] != 'S') return false;
    if (proto::decode(request, request, request.size())) return false;
    if (proto::decode(request, response, 41)) return false;
    response[0] ^= 1;
    return !proto::decode(request, response, response.size());
}());

static_assert(proto::parse_country(std::array<Byte, 2>{'U', 'S'}).value().view() == "US");
static_assert(!proto::parse_country(std::array<Byte, 2>{0, 0}).value().known());
static_assert(!proto::parse_country(std::array<Byte, 2>{0, 'S'}));
static_assert(!proto::parse_country(std::array<Byte, 2>{0xFF, 0x80}));
static_assert(!proto::parse_country(std::array<Byte, 3>{'U', 'S', 0}));
static_assert(!proto::parse_country(std::span<const Byte>{}));

static_assert(proto::parse_done(std::array<Byte, 4>{1, 0, 0, 0}).value());
static_assert(!proto::parse_done(std::array<Byte, 4>{0, 0, 0, 0}).value());
static_assert(!proto::parse_done(std::array<Byte, 4>{2, 0, 0, 0}));
static_assert(!proto::parse_done(std::array<Byte, 2>{1, 0}));

constexpr auto output_block(std::string_view data, std::uint32_t sequence, std::uint32_t status = 0) {
    std::vector<Byte> b(16 + data.size());
    proto::put_u32(b, 4, status);
    proto::put_u32(b, 8, sequence);
    proto::put_u32(b, 12, static_cast<std::uint32_t>(data.size()));
    std::ranges::transform(data, b.begin() + 16, [](char c) { return static_cast<Byte>(c); });
    return b;
}
static_assert([] {
    proto::OutputAssembler out;
    if (out.push(output_block("6G In", 7, 2)) != false) return false;
    if (out.push(output_block(std::string_view{"fo\r\nok\0padding", 14}, 7, 2)) != true) return false;
    return std::move(out).take() == "6G Info\nok";
}());
static_assert([] {
    proto::OutputAssembler out;
    return out.push(output_block("part", 1, 2)).has_value() && !out.push(output_block("changed", 2));
}());
static_assert([] {
    for (std::size_t len = 0; len < 16; ++len)
        if (proto::OutputAssembler{}.push(std::vector<Byte>(len))) return false;
    proto::OutputAssembler out;
    return out.push(output_block("\xC3\xA9", 1)).has_value() && !std::move(out).take();
}());

static_assert(proto::manual_6ghz_supported("6G Info\n6G Support (domain:05), due to REGU_RSN_MANUAL"));
static_assert(!proto::manual_6ghz_supported("6G NOT Support"));
static_assert(!proto::manual_6ghz_supported("6G Support (domain:00), due to REGU_RSN_MANUAL"));
static_assert(!proto::manual_6ghz_supported("6G Support (domain:05), due to REGU_RSN_11D"));
static_assert(!proto::manual_6ghz_supported("6G Support (domain:05)\nREGU_RSN_MANUAL"));
static_assert(proto::Status{{{'P', 'L'}}, "6G Support (domain:05), due to REGU_RSN_MANUAL"}.active(code("PL")));
static_assert(!proto::Status{{{'U', 'S'}}, "6G Support (domain:05), due to REGU_RSN_MANUAL"}.active(code("PL")));
static_assert(!proto::Status{{}, "6G Support (domain:05), due to REGU_RSN_MANUAL"}.active(code("PL")));

static_assert(hw::is_steam_frame("USB\\VID_28DE&PID_2432\\ONE"));
static_assert(hw::is_steam_frame("usb\\vid_28de&pid_2432&rev_0001\\TWO"));
static_assert(hw::is_steam_frame("USB\\VID_28DE&PID_2432&MI_00\\7&1234&0&0000"));
static_assert(!hw::is_steam_frame("PCI\\VEN_14C3&DEV_7922\\OTHER"));
static_assert(!hw::is_steam_frame("USB\\VID_28DE&PID_24320\\OTHER"));
static_assert(!hw::is_steam_frame("PCI\\VID_28DE&PID_2432\\x"));
static_assert(!hw::is_steam_frame("USB\\VID_1234&PID_2432\\x"));
static_assert(!hw::is_steam_frame("USB\\VID_1234\\VID_28DE&PID_2432"));
static_assert(!hw::is_steam_frame(""));
static_assert(hw::supported("usb\\vid_28de&pid_2432&rev_0001\\x", "RTWLANUVAL"));
static_assert(!hw::supported("USB\\VID_28DE&PID_2432\\x", "other"));

static_assert(hw::resolve_image_path("\\SystemRoot\\System32\\drivers\\rtwlanuval.sys", "C:\\WINDOWS") ==
              "c:\\windows\\system32\\drivers\\rtwlanuval.sys");
static_assert(hw::resolve_image_path("System32\\DRIVERS\\rtwlanuval.sys", "C:\\Windows") ==
              "c:\\windows\\system32\\drivers\\rtwlanuval.sys");
static_assert(hw::resolve_image_path("\"\\??\\C:\\Windows\\System32\\drivers\\rtwlanuval.sys\"", "C:\\Windows") ==
              "c:\\windows\\system32\\drivers\\rtwlanuval.sys");
static_assert(hw::expected_driver_path("C:\\Windows\\System32") == "c:\\windows\\system32\\drivers\\rtwlanuval.sys");

static_assert(hw::same_guid(hw::parse_guid("{4D36E972-E325-11CE-BFC1-08002BE10318}").value(), hw::net_class));
static_assert(hw::same_guid(hw::parse_guid("4d36e972-e325-11ce-bfc1-08002be10318").value(), hw::net_class));
static_assert(!hw::parse_guid("4d36e972-e325-11ce-bfc1-08002be1031g"));
static_assert(!hw::parse_guid("{4d36e972e32511cebfc108002be10318}"));

static_assert(text::to_hex(std::array<Byte, 3>{0x00, 0xab, 0xff}) == "00abff");

} // namespace tests

// ── Windows plumbing ────────────────────────────────────────────────────
namespace win {

using Error = std::string;
template <class T> using Result = std::expected<T, Error>;

[[nodiscard]] auto api_error(std::string_view what, DWORD code) -> std::unexpected<Error> {
    return std::unexpected(std::format("{} failed: Windows error {}{}", what, code,
                                       code == ERROR_ACCESS_DENIED ? " (run as administrator)" : ""));
}

struct HandleCloser {
    void operator()(HANDLE h) const noexcept { ::CloseHandle(h); }
};
struct KeyCloser {
    void operator()(HKEY k) const noexcept { ::RegCloseKey(k); }
};
struct DevInfoCloser {
    void operator()(HDEVINFO d) const noexcept { ::SetupDiDestroyDeviceInfoList(d); }
};
struct WlanCloser {
    void operator()(HANDLE h) const noexcept { ::WlanCloseHandle(h, nullptr); }
};
struct WlanFree {
    void operator()(void* p) const noexcept { ::WlanFreeMemory(p); }
};
using unique_handle = std::unique_ptr<void, HandleCloser>;
using unique_key = std::unique_ptr<std::remove_pointer_t<HKEY>, KeyCloser>;
using unique_devinfo = std::unique_ptr<void, DevInfoCloser>;
using unique_wlan = std::unique_ptr<void, WlanCloser>;
template <class T> using unique_wlan_memory = std::unique_ptr<T, WlanFree>;

[[nodiscard]] auto to_utf8(std::wstring_view w) -> std::string {
    if (w.empty()) return {};
    const auto wlen = static_cast<int>(w.size());
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(std::max(n, 0)), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, s.data(), n, nullptr, nullptr);
    return s;
}

// REG_SZ / REG_EXPAND_SZ (expanded) value; nullopt if missing or another type.
[[nodiscard]] auto reg_string(HKEY root, const wchar_t* subkey, const wchar_t* name) -> std::optional<std::wstring> {
    std::wstring value(128, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        auto bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
        const auto rc = ::RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes);
        if (rc == ERROR_SUCCESS) {
            value.resize(bytes / sizeof(wchar_t));
            while (!value.empty() && value.back() == L'\0') value.pop_back();
            return value;
        }
        if (rc != ERROR_MORE_DATA) return std::nullopt;
        value.resize(bytes / sizeof(wchar_t) + 1);
    }
    return std::nullopt;
}

[[nodiscard]] auto system_dir() -> std::wstring {
    std::array<wchar_t, MAX_PATH> buf{};
    const auto n = ::GetSystemDirectoryW(buf.data(), static_cast<UINT>(buf.size()));
    return (n > 0 && n < buf.size()) ? std::wstring(buf.data(), n) : std::wstring{};
}
[[nodiscard]] auto windows_dir() -> std::wstring {
    std::array<wchar_t, MAX_PATH> buf{};
    const auto n = ::GetWindowsDirectoryW(buf.data(), static_cast<UINT>(buf.size()));
    return (n > 0 && n < buf.size()) ? std::wstring(buf.data(), n) : std::wstring{};
}

[[nodiscard]] auto read_file(const std::filesystem::path& path) -> Result<std::vector<Byte>> {
    std::ifstream in{path, std::ios::binary};
    if (!in) return std::unexpected(std::format("cannot open {}", to_utf8(path.native())));
    std::vector<Byte> data(static_cast<std::size_t>(std::filesystem::file_size(path)));
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!in) return std::unexpected(std::format("cannot read {}", to_utf8(path.native())));
    return data;
}

[[nodiscard]] auto sha256(Bytes data) -> Result<std::array<Byte, 32>> {
    std::array<Byte, 32> digest{};
    const auto status = ::BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data.data()),
                                     static_cast<ULONG>(data.size()), digest.data(), static_cast<ULONG>(digest.size()));
    if (!BCRYPT_SUCCESS(status)) return std::unexpected(std::format("BCryptHash failed: 0x{:08x}", status));
    return digest;
}

// System-wide mutex: the driver uses shared buffers,
// so concurrent diagnostics corrupt each other.
[[nodiscard]] auto acquire_diagnostic_lock() -> Result<unique_handle> {
    HANDLE raw = ::CreateMutexW(nullptr, TRUE, L"Global\\SteamFrameLabOriginalDriverDiagnosticV1");
    const auto err = ::GetLastError();
    if (!raw) return api_error("CreateMutexW", err);
    unique_handle lock{raw};
    if (err == ERROR_ALREADY_EXISTS)
        return std::unexpected("another diagnostic is using the driver; close other tools (e.g. the Steam Frame 6 GHz app) and retry");
    return lock;
}

// A present network adapter, as Win32_NetworkAdapter would report it.
struct NetDevice {
    GUID interface_guid;
    std::string instance_id;
    std::string service;
};

[[nodiscard]] auto net_devices() -> Result<std::vector<NetDevice>> {
    HDEVINFO raw = ::SetupDiGetClassDevsW(&hw::net_class, nullptr, nullptr, DIGCF_PRESENT);
    if (raw == INVALID_HANDLE_VALUE) return api_error("SetupDiGetClassDevsW", ::GetLastError());
    const unique_devinfo set{raw};

    std::vector<NetDevice> devices;
    SP_DEVINFO_DATA info{.cbSize = sizeof(SP_DEVINFO_DATA)};
    for (DWORD i = 0; ::SetupDiEnumDeviceInfo(set.get(), i, &info); ++i) {
        // The interface GUID lives in the driver (class) key as NetCfgInstanceId.
        HKEY raw_key = ::SetupDiOpenDevRegKey(set.get(), &info, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (raw_key == INVALID_HANDLE_VALUE) continue;
        const unique_key key{raw_key};
        const auto cfg = reg_string(key.get(), nullptr, L"NetCfgInstanceId");
        if (!cfg) continue;
        const auto guid = hw::parse_guid(to_utf8(*cfg));
        if (!guid) continue;

        std::array<wchar_t, 512> id{};
        if (!::SetupDiGetDeviceInstanceIdW(set.get(), &info, id.data(), static_cast<DWORD>(id.size()), nullptr))
            continue;
        std::array<wchar_t, 256> service{};
        ::SetupDiGetDeviceRegistryPropertyW(set.get(), &info, SPDRP_SERVICE, nullptr,
                                            reinterpret_cast<PBYTE>(service.data()),
                                            static_cast<DWORD>((service.size() - 1) * sizeof(wchar_t)), nullptr);
        devices.push_back({*guid, to_utf8(id.data()), to_utf8(service.data())});
    }
    return devices;
}

struct WlanInterface {
    GUID guid;
    std::string description;
};

class WlanClient {
public:
    [[nodiscard]] static auto open() -> Result<WlanClient> {
        DWORD version = 0;
        HANDLE raw = nullptr;
        if (const auto rc = ::WlanOpenHandle(2, nullptr, &version, &raw); rc != ERROR_SUCCESS)
            return api_error("WlanOpenHandle", rc);
        return WlanClient{unique_wlan{raw}};
    }

    [[nodiscard]] auto interfaces() const -> Result<std::vector<WlanInterface>> {
        WLAN_INTERFACE_INFO_LIST* raw = nullptr;
        if (const auto rc = ::WlanEnumInterfaces(handle_.get(), nullptr, &raw); rc != ERROR_SUCCESS)
            return api_error("WlanEnumInterfaces", rc);
        const unique_wlan_memory<WLAN_INTERFACE_INFO_LIST> list{raw};
        if (!list) return std::unexpected("WlanEnumInterfaces returned a null list");
        if (list->dwNumberOfItems > 1024) return std::unexpected("WLAN interface count invalid");
        // Windows owns this variable-length array; take only its reported item count.
        const std::span entries{&list->InterfaceInfo[0], list->dwNumberOfItems};
        std::vector<WlanInterface> out;
        out.reserve(entries.size());
        for (const auto& e : entries) {
            const auto& desc = e.strInterfaceDescription;
            out.push_back({e.InterfaceGuid, to_utf8({desc, ::wcsnlen(desc, std::size(desc))})});
        }
        return out;
    }

    [[nodiscard]] auto get() const noexcept -> HANDLE { return handle_.get(); }

private:
    explicit WlanClient(unique_wlan h) noexcept : handle_{std::move(h)} {}
    unique_wlan handle_;
};

} // namespace win

// ── adapter discovery & driver integrity (mirrors backend.rs enumerate) ─
using win::Error;
using win::Result;

struct Adapter {
    GUID guid;
    std::string name;
    std::string instance_id;
    std::string service;
};

[[nodiscard]] auto find_steam_frame(const win::WlanClient& wlan) -> Result<Adapter> {
    const auto interfaces = wlan.interfaces();
    if (!interfaces) return std::unexpected(interfaces.error());
    const auto devices = win::net_devices();
    if (!devices) return std::unexpected(devices.error());

    std::vector<Adapter> found;
    for (const auto& iface : *interfaces) {
        const auto dev = std::ranges::find_if(
            *devices, [&](const win::NetDevice& d) { return hw::same_guid(d.interface_guid, iface.guid); });
        // Identify by USB hardware ID, never by a renameable display name or "first interface".
        if (dev == devices->end() || !hw::is_steam_frame(dev->instance_id)) continue;
        found.push_back({iface.guid, iface.description, dev->instance_id, dev->service});
    }

    if (found.empty())
        return std::unexpected("no Steam Frame adapter (USB VID_28DE&PID_2432) among WLAN interfaces; is it plugged in?");
    if (found.size() > 1)
        return std::unexpected(std::format("{} Steam Frame adapters found; plug in only one", found.size()));
    auto& adapter = found.front();
    if (!hw::supported(adapter.instance_id, adapter.service))
        return std::unexpected(std::format("adapter {} uses service '{}', not '{}'", adapter.instance_id,
                                           adapter.service, hw::driver_service));
    return std::move(adapter);
}

struct DriverInfo {
    std::string sha256;
    [[nodiscard]] auto verified() const noexcept -> bool { return sha256 == hw::verified_sha256; }
};

// Service path must be <System32>\drivers\rtwlanuval.sys, the file must be readable.
// An unknown hash is only a warning (same policy as driver_compatibility).
[[nodiscard]] auto inspect_driver() -> Result<DriverInfo> {
    const auto image = win::reg_string(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\rtwlanuval",
                                       L"ImagePath");
    const auto system_dir = win::system_dir();
    const auto windows_dir = win::windows_dir();
    if (!image || system_dir.empty() || windows_dir.empty())
        return std::unexpected("cannot read the rtwlanuval service path; refusing private requests");
    const auto actual = hw::resolve_image_path(win::to_utf8(*image), win::to_utf8(windows_dir));
    const auto expected = hw::expected_driver_path(win::to_utf8(system_dir));
    if (actual != expected)
        return std::unexpected(std::format("driver service path '{}' is not '{}'; refusing private requests",
                                           actual, expected));

    const auto data = win::read_file(std::filesystem::path{system_dir} / L"drivers" / L"rtwlanuval.sys");
    if (!data) return std::unexpected(data.error());
    const auto digest = win::sha256(*data);
    if (!digest) return std::unexpected(digest.error());
    return DriverInfo{text::to_hex(*digest)};
}

// ── IHV transport + protocol operations (mirrors protocol.rs) ──────────
class Driver {
public:
    Driver(HANDLE wlan, const GUID& iface) noexcept : wlan_{wlan}, iface_{iface} {}

    [[nodiscard]] auto country() const -> Result<proto::Country> {
        const auto payload = exchange(proto::country_request);
        if (!payload) return std::unexpected(payload.error());
        const auto country = proto::parse_country(*payload);
        if (!country)
            return std::unexpected(std::format("{}: payload_len={}, payload_hex={}", country.error(),
                                               payload->size(), text::to_hex(*payload)));
        return *country;
    }

    // consume stale done → submit once → wait for done → read output. Never resends.
    [[nodiscard]] auto diagnostic(const proto::DiagPacket& command) const -> Result<std::string> {
        using namespace std::chrono_literals;
        if (const auto stale = done(); !stale) return std::unexpected(stale.error());
        if (const auto sent = exchange(command); !sent) return std::unexpected(sent.error());

        bool completed = false;
        for (int i = 0; i < 20 && !completed; ++i) {
            std::this_thread::sleep_for(100ms);
            const auto flag = done();
            if (!flag) return std::unexpected(flag.error());
            completed = *flag;
        }
        if (!completed)
            return std::unexpected("timed out waiting for the driver; request was not resent, actual state unknown");

        proto::OutputAssembler output;
        for (int i = 0; i < 64; ++i) {
            const auto block = exchange(proto::output_request);
            if (!block) return std::unexpected(block.error());
            const auto complete = output.push(*block);
            if (!complete) return std::unexpected(Error{complete.error()});
            if (*complete) {
                auto text = std::move(output).take();
                if (!text) return std::unexpected(Error{text.error()});
                return std::move(*text);
            }
        }
        return std::unexpected("driver output exceeded the limit; not resent");
    }

    [[nodiscard]] auto status() const -> Result<proto::Status> {
        const auto country = this->country();
        if (!country) return std::unexpected(country.error());
        auto info = diagnostic(proto::info_request);
        if (!info) return std::unexpected(info.error());
        if (!info->contains("6G Info"))
            return std::unexpected(std::format("unexpected 6 GHz status output: {}", *info));
        return proto::Status{*country, std::move(*info)};
    }

private:
    template <std::size_t Size>
    [[nodiscard]] auto exchange(std::array<Byte, Size> request) const -> Result<std::vector<Byte>> {
        auto response = request; // pre-filled
        DWORD returned = 0;
        const auto rc = ::WlanIhvControl(wlan_, &iface_, wlan_ihv_control_type_driver,
                                         static_cast<DWORD>(request.size()), request.data(),
                                         static_cast<DWORD>(response.size()), response.data(), &returned);
        if (rc != ERROR_SUCCESS) return win::api_error("WlanIhvControl (not retried)", rc);
        const auto payload = proto::decode(request, response, returned);
        if (!payload) return std::unexpected(Error{payload.error()});
        return std::vector<Byte>(payload->begin(), payload->end());
    }

    [[nodiscard]] auto done() const -> Result<bool> {
        const auto payload = exchange(proto::done_request);
        if (!payload) return std::unexpected(payload.error());
        const auto flag = proto::parse_done(*payload);
        if (!flag) return std::unexpected(Error{flag.error()});
        return *flag;
    }

    HANDLE wlan_;
    GUID iface_;
};

// ── set flow (mirrors backend.rs operate_locked + protocol::set_and_check) ─
enum class Exit : int { ok = 0, failed = 1, no_6ghz = 2 };

void print_indented(std::string_view info) {
    for (const auto line : proto::lines(info))
        if (!line.empty()) std::println("    {}", line);
}

[[nodiscard]] auto set_country(const Driver& driver, proto::CountryCode target) -> Result<Exit> {
    const auto before = driver.status();
    if (!before) return std::unexpected(std::format("could not read status; nothing sent: {}", before.error()));
    std::println("Before: {}", before->country.view());

    if (before->active(target)) {
        std::println("Already {} with 6 GHz support; nothing sent.", target.view());
        return Exit::ok;
    }

    const auto changed = std::format("Country code has changed to {}", target.view());
    const auto reply = driver.diagnostic(proto::set_request(target));
    const bool recognized = reply && (reply->contains(changed) || reply->contains("Invalid country code!"));
    if (!recognized) {
        // Don't overwrite a possibly still-running diagnostic: only re-read the country.
        const auto now = driver.country();
        std::println(stderr, "UNCERTAIN: {}; country now: {}. Not resent.",
                     reply ? std::format("unexpected driver reply '{}'", text::trim(*reply)) : reply.error(),
                     now ? std::string(now->view()) : now.error());
        return Exit::failed;
    }
    std::println("Driver: {}", text::trim(*reply));

    // Re-check even after an explicit rejection; never infer success from the reply alone.
    const auto after = driver.status();
    if (!after) return std::unexpected(std::format("post-check failed, state unknown: {}", after.error()));
    std::println("After:  {}", after->country.view());
    print_indented(after->info);

    if (!reply->contains(changed) || !after->country.is(target)) {
        std::println(stderr, "FAILED: country code was not changed to {}", target.view());
        return Exit::failed;
    }
    if (!proto::manual_6ghz_supported(after->info)) {
        std::println(stderr, "WARNING: country is {}, but the driver reports no manual 6 GHz support for it",
                     target.view());
        return Exit::no_6ghz;
    }
    std::println("SUCCESS: country set to {} with 6 GHz support", target.view());
    return Exit::ok;
}

[[nodiscard]] auto run(std::optional<proto::CountryCode> target) -> Result<Exit> {
    const auto lock = win::acquire_diagnostic_lock();
    if (!lock) return std::unexpected(lock.error());
    const auto wlan = win::WlanClient::open();
    if (!wlan) return std::unexpected(wlan.error());
    const auto adapter = find_steam_frame(*wlan);
    if (!adapter) return std::unexpected(adapter.error());
    const auto driver_info = inspect_driver();
    if (!driver_info) return std::unexpected(driver_info.error());

    if (!driver_info->verified())
        std::println(stderr, "warning: unverified driver version (SHA-256 {}); continuing", driver_info->sha256);

    const Driver driver{wlan->get(), adapter->guid};
    if (!target) {
        const auto country = driver.country();
        if (!country) return std::unexpected(country.error());
        std::println("{}", country->view());
        return Exit::ok;
    }

    std::println("Adapter: {}", adapter->name);
    std::println("Device:  {}", adapter->instance_id);
    if (driver_info->verified()) std::println("Driver:  verified {}", hw::verified_version);
    return set_country(driver, *target);
}

constexpr std::string_view usage = R"(usage: set_region [CC]
  (no argument)  print the adapter's current country code
  CC             set the country code (ISO 3166-1 alpha-2, e.g. PL, US, DE)
Run as administrator with the Steam Frame USB adapter plugged in.
Exit codes: 0 ok, 1 error, 2 country set but no 6 GHz support.)";

int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    const std::span<char*> args{argv + 1, static_cast<std::size_t>(std::max(argc - 1, 0))};

    std::optional<proto::CountryCode> target;
    if (args.size() == 1) {
        const std::string_view arg = args[0];
        if (arg == "-h" || arg == "--help" || arg == "/?") {
            std::println("{}", usage);
            return 0;
        }
        target = proto::CountryCode::parse(arg);
        if (!target) {
            std::println(stderr, "error: '{}' is not a two-letter country code\n{}", arg, usage);
            return 1;
        }
    } else if (args.size() > 1) {
        std::println(stderr, "{}", usage);
        return 1;
    }

    try {
        const auto result = run(target);
        if (!result) {
            std::println(stderr, "error: {}", result.error());
            return 1;
        }
        return std::to_underlying(*result);
    } catch (const std::exception& e) {
        std::println(stderr, "error: {}", e.what());
        return 1;
    }
}
